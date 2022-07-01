#ifndef __COMMON_H__
#define __COMMON_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define null ((void*)0ULL)

#define assert(cond,err) do { \
        if(!(cond)) {           \
            terminal_print_string("assertion failed! "); terminal_print_string(err); \
            kernel_panic(COLOR(128, 128, 128)); \
        }                                       \
    } while(false);

#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __alignof(x, n) (((intp)(x)) & ((n)-1))
#define __alignup(x, n) (void*)((__alignof(x, n) != 0) ? ((intp)(x) + ((n) - __alignof(x, n))) : (intp)(x))
#define __aligndown(x, n) (void*)((intp)(x) - __alignof(x, n))
#define __interrupt __attribute__((interrupt))

#define countof(a) (sizeof(a) / sizeof((a)[0]))
#define unused(x) ((void)(x))

#define PANIC(c)   kernel_panic(c)

typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;

typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;

typedef u64       intp; // always == sizeof(void*)
//TODO __compiletime_assert__(sizeof(intp) == sizeof(void*))

typedef u32 color;

#define COLOR(r,g,b) (color)(0x00000000 | ((r) << 16) | ((g) << 8) | (b))

#define zero(m)           do { for(u64 i = 0; i < sizeof(*m); i++) *((u8*)m+i) = 0; } while(false)
#define memset(m, v, s)   do { for(u64 i = 0; i < s; i++) *((u8*)m+i) = v; } while(false)
#define memset64(m, v, c) do { for(u64 i = 0; i < c; i++) *((u64*)m+i) = v; } while(false)

#define lmask(n) ((1ULL << (n & 0x3F)) - (n == 64) - 1)

#endif
