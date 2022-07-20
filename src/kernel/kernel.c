// Based on code from https://wiki.osdev.org/Bare_Bones
#include "lai/helpers/pm.h"

#include "common.h"

#include "acpi.h"
#include "bootmem.h"
#include "cpu.h"
#include "efifb.h"
#include "hpet.h"
#include "interrupts.h"
#include "kalloc.h"
#include "multiboot2.h"
#include "paging.h"
#include "palloc.h"
#include "pci.h"
#include "serial.h"
#include "stdio.h"
#include "terminal.h"

volatile u32 blocking = 0;
u8 scancode;
extern u64 master_ticks;

__noreturn void kernel_panic(u32 error)
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

void kernel_main(struct multiboot_info*);

static void initialize_kernel(struct multiboot_info* multiboot_info)
{
    // create a terminal before any print calls are made -- they won't
    // show up on screen until a framebuffer is enabled, but they are buffered in memory until then
    // for now this is safe to call immediately, since no memory allocation happens in terminal_init()
    terminal_init();

    // initialize serial port 
    serial_init();

    fprintf(stderr, "Boot..kernel_main at 0x%lX\n", (intp)kernel_main);

    // parse multiboot right away
    multiboot2_parse(multiboot_info);

    // create bootmem storage
    bootmem_init();

    // create the frame buffer so we can actually show the user stuff
    efifb_init();

    // parse the ACPI tables. we need it to enable interrupts
    acpi_init();

    // immediately setup and enable interrupts
    interrupts_init();

    // take over from the bootmem allocator
    palloc_init();
    kalloc_init();

    // take over from the page table initialized at boot
    paging_init();

    // enable the kernel timer
    hpet_init();

    // finish ACPI initialization
    acpi_init_lai();

    // begin enumerating system devices
    pci_init();

    // TODO enable/use high memory in palloc only after paging is initialized
}

void kernel_main(struct multiboot_info* multiboot_info) 
{
    initialize_kernel(multiboot_info);

    fprintf(stderr, "kernel ready...\n");

    // testing loop
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
            } else if(scancode == 62) {
                fprintf(stderr, "calling lai_acpi_reset()\n");
                lai_api_error_t err = lai_acpi_reset();
                fprintf(stderr, "error = %s\n", lai_api_error_to_string(err));
                acpi_reset();
            } else if(scancode == 63) {
                fprintf(stderr, "calling lai_acpi_sleep(5)\n");
                lai_enter_sleep(5);
            } else if(scancode == 25) { // 'p'
                pci_dump_device_list();
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

