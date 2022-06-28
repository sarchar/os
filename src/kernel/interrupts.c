#include "common.h"

#include "cpu.h"
#include "idt.h"
#include "interrupts.h"
#include "kernel.h"

void interrupts_init()
{
    idt_init();

    __sti(); // enable interrupts
}

void interrupt_stub()
{
    kernel_panic();
    return;
}

void interrupt_stub_noerr()
{
    kernel_panic();
    return;
}

