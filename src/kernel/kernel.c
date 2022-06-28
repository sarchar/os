// Based on code from https://wiki.osdev.org/Bare_Bones
#include "common.h"

#include "cpu.h"
#include "efifb.h"
#include "interrupts.h"
#include "multiboot2.h"
#include "terminal.h"

/* Check if the compiler thinks you are targeting the wrong operating system. */
#if defined(__linux__)
#error "You are not using a cross-compiler, you will most certainly run into trouble"
#endif
 
/* This tutorial will only work for the 32-bit ix86 targets. */
#if !defined(__x86_64__)
#error "This tutorial needs to be compiled with a ix86-elf compiler"
#endif

volatile u32 blocking = 0;
u8 scancode;
 
struct multiboot_info
{
	multiboot_uint32_t total_size;
	multiboot_uint32_t reserved;
};

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

void kernel_main(struct multiboot_info* multiboot_info_ptr) 
{
    struct multiboot_tag_load_base_addr* mbt_load_base_addr;
    struct multiboot_tag_string*         mbt_string;
    struct multiboot_tag_mmap*           mbt_mmap;
    struct multiboot_mmap_entry*         mbt_mmap_entry;
    struct multiboot_tag_framebuffer*    mbt_framebuffer;

    // immediately setup and enable interrupts
    interrupts_init();

    // now create a terminal before any print calls are made -- they won't
    // show up on screen until a framebuffer is enabled, but they are buffered in memory until then
    terminal_init();

    terminal_print_string("Boot\n");
    terminal_print_string("Multiboot info ptr: $"); terminal_print_pointer(multiboot_info_ptr);
	terminal_print_string("\nMultiboot total size: $"); terminal_print_u32(multiboot_info_ptr->total_size);

    // loop over all the tags in the structure up to total_size, which includes the info structure size
    multiboot_info_ptr->total_size -= 8;

    struct multiboot_tag* mbt = (struct multiboot_tag*)((void*)multiboot_info_ptr + sizeof(struct multiboot_info));
    while(multiboot_info_ptr->total_size != 0) {
        switch(mbt->type) {
        case MULTIBOOT_TAG_TYPE_CMDLINE:
            mbt_string = (struct multiboot_tag_string*)mbt;
            terminal_print_string("\nMBT Command line: ");
            terminal_print_string(mbt_string->string);
            break;

        case MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME:
            mbt_string = (struct multiboot_tag_string*)mbt;
            terminal_print_string("\nMBT Boot loader name: ");
            terminal_print_string(mbt_string->string);
            break;

        case MULTIBOOT_TAG_TYPE_MMAP:
            mbt_mmap = (struct multiboot_tag_mmap*)mbt;
            mbt_mmap_entry = mbt_mmap->entries;

            for(uint32_t size = mbt_mmap->size - sizeof(struct multiboot_tag_mmap); size != 0;) {
                terminal_print_string("\nMBT Memory map entry: $");
                terminal_print_pointer((void*)mbt_mmap_entry->addr);
                terminal_print_string(" length $");
                terminal_print_u64(mbt_mmap_entry->len);
                terminal_print_string(" type 0x");
                terminal_print_u8(mbt_mmap_entry->type);
                
                // next entry
                mbt_mmap_entry = (struct multiboot_mmap_entry*)((void *)mbt_mmap_entry + mbt_mmap->entry_size);
                size -= mbt_mmap->entry_size;

                // align-up the mbt pointer to 8 byte boundaries
                if(((uint64_t)mbt_mmap_entry & 0x07) != 0) {
                    int alignment = 8 - ((uint64_t)mbt_mmap_entry & 0x07);
                    mbt_mmap_entry = (struct multiboot_mmap_entry*)((void *)mbt_mmap_entry + alignment);
                    size -= alignment;
                }
            }
            break;

        case MULTIBOOT_TAG_TYPE_LOAD_BASE_ADDR:
            mbt_load_base_addr = (struct multiboot_tag_load_base_addr*)mbt;
            terminal_print_string("\nMBT Base load address: $");
            terminal_print_u32(mbt_load_base_addr->load_base_addr);
            break;

        case MULTIBOOT_TAG_TYPE_FRAMEBUFFER:
            mbt_framebuffer = (struct multiboot_tag_framebuffer*)mbt;
            terminal_print_string("\nMBT Framebuffer: address $");
            terminal_print_pointer((void*)mbt_framebuffer->common.framebuffer_addr);
            terminal_print_string(" pitch $");
            terminal_print_u32(mbt_framebuffer->common.framebuffer_pitch);
            terminal_print_string(" width $");
            terminal_print_u32(mbt_framebuffer->common.framebuffer_width);
            terminal_print_string(" height $");
            terminal_print_u32(mbt_framebuffer->common.framebuffer_height);
            terminal_print_string(" bpp $");
            terminal_print_u8(mbt_framebuffer->common.framebuffer_bpp);
            terminal_print_string(" type $");
            terminal_print_u8(mbt_framebuffer->common.framebuffer_bpp);

            if(!efifb_init((u32*)mbt_framebuffer->common.framebuffer_addr,
                           mbt_framebuffer->common.framebuffer_width,
                           mbt_framebuffer->common.framebuffer_height,
                           mbt_framebuffer->common.framebuffer_bpp,
                           mbt_framebuffer->common.framebuffer_pitch)) {
                PANIC(COLOR(0, 0, 0)); // color won't mater here
            }

            // set screen blue to show that we have initialized the frame buffer
            efifb_clear(COLOR(0, 0, 255));
            // wait for some arbitrary amount of time so that the blue screen is visible
            for(int i = 0; i < 3000000; i++) asm volatile("pause");
            // and clear it to black before proceeding
            efifb_clear(COLOR(0, 0, 0));

            // now that there's a working framebuffer, refresh the terminal to it
            terminal_redraw();

            // draw a 8x8 square in the corner to indicate that we made it this far
            for(u32 y = 0; y < 8; y++) {
                for(u32 x = 0; x < 8; x++) {
                    efifb_putpixel(x + mbt_framebuffer->common.framebuffer_width - 16,
                                   y + mbt_framebuffer->common.framebuffer_height - 16,
                                   COLOR(0, 255, 0));
                }
            }

            break;

        case MULTIBOOT_TAG_TYPE_EFI_MMAP:
            {
                struct multiboot_tag_efi_mmap* mbt_efi_mmap = (struct multiboot_tag_efi_mmap*)mbt;
                terminal_print_string("\nMBT EFI mmap: descriptor size ");
                terminal_print_u32(mbt_efi_mmap->descr_size);
            }
            break;

        case MULTIBOOT_TAG_TYPE_ACPI_NEW:
            terminal_print_string("\nGot tag ACPI_NEW");
            break;

        case MULTIBOOT_TAG_TYPE_EFI64:
            // EFI system table is still passed even though ExitBootServices was called?
            break;

        case MULTIBOOT_TAG_TYPE_ACPI_OLD:
            break;

        case MULTIBOOT_TAG_TYPE_APM:
        case MULTIBOOT_TAG_TYPE_ELF_SECTIONS:
        case MULTIBOOT_TAG_TYPE_BASIC_MEMINFO:
        case MULTIBOOT_TAG_TYPE_BOOTDEV:
        case MULTIBOOT_TAG_TYPE_END:
            // unused
            break;

        default:
            terminal_print_string("\nMB unknown type $");
            terminal_print_u32(mbt->type);
            terminal_print_string(" size $");
            terminal_print_u32(mbt->size);
            break;
        }

        multiboot_info_ptr->total_size -= mbt->size;
        mbt = (struct multiboot_tag*)((void*)mbt + mbt->size);

        // align-up the mbt pointer to 8 byte boundaries
        if(((uint64_t)mbt & 0x07) != 0) {
            int alignment = 8 - ((uint64_t)mbt & 0x07);
            mbt = (struct multiboot_tag*)((void *)mbt + alignment);
            multiboot_info_ptr->total_size -= alignment;
        }
    }

    // cause a page fault exception (testing the idt)
    //*(u32 *)0xfefefefe00000000 = 1;

    terminal_print_string("\n");
    u32 count = 0;
    while(1) {
        while(blocking > 0) {
            __cli();
            terminal_print_u8(scancode);
            terminal_print_string("\n");

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

    terminal_print_string("\n...exiting...");
}

