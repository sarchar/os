#ifndef __KALLOC_H__
#define __KALLOC_H__

void kalloc_init();

void* kalloc(u32 size);
void  kfree(void* ptr, u32 size);

#endif
