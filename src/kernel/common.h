#ifndef __COMMON_H__
#define __COMMON_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define null ((void*)0ULL)
#define stringify(x) #x
#define stringify2(x) stringify(x)

#define assert(cond,err) do { \
        if(!(cond)) {           \
            terminal_print_string("assertion failed at " __FILE__ ":" stringify2(__LINE__) ": "); terminal_print_string(err); \
            kernel_panic(COLOR(128, 128, 128)); \
        }                                       \
    } while(false);

#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __alignof(x, n) (((intp)(x)) & ((n)-1))
#define __alignup(x, n) (void*)(((intp)(x) + ((n)-1)) & ~((n)-1))
#define __aligndown(x, n) (void*)((intp)(x) & ~((n)-1))
#define __interrupt __attribute__((interrupt))
#define __popcnt(x) __builtin_popcountll(x)

#define max(a, b) (((a) >= (b)) ? (a) : (b))
#define min(a, b) (((a) <= (b)) ? (a) : (b))
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

#define zero(m)           do { for(u64 ___asadf431 = 0; ___asadf431 < sizeof(*m); ___asadf431++) ((u8*)(m))[___asadf431] = 0; } while(false)
#define memset(m, v, s)   do { for(u64 ___asadf431 = 0; ___asadf431 < s; ___asadf431++) ((u8*)(m))[___asadf431] = v; } while(false)
#define memset64(m, v, c) do { for(u64 ___asadf431 = 0; ___asadf431 < c; ___asadf431++) ((u64*)(m))[___asadf431] = v; } while(false)

#define lmask(n) ((1ULL << (n & 0x3F)) - (n == 64) - 1)

#endif
