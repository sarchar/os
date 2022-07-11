// Based on code from https://wiki.osdev.org/Bare_Bones
#include "common.h"

#include "acpi.h"
#include "bootmem.h"
#include "cpu.h"
#include "efifb.h"
#include "interrupts.h"
#include "multiboot2.h"
#include "paging.h"
#include "palloc.h"
#include "stdio.h"
#include "terminal.h"

volatile u32 blocking = 0;
u8 scancode;
extern u64 master_ticks;

void kernel_panic(u32 error)
{
    // attempt to set the screen to all red
    //efifb_clear(COLOR(255,0,0));
    for(u32 y = 540; y < 620; y++) {
        for(u32 x = 840; x < 920; x++) {
            efifb_putpixel(x, y, (color)error);
        }
    }

    // loop forever
    while(1) { asm("hlt"); }
}

void kernel_main(struct multiboot_info* multiboot_info) 
{
    // create a terminal before any print calls are made -- they won't
    // show up on screen until a framebuffer is enabled, but they are buffered in memory until then
    terminal_init();
    fprintf(stderr, "Boot..kernel_main at 0x%lX\n", (intp)kernel_main);

    // immediately setup and enable interrupts
    interrupts_init();

    // parse multiboot
    multiboot2_parse(multiboot_info);
    acpi_init(); // TODO will need to happen before interrupts_init() but for now just testing acpi

    // after this, the bootmem allocator is no longer useful
    palloc_init();

#define TEST_PALLOC(n) { \
    void* p = palloc_claim(n); \
    fprintf(stderr, "palloc_claim(" #n ") = $%lX\n", (intp)p); \
    }

    //TEST_PALLOC(0);
    //TEST_PALLOC(0);
    //TEST_PALLOC(0);
    //TEST_PALLOC(0);
    //TEST_PALLOC(0);
    //TEST_PALLOC(0);
    //TEST_PALLOC(0);
    //TEST_PALLOC(0);
    //TEST_PALLOC(0);
    TEST_PALLOC(10);
    TEST_PALLOC(9);
    TEST_PALLOC(8);
    //TEST_PALLOC(7);
    void* p7 = palloc_claim(7);
    fprintf(stderr, "palloc_claim(7) = $%lX\n", (intp)p7);
    TEST_PALLOC(6);
    TEST_PALLOC(5);
    TEST_PALLOC(4);
    TEST_PALLOC(3);
    TEST_PALLOC(2);
    TEST_PALLOC(1);
    TEST_PALLOC(0);
    TEST_PALLOC(0);
    void* p0a = palloc_claim(0);
    fprintf(stderr, "palloc_claim(0) = $%lX\n", (intp)p0a);
    void* p0 = palloc_claim(0);
    fprintf(stderr, "palloc_claim(0) = $%lX\n", (intp)p0);
    //TEST_PALLOC(0);
    //TEST_PALLOC(0);
    palloc_abandon(p0, 0);
    palloc_abandon(p0a, 0);
    palloc_abandon(p7, 7);
    
    paging_init();

    // cause a page fault exception (testing the idt)

    u32 count = 0;
    while(1) {
        while(blocking > 0) {
            __cli();
            fprintf(stderr, "kb: %d, master_ticks = %llu\n", scancode, master_ticks);

            // F1 - page fault, F2 - gpf, F3 - division by 0
            if(scancode == 59) {
                *(u64 *)0x00007ffc00000000 = 1;    // page fault
            } else if(scancode == 60) {
                *(u32 *)0xf0fffefe00000000 = 1;  // gpf because address isn't canonical
            } else if(scancode == 61) {
                __asm__ volatile("div %0" : : "c"(0));  // division by 0 error
            }

            for(u32 y = 0; y < 16; y++) {
                for(u32 x = 0; x < 16; x++) {
                    efifb_putpixel(16+x+count*32, 600+y, COLOR(255, 255, 255));
                }
            }

            blocking--;
            count++;
            __sti();
        }
    }

    fprintf(stderr, "...exiting kernel code...\n");
}

