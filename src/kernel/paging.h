#ifndef __PAGING_H__
#define __PAGING_H__

#define PAGE_SIZE  4096
#define PAGE_SHIFT 12

// convert between kerenl virtual addresses and physical addresses
#define __va_kernel(p) ((intp)(p) + (intp)&_kernel_vma_base)
#define __pa_kernel(v) ((intp)(v) - (intp)&_kernel_vma_base)

enum MAP_PAGE_FLAGS {
    MAP_PAGE_FLAG_DISABLE_CACHE = (1 << 0),
    MAP_PAGE_FLAG_WRITABLE      = (1 << 1),
    MAP_PAGE_FLAG_USER          = (1 << 2)
};

void paging_init();

// called on the APs
void paging_set_kernel_page_table();

// map a single page into virtual memory
// flags uses enum MAP_PAGE_FLAGS
void paging_map_page(intp phys, intp virt, u32 flags);
intp paging_unmap_page(intp virt); // returns the physical address stored in that page table entry

// unmap a single page

// map a 2MiB huge block into virtual memory
void paging_map_2mb(intp phys, intp vert, u32 flags);

// map a chunk of memory using 4K and 2MB pages. region_start and size must be page aligned/multiple of a page size
// flags is enum MAP_PAGE_FLAGS
void paging_identity_map_region(intp region_start, u64 size, u32 flags);

// dump page table information for a given virtual address
void paging_debug_address(intp);

// TODO this eventually needs to move into a virtual memory manager
//intp vmem_map_page(intp phys, u32 flags);
#define vmem_map_page(phys,flags) vmem_map_pages(phys, 1, flags)
intp vmem_map_pages(intp phys, u64 npages, u32 flags);
//intp vmem_unmap_page(intp virt);  // returns the physical address stored in that page table entry
#define vmem_unmap_page(virt) vmem_unmap_pages(virt, 1)
intp vmem_unmap_pages(intp virt, u64 npages);  // returns the physical address for `virt`

#endif
