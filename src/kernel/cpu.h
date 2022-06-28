#ifndef __CPU_H__
#define __CPU_H__

#define __cli() __asm__ volatile("cli")
#define __sti() __asm__ volatile("sti")

// borrowed from https://wiki.osdev.org/Inline_Assembly/Examples#I.2FO_access
static inline void __outb(u16 port, u8 val)
{
    /* There's an outb %al, $imm8  encoding, for compile-time constant port numbers that fit in 8b.  (N constraint).
     * Wider immediate constants would be truncated at assemble-time (e.g. "i" constraint).
     * The  outb  %al, %dx  encoding is the only option for all other cases.
     * %1 expands to %dx because  port  is a uint16_t.  %w1 could be used if we had the port number a wider C type */
    asm volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}

static inline void __outw(u16 port, u16 val)
{
    asm volatile ( "outw %0, %1" : : "a"(val), "Nd"(port) );
}

static inline void __outl(u16 port, u32 val)
{
    asm volatile ( "outl %0, %1" : : "a"(val), "Nd"(port) );
}

static inline void __outq(u16 port, u64 val)
{
    asm volatile ( "outq %0, %1" : : "a"(val), "Nd"(port) );
}

static inline u8 __inb(u16 port)
{
    u8 ret;
    asm volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static inline u16 __inw(u16 port)
{
    u16 ret;
    asm volatile ( "inw %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static inline u32 __inl(u16 port)
{
    u32 ret;
    asm volatile ( "inl %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static inline u64 __inq(u16 port)
{
    u64 ret;
    asm volatile ( "inq %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static inline void __io_wait(void)
{
    __outb(0x80, 0);
}

#endif
