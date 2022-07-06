#ifndef __MULTIBOOT2_H__
#define __MULTIBOOT2_H__

#include "ext/multiboot2.h"

struct multiboot_info
{
	multiboot_uint32_t total_size;
	multiboot_uint32_t reserved;
};

void multiboot2_parse(struct multiboot_info*);

#endif
