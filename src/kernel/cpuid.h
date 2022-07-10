#ifndef __CPUID_H__
#define __CPUID_H__

enum {
    CPUID_FEAT_EDX_APIC = (1 << 9)
};

static inline void __cpuid(u64 code, u64* eax, u64* ebx, u64* ecx, u64* edx)
{
    __asm__ ("cpuid" : "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx) : "0" (code));
}

#endif
