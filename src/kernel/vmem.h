#ifndef __VMEM_H__
#define __VMEM_H__

// initialize the virtual memory manager
void vmem_init();

// map contiguous pages
// returns base virtual address of mapped pages
intp vmem_map_pages(intp phys, u64 npages, u32 flags);

// unmap contiguous mages
// returns base physical address of unmapped pages
intp vmem_unmap_pages(intp virt, u64 npages);

// helpers for single page map/unmap
#define vmem_map_page(phys,flags) vmem_map_pages(phys, 1, flags)
#define vmem_unmap_page(virt) vmem_unmap_pages(virt, 1)

#endif
