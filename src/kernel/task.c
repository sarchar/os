#include "common.h"

#include "apic.h"
#include "cpu.h"
#include "kalloc.h"
#include "kernel.h"
#include "paging.h"
#include "palloc.h"
#include "task.h"
#include "stdio.h"

// task_become is used once per cpu to become task, which the values of the task
// being filled out when the task switch occurs
struct task* task_become()
{
    struct task* task = (struct task*)kalloc(sizeof(struct task));
    zero(task);
    // default to looping to itself
    task->prev = task->next = task;
    return task;
}

static __noreturn void _task_start() 
{
    struct task* task = get_cpu()->current_task;
    task_exit(task->entry(task));
}

struct task* task_create(task_entry_point_function* entry, intp userdata)
{
    struct task* task = (struct task*)kalloc(sizeof(struct task));
    zero(task);

    // set up the task entry point
    task->entry = entry;
    task->userdata = userdata;

    // default to looping to itself
    task->prev = task->next = task;

    // allocate stack
    task->rsp = (u64)palloc_claim(1) + 2*PAGE_SIZE - 7 * sizeof(u64); // 7 pops to change tasks

    // initialize parameters on the stack
    u64* stack = (u64*)task->rsp;
    stack[0] = 0xDEAD; // rbx
    stack[1] = 0xBEEF; // rbp
    stack[2] = 0; // r12
    stack[3] = 0; // r13
    stack[4] = 0; // r14
    stack[5] = 0; // r15
    stack[6] = (u64)(intp)_task_start; // rip

    return task;
}

void task_free(struct task* task)
{
    palloc_abandon(task->rsp & ~(2*PAGE_SIZE - 1), 1);
    kfree(task);
}

// to enqueue, we put new_task at the end of the list
void task_enqueue(struct task** task_queue, struct task* new_task)
{
    new_task->next = *task_queue;

    if(*task_queue != null) {
        if((*task_queue)->prev != null) {
            (*task_queue)->prev->next = new_task;
            new_task->prev = (*task_queue)->prev;
        }

        (*task_queue)->prev = new_task;
        new_task->next = *task_queue;
    } else {
        new_task->prev = null;
        *task_queue = new_task;
    }
}

void task_dequeue(struct task** task_queue, struct task* task)
{
    if(task->prev != null) {
        task->prev->next = task->next;
    }

    if(task->next != null) {
        task->next->prev = task->prev;
    }

    // replace the head if necessary
    if(*task_queue == task) {
        if(task == task->next) { // last link in the list when it points to itself
            *task_queue = null;
        } else {
            *task_queue = task->next;
        }
    }
}

void task_yield()
{
    struct cpu* cpu = get_cpu();
    task_switch_to(cpu->current_task->next);
}

extern void _task_switch_to(struct task*, struct task*);

__noreturn void task_exit(s64 return_value)
{
    struct cpu* cpu = get_cpu();
    struct task* task = cpu->current_task;

    // set the return value
    task->return_value = return_value;

    // remove from the current task queue and add to the exited task queue
    task_dequeue(&cpu->current_task, task);
    task_enqueue(&cpu->exited_task, task);

    // switch to the next task and never return
    if(cpu->current_task != null) {
        _task_switch_to(task, cpu->current_task); // switch from 'task' (currently running) to the new head
    }

    // satisfy the compiler
    while(1) __pause();
}

void task_switch_to(struct task* to_task)
{
    struct cpu* cpu = get_cpu();
    struct task* from_task = cpu->current_task;

    cpu->current_task = to_task;
    _task_switch_to(from_task, to_task);
}

