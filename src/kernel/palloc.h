#ifndef __PALLOC_H__
#define __PALLOC_H__

#define PALLOC_INIT_MINIMUM_SIZE (64 * 1024)  // minimum size of a contiguous chunk of memory required to initialize the palloc
                                              // initialization happens even before we have a virtual memory manager, so we
                                              // have to manually map and claim some virtual memory space to operate before
                                              // going to into normal operation mode
#define PALLOC_MINIMUM_SIZE      (64 * 1024)  // minimum size of contiguous ram that can be added to the physical page manager

#define palloc_claim_one() palloc_claim(0)

void palloc_init();
void* palloc_claim(u8 n); // allocate 2^n pages

#endif
