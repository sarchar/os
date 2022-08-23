#ifndef __BOOTMEM_H__
#define __BOOTMEM_H__

void bootmem_init();

void* bootmem_alloc(u64 size, u8 alignment);
u64 bootmem_count_free_pages();
u64 bootmem_reclaim_region(intp* region_start);
u8 bootmem_num_regions();
u64 bootmem_get_region_size(u8 region_index);

#endif
