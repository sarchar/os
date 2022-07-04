// palloc - physical page allocator
// A basic implementation of the buddy system, inspired by how Linux (used to?) work, at 
// least around the 2.4-2.6 era.  Lots of information learned from 
// https://hzliu123.github.io/linux-kernel/Physical%20Memory%20Management%20in%20Linux.pdf
#include "common.h"

#include "bootmem.h"
#include "kernel.h"
#include "palloc.h"
#include "terminal.h"

#define PALLOC_MAX_ORDER 11 // 2^10 pages * 4KiB/page = 4MiB max contiguous allocation

struct free_page {
    struct free_page* next;
    u8*  map; // this isn't used in the linked list
};

static struct free_page* free_page_head[PALLOC_MAX_ORDER] = { null, };

// region data is required to determine page index for the bitmaps
struct region {
    void* start;
    void* end;
    u64   size;
    u64   npages;
};

static struct region* regions;
static u8 num_regions;

void palloc_init()
{
    // allocate storage for our free_page_head pointers
    for(u8 i = 0; i < PALLOC_MAX_ORDER; i++) {
        free_page_head[i] = (struct free_page*)bootmem_alloc(sizeof(struct free_page), 8);
        zero(free_page_head[i]);
    }

    // create storage for the region info
    num_regions = bootmem_num_regions();
    regions = (struct region*)bootmem_alloc(sizeof(struct region) * num_regions, 8);
    memset(regions, 0, sizeof(struct region) * num_regions);

    // We ask bootmem for how many free pages it has, and then allocate bitmaps
    // using that information. however, allocating the bitmap itself reduces the number
    // of pages available to the system, which means there are pages consumed for the bitmap
    // that represent the bitmap itself, which is unnecessary. For example, at order 0,
    //
    // 1TiB ram = 268,435,456 pages, requiring a 33,554,432 byte bitmap, or 8192 pages. So 8KiB (2 pages) of
    // space in the bitmap will be completely unused and left as zero for eternity.
    //
    // 8KiB/1TiB = 00.0000007% wasted space for my completely naive implementation
    //
    // Also, 33,554,432/1TiB = 00.00305% space is used for the bitmap
    //
    // The bitmap and sizes decrease by 2 for every higher order
    //
    // Also note all the bitmap sizes are halved due to the buddy system
    //
    u64 npages = bootmem_count_free_pages();

    // allocate the bitmaps
    for(u32 order = 0; order < PALLOC_MAX_ORDER - 1; ++order) {
        u64 mapsize = ((npages >> (order + 1)) + 7) >> 3; // divide pages by 2^(order layer, plus 1 because of the buddy), then divide by sizeof(byte) rounded up
        free_page_head[order]->map = (u8*)bootmem_alloc(mapsize, 8);
        memset(free_page_head[order]->map, 0, mapsize);
    }

    // and now that we have a bitmaps, we can start reclaiming bootmem
    void* region_start;
    u64 region_size;
    u8 region_index = 0;

    while((region_size = bootmem_reclaim_region(&region_start)) != 0) {
        // alignup
        u16 wasted_alignment = (4096 - __alignof(region_start, 4096)) & 4095;
        region_start = __alignup(region_start, 4096);
        region_size -= wasted_alignment;

        terminal_print_string("palloc: reclaiming region at start="); terminal_print_pointer(region_start);
        terminal_print_string(" size=$"); terminal_print_u32(region_size);
        terminal_print_string(" wasted=$"); terminal_print_u16(wasted_alignment);
        terminal_putc(L'\n');

        // put in region info
        regions[region_index].start = region_start;
        regions[region_index].size = region_size;
        regions[region_index].end = (void*)((intp)region_start + region_size);
        regions[region_index].npages = region_size >> 12; // this value will be used quite a lot, so a small optimization here
        region_index++;

        // now add the region to free_page_head
        while(region_size != 0) {
            u8 order = PALLOC_MAX_ORDER - 1;
            u32 block_size = 1 << (order + 12);

            // if any bit in mask (1<<order)-1 is set on the region start address, this node can't have a buddy, so it goes down in order and stays there forever
            // I.e., if a 4MiB region starts *not* on a 4MiB aligned boundary, it can't be two 2MiB blocks later.
            while((order > 0) && ((((intp)region_start & (block_size - 1)) != 0) || (block_size > region_size))) {
                --order;
                block_size >>= 1;
            }

            // add the block to the list
            struct free_page* fp = (struct free_page*)region_start;
            fp->next = free_page_head[order]->next;
            free_page_head[order]->next = fp;

            //terminal_print_string("palloc: adding block at order="); terminal_print_u8(order);
            //terminal_print_string(" address=$"); terminal_print_pointer(region_start);
            //terminal_print_string(" size=$"); terminal_print_u32(region_size); //size remaining in region
            //terminal_print_string(" next=$"); terminal_print_pointer(fp->next); //next pointer
            //terminal_putc(L'\n');

            // increment region_start and reduce region_size
            region_start = (void*)((intp)region_start + block_size);

            region_size -= block_size;

        }
    }
}

// can pass null for valid if you know the page is managed
static inline u64 palloc_indexof(struct free_page* page, bool* valid)
{
    // determine an index into bitmap at order 0 where `page' goes
    intp page_addr = (intp)page;
    u64 index = 0;
    
    if(valid != null) *valid = false;

    for(u8 i = 0; i < num_regions; i++) {
        if(page_addr < (intp)regions[i].start || page_addr >= (intp)regions[i].end) {
            // not in this region
            index += regions[i].npages;
        } else {
            intp offs = page_addr - (intp)regions[i].start;
            index += offs >> 12;
            if(valid != null) *valid = true;
        }
    }

    return index;
}

void* palloc_claim(u8 n) // allocate 2^n pages
{
    assert(n < PALLOC_MAX_ORDER, "n must be a valid order size");

    // working order
    u8 order = n;

    // find first order >= requested size with free blocks
    while(order < PALLOC_MAX_ORDER && free_page_head[order]->next == null) order++;

    // out of memory?
    if(order == PALLOC_MAX_ORDER) return null;

    // remove the head from the current order
    struct free_page* left = free_page_head[order]->next;
    //terminal_print_u8(order); terminal_putc(L' '); terminal_print_pointer((void*)left);terminal_putc(L'\n');
    free_page_head[order]->next = left->next;

    // split blocks all the way down to the requested size, if necessary
    while(order != n) {
        u64 block_size = (1 << order) << 12; // 2^n*4096
    
        //terminal_print_string("palloc: splitting block order $"); terminal_print_u8(order);
        //terminal_print_string(" address=$"); terminal_print_pointer((void*)left);
        //terminal_putc(L'\n');

        // split 'fp' into two block_size/2 blocks
        struct free_page* right = (struct free_page*)((u8*)left + (block_size >> 1));
        zero(right);

        //terminal_print_string("* right address="); terminal_print_pointer((void*)right);
        //terminal_putc(L'\n');
        assert(((intp)left ^ (1 << ((order - 1) + 12))) == (intp)right, "verifying buddy address");

        // mark 'right' as used
        u32 right_index = palloc_indexof(right, null) >> (order); // order-1 for lower order but +1 for buddy system
        //terminal_print_string("* right_index = "); terminal_print_u32(right_index); terminal_putc(L'\n');
        free_page_head[order - 1]->map[right_index >> 3] ^= (1 << (right_index & 7)); // toggle the bit, should become a 1 now

        // add the right node to the free page list at the lower order
        right->next = free_page_head[order - 1]->next;
        free_page_head[order - 1]->next = right;

        // and continue dividing 'left' if necessary
        --order;
    }

    // toggle the left bit
    u32 left_index = palloc_indexof(left, null) >> order;
    free_page_head[order]->map[left_index >> 3] ^= (1 << (left_index & 7)); // toggle the bit

    // return the page
    return (void*)left;
}


