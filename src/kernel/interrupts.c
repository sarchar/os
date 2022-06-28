#include "common.h"

#include "cpu.h"
#include "efifb.h"
#include "idt.h"
#include "interrupts.h"
#include "kernel.h"

// Temporarily use PIC to enable some basic interrputs. This will all be wiped once APIC is implemented.
#define PIC1_COMMAND    0x20        // IO base address for master PIC
#define PIC1_DATA       (PIC1_COMMAND+1)
#define PIC2_COMMAND    0xA0        // IO base address for slave PIC
#define PIC2_DATA       (PIC2_COMMAND+1)

#define PIC_EOI         0x20        // End-of-interrupt command code
#define PIC_READ_IRR    0x0A        // OCW3 irq ready next CMD read 
#define PIC_READ_ISR    0x0B        // OCW3 irq service next CMD read

static inline void pic_send_eoi(u8 irq)
{
    if(irq >= 8) __outb(PIC2_COMMAND, PIC_EOI);
    __outb(PIC1_COMMAND, PIC_EOI);
}

// Helper func
static u16 _pic_get_irq_reg(u8 ocw3)
{
    // OCW3 to PIC CMD to get the register values.  PIC2 is chained, and represents IRQs 8-15.  PIC1 is IRQs 0-7, with 2 being the chain 
    __outb(PIC1_COMMAND, ocw3);
    __outb(PIC2_COMMAND, ocw3);
    return (__inb(PIC2_COMMAND) << 8) | __inb(PIC1_COMMAND);
}

// Returns the combined value of the cascaded PICs irq request register
static u16 pic_get_irr(void)
{
    return _pic_get_irq_reg(PIC_READ_IRR);
}

/* Returns the combined value of the cascaded PICs in-service register */
static u16 pic_get_isr(void)
{
    return _pic_get_irq_reg(PIC_READ_ISR);
}

// reinitialize the PIC controllers, giving them specified vector offsets rather than 8h and 70h, as configured by default
#define ICW1_ICW4       0x01        // ICW4 (not) needed
#define ICW1_SINGLE     0x02        // Single (cascade) mode
#define ICW1_INTERVAL4  0x04        // Call address interval 4 (8)
#define ICW1_LEVEL      0x08        // Level triggered (edge) mode
#define ICW1_INIT       0x10        // Initialization - required!
 
#define ICW4_8086       0x01        // 8086/88 (MCS-80/85) mode
#define ICW4_AUTO       0x02        // Auto (normal) EOI
#define ICW4_BUF_SLAVE  0x08        // Buffered mode/slave
#define ICW4_BUF_MASTER 0x0C        // Buffered mode/master
#define ICW4_SFNM       0x10        // Special fully nested (not)

// arguments:
//  offset1 - vector offset for master PIC
//            vectors on the master become offset1..offset1+7
//  offset2 - same for slave PIC: offset2..offset2+7
static void pic_remap(u8 offset1, u8 offset2)
{
    u8 a1, a2;

    // the PICs put out the interrupt mask if no command word has been written
    a1 = __inb(PIC1_DATA);                      // save masks
    a2 = __inb(PIC2_DATA);

    // starts the initialization sequence (in cascade mode)
    __outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    __io_wait();
    __outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    __io_wait();

    // ICW2 (control word 2): master PIC vector offset
    __outb(PIC1_DATA, offset1);
    __io_wait();

    // ICW2: Slave PIC vector offset
    __outb(PIC2_DATA, offset2);
    __io_wait();

    // ICW3: tell Master PIC that there is a slave PIC at IRQ2 (0000 0100)
    __outb(PIC1_DATA, 0x04);
    __io_wait();

    // ICW3: tell Slave PIC its cascade identity (0000 0010)
    __outb(PIC2_DATA, 0x02);
    __io_wait();

    // ICW4: put PICs into 8086 compat mode
    __outb(PIC1_DATA, ICW4_8086);
    __io_wait();
    __outb(PIC2_DATA, ICW4_8086);
    __io_wait();

    // restore the saved irq masks
    __outb(PIC1_DATA, a1);
    __outb(PIC2_DATA, a2);
}

static void pic_set_mask(u8 irq) 
{
    u16 port;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }

    __outb(port, __inb(port) | (1 << irq));        
}

static void pic_clear_mask(u8 irq) 
{
    u16 port;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }

    __outb(port, __inb(port) & ~(1 << irq));
}

void interrupts_init()
{
    idt_init();

    // remap the PIC irqs to 0x20-0x2F
    pic_remap(0x20, 0x28); // TODO when switching to APIC, see https://wiki.osdev.org/PIC#Disabling first!

    // enable keyboard interrupt IRQ 1
    pic_clear_mask(1);     // TODO handle spurious IRQs, see https://wiki.osdev.org/PIC#Spurious_IRQs

    // reset the PS/2 keyboard
    u8 data = __inb(0x61);     
    __outb(0x61, data | 0x80);  //Disables the keyboard  
    __io_wait();
    __outb(0x61, data & 0x7F);  //Enables the keyboard  

    __sti(); // enable interrupts
}

void interrupt_stub()
{
    kernel_panic(COLOR(255, 0, 0));
    return;
}

void interrupt_stub_noerr()
{
    kernel_panic(COLOR(255, 255, 0));
    return;
}

extern volatile u32 blocking;
extern u8 scancode;

void _interrupt_kb_handler()
{
    static u8 count = 0;
    blocking++;

    scancode = __inb(0x60); // read the keyboard character
    pic_send_eoi(1);
}

// Define an ISR stub that makes a call to a C function 
__asm__(
    ".global interrupt_kb_handler\n"
    ".align 8\n"
    "interrupt_kb_handler:\n"
    "\t" "push %rdx\n"  // Save registers. TODO save allll the registers. Right now only A and D are being used in _interrupt_kb_handler
    "\t" "push %rax\n"
    "\t" "cld\n"        // C code following the SysV ABI requires DF to be clear on function entry
    "\t" "call _interrupt_kb_handler\n"   // Call actual handler
    "\t" "pop %rax\n"   // Restore all the registers
    "\t" "pop %rdx\n"
    "\t" "iretq\n"      // Return from interrupt
);

