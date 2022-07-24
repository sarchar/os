// parse multiboot2 info struct
// I believe this could be code and data in memory that could be recovered by the kernel
// if we put this code and data into separate, disposable sections
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

intp multiboot2_mmap_next_free_region(u64* size, u8* region_type)
{
    assert(mbt_load_base_addr != null, "we must know our load address before allocating regions to bootmem");

    static struct multiboot_mmap_entry* iter = null;
    static u32 iter_size;

    if(iter == null) {
        iter = mbt_mmap->entries;
        iter_size = mbt_mmap->size - sizeof(struct multiboot_tag_mmap);
    } 

    intp ret = (intp)-1;

    while(iter_size > 0) {
        //fprintf(stderr, "multiboot: region addr=0x%lX size=0x%lX type=%d\n", iter->addr, iter->len, iter->type);

        // TODO reclaim ACPI ?
        // TODO the check for <0x100000000 and for kernel region should be done in bootmem itself
        // maybe pass in a flags parameter and set flags based on the type of region it is
        switch(iter->type) {
        case MULTIBOOT_MEMORY_AVAILABLE:
            if(iter->addr < 0x100000000) {
                // the region the kernel is loaded into cannot be used for bootmem (well, unless we use _kernel_end_address 
                // but there's no point for that right now. TODO later we can move the kernel and reclaim the entire region
                if(mbt_load_base_addr->load_base_addr < iter->addr || mbt_load_base_addr->load_base_addr > (iter->addr + iter->len)) {
                    ret = (intp)iter->addr;
                    *size = iter->len;
                    *region_type = MULTIBOOT_REGION_TYPE_AVAILABLE;
                }
            }
            break;

        case MULTIBOOT_MEMORY_ACPI_RECLAIMABLE:
            ret = (intp)iter->addr;
            *size = iter->len;
            *region_type = MULTIBOOT_REGION_TYPE_AHCI_RECLAIMABLE;
            break;

        default:
            ret = (intp)-1;
            break;
        }
    
        // next entry
        iter = (struct multiboot_mmap_entry*)((void *)iter + mbt_mmap->entry_size);
        iter_size -= mbt_mmap->entry_size;
    
        // align-up the mbt pointer to 8 byte boundaries
        if(((uint64_t)iter & 0x07) != 0) {
            int alignment = 8 - ((uint64_t)iter & 0x07);
            iter = (struct multiboot_mmap_entry*)((void *)iter + alignment);
            iter_size -= alignment;
        }

        // if address is valid, return it. otherwise check next region
        if(ret != (intp)-1) return ret;
    }

    // ran out of regions. reset iterator to allow more iterations, and return false
    iter = null;
    return (intp)-1;
}

intp multiboot2_acpi_get_rsdp()
{
    // print the 8 characters at the beginning of the rsdp
    return (intp)mbt_acpi->rsdp;
}

void multiboot2_framebuffer_get(u32** framebuffer, u32* width, u32* height, u8* bpp, u32* pitch, u8* type)
{
    *framebuffer = (u32*)mbt_framebuffer->common.framebuffer_addr;
    *width       = mbt_framebuffer->common.framebuffer_width;
    *height      = mbt_framebuffer->common.framebuffer_height;
    *bpp         = mbt_framebuffer->common.framebuffer_bpp;
    *pitch       = mbt_framebuffer->common.framebuffer_pitch;
    *type        = mbt_framebuffer->common.framebuffer_type;
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
            fprintf(stderr, "multiboot: command line: %s\n", mbt_cmdline->string);
            break;

        case MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME:
            mbt_bootloader_name = (struct multiboot_tag_string*)mbt;
            //fprintf(stderr, "MBT Boot loader name: %s\n", mbt_bootloader_name->string);
            break;

        case MULTIBOOT_TAG_TYPE_LOAD_BASE_ADDR:
            mbt_load_base_addr = (struct multiboot_tag_load_base_addr*)mbt;
            fprintf(stderr, "multiboot: base load address: 0x%08lX\n", mbt_load_base_addr->load_base_addr);
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
                //TODO is this tag needed if MULTIBOOT_TAG_TYPE_MMAP works too?
                //struct multiboot_tag_efi_mmap* mbt_efi_mmap = (struct multiboot_tag_efi_mmap*)mbt;
                //fprintf(stderr, "MBT EFI mmap: descriptor size %d\n", mbt_efi_mmap->descr_size);
            }
            break;

        case MULTIBOOT_TAG_TYPE_ACPI_NEW:
            mbt_acpi = (struct multiboot_tag_new_acpi*)mbt;
            fprintf(stderr, "multiboot: ACPI RSDP at 0x%lX\n", (intp)&mbt_acpi->rsdp[0]);
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
}


