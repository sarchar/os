#include "common.h"

#include "cpu.h"
#include "kalloc.h"
#include "kernel.h"
#include "multiboot2.h"
#include "paging.h"
#include "palloc.h"
#include "stdio.h"

// bits 51-63 are not address, and the physical address must be 2MiB aligned
#define CPU_PAGE_TABLE_ADDRESS_MASK_1GB     0x0007FFFFC0000000
#define CPU_PAGE_TABLE_ADDRESS_MASK_2MB     0x0007FFFFFFE00000
#define CPU_PAGE_TABLE_ADDRESS_MASK_4KB     0x0007FFFFFFFFFC00

enum CPU_PAGE_TABLE_ENTRY_FLAGS {
    CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT   = (1 << 0),
    CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE = (1 << 1),
    CPU_PAGE_TABLE_ENTRY_FLAG_SUPERUSER = (1 << 2),
    CPU_PAGE_TABLE_ENTRY_WRITE_THROUGH  = (1 << 3),
    CPU_PAGE_TABLE_ENTRY_CACHE_DISABLE  = (1 << 4),
    CPU_PAGE_TABLE_ENTRY_ACCESSED       = (1 << 5),
    CPU_PAGE_TABLE_ENTRY_DIRTY          = (1 << 6),
    CPU_PAGE_TABLE_ENTRY_FLAG_HUGE      = (1 << 7)
};

struct page_table {
    u64*   _cpu_table;             // always one page, and page aligned. this is the table sent to the cpu
    u64    flags;                  // TODO fun flaggies
    struct page_table** entries;   // always 512 entries, might as well be page aligned since it takes 4096 bytes
    struct page_table_allocator_page* alloc_page; // pointer to the alloc page this page table data resides in
};

static struct page_table* paging_root;

static void _map_page(intp phys, intp virt, u32 flags);
static void _map_2mb(intp phys, intp virt, u32 flags);

static struct page_table* _allocate_page_table()
{
    struct page_table* pt = kalloc(sizeof(struct page_table));

    pt->_cpu_table = (u64*)palloc_claim_one();
    pt->entries    = (struct page_table**)palloc_claim_one();
    pt->alloc_page = null;
    pt->flags      = 0;

    memset64(pt->_cpu_table, 0, 512);
    memset64(pt->entries, 0, 512);

    return pt;
}

static void _map_kernel()
{
    u64 kernel_size = (intp)(&_kernel_end_address)-(intp)(&_kernel_vma_base)-(intp)(&_kernel_load_address);
    fprintf(stderr, "paging: kernel loaded at 0x%lX, vma=0x%lX, end=0x%lX, size=0x%lX\n", &_kernel_load_address, &_kernel_vma_base, &_kernel_end_address, kernel_size);
    fprintf(stderr, "paging: stack at 0x%lX, top=0x%lX\n", (intp)&_stack_bottom, (intp)&_stack_top);

    // map only the pages the kernel occupies into virtual memory
    for(intp offs = 0; offs < kernel_size; offs += 0x1000) {
        intp phys = (intp)&_kernel_load_address + offs;
        intp virt = phys | (intp)&_kernel_vma_base;
        _map_page(phys, virt, MAP_PAGE_FLAG_WRITABLE);
    }

    // All of the lowmem free regions are identity mapped so that palloc/kalloc
    // and other various modules that used bootmem up until this point, continue to work.
    intp region_start;
    u64 region_size;
    u8 region_type;
    while((region_start = multiboot2_mmap_next_free_region(&region_size, &region_type)) != (intp)-1) {
        // also map AHCI areas for parsing ahci later
        if(region_type == MULTIBOOT_REGION_TYPE_AVAILABLE || region_type == MULTIBOOT_REGION_TYPE_AHCI_RECLAIMABLE) {
            paging_identity_map_region(region_start, region_size, MAP_PAGE_FLAG_WRITABLE);
        }
    }
}

void paging_init()
{
    // TODO build a new page table using memory allocated from palloc and switch to it
    // paging_root is the level 4 page table
    paging_root = _allocate_page_table();
    fprintf(stderr, "paging: initializing page tables (paging_root->_cpu_table=0x%lX)\n", paging_root->_cpu_table);

    // map the kernel into virtual space
    _map_kernel();

    // and change cr3 to set the new page tables
    // setting cr3 forces a full tlb invalidate
    __wrcr3((u64)paging_root->_cpu_table);

    // testing
    u8* physpage = (u8*)palloc_claim_one();
    physpage[1000] = 0x55; // we have identity mapping on all memory
    physpage[1001] = 0xAA;
    paging_map_page((intp)physpage, 0x3F00000000, MAP_PAGE_FLAG_WRITABLE);
    assert(((u8*)0x3F00000000)[1000] == 0x55, "55 is wrong");
    assert(((u8*)0x3F00000000)[1001] == 0xAA, "AA is wrong");
    return;
}

// _map_page does the hard work of mapping a physical address for the specified virtual address
// location, but does not flush the TLB. For TLB flush, call paging_map_page() instead.
static void _map_page(intp phys, intp virt, u32 flags)
{
    assert((virt >> 47) == 0 || (virt >> 47) == 0x1FFFF, "virtual address must be canonical");
    assert(__alignof(virt, 4096) == 0, "virtual address must be 4KB aligned");
    assert(__alignof(phys, 4096) == 0, "physical address must be 4KB aligned");

    //fprintf(stderr, "paging: mapping page at 0x%08lX to virtual address 0x%08lX\n", phys, virt);

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
        paging_root->_cpu_table[pml4_index] = (intp)pdpt->_cpu_table | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE;
    }

    // create a level 2 page table (page directory) if necessary
    u32 pdpt_index = (virt >> 30) & 0x1FF;
    struct page_table* pd = pdpt->entries[pdpt_index];
    if(pdpt->_cpu_table[pdpt_index] == 0) {
        pd = _allocate_page_table();
        pdpt->entries[pdpt_index] = pd;
        pdpt->_cpu_table[pdpt_index] = (intp)pd->_cpu_table | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE;
    }

    // create a level 1 page table if necessary
    u32 pd_index = (virt >> 21) & 0x1FF;
    struct page_table* pt = pd->entries[pd_index];
    if(pd->_cpu_table[pd_index] == 0) {
        pt = _allocate_page_table();
        pd->entries[pd_index] = pt;
        pd->_cpu_table[pd_index] = (intp)pt->_cpu_table | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE;
    }

    // create the entry in the lowest level table (page table). if it already exists, error out
    u32 pt_index = (virt >> 12) & 0x1FF;
    u64* pte = &pt->_cpu_table[pt_index];
    assert(*pte == 0, "mapping for virtual address already exists");

    // set the page directory entry
    u32 pt_flags = CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT;
    if(flags & MAP_PAGE_FLAG_DISABLE_CACHE) pt_flags |= CPU_PAGE_TABLE_ENTRY_CACHE_DISABLE;
    if(flags & MAP_PAGE_FLAG_WRITABLE) pt_flags |= CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE;
    *pte = (phys & CPU_PAGE_TABLE_ADDRESS_MASK_4KB) | pt_flags;
}

// Dump information on how a virtual address is decoded
void paging_debug_address(intp virt)
{
    // create a level 3 page table (page directory pointers) if necessary
    u32 pml4_index = (virt >> 39) & 0x1FF;
    fprintf(stderr, "paging: pml4_index=%d (0x%03X)\n", pml4_index, pml4_index);
    struct page_table* pdpt = paging_root->entries[pml4_index];
    fprintf(stderr, "paging: paging_root=0x%lX\n", paging_root);
    fprintf(stderr, "paging: paging_root->_cpu_table[pml4_index]=0x%lX\n", paging_root->_cpu_table[pml4_index]);
    fprintf(stderr, "paging: paging_root->entries[pml4_index]=0x%lX\n", pdpt);
}

void paging_map_page(intp phys, intp virt, u32 flags)
{
    // call _map_page and then flushes TLB
    _map_page(phys, virt, flags);
    __invlpg(virt);
}

// _map_2mb does the hard work of mapping the physical address into the specified virtual address
// location, but does not flush the TLB. For TLB flush, call paging_map_2mb() instead.
static void _map_2mb(intp phys, intp virt, u32 flags)
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
        paging_root->_cpu_table[pml4_index] = (intp)pdpt->_cpu_table | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE;
    }

    // create a level 2 page table (page directory) if necessary
    u32 pdpt_index = (virt >> 30) & 0x1FF;
    struct page_table* pd = pdpt->entries[pdpt_index];
    if(pdpt->_cpu_table[pdpt_index] == 0) {
        pd = _allocate_page_table();
        pdpt->entries[pdpt_index] = pd;
        pdpt->_cpu_table[pdpt_index] = (intp)pd->_cpu_table | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE;
    }

    // create the entry in the level 2 table (page directory). if it already exists, error out
    u32 pd_index = (virt >> 21) & 0x1FF;
    u64* pde = &pd->_cpu_table[pd_index];
    assert(*pde == 0, "mapping for virtual address already exists");

    // set the page directory entry with huge page bit set
    u32 pt_flags = CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT;
    if(flags & MAP_PAGE_FLAG_DISABLE_CACHE) pt_flags |= CPU_PAGE_TABLE_ENTRY_CACHE_DISABLE;
    if(flags & MAP_PAGE_FLAG_WRITABLE) pt_flags |= CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE;
    *pde = (phys & CPU_PAGE_TABLE_ADDRESS_MASK_2MB) | CPU_PAGE_TABLE_ENTRY_FLAG_HUGE | pt_flags;
}

void paging_map_2mb(intp phys, intp virt, u32 flags)
{
    // calls _map_2mb and then flushes TLB
    _map_2mb(phys, virt, flags);

    // TODO do I actually have to issue 512 INVLPGs?
    for(u32 i = 0; i < 512; i++) {
        __invlpg(virt);
        virt += 4096;
    }
}

void paging_identity_map_region(intp region_start, u64 region_size, u32 flags)
{
    assert(__alignof(region_start, 4096) == 0, "regions must start on page boundaries");
    assert(__alignof(region_size, 4096) == 0, "region size must be a multiple of page size");

    //fprintf(stderr, "paging: identity mapping whole region 0x%lX-0x%lX\n", region_start, region_start+region_size-1);

    // map 4K pages to start if the address doesn't sit on a 2MiB boundary
    u32 pre = __alignof(region_start, 0x200000);
    if(pre) {
        intp pre_size = min(region_size, 0x200000 - pre); // might be less than 2MiB
        for(intp offs = 0; offs < pre_size; offs += 0x1000) {
            _map_page(region_start + offs, region_start + offs, flags);
        }
        region_start = (intp)__alignup(region_start, 0x200000);
        region_size -= pre_size;
    }

    // continually map 2MiB blocks while there's >2MiB left
    while(region_size >= 0x200000) {
        _map_2mb(region_start, region_start, flags);
        region_start += 0x200000;
        region_size  -= 0x200000;
    }

    // finally map remaining 4K pages
    while(region_size > 0) {
        _map_page(region_start, region_start, flags);
        region_start += 0x1000;
        region_size  -= 0x1000;
    }
}

