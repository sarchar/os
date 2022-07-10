#include "common.h"

#include "cpu.h"
#include "kernel.h"
#include "paging.h"
#include "palloc.h"
#include "stdio.h"

#define CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT   (1 << 0)
#define CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE (1 << 1)
#define CPU_PAGE_TABLE_ENTRY_FLAG_HUGE      (1 << 7)

// bits 51-63 are not address, and the physical address must be 2MiB aligned
#define CPU_PAGE_TABLE_ADDRESS_MASK_1GB     0x0007FFFFC0000000
#define CPU_PAGE_TABLE_ADDRESS_MASK_2MB     0x0007FFFFFFE00000
#define CPU_PAGE_TABLE_ADDRESS_MASK_4KB     0x0007FFFFFFFFFC00

struct page_table_allocator_page {
    struct page_table_allocator_page* prev;
    struct page_table_allocator_page* next;
    u8  free_count;
    u8  unused0;
    u16 unused1;
    u32 unused2;
    u64 unused3;
    u64 unused4;
    u64 unused5;
    // bitmap must be the last entry in this struct
    u64 bitmap[]; // always (4096-2*sizeof(struct page_table_allocator_page*))/sizeof(struct page_table) bits
};

struct page_table_allocator {
    struct page_table_allocator_page* allocator_head;
    u32 npages;
};

struct page_table {
    u64* _cpu_table;               // always one page, and page aligned. this is the table sent to the cpu
    struct page_table** entries;   // always 512 entries, might as well be page aligned since it takes 4096 bytes
    struct page_table_allocator_page* alloc_page; // pointer to the alloc page this page table data resides in
    u64 flags;                     // TODO fun flaggies
};

#define ALLOCATOR_BITMAP_COUNT     ((4096 - sizeof(struct page_table_allocator_page)) / sizeof(struct page_table))
#define ALLOCATOR_BITMAP_SIZE      ((ALLOCATOR_BITMAP_COUNT + 63) / 64)
#define ALLOCATOR_BITMAP_INDEX(p)  (((intp)(p) & 0x0FFF) >> 5)
#define ALLOCATOR_PTR(node,index)  (struct page_table*)((intp)(node) + sizeof(struct page_table_allocator_page) + ALLOCATOR_BITMAP_SIZE + (index)*sizeof(struct page_table))

static struct page_table_allocator _allocator;
static struct page_table* paging_root;

static void _map_2mb(intp phys, intp virt);

static void _allocator_init()
{
    fprintf(stderr, "paging: ALLOCATOR_BITMAP_COUNT = %d\n", ALLOCATOR_BITMAP_COUNT);
    fprintf(stderr, "paging: ALLOCATOR_BITMAP_SIZE = %d\n", ALLOCATOR_BITMAP_SIZE);
    fprintf(stderr, "paging: ALLOCATOR_PTR(null,0) = 0x%lX\n", ALLOCATOR_PTR(null, 0));

    _allocator.allocator_head = (struct page_table_allocator_page*)__va_kernel(palloc_claim_one());
    fprintf(stderr, "paging: allocator_head = 0x%lX\n", _allocator.allocator_head);
    memset64(_allocator.allocator_head, 0, 512); // zero out the page
    _allocator.allocator_head->free_count = ALLOCATOR_BITMAP_COUNT;
    _allocator.npages = 1;
}

static struct page_table_allocator_page* _allocate_new_head()
{
    struct page_table_allocator_page* new_head = (struct page_table_allocator_page*)__va_kernel(palloc_claim_one());
    memset64(new_head, 0, 512); // zero out the page
    new_head->free_count = ALLOCATOR_BITMAP_COUNT;
    _allocator.allocator_head->prev = new_head;
    new_head->next = _allocator.allocator_head;
    _allocator.allocator_head = new_head;
    _allocator.npages += 1;
    return new_head;
}

static struct page_table* _allocate_page_table()
{
    struct page_table_allocator_page* cur = _allocator.allocator_head;

    // allocate a new set of page table pointer storage if necessary
    if(cur->free_count == 0) cur = _allocate_new_head();

    // guaranteed to have a free slot in this node somewhere
    for(u8 i = 0; i < ALLOCATOR_BITMAP_SIZE; i++) {
        if(cur->bitmap[i] == (u64)-1) continue;

        // would it matter to search byte by byte?
        // generally the bits will be taken up in order, so each successive call
        // will take longer and longer TODO
        for(u8 j = 0; j < 64; j++) {
            if((cur->bitmap[i] & (1 << j)) == 0) {
                cur->bitmap[i] |= (1 << j);
                cur->free_count -= 1;

                struct page_table* pt = ALLOCATOR_PTR(cur, i * 64 + j);
                pt->_cpu_table = (u64*)__va_kernel(palloc_claim_one());
                pt->entries    = (struct page_table**)__va_kernel(palloc_claim_one());
                pt->alloc_page = cur;
                pt->flags      = 0;

                memset64(pt->_cpu_table, 0, 512);
                memset64(pt->entries, 0, 512);

                return pt;
            }
        }
    }

    assert(false, "don't you get here");
    return null;
}

void paging_init()
{
    _allocator_init();

    // TODO build a new page table using memory allocated from palloc and switch to it
    // paging_root is the level 4 page table
    paging_root = _allocate_page_table();
    fprintf(stderr, "paging: initializing page tables (paging_root->_cpu_table=0x%lX)\n", paging_root->_cpu_table);

    // identity map 0-4GB into both virtual address 0 and kernel address space using 2MiB huge pages
    for(intp base = 0; base < 0x100000000; base += 0x200000) {
        _map_2mb(base, base);
        _map_2mb(base, base | (intp)&_kernel_vma_base);
    }

    // and change cr3 to set the new page tables
    // setting cr3 forces a full tlb invalidate
    __wrcr3((u64)__pa_kernel(paging_root->_cpu_table));

    // testing
    u8* physpage = (u8*)palloc_claim_one();
    physpage[1000] = 0x55; // we have identity mapping on all memory
    physpage[1001] = 0xAA;
    paging_map_page((intp)physpage, 0x3F00000000);
    assert(((u8*)0x3F00000000)[1000] == 0x55, "55 is wrong");
    assert(((u8*)0x3F00000000)[1001] == 0xAA, "AA is wrong");
    return;

}

// _map_page does the hard work of mapping a physical address for the specified virtual address
// location, but does not flush the TLB. For TLB flush, call paging_map_page() instead.
static void _map_page(intp phys, intp virt)
{
    assert((virt >> 47) == 0 || (virt >> 47) == 0x1FFFF, "virtual address must be canonical");
    assert(__alignof(virt, 4096) == 0, "virtual address must be 4KB aligned");
    assert(__alignof(phys, 4096) == 0, "physical address must be 4KB aligned");

    fprintf(stderr, "paging: mapping page at 0x%08lX to virtual address 0x%08lX\n", phys, virt);

    // shift right 12 for pt1
    // shift right 21 for pt2
    // shift right 30 for pt3
    // shift right 39 for pt4

    // create a level 3 page table (page directory pointers) if necessary
    u32 pml4_index = (virt >> 39) & 0x1FF;
    struct page_table* pdpt = paging_root->entries[pml4_index];
    if(paging_root->_cpu_table[pml4_index] == 0) {
        pdpt = _allocate_page_table();
        paging_root->entries[pml4_index] = pdpt;
        paging_root->_cpu_table[pml4_index] = __pa_kernel(pdpt->_cpu_table) | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE;
    }

    // create a level 2 page table (page directory) if necessary
    u32 pdpt_index = (virt >> 30) & 0x1FF;
    struct page_table* pd = pdpt->entries[pdpt_index];
    if(pdpt->_cpu_table[pdpt_index] == 0) {
        pd = _allocate_page_table();
        pdpt->entries[pdpt_index] = pd;
        pdpt->_cpu_table[pdpt_index] = __pa_kernel(pd->_cpu_table) | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE;
    }

    // create a level 1 page table if necessary
    u32 pd_index = (virt >> 21) & 0x1FF;
    struct page_table* pt = pd->entries[pd_index];
    if(pd->_cpu_table[pd_index] == 0) {
        pt = _allocate_page_table();
        pd->entries[pd_index] = pt;
        pd->_cpu_table[pd_index] = __pa_kernel(pt->_cpu_table) | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE;
    }

    // create the entry in the lowest level table (page table). if it already exists, error out
    u32 pt_index = (virt >> 12) & 0x1FF;
    u64* pte = &pt->_cpu_table[pt_index];
    assert(*pte == 0, "mapping for virtual address already exists");

    // set the page directory entry with huge page bit set
    *pte = (phys & CPU_PAGE_TABLE_ADDRESS_MASK_4KB) | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE;
}

void paging_map_page(intp phys, intp virt)
{
    // call _map_page and then flushes TLB
    _map_page(phys, virt);
    __invlpg(virt);
}

// _map_2mb does the hard work of mapping the physical address into the specified virtual address
// location, but does not flush the TLB. For TLB flush, call paging_map_2mb() instead.
static void _map_2mb(intp phys, intp virt)
{
    assert((virt >> 47) == 0 || (virt >> 47) == 0x1FFFF, "virtual address must be canonical");
    assert(__alignof(virt, 4096) == 0, "virtual address must be 4KB aligned");
    assert(__alignof(phys, 0x200000) == 0, "physical address must be 2MiB aligned");

    //fprintf(stderr, "paging: mapping 1GiB at 0x%08lX to virtual address 0x%08lX\n", phys, virt);

    // shift right 12 for pt1
    // shift right 21 for pt2
    // shift right 30 for pt3
    // shift right 39 for pt4

    // create a level 3 page table (page directory pointers) if necessary
    u32 pml4_index = (virt >> 39) & 0x1FF;
    struct page_table* pdpt = paging_root->entries[pml4_index];
    if(paging_root->_cpu_table[pml4_index] == 0) {
        pdpt = _allocate_page_table();
        paging_root->entries[pml4_index] = pdpt;
        paging_root->_cpu_table[pml4_index] = __pa_kernel(pdpt->_cpu_table) | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE;
    }

    // create a level 2 page table (page directory) if necessary
    u32 pdpt_index = (virt >> 30) & 0x1FF;
    struct page_table* pd = pdpt->entries[pdpt_index];
    if(pdpt->_cpu_table[pdpt_index] == 0) {
        pd = _allocate_page_table();
        pdpt->entries[pdpt_index] = pd;
        pdpt->_cpu_table[pdpt_index] = __pa_kernel(pd->_cpu_table) | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE;
    }

    // create the entry in the level 2 table (page directory). if it already exists, error out
    u32 pd_index = (virt >> 21) & 0x1FF;
    u64* pde = &pd->_cpu_table[pd_index];
    assert(*pde == 0, "mapping for virtual address already exists");

    // set the page directory entry with huge page bit set
    *pde = (phys & CPU_PAGE_TABLE_ADDRESS_MASK_2MB) | CPU_PAGE_TABLE_ENTRY_FLAG_HUGE | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE;
}

void paging_map_2mb(intp phys, intp virt)
{
    // calls _map_2mb and then flushes TLB
    _map_2mb(phys, virt);

    // TODO do I seriously have to issue 512 INVLPGs?
    for(u32 i = 0; i < 512; i++) {
        __invlpg(virt);
        virt += 4096;
    }
}

