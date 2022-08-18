#ifndef __SYSCALL_H__
#define __SYSCALL_H__

#define SYSCALL_EXIT   0
#define SYSCALL_USLEEP 1

s64 syscall_do(u64 no, u64 arg0, u64 arg1, u64 arg2, u64 arg3, u64 arg4, u64 arg5);

#endif
