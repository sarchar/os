#include "common.h"

#include "apic.h"
#include "cpu.h"
#include "kalloc.h"
#include "kernel.h"
#include "paging.h"
#include "palloc.h"
#include "task.h"
#include "stdio.h"

// log2 size of the stack (order for palloc) that is allocated to each task
#define TASK_STACK_SIZE 2     // 2^2 = 4*4096 = 16KiB

#define TASK_PUSH_STACK(t,val) task->rsp -= sizeof(u64); *((u64*)task->rsp) = (u64)(val)

static u64 next_task_id = (u64)-1;

// task_become is used once per cpu to "become" a task, which the values of the task
// being filled out when the first task switch occurs
void task_become()
{
    struct cpu* cpu = get_cpu();
    struct task* task = (struct task*)kalloc(sizeof(struct task));
    zero(task);

    // initialize the linked list to just point to itself
    task->prev = task->next = task;

    // setup state
    task->task_id = __atomic_inc(&next_task_id);
    task->state = TASK_STATE_RUNNING;

    // and timing
    task->last_global_ticks = global_ticks;

    // TODO allocate space for the stack and copy the current stack (if requested)

    // make the current task be this one
    assert(cpu->current_task == null, "only call this once");
    cpu->current_task = task;
}

// _task_entry is the entry point to all kernel tasks and handles when the task returns instead of exits
static __noreturn void _task_entry() 
{
    struct task* task = get_cpu()->current_task;

    // at the beginning of a task, enable interrupts
    __sti();

    // jump to the main function, and if it returns call task_exit
    task_exit(task->entry(task), true);
}

struct task* task_create(task_entry_point_function* entry, intp userdata)
{
    struct task* task = (struct task*)kalloc(sizeof(struct task));
    zero(task);

    // assign the task id
    task->task_id = __atomic_inc(&next_task_id);
    task->state = TASK_STATE_NEW;

    // set up the task entry point
    task->entry = entry;
    task->userdata = userdata;

    // default to looping to itself
    task->prev = task->next = task;

    // allocate stack, and set RSP to the base of where we initialize registers
    task->rsp = (u64)palloc_claim(TASK_STACK_SIZE) + (1 << TASK_STACK_SIZE) * PAGE_SIZE;

    // initialize parameters on the stack
    TASK_PUSH_STACK(task, (intp)_task_entry); // rip
    TASK_PUSH_STACK(task, 0); // r15
    TASK_PUSH_STACK(task, 0); // r14
    TASK_PUSH_STACK(task, 0); // r13
    TASK_PUSH_STACK(task, 0); // r12
    TASK_PUSH_STACK(task, 0); // rbp
    TASK_PUSH_STACK(task, 0); // rbx

    return task;
}

void task_free(struct task* task)
{
    palloc_abandon(task->rsp & ~((1 << TASK_STACK_SIZE)*PAGE_SIZE - 1), 1);
    kfree(task);
}

// to enqueue, we put new_task at the end of the list
void task_enqueue(struct task** task_queue, struct task* new_task)
{
    u64 cpu_flags = __cli_saveflags();

    if(*task_queue != null) {
        // previous task points to a new task
        (*task_queue)->prev->next = new_task;

        // new task points both ways
        new_task->prev = (*task_queue)->prev;
        new_task->next = *task_queue;

        // head's prev now points to new task
        (*task_queue)->prev = new_task;
    } else {
        // just a single item in the list now
        new_task->prev = new_task->next = new_task;
        *task_queue = new_task;
    }

    __restoreflags(cpu_flags);
}

void task_dequeue(struct task** task_queue, struct task* task)
{
    u64 cpu_flags = __cli_saveflags();

    // replace the head if necessary
    if(*task_queue == task) {
        if(task == task->next) { // last link in the list when it points to itself
            *task_queue = null;
        } else {
            *task_queue = task->next;
        }
    }

    if(task->prev != null) {
        task->prev->next = task->next;
    }

    if(task->next != null) {
        task->next->prev = task->prev;
    }

    __restoreflags(cpu_flags);
}

extern void _task_switch_to(struct task*, struct task*);

static struct task* _select_next_task(struct cpu* cpu)
{
    // current task will never be null if we get there from a task switch
    // so start looking through the list for a next valid task

    // simple round robin algorithm finds the next task that is able to be ran
    struct task* next_task = cpu->current_task->next;
    while(next_task != cpu->current_task) {
        if(next_task->state == TASK_STATE_NEW || next_task->state == TASK_STATE_READY) {
            return next_task;
        }

        next_task = next_task->next;
    }

    // if we get here then no task other than the current task was valid, so we have to check 
    // if the current task is still runnable
    if(next_task->state == TASK_STATE_NEW || next_task->state == TASK_STATE_READY) {
        return next_task;
    }

    // no tasks found, go idle
    return null;
}

void task_yield(enum TASK_YIELD_REASON reason)
{
    u64 cpu_flags = __cli_saveflags();

    struct cpu* cpu = get_cpu();
    struct task* from_task = cpu->current_task;

    // set up the next state for the current task
    assert(from_task->state == TASK_STATE_RUNNING, "running task should have correct state");
    switch(reason) {
    case TASK_YIELD_PREEMPT:
    case TASK_YIELD_VOLUNTARY:
        from_task->state = TASK_STATE_READY; // preserve EXITED state
        break;
    case TASK_YIELD_EXITED:
        from_task->state = TASK_STATE_EXITED;
        break;
    }

    // select the next task
    struct task* to_task = _select_next_task(cpu);

    // if the current task has exited, remove it from the running list (_select_next_task will never give us an EXITED task)
    if(from_task->state == TASK_STATE_EXITED) {
        task_dequeue(&cpu->current_task, from_task);
        //fprintf(stderr, "task: putting task %d into exited queue\n", from_task->task_id);

        // when a task exits, sometimes the return value is useful and we save that context for some other processing
        if(from_task->save_context) {
            task_enqueue(&cpu->exited_task, from_task);
        } else {
            // otherwise, we can free the memory now
            task_free(from_task);
        }
    }

    // if there's no more tasks to run, halt forever
    // TODO: loop forever, checking if any task is ever added to the queue
    if(to_task == null) {
        // free memory for all exited processes
        while(cpu->exited_task != null) {
            struct task* exited = cpu->exited_task;

            fprintf(stderr, "task: unhandled task %d exited with return value = %d\n", exited->task_id, exited->return_value);

            task_dequeue(&cpu->exited_task, exited);
            task_free(exited);
        }

        // wait until a new task arrives
        while(1) __pause();
    }

    // we have a task, switch to it
    cpu->current_task = to_task;
    cpu->current_task->state = TASK_STATE_RUNNING;
    _task_switch_to(from_task, to_task);
    
//resume_task:
    // at this point our task has just been resumed, restore interrupt flag and return
    __restoreflags(cpu_flags);
}

void __noreturn task_exit(s64 return_value, bool save_context)
{
    // disable interrupts
    __cli();

    //fprintf(stderr, "task: exit called on task %d\n", get_cpu()->current_task->task_id);

    struct cpu* cpu = get_cpu();
    struct task* task = cpu->current_task;

    // set the return value and let task_yield() remove the task from the current list
    task->return_value = return_value;
    task->save_context = save_context;

    // don't have to restore irqs here since task_yield will never return (our process
    // has been removed from the task list, and some other task will enable interrupts
    // from when it switched away)
    task_yield(TASK_YIELD_EXITED);

    // will never get here, but this makes the compiler happy
    while(1) ;
}

