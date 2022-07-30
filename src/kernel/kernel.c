// Based on code from https://wiki.osdev.org/Bare_Bones
#include "lai/helpers/pm.h"

#include "common.h"

#include "acpi.h"
#include "apic.h"
#include "bootmem.h"
#include "cpu.h"
#include "efifb.h"
#include "hpet.h"
#include "interrupts.h"
#include "kalloc.h"
#include "kernel.h"
#include "multiboot2.h"
#include "paging.h"
#include "palloc.h"
#include "pci.h"
#include "serial.h"
#include "stdio.h"
#include "terminal.h"

#include "drivers/ahci.h"
#include "drivers/ps2keyboard.h"

extern void _gdt_fixup(intp vma_base);
void kernel_main(struct multiboot_info*);

static u8 volatile exit_kernel = 0;

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
    __cli();           // disable interrupts before changing pages

    // gdt has to be fixed up to use _kernel_vma_base before switching the 
    // page table and interrupts over to highmem
    _gdt_fixup((intp)&_kernel_vma_base);

    paging_init();     // unmaps a large portion of lowmem

    // a few modules have to map new memory
    efifb_map();       // the EFI framebuffer needs virtual mapping
    terminal_redraw(); // remapping efifb may have missed some putpixel calls
    apic_map();        // the APIC needs memory mapping

    __sti();           // re-enable interrupts

    // enable the kernel timer
    hpet_init();

    // map PCI into virtual memory
    pci_init();

    // finish ACPI initialization
    acpi_init_lai();

    // enumerate system devices
    // in the future, this could happen after all drivers are "loaded",
    // and then as devices are discovered they can be mapped into their respective drivers
    // right now, drivers search for devices they're interested in
    pci_enumerate_devices();

    // TODO enable high memory in palloc after paging is initialized
}

static void load_drivers()
{
    ps2keyboard_load();
    ahci_load();
}

static void run_command(char* cmdbuffer)
{
    if(strcmp(cmdbuffer, "pf") == 0) {
        *(u64 *)0x00007ffc00000000 = 1;    // page fault
    } else if(strcmp(cmdbuffer, "div0") == 0) {
        __asm__ volatile("div %0" : : "c"(0));  // division by 0 error
    } else if(strcmp(cmdbuffer, "gpf") == 0) {
        *(u32 *)0xf0fffefe00000000 = 1;  // gpf because address isn't canonical
    } else if(strcmp(cmdbuffer, "reboot") == 0) {
        fprintf(stderr, "calling lai_acpi_reset()\n");
        lai_api_error_t err = lai_acpi_reset();
        // we shouldn't see this, but sometimes ACPI reset isn't supported
        fprintf(stderr, "error = %s\n", lai_api_error_to_string(err));
        acpi_reset();
    } else if(strcmp(cmdbuffer, "sleep") == 0) {
        fprintf(stderr, "calling lai_acpi_sleep(5)\n");
        lai_enter_sleep(5);
    } else if(strcmp(cmdbuffer, "pci") == 0) {
        pci_dump_device_list();
    } else if(strcmp(cmdbuffer, "ahci") == 0) {
        ahci_dump_registers();
    } else if(strcmp(cmdbuffer, "exit") == 0) {
        exit_kernel = 1;
    }
}

static void handle_keypress(char c, void* userdata)
{
    static char cmdbuffer[512];
    static u32 cmdlen = 0;

    unused(userdata);

    if(c == '\t') return;
    else if(c == '\n') {
        terminal_putc((u16)c);

        cmdbuffer[cmdlen] = 0;
        run_command(cmdbuffer);
        cmdlen = 0;

        fprintf(stderr, "> ");
    } else if(c == '\b') {
        // TODO
    } else {
        // save the final byte for \0
        if(cmdlen < 511) {
            cmdbuffer[cmdlen++] = c;
        }

        terminal_putc((u16)c);
    }
}

void kernel_main(struct multiboot_info* multiboot_info) 
{
    initialize_kernel(multiboot_info);
    load_drivers();

    ps2keyboard_hook_ascii(&handle_keypress, null);

    fprintf(stderr, "kernel ready...\n\n");
    fprintf(stderr, "> ");

    // update drivers forever (they should just use kernel tasks in the future)
    while(!exit_kernel) {
        ps2keyboard_update();
    }

    fprintf(stderr, "...exiting kernel code...\n");
}

