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

enum TASK_FLAGS {
    TASK_FLAG_USER            = 1 << 0,
    TASK_FLAG_NOT_PREEMPTABLE = 1 << 1,
};

struct page_table;
struct task {
    ////////////////////////////////////////////////////////////////////////////
    // structure definition here must match task.asm
    ////////////////////////////////////////////////////////////////////////////

    // saved context variables
    intp rip; 
    intp rsp;
    intp cr3;
    u64  rflags;
    u64  last_global_ticks;

    // length of time in ms the task has been running
    u64  runtime;

    // flags (user mode, etc)
    u64  flags;

    // entry point to the actual task
    task_entry_point_function* entry;

    // base of the stack
    u64  stack_bottom;

    ////////////////////////////////////////////////////////////////////////////
    // the structure from this point forward doesn't need to match task.asm
    ////////////////////////////////////////////////////////////////////////////

    // this tasks page table
    struct page_table* page_table;

    // cpu this task is on
    struct cpu*  cpu;

    // current running state
    enum TASK_STATE state;

    u64  task_id;
    intp userdata;

    u64  return_value;

    u8   padding0;
    s8   priority;
    u16  padding1;
    u32  padding2;

    // private vmem space
    intp vmem;

    struct task* prev;
    struct task* next;
};

void task_become();
struct task* task_create(task_entry_point_function*, intp, bool); 
intp task_allocate_stack(intp, u64*, bool);
void task_free(struct task*);

// yield from the current task and switch to the next one
enum TASK_YIELD_REASON {
    TASK_YIELD_PREEMPT,
    TASK_YIELD_EXITED,
    TASK_YIELD_VOLUNTARY,
    TASK_YIELD_WAIT_CONDITION
};

void task_set_priority(s8);
void task_set_preemtable(struct task*, bool);

void task_yield(enum TASK_YIELD_REASON);
void task_clean();

// exit the current task
__noreturn void task_exit(s64);

// notify that a task can be unblocked, this will often happen on a different cpu
void task_unblock(struct task*);

void task_enqueue(struct task* volatile*, struct task*);
void task_enqueue_for(u32, struct task*);
void task_dequeue(struct task* volatile*, struct task*);

#endif
