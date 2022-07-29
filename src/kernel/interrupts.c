#include "common.h"

#include "apic.h"
#include "cpu.h"
#include "efifb.h"
#include "idt.h"
#include "interrupts.h"
#include "kernel.h"
#include "stdio.h"
#include "terminal.h"

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

static void _default_installable_irq_handler(intp pc, void* userdata)
{
    unused(pc);
    unused(userdata);
}

static struct {
    installable_irq_handler* handler;
    void* userdata;
} installable_irq_handlers[NUM_INTERRUPTS - 32];

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

    // install default handlers for all the installable irqs
    for(u32 i = 0; i < countof(installable_irq_handlers); i++) {
        installable_irq_handlers[i].handler = _default_installable_irq_handler;
        installable_irq_handlers[i].userdata = null;
    }

    __sti(); // enable interrupts
}

void interrupts_install_handler(u8 vector, installable_irq_handler* handler, void* userdata)
{
    installable_irq_handlers[vector - 32].handler = handler;
    installable_irq_handlers[vector - 32].userdata = userdata;
}

static void _call_installable_handler(intp fault_addr, u64 irq_vector)
{
    installable_irq_handlers[irq_vector-32].handler(fault_addr, installable_irq_handlers[irq_vector-32].userdata);
}

extern void _interrupt_handler_common(void);

// Note: using 'movq' doesn't actually encode an 8-byte move, and I'm not sure why.
// But without a proper 8-byte mov, the upper long in rax isn't properly set to
// the address of the interrupt handler. So leaq is used here instead.
#define DEFINE_INTERRUPT_HANDLER(v,name)           \
    __asm__(                                       \
        ".extern _" #name "\n"                     \
        ".global " #name "\n"                      \
        ".align 128\n"                              \
        #name ":\n"                                \
        "\t" "push %rax\n"                         /* save rax */                                     \
        "\t" "push %rdx\n"                         /* save rdx */                                     \
        "\t" "push %rdi\n"                         /* save rdi */                                     \
        "\t" "push %rsi\n"                         /* save rsi */                                     \
        "\t" "mov 32(%rsp), %rdi\n"                /* load the saved rip into rdi */                  \
        "\t" "movabs $_" #name ", %rax\n"          /* load the actual irq handler address into rax */ \
        "\t" "mov $" #v ", %rsi\n"                 /* place vector number into rdx */                 \
        "\t" "jmp _interrupt_handler_common\n"     \
    );                                             \
    void _##name(void* fault_addr, u64 irq_vector)

#define DEFINE_INTERRUPT_HANDLER_ERR(v,name)       \
    __asm__(                                       \
        ".extern _" #name "\n"                     \
        ".global " #name "\n"                      \
        ".align 128\n"                             \
        #name ":\n"                                \
        "\t" "xchg %rax,0(%rsp)\n"                 /* save rax by swapping the error code with it */  \
        "\t" "push %rdx\n"                         /* save rdx */                                     \
        "\t" "push %rdi\n"                         /* save rdi */                                     \
        "\t" "mov %rax, %rdi\n"                    /* move the cpu error code into rdi */             \
        "\t" "push %rsi\n"                         /* save rsi */                                     \
        "\t" "mov 24(%rsp), %rsi\n"                /* load the saved rip into rsi */                  \
        "\t" "movabs $_" #name ", %rax\n"          /* load the actual irq handler address into rax */ \
        "\t" "mov $" #v ", %rdx\n"                 /* place vector number into rdx */                 \
        "\t" "jmp _interrupt_handler_common\n"     \
    );                                             \
    void _##name(u64 error_code, void* fault_addr, u64 irq_vector)

DEFINE_INTERRUPT_HANDLER_ERR(255, interrupt_stub)
{
    unused(error_code);
    unused(fault_addr);
    unused(irq_vector);
    kernel_panic(COLOR(255, 0, 0));
}

DEFINE_INTERRUPT_HANDLER(255, interrupt_stub_noerr)
{
    unused(fault_addr);
    unused(irq_vector);
    kernel_panic(COLOR(255, 255, 0));
}

DEFINE_INTERRUPT_HANDLER(0, interrupt_div_by_zero)
{
    unused(irq_vector);
    fprintf(stderr, "division by zero at address $%lX ", fault_addr);
    kernel_panic(COLOR(255, 128, 128));
}

DEFINE_INTERRUPT_HANDLER_ERR(13, interrupt_gpf)
{
    unused(irq_vector);
    fprintf(stderr, "general protection fault: error = $%lX at address $%lX\n", error_code, fault_addr);
    kernel_panic(COLOR(255, 0, 0));
}

DEFINE_INTERRUPT_HANDLER_ERR(14, interrupt_page_fault)
{
    unused(irq_vector);
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

#define DEFINE_INSTALLABLE_INTERRUPT(n)                            \
    DEFINE_INTERRUPT_HANDLER(n, interrupt_installable_##n) {       \
        _call_installable_handler((intp)fault_addr, irq_vector);   \
    }

DEFINE_INSTALLABLE_INTERRUPT(32);
DEFINE_INSTALLABLE_INTERRUPT(33);
DEFINE_INSTALLABLE_INTERRUPT(34);
DEFINE_INSTALLABLE_INTERRUPT(35);
DEFINE_INSTALLABLE_INTERRUPT(36);
DEFINE_INSTALLABLE_INTERRUPT(37);
DEFINE_INSTALLABLE_INTERRUPT(38);
DEFINE_INSTALLABLE_INTERRUPT(39);
DEFINE_INSTALLABLE_INTERRUPT(40);
DEFINE_INSTALLABLE_INTERRUPT(41);
DEFINE_INSTALLABLE_INTERRUPT(42);
DEFINE_INSTALLABLE_INTERRUPT(43);
DEFINE_INSTALLABLE_INTERRUPT(44);
DEFINE_INSTALLABLE_INTERRUPT(45);
DEFINE_INSTALLABLE_INTERRUPT(46);
DEFINE_INSTALLABLE_INTERRUPT(47);
DEFINE_INSTALLABLE_INTERRUPT(48);
DEFINE_INSTALLABLE_INTERRUPT(49);
DEFINE_INSTALLABLE_INTERRUPT(50);
DEFINE_INSTALLABLE_INTERRUPT(51);
DEFINE_INSTALLABLE_INTERRUPT(52);
DEFINE_INSTALLABLE_INTERRUPT(53);
DEFINE_INSTALLABLE_INTERRUPT(54);
DEFINE_INSTALLABLE_INTERRUPT(55);
DEFINE_INSTALLABLE_INTERRUPT(56);
DEFINE_INSTALLABLE_INTERRUPT(57);
DEFINE_INSTALLABLE_INTERRUPT(58);
DEFINE_INSTALLABLE_INTERRUPT(59);
DEFINE_INSTALLABLE_INTERRUPT(60);
DEFINE_INSTALLABLE_INTERRUPT(61);
DEFINE_INSTALLABLE_INTERRUPT(62);
DEFINE_INSTALLABLE_INTERRUPT(63);
DEFINE_INSTALLABLE_INTERRUPT(64);
DEFINE_INSTALLABLE_INTERRUPT(65);
DEFINE_INSTALLABLE_INTERRUPT(66);
DEFINE_INSTALLABLE_INTERRUPT(67);
DEFINE_INSTALLABLE_INTERRUPT(68);
DEFINE_INSTALLABLE_INTERRUPT(69);
DEFINE_INSTALLABLE_INTERRUPT(70);
DEFINE_INSTALLABLE_INTERRUPT(71);
DEFINE_INSTALLABLE_INTERRUPT(72);
DEFINE_INSTALLABLE_INTERRUPT(73);
DEFINE_INSTALLABLE_INTERRUPT(74);
DEFINE_INSTALLABLE_INTERRUPT(75);
DEFINE_INSTALLABLE_INTERRUPT(76);
DEFINE_INSTALLABLE_INTERRUPT(77);
DEFINE_INSTALLABLE_INTERRUPT(78);
DEFINE_INSTALLABLE_INTERRUPT(79);
DEFINE_INSTALLABLE_INTERRUPT(80);
DEFINE_INSTALLABLE_INTERRUPT(81);
DEFINE_INSTALLABLE_INTERRUPT(82);
DEFINE_INSTALLABLE_INTERRUPT(83);
DEFINE_INSTALLABLE_INTERRUPT(84);
DEFINE_INSTALLABLE_INTERRUPT(85);
DEFINE_INSTALLABLE_INTERRUPT(86);
DEFINE_INSTALLABLE_INTERRUPT(87);
DEFINE_INSTALLABLE_INTERRUPT(88);
DEFINE_INSTALLABLE_INTERRUPT(89);
DEFINE_INSTALLABLE_INTERRUPT(90);
DEFINE_INSTALLABLE_INTERRUPT(91);
DEFINE_INSTALLABLE_INTERRUPT(92);
DEFINE_INSTALLABLE_INTERRUPT(93);
DEFINE_INSTALLABLE_INTERRUPT(94);
DEFINE_INSTALLABLE_INTERRUPT(95);
DEFINE_INSTALLABLE_INTERRUPT(96);
DEFINE_INSTALLABLE_INTERRUPT(97);
DEFINE_INSTALLABLE_INTERRUPT(98);
DEFINE_INSTALLABLE_INTERRUPT(99);
DEFINE_INSTALLABLE_INTERRUPT(100);
DEFINE_INSTALLABLE_INTERRUPT(101);
DEFINE_INSTALLABLE_INTERRUPT(102);
DEFINE_INSTALLABLE_INTERRUPT(103);
DEFINE_INSTALLABLE_INTERRUPT(104);
DEFINE_INSTALLABLE_INTERRUPT(105);
DEFINE_INSTALLABLE_INTERRUPT(106);
DEFINE_INSTALLABLE_INTERRUPT(107);
DEFINE_INSTALLABLE_INTERRUPT(108);
DEFINE_INSTALLABLE_INTERRUPT(109);
DEFINE_INSTALLABLE_INTERRUPT(110);
DEFINE_INSTALLABLE_INTERRUPT(111);
DEFINE_INSTALLABLE_INTERRUPT(112);
DEFINE_INSTALLABLE_INTERRUPT(113);
DEFINE_INSTALLABLE_INTERRUPT(114);
DEFINE_INSTALLABLE_INTERRUPT(115);
DEFINE_INSTALLABLE_INTERRUPT(116);
DEFINE_INSTALLABLE_INTERRUPT(117);
DEFINE_INSTALLABLE_INTERRUPT(118);
DEFINE_INSTALLABLE_INTERRUPT(119);
DEFINE_INSTALLABLE_INTERRUPT(120);
DEFINE_INSTALLABLE_INTERRUPT(121);
DEFINE_INSTALLABLE_INTERRUPT(122);
DEFINE_INSTALLABLE_INTERRUPT(123);
DEFINE_INSTALLABLE_INTERRUPT(124);
DEFINE_INSTALLABLE_INTERRUPT(125);
DEFINE_INSTALLABLE_INTERRUPT(126);
DEFINE_INSTALLABLE_INTERRUPT(127);
DEFINE_INSTALLABLE_INTERRUPT(128);
DEFINE_INSTALLABLE_INTERRUPT(129);
DEFINE_INSTALLABLE_INTERRUPT(130);
DEFINE_INSTALLABLE_INTERRUPT(131);
DEFINE_INSTALLABLE_INTERRUPT(132);
DEFINE_INSTALLABLE_INTERRUPT(133);
DEFINE_INSTALLABLE_INTERRUPT(134);
DEFINE_INSTALLABLE_INTERRUPT(135);
DEFINE_INSTALLABLE_INTERRUPT(136);
DEFINE_INSTALLABLE_INTERRUPT(137);
DEFINE_INSTALLABLE_INTERRUPT(138);
DEFINE_INSTALLABLE_INTERRUPT(139);
DEFINE_INSTALLABLE_INTERRUPT(140);
DEFINE_INSTALLABLE_INTERRUPT(141);
DEFINE_INSTALLABLE_INTERRUPT(142);
DEFINE_INSTALLABLE_INTERRUPT(143);
DEFINE_INSTALLABLE_INTERRUPT(144);
DEFINE_INSTALLABLE_INTERRUPT(145);
DEFINE_INSTALLABLE_INTERRUPT(146);
DEFINE_INSTALLABLE_INTERRUPT(147);
DEFINE_INSTALLABLE_INTERRUPT(148);
DEFINE_INSTALLABLE_INTERRUPT(149);
DEFINE_INSTALLABLE_INTERRUPT(150);
DEFINE_INSTALLABLE_INTERRUPT(151);
DEFINE_INSTALLABLE_INTERRUPT(152);
DEFINE_INSTALLABLE_INTERRUPT(153);
DEFINE_INSTALLABLE_INTERRUPT(154);
DEFINE_INSTALLABLE_INTERRUPT(155);
DEFINE_INSTALLABLE_INTERRUPT(156);
DEFINE_INSTALLABLE_INTERRUPT(157);
DEFINE_INSTALLABLE_INTERRUPT(158);
DEFINE_INSTALLABLE_INTERRUPT(159);
DEFINE_INSTALLABLE_INTERRUPT(160);
DEFINE_INSTALLABLE_INTERRUPT(161);
DEFINE_INSTALLABLE_INTERRUPT(162);
DEFINE_INSTALLABLE_INTERRUPT(163);
DEFINE_INSTALLABLE_INTERRUPT(164);
DEFINE_INSTALLABLE_INTERRUPT(165);
DEFINE_INSTALLABLE_INTERRUPT(166);
DEFINE_INSTALLABLE_INTERRUPT(167);
DEFINE_INSTALLABLE_INTERRUPT(168);
DEFINE_INSTALLABLE_INTERRUPT(169);
DEFINE_INSTALLABLE_INTERRUPT(170);
DEFINE_INSTALLABLE_INTERRUPT(171);
DEFINE_INSTALLABLE_INTERRUPT(172);
DEFINE_INSTALLABLE_INTERRUPT(173);
DEFINE_INSTALLABLE_INTERRUPT(174);
DEFINE_INSTALLABLE_INTERRUPT(175);
DEFINE_INSTALLABLE_INTERRUPT(176);
DEFINE_INSTALLABLE_INTERRUPT(177);
DEFINE_INSTALLABLE_INTERRUPT(178);
DEFINE_INSTALLABLE_INTERRUPT(179);
DEFINE_INSTALLABLE_INTERRUPT(180);
DEFINE_INSTALLABLE_INTERRUPT(181);
DEFINE_INSTALLABLE_INTERRUPT(182);
DEFINE_INSTALLABLE_INTERRUPT(183);
DEFINE_INSTALLABLE_INTERRUPT(184);
DEFINE_INSTALLABLE_INTERRUPT(185);
DEFINE_INSTALLABLE_INTERRUPT(186);
DEFINE_INSTALLABLE_INTERRUPT(187);
DEFINE_INSTALLABLE_INTERRUPT(188);
DEFINE_INSTALLABLE_INTERRUPT(189);
DEFINE_INSTALLABLE_INTERRUPT(190);
DEFINE_INSTALLABLE_INTERRUPT(191);
DEFINE_INSTALLABLE_INTERRUPT(192);
DEFINE_INSTALLABLE_INTERRUPT(193);
DEFINE_INSTALLABLE_INTERRUPT(194);
DEFINE_INSTALLABLE_INTERRUPT(195);
DEFINE_INSTALLABLE_INTERRUPT(196);
DEFINE_INSTALLABLE_INTERRUPT(197);
DEFINE_INSTALLABLE_INTERRUPT(198);
DEFINE_INSTALLABLE_INTERRUPT(199);
DEFINE_INSTALLABLE_INTERRUPT(200);
DEFINE_INSTALLABLE_INTERRUPT(201);
DEFINE_INSTALLABLE_INTERRUPT(202);
DEFINE_INSTALLABLE_INTERRUPT(203);
DEFINE_INSTALLABLE_INTERRUPT(204);
DEFINE_INSTALLABLE_INTERRUPT(205);
DEFINE_INSTALLABLE_INTERRUPT(206);
DEFINE_INSTALLABLE_INTERRUPT(207);
DEFINE_INSTALLABLE_INTERRUPT(208);
DEFINE_INSTALLABLE_INTERRUPT(209);
DEFINE_INSTALLABLE_INTERRUPT(210);
DEFINE_INSTALLABLE_INTERRUPT(211);
DEFINE_INSTALLABLE_INTERRUPT(212);
DEFINE_INSTALLABLE_INTERRUPT(213);
DEFINE_INSTALLABLE_INTERRUPT(214);
DEFINE_INSTALLABLE_INTERRUPT(215);
DEFINE_INSTALLABLE_INTERRUPT(216);
DEFINE_INSTALLABLE_INTERRUPT(217);
DEFINE_INSTALLABLE_INTERRUPT(218);
DEFINE_INSTALLABLE_INTERRUPT(219);
DEFINE_INSTALLABLE_INTERRUPT(220);
DEFINE_INSTALLABLE_INTERRUPT(221);
DEFINE_INSTALLABLE_INTERRUPT(222);
DEFINE_INSTALLABLE_INTERRUPT(223);
DEFINE_INSTALLABLE_INTERRUPT(224);
DEFINE_INSTALLABLE_INTERRUPT(225);
DEFINE_INSTALLABLE_INTERRUPT(226);
DEFINE_INSTALLABLE_INTERRUPT(227);
DEFINE_INSTALLABLE_INTERRUPT(228);
DEFINE_INSTALLABLE_INTERRUPT(229);
DEFINE_INSTALLABLE_INTERRUPT(230);
DEFINE_INSTALLABLE_INTERRUPT(231);
DEFINE_INSTALLABLE_INTERRUPT(232);
DEFINE_INSTALLABLE_INTERRUPT(233);
DEFINE_INSTALLABLE_INTERRUPT(234);
DEFINE_INSTALLABLE_INTERRUPT(235);
DEFINE_INSTALLABLE_INTERRUPT(236);
DEFINE_INSTALLABLE_INTERRUPT(237);
DEFINE_INSTALLABLE_INTERRUPT(238);
DEFINE_INSTALLABLE_INTERRUPT(239);
DEFINE_INSTALLABLE_INTERRUPT(240);
DEFINE_INSTALLABLE_INTERRUPT(241);
DEFINE_INSTALLABLE_INTERRUPT(242);
DEFINE_INSTALLABLE_INTERRUPT(243);
DEFINE_INSTALLABLE_INTERRUPT(244);
DEFINE_INSTALLABLE_INTERRUPT(245);
DEFINE_INSTALLABLE_INTERRUPT(246);
DEFINE_INSTALLABLE_INTERRUPT(247);
DEFINE_INSTALLABLE_INTERRUPT(248);
DEFINE_INSTALLABLE_INTERRUPT(249);
DEFINE_INSTALLABLE_INTERRUPT(250);
DEFINE_INSTALLABLE_INTERRUPT(251);
DEFINE_INSTALLABLE_INTERRUPT(252);
DEFINE_INSTALLABLE_INTERRUPT(253);
DEFINE_INSTALLABLE_INTERRUPT(254);
DEFINE_INSTALLABLE_INTERRUPT(255);

u8 ahci_irq = 0;
