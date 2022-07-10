// parse multiboot2 info struct
#include "common.h"

#include "acpi.h"
#include "bootmem.h"
#include "cpu.h"
#include "efifb.h"
#include "kernel.h"
#include "multiboot2.h"
#include "string.h"
#include "stdio.h"
#include "terminal.h"

static struct multiboot_tag_load_base_addr* mbt_load_base_addr  = null;
static struct multiboot_tag_string*         mbt_cmdline         = null;
static struct multiboot_tag_string*         mbt_bootloader_name = null;
static struct multiboot_tag_mmap*           mbt_mmap            = null;
static struct multiboot_tag_framebuffer*    mbt_framebuffer     = null;
static struct multiboot_tag_new_acpi*       mbt_acpi            = null;

static void multiboot2_handle_mmap()
{
    assert(mbt_load_base_addr != null, "we must know our load address before allocating regions to bootmem");
    
    struct multiboot_mmap_entry* mbt_mmap_entry = mbt_mmap->entries;
    for(u32 size = mbt_mmap->size - sizeof(struct multiboot_tag_mmap); size != 0;) {
        /*
        fprintf(stderr, "MBT Memory map entry: $%lX len %llu ", (intp)mbt_mmap_entry->addr, mbt_mmap_entry->len);
    
        switch(mbt_mmap_entry->type) {
        case MULTIBOOT_MEMORY_AVAILABLE:
            fprintf(stderr, " AVAILABLE\n");
            break;
        case MULTIBOOT_MEMORY_RESERVED:
            fprintf(stderr, " RESERVED\n");
            break;
        case MULTIBOOT_MEMORY_BADRAM:
            fprintf(stderr, " BADRAM\n");
            break;
        case MULTIBOOT_MEMORY_NVS:
            fprintf(stderr, " NVS\n");
            break;
        case MULTIBOOT_MEMORY_ACPI_RECLAIMABLE:
            fprintf(stderr, " ACPI RECLAIMABLE\n");
            break;
        default:
            fprintf(stderr, " UNKNOWN\n");
            break;
        }
        */
    
        // Add all available memory to the system
        // TODO reclaim ACPI ?
        if(mbt_mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE && mbt_mmap_entry->addr < 0x100000000) {
            // the region the kernel is loaded into cannot be used for bootmem (well, unless we use _kernel_end_address 
            // but there's no point for that right now. TODO later we can move the kernel and reclaim the entire region
            if(mbt_load_base_addr->load_base_addr < mbt_mmap_entry->addr 
               || mbt_load_base_addr->load_base_addr > (mbt_mmap_entry->addr + mbt_mmap_entry->len)) {
                bootmem_addregion((void*)mbt_mmap_entry->addr, mbt_mmap_entry->len);
                mbt_mmap_entry->type = MULTIBOOT_MEMORY_RESERVED;
            }
        }
    
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
}

static void multiboot2_handle_framebuffer()
{
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
    
    // draw an 8x8 square in the corner to indicate that we made it this far
    for(u32 y = 0; y < 8; y++) {
        for(u32 x = 0; x < 8; x++) {
            efifb_putpixel(x + mbt_framebuffer->common.framebuffer_width - 16,
                           y + mbt_framebuffer->common.framebuffer_height - 16,
                           COLOR(0, 255, 0));
        }
    }
}

static void multiboot2_handle_acpi()
{
    // print the 8 characters at the beginning of the rsdp
    acpi_set_rsdp_base((intp)mbt_acpi->rsdp);
}

void multiboot2_parse(struct multiboot_info* multiboot_info)
{
    // loop over all the tags in the structure up to total_size, which includes the info structure size
    multiboot_info->total_size -= sizeof(struct multiboot_info);

    struct multiboot_tag* mbt = (struct multiboot_tag*)((void*)multiboot_info + sizeof(struct multiboot_info));
    while(multiboot_info->total_size != 0) {
        switch(mbt->type) {
        case MULTIBOOT_TAG_TYPE_CMDLINE:
            mbt_cmdline = (struct multiboot_tag_string*)mbt;
            fprintf(stderr, "MBT Command line: %s\n", mbt_cmdline->string);
            break;

        case MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME:
            mbt_bootloader_name = (struct multiboot_tag_string*)mbt;
            fprintf(stderr, "MBT Boot loader name: %s\n", mbt_bootloader_name->string);
            break;

        case MULTIBOOT_TAG_TYPE_LOAD_BASE_ADDR:
            mbt_load_base_addr = (struct multiboot_tag_load_base_addr*)mbt;
            fprintf(stderr, "MBT Base load address: $%lX\n", mbt_load_base_addr->load_base_addr);
            break;

        case MULTIBOOT_TAG_TYPE_MMAP:
            assert(mbt_load_base_addr != null, "we must know our load address before allocating regions to bootmem");
            mbt_mmap = (struct multiboot_tag_mmap*)mbt;
            break;

        case MULTIBOOT_TAG_TYPE_FRAMEBUFFER:
            mbt_framebuffer = (struct multiboot_tag_framebuffer*)mbt;
            fprintf(stderr, "MBT Framebuffer: address $%lX pitch %d width %d height %d bpp %d type %d\n",
                    (intp)mbt_framebuffer->common.framebuffer_addr,
                    (intp)mbt_framebuffer->common.framebuffer_pitch,
                    (intp)mbt_framebuffer->common.framebuffer_width,
                    (intp)mbt_framebuffer->common.framebuffer_height,
                    (intp)mbt_framebuffer->common.framebuffer_bpp,
                    (intp)mbt_framebuffer->common.framebuffer_type);

        case MULTIBOOT_TAG_TYPE_EFI_MMAP:
            {
                struct multiboot_tag_efi_mmap* mbt_efi_mmap = (struct multiboot_tag_efi_mmap*)mbt;
                fprintf(stderr, "MBT EFI mmap: descriptor size %d\n", mbt_efi_mmap->descr_size);
            }
            break;

        case MULTIBOOT_TAG_TYPE_ACPI_NEW:
            mbt_acpi = (struct multiboot_tag_new_acpi*)mbt;
            fprintf(stderr, "MBT ACPI: RSDP at 0x%lX\n", (intp)&mbt_acpi->rsdp[0]);
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
            fprintf(stderr, "MB unknown type $%X size=%d\n", mbt->type, mbt->size);
            break;
        }

        multiboot_info->total_size -= mbt->size;
        mbt = (struct multiboot_tag*)((void*)mbt + mbt->size);

        // align-up the mbt pointer to 8 byte boundaries
        if(((uint64_t)mbt & 0x07) != 0) {
            int alignment = 8 - ((uint64_t)mbt & 0x07);
            mbt = (struct multiboot_tag*)((void *)mbt + alignment);
            multiboot_info->total_size -= alignment;
        }
    }

    multiboot2_handle_mmap();
    multiboot2_handle_framebuffer();
    multiboot2_handle_acpi();
}


