// The basis for this module is taken from https://wiki.osdev.org/Interrupts_tutorial
#include "common.h"

#include "cpu.h"
#include "idt.h"
#include "interrupts.h"
#include "kernel.h"

#define IDT_FLAG_GATE_TYPE_INTERRUPT 0x0E
#define IDT_FLAG_GATE_TYPE_TRAP      0x0F
#define IDT_FLAG_PRIVILEGE_LEVEL0    (0 << 5)
#define IDT_FLAG_PRIVILEGE_LEVEL1    (1 << 5)
#define IDT_FLAG_PRIVILEGE_LEVEL2    (2 << 5)
#define IDT_FLAG_PRIVILEGE_LEVEL3    (3 << 5)
#define IDT_FLAG_PRESENT             0x80

// see https://wiki.osdev.org/Interrupt_Descriptor_Table#Structure_on_x86-64 for the layout of this structure
struct idt_entry {
    u16    offset_1;     // The lower 16 bits of the interrupt handler's address
    u16    kernel_cs;    // The GDT segment selector that the CPU will load into CS before calling the interrupt handler
    u8     ist;          // The IST in the TSS that the CPU will load into RSP
    u8     attributes;   // Type and attributes; see the IDT page
    u16    offset_2;     // The higher 16 bits of the lower 32 bits of the interrupt handler's address
    u32    offset_3;     // The higher 32 bits of the interrupt handler's address
    u32    reserved;     // Set to zero
} __packed;

// descriptor used to tell the cpu about our IDT.
struct idtr {
    u16    limit;
    u64    base;
} __packed;

__aligned(16) static struct idt_entry _idt[NUM_INTERRUPTS] = { { 0, }, }; // Create an array of IDT entries
__aligned(16) static struct idtr _idtr;

void idt_set_entry(u8 vector_number, void* interrupt_handler, u8 flags) 
{
    struct idt_entry* entry = &_idt[vector_number];
    
    entry->offset_1   = (intp)interrupt_handler & 0xFFFF;
    entry->kernel_cs  = 8; //GDT_OFFSET_KERNEL_CODE; (value of GDT.code in boot.asm)
    entry->ist        = 0;
    entry->attributes = flags;
    entry->offset_2   = ((intp)interrupt_handler >> 16) & 0xFFFF;
    entry->offset_3   = ((intp)interrupt_handler >> 32) & 0xFFFFFFFF;
    entry->reserved   = 0;
}

void idt_init() 
{
    _idtr.base = (intp)&_idt[0] - (intp)&_kernel_vma_base; // the cpu wants a physical address
    _idtr.limit = (u16)(sizeof(_idt) - 1);
    
    idt_set_entry( 0, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry( 1, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry( 2, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry( 3, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry( 4, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry( 5, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry( 6, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry( 7, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry( 8, interrupt_stub,       IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry( 9, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(10, interrupt_stub,       IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(11, interrupt_stub,       IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(12, interrupt_stub,       IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(13, interrupt_gpf,        IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT); // general protection fault
    idt_set_entry(14, interrupt_page_fault, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT); // page fault
    idt_set_entry(15, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(16, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(17, interrupt_stub,       IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(18, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(19, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(20, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(21, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(22, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(23, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(24, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(25, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(26, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(27, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(28, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(29, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(30, interrupt_stub,       IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(31, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(32, interrupt_stub_noerr, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);
    idt_set_entry(33, interrupt_kb_handler, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT);

    // TODO I'm sure there's a cleaner way to make gcc do this properly
    // but I haven't figured it out yet.
    __asm__ volatile ("movabs %0, %%rax\n\t" 
                      "movabs %1, %%rdx\n\t"
                      "subq %%rdx, %%rax\n\t"
                      "lidt (%%rax)\n\t"
                        : : "i"(&_idtr), "i"(&_kernel_vma_base) : "rax", "rdx");
}


