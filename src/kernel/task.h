#ifndef __TASK_H__
#define __TASK_H__

typedef s64 (task_entry_point_function)();

struct task {
    // these values must align with task.asm
    u64  rsp;
    u64  last_global_ticks;

    // length of time in ms the task has been running
    u64  runtime;

    // these values can be in any order
    task_entry_point_function* entry;
    intp userdata;
    u64  return_value;

    //struct x86_registers registers;
    struct task* prev;
    struct task* next;
};

struct task* task_become();
struct task* task_create(task_entry_point_function*, intp); 
void task_free(struct task*);

// yield from the current task and switch to the next one
void task_yield();
// exit the current task
__noreturn void task_exit(s64);

void task_switch_to(struct task*);

void task_enqueue(struct task**, struct task*);
void task_dequeue(struct task**, struct task*);

#endif
