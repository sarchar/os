#include "common.h"

#include "apic.h"
#include "cpu.h"
#include "kernel.h"
#include "paging.h"
#include "stdio.h"

#define IA32_APIC_BASE_MSR 0x1B
#define IA32_APIC_BASE_MSR_ENABLE 0x800

#define IO_APIC_ID_REG             0x00
#define IO_APIC_VERSION_REG        0x01
#define IO_APIC_ARBITRATION_REG    0x02
#define IO_APIC_REDIERCTION_REG(n) (0x10 + (n << 1))

#define LAPIC_EOI_REG              0xB0   // end of interrupt register
#define LAPIC_SIV_REG              0xF0   // spurious interrupt vector register

static struct {
    intp base;

    struct {
        u8   acpi_processor_id;
        u8   acpi_id;
        u8   padding0;
        bool enabled;
        u32  padding1;
    } cpu;
} local_apic = { .base = (intp)-1 };

static struct {
    u8   apic_id;
    u8   global_system_interrupt_base;
    u8   version;
    u8   num_interrupts;
    u32  reserved;
    intp base;
} io_apic = { .base = (intp)-1 };

static bool _has_lapic()
{
    //u64 a, b, c, d;
    //__cpuid(1, &a, &b, &c, &d);
    //return (d & CPUID_FEAT_EDX_APIC) != 0;
    return local_apic.base != (intp)-1;
}

void _set_lapic_base(intp base) 
{
    local_apic.base = base;
    __wrmsr(IA32_APIC_BASE_MSR, (base & 0xFFFFFFFFFFFF000ULL) | IA32_APIC_BASE_MSR_ENABLE);
}

static void _write_lapic(u16 reg, u32 value)
{
    *(u32 volatile*)(local_apic.base + reg) = value;
}

static void _write_io_apic(u8 reg, u32 value)
{
    // write register number to IOAPICBASE
    *(u32 volatile*)(io_apic.base + 0) = (u32)reg;
    // write data register
    *(u32 volatile*)(io_apic.base + 0x10) = value;
}

static u32 _read_io_apic(u8 reg)
{
    // write register number to IOAPICBASE
    *(u32 volatile*)(io_apic.base + 0) = (u32)reg;
    // read data register
    return *(u32 volatile*)(io_apic.base + 0x10);
}

static u32 _read_lapic(u16 reg)
{
    return *((u32 volatile*)(local_apic.base + reg));
}

static void _initialize_lapic()
{
    assert(_has_lapic(), "only APIC-supported systems for now");

    //_set_lapic_base(local_apic.base); // not sure if this is needed, but it's present on osdev wiki
    _write_lapic(LAPIC_SIV_REG, 0x1FF); // enable LAPIC and set the spurious interrupt vector to 255

    fprintf(stderr, "apic: LAPIC enabled (local_apic.base=0x%08lX)\n", local_apic.base);
}

static void _initialize_ioapic()
{
}

// https://blog.wesleyac.com/posts/ioapic-interrupts describes the process for getting that first
// keyboard interrupt via the APIC
void apic_init()
{
    _initialize_lapic();
    _initialize_ioapic();

    // enable keyboard interrupt (IRQ1 == global system interrupt 1) map to cpu irq 33
    apic_set_io_apic_redirection(1, 33,
                             IO_APIC_REDIRECTION_FLAG_DELIVERY_NORMAL,
                             IO_APIC_REDIRECTION_DESTINATION_PHYSICAL,
                             IO_APIC_REDIRECTION_ACTIVE_HIGH,
                             IO_APIC_REDIRECTION_EDGE_SENSITIVE,
                             true,
                             local_apic.cpu.acpi_id);

    // HPET currently configured to trigger global system interrupt 19
    // map it to 80 in the IDT
    apic_set_io_apic_redirection(19, 80,
                             IO_APIC_REDIRECTION_FLAG_DELIVERY_NORMAL,
                             IO_APIC_REDIRECTION_DESTINATION_PHYSICAL,
                             IO_APIC_REDIRECTION_ACTIVE_HIGH,
                             IO_APIC_REDIRECTION_EDGE_SENSITIVE,
                             false, // don't enable interrupts yet
                             local_apic.cpu.acpi_id);

    apic_io_apic_enable_interrupt(19); // enable the interrupt in the redirection register
}

void apic_map()
{
    paging_map_page(io_apic.base   , io_apic.base   , MAP_PAGE_FLAG_WRITABLE | MAP_PAGE_FLAG_DISABLE_CACHE);
    paging_map_page(local_apic.base, local_apic.base, MAP_PAGE_FLAG_WRITABLE | MAP_PAGE_FLAG_DISABLE_CACHE);
}

// See https://wiki.osdev.org/APIC#IO_APIC_Configuration
void apic_set_io_apic_redirection(u8 io_apic_irq, u8 cpu_irq, u8 delivery_mode, u8 destination_mode, u8 active_level, u8 trigger_mode, bool enabled, u8 destination)
{
    u64 cntrl = ((u64)destination << 56) | ((u64)trigger_mode << 15) | ((u64)active_level << 13) | ((u64)destination_mode << 11) | ((u64)delivery_mode << 8) | (u64)cpu_irq;

    // set IRQ enabled bit if requested
    if(!enabled) {
        cntrl |= 1 << 16;
    }

    // TODO determine which I/O APIC is responsible for the io_apic_irq

    // write 64-bit value to IO_APIC_REDIERCTION_REG corresponding to io_apic_irq
    _write_io_apic(IO_APIC_REDIERCTION_REG(io_apic_irq) + 0, (cntrl & 0xFFFFFFFF)); 
    _write_io_apic(IO_APIC_REDIERCTION_REG(io_apic_irq) + 1, cntrl >> 32);
}

void apic_io_apic_enable_interrupt(u8 io_apic_irq)
{
    u32 low = _read_io_apic(IO_APIC_REDIERCTION_REG(io_apic_irq));
    low &= ~(1 << 16);
    _write_io_apic(IO_APIC_REDIERCTION_REG(io_apic_irq), low);
}

void apic_io_apic_disable_interrupt(u8 io_apic_irq)
{
    u32 low = _read_io_apic(IO_APIC_REDIERCTION_REG(io_apic_irq));
    low |= 1 << 16;
    _write_io_apic(IO_APIC_REDIERCTION_REG(io_apic_irq), low);
}

void apic_notify_acpi_io_apic(u8 io_apic_id, intp io_apic_base, u8 global_system_interrupt_base)
{
    assert(io_apic.base == (intp)-1, "don't notify two I/O APICs"); // TODO?
    io_apic.apic_id = io_apic_id;
    io_apic.base    = io_apic_base;
    io_apic.global_system_interrupt_base = global_system_interrupt_base;

    u32 c = _read_io_apic(IO_APIC_VERSION_REG);
    io_apic.version = c & 0xFF;
    io_apic.num_interrupts = (c >> 16) & 0xFF;

    fprintf(stderr, "apic: I/O APIC id=%d version=0x%X handles interrupts %d..%d\n", io_apic.apic_id, io_apic.version, 
            io_apic.global_system_interrupt_base, io_apic.global_system_interrupt_base+io_apic.num_interrupts);
}

void apic_notify_acpi_io_apic_interrupt_source_override(u8 bus_source, u8 irq_source, u8 global_system_interrupt, u8 flags)
{
    // TODO will one day need to use these overrides
    unused(flags); // TODO - edge triggered, high/low trigger, etc
    unused(bus_source);
    unused(irq_source);
    unused(global_system_interrupt);
}

void apic_notify_acpi_local_apic(intp lapic_base, bool system_has_pic)
{
    unused(system_has_pic);
    local_apic.base = lapic_base;
}

void apic_register_processor_lapic(u8 acpi_processor_id, u8 acpi_id, bool enabled)
{
    // TODO support more processors, for now just use processor id 0
    if(acpi_processor_id != 0) return;
    assert(enabled, "proc 0 not enabled?");
        
    local_apic.cpu.acpi_processor_id = acpi_processor_id;
    local_apic.cpu.acpi_id = acpi_id;
    local_apic.cpu.enabled = enabled;
}

void apic_notify_acpi_lapic_nmis(u8 acpi_processor_id, u8 lint_number, u8 flags)
{
    // TODO register the lint_number in the CPU's local vector table
    // and then map that local interrupt into a cpu interrupt. 
    // See https://wiki.osdev.org/APIC#Local_Vector_Table_Registers
    // It's similar to the I/O APIC redirection table
    // Flags seems to contain edge/level triggered, etc., information
    unused(acpi_processor_id);
    unused(lint_number);
    unused(flags);
}

void _send_lapic_eoi()
{
    _write_lapic(LAPIC_EOI_REG, 0);
}

