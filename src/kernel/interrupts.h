#ifndef __INTERRUPTS_H__
#define __INTERRUPTS_H__

#define NUM_INTERRUPTS 256

typedef void (*interrupt_handler)();

void interrupts_init();

// the interrupt handlers, externed for idt.c
void interrupt_stub();
void interrupt_stub_noerr();

void interrupt_div_by_zero();
void interrupt_gpf();
void interrupt_page_fault();
void interrupt_kb_handler();
void interrupt_timer();

#endif
