// The basis for this module is taken from https://wiki.osdev.org/Interrupts_tutorial
#include "common.h"

#include "cpu.h"
#include "idt.h"
#include "interrupts.h"
#include "kernel.h"
#include "paging.h"

enum IDT_FLAGS {
    IDT_FLAG_GATE_TYPE_INTERRUPT = 0x0E,
    IDT_FLAG_GATE_TYPE_TRAP      = 0x0F,
    IDT_FLAG_PRIVILEGE_LEVEL0    = (0 << 5),
    IDT_FLAG_PRIVILEGE_LEVEL1    = (1 << 5),
    IDT_FLAG_PRIVILEGE_LEVEL2    = (2 << 5),
    IDT_FLAG_PRIVILEGE_LEVEL3    = (3 << 5),
    IDT_FLAG_PRESENT             = 0x80
};

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
   
    // the first 32 interrupts are internal to the cpu
    idt_set_entry( 0, interrupt_div_by_zero, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // division by zero
    idt_set_entry( 1, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // debug
    idt_set_entry( 2, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // NMI
    idt_set_entry( 3, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // breakpoint
    idt_set_entry( 4, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // overflow
    idt_set_entry( 5, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // bound range exceeded
    idt_set_entry( 6, interrupt_invalid_op , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // invalid opcode
    idt_set_entry( 7, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // device not available
    idt_set_entry( 8, interrupt_stub       , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // double fault
    idt_set_entry( 9, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // cop segment overrun
    idt_set_entry(10, interrupt_stub       , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // invalid tss
    idt_set_entry(11, interrupt_stub       , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // segment not present
    idt_set_entry(12, interrupt_stub       , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // stack-segment fault
    idt_set_entry(13, interrupt_gpf        , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // general protection fault
    idt_set_entry(14, interrupt_page_fault , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // page fault
    idt_set_entry(15, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // reserved
    idt_set_entry(16, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // x87 floating point exception
    idt_set_entry(17, interrupt_stub       , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // alignment check
    idt_set_entry(18, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // machine check
    idt_set_entry(19, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // SIMD floating-point exception
    idt_set_entry(20, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // virtualization exception
    idt_set_entry(21, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // control protection exception
    idt_set_entry(22, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // reserved
    idt_set_entry(23, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // reserved
    idt_set_entry(24, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // reserved
    idt_set_entry(25, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // reserved
    idt_set_entry(26, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // reserved
    idt_set_entry(27, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // reserved
    idt_set_entry(28, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // hypervisor injection exception
    idt_set_entry(29, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // VMM communication exception
    idt_set_entry(30, interrupt_stub       , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // security exception
    idt_set_entry(31, interrupt_stub_noerr , IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_TRAP); // reserved

    // install the system call interrupt at 0x81 with privilege level of 3, allowing ring 3 code to enter the kernel here
    idt_set_entry(0x81, interrupt_syscall, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL3 | IDT_FLAG_GATE_TYPE_INTERRUPT);

    // Set the rest of the interrupt handlers as installable interrupts
#define SET_INSTALLABLE_ENTRY(n) idt_set_entry(n, interrupt_installable_##n, IDT_FLAG_PRESENT | IDT_FLAG_PRIVILEGE_LEVEL0 | IDT_FLAG_GATE_TYPE_INTERRUPT)
    SET_INSTALLABLE_ENTRY(32);
    SET_INSTALLABLE_ENTRY(33);
    SET_INSTALLABLE_ENTRY(34);
    SET_INSTALLABLE_ENTRY(35);
    SET_INSTALLABLE_ENTRY(36);
    SET_INSTALLABLE_ENTRY(37);
    SET_INSTALLABLE_ENTRY(38);
    SET_INSTALLABLE_ENTRY(39);
    SET_INSTALLABLE_ENTRY(40);
    SET_INSTALLABLE_ENTRY(41);
    SET_INSTALLABLE_ENTRY(42);
    SET_INSTALLABLE_ENTRY(43);
    SET_INSTALLABLE_ENTRY(44);
    SET_INSTALLABLE_ENTRY(45);
    SET_INSTALLABLE_ENTRY(46);
    SET_INSTALLABLE_ENTRY(47);
    SET_INSTALLABLE_ENTRY(48);
    SET_INSTALLABLE_ENTRY(49);
    SET_INSTALLABLE_ENTRY(50);
    SET_INSTALLABLE_ENTRY(51);
    SET_INSTALLABLE_ENTRY(52);
    SET_INSTALLABLE_ENTRY(53);
    SET_INSTALLABLE_ENTRY(54);
    SET_INSTALLABLE_ENTRY(55);
    SET_INSTALLABLE_ENTRY(56);
    SET_INSTALLABLE_ENTRY(57);
    SET_INSTALLABLE_ENTRY(58);
    SET_INSTALLABLE_ENTRY(59);
    SET_INSTALLABLE_ENTRY(60);
    SET_INSTALLABLE_ENTRY(61);
    SET_INSTALLABLE_ENTRY(62);
    SET_INSTALLABLE_ENTRY(63);
    SET_INSTALLABLE_ENTRY(64);
    SET_INSTALLABLE_ENTRY(65);
    SET_INSTALLABLE_ENTRY(66);
    SET_INSTALLABLE_ENTRY(67);
    SET_INSTALLABLE_ENTRY(68);
    SET_INSTALLABLE_ENTRY(69);
    SET_INSTALLABLE_ENTRY(70);
    SET_INSTALLABLE_ENTRY(71);
    SET_INSTALLABLE_ENTRY(72);
    SET_INSTALLABLE_ENTRY(73);
    SET_INSTALLABLE_ENTRY(74);
    SET_INSTALLABLE_ENTRY(75);
    SET_INSTALLABLE_ENTRY(76);
    SET_INSTALLABLE_ENTRY(77);
    SET_INSTALLABLE_ENTRY(78);
    SET_INSTALLABLE_ENTRY(79);
    SET_INSTALLABLE_ENTRY(80);
    SET_INSTALLABLE_ENTRY(81);
    SET_INSTALLABLE_ENTRY(82);
    SET_INSTALLABLE_ENTRY(83);
    SET_INSTALLABLE_ENTRY(84);
    SET_INSTALLABLE_ENTRY(85);
    SET_INSTALLABLE_ENTRY(86);
    SET_INSTALLABLE_ENTRY(87);
    SET_INSTALLABLE_ENTRY(88);
    SET_INSTALLABLE_ENTRY(89);
    SET_INSTALLABLE_ENTRY(90);
    SET_INSTALLABLE_ENTRY(91);
    SET_INSTALLABLE_ENTRY(92);
    SET_INSTALLABLE_ENTRY(93);
    SET_INSTALLABLE_ENTRY(94);
    SET_INSTALLABLE_ENTRY(95);
    SET_INSTALLABLE_ENTRY(96);
    SET_INSTALLABLE_ENTRY(97);
    SET_INSTALLABLE_ENTRY(98);
    SET_INSTALLABLE_ENTRY(99);
    SET_INSTALLABLE_ENTRY(100);
    SET_INSTALLABLE_ENTRY(101);
    SET_INSTALLABLE_ENTRY(102);
    SET_INSTALLABLE_ENTRY(103);
    SET_INSTALLABLE_ENTRY(104);
    SET_INSTALLABLE_ENTRY(105);
    SET_INSTALLABLE_ENTRY(106);
    SET_INSTALLABLE_ENTRY(107);
    SET_INSTALLABLE_ENTRY(108);
    SET_INSTALLABLE_ENTRY(109);
    SET_INSTALLABLE_ENTRY(110);
    SET_INSTALLABLE_ENTRY(111);
    SET_INSTALLABLE_ENTRY(112);
    SET_INSTALLABLE_ENTRY(113);
    SET_INSTALLABLE_ENTRY(114);
    SET_INSTALLABLE_ENTRY(115);
    SET_INSTALLABLE_ENTRY(116);
    SET_INSTALLABLE_ENTRY(117);
    SET_INSTALLABLE_ENTRY(118);
    SET_INSTALLABLE_ENTRY(119);
    SET_INSTALLABLE_ENTRY(120);
    SET_INSTALLABLE_ENTRY(121);
    SET_INSTALLABLE_ENTRY(122);
    SET_INSTALLABLE_ENTRY(123);
    SET_INSTALLABLE_ENTRY(124);
    SET_INSTALLABLE_ENTRY(125);
    SET_INSTALLABLE_ENTRY(126);
    SET_INSTALLABLE_ENTRY(127);
    SET_INSTALLABLE_ENTRY(128);
//    SET_INSTALLABLE_ENTRY(129);
    SET_INSTALLABLE_ENTRY(130);
    SET_INSTALLABLE_ENTRY(131);
    SET_INSTALLABLE_ENTRY(132);
    SET_INSTALLABLE_ENTRY(133);
    SET_INSTALLABLE_ENTRY(134);
    SET_INSTALLABLE_ENTRY(135);
    SET_INSTALLABLE_ENTRY(136);
    SET_INSTALLABLE_ENTRY(137);
    SET_INSTALLABLE_ENTRY(138);
    SET_INSTALLABLE_ENTRY(139);
    SET_INSTALLABLE_ENTRY(140);
    SET_INSTALLABLE_ENTRY(141);
    SET_INSTALLABLE_ENTRY(142);
    SET_INSTALLABLE_ENTRY(143);
    SET_INSTALLABLE_ENTRY(144);
    SET_INSTALLABLE_ENTRY(145);
    SET_INSTALLABLE_ENTRY(146);
    SET_INSTALLABLE_ENTRY(147);
    SET_INSTALLABLE_ENTRY(148);
    SET_INSTALLABLE_ENTRY(149);
    SET_INSTALLABLE_ENTRY(150);
    SET_INSTALLABLE_ENTRY(151);
    SET_INSTALLABLE_ENTRY(152);
    SET_INSTALLABLE_ENTRY(153);
    SET_INSTALLABLE_ENTRY(154);
    SET_INSTALLABLE_ENTRY(155);
    SET_INSTALLABLE_ENTRY(156);
    SET_INSTALLABLE_ENTRY(157);
    SET_INSTALLABLE_ENTRY(158);
    SET_INSTALLABLE_ENTRY(159);
    SET_INSTALLABLE_ENTRY(160);
    SET_INSTALLABLE_ENTRY(161);
    SET_INSTALLABLE_ENTRY(162);
    SET_INSTALLABLE_ENTRY(163);
    SET_INSTALLABLE_ENTRY(164);
    SET_INSTALLABLE_ENTRY(165);
    SET_INSTALLABLE_ENTRY(166);
    SET_INSTALLABLE_ENTRY(167);
    SET_INSTALLABLE_ENTRY(168);
    SET_INSTALLABLE_ENTRY(169);
    SET_INSTALLABLE_ENTRY(170);
    SET_INSTALLABLE_ENTRY(171);
    SET_INSTALLABLE_ENTRY(172);
    SET_INSTALLABLE_ENTRY(173);
    SET_INSTALLABLE_ENTRY(174);
    SET_INSTALLABLE_ENTRY(175);
    SET_INSTALLABLE_ENTRY(176);
    SET_INSTALLABLE_ENTRY(177);
    SET_INSTALLABLE_ENTRY(178);
    SET_INSTALLABLE_ENTRY(179);
    SET_INSTALLABLE_ENTRY(180);
    SET_INSTALLABLE_ENTRY(181);
    SET_INSTALLABLE_ENTRY(182);
    SET_INSTALLABLE_ENTRY(183);
    SET_INSTALLABLE_ENTRY(184);
    SET_INSTALLABLE_ENTRY(185);
    SET_INSTALLABLE_ENTRY(186);
    SET_INSTALLABLE_ENTRY(187);
    SET_INSTALLABLE_ENTRY(188);
    SET_INSTALLABLE_ENTRY(189);
    SET_INSTALLABLE_ENTRY(190);
    SET_INSTALLABLE_ENTRY(191);
    SET_INSTALLABLE_ENTRY(192);
    SET_INSTALLABLE_ENTRY(193);
    SET_INSTALLABLE_ENTRY(194);
    SET_INSTALLABLE_ENTRY(195);
    SET_INSTALLABLE_ENTRY(196);
    SET_INSTALLABLE_ENTRY(197);
    SET_INSTALLABLE_ENTRY(198);
    SET_INSTALLABLE_ENTRY(199);
    SET_INSTALLABLE_ENTRY(200);
    SET_INSTALLABLE_ENTRY(201);
    SET_INSTALLABLE_ENTRY(202);
    SET_INSTALLABLE_ENTRY(203);
    SET_INSTALLABLE_ENTRY(204);
    SET_INSTALLABLE_ENTRY(205);
    SET_INSTALLABLE_ENTRY(206);
    SET_INSTALLABLE_ENTRY(207);
    SET_INSTALLABLE_ENTRY(208);
    SET_INSTALLABLE_ENTRY(209);
    SET_INSTALLABLE_ENTRY(210);
    SET_INSTALLABLE_ENTRY(211);
    SET_INSTALLABLE_ENTRY(212);
    SET_INSTALLABLE_ENTRY(213);
    SET_INSTALLABLE_ENTRY(214);
    SET_INSTALLABLE_ENTRY(215);
    SET_INSTALLABLE_ENTRY(216);
    SET_INSTALLABLE_ENTRY(217);
    SET_INSTALLABLE_ENTRY(218);
    SET_INSTALLABLE_ENTRY(219);
    SET_INSTALLABLE_ENTRY(220);
    SET_INSTALLABLE_ENTRY(221);
    SET_INSTALLABLE_ENTRY(222);
    SET_INSTALLABLE_ENTRY(223);
    SET_INSTALLABLE_ENTRY(224);
    SET_INSTALLABLE_ENTRY(225);
    SET_INSTALLABLE_ENTRY(226);
    SET_INSTALLABLE_ENTRY(227);
    SET_INSTALLABLE_ENTRY(228);
    SET_INSTALLABLE_ENTRY(229);
    SET_INSTALLABLE_ENTRY(230);
    SET_INSTALLABLE_ENTRY(231);
    SET_INSTALLABLE_ENTRY(232);
    SET_INSTALLABLE_ENTRY(233);
    SET_INSTALLABLE_ENTRY(234);
    SET_INSTALLABLE_ENTRY(235);
    SET_INSTALLABLE_ENTRY(236);
    SET_INSTALLABLE_ENTRY(237);
    SET_INSTALLABLE_ENTRY(238);
    SET_INSTALLABLE_ENTRY(239);
    SET_INSTALLABLE_ENTRY(240);
    SET_INSTALLABLE_ENTRY(241);
    SET_INSTALLABLE_ENTRY(242);
    SET_INSTALLABLE_ENTRY(243);
    SET_INSTALLABLE_ENTRY(244);
    SET_INSTALLABLE_ENTRY(245);
    SET_INSTALLABLE_ENTRY(246);
    SET_INSTALLABLE_ENTRY(247);
    SET_INSTALLABLE_ENTRY(248);
    SET_INSTALLABLE_ENTRY(249);
    SET_INSTALLABLE_ENTRY(250);
    SET_INSTALLABLE_ENTRY(251);
    SET_INSTALLABLE_ENTRY(252);
    SET_INSTALLABLE_ENTRY(253);
    SET_INSTALLABLE_ENTRY(254);
    SET_INSTALLABLE_ENTRY(255);
#undef SET_INSTALLABLE_ENTRY

    // Load the IDT
    idt_install();
}

void idt_install()
{
    __aligned(16) struct idtr _idtr = {
        .base = (intp)&_idt[0],
        .limit = (u16)(sizeof(_idt) - 1)
    };

    __asm__ volatile ("lidt %0\t" : : "m"(_idtr));
}

