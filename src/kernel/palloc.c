// palloc - physical page allocator
// A basic implementation of the buddy system, inspired by how Linux (used to?) work, at 
// least around the 2.4-2.6 era.  Lots of information learned from 
// https://hzliu123.github.io/linux-kernel/Physical%20Memory%20Management%20in%20Linux.pdf

#include "common.h"

#include "bootmem.h"
#include "kernel.h"
#include "multiboot2.h"
#include "palloc.h"
#include "paging.h"
#include "smp.h"
#include "stdio.h"
#include "string.h"

#define PALLOC_VERBOSE 0

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
    intp start;
    u64  size;
    u64  npages;
    u8*  maps[PALLOC_MAX_ORDER-1]; // highest order doesn't need a map
};

static struct region* regions;
static u8 num_highmem_regions;
static u8 num_bootmem_regions;
static u8 num_regions;

// if nb is not null, set nb to non-zero if the bit in the bitmap was set, 0 otherwise
// returns false if the address of the page isn't managed by palloc (i.e., doesn't
// fit in any region)
static inline bool palloc_togglebit(struct free_page* base, u8 order, u8* nb)
{
    intp base_addr = (intp)base;
    u64 block_size = 1 << (order + PAGE_SHIFT);

    for(u8 i = 0; i < num_regions; i++) {
        // check if not in this region. the block has to be within the region entirely
        if(base_addr < regions[i].start || (base_addr + block_size) >= (regions[i].start + regions[i].size)) continue;

        // base_addr is in this region, determine the bitmap block index
        // since region start could start in the middle of the left buddy, align down to get the
        // the index calculation correct
        u64 aligned_region_start = regions[i].start & ~((1 << (order + 1 + PAGE_SHIFT)) - 1);
        u64 index = (base_addr - aligned_region_start) >> (order + 1 + PAGE_SHIFT);

        // flip the bit
        regions[i].maps[order][index >> 3] ^= 1 << (index & 7);

#if PALLOC_VERBOSE > 3
        fprintf(stderr, "palloc: toggle bit region=$%lX order=%d index=%d base=$%lX new bit=$%02X\n",
                regions[i].start, order, index, (intp)base, regions[i].maps[order][index >> 3] & (1 << (index & 7)));
#endif

        // and get the new value
        if(nb != null) *nb = regions[i].maps[order][index >> 3] & (1 << (index & 7));
        
        // done
        return true;
    }

    return false;
}

static void _initialize_region(struct region* r, intp region_start, u64 region_size)
{
    // alignup
    u16 wasted_alignment = (intp)__alignup(region_start, 4096) - (intp)region_start;
    region_start = (intp)__alignup(region_start, 4096);
    region_size -= wasted_alignment;

#if PALLOC_VERBOSE > 0
    fprintf(stderr, "palloc: reclaiming region at start=$%lX size=%d wasted=%d\n", region_start, region_size, wasted_alignment);
#endif

    // put in region info
    r->start  = region_start;
    r->size   = region_size;
    r->npages = region_size >> PAGE_SHIFT; // this value will be used quite a lot, so a small optimization here

#if PALLOC_VERBOSE > 1
    fprintf(stderr, "palloc: region 0x%lX has npages=%d end=0x%lX\n", region_start, r->npages, region_start + r->size);
#endif

    // now add the region to free_page_head
    while(region_size != 0) {
        u8 order = PALLOC_MAX_ORDER - 1;
        u32 block_size = 1 << (order + PAGE_SHIFT);

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

#if PALLOC_VERBOSE > 4
        fprintf(stderr, "palloc: adding block at order=%d address=$%lX size=%llu next=$%lX\n", order, (intp)region_start, region_size, (intp)fp->next);
#endif

        // increment region_start and reduce region_size
        region_start = (intp)region_start + block_size;
        region_size -= block_size;
    }
}


void palloc_init()
{
    // allocate storage for our free_page_head pointers
    for(u8 i = 0; i < PALLOC_MAX_ORDER; i++) {
        free_page_head[i] = (struct free_page*)bootmem_alloc(sizeof(struct free_page), 8);
        zero(free_page_head[i]);
    }

    // later, we will be adding high memory regions to palloc, but as of right now, they aren't available
    // in the kernel's page table. so we cound them up so that we can allocate storage for the region info
    // that will be filled in later.
    intp region_start;
    u64  region_size;
    u8   region_type;
    num_highmem_regions = 0;
    while((region_start = multiboot2_mmap_next_free_region(&region_size, &region_type)) != (intp)-1) {
        if(region_type == MULTIBOOT_REGION_TYPE_AVAILABLE && region_start >= 0x100000000) num_highmem_regions++;
    }

    // create storage for the region info
    num_bootmem_regions = bootmem_num_regions();
    num_regions = num_bootmem_regions + num_highmem_regions;
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
    // Also, all the bitmap sizes are halved due to the buddy system
    //

    // have to loop over only bootmem regions and build bitmaps for them individually
    for(u8 r = 0; r < num_bootmem_regions; r++) {
        u32 npages = bootmem_get_region_size(r) >> PAGE_SHIFT;

        // allocate the bitmaps
        for(u32 order = 0; order < PALLOC_MAX_ORDER - 1; ++order) {
            u64 mapsize = ((npages >> (order + 1)) + 7) >> 3; // divide pages by 2^(order layer, plus 1 because of the buddy), then divide by sizeof(byte) in bits, rounded up
            regions[r].maps[order] = (u8*)bootmem_alloc(mapsize, 8);
            memset(regions[r].maps[order], 0, mapsize);
        }
    }

    // and now that we have a bitmaps, we can start reclaiming bootmem
    u8 region_index = 0;

    while((region_size = bootmem_reclaim_region(&region_start)) != 0) {
        _initialize_region(&regions[region_index], region_start, region_size);
        region_index++;
    }
}

void palloc_init_highmem()
{
    // now we can finally add highmem to palloc. region structures were already allocated, now we just have to initialize them
    intp region_start;
    u64  region_size;
    u8   region_type;
    u8   region_index = num_bootmem_regions; // highmem regions are right after the bootmem ones
    while((region_start = multiboot2_mmap_next_free_region(&region_size, &region_type)) != (intp)-1) {
        if(region_type != MULTIBOOT_REGION_TYPE_AVAILABLE || region_start < 0x100000000) continue;

        // for now, we assume that all high memory is mapped by the hardware under 64TiB
        assert((region_start + region_size) <= 0x0000400000000000UL, "only support hardware with highmem positioned under 64TiB");

        // we have to allocate the bitmaps first, directly in the region itself
        u32 npages = region_size >> PAGE_SHIFT;

        for(u32 order = 0; order < PALLOC_MAX_ORDER - 1; ++order) {
            u64 mapsize = ((npages >> (order + 1)) + 7) >> 3; // divide pages by 2^(order layer, plus 1 because of the buddy), then divide by sizeof(byte) rounded up
            regions[region_index].maps[order] = (u8*)__alignup(region_start, 8); // map pointer needs to be 8 byte aligned
            memset(regions[region_index].maps[order], 0, mapsize);

            // reduce the remaining size of the region (_initialize_region will round up any non-page boundary, which just becomes unusable memory)
            // all maps need to be 8 byte aligned
            u8 wasted = (intp)regions[region_index].maps[order] - (intp)region_start;
            region_start += (mapsize + wasted);
            region_size -= (mapsize + wasted);
        }

#if PALLOC_VERBOSE > 0
        fprintf(stderr, "palloc: adding high mem region 0x%lX size=%d\n", region_start, region_size);
#endif
        _initialize_region(&regions[region_index], region_start, region_size);
        region_index++;
    }

    assert(region_index == num_regions, "we should have all the regions now. why not?");
}

declare_ticketlock(palloc_lock);

intp palloc_claim(u8 n) // allocate 2^n pages
{
    assert(n < PALLOC_MAX_ORDER, "n must be a valid order size");

    // working order
    u8 order = n;

    // we need a lock to be threadsafe
    acquire_lock(palloc_lock);

    // find first order >= requested size with free blocks
    while(order < PALLOC_MAX_ORDER && free_page_head[order]->next == null) order++;

    // out of memory?
    if(order == PALLOC_MAX_ORDER) {
        release_lock(palloc_lock);
        return 0;
    }

    // remove the head from the current order
    struct free_page* left = free_page_head[order]->next;
    free_page_head[order]->next = left->next;
    if(free_page_head[order]->next != null) free_page_head[order]->next->prev = null;

#if PALLOC_VERBOSE > 1
    fprintf(stderr, "palloc: removed block $%lX at order %d (new free_page_head[order]->next = 0x%lX)\n", (intp)left, order, free_page_head[order]->next);
#endif

    // split blocks all the way down to the requested size, if necessary
    while(order != n) {
        u64 block_size = 1 << (order + PAGE_SHIFT); // 2^n*4096
    
        // split 'fp' into two block_size/2 blocks
        struct free_page* right = (struct free_page*)((u8*)left + (block_size >> 1));
        assert(((intp)left ^ (1 << ((order - 1) + PAGE_SHIFT))) == (intp)right, "verifying buddy address");

        // immediately toggle the bit out of the order the block comes from, before any splits
        u8 bit;
        palloc_togglebit(left, order, &bit);

#if PALLOC_VERBOSE > 2
        fprintf(stderr, "palloc: splitting block order %d left=$%lX right=$%lX (new bit = $%02X)\n", order, (intp)left, (intp)right, bit);
#else
        unused(bit);
#endif

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
    u8 bit;
    bool buddy_valid = palloc_togglebit(left, order, &bit);

#if PALLOC_VERBOSE > 1
    fprintf(stderr, "palloc: marked block $%lX order %d used (new bit = $%02X) buddy_valid=%d\n", left, order, bit, (u8)buddy_valid);
#else
    unused(bit);
    unused(buddy_valid);
#endif


    // release lock
    release_lock(palloc_lock);

    // return the page
    return (intp)left;
}

void palloc_abandon(intp base, u8 n)
{
    assert(n < PALLOC_MAX_ORDER, "n must be a valid order size");

    // working order
    u8 order = n;

    // claim palloc lock
    acquire_lock(palloc_lock);

    while(true) {
        // start by determining if the buddy is available or not
        u64 block_size = 1 << (order + PAGE_SHIFT); // 2^n*4096
        intp buddy_addr = base ^ block_size; // toggle the bit to get the buddy address

        // buddy_addr and 'base' both have the same bitmap index for palloc_togglebit,
        // so we try to set buddy_addr first (most of the time it succeeds),
        // but if buddy_addr isn't valid, then flip the bit on base instead. now we know
        // if there's a valid buddy to merge
        u8 bit;
        bool buddy_valid;
        if(!(buddy_valid = palloc_togglebit((struct free_page*)buddy_addr, order, &bit))) {
            palloc_togglebit((struct free_page*)base, order, &bit);
        }

#if PALLOC_VERBOSE > 1
        fprintf(stderr, "palloc: marking block $%lX order %d free (new bit = $%02X) buddy_valid=%d\n", base, order, bit, (u8)buddy_valid);
#endif

        // a released block with no buddy must stay at this level, otherwise try combining to larger blocks
        if(buddy_valid && (bit == 0) && order < PALLOC_MAX_ORDER - 1) {
#if PALLOC_VERBOSE > 2
            fprintf(stderr, "palloc: combining blocks base=$%lX and buddy=$%lX into order %d\n", base, buddy_addr, order + 1);
#endif

            // bit is 0, so buddy must be in the free blocks list
            struct free_page* buddy = (struct free_page*)buddy_addr;

            // remove buddy from free_list
            if(buddy->prev == null) { 
#if PALLOC_VERBOSE > 2
                fprintf(stderr, "free_page_head[%d]->next = $%lX, buddy = $%lX\n", order, free_page_head[order]->next, buddy);
#endif
                assert(free_page_head[order]->next == buddy, "must be the case"); // the only time a node's prev pointer should be null is if it's at the start of the free list
                free_page_head[order]->next = buddy->next;
            } else {
                buddy->prev->next = buddy->next;
            }
            buddy->next->prev = buddy->prev;

            // use the lower address and add try combining in the next higher order
            base &= ~block_size;
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

    release_lock(palloc_lock);
}

