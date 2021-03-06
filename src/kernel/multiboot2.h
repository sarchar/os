#ifndef __MULTIBOOT2_H__
#define __MULTIBOOT2_H__

#include "ext/multiboot2.h"

struct multiboot_info
{
	multiboot_uint32_t total_size;
	multiboot_uint32_t reserved;
};

enum MULTIBOOT_REGION_TYPE {
    MULTIBOOT_REGION_TYPE_AVAILABLE,
    MULTIBOOT_REGION_TYPE_AHCI_RECLAIMABLE,
};

void multiboot2_parse(struct multiboot_info*);

// loop over available regions (for bootmem)
intp multiboot2_mmap_next_free_region(u64* size, u8* region_type);
intp multiboot2_acpi_get_rsdp();
void multiboot2_framebuffer_get(u32** framebuffer, u32* width, u32* height, u8* bpp, u32* pitch, u8* type);

#endif
