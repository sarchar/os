#include "common.h"

#include "kalloc.h"
#include "kernel.h"
#include "paging.h"
#include "palloc.h"
#include "smp.h"
#include "stdio.h"

// If a slot in a chunk contains the value KALLOC_MAGIC, then
// the slot is free and the one after it is too, unless you've reached
// the end of the page/associated memory
#define KALLOC_MAGIC 0x1E1EA5A5A5A5E1E1ULL

#define KALLOC_MIN_N 4   // 16 bytes is the smallest allocation unit
#define KALLOC_MAX_N 12  // 4096 bytes is the largest allocation unit

#define KALLOC_VERBOSE 0

struct kalloc_pool {
    intp   next_free;
    u32    num_free;
    u32    num_alloc;
    struct ticketlock lock;
};

static struct kalloc_pool kalloc_pools[KALLOC_MAX_N - KALLOC_MIN_N + 1]; // allocation pools. if you allocate anything not equal to 2^n you're wasting space

static u8 pool_to_order[KALLOC_MAX_N - KALLOC_MIN_N + 1] = { 0, 0, 1, 2, 3, 4, 5 };

// Allocate and initialize a new chunk of 2^page_order pages into pool n
static void _increase_pool(u8 n, u8 page_order)
{
    assert(n < countof(kalloc_pools), "pool index out of range");
    struct kalloc_pool* pool = &kalloc_pools[n];
    assert(pool->num_free == 0, "only increase pool when no more objects exist");

#if KALLOC_VERBOSE > 0
    fprintf(stderr, "kalloc: _increase_pool(pool=0x%lX, n=%d (2^%d), page_order=%d)\n", pool, n, n+KALLOC_MIN_N, page_order);
#endif

    intp mem = palloc_claim(page_order);
    zero((u8*)mem);

    u32 added_count = 1 << (PAGE_SHIFT + page_order - (n + KALLOC_MIN_N)); // 4096*2^page_order / 2^n == 2^(12+page_order)/2^n == 2^(12+page_order-n) 

    // first free slot is at the beginning of memory
    pool->next_free = mem;

    // make the data at next_free use KALLOC_MAGIC series
    *(u64*)pool->next_free = KALLOC_MAGIC;

    pool->num_free = added_count; // set the total # of free/available objects

#if KALLOC_VERBOSE > 1
    fprintf(stderr, "kalloc: increased pool by %d objects, next_free=0x%lX *next_free=0x%lX\n", added_count, pool->next_free, *(u64*)pool->next_free);
#endif
}

static intp _next_from_pool(u8 n)
{
    struct kalloc_pool* pool = &kalloc_pools[n - KALLOC_MIN_N];
    u32 size = 1 << n;

#if KALLOC_VERBOSE > 0
    fprintf(stderr, "kalloc: _next_from_pool(pool=0x%lX (2^%d), size=%d, num_free=%d, next_free=0x%lX)\n", pool, n, size, pool->num_free, pool->next_free);
#endif

    // in order for KALLOC_MAGIC to work, pools are only increased in size when there's 0 objects left
    if(pool->num_free == 0) _increase_pool(n - KALLOC_MIN_N, pool_to_order[n - KALLOC_MIN_N]);
    assert(pool->num_free != 0, "_increase_pool failed");

    // pointer to the new memory
    intp ret = pool->next_free;

    // set up next_free
    if(*(u64*)ret == KALLOC_MAGIC) {
        // move linearly in memory, guaranteed not to run out space
        // since num_free must have been nonzero to get here
        assert(pool->num_free > 0, "told ya");
        pool->next_free += size; // KALLOC_MAGIC lets us move linearly in the block of memory
        if(pool->num_free > 1) { // only write magic into the next byte if there's actually valid memory there
            *(u64 *)pool->next_free = KALLOC_MAGIC; // be sure to set magic value for the next allocation
        }
    } else {
        // follow the linked list. guaranteed to not run out of memory
        pool->next_free = *(intp*)ret;
    }

    // decrease count and move to full if necessary
    pool->num_free--;
    pool->num_alloc++;

    // return allocated node
    return ret;
}

void kalloc_init()
{
    declare_ticketlock(lock_init);

#if KALLOC_VERBOSE > 0
    fprintf(stderr, "kalloc: kalloc_init()\n");
#endif
    zero(kalloc_pools);

    // preinit all the pools
    for(u8 n = 0; n < countof(kalloc_pools); n++) {
        kalloc_pools[n].lock = lock_init;
        acquire_lock(kalloc_pools[n].lock);
        _increase_pool(n, pool_to_order[n]);
        release_lock(kalloc_pools[n].lock);
    }
}

void* kalloc(u32 size)
{
    u8 n = next_power_of_2(size);
    n = max(KALLOC_MIN_N, n); // set a minimum value of KALLOC_MIN_N

    assert(n <= KALLOC_MAX_N, "allocation too large");

#if KALLOC_VERBOSE > 0
    fprintf(stderr, "kalloc: kalloc(size=%d), n=%d\n", size, n);
#endif

    u32 wasted = (1 << n) - size;
    unused(wasted); // TODO

    struct kalloc_pool* pool = &kalloc_pools[n - KALLOC_MIN_N];
    acquire_lock(pool->lock); // lock only the pool involved

    void* ret = (void*)_next_from_pool(n);
    release_lock(pool->lock);

#if KALLOC_VERBOSE > 1
    fprintf(stderr, "kalloc: kalloc(size=%d) ret=0x%lX\n", size, ret);
#endif
    return ret;
}

void kfree(void* mem, u32 size)
{
    // determine the pool
    u8 n = next_power_of_2(size);
    n = max(KALLOC_MIN_N, n); // set a minimum value of KALLOC_MIN_N
    assert(n <= KALLOC_MAX_N, "allocation too large");
    struct kalloc_pool* pool = &kalloc_pools[n - KALLOC_MIN_N];

    acquire_lock(pool->lock);
    intp old_next_free = pool->next_free;
    pool->next_free = (intp)mem;
    *(u64*)mem = old_next_free;
    pool->num_free++;
    pool->num_alloc--;
    release_lock(pool->lock);
}

