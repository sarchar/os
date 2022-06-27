#ifndef __COMMON_H__
#define __COMMON_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define countof(a) (sizeof(a) / sizeof((a)[0]))
#define unused(x) ((void)(x))

#define PANIC()   kernel_panic()

typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;

typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;

#endif
