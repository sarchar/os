#ifndef __COMMON_H__
#define __COMMON_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define null ((void*)0ULL)
#define stringify(x) #x
#define stringify2(x) stringify(x)

            //fprintf(stderr, "assertion failed at " __FILE__ ":" stringify2(__LINE__) ": %s\n", err); 
extern char const* assert_error_message; // might want this once per cpu more
#define __assert_int(cond,err) do { \
        if(!(cond)) {           \
            assert_error_message = err;         \
            kernel_panic(COLOR(128, 128, 128)); \
        }                                       \
    } while(false);
#define __assert(cond, err) \
    __assert_int(cond, "assertion failed at " __FILE__ ":" stringify2(__LINE__) ": " err "\n")
#define assert(cond,err) __assert(cond,err)

#define static_assert(cond,err) _Static_assert(cond, err)
#define __always_inline __attribute__((always_inline)) inline
#define __noreturn __attribute__((noreturn))

#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __alignof(x, n) (((intp)(x)) & ((n)-1))
#define __alignup(x, n) (void*)(((intp)(x) + ((n)-1)) & ~((n)-1))
#define __aligndown(x, n) (void*)((intp)(x) & ~((n)-1))
#define __interrupt __attribute__((interrupt))
#define __popcnt(x) __builtin_popcountll(x)
#define __bswap16(x) __builtin_bswap16(x)
#define __bswap32(x) __builtin_bswap32(x)

#define max(a, b) (((a) >= (b)) ? (a) : (b))
#define min(a, b) (((a) <= (b)) ? (a) : (b))
#define countof(a) (sizeof(a) / sizeof((a)[0]))
#define unused(x) ((void)(x))
#define containerof(v,T,e) (T*)((intp)(v) - offsetof(T, e))

typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;

typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;

typedef u64       intp; // always == sizeof(void*)
static_assert(sizeof(intp) == sizeof(void*), "size of pointer must match size of intp");

typedef u32 color;

#define COLOR(r,g,b) (color)(0x00000000 | ((r) << 16) | ((g) << 8) | (b))

#define zero(m)           do { for(u64 ___asadf431 = 0; ___asadf431 < sizeof(*m); ___asadf431++) ((u8*)(m))[___asadf431] = 0; } while(false)
#define memset64(m, v, c) do { for(u64 ___asadf431 = 0; ___asadf431 < (c); ___asadf431++) ((u64*)(m))[___asadf431] = (v); } while(false)

#define lmask(n) ((1ULL << (n & 0x3F)) - (n == 64) - 1)

#define is_power_of_2(x) (((x) & ((x) - 1)) == 0)

// return n such that 2^n >= x
// not valid for x=0 or 1
#define next_power_of_2(x) (is_power_of_2(x) ? (63 - __builtin_clzll((u64)(x))) : (64 - __builtin_clzll((u64)(x)))) 
// # of bytes between the next power of two and given x
#define til_next_power_of_2(x) ((1<<next_power_of_2(x))-(x))

// to use wait_bit_set you need to include hpet.h
// cnd must be a volatile boolean check
// timeout is in microseconds
// tmp needs to be any u64 
//
// use like:
// u64 volatile* mem;
// u64 tmp;
// wait_until_true(mem & SOME_BIT, 1000000, tmp) { 
//      timeout_condition();
// } else {
//      condition_is_true();
// }
#define _wutd(start)  /* wait until timer delta */ \
        (hpet_kernel_timer_delta_to_us((start), hpet_get_kernel_timer_value()))

#define wait_until_true(cnd, timeout, tmp)                  \
        (tmp) = hpet_get_kernel_timer_value();              \
        while(!(cnd) && _wutd(tmp) < timeout) __pause();    \
        if(_wutd(tmp) >= timeout)

#define wait_until_false(cnd, timeout, tmp)                 \
        (tmp) = hpet_get_kernel_timer_value();              \
        while((cnd) && _wutd(tmp) < timeout) __pause();     \
        if(_wutd(tmp) >= timeout)


// with the above functions we can hack in a simple usleep function
#define usleep(us) do { u64 tmp; wait_until_false(true, us, tmp) {}; } while(0)
#define mleep(msecs) do { usleep(secs*1000); } while(0)
#define sleep(secs) do { usleep(secs*1000000); } while(0)

// high res timer macros
#define timer_now() hpet_get_kernel_timer_value()
// time in microseconds since 's' was sampled
#define timer_since(s) hpet_kernel_timer_delta_to_us(s, hpet_get_kernel_timer_value())

#endif
