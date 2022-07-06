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
    struct free_page* prev;
};

static struct free_page* free_page_head[PALLOC_MAX_ORDER] = { null, };

// region data is required to determine page index for the bitmaps
// maps are stored in this region struction, one for each order 
// we need the start of a region to be index 0 in every map
// TODO in the long run it would be nice to have a single bitmap for all pages, per order
struct region {
    void* start;
    void* end;
    u64   size;
    u64   npages;
    u8*   maps[PALLOC_MAX_ORDER-1]; // highest order doesn't need a map
};

static struct region* regions;
static u8 num_regions;

// if nb is not null, set nb to non-zero if the bit in the bitmap was set, 0 otherwise
// returns false if the address of the page isn't managed by palloc (i.e., doesn't
// fit in any region)
static inline bool palloc_togglebit(struct free_page* base, u8 order, u8* nb)
{
    intp base_addr = (intp)base;

    for(u8 i = 0; i < num_regions; i++) {
        // check if not in this region
        if(base_addr < (intp)regions[i].start || base_addr >= (intp)regions[i].end) continue;

        // base_addr is in this region, determine the bitmap block index
        u64 index = base_addr >> (order + 1 + 12);

        // flip the bit
        regions[i].maps[order][index >> 3] ^= 1 << (index & 7);
        //terminal_print_string("palloc: toggle bit region=$"); terminal_print_pointer((void*)regions[i].start);
        //terminal_print_string(" order="); terminal_print_u8(order);
        //terminal_print_string(" index="); terminal_print_u32(index);
        //terminal_print_string(" base="); terminal_print_pointer((void*)base);
        //terminal_print_string(" new bit="); terminal_print_u8(regions[i].maps[order][index >> 3] & (1 << (index & 7)));
        //terminal_putc(L'\n');

        // and get the new value
        if(nb != null) *nb = regions[i].maps[order][index >> 3] & (1 << (index & 7));
        
        // done
        return true;
    }

    return false;
}

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

    // have to loop over all regions and build bitmaps for them individually
    u8 num_bootmem_regions = bootmem_num_regions();
    for(u8 r = 0; r < num_bootmem_regions; r++) {
        u32 npages = bootmem_get_region_size(r) >> 12;

        // allocate the bitmaps
        for(u32 order = 0; order < PALLOC_MAX_ORDER - 1; ++order) {
            u64 mapsize = ((npages >> (order + 1)) + 7) >> 3; // divide pages by 2^(order layer, plus 1 because of the buddy), then divide by sizeof(byte) rounded up
            regions[r].maps[order] = (u8*)bootmem_alloc(mapsize, 8);
            memset(regions[r].maps[order], 0, mapsize);
        }
    }

    // and now that we have a bitmaps, we can start reclaiming bootmem
    void* region_start;
    u64 region_size;
    u8 region_index = 0;

    while((region_size = bootmem_reclaim_region(&region_start)) != 0) {
        // alignup
        u16 wasted_alignment = (intp)__alignup(region_start, 4096) - (intp)region_start;
        region_start = __alignup(region_start, 4096);
        region_size -= wasted_alignment;

        //terminal_print_string("palloc: reclaiming region at start="); terminal_print_pointer(region_start);
        //terminal_print_string(" size=$"); terminal_print_u32(region_size);
        //terminal_print_string(" wasted=$"); terminal_print_u16(wasted_alignment);
        //terminal_putc(L'\n');

        // put in region info
        regions[region_index].start = region_start;
        regions[region_index].size = region_size;
        regions[region_index].end = (void*)((intp)region_start + region_size);
        regions[region_index].npages = region_size >> 12; // this value will be used quite a lot, so a small optimization here

        terminal_print_string("palloc: region $"); terminal_print_pointer(region_start);
        terminal_print_string(" has npages="); terminal_print_u32(regions[region_index].npages);
        terminal_putc(L'\n');

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
            fp->prev = null; // doesn't actually point back to free_page_head[order], since that's not a block of pages
            if(fp->next != null) {
                fp->next->prev = fp;
            }
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
    free_page_head[order]->next = left->next;
    free_page_head[order]->next->prev = null;

    // split blocks all the way down to the requested size, if necessary
    while(order != n) {
        u64 block_size = (1 << order) << 12; // 2^n*4096
    
        // split 'fp' into two block_size/2 blocks
        struct free_page* right = (struct free_page*)((u8*)left + (block_size >> 1));
        zero(right);

        terminal_print_string("palloc: splitting block order $"); terminal_print_u8(order);
        terminal_print_string(" address=$"); terminal_print_pointer((void*)left);
        terminal_print_string(" right=$"); terminal_print_pointer((void*)right);
        terminal_putc(L'\n');

        // immediately toggle the bit out of the order the block comes from, before any splits
        palloc_togglebit(left, order, null);

        //terminal_print_string("* right address="); terminal_print_pointer((void*)right);
        //terminal_putc(L'\n');
        assert(((intp)left ^ (1 << ((order - 1) + 12))) == (intp)right, "verifying buddy address");

        // add the right node to the free page list at the lower order. the bitmap bit will be 
        // toggled below/next time through the loop
        right->next = free_page_head[order - 1]->next;
        right->prev = null; // doesn't actually point back to free_page_head[order], since that's not an actual block of pages
        if(right->next != null) right->next->prev = right;
        free_page_head[order - 1]->next = right;

        // and continue dividing 'left' if necessary
        --order;
    }

    // toggle the left bit
    palloc_togglebit(left, order, null);

    // return the page
    return (void*)left;
}

void palloc_abandon(void* base, u8 n)
{
    assert(n < PALLOC_MAX_ORDER, "n must be a valid order size");

    // working order
    u8 order = n;

    while(true) {
        // start by determining if the buddy is available or not
        u64 block_size = 1 << (order + 12); // 2^n*4096
        void* buddy_addr = (void*)((intp)base ^ block_size); // toggle the bit to get the buddy address

        // buddy_addr and 'base' both have the same bitmap index
        // but we try to set buddy_addr first (most of the time it succeeds),
        // but if buddy_addr isn't valid, then flip the bit on base and return
        u8 bit;
        bool buddy_valid;
        if(!(buddy_valid = palloc_togglebit(buddy_addr, order, &bit))) {
            palloc_togglebit(base, order, &bit);
        }

        terminal_print_string("palloc: marking block $"); terminal_print_pointer((void*)base);
        terminal_print_string(" order $"); terminal_print_u8(order);
        terminal_print_string(" free (new bit = "); terminal_print_u8(bit);
        terminal_print_string(") buddy_valid = "); terminal_print_u8((u8)buddy_valid);
        terminal_print_string("\n");

        // a released block with no buddy must stay at this level, otherwise
        // try combining to larger blocks
        if(buddy_valid && (bit == 0) && order < PALLOC_MAX_ORDER - 1) {
            terminal_print_string("palloc: combining blocks $"); terminal_print_pointer((void*)buddy_addr);
            terminal_print_string(" and $"); terminal_print_pointer(base);
            terminal_print_string(" into order $"); terminal_print_u8(order + 1);
            terminal_putc(L'\n');

            // bit is 0, so both blocks must be free
            struct free_page* buddy = (struct free_page*)buddy_addr;

            // remove buddy from free_list
            if(buddy->prev == null) {
                assert(free_page_head[order]->next == buddy, "must be the case");
                free_page_head[order]->next = buddy->next;
            } else {
                buddy->prev->next = buddy->next;
            }
            buddy->next->prev = buddy->prev;

            // use the lower address and add it to the higher layer, then repeat the process
            struct free_page* combined = (struct free_page*)((intp)base & ~block_size);
            combined->prev = null;
            combined->next = free_page_head[order + 1]->next;
            combined->next->prev = combined;
            free_page_head[order + 1]->next = combined;

            // try combining again one order higher
            order++;
            base = (void*)combined;
        } else {
            // not combing with a buddy, so add to current order and break out
            struct free_page* np = (struct free_page*)base;
            np->prev = null;
            np->next = free_page_head[order]->next;
            free_page_head[order]->next = np;
            break;
        }
    }
}

