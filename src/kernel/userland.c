// temporary module for a userland program
#include "common.h"

#include "cpu.h"
#include "kernel.h"

#define USERLAND_CODE __attribute__((section(".userland.text")))
#define USERLAND_DATA __attribute__((section(".userland.data")))

static USERLAND_DATA bool got_user_mode = false;

USERLAND_CODE void _user_task_entry() 
{
    got_user_mode = true;
    asm volatile("mov $0xDEADBEEF, %%rax\n\tint $0x81\n" : : : "rax");
//    if(global_ticks > 5) got_user_mode = true;
    while(1) __pause();
}

