#ifndef __PAGING_H__
#define __PAGING_H__

// convert between kerenl virtual addresses and physical addresses
#define __va_kernel(p) ((intp)(p) + (intp)&_kernel_vma_base)
#define __pa_kernel(v) ((intp)(v) - (intp)&_kernel_vma_base)

enum MAP_PAGE_FLAGS {
    MAP_PAGE_FLAG_DISABLE_CACHE = (1 << 0),
    MAP_PAGE_FLAG_WRITABLE      = (1 << 1)
};

void paging_init();

// map a single page into virtual memory
// flags uses enum MAP_PAGE_FLAGS
void paging_map_page(intp phys, intp virt, u32 flags);

// unmap a single page

// map a 2MiB huge block into virtual memory
void paging_map_2mb(intp phys, intp vert, u32 flags);

// map a chunk of memory using 4K and 2MB pages. region_start and size must be page aligned/multiple of a page size
// flags is enum MAP_PAGE_FLAGS
void paging_identity_map_region(intp region_start, u64 size, u32 flags);

// dump page table information for a given virtual address
void paging_debug_address(intp);

// TODO this eventually needs to move into a virtual memory manager
intp vmem_map_page(intp phys, u32 flags);

#endif
