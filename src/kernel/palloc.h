#ifndef __PALLOC_H__
#define __PALLOC_H__

#define palloc_claim_one() palloc_claim(0)

void palloc_init();
void* palloc_claim(u8 n); // allocate 2^n pages
void palloc_abandon(void* base, u8 n); //base is 2^n pages

#endif
