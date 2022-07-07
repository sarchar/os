// palloc - physical page allocator
// A basic implementation of the buddy system, inspired by how Linux (used to?) work, at 
// least around the 2.4-2.6 era.  Lots of information learned from 
// https://hzliu123.github.io/linux-kernel/Physical%20Memory%20Management%20in%20Linux.pdf

#include "common.h"

#include "bootmem.h"
#include "kernel.h"
#include "palloc.h"
#include "stdio.h"

#define PALLOC_VERBOSE 0

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
    u64 block_size = 1 << (order + 12);

    for(u8 i = 0; i < num_regions; i++) {
        // check if not in this region. the block has to be within the region entirely
        if(base_addr < (intp)regions[i].start || (base_addr + block_size) >= (intp)regions[i].end) continue;

        // base_addr is in this region, determine the bitmap block index
        u64 index = base_addr >> (order + 1 + 12);

        // flip the bit
        regions[i].maps[order][index >> 3] ^= 1 << (index & 7);

#if PALLOC_VERBOSE > 1
        fprintf(stderr, "palloc: toggle bit region=$%lX order=%d index=%d base=$%lX new bit=$%02X\n",
                (intp)regions[i].start, order, index, (intp)base, regions[i].maps[order][index >> 3] & (1 << (index & 7)));
#endif

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

#if PALLOC_VERBOSE > 0
        fprintf(stderr, "palloc: reclaiming region at start=$%lX size=%d wasted=%d\n", (intp)region_start, region_size, wasted_alignment);
#endif

        // put in region info
        regions[region_index].start = region_start;
        regions[region_index].size = region_size;
        regions[region_index].end = (void*)((intp)region_start + region_size);
        regions[region_index].npages = region_size >> 12; // this value will be used quite a lot, so a small optimization here

#if PALLOC_VERBOSE > 1
        fprintf(stderr, "palloc: region $%lX has npages=%d\n", (intp)region_start, regions[region_index].npages);
#endif

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

#if PALLOC_VERBOSE > 1
            fprintf(stderr, "palloc: adding block at order=%d address=$%lX size=%llu next=$%lX\n", order, (intp)region_start, region_size, (intp)fp->next);
#endif

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
    if(free_page_head[order]->next != null) free_page_head[order]->next->prev = null;

#if PALLOC_VERBOSE > 0
    fprintf(stderr, "palloc: removed block $%lX at order %d\n", (intp)left, order);

    if(left == 0x7feb6000) {
        assert(left->next != 0x7feb6000, "what");
    }
#endif

    // split blocks all the way down to the requested size, if necessary
    while(order != n) {
        u64 block_size = (1 << order) << 12; // 2^n*4096
    
        // split 'fp' into two block_size/2 blocks
        struct free_page* right = (struct free_page*)((u8*)left + (block_size >> 1));

#if PALLOC_VERBOSE > 0
        fprintf(stderr, "palloc: splitting block order %d address=$%lX right=$%lX\n", order, (intp)left, (intp)right);
#endif

        // immediately toggle the bit out of the order the block comes from, before any splits
        palloc_togglebit(left, order, null);

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

#if PALLOC_VERBOSE > 0
        fprintf(stderr, "palloc: marking block $%lX order %d free (new bit = $%02X) buddy_valid=%d\n", (intp)base, order, bit, (u8)buddy_valid);
#endif

        // a released block with no buddy must stay at this level, otherwise
        // try combining to larger blocks
        if(buddy_valid && (bit == 0) && order < PALLOC_MAX_ORDER - 1) {
#if PALLOC_VERBOSE > 0
            fprintf(stderr, "palloc: combining blocks base=$%lX and buddy=$%lX into order %d\n", (intp)base, (intp)buddy_addr, order + 1);
#endif

            // bit is 0, so buddy must be in the free blocks list
            struct free_page* buddy = (struct free_page*)buddy_addr;

            // remove buddy from free_list
            if(buddy->prev == null) { 
#if PALLOC_VERBOSE > 1
                fprintf(stderr, "free_page_head[%d]->next = $%lX, buddy = $%lX\n", order, free_page_head[order]->next, buddy);
#endif
                assert(free_page_head[order]->next == buddy, "must be the case"); // the only time a node's prev pointer should be null is if it's at the start of the free list
                free_page_head[order]->next = buddy->next;
            } else {
                buddy->prev->next = buddy->next;
            }
            buddy->next->prev = buddy->prev;

            // use the lower address and add try combining in the next higher order
            base = (void*)((intp)base & ~block_size);
            order++;
        } else {
            // not combing with a buddy, so add to current order and break out
            struct free_page* np = (struct free_page*)base;
            np->prev = null;
            assert(free_page_head[order]->next != np, "what4");
            np->next = free_page_head[order]->next;
            if(np->next != null) np->next->prev = np;
            free_page_head[order]->next = np;
            assert(np->next != np, "what3");
            break;
        }
    }
}

