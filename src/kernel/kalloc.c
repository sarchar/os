#include "common.h"

#include "kalloc.h"
#include "kernel.h"
#include "paging.h"
#include "palloc.h"
#include "stdio.h"

// If a slot in a chunk contains the value KALLOC_MAGIC, then
// the slot is free and the one after it is too, unless you've reached
// the end of the page/associated memory
#define KALLOC_MAGIC 0x1E1EA5A5A5A5E1E1ULL

#define KALLOC_MIN_N 3  // 8 bytes is the smallest allocation unit
//#define KALLOC_MAX_N 9  // 512 bytes is the largest allocation unit
#define KALLOC_MAX_N 16  // 64k is the largest allocation unit

#define KALLOC_VERBOSE 0

// kalloc chunks are just contiguous pages 
struct kalloc_chunk {
    struct kalloc_chunk* prev;
    struct kalloc_chunk* next;
    intp   base;
    u8     order;      // limited by PALLOC_MAX_ORDER
    u8     unused0;    
    u16    free_count; // max of 64k objects. At KALLOC_MIN_N, that requires order 7 pages (128 pages, or 512KiB)
    u32    next_free;  // offset into base where there's free memory
};

struct kalloc_pool {
    struct kalloc_chunk* chunks_full;
    struct kalloc_chunk* chunks_notfull;
    u32    num_full;
    u32    num_notfull;
};

static struct {
    struct kalloc_pool pools[KALLOC_MAX_N - KALLOC_MIN_N + 1]; // 8..512 byte allocations. if you allocate anything not equal to 2^n you're wasting space
    struct kalloc_pool chunk_pool;
} kalloc_data;

static_assert(is_power_of_2(sizeof(struct kalloc_chunk)), "size of kalloc_chunk must be a power of two");

// Increase the size of the chunk pointer pool by specified number of pages (2^page_order)
static void _increase_chunk_ptr_pool(u32 page_order)
{
#if KALLOC_VERBOSE > 1
    fprintf(stderr, "kalloc: _increase_chunk_ptr_pool(page_order=%d)\n", page_order);
#endif

    struct kalloc_chunk* c = (struct kalloc_chunk*)__va_kernel(palloc_claim(page_order)); // this chunk pointer is stored within the memory itself
    c->base = (intp)c;

    // we're going to be storing kalloc_chunks, but the first slot is occupied by this chunk
    c->free_count = (1 << (page_order + 12 - next_power_of_2(sizeof(struct kalloc_chunk)))) - 1;

    // first free slot is immediately after 'c'
    c->next_free = sizeof(struct kalloc_chunk);
    *(u64*)(c->base + c->next_free) = KALLOC_MAGIC;

    // put the new chunk into the notfull list
    c->prev = c->next = null;
    if(kalloc_data.chunk_pool.chunks_notfull != null) {
        kalloc_data.chunk_pool.chunks_notfull->prev = c;
        c->next = kalloc_data.chunk_pool.chunks_notfull;
    }

    kalloc_data.chunk_pool.chunks_notfull = c;
    kalloc_data.chunk_pool.num_notfull++; // number of nodes, not pages or free chunks
}

static void* _next_from_pool(struct kalloc_pool* pool, u32 size)
{
#if KALLOC_VERBOSE > 1
    fprintf(stderr, "kalloc: _next_from_pool(pool=0x%lX, size=%d)\n", (intp)pool, size);
#endif
    assert(pool->chunks_notfull != null, "must increase pool size before calling _next_from_pool");
    struct kalloc_chunk* nfc = pool->chunks_notfull;

    // pointer to the new memory
    void* ret = (void*)(nfc->base + nfc->next_free);

#if KALLOC_VERBOSE > 1
    fprintf(stderr, "kalloc: nfc->base=0x%lX nfc->next_free=%d ret=0x%lX\n", nfc->base, nfc->next_free, ret);
#endif

    // set up next_free
    if(*(u64*)ret == KALLOC_MAGIC) {
        // move linearly in memory, guaranteed not to run out space
        // since free_count must have been nonzero to get here
        assert(nfc->free_count > 0, "told ya");
        nfc->next_free += size;
        if(nfc->free_count > 1) { // only write magic into the next byte if there's actually valid memory there
            *(u64 *)(nfc->base + nfc->next_free) = KALLOC_MAGIC; // be sure to set magic value for the next allocation
        }
    } else {
        // follow the linked list. guaranteed to not run out of memory
        nfc->next_free = *(intp*)ret;
    }

    // decrease count and move to full if necessary
    if(--nfc->free_count == 0) {
        nfc->next->prev = null;
        pool->chunks_notfull = nfc->next;

        nfc->prev = nfc->next = null;
        if(pool->chunks_full != null) {
            pool->chunks_full->prev = nfc;
            nfc->next = pool->chunks_full;
        }

        pool->chunks_full = nfc;
        pool->num_notfull--;
        pool->num_full++;
    }

    // return allocated node
    return ret;
}

static struct kalloc_chunk* _new_chunk_ptr()
{
#if KALLOC_VERBOSE > 1
    fprintf(stderr, "kalloc: _new_chunk_ptr()\n");
#endif

    // make sure there's free chunks available
    struct kalloc_pool* pool = &kalloc_data.chunk_pool;
    if(pool->chunks_notfull == null) _increase_chunk_ptr_pool(1);
    return (struct kalloc_chunk*)_next_from_pool(pool, sizeof(struct kalloc_chunk));
}

// Allocate and initialize a new chunk of 2^page_order pages into pool n
static void _increase_pool(u8 n, u8 page_order)
{
#if KALLOC_VERBOSE > 1
    fprintf(stderr, "kalloc: _increase_pool(n=%d, page_order=%d)\n", n, page_order);
#endif
    assert(n < countof(kalloc_data.pools), "pool index out of range");

    struct kalloc_chunk* c = _new_chunk_ptr();
    void* mem = (void*)__va_kernel(palloc_claim(page_order));
    zero(mem);

    c->base = (intp)mem;
    c->order = page_order;
    c->free_count = 1 << (12 + page_order - (n + KALLOC_MIN_N)); // 4096*2^page_order / 2^n == 2^(12+page_order)/2^n == 2^(12+page_order-n) 

    // first free slot is at the beginning of memory
    c->next_free = 0;
    *(u64*)(c->base + c->next_free) = KALLOC_MAGIC;

    // put the new chunk into the notfull list
    c->prev = c->next = null;
    if(kalloc_data.pools[n].chunks_notfull != null) {
        kalloc_data.pools[n].chunks_notfull->prev = c;
        c->next = kalloc_data.pools[n].chunks_notfull;
    }

    kalloc_data.pools[n].chunks_notfull = c;
    kalloc_data.pools[n].num_notfull++; // number of nodes, not pages or free chunks
}

void kalloc_init()
{
#if KALLOC_VERBOSE > 1
    fprintf(stderr, "kalloc: kalloc_init()\n");
#endif
    zero(&kalloc_data);

    // preinit all the pools
    for(u8 n = 0; n < countof(kalloc_data.pools); n++) {
        _increase_pool(n, 3);
    }
}

void* kalloc(u32 size)
{
    u8 n = next_power_of_2(size);
#if KALLOC_VERBOSE > 1
    fprintf(stderr, "kalloc: kalloc(size=%d), n=%d\n", size, n);
#endif

    u32 wasted = (1 << n) - size;
    unused(wasted); // TODO

    struct kalloc_pool* pool = &kalloc_data.pools[n - KALLOC_MIN_N];
    if(pool->chunks_notfull == null) _increase_pool(n - KALLOC_MIN_N, 3);

    void* ret = (void*)_next_from_pool(pool, (1 << n));
    return ret;
}

// TODO replace this linear search with a red-black tree that contains all the chunks of all the size-N pools
//
// the goal here is to find the chunk that mem belongs to. to get something working, I'm searching all 
// pools and all chunks for where 'mem' belongs and then releasing it. It's not a big deal with my dumpy
// and small kernel, but if it grows into something where performance matters, a tree or some other mechanism
// would be better. it looks like Linux uses a lookup into 'vmem_map' using the page number as an index,
// which might be nice to implement instead of the tree, as well
void kfree(void* mem)
{
    intp memaddr = (intp)mem;

    struct kalloc_pool* pool;
    struct kalloc_chunk* c;
    for(u8 i = 0; i < countof(kalloc_data.pools); i++) {
        pool = &kalloc_data.pools[i];

        // search both notfull and full chunk lists
        c = pool->chunks_notfull;
        while(c != null) {
            if(memaddr >= c->base && memaddr < (c->base + (1 << (12 + c->order)))) break;
            c = c->next;
        }

        if(c != null) break;

        c = pool->chunks_full;
        while(c != null) {
            if(memaddr >= c->base && memaddr < (c->base + (1 << (12 + c->order)))) break;
            c = c->next;
        }

        if(c != null) break;
    }

    assert(c != null, "couldn't find where `mem` belongs");

    // now c is where mem belongs
    *(intp*)mem = c->next_free;
    c->next_free = (intp)mem - c->base;
    
    // check if we're moving from full to notfull
    if(++c->free_count == 1) {
        if(c->next != null) c->next->prev = c->prev;
        if(c->prev != null) c->prev->next = c->next;
        if(c == pool->chunks_full) pool->chunks_full = c->next;

        c->next = pool->chunks_notfull;
        c->prev = null;
        pool->chunks_notfull = c;

        pool->num_notfull++;
        pool->num_full--;
    }
}

