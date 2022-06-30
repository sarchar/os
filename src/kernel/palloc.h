#ifndef __PALLOC_H__
#define __PALLOC_H__

#define PALLOC_INIT_MINIMUM_SIZE (64 * 1024)  // minimum size of a contiguous chunk of memory required to initialize the palloc
                                              // initialization happens even before we have a virtual memory manager, so we
                                              // have to manually map and claim some virtual memory space to operate before
                                              // going to into normal operation mode
#define PALLOC_MINIMUM_SIZE      (64 * 1024)  // minimum size of contiguous ram that can be added to the physical page manager

void palloc_init(void* init_ram, u64 init_ram_size);
void palloc_add_free_region(void* ram, u64 ram_size);

#endif
