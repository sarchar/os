#include "common.h"

#include "apic.h"
#include "cpu.h"
#include "kalloc.h"
#include "kernel.h"
#include "paging.h"
#include "palloc.h"
#include "task.h"
#include "smp.h"
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
    task->cpu = cpu;

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
    task->cpu = get_cpu();

    // set up the task entry point
    task->entry = entry;
    task->userdata = userdata;

    // default to looping to itself
    task->prev = task->next = task;

    // allocate stack, and set RSP to the base of where we initialize registers
    u64 stack_size;
    task->stack_bottom = task_allocate_stack(&stack_size);
    task->rsp = (u64)task->stack_bottom + stack_size;

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

intp task_allocate_stack(u64* stack_size)
{
    *stack_size = (1 << TASK_STACK_SIZE) * PAGE_SIZE;
    return palloc_claim(TASK_STACK_SIZE);
}

void task_free(struct task* task)
{
    if(task->stack_bottom != 0) palloc_abandon(task->stack_bottom, TASK_STACK_SIZE);
    kfree(task);
}

void task_set_priority(s8 priority)
{
    get_cpu()->current_task->priority = priority;
}

void task_enqueue_for(u32 target_cpu_index, struct task* new_task)
{
    // wake up the other cpu and tell it to add the task to its running queue
    struct ipcall* ipcall = apic_ipcall_build(IPCALL_TASK_ENQUEUE, (void*)new_task);
    apic_ipcall_send(target_cpu_index, ipcall);
}

// to enqueue, we put new_task at the end of the list
void task_enqueue(struct task** task_queue, struct task* new_task)
{
    u64 cpu_flags = __cli_saveflags();

    // the task now should run on the specified cpu
    new_task->cpu = get_cpu();

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

static struct task* _select_next_task(struct task* start)
{
    if(start == null) return null;

    // current task will never be null if we get there from a task switch
    // so start looking through the list for a next valid task

    // simple round robin algorithm finds the next task that is runnable
    struct task* next_task = start;
    while(next_task != start->prev) {
        // skip low priority tasks, and tasks that aren't runnable
        if(next_task->priority >= 0 && (next_task->state == TASK_STATE_NEW || next_task->state == TASK_STATE_READY)) {
            return next_task;
        }

        next_task = next_task->next;
    }

    // if we get here then no task other than the current task was valid, so we have to check 
    // if the current task is still runnable
    if(next_task->state == TASK_STATE_NEW || next_task->state == TASK_STATE_READY) {
        // if the current task has a low priority, but so does the next one, move onto the next one
        // this simple move-forward move will allow all low priority tasks to get a chance when no
        // other normal priority tasks exist. It also doesn't matter if next_task->next == next_task,
        // since it'll just be the same result either way
        if(next_task->priority < 0 && next_task->next->priority < 0) return next_task->next;

        // otherwise, current task will do
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
    struct task* to_task;

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
    case TASK_YIELD_MUTEX_BLOCK:
        from_task->state = TASK_STATE_BLOCKED;
        break;
    }

    // if the current task has exited, remove it from the running list (_select_next_task will never give us an EXITED task)
    if(from_task->state == TASK_STATE_EXITED) {
        task_dequeue(&cpu->current_task, from_task);
//        fprintf(stderr, "task: putting task %d into exited queue\n", from_task->task_id);

        // when a task exits, sometimes the return value is useful and we save that context for some other processing
        if(from_task->save_context) {
            task_enqueue(&cpu->exited_task, from_task);
        } else {
            // otherwise, we can free the memory now
            task_free(from_task);
        }

        // select the next task starting with the current task
        to_task = _select_next_task(cpu->current_task);
    } else if(from_task->state == TASK_STATE_BLOCKED) { // no need to keep iterating over this task if its blocked, so move it to a different queue
        task_dequeue(&cpu->current_task, from_task);
//        fprintf(stderr, "task: putting task %d into blocked queue\n", from_task->task_id);
        task_enqueue(&cpu->blocked_task, from_task);
        // select next task as normal
        to_task = _select_next_task(cpu->current_task);
    } else {
        // select the next task starting with the next
        to_task = _select_next_task(cpu->current_task->next);
    }

    // if there's no more tasks to run, halt forever
    // TODO: loop forever, checking if any task is ever added to the queue
    if(to_task == null) {
        // free memory for all exited processes
        while(cpu->exited_task != null) {
            struct task* exited = cpu->exited_task;

//            fprintf(stderr, "task: unhandled task %d exited with return value = %d\n", exited->task_id, exited->return_value);

            task_dequeue(&cpu->exited_task, exited);
            task_free(exited);
        }

        // wait until a new task arrives
        fprintf(stderr, "task: warning: cpu %d entering permanent idle state\n", cpu->cpu_index);
        while(1) __hlt();
    }

    // we have a task, switch to it
    cpu->current_task = to_task;
    cpu->current_task->state = TASK_STATE_RUNNING;
    _task_switch_to(from_task, to_task);
    
//resume_task:
    // at this point our task has just been resumed, restore interrupt flag and return
    __restoreflags(cpu_flags);
}

// task_unblock is sometimes called from within an interrupt, and so shouldn't ever
// call fprintf() (as interrupts are disabled and could cause a deadlock in the stream mutex)
void task_unblock(struct task* task)
{
    assert(task->state == TASK_STATE_BLOCKED, "can't unblock an unblocked task");

    struct cpu* cpu = get_cpu();

    if(task->cpu == cpu) { // easy case here, just requeue it in READY state
        u64 cpu_flags = __cli_saveflags();
        task_dequeue(&cpu->blocked_task, task);
        task->state = TASK_STATE_READY;
        task_enqueue(&cpu->current_task, task);
        __restoreflags(cpu_flags);
    } else {
        // wake up the other cpu and tell it to add the task back to its queue
        struct ipcall* ipcall = apic_ipcall_build(IPCALL_TASK_UNBLOCK, (void*)task);
        apic_ipcall_send(task->cpu->cpu_index, ipcall);
    } 
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

