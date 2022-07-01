// Complicated page allocator, just for fun
//
// On a 64-bit system with 4 layer paging, a virtual address can be split as so:
// 
// (in binary) cccccccc cccccccc ffffffff fPPPPPPP PPtttttt tttDDDDD DDDDoooo oooooooo
//
// where,
//
// o - page offset (0-4095)
// D - page table entry (0-511)
// t - page directory entry (0-511)
// P - page directory pointer entry (0-511)
// f - page map level 4 entry (0-511)
// c - canonical all 0s or 1s
//
// Since the upper 16 bits aren't used in paging, this means virtual addresses have an
// effective size of 48 bits, or a total memory space of 256TiB.
//
// A CPU with PAE enabled (required for 64-bit) can support physical addresses up to 52-bit,
// or, an address space of 4PiB.  However, 4PiB divided by 64 several times doesn't end
// up cleaning at 4K pages.  It does if we manage 64PiB, though.  So this page allocator
// will manage a potential physical address space of 64PiB, even though the vast, vast
// majority of processors will have total system ram measured in the gigabytes (as of 2022).
//
// We could assert that we don't support any memory over some number, like 4TiB, to reduce
// the number of bitmask checks. But since I like the idea of supporting all memory, and 
// there are some optimizations in place, that that overhead shouldn't be too bad or even
// noticeable.
//
// So, with one 64-bit integer used as a bitmask, we can divide 64PiB by 64 to get 
// 1PiB slices.  In that bitmask, a 1 represents that chunk of memory having *at least one*
// free system page of ram.  A zero indicates all pages are consumed or not present. So
// on the vast majority of systems, the top level bitmask will almost always be
// 0x0000000000000001, showing that the lowest 1PiB of memory space has some free
// memory. In fact, the next bitmask will likely also be 0x0000000000000001, showing that only
// the lowest 16TiB has RAM.
//
// Each layer will keep a total of how many pages are available in all of the divided memory
// below it.  So if a system has 4GiB of available memory we will have a total tally of 
// 1,048,576 4K pages even at the topmost layer.
//
// Let's assume our system now has 256GiB (67,108,864 4K pages) of free usable RAM and look at 
// how the allocating process works. Let's say we want to allocate T=1GiB of physical
// memory in pages (T/4096 = 262,144 pages). We also assume our data structure has already been
// previously initialized with the free system RAM available.
//
// From our top layer, we see that bit 0 (memory in the first 64TiB) has free pages. From 
// knowing that our top layer has only 1 bit set, we can conclude ALL pages are in that first
// 64TiB, but that information isn't strictly necessary for operation.  We can immediately 
// jump into the first bit we find set to 1, and ask it if it has enough memory 


//  16PiB / 64 = 256TiB slices
// 256TiB / 64 = 4TiB slices
//   4TiB / 64 = 64GiB slices
//  64GiB / 64 = 1GiB slices
//   1GiB / 64 = 16Mib slices
//  16MiB / 64 = 256KiB slices
// 256KiB / 64 = 4K pages          (1 64-bit int of 4K slices = 256KiB of data represented)
//
// 4K slices -> 256KiB slices -> 16MiB -> 1GiB -> 64GiB -> 4TiB -> 256TiB -> 16PiB
//
// 256g / 4096 = 67108864 pages, and for the entire bitmap would require 1048576*8=8388608 bytes
// 256g / 256k = 1048576 slices, and for the entire bitmap would require 16384*8=131072 bytes
// 256g / 16m  = 262144 slices, and for the entire bitmap would require  4096*8=32768 bytes
// 256g / 1g   = 256 slices, and for the entire bitmap would require     4*8=32 bytes
// 256g / 64g  = 4 slices, and for the entire bitmap would require       1*8=8 bytes       (actually requires less than 1 bitmap but 1 is the minimum)
//
// for a total bitmap space requirement of 8,552,488 bytes (approx 00.003% of available space)
//
// Is 8MiB of overhead appropriate for 256GiB of data?
//
// Since each slice divides the world by 64, we need 6 bits out of the address to select a slice.
// So an address of the format:
//
// (in binary) cccccccc ccHHHHHH FFFFFFee eeeeDDDD DDcccccc BBBBBBaa aaaa4444 44444444
//                        256T   4T    64G    1G     16M    256K  page   page offset 
//
// c - must be all zeros (or ignored)
// 4 - the page offset (12 bits, 0-4095)
// a - the page selector (6 bits, 0-63)
// B - the 256K slice index
// c - the 16M slice index
// D - the 1G slice index
// e - the 64G slice index
// etc
//
//
// Finding address A=0x21234CFFE0510:
//
// First mask out 'c': A = A & ~0xFFC0_0000_0000_0000
// Then the topmost 16PiB level has 256TiB slices (each 0-0x0000_FFFF_FFFF_FFFF, or 48 bits) p = (A >> 48) & 0x3F = slice 2   @ addr 0x2000000000000
// Then the        256TiB level has   4TiB slices (each 0-0x0000_03FF_FFFF_FFFF, or 42 bits) p = (A >> 42) & 0x3F = slice 4   @ addr 0x0100000000000
// Then the          4TiB level has  64GiB slices (each 0-0x0000_000F_FFFF_FFFF, or 36 bits) p = (A >> 36) & 0x3F = slice 35  @ addr 0x0023000000000
// Then the         64GiB level has   1GiB slices (each 0-0x0000_0000_3FFF_FFFF, or 30 bits) p = (A >> 30) & 0x3F = slice 19  @ addr 0x00004c0000000
// Then the          1GiB level has  16MiB slices (each 0-0x0000_0000_00FF_FFFF, or 24 bits) p = (A >> 24) & 0x3F = slice 15  @ addr 0x000000f000000
// Then the         16MiB level has 256KiB slices (each 0-0x0000_0000_0003_FFFF, or 18 bits) p = (A >> 18) & 0x3F = slice 63  @ addr 0x0000000fc0000
// Then the        256KiB level has   4KiB pages  (each 0-0x0000_0000_0000_0FFF, or 12 bits) p = (A >> 12) & 0x3F = page  32  @ addr 0x0000000020000
// Then the page offset                                                                                                       @ addr 0x0000000000510
//
//
// Initializing with 256GiB free at address 0x100000000 (so memory will reside in 0x100000000-0x40ffffffff). We can create a path for the start and end
//
// path(0x0100000000) = (0, 0, 0, 4, 0, 0, 0, '0x0') (the rightmost value is the page offset and will always be 0)
// path(0x40ffffffff) = (0, 0, 4, 3, 63, 63, 63, '0xfff') (the rightmost value is the page offset and will always be 0xfff)
//
// Basically, just set the bitmap values to 1 for all values between the two paths
//
// set_bitmask_for_layer:
//      for(int v = start_value; v <= end_value; v++) {
//          bitmask |= (1 << v);
//          process_path_element(next layer)
//      }
//
//  16PiB: 0x00000000_00000001 (free pages = 67108864)
// 256TiB: 0x00000000_00000001 (free pages = 67108864)
//   4TiB: 0x00000000_0000001F (free pages = 67108864) <-- this is the first layer where our memory chunk is larger than the slice size, so multiple bits get set,
//                                                         namely, this shows that the area from 0 -> 320GiB has data

// so now we have four 64GiB bitmasks, each looking the same
//  64GiB: 0x00000000_00000010 (free pages = 67108864)
//

#include "common.h"

#include "kernel.h"
#include "palloc.h"
#include "terminal.h"

// It takes 7 divisions to get down to page level bitmap
// 16PiB -> 256TiB -> 4TiB -> 64GiB -> 1GiB -> 16MiB -> 256KiB -> 4K
#define PALLOC_PATH_LEN 7
#define PALLOC_TOPLEVEL_SLICE_SIZE (1ULL << 48)
#define PALLOC_ADDRESS_FROM_PATH(p) (((p)[0] << 48) | ((p)[1] << 42) | ((p)[2] << 36) | ((p)[3] << 30) | ((p)[4] << 24) | ((p)[5] << 18) | ((p)[6] << 12))


struct palloc_node {
    u64    free_pages;

    // the anonymous union allows this 64-bit storage area to be
    // used as an array of pointers to subnodes or to be used
    // as the final bitmap in the leaf nodes
    union {
        struct palloc_node** slices; // always 64 in length, one for each slice
                                     // null pointer to array means all `free_pages' are available in this layer
                                     // for example, if this layer represents 16MiB, then each slice would be 256KiB.
                                     // but since there are no allocated slices, a full 16MiB (4,096 4K pages) is available
                                     // at the base address this layer represents
        u64 bitmap;  // the set of bits indicating whether the page is free or not
    };
};

static struct palloc_node* palloc_root  = null;
static u8* palloc_internal_memory       = null;
static u64 palloc_internal_memory_start = 0;

struct {
    u32   pages;      // # of pages claimed at the end of the memory section
    u64   allocated;  // # of bytes allocated
    u64   wasted;     // # of bytes wasted due to alignment or large allocations that force claim new pages
} palloc_stats = { 0, };

static inline void _get_path(void* address, u8* path)
{
    intp v = (intp)address;

    // top 10 bits are unused
    path[0] = (v >> 48) & 0x3F; // selects 256TiB slice
    path[1] = (v >> 42) & 0x3F; // selects 4TiB slice
    path[2] = (v >> 36) & 0x3F; // selects 64GiB slice
    path[3] = (v >> 30) & 0x3F; // selects 1GiB slice
    path[4] = (v >> 24) & 0x3F; // selects 16MiB slice
    path[5] = (v >> 18) & 0x3F; // selects 256KiB slice
    path[6] = (v >> 12) & 0x3F; // selects 4K page
}

static inline intp _get_base_address(u8* path)
{
    return ((intp)path[0] << 48) |
           ((intp)path[1] << 42) |
           ((intp)path[2] << 36) |
           ((intp)path[3] << 30) |
           ((intp)path[4] << 24) |
           ((intp)path[5] << 18) |
           ((intp)path[6] << 12);
}

static inline void _print_path(u8* path)
{
    terminal_print_string("palloc: path(");
    for(u8 i = 0; i < PALLOC_PATH_LEN; i++) {
        terminal_print_u8(path[i]);
        if(i != (PALLOC_PATH_LEN - 1)) terminal_putc(L',');
    }
    terminal_print_string(") address = $");
    terminal_print_pointer((void*)_get_base_address(path));
    terminal_putc(L'\n');
}

// initialize path and layer_nodes to point to the first leaf node with at least one page free
static inline bool _init_leaf_search(struct palloc_node** layer_nodes, u8* path, u8* depth)
{
    *depth = 0;
    layer_nodes[0] = palloc_root;

    for(;;) {
        if(*depth == (PALLOC_PATH_LEN - 1)) {
            assert(layer_nodes[*depth]->bitmap != 0, "must not happen"); // leaf node said to have free pages doesn't
            path[*depth] = 0; // not using the first free page within a leaf
            return true;
        } else {
            assert(layer_nodes[*depth]->slices != null, "must not happen"); // node said to have child nodes doesn't

            u8 i;
            for(i = 0; i < 64; i++) {
                // slice needs to exist and have at least one free page
                if(layer_nodes[*depth]->slices[i] == null || layer_nodes[*depth]->slices[i]->free_pages == 0) continue;

                // found a slot with pages available
                path[*depth] = i;
                layer_nodes[*depth+1] = layer_nodes[*depth]->slices[i];

                // next depth
                (*depth)++;
                break;
            }

            assert(i != 64, "must not happen"); // node said to have child nodes doesn't
        }
    }

    return false;
}

static bool _next_slice(u8* path, u8* depth)
{
    // go up to the next slice
    (*depth)--;
    path[*depth]++;
    
    // continue moving up while nodes run out of slices
    while(path[*depth] == 64 && *depth > 0) {
        // have to go up a layer, and back down into the next slice
        path[*depth] = 0; // reset current node's slice to 0 since we're linearly incrementing into the next node
    
        // go up a layer
        (*depth)--;
    
        // increment to the next slice within that layer (which might make us go up another layer!)
        path[*depth]++;
    }

    // return false if increment is done, no more memory
    return (depth != 0 || path[0] != 64);
}

// find the next leaf node with n pages free
static bool _search_leaf_next(struct palloc_node** layer_nodes, u8* path, u8* depth, u8 n)
{
    _next_slice(path, depth);

    // paths have been incremented to point to the next slice, but it may not exist or not have enough
    // pages, so we have to continue that search
    for(;;) {
        if(*depth == (PALLOC_PATH_LEN - 1)) {
            path[*depth] = 0; // don't need to know where the first free page is
            return true;
        } else {
            u8 i;
            for(i = 0; i < 64; i++) {
                // slice needs to exist and have enough pages
                if(layer_nodes[*depth]->slices[i] == null) continue;
                if(layer_nodes[*depth]->slices[i]->free_pages < n) continue;

                // found a slot with pages available
                path[*depth] = i;

                layer_nodes[*depth+1] = layer_nodes[*depth]->slices[i];
                (*depth)++;
                break;
            }

            // this happens if none of the slices within this node had a enough free pages
            // so we have to move to the next slice in the parent layer
            if(i == 64) {
                if(!_next_slice(path, depth)) {
                    assert(false, "no more memory to search");
                }
            }
        }
    }

    return false;
}


// steal some ram from the ram provided, to be used as bitmaps
static inline void* _palloc_steal(void* mem, u64* mem_size, u64 alloc_size, u64 alignment)
{
    assert(alloc_size <= 4096, "cannot allocate more than 1 page at a time");

    if(palloc_internal_memory == null) {
        assert(*mem_size > 4096, "must have at least one page of memory remaining");
        palloc_internal_memory = (u8*)((intp)mem + *mem_size - 4096);
        palloc_internal_memory_start = 0;
        palloc_stats.pages++;

        //terminal_print_string("palloc: stealing one page from provided ram at $");
        //terminal_print_pointer((void*)palloc_internal_memory);
        //terminal_putc(L'\n');

        *mem_size -= 4096;

        // TODO map the page into virtual memory. for now kernel is identity mapped in first 4GiB so we can just write to that memory

        // zero out page
        memset64(palloc_internal_memory, 0, 4096/8);
    }

    // claim another page if there's not enough memory in this one
    u64 old_start = palloc_internal_memory_start;
    palloc_internal_memory_start = (u64)__alignup(palloc_internal_memory_start, alignment);

    palloc_stats.wasted += palloc_internal_memory_start - old_start;

    if((4096 - palloc_internal_memory_start) < alloc_size) {
        palloc_stats.wasted += 4096 - palloc_internal_memory_start;
        palloc_internal_memory = null;

        // try again bye allocating a new page
        return _palloc_steal(mem, mem_size, alloc_size, alignment);
    } else {
        // give them some ram, dude
        void* res = (void*)&palloc_internal_memory[palloc_internal_memory_start];
        palloc_internal_memory_start += alloc_size;

        palloc_stats.allocated += alloc_size;

        // if all memory in the block is claimed, reinit on the next call
        if(palloc_internal_memory_start == 4096) palloc_internal_memory = null;

        return res;
    }
}

static void _palloc_print_stats()
{
    terminal_print_string("palloc: pages = $"); terminal_print_u32(palloc_stats.pages);
    terminal_print_string(" allocated = $"); terminal_print_u64(palloc_stats.allocated);
    terminal_print_string(" wasted = $"); terminal_print_u64(palloc_stats.wasted);
    terminal_putc(L'\n');
}

// from palloc_root, set all the bits in all the necessary bitmaps to 1 that correspond to memory pages in [mem, mem_size).
static inline void _set_free_bits(void* mem, u64 mem_size)
{
    struct palloc_node* layer_nodes[PALLOC_PATH_LEN];

    u8   depth;
    intp base_address;
    u64  slice_size;

    u64  pages_added[PALLOC_PATH_LEN] = { 0, };

    u8 path[PALLOC_PATH_LEN];
    _get_path(mem, path);

    // find starting position in the bitmap tree
    layer_nodes[0] = palloc_root;
    depth = 0;

    // compute the base address first, down to the page, then in the loop update it as we add pages
    base_address = 0;
    slice_size = PALLOC_TOPLEVEL_SLICE_SIZE;
    for(u32 i = 0; i < PALLOC_PATH_LEN; i++) {
        base_address += (path[i] * slice_size); // add the offset into the layer where the slice is found
        slice_size >>= 6; // each lower layer represents 64x less per slice
    }

    //terminal_print_string("end memory address = $"); terminal_print_pointer((void*)((intp)mem + mem_size)); terminal_putc(L'\n');

    // continue setting bits while we have more memory!
    while(true) {

        // start by delving into the depths of the tree
        while(depth < (PALLOC_PATH_LEN - 1)) {                  // the lowest layer is the actual bitmap of 4K pages, so we stop there
            // get slice index
            u8 slice_index = path[depth];

            // TODO determine if the free memory aligns with the base and covers this entire node's worth of memory
            // if it does, add the free pages to and leave 'slices' to be null.

            if(layer_nodes[depth]->slices == null) {
                assert(layer_nodes[depth]->free_pages == 0, "cannot add more memory to the same node");

                // by defining slices, we need each pointer to be valid
                layer_nodes[depth]->slices = (struct palloc_node**)_palloc_steal(mem, &mem_size, sizeof(struct palloc_node*) * 64, 8);
            }

            if(layer_nodes[depth]->slices[slice_index] == null) {
                layer_nodes[depth]->slices[slice_index] = (struct palloc_node*)_palloc_steal(mem, &mem_size, sizeof(struct palloc_node), 8);
            }

            //terminal_print_string("At depth "); terminal_print_u8(depth);
            //terminal_print_string(" fetching slice "); terminal_print_u8(slice_index);
            //terminal_putc(L'\n');

            depth += 1;
            layer_nodes[depth] = layer_nodes[depth-1]->slices[slice_index];
        }

        //terminal_print_string("At depth 06 base = $"); terminal_print_pointer((void*)base_address);
        //terminal_print_string(", page = "); terminal_print_u8(path[depth]);
        //terminal_putc(L'\n');
        //terminal_print_string("Setting bits starting at base = $"); terminal_print_pointer((void*)base_address);
        //terminal_putc(L'\n');

        assert(depth == PALLOC_PATH_LEN - 1, "must be at a leaf node of the tree");

        // start setting bits
        pages_added[depth] = 0;

        // TODO instead of a for loop, calculate min(64, pages_left) and set all bits at once
        while((base_address < ((intp)mem + mem_size)) && (path[depth] != 64)) {
            //terminal_print_string(" marking page "); terminal_print_u8(path[depth]); terminal_print_stringnl(" as free");

            layer_nodes[depth]->bitmap |= (1 << path[depth]);
            pages_added[depth]++;

            // move to the next slice
            base_address += 0x1000;
            path[depth]++;
        }

        // all the above pages were added at the leaf node layer
        layer_nodes[depth]->free_pages += pages_added[depth];

        //terminal_print_string(" base after setting bits = $"); terminal_print_pointer((void*)base_address); terminal_putc(L'\n');

        // get out of loop when done!
        if(base_address >= ((intp)mem + mem_size)) break;

        // reset base address to the beginning of the leaf node
        //base_address -= (64 * slice_size);
        //terminal_print_string(" base at start of leaf node = $"); terminal_print_pointer((void*)base_address); terminal_putc(L'\n');

        // handle overflowing into the next node, which may require traversing up the tree several layers
        while(path[depth] == 64 && depth > 0) {
            // no more in this layer, bail!!
            // have to go up a layer, and back down into the next node
            path[depth] = 0; // reset current node's slice to 0 since we're linearly incrementing into the next node
            //terminal_print_string("^going up from depth "); terminal_print_u8(depth); terminal_putc(L'\n');

            // when we go up a layer, we have to adjust the base_address back to the base of the node above
            // as well as update slice_size
            slice_size <<= 6; // multiply slice size by 64

            // go up a layer
            depth--;

            // increment to the next slice within that layer (which might make us go up another layer!)
            path[depth]++;

            // accumulate the free pages added at the previous layer
            layer_nodes[depth]->free_pages += pages_added[depth + 1];
            //if(depth == 3) {
            //    terminal_print_string("*added "); terminal_print_u32((u32)pages_added[depth+1]); terminal_putc(L'\n');
            //}
            pages_added[depth] += pages_added[depth + 1];
            pages_added[depth + 1] = 0; // reset the accumulator at the lower level so it isn't added again

            // finally adjust the base address to be the base of the current node, -1 for the increment right above
            //base_address -= ((path[depth] - 1) * slice_size);
            //terminal_print_string("^base_address after going up $"); terminal_print_pointer((void*)base_address); terminal_putc(L'\n');

            //terminal_print_string("^path at depth "); terminal_print_u8(depth); terminal_print_string(" now "); terminal_print_u8(path[depth]); terminal_putc(L'\n');
        }

        //terminal_print_string("base_address at end of loop: $"); terminal_print_pointer((void*)base_address); terminal_putc(L'\n');
    }

    // tally up the added pages all the way up the tree
    for(u8 i = depth; i > 0; --i) {
        //terminal_print_string("* at layer "); terminal_print_u8(i-1); terminal_print_string(" added "); terminal_print_u32((u32)pages_added[i]); terminal_putc(L'\n');
        layer_nodes[i - 1]->free_pages += pages_added[i];
        pages_added[i - 1] += pages_added[i];
    }

    terminal_print_string("palloc: free pages = $"); terminal_print_u64(palloc_root->free_pages); terminal_putc(L'\n');

    _palloc_print_stats();
}

void palloc_init(void* init_ram, u64 init_ram_size)
{
    assert(__alignof(init_ram, 4096) == 0, "provided init ram must be page aligned");

    terminal_print_string("palloc: initializing with init_ram=$"); terminal_print_pointer(init_ram);
    terminal_print_string(" size=$"); terminal_print_u64(init_ram_size);
    terminal_putc(L'\n');
    
    // initialize the root layer
    palloc_root = (struct palloc_node*)_palloc_steal(init_ram, &init_ram_size, sizeof(struct palloc_node), 8);

    // set allll the bits making each page free
    _set_free_bits(init_ram, init_ram_size);
#if 0
    // loop from the base address to the end address, setting bits in the bitmap to 1
    // if the base address is aligned with the current slice's base address and it is equal or larger than the entire layer
    // the parent layer can be used instead. In other words, say we're mapping 512KiB and it's aligned to 256KiB boundary
    // then two consecutive bits in the layer above (the 16MiB layer) can indicate the entire 256KiB regions are available.
    
    u64 current_address          = 0;
    u64 current_layer_slice_size = (1 << 48);   // root is 16PiB, divided by 64 = 256TiB

    // assign the number of free pages to the current layer
    palloc_root->free_pages = (init_ram >> 12);

    terminal_print_string("palloc: init base_path: ");
    for(int i = 0; i < PALLOC_PATH_LEN; i++) {
        terminal_print_u8(base_path[i]);
        terminal_print_string(" ");
    }

    terminal_putc(L'\n');
#endif
}

void palloc_add_free_region(void* ram, u64 ram_size)
{
    terminal_print_string("palloc: TODO adding new RAM $"); terminal_print_pointer(ram);
    terminal_print_string(" size=$"); terminal_print_u64(ram_size);
    terminal_print_string("\n");
}

// can only claim up to 64 pages at a time, and the result is guaranteed to be contiguous
void* palloc_claim(u8 n)
{
    assert(n > 0 && n <= 64, "must be in range [1, 64]");

    u8 path[PALLOC_PATH_LEN];
    struct palloc_node* layer_nodes[PALLOC_PATH_LEN];
    u8 depth;

    // initialize the search algorithm
    if(!_init_leaf_search(layer_nodes, path, &depth)) {
        assert(false, "out of memory");
    }

    // find the first leaf node with n pages available
    // _init_leaf_search doesn't scan based on number of requried pages, but the
    // first node with some pages might still satisfy our requirement, otherwise
    // start scanning for the first leaf node with enough pages
    if(layer_nodes[depth]->free_pages < n) {
        _search_leaf_next(layer_nodes, path, &depth, n);
    }
    //terminal_print_string("* palloc: found leaf node with enough pages ($"); terminal_print_u8((u8)layer_nodes[depth]->free_pages); terminal_print_string("):\n");
    //_print_path(path);

    while(true) {
        u64 mask = lmask(n);
        for(u8 i = 0; i <= 64 - n; i++) {
            if((layer_nodes[depth]->bitmap & mask) == mask) { // enough bits at position i
                // mark bits
                layer_nodes[depth]->bitmap &= ~mask;

                // add i offset to base address
                intp base_address = _get_base_address(path) + 0x1000 * i;

                // propagate free pages
                for(s8 i = depth; i >= 0; --i) {
                    layer_nodes[i]->free_pages -= n;
                }

                // return physical address
                return (void*)base_address;
            } else {
                mask <<= 1;
            }
        }

        // not enough bits found in this leaf node, go to the next leaf node with 
        if(!_search_leaf_next(layer_nodes, path, &depth, n)) {
            assert(false, "out of physical memory");
        }
        //terminal_print_string("* palloc: found leaf node with enough pages ($"); terminal_print_u8((u8)layer_nodes[depth]->free_pages); terminal_print_string("):\n");
        //_print_path(path);
    }

    return null;
}

