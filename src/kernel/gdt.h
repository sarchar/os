#ifndef __GDT_H__
#define __GDT_H__

struct gdt_tss {
    u32 reserved0;
    u64 rsp0;     // stack pointers for privilege levels 0..2
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist1;     // interrupt stack table pointers
    u64 ist2;
    u64 ist3;
    u64 ist4;
    u64 ist5;
    u64 ist6;
    u64 ist7;
    u64 reserved2;
    u16 reserved3;
    u16 io_map_base_address;
} __packed;


void gdt_init();
void gdt_install(u32);
void gdt_set_tss_rsp0(intp);

#endif
