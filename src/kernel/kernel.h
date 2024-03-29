#ifndef __KERNEL_H__
#define __KERNEL_H__

/* Check if the compiler thinks you are targeting the wrong operating system. */
#if defined(__linux__)
#error "You are not using a cross-compiler, you will most certainly run into trouble"
#endif
 
/* This tutorial will only work for the 32-bit ix86 targets. */
#if !defined(__x86_64__)
#error "This tutorial needs to be compiled with a ix86-elf compiler"
#endif

extern u64 _kernel_vma_base;
extern u64 _kernel_load_address;
extern u64 _kernel_end_address;
extern u64 _stack_bottom;
extern u64 _stack_top;
extern u64 _userland_text_start;
extern u64 _userland_text_end;
extern u64 _userland_data_start;
extern u64 _userland_data_end;

extern u64 volatile global_ticks;

#define PANIC(c)   kernel_panic(c)
__noreturn void kernel_panic(u32 error); // hack to not include the color type

__noreturn void kernel_do_work();

#endif
