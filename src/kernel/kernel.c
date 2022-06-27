// Based on code from https://wiki.osdev.org/Bare_Bones
#include "common.h"

#include "efifb.h"
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
 
//!/* Hardware text mode color constants. */
//!enum vga_color {
//!    VGA_COLOR_BLACK = 0,
//!    VGA_COLOR_BLUE = 1,
//!    VGA_COLOR_GREEN = 2,
//!    VGA_COLOR_CYAN = 3,
//!    VGA_COLOR_RED = 4,
//!    VGA_COLOR_MAGENTA = 5,
//!    VGA_COLOR_BROWN = 6,
//!    VGA_COLOR_LIGHT_GREY = 7,
//!    VGA_COLOR_DARK_GREY = 8,
//!    VGA_COLOR_LIGHT_BLUE = 9,
//!    VGA_COLOR_LIGHT_GREEN = 10,
//!    VGA_COLOR_LIGHT_CYAN = 11,
//!    VGA_COLOR_LIGHT_RED = 12,
//!    VGA_COLOR_LIGHT_MAGENTA = 13,
//!    VGA_COLOR_LIGHT_BROWN = 14,
//!    VGA_COLOR_WHITE = 15,
//!};
//!
//!static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) 
//!{
//!    return fg | bg << 4;
//!}
//! 
//!static inline uint16_t vga_entry(unsigned char uc, uint8_t color) 
//!{
//!    return (uint16_t) uc | (uint16_t) color << 8;
//!}
//! 
//!size_t strlen(const char* str) 
//!{
//!    size_t len = 0;
//!    while (str[len])
//!        len++;
//!    return len;
//!}
//! 
//!static const size_t VGA_WIDTH = 80;
//!static const size_t VGA_HEIGHT = 25;
//!
//!size_t terminal_row;
//!size_t terminal_column;
//!uint8_t terminal_color;
//!uint16_t* terminal_buffer;
//! 
//!void terminal_initialize(void) 
//!{
//!    return;
//!
//!    terminal_row = 0;
//!    terminal_column = 0;
//!    terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
//!    terminal_buffer = (uint16_t*) 0x000B8000; //0xB8000;
//!    for (size_t y = 0; y < VGA_HEIGHT; y++) {
//!        for (size_t x = 0; x < VGA_WIDTH; x++) {
//!            const size_t index = y * VGA_WIDTH + x;
//!            terminal_buffer[index] = vga_entry(' ', terminal_color);
//!        }
//!    }
//!}
//!
//!void terminal_setcolor(uint8_t color) 
//!{
//!    terminal_color = color;
//!}
//! 
//!void terminal_putentryat(char c, uint8_t color, size_t x, size_t y) 
//!{
//!    const size_t index = y * VGA_WIDTH + x;
//!    terminal_buffer[index] = vga_entry(c, color);
//!}
//! 
//!void terminal_putchar(char c) 
//!{
//!    return;
//!
//!    terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
//!    if (++terminal_column == VGA_WIDTH) {
//!        terminal_column = 0;
//!        if (++terminal_row == VGA_HEIGHT) terminal_row = 0;
//!    }
//!}
//!
//!void terminal_write(const char* data, size_t size) 
//!{
//!    for (size_t i = 0; i < size; i++) {
//!        char c = data[i];
//!        if(c == '\n') {
//!            terminal_column = 0;
//!            terminal_row += 1;
//!            //TODO: scroll
//!        } else {
//!            terminal_putchar(data[i]);
//!        }
//!    }
//!}
//! 
//!void terminal_writestring(const char* data) 
//!{
//!    terminal_write(data, strlen(data));
//!}
//! 
//!static char const HEX_LETTERS[] = "0123456789ABCDEF";
//!
//!void terminal_printpointer(void* address)
//!{
//!    // don't display the upper long if it's all zeros
//!    int loop_size = (((long long)address & 0xFFFFFFFF00000000LL) == 0) ? 8 : 16;
//!
//!    terminal_putchar('$');
//!    for(int i = 0; i < loop_size; i++) {
//!        unsigned char v = (((long long)address) >> (((loop_size - 1) - i) * 4)) & 0x0F;
//!        terminal_putchar(HEX_LETTERS[v]);
//!    }
//!}
//!
//!void terminal_print_u8(uint8_t value)
//!{
//!    for(int i = 0; i < 2; i++) {
//!        unsigned char v = (value >> ((1 - i) * 4)) & 0x0F;
//!        terminal_putchar(HEX_LETTERS[v]);
//!    }
//!}
//!
//!void terminal_print_u32(uint32_t value)
//!{
//!    for(int i = 0; i < 8; i++) {
//!        unsigned char v = (value >> ((7 - i) * 4)) & 0x0F;
//!        terminal_putchar(HEX_LETTERS[v]);
//!    }
//!}
//!
//!void terminal_print_u64(uint64_t value)
//!{
//!    for(int i = 0; i < 16; i++) {
//!        unsigned char v = (value >> ((15 - i) * 4)) & 0x0F;
//!        terminal_putchar(HEX_LETTERS[v]);
//!    }
//!}

struct multiboot_info
{
	multiboot_uint32_t total_size;
	multiboot_uint32_t reserved;
};

void kernel_panic()
{
    // attempt to set the screen to all red
    efifb_clear(COLOR(255,0,0));

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

    terminal_init();

//!    terminal_initialize();
//!    terminal_writestring("Hello, kernel World!\nTest\nMultiboot info ptr: ");
//!    terminal_printpointer(multiboot_info_ptr);
//!	terminal_writestring("\nMultiboot total size: ");
//!    terminal_print_u32(multiboot_info_ptr->total_size);

    // loop over all the tags in the structure up to total_size, which includes the info structure size
    multiboot_info_ptr->total_size -= 8;

    struct multiboot_tag* mbt = (struct multiboot_tag*)((void*)multiboot_info_ptr + sizeof(struct multiboot_info));
    while(multiboot_info_ptr->total_size != 0) {
        switch(mbt->type) {
        case MULTIBOOT_TAG_TYPE_CMDLINE:
            mbt_string = (struct multiboot_tag_string*)mbt;
//!            terminal_writestring("\nMBT Command line: ");
//!            terminal_writestring(mbt_string->string);
            break;

        case MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME:
            mbt_string = (struct multiboot_tag_string*)mbt;
//!            terminal_writestring("\nMBT Boot loader name: ");
//!            terminal_writestring(mbt_string->string);
            break;

        case MULTIBOOT_TAG_TYPE_MMAP:
            mbt_mmap = (struct multiboot_tag_mmap*)mbt;
            mbt_mmap_entry = mbt_mmap->entries;

            for(uint32_t size = mbt_mmap->size - sizeof(struct multiboot_tag_mmap); size != 0;) {
//!                terminal_writestring("\nMBT Memory map entry: ");
//!                terminal_printpointer((void*)mbt_mmap_entry->addr);
//!                terminal_writestring(" length ");
//!                terminal_print_u64(mbt_mmap_entry->len);
//!                terminal_writestring(" type 0x");
//!                terminal_putchar(HEX_LETTERS[mbt_mmap_entry->type]);
                
                // next entry
                mbt_mmap_entry = (struct multiboot_mmap_entry*)((void *)mbt_mmap_entry + mbt_mmap->entry_size);
                size -= mbt_mmap->entry_size;
            }
            break;

        case MULTIBOOT_TAG_TYPE_LOAD_BASE_ADDR:
            mbt_load_base_addr = (struct multiboot_tag_load_base_addr*)mbt;
//!            terminal_writestring("\nMBT Base load address: ");
//!            terminal_print_u32(mbt_load_base_addr->load_base_addr);
            break;

        case MULTIBOOT_TAG_TYPE_FRAMEBUFFER:
            mbt_framebuffer = (struct multiboot_tag_framebuffer*)mbt;
//!            terminal_writestring("\nMBT Framebuffer: address ");
//!            terminal_printpointer((void*)mbt_framebuffer->common.framebuffer_addr);
//!            terminal_writestring(" pitch ");
//!            terminal_print_u32(mbt_framebuffer->common.framebuffer_pitch);
//!            terminal_writestring(" width ");
//!            terminal_print_u32(mbt_framebuffer->common.framebuffer_width);
//!            terminal_writestring(" height ");
//!            terminal_print_u32(mbt_framebuffer->common.framebuffer_height);
//!            terminal_writestring(" bpp ");
//!            terminal_print_u8(mbt_framebuffer->common.framebuffer_bpp);
//!            terminal_writestring(" type ");
//!            terminal_print_u8(mbt_framebuffer->common.framebuffer_bpp);

            if(!efifb_init((u32*)mbt_framebuffer->common.framebuffer_addr,
                           mbt_framebuffer->common.framebuffer_width,
                           mbt_framebuffer->common.framebuffer_height,
                           mbt_framebuffer->common.framebuffer_bpp,
                           mbt_framebuffer->common.framebuffer_pitch)) {
                PANIC();
            }

            // set screen blue to show that we have initialized the frame buffer
            efifb_clear(COLOR(0, 0, 255));
            // wait for some arbitrary amount of time so that the blue screen is visible
            for(int i = 0; i < 3000000; i++) asm volatile("pause");
            // and clear it to black before proceeding
            efifb_clear(COLOR(0, 0, 0));

            // draw a 8x8 square in the corner to indicate that we made it this far
            for(u32 y = 0; y < 8; y++) {
                for(u32 x = 0; x < 8; x++) {
                    efifb_putpixel(x + mbt_framebuffer->common.framebuffer_width - 16,
                                   y + mbt_framebuffer->common.framebuffer_height - 16,
                                   COLOR(0, 255, 0));
                }
            }

            // put a character to screen
            terminal_setc(L'0', 30, 10);

            break;

        case MULTIBOOT_TAG_TYPE_EFI64:
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
//!            terminal_writestring("\nMB unknown type ");
//!            terminal_print_u32(mbt->type);
//!            terminal_writestring(" size ");
//!            terminal_print_u32(mbt->size);
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

//!    terminal_writestring("\ndone");
}

