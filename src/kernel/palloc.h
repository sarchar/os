#ifndef __PALLOC_H__
#define __PALLOC_H__

#define PALLOC_MAX_ORDER 11 // 1 greater than the actual order, 2^10 pages * 4KiB/page = 4MiB max contiguous allocation

#define palloc_claim_one() palloc_claim(0)

void palloc_init();
void palloc_init_highmem();
intp palloc_claim(u8 n); // allocate 2^n pages
void palloc_abandon(intp base, u8 n); //base is 2^n pages

#endif
