#include "common.h"

#include "cpu.h"
#include "errno.h"
#include "hpet.h"
#include "kernel.h"
#include "syscall.h"
#include "task.h"

typedef s64 (syscall_function)(u64 arg0, u64 arg1, u64 arg2, u64 arg3, u64 arg4, u64 arg5);

static __noreturn void _exit(u64);
static s64 _usleep(u64);

static syscall_function* _syscall_table[] = {
    /* 0 */ (syscall_function*)&_exit,
    /* 1 */ (syscall_function*)&_usleep,
};

s64 syscall_do(u64 no, u64 arg0, u64 arg1, u64 arg2, u64 arg3, u64 arg4, u64 arg5)
{
    if(no >= countof(_syscall_table)) return -EINVAL;
    return _syscall_table[no](arg0, arg1, arg2, arg3, arg4, arg5);
}

static __noreturn void _exit(u64 arg0)
{
    task_exit(arg0);
}

static s64 _usleep(u64 us)
{
    usleep(us);
    return 0;
}

