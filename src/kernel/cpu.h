#ifndef __CPU_H__
#define __CPU_H__

#include "cpuid.h"

#define __cli()           asm volatile("cli")
#define __sti()           asm volatile("sti")
#define __barrier()       asm volatile("" : : : "memory")
#define __pause()         asm volatile("pause");
#define __pause_barrier() asm volatile("pause" : : : "memory");

enum MSRS {
    MSR_FS_BASE        = 0xC0000100,
    MSR_GS_BASE        = 0xC0000101,
    MSR_KERNEL_GS_BASE = 0xC0000102
};

// borrowed from https://wiki.osdev.org/Inline_Assembly/Examples#I.2FO_access
static inline void __outb(u16 port, u8 val)
{
    /* There's an outb %al, $imm8  encoding, for compile-time constant port numbers that fit in 8b.  (N constraint).
     * Wider immediate constants would be truncated at assemble-time (e.g. "i" constraint).
     * The  outb  %al, %dx  encoding is the only option for all other cases.
     * %1 expands to %dx because  port  is a uint16_t.  %w1 could be used if we had the port number a wider C type */
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port) );
}

static inline void __outw(u16 port, u16 val)
{
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port) );
}

static inline void __outl(u16 port, u32 val)
{
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port) );
}

static inline void __outq(u16 port, u64 val)
{
    asm volatile("outq %0, %1" : : "a"(val), "Nd"(port) );
}

static inline u8 __inb(u16 port)
{
    u8 ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static inline u16 __inw(u16 port)
{
    u16 ret;
    asm volatile("inw %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static inline u32 __inl(u16 port)
{
    u32 ret;
    asm volatile("inl %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static inline u64 __inq(u16 port)
{
    u64 ret;
    asm volatile("inq %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static inline void __io_wait(void)
{
    __outb(0x80, 0);
}

static inline u64 __rdmsr(u32 msr)
{
    u64 edx;
    u64 eax;
    asm volatile("rdmsr" : "=a"(eax), "=d"(edx) : "c" (msr) );
    return eax | (edx << 32);
}

static inline void __wrmsr(u64 msr, u64 value)
{
    u32 low = value & 0xFFFFFFFF;
    u32 high = value >> 32;
    asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline u64 __rdcr2()
{
    u64 ret;
    asm volatile("mov %%cr2, %0" : "=a"(ret) );
    return ret;
}

static inline void __wrcr3(u64 val)
{
    asm volatile("mov %0, %%cr3" : : "a"(val) );
}

static inline void __invlpg(intp addr) 
{
    asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

// GSBase
//
struct cpu {
    struct cpu* this;
    u32 cpu_index;

    // struct task* current_task;
    // struct process* current_user_process;
    // etc
};

static inline intp __get_cpu()
{
    intp v;
    asm volatile("movq %%gs:0, %0" : "=c"(v));
    return v;
}

static inline void __set_cpu(intp addr)
{
    __wrmsr(MSR_KERNEL_GS_BASE, (u64)addr);
}

static inline void __swapgs()
{
    asm volatile("swapgs");
}

#define get_cpu()    ((struct cpu*)__get_cpu())
#define set_cpu(cpu) (__set_cpu((intp)(cpu)))

// synchronization primitives
//
// from http://locklessinc.com/articles/locks/
#define __atomic_xadd(p, v) __sync_fetch_and_add((p), (v))
#define __atomic_add(p, v) __sync_add_and_fetch((p), (v))

#define __atomic_inc(p) __atomic_add((p), 1)
#define __atomic_dec(p) __atomic_add((p), -1)
#define __atomic_set_bit(p, v) __sync_or_and_fetch((p), 1<<(v))
#define __atomic_clear_bit(p, v) __sync_and_and_fetch((p), ~(1<<(v)))

#define __compare_and_exchange(p, o, n) __sync_val_compare_and_swap((p), (o), (n))

// exchange value at ptr with x, returninng the old value
static inline u64 __xchgq(u64* ptr, u64 x)
{
    asm volatile("xchgq %0, %1" : "=r"(x) : "m"(*(u64*)ptr), "0" ((u64)x) :"memory");
    return x;
}

static inline u32 __xchgl(u32* ptr, u32 x)
{
    asm volatile("xchgl %0, %1" : "=r"(x) : "m"(*(u32*)ptr), "0" ((u32)x) :"memory");
    return x;
}

static inline u16 __xchgw(u16* ptr, u16 x)
{
    asm volatile("xchgw %0, %1" : "=r"(x) : "m"(*(u16*)ptr), "0" ((u16)x) :"memory");
    return x;
}

static inline u8 __xchgb(u8* ptr, u8 x)
{
    asm volatile("xchgb %0, %1" : "=r"(x) : "m"(*(u8*)ptr), "0" ((u8)x) :"memory");
    return x;
}

// test and set bit
static inline u64 __atomic_btsq(u64 volatile* ptr, u16 x)
{
    u64 out;
    asm volatile("lock; bts %2, %1\n"
                 "\tsbb %0, %0\n"
                 : "=r"(out), "=m"(*ptr) : "Ir"(x) : "memory");
    return out;
}

#endif
