// temporary module for a userland program
#include "common.h"

#include "cpu.h"
#include "kernel.h"

#define USERLAND_CODE __attribute__((section(".userland.text")))
#define USERLAND_DATA __attribute__((section(".userland.data")))

//static USERLAND_DATA bool got_user_mode = false;

USERLAND_CODE s64 syscall(u64 syscall_number, u64 arg0, u64 arg1, u64 arg2, u64 arg3)
{
    s64 ret;
    asm volatile(
        "mov %1, %%rdi\n" 
        "\tmov %2, %%rsi\n" 
        "\tmov %3, %%rdx\n" 
        "\tmov %4, %%rcx\n"
        "\tmov %5, %%rax\n"
        "\tint $0x81\n"
        "\tmov %%rax, %0\n" : "=m"(ret) : "m"(arg0), "m"(arg1), "m"(arg2), "m"(arg3), "m"(syscall_number));
    return ret;
}

USERLAND_CODE s64 _user_task_entry() 
{
    while(1) {
        syscall(1, 1000, 0, 0, 0); // function 1 - msleep(1000)
        __pause();
    }

    return 0;
}

