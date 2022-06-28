#ifndef __COMMON_H__
#define __COMMON_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __alignof(x, n) (((u64*)(x)) & ((n)-1))
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

#endif
