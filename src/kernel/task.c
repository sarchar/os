#include "common.h"

#include "apic.h"
#include "cpu.h"
#include "interrupts.h"
#include "kernel.h"
#include "paging.h"
#include "palloc.h"
#include "task.h"
#include "smp.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "vmem.h"

// log2 size of the stack (order for palloc) that is allocated to each task
#define TASK_STACK_SIZE 2     // 2^2 = 4*4096 = 16KiB

#define TASK_PUSH_STACK(t,val) (t)->rsp -= sizeof(u64); *((u64*)(t)->rsp) = (u64)(val)

static u64 next_task_id = (u64)-1;

extern void _task_switch_to(struct task*, struct task*);
extern void _task_entry_user(void);

// task_become is used once per cpu to "become" a task, which the values of the task
// being filled out when the first task switch occurs
void task_become()
{
    struct cpu* cpu = get_cpu();
    struct task* task = (struct task*)malloc(sizeof(struct task));
    zero(task);

    // initialize the linked list to just point to itself
    task->prev = task->next = task;

    // setup state
    task->task_id = __atomic_inc(&next_task_id);
    task->state = TASK_STATE_RUNNING;
    task->cpu = cpu;

    // page table (use the current table)
    task->page_table = paging_get_kernel_page_table();
    task->cr3 = paging_get_cpu_table(task->page_table);
    assert(task->cr3 == __rdcr3(), "must match");

    // and timing
    task->last_global_ticks = global_ticks;

    // TODO allocate space for the stack and copy the current stack (if requested)

    // make the current task be this one
    assert(cpu->current_task == null, "only call this once");
    cpu->current_task = task;
}

// _task_entry_kernel is the entry point to all kernel tasks and handles when the task returns instead of exits
static __noreturn void _task_entry_kernel() 
{
    struct task* task = get_cpu()->current_task;

    // at the beginning of a task, enable interrupts
    __sti();

    // jump to the main function, and if it returns call task_exit
    task_exit(task->entry(task), true);
}

struct task* task_create(task_entry_point_function* entry, intp userdata, bool is_user)
{
    struct task* task = (struct task*)malloc(sizeof(struct task));
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

    // set up the task's page table (kernel page table if kernel task, new one otherwise)
    if(is_user) {
        task->flags |= TASK_FLAG_USER;
        task->page_table = paging_create_private_table();
        task->vmem = vmem_create_private_memory(task->page_table);
    } else {
        task->page_table = paging_get_kernel_page_table();
    }
    task->cr3 = paging_get_cpu_table(task->page_table);

    // entry point to the task
    task->rip = is_user ? (u64)_task_entry_user : (u64)_task_entry_kernel;

    // default cpu flags
    task->rflags = __saveflags() & ~(1 << 9); // IF (interrupt enable flag) is bit 9. Section 3.4.3 of Volume 1 Software Development Manual

    // allocate stack, and set RSP to the top of the stack
    u64 stack_size;
    task->stack_bottom = task_allocate_stack(task->vmem, &stack_size, is_user);
    task->rsp = (u64)task->stack_bottom + stack_size;

    // we don't need to initialize the 6 registers (r15, r14, r13, r12, rbp, rbx) on the new task's stack
    // since they're already zero from task_allocate_stack. we just need to move the stack pointer to
    // accommodate the pops that will happen later
    task->rsp -= 6 * sizeof(u64);

    return task;
}

intp task_allocate_stack(intp vmem, u64* stack_size, bool is_user)
{
    intp ret = palloc_claim(TASK_STACK_SIZE);

    // physical addresses are identity mapped, so we can safely clear the stack here
    memset64((void*)ret, 0, (1ULL << (TASK_STACK_SIZE + PAGE_SHIFT - 3)));

    // map the stack into virtual memory
    u32 map_flags = MAP_PAGE_FLAG_WRITABLE;
    if(is_user) map_flags |= MAP_PAGE_FLAG_USER;
    ret = vmem_map_pages(vmem, ret, 1 << TASK_STACK_SIZE, map_flags);

    *stack_size = (1 << TASK_STACK_SIZE) * PAGE_SIZE;

    return ret;
}

void task_free(struct task* task)
{
    if(task->stack_bottom != 0) { // the boot threads use stacks that are in .bss, and this is set to 0
        // unmap the stack, and free the physical pages used for it
        intp phys = vmem_unmap_pages(task->vmem, task->stack_bottom, 1 << TASK_STACK_SIZE);
        palloc_abandon(phys, TASK_STACK_SIZE);
    }

    free(task);
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
void task_enqueue(struct task* volatile* task_queue, struct task* new_task)
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

void task_dequeue(struct task* volatile* task_queue, struct task* task)
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

    // prev/next pointers are never null in a circularly linked list
    task->prev->next = task->next;
    task->next->prev = task->prev;

    // make it point to itself
    task->prev = task->next = task;

    __restoreflags(cpu_flags);
}

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
    assert(from_task != null || reason == TASK_YIELD_PREEMPT, "can't happen");

    // we can actually get task_yield() from the timer interrupt, but it's possible we
    // don't have task in that case. all other yields must be called with valid a current_task.
    if(from_task == null) return;

    // set up the next state for the current task
    assert(from_task->state == TASK_STATE_RUNNING, "running task should have correct state");
    switch(reason) {
    case TASK_YIELD_PREEMPT:
    case TASK_YIELD_VOLUNTARY:
        from_task->state = TASK_STATE_READY;
        break;
    case TASK_YIELD_EXITED:
        from_task->state = TASK_STATE_EXITED;
        break;
    case TASK_YIELD_MUTEX_BLOCK:
        from_task->state = TASK_STATE_BLOCKED;
        break;
    }

    // it's now safe to transfer unblocked tasks to the running task pool
    while(cpu->unblocked_task != null) {
        struct task* unblocked_task = cpu->unblocked_task;
        task_dequeue(&cpu->unblocked_task, unblocked_task);
        task_enqueue(&cpu->current_task, unblocked_task);
    }

    // if the current task has exited, remove it from the running list (_select_next_task will never give us an EXITED task)
    struct task* to_task;
    if(from_task->state == TASK_STATE_EXITED) {
        task_dequeue(&cpu->current_task, from_task);
        //fprintf(stderr, "task: putting task %d into exited queue\n", from_task->task_id);

        // when a task exits, we must have it to be freed later. if it is freed now, the stack *that we're currently using*
        // will be released to palloc, and very likely get destroyed. thus, all processors must have a cleanup task that occassionally runs
        // see task_idle_forever()
        task_enqueue(&cpu->exited_task, from_task);

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

    // when all tasks are blocked (there's always at least a kernel work thread on each cpu),
    // then we may have a situation where to_task is null and we can't switch to any task
    // so we have to enable interrupts so that IPIs can unblock tasks
    if(to_task == null) { // will only happen when cpu->current_task set is empty
        assert(cpu->current_task == null, "there must be no runnable tasks");

        u32 cpu_flags = __sti_saveflags(); // shadows parent cpu_flags
        while((to_task = cpu->unblocked_task) == null) { // we wait here until a task becomes unblocked
            // there's a change we get preempted here, but current_task will be null, so another yield will do nothing
            __pause_barrier();
        }
        __restoreflags(cpu_flags);

        // just move one unblocked task for now
        task_dequeue(&cpu->unblocked_task, to_task);
        task_enqueue(&cpu->current_task, to_task);
        // ... and let to_task become the current task
        assert(to_task->state == TASK_STATE_READY, "unblocked task must be in ready state");
    }

    // now have a task, switch to it
    cpu->current_task = to_task;
    cpu->current_task->state = TASK_STATE_RUNNING;
    _task_switch_to(from_task, to_task);

    goto resume_task;
    
resume_task:
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

        // it's not safe to put unblocked tasks directly in the current queue, because
        // the current queue could be empty and we may be waiting in task_wait_for_task()
        task->state = TASK_STATE_READY;
        task_enqueue(&cpu->unblocked_task, task);
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

// perform some task cleanup maintenence like freeing exited task structures
// TODO use events to know when things have been added?
void task_clean()
{
    struct cpu* cpu = get_cpu();

    u64 cpu_flags = __cli_saveflags(); // disable preemption
    while(cpu->exited_task != null) {
        struct task* task = cpu->exited_task;
        task_dequeue(&cpu->exited_task, task);

        __restoreflags(cpu_flags); // can't have interrupts disabled during fprintf

        // print exit code
        fprintf(stderr, "cpu%d: task %d exited (ret = %d)\n", cpu->cpu_index, task->task_id, task->return_value);
        task_free(task); // safe now, because task != current_task

        cpu_flags = __cli_saveflags();
    }

    __restoreflags(cpu_flags);
}

