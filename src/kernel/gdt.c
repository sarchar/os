#include "common.h"

#include "apic.h"
#include "cpu.h"
#include "gdt.h"
#include "paging.h"
#include "palloc.h"
#include "stdio.h"

#define ONE_GDT_SIZE  (sizeof(struct gdt_entry) * 7)
#define ALL_GDTs_SIZE ((u64)__alignup((intp)(ONE_GDT_SIZE * ncpus), 8)) // 8-byte align the TSSs
#define ONE_TSS_SIZE  ((u64)__alignup((intp)sizeof(struct gdt_tss), 128)) // align to 128 bytes per struct
#define ALL_TSSs_SIZE (ONE_TSS_SIZE * ncpus)

struct gdt_entry {
    u16 limit0_to_15;
    u16 base0_to_15;
    u8  base16_to_23;
    union {
        u8  access;
        struct {
            u8 accessed    : 1;
            u8 rw          : 1;
            u8 dc          : 1;
            u8 exec        : 1;
            u8 dtype       : 1; // descriptor type. 1 = code or data segment
            u8 dpl         : 2;
            u8 present     : 1;
        } __packed access_cd; // cd = code or data
        struct {
            u8 stype       : 4;
            u8 dtype       : 1; // descriptor type. 0 = system segment
            u8 dpl         : 2;
            u8 present     : 1;
        } __packed access_sys;
    };
    union {
        u8  flags_limit16_to_19;
        struct {
            u8 limit16_to_19 : 4;
            u8 reserved0     : 1;
            u8 long_mode     : 1; // 1 = long mode code segment (size flag should be 0), 0 = all other types of segments
            u8 size          : 1; // 0 = 16-bit segment, 1 = 32-bit segment
            u8 granularity   : 1; // 0 = byte, 1 = 4KiB page
        } __packed;
    } __packed flags;
    u8  base24_to_31;
} __packed;

struct gdt_long_mode_entry {
    struct gdt_entry low;
    u32    base32_to_63;
    u32    reserved0;
} __packed;

static_assert(sizeof(struct gdt_entry) == 8, "GDT entry must be defined to be 8 bytes");
static_assert(sizeof(struct gdt_tss) == 104, "TSS must be 104 bytes in size");

static intp _gdt;

// set the same code/data segment on all GDTs
// limit and base aren't used in long mode and should be set to 0 and 0xFFFFFFFF, respectively
static void gdt_set_entry_cd(u8 i, u32 base, u32 limit, bool is_code, bool is_user)
{
    // we'll always use 4K granularity
    limit >>= 12;

    u32 ncpus = apic_num_local_apics();
    for(u32 cpu_index = 0; cpu_index < ncpus; cpu_index++) {
        struct gdt_entry* entry = &((struct gdt_entry*)((u8*)_gdt + ONE_GDT_SIZE * cpu_index))[i];

        // set the base
        entry->base0_to_15   = base & 0x0000FFFF;
        entry->base16_to_23  = (base & 0x00FF0000) >> 16;
        entry->base24_to_31  = (base & 0xFF000000) >> 24;

        // set the size size
        entry->limit0_to_15        = limit & 0x0000FFFF;
        entry->flags.limit16_to_19 = (limit & 0x000F0000) >> 16;

        // set the access and flags bits
        entry->access_cd.dtype       = 1; // not a system segment
        entry->access_cd.present     = 1;
        entry->access_cd.dpl         = is_user ? 3 : 0; // privilege level
        entry->access_cd.exec        = is_code ? 1 : 0;
        entry->access_cd.rw          = 1; // for code segments, readable; for data segments, writeable.
        entry->flags.long_mode       = is_code ? 1 : 0; // long mode is only enabled on code segments
        entry->flags.size            = is_code ? 0 : 1; // for long mode segments, size flag should be 0. otherwise 1 for 32-bit data segments
        entry->flags.granularity     = 1; // 4KiB page granularity
    }
}

// limit and base aren't used in long mode and should be set to 0 and 0xFFFFFFFF, respectively
static void gdt_set_entry_tss(u32 cpu_index, u8 i, struct gdt_tss* tss)
{
    struct gdt_entry* entry = &((struct gdt_entry*)((u8*)_gdt + ONE_GDT_SIZE * cpu_index))[i];
    struct gdt_long_mode_entry* long_entry = (struct gdt_long_mode_entry*)entry;

    // set the base
    intp base = (intp)tss;
    entry->base0_to_15   = base & 0x0000FFFF;
    entry->base16_to_23  = (base & 0x00FF0000) >> 16;
    entry->base24_to_31  = (base & 0xFF000000) >> 24;

    // set the size size
    u64 limit = sizeof(struct gdt_tss); // using byte granularity
    entry->limit0_to_15        = limit & 0x0000FFFF;
    entry->flags.limit16_to_19 = (limit & 0x000F0000) >> 16;

    // set the access and flags bits
    entry->access_sys.dtype       = 0; // system segment
    entry->access_sys.stype       = 9; // 9 = 32/64-bit TSS (available)
    entry->access_sys.present     = 1;
    entry->access_sys.dpl         = 0; // privilege level
    entry->flags.size             = 1; // 32-bit segment (actually, 64...)
    entry->flags.granularity      = 0; // byte granularity

    // the long bits
    long_entry->base32_to_63 = (base & 0xFFFFFFFF00000000ULL) >> 32; 
}

// gdt_init is only called once on the bootstrap processor
void gdt_init()
{
    // our gdt will be:
    //   1 null segment
    //   1 kernel code segment
    //   1 kernel data segment
    //   1 user code segment
    //   1 user data segment
    //   1 system TSS segment (a 64-bit TSS occupies 2 gdt entries!)
    // for a total of 7 entries. at 8 bytes each, that's 56 bytes per GDT.
    // plus one TSS structure.
    // and we will have one of each for each CPU. 
    u32 ncpus = apic_num_local_apics();
    u64 required_space = ALL_GDTs_SIZE + ALL_TSSs_SIZE;
    u8  palloc_order = next_power_of_2((required_space + PAGE_SIZE - 1) / PAGE_SIZE); // round up to # of pages
    //fprintf(stderr, "gdt: required_space=%d need 2^%d=%d pages for GDTs\n", required_space, palloc_order, (1ULL << palloc_order));

    _gdt = palloc_claim(palloc_order);
    memset64(_gdt, 0, (1ULL << (12 + palloc_order - 3))); // this sets the null descriptor

    gdt_set_entry_cd(1, 0x00000000, 0xFFFFFFFF, true, false); // kernel code
    gdt_set_entry_cd(2, 0x00000000, 0xFFFFFFFF, false, false); // kernel data
    gdt_set_entry_cd(3, 0x00000000, 0xFFFFFFFF, true, true); // user code
    gdt_set_entry_cd(4, 0x00000000, 0xFFFFFFFF, false, true); // user data

    for(u32 cpu_index = 0; cpu_index < ncpus; cpu_index++) {
        struct gdt_tss* tss = (struct gdt_tss*)(_gdt + ALL_GDTs_SIZE + cpu_index * ONE_TSS_SIZE);
        zero(tss);
        gdt_set_entry_tss(cpu_index, 5, tss);
    }
}

void gdt_install(u32 cpu_index)
{
    // descriptor used to tell the cpu about our GDT.
    __aligned(16) struct {
        u16    limit;
        u64    base;
    } __packed _gdtr = {
        .base = (u64)(_gdt + cpu_index * ONE_GDT_SIZE),
        .limit = (u16)(ONE_GDT_SIZE - 1),
    };

    asm volatile ("lgdt %0\n" : : "m"(_gdtr));
//    asm volatile ("lgdt %0\n"
//                  "jmp 0x08:.reload_cs\n"
//                  ".reload_cs:\n" : : "m"(_gdtr));
}

void gdt_set_tss_rsp0(intp rsp)
{
    u32 ncpus = apic_num_local_apics(); // needed for ALL_GDTs_SIZE

    // get pointer to TSS
    struct cpu* cpu = get_cpu();
    struct gdt_tss* tss = (struct gdt_tss*)(_gdt + ALL_GDTs_SIZE + cpu->cpu_index * ONE_TSS_SIZE);

    // set rsp0
    tss->rsp0 = rsp;

    // reload the task segment register and set the segment registers to user data
    // the user data segment is fine for kernel use
    asm volatile ("mov $(5*8), %%ax\n"
                  "\tltr %%ax\n"
                  "\tmov $(4*8), %%ax\n"
                  "\tmov %%ax, %%ds\n"
                  "\tmov %%ax, %%es\n"
                  "\tmov %%ax, %%fs\n" : : : "ax");
}

