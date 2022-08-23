#ifndef __VMEM_H__
#define __VMEM_H__

#define VMEM_KERNEL 0

// initialize the virtual memory manager
void vmem_init();

// create a new virtual memory area in the private address space
intp vmem_create_private_memory(struct page_table*);

// map contiguous pages
// returns base virtual address of mapped pages
//intp vmem_map_pages(intp phys, u64 npages, u32 flags);
intp vmem_map_pages(intp _vmem, intp phys, u64 npages, u32 flags);

// unmap contiguous mages
// returns base physical address of unmapped pages
//intp vmem_unmap_pages(intp virt, u64 npages);
intp vmem_unmap_pages(intp _vmem, intp virt, u64 npages);

// helpers for single page map/unmap
#define vmem_map_page(vmem,phys,flags) vmem_map_pages(vmem, phys, 1, flags)
#define vmem_unmap_page(vmem, virt) vmem_unmap_pages(vmem, virt, 1)

#endif
