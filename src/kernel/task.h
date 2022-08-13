#ifndef __TASK_H__
#define __TASK_H__

typedef s64 (task_entry_point_function)();

enum TASK_STATE {
    TASK_STATE_NEW = 0,   // task has been created but has not run once yet
    TASK_STATE_EXITED,    // task has exited but structure is still valid
    TASK_STATE_RUNNING,   // task is currently running
    TASK_STATE_READY,     // task is ready to run (but not currently running)
    TASK_STATE_BLOCKED,   // task is waiting on some event to occur
};

struct task {
    ////////////////////////////////////////////////////////////////////////////
    // structure definition here must match task.asm
    ////////////////////////////////////////////////////////////////////////////

    // saved context variables
    u64  rsp;
    u64  last_global_ticks;

    // length of time in ms the task has been running
    u64  runtime;

    ////////////////////////////////////////////////////////////////////////////
    // the structure from this point forward doesn't need to match task.asm
    ////////////////////////////////////////////////////////////////////////////

    u64  stack_bottom;

    // cpu this task is on
    struct cpu*  cpu;

    // current running state
    enum TASK_STATE state;

    u64  task_id;
    task_entry_point_function* entry;
    intp userdata;

    u64  return_value;
    bool save_context;
    s8   priority;
    u16  padding2;
    u32  padding3;

    struct task* prev;
    struct task* next;
};

void task_become();
struct task* task_create(task_entry_point_function*, intp); 
intp task_allocate_stack(u64*);
void task_free(struct task*);

// yield from the current task and switch to the next one
enum TASK_YIELD_REASON {
    TASK_YIELD_PREEMPT,
    TASK_YIELD_EXITED,
    TASK_YIELD_VOLUNTARY,
    TASK_YIELD_MUTEX_BLOCK
};

void task_set_priority(s8);

void task_yield(enum TASK_YIELD_REASON);

// exit the current task
__noreturn void task_exit(s64, bool);

// notify that a task can be unblocked, this will often happen on a different cpu
void task_unblock(struct task*);

void task_enqueue(struct task**, struct task*);
void task_dequeue(struct task**, struct task*);

#endif
