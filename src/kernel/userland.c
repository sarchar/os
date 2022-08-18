// temporary module for a userland program
#include "common.h"

#include "cpu.h"
#include "kernel.h"
#include "syscall.h"

#define USERLAND_CODE __attribute__((section(".userland.text")))
#define USERLAND_DATA __attribute__((section(".userland.data")))

//static USERLAND_DATA bool got_user_mode = false;

USERLAND_CODE s64 syscall(u64 syscall_number, u64 arg0, u64 arg1, u64 arg2, u64 arg3, u64 arg4, u64 arg5)
{
    s64 ret;
    asm volatile(
        "mov %1, %%rdi\n" 
        "\tmov %2, %%rsi\n" 
        "\tmov %3, %%rdx\n" 
        "\tmov %4, %%rcx\n"
        "\tmov %5, %%r8\n"
        "\tmov %6, %%r9\n"
        "\tmov %7, %%rax\n"
        "\tint $0x81\n"
        "\tmov %%rax, %0\n" : "=m"(ret) : "m"(arg0), "m"(arg1), "m"(arg2), "m"(arg3), "m"(arg4), "m"(arg5), "m"(syscall_number));
    return ret;
}

USERLAND_CODE __noreturn void sc_exit(u64 exit_code)
{
    syscall(SYSCALL_EXIT, exit_code, 0, 0, 0, 0, 0);
    while(1) ;
}

USERLAND_CODE void sc_usleep(u64 us)
{
    syscall(SYSCALL_USLEEP, us, 0, 0, 0, 0, 0);
}

USERLAND_CODE __noreturn s64 userland_task_main() 
{
    for(u64 i = 0; i < 5; i++) {
        sc_usleep(1000000);
    }

    sc_exit(0);
}

