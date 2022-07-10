#include "common.h"

#include "apic.h"
#include "cpu.h"
#include "kernel.h"
#include "stdio.h"

#define IA32_APIC_BASE_MSR 0x1B
#define IA32_APIC_BASE_MSR_ENABLE 0x800

static intp _lapic_base;

static bool _has_lapic()
{
    u64 a, b, c, d;
    __cpuid(1, &a, &b, &c, &d);
    return (d & CPUID_FEAT_EDX_APIC) != 0;
}

intp _get_lapic_base() 
{
    //return (intp)__rdmsr(IA32_APIC_BASE_MSR);
    return (intp)0xFEE00000;
}

void _set_lapic_base(intp base) 
{
    __wrmsr(IA32_APIC_BASE_MSR, (base & 0xFFFFFFFFFFFF000ULL) | IA32_APIC_BASE_MSR_ENABLE);
}

static void _write_register(u16 reg, u32 value)
{
    *((u32 volatile*)(_lapic_base + reg)) = value;
}

static u32 _read_register(u16 reg)
{
    return *((u32 volatile*)(_lapic_base + reg));
}

void apic_init()
{
    assert(_has_lapic(), "only APIC-supported systems for now");

    _lapic_base = _get_lapic_base();

    fprintf(stderr, "apic: found LAPIC at 0x%lX\n", _lapic_base);

    _set_lapic_base(_lapic_base); // not sure if this is needed, but it's present on osdev wiki

    // hardcode enable IRQ 1 mapped to global interrupt 33
    u32 volatile* ioregsel = (u32 volatile*)0xFEC00000;
    u32 volatile* ioregwin = (u32 volatile*)0xFEC00010;
    // write to registers 0x12-13 the value (0 << 56) | (0 << 16) | (0 << 15) | (0 << 13) | (0 << 11) | (0 << 8) | (33 < 0)
    u64 value = (0ULL << 56) | (0 << 16) | (0 << 15) | (0 << 13) | (0 << 11) | (0 << 8) | (33 << 0);
    *ioregsel = 0x12;
    *ioregwin = (value & 0xFFFFFFFF);
    *ioregsel = 0x13;
    *ioregwin = (value >> 32);

    _write_register(0xF0, 0x10F); // enable LAPIC and set the spurious interrupt vector to 255
}

void _lapic_eoi()
{
    _write_register(0xB0, 0);
}

