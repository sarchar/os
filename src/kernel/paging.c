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
    struct page_table** entries;   // always 512 entries, might as well be page aligned since it takes 4096 bytes
    u64    flags;                  // TODO fun flaggies
    u8     num_entries;
    u8     unused0[7];
};

static struct page_table* paging_root;

static void _map_page(intp phys, intp virt, u32 flags);
static intp _unmap_page(intp virt);
static void _map_2mb(intp phys, intp virt, u32 flags);

static struct page_table* _allocate_page_table()
{
    struct page_table* pt = kalloc(sizeof(struct page_table));

    pt->_cpu_table  = (u64*)palloc_claim_one();
    pt->entries     = (struct page_table**)palloc_claim_one();
    pt->num_entries = 0;
    pt->flags       = 0;

    memset64(pt->_cpu_table, 0, 512);
    memset64(pt->entries, 0, 512);

    return pt;
}

static void _free_page_table(struct page_table* pt)
{
    kfree(pt);
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
    paging_set_kernel_page_table();

    // testing
    u8* physpage = (u8*)palloc_claim_one();
    physpage[1000] = 0x55; // we have identity mapping on all memory
    physpage[1001] = 0xAA;
    paging_map_page((intp)physpage, 0x3F00000000, MAP_PAGE_FLAG_WRITABLE);
    assert(((u8*)0x3F00000000)[1000] == 0x55, "55 is wrong");
    assert(((u8*)0x3F00000000)[1001] == 0xAA, "AA is wrong");
    return;
}

void paging_set_kernel_page_table()
{
    __wrcr3((u64)paging_root->_cpu_table);
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
        paging_root->num_entries++;
    }

    // create a level 2 page table (page directory) if necessary
    u32 pdpt_index = (virt >> 30) & 0x1FF;
    struct page_table* pd = pdpt->entries[pdpt_index];
    if(pdpt->_cpu_table[pdpt_index] == 0) {
        pd = _allocate_page_table();
        pdpt->entries[pdpt_index] = pd;
        pdpt->_cpu_table[pdpt_index] = (intp)pd->_cpu_table | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE;
        pdpt->num_entries++;
    }

    // create a level 1 page table if necessary
    u32 pd_index = (virt >> 21) & 0x1FF;
    struct page_table* pt = pd->entries[pd_index];
    if(pd->_cpu_table[pd_index] == 0) {
        pt = _allocate_page_table();
        pd->entries[pd_index] = pt;
        pd->_cpu_table[pd_index] = (intp)pt->_cpu_table | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE;
        pd->num_entries++;
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
    pt->num_entries++;
}

// _unmap_page simply removes the entry that 'virt' resolves to from the lowest level page table
static intp _unmap_page(intp virt)
{
    assert((virt >> 47) == 0 || (virt >> 47) == 0x1FFFF, "virtual address must be canonical");
    assert(__alignof(virt, 4096) == 0, "virtual address must be 4KB aligned");

    //fprintf(stderr, "paging: unmapping page at 0x%08lX\n", virt);

    // shift right 12 for pt1
    // shift right 21 for pt2
    // shift right 30 for pt3
    // shift right 39 for pt4

    // find the level 3 page table (page directory pointers), and error out if it doesn't exist
    u32 pml4_index = (virt >> 39) & 0x1FF;
    assert(paging_root->_cpu_table[pml4_index] & CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT, "virtual address mapping not found");
    struct page_table* pdpt = paging_root->entries[pml4_index];

    // find the level 2 page table (page directory pointers), and error out if it doesn't exist
    u32 pdpt_index = (virt >> 30) & 0x1FF;
    assert(pdpt->_cpu_table[pdpt_index] & CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT, "virtual address mapping not found");
    struct page_table* pd = pdpt->entries[pdpt_index];

    // find the level 1 page table (page directory pointers), and error out if it doesn't exist
    u32 pd_index = (virt >> 21) & 0x1FF;
    assert(pd->_cpu_table[pd_index] & CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT, "virtual address not found");
    struct page_table* pt = pd->entries[pd_index];

    // make sure the level 0 page table entry exists
    u32 pt_index = (virt >> 12) & 0x1FF;
    u64* pte = &pt->_cpu_table[pt_index];
    assert(*pte & CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT, "page table entry not present");

    // free the page table entry
    intp ret = *pte & CPU_PAGE_TABLE_ADDRESS_MASK_4KB;
    *pte = 0;

    // free nested page tables if they become empty
    if(--pt->num_entries != 0) goto done;

    _free_page_table(pt);
    pd->entries[pd_index] = null;
    pd->_cpu_table[pd_index] = 0;  // TODO do these have to be invalidated?

    if(--pd->num_entries != 0) goto done;

    _free_page_table(pd);
    pdpt->entries[pdpt_index] = null;
    pdpt->_cpu_table[pdpt_index] = 0;  // TODO do these have to be invalidated?

    if(--pdpt->num_entries != 0) goto done;

    _free_page_table(pdpt);
    paging_root->entries[pml4_index] = null;
    paging_root->_cpu_table[pml4_index] = 0; // TODO do these have to be invalidated?
    pdpt->num_entries -= 1;

done:
    return ret;
}


// Dump information on how a virtual address is decoded
void _make_flags_string(char* buf, u64 v)
{
    if(v & CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT) buf[1] = 'P';
    else                                      buf[1] = 'p';
    if(v & CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE) buf[2] = 'W';
    else                                        buf[2] = 'w';
    if(v & CPU_PAGE_TABLE_ENTRY_FLAG_SUPERUSER) buf[3] = 'S';
    else                                        buf[3] = 's';
    if(v & CPU_PAGE_TABLE_ENTRY_WRITE_THROUGH) buf[4] = 'T';
    else                                       buf[4] = 't';
    if(v & CPU_PAGE_TABLE_ENTRY_CACHE_DISABLE) buf[5] = 'C';
    else                                       buf[5] = 'c';
    if(v & CPU_PAGE_TABLE_ENTRY_ACCESSED)      buf[6] = 'A';
    else                                       buf[6] = 'a';
    if(v & CPU_PAGE_TABLE_ENTRY_DIRTY)         buf[7] = 'D';
    else                                       buf[7] = 'd';
    if(v & CPU_PAGE_TABLE_ENTRY_FLAG_HUGE)     buf[8] = 'H';
    else                                       buf[8] = 'h';
}

void paging_debug_address(intp virt)
{
    char buf[] = "[........]";

    fprintf(stderr, "paging: table dump for address 0x%016lX\n", virt);
    fprintf(stderr, "0x%016lX (paging_root)\n", paging_root->_cpu_table);

    // print pml4 entry
    u32 pml4_index = (virt >> 39) & 0x1FF;
    struct page_table* pdpt = paging_root->entries[pml4_index];
    _make_flags_string(buf, paging_root->_cpu_table[pml4_index]);
    u64 base = pml4_index * (1ULL << 39);
    fprintf(stderr, "`- [%d] 0x%016X (pdpt), 0x%016lX .. 0x%016lX flags=%s\n", pml4_index, paging_root->_cpu_table[pml4_index], base, (base + (1ULL << 39)) - 1, buf);
    if(buf[1] == 'p' || buf[7] == 'H') return;

    // print page directory pointers table entry
    u32 pdpt_index = (virt >> 30) & 0x1FF;
    struct page_table* pd = pdpt->entries[pdpt_index];
    _make_flags_string(buf, pdpt->_cpu_table[pdpt_index]);
    base += pdpt_index * (1ULL << 30);
    fprintf(stderr, "   `- [%d] 0x%016X (pd), 0x%016lX .. 0x%016lX flags=%s\n", pdpt_index, pdpt->_cpu_table[pdpt_index], base, (base + (1ULL << 30)) - 1, buf);
    if(buf[1] == 'p' || buf[7] == 'H') return;

    // print page directory table entry
    u32 pd_index = (virt >> 21) & 0x1FF;
    struct page_table* pt = pd->entries[pd_index];
    _make_flags_string(buf, pd->_cpu_table[pd_index]);
    base += pd_index * (1ULL << 21);
    fprintf(stderr, "      `- [%d] 0x%016X (pt), 0x%016lX .. 0x%016lX flags=%s\n", pd_index, pd->_cpu_table[pd_index], base, (base + (1ULL << 21)) - 1, buf);
    if(buf[1] == 'p' || buf[7] == 'H') return;

    // print page table entry
    u32 pt_index = (virt >> 12) & 0x1FF;
    _make_flags_string(buf, pt->_cpu_table[pt_index]);
    base += pt_index * (1ULL << 12);
    fprintf(stderr, "         `- [%d] 0x%016X (pte), 0x%016lX .. 0x%016lX flags=%s\n", pt_index, pt->_cpu_table[pt_index], base, (base + (1ULL << 12)) - 1, buf);
}

void paging_map_page(intp phys, intp virt, u32 flags)
{
    // call _map_page and then flushes TLB
    _map_page(phys, virt, flags);
    __invlpg(virt);
}

intp paging_unmap_page(intp virt)
{
    return _unmap_page(virt);
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

// TODO this eventually needs to move into a virtual memory manager
// map a single page at physical address phys to a newly allocated virtual
// memory address
intp vmem_map_page(intp phys, u32 flags)
{
    // TODO for now, I know that paging_ only allocates memory in the 0-4GB region
    // TODO and so we just pick some address in unoccupied high memory and map it there directly
    assert(phys < 0x100000000, "TODO only working with low mem for now");
    intp virtual_address = phys | 0xFFFF800000000000ULL;

    paging_map_page(phys, virtual_address, flags);
    return virtual_address;
}

intp vmem_unmap_page(intp virt)
{
    return paging_unmap_page(virt);
}
