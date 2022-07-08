#ifndef __PAGING_H__
#define __PAGING_H__

// convert between kerenl virtual addresses and physical addresses
#define __va_kernel(p) ((intp)(p) + (intp)&_kernel_vma_base)
#define __pa_kernel(v) ((intp)(v) - (intp)&_kernel_vma_base)

void paging_init();

// map a single page into virtual memory
void paging_map_page(intp phys, intp virt);

// map a 2MiB huge block into virtual memory
void paging_map_2mb(intp phys, intp vert);

#endif
