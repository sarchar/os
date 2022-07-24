#include "common.h"

#include "apic.h"
#include "cpu.h"
#include "efifb.h"
#include "idt.h"
#include "interrupts.h"
#include "kernel.h"
#include "stdio.h"
#include "terminal.h"

u64 volatile master_ticks = 0;

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
#define ICW4_MASTER     0x04        // Master/slave
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
    __outb(PIC1_DATA, ICW4_8086 | ICW4_MASTER);
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

void _disable_pic()
{
    unused(pic_clear_mask);
    unused(pic_set_mask);
    unused(pic_get_isr);
    unused(pic_get_irr);

    // remap the PIC irqs to 0x20-0x2F
    pic_remap(0x20, 0x28); // TODO when switching to APIC, see https://wiki.osdev.org/PIC#Disabling first!

    // mask out all IRQs
    __outb(PIC1_DATA, 0xFF);
    __outb(PIC2_DATA, 0xFF);
}

void interrupts_init()
{
    _disable_pic();

    idt_init();

    apic_init();

    // reset the PS/2 keyboard
    u8 data = __inb(0x61);     
    __outb(0x61, data | 0x80);  //Disables the keyboard  
    __io_wait();
    __outb(0x61, data & 0x7F);  //Enables the keyboard  
    __io_wait();
    __inb(0x60);                //Clear the ps/2 output, as it generates an extra irq

    __sti(); // enable interrupts
}

extern void _interrupt_handler_common(void);

// Note: using 'movq' doesn't actually encode an 8-byte move, and I'm not sure why.
// But without a proper 8-byte mov, the upper long in rax isn't properly set to
// the address of the interrupt handler. So leaq is used here instead.
#define DEFINE_INTERRUPT_HANDLER(name)             \
    __asm__(                                       \
        ".extern _" #name "\n"                     \
        ".global " #name "\n"                      \
        ".align 16\n"                              \
        #name ":\n"                                \
        "\t" "push %rax\n"                         /* save rax */                                     \
        "\t" "push %rdi\n"                         /* save rdi */                                     \
        "\t" "push %rsi\n"                         /* save rsi */                                     \
        "\t" "mov 24(%rsp), %rdi\n"                /* load the saved rip into rdi */                  \
        "\t" "movabs $_" #name ", %rax\n"          /* load the actual irq handler address into rax */ \
        "\t" "jmp _interrupt_handler_common\n"     \
    );                                             \
    void _##name(void* fault_addr)

#define DEFINE_INTERRUPT_HANDLER_ERR(name)         \
    __asm__(                                       \
        ".extern _" #name "\n"                     \
        ".global " #name "\n"                      \
        ".align 16\n"                              \
        #name ":\n"                                \
        "\t" "xchg %rax,0(%rsp)\n"                 /* save rax by swapping the error code with it */  \
        "\t" "push %rdi\n"                         /* save rdi */                                     \
        "\t" "mov %rax, %rdi\n"                    /* move the cpu error code into rdi */             \
        "\t" "push %rsi\n"                         /* save rsi */                                     \
        "\t" "mov 24(%rsp), %rsi\n"                /* load the saved rip into rsi */                  \
        "\t" "movabs $_" #name ", %rax\n"          /* load the actual irq handler address into rax */ \
        "\t" "jmp _interrupt_handler_common\n"     \
    );                                             \
    void _##name(u64 error_code, void* fault_addr)

DEFINE_INTERRUPT_HANDLER_ERR(interrupt_stub)
{
    unused(error_code);
    unused(fault_addr);
    kernel_panic(COLOR(255, 0, 0));
}

DEFINE_INTERRUPT_HANDLER(interrupt_stub_noerr)
{
    unused(fault_addr);
    kernel_panic(COLOR(255, 255, 0));
}

DEFINE_INTERRUPT_HANDLER(interrupt_div_by_zero)
{
    fprintf(stderr, "division by zero at address $%lX ", fault_addr);
    kernel_panic(COLOR(255, 128, 128));
}

DEFINE_INTERRUPT_HANDLER_ERR(interrupt_gpf)
{
    fprintf(stderr, "general protection fault: error = $%lX at address $%lX\n", error_code, fault_addr);
    kernel_panic(COLOR(255, 0, 0));
}

DEFINE_INTERRUPT_HANDLER_ERR(interrupt_page_fault)
{
    fprintf(stderr, "page fault: error = $%lX at address $%lX ", error_code, fault_addr);

    u8 rw = (error_code & 0x02);

    u64 access_address = __rdcr2();
    fprintf(stderr, " %s $%lX\n", rw ? "writing" : "reading", (intp)access_address);

    // put a page at the access_address 
#if 0
    intp new_page = (intp)palloc_claim(0);
    paging_map_page(new_page, (intp)access_address);
#else
    kernel_panic(COLOR(0, 255, 0));
#endif
}

extern volatile u32 blocking;
extern u8 scancode;

DEFINE_INTERRUPT_HANDLER(interrupt_kb_handler)
{
    unused(fault_addr);

    // read the keyboard character
    scancode = __inb(0x60);
    blocking++;

    // all PIC interrupts need to send the controller the end-of-interrupt command
    //pic_send_eoi(1);
}

DEFINE_INTERRUPT_HANDLER(interrupt_timer)
{
    unused(fault_addr);
    master_ticks++;
}

