#ifndef __PAGING_H__
#define __PAGING_H__

#define PAGE_SIZE  4096
#define PAGE_SHIFT 12

#define PAGING_KERNEL paging_get_kernel_page_table()

// convert between kerenl virtual addresses and physical addresses
#define __va_kernel(p) ((intp)(p) + (intp)&_kernel_vma_base)
#define __pa_kernel(v) ((intp)(v) - (intp)&_kernel_vma_base)

struct page_table;

enum MAP_PAGE_FLAGS {
    MAP_PAGE_FLAG_DISABLE_CACHE = (1 << 0),
    MAP_PAGE_FLAG_WRITABLE      = (1 << 1),
    MAP_PAGE_FLAG_USER          = (1 << 2)
};

void paging_init();

// called on the APs
void paging_set_kernel_page_table();
struct page_table* paging_get_kernel_page_table();
intp paging_get_cpu_table(struct page_table*);

// user space page tables
struct page_table* paging_create_private_table();

// map a single page into virtual memory
// flags uses enum MAP_PAGE_FLAGS
void paging_map_page(struct page_table*, intp phys, intp virt, u32 flags);
intp paging_unmap_page(struct page_table*, intp virt); // returns the physical address stored in that page table entry

// unmap a single page

// map a 2MiB huge block into virtual memory
void paging_map_2mb(intp phys, intp vert, u32 flags);

// map a chunk of memory using 4K and 2MB pages. region_start and size must be page aligned/multiple of a page size
// flags is enum MAP_PAGE_FLAGS
void paging_identity_map_region(struct page_table*, intp region_start, u64 size, u32 flags);

// dump page table information for a given virtual address
void paging_debug_table(struct page_table*);
void paging_debug_address(intp);

#endif
