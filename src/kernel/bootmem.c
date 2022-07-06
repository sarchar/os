// bootmem allocator is very, very basic with a few restrictions
//
// 1. allocation size must be at least sizeof(struct bootmem_region) (16) bytes
// 2. there is no freeing mechanism, so memory will be permanently reserved by the kernel
// 3. only low memory (<4G) is available
// 4. the allocated memory must be physically contiguous, so the largest allocation size
//    depends on the largest memory region available on the system
// 5. NOT page based, which means when it comes time to move regions to palloc, partially
//    used pages will be wasted
// 
#include "common.h"

#include "bootmem.h"
#include "cpu.h"
#include "efifb.h"
#include "kernel.h"
#include "terminal.h"

#define BOOTMEM_SMALLEST_REGION_SIZE 1024 // smallest region of memory that we care to manage

struct bootmem_region {
    struct bootmem_region* next;
    u64 size;
};

static struct bootmem_region* regions = null;

struct {
    u64 free;
    u64 allocated;
    u64 wasted_due_to_size;
    u64 wasted_due_to_alignment;
    u64 wasted_due_to_partial_page;
    u8  num_regions;
} bootmem_accounting = { 0, };

void bootmem_addregion(void* region_start, u64 size)
{
    if(size < BOOTMEM_SMALLEST_REGION_SIZE) {
        // not enough data remaining to care about, so toss it
        bootmem_accounting.wasted_due_to_size += size;

        terminal_print_string("bootmem: ignoring region $"); terminal_print_pointer(region_start);
        terminal_print_string(" size=$"); terminal_print_u64(size);
        terminal_putc(L'\n');
        
        return;
    }

    struct bootmem_region* new_region = (struct bootmem_region*)region_start;
    new_region->next = regions;
    new_region->size = size;
    regions = new_region;
    bootmem_accounting.num_regions += 1;
    bootmem_accounting.free += size;

    terminal_print_string("bootmem: adding region $"); terminal_print_pointer(region_start);
    terminal_print_string(" size=$"); terminal_print_u64(size);
    terminal_putc(L'\n');
}

void* bootmem_alloc(u64 size, u8 alignment)
{
    // size cannot be smaller than sizeof(struct bootmem_region), otherwise an
    // allocation could overwrite data inside our region struct
    size = max(size, sizeof(struct bootmem_region));

    struct bootmem_region* prev = null;
    struct bootmem_region* cur = regions;

    u8 extra;
    while(cur != null) {
        extra = (intp)__alignup(cur, alignment) - (intp)cur; // increase size for alignment requirement

        if((size + extra) <= cur->size) break;         // break out if this region has enough memory

        prev = cur;
        cur = cur->next;
    }

    if(cur == null) {
        terminal_print_string("bootmem: allocation of size $"); terminal_print_pointer((void*)size);
        terminal_print_string(" failed\n");
        assert(false, "bootmem alloc failed");
        return null;
    }

    // treat the alignment as an increase in size
    size += extra;

    // handle remaining region
    u64 size_remaining = cur->size - size;
    if(size_remaining >= BOOTMEM_SMALLEST_REGION_SIZE) {
        struct bootmem_region* new_region = (struct bootmem_region*)((u8*)cur + size);
        new_region->next = cur->next;
        new_region->size = cur->size - size;
        if(prev != null) {
            prev->next = new_region; // update prev pointer to point to the correct place
        } else {
            regions = new_region;
        }
    } else {
        // not enough data remaining to care about, so toss it
        bootmem_accounting.wasted_due_to_size += size_remaining;

        if(prev != null) {
            prev->next = cur->next;
        } else {
            regions = cur->next;
        }
    }

    // the 'cur' pointer is the memory you get
    bootmem_accounting.allocated += size;
    bootmem_accounting.wasted_due_to_alignment += extra;
    return (void*)__alignup(cur, alignment);
}

u32 bootmem_count_free_pages()
{
    struct bootmem_region* cur = regions;
    u32 npages = 0;
    while(cur != null) {
        u32 wasted = 4096 - __alignof(cur, 4096);
        u32 p = (cur->size - wasted) >> 12;
        npages += p;
        //wasted += cur->size - (p << 12);
        cur = cur->next;
    }

    return npages;
}

u64 bootmem_reclaim_region(void** region_start)
{
    struct bootmem_region* cur = regions;
    if(cur == null) return 0;

    regions = cur->next;
    *region_start = (void*)cur;
    assert(cur->size != 0, "bug: all memory in this region has been consumed"); // TODO actually allow returning 0 size regions so palloc can keep proper indexing?
    return cur->size;
}

// I don't care about keeping a running num_regions tally, since
// this is only called once during kernel bootup
u8 bootmem_num_regions()
{
    u8 c = 0;
    struct bootmem_region* cur = regions;
    while(cur != null) {
        c++;
        cur = cur->next;
    }

    return c;
}

u64 bootmem_get_region_size(u8 region_index)
{
    struct bootmem_region* cur = regions;
    while(region_index-- > 0) cur = cur->next;
    return cur->size;
}

