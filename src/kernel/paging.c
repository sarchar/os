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
    CPU_PAGE_TABLE_ENTRY_FLAG_USER      = (1 << 2),
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

static struct page_table* kernel_page_table;

static void _map_page(struct page_table* table_root, intp phys, intp virt, u32 flags);
static intp _unmap_page(struct page_table* table_root, intp virt);
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
    kfree(pt, sizeof(struct page_table));
}

static void _map_kernel()
{
    u64 kernel_size = (intp)(&_kernel_end_address)-(intp)(&_kernel_vma_base)-(intp)(&_kernel_load_address);
    fprintf(stderr, "paging: kernel loaded at 0x%lX, vma=0x%lX, end=0x%lX, size=0x%lX\n", &_kernel_load_address, &_kernel_vma_base, &_kernel_end_address, kernel_size);
    fprintf(stderr, "paging: stack at 0x%lX, top=0x%lX\n", (intp)&_stack_bottom, (intp)&_stack_top);
    fprintf(stderr, "paging: userland text at 0x%lX, size=0x%lX\n", (intp)&_userland_text_start, (intp)&_userland_text_end - (intp)&_userland_text_start);
    fprintf(stderr, "paging: userland data at 0x%lX, size=0x%lX\n", (intp)&_userland_data_start, (intp)&_userland_data_end - (intp)&_userland_data_start);

    // map only the pages the kernel occupies into virtual memory
    for(intp offs = 0; offs < kernel_size; offs += PAGE_SIZE) {
        intp phys = (intp)&_kernel_load_address + offs;
        intp virt = phys | (intp)&_kernel_vma_base;
        _map_page(kernel_page_table, phys, virt, MAP_PAGE_FLAG_WRITABLE);
    }

    // All of the lowmem free regions are identity mapped so that palloc/kalloc
    // and other various modules that used bootmem up until this point, continue to work.
    intp region_start;
    u64 region_size;
    u8 region_type;
    while((region_start = multiboot2_mmap_next_free_region(&region_size, &region_type)) != (intp)-1) {
        // also map AHCI areas for parsing ahci later
        if(region_type == MULTIBOOT_REGION_TYPE_AVAILABLE || region_type == MULTIBOOT_REGION_TYPE_AHCI_RECLAIMABLE) {
            paging_identity_map_region(kernel_page_table, region_start, region_size, MAP_PAGE_FLAG_WRITABLE);
        }
    }

    // TEMP TEMP userland memory!
    // map only the pages userland occupies into virtual memory with user flag
    for(intp offs = (intp)&_userland_text_start; offs < (intp)&_userland_text_end; offs += PAGE_SIZE) {
        intp virt = offs;
        intp phys = virt - (intp)&_kernel_vma_base;
        _map_page(kernel_page_table, phys, virt, MAP_PAGE_FLAG_WRITABLE | MAP_PAGE_FLAG_USER);
    }

    // map only the pages userland occupies into virtual memory with user flag
    for(intp offs = (intp)&_userland_data_start; offs < (intp)&_userland_data_end; offs += PAGE_SIZE) {
        intp virt = offs;
        intp phys = virt - (intp)&_kernel_vma_base;
        _map_page(kernel_page_table, phys, virt, MAP_PAGE_FLAG_WRITABLE | MAP_PAGE_FLAG_USER);
    }
}

void paging_init()
{
    // build a new page table using memory allocated from palloc and switch to it
    // kernel_page_table is the level 4 page table
    kernel_page_table = _allocate_page_table();
    fprintf(stderr, "paging: initializing page tables (kernel_page_table->_cpu_table=0x%lX)\n", kernel_page_table->_cpu_table);

    // map the kernel into virtual space
    _map_kernel();

    // and change cr3 to set the new page tables
    // setting cr3 forces a full tlb invalidate
    paging_set_kernel_page_table();

    // create empty but present tables for all the high memory pml4 entries
    for(u32 i = 256; i < 512; i++) {
        if(kernel_page_table->_cpu_table[i] != 0) continue;

        struct page_table* pdpt = _allocate_page_table();
        kernel_page_table->entries[i] = pdpt;
        kernel_page_table->_cpu_table[i] = (intp)pdpt->_cpu_table | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE | CPU_PAGE_TABLE_ENTRY_FLAG_USER;
        kernel_page_table->num_entries++;
    }
    
    return;
}

void paging_set_kernel_page_table()
{
    __wrcr3((u64)kernel_page_table->_cpu_table);
}

struct page_table* paging_get_kernel_page_table()
{
    return kernel_page_table;
}

intp paging_get_cpu_table(struct page_table* table_root)
{
    return (intp)table_root->_cpu_table;
}

// create a new page table root, and map the kernel into it by copying pages for 0-4GiB and 0xFFFF800000000000+
struct page_table* paging_create_private_table()
{
    struct page_table* private = _allocate_page_table();

    // entry 0 maps 0x00000000_00000000->0x0000007F_FFFFFFFF
    // entry 0 will never be null
    assert(kernel_page_table->entries[0] != null, "low mem missing pointer in page table?");
    private->entries[0]    = kernel_page_table->entries[0];
    private->_cpu_table[0] = kernel_page_table->_cpu_table[0];
    private->num_entries++; 

    // entries 256-511 map 0xFFFF8000_00000000-0xFFFFFFFF_FFFFFFFF
    for(u64 i = 256; i < 512; i++) {
        assert(kernel_page_table->entries[i] != null, "kernel high memory must have page table entries in the PML4");
        private->entries[i]    = kernel_page_table->entries[i];
        private->_cpu_table[i] = kernel_page_table->_cpu_table[i];
        private->num_entries++;
    }

    return private;
}

// _map_page does the hard work of mapping a physical address for the specified virtual address
// location, but does not flush the TLB. For TLB flush, call paging_map_page() instead.
static void _map_page(struct page_table* table_root, intp phys, intp virt, u32 flags)
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
    struct page_table* pdpt = table_root->entries[pml4_index];
    if(table_root->_cpu_table[pml4_index] == 0) {
        pdpt = _allocate_page_table();
        table_root->entries[pml4_index] = pdpt;
        table_root->_cpu_table[pml4_index] = (intp)pdpt->_cpu_table | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE | CPU_PAGE_TABLE_ENTRY_FLAG_USER;
        table_root->num_entries++;
    }

    // create a level 2 page table (page directory) if necessary
    u32 pdpt_index = (virt >> 30) & 0x1FF;
    struct page_table* pd = pdpt->entries[pdpt_index];
    if(pdpt->_cpu_table[pdpt_index] == 0) {
        pd = _allocate_page_table();
        pdpt->entries[pdpt_index] = pd;
        pdpt->_cpu_table[pdpt_index] = (intp)pd->_cpu_table | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE | CPU_PAGE_TABLE_ENTRY_FLAG_USER;
        pdpt->num_entries++;
    }

    // create a level 1 page table if necessary
    u32 pd_index = (virt >> 21) & 0x1FF;
    struct page_table* pt = pd->entries[pd_index];
    if(pd->_cpu_table[pd_index] == 0) {
        pt = _allocate_page_table();
        pd->entries[pd_index] = pt;
        pd->_cpu_table[pd_index] = (intp)pt->_cpu_table | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE | CPU_PAGE_TABLE_ENTRY_FLAG_USER;
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
    if(flags & MAP_PAGE_FLAG_USER) pt_flags |= CPU_PAGE_TABLE_ENTRY_FLAG_USER;
    *pte = (phys & CPU_PAGE_TABLE_ADDRESS_MASK_4KB) | pt_flags;
    pt->num_entries++;
}

// _unmap_page simply removes the entry that 'virt' resolves to from the lowest level page table
static intp _unmap_page(struct page_table* table_root, intp virt)
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
    assert(table_root->_cpu_table[pml4_index] & CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT, "virtual address mapping not found");
    struct page_table* pdpt = table_root->entries[pml4_index];

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
    table_root->entries[pml4_index] = null;
    table_root->_cpu_table[pml4_index] = 0; // TODO do these have to be invalidated?
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
    if(v & CPU_PAGE_TABLE_ENTRY_FLAG_USER)     buf[3] = 'U';
    else                                       buf[3] = 'u';
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
    fprintf(stderr, "0x%016lX (kernel_page_table)\n", kernel_page_table->_cpu_table);

    // print pml4 entry
    u32 pml4_index = (virt >> 39) & 0x1FF;
    struct page_table* pdpt = kernel_page_table->entries[pml4_index];
    _make_flags_string(buf, kernel_page_table->_cpu_table[pml4_index]);
    u64 base = pml4_index * (1ULL << 39);
    fprintf(stderr, "`- [%d] 0x%016X (pdpt), 0x%016lX .. 0x%016lX flags=%s\n", pml4_index, kernel_page_table->_cpu_table[pml4_index], base, (base + (1ULL << 39)) - 1, buf);
    if(buf[1] == 'p' || buf[8] == 'H') return;

    // print page directory pointers table entry
    u32 pdpt_index = (virt >> 30) & 0x1FF;
    struct page_table* pd = pdpt->entries[pdpt_index];
    _make_flags_string(buf, pdpt->_cpu_table[pdpt_index]);
    base += pdpt_index * (1ULL << 30);
    fprintf(stderr, "   `- [%d] 0x%016X (pd), 0x%016lX .. 0x%016lX flags=%s\n", pdpt_index, pdpt->_cpu_table[pdpt_index], base, (base + (1ULL << 30)) - 1, buf);
    if(buf[1] == 'p' || buf[8] == 'H') return;

    // print page directory table entry
    u32 pd_index = (virt >> 21) & 0x1FF;
    struct page_table* pt = pd->entries[pd_index];
    _make_flags_string(buf, pd->_cpu_table[pd_index]);
    base += pd_index * (1ULL << 21);
    fprintf(stderr, "      `- [%d] 0x%016X (pt), 0x%016lX .. 0x%016lX flags=%s\n", pd_index, pd->_cpu_table[pd_index], base, (base + (1ULL << 21)) - 1, buf);
    if(buf[1] == 'p' || buf[8] == 'H') return;

    // print page table entry
    u32 pt_index = (virt >> 12) & 0x1FF;
    _make_flags_string(buf, pt->_cpu_table[pt_index]);
    base += pt_index * (1ULL << 12);
    fprintf(stderr, "         `- [%d] 0x%016X (pte), 0x%016lX .. 0x%016lX flags=%s\n", pt_index, pt->_cpu_table[pt_index], base, (base + (1ULL << 12)) - 1, buf);
}

void paging_debug_table(struct page_table* table_root)
{
    char buf[] = "[........]";

    fprintf(stderr, "paging: full table dump\n");
    fprintf(stderr, "0x%016lX (_cpu_table (cr3))\n", table_root->_cpu_table);

    for(u32 pml4_index = 256; pml4_index < 512; pml4_index++) {
        // print pml4 entry
        struct page_table* pdpt = table_root->entries[pml4_index];
        _make_flags_string(buf, table_root->_cpu_table[pml4_index]);
        if(buf[1] == 'p' || buf[8] == 'H') continue;
        u64 base = pml4_index * (1ULL << 39);
        if(base & 0x0000800000000000) base |= 0xFFFF000000000000ULL;
        fprintf(stderr, "`- [%d] 0x%016X (pdpt), 0x%016lX .. 0x%016lX flags=%s\n", pml4_index, table_root->_cpu_table[pml4_index], base, (base + (1ULL << 39)) - 1, buf);

        for(u32 pdpt_index = 0; pdpt_index < 512; pdpt_index++) {
            // print page directory pointers table entry
            struct page_table* pd = pdpt->entries[pdpt_index];
            _make_flags_string(buf, pdpt->_cpu_table[pdpt_index]);
            if(buf[1] == 'p' || buf[8] == 'H') continue;
            base += pdpt_index * (1ULL << 30);
            fprintf(stderr, "   `- [%d] 0x%016X (pd), 0x%016lX .. 0x%016lX flags=%s\n", pdpt_index, pdpt->_cpu_table[pdpt_index], base, (base + (1ULL << 30)) - 1, buf);

            for(u32 pd_index = 0; pd_index < 512; pd_index++) {
                // print page directory table entry
                struct page_table* pt = pd->entries[pd_index];
                _make_flags_string(buf, pd->_cpu_table[pd_index]);
                if(buf[1] == 'p' || buf[8] == 'H') continue;
                base += pd_index * (1ULL << 21);
                fprintf(stderr, "      `- [%d] 0x%016X (pt), 0x%016lX .. 0x%016lX flags=%s\n", pd_index, pd->_cpu_table[pd_index], base, (base + (1ULL << 21)) - 1, buf);

                for(u32 pt_index = 0; pt_index < 512; pt_index++) {
                    // print page table entry
                    _make_flags_string(buf, pt->_cpu_table[pt_index]);
                    if(buf[1] == 'p' || buf[8] == 'H') continue;
                    base += pt_index * (1ULL << 12);
                    fprintf(stderr, "         `- [%d] 0x%016X (pte), 0x%016lX .. 0x%016lX flags=%s\n", pt_index, pt->_cpu_table[pt_index], base, (base + (1ULL << 12)) - 1, buf);
                }
            }
        }
    }
}


void paging_map_page(struct page_table* table_root, intp phys, intp virt, u32 flags)
{
    // call _map_page and then flushes TLB
    _map_page(table_root, phys, virt, flags);
    __invlpg(virt);
}

intp paging_unmap_page(struct page_table* table_root, intp virt)
{
    return _unmap_page(table_root, virt);
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
    struct page_table* pdpt = kernel_page_table->entries[pml4_index];
    if(kernel_page_table->_cpu_table[pml4_index] == 0) {
        pdpt = _allocate_page_table();
        kernel_page_table->entries[pml4_index] = pdpt;
        kernel_page_table->_cpu_table[pml4_index] = (intp)pdpt->_cpu_table | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE | CPU_PAGE_TABLE_ENTRY_FLAG_USER;
    }

    // create a level 2 page table (page directory) if necessary
    u32 pdpt_index = (virt >> 30) & 0x1FF;
    struct page_table* pd = pdpt->entries[pdpt_index];
    if(pdpt->_cpu_table[pdpt_index] == 0) {
        pd = _allocate_page_table();
        pdpt->entries[pdpt_index] = pd;
        pdpt->_cpu_table[pdpt_index] = (intp)pd->_cpu_table | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE | CPU_PAGE_TABLE_ENTRY_FLAG_USER;
    }

    // create the entry in the level 2 table (page directory). if it already exists, error out
    u32 pd_index = (virt >> 21) & 0x1FF;
    u64* pde = &pd->_cpu_table[pd_index];
    assert(*pde == 0, "mapping for virtual address already exists");

    // set the page directory entry with huge page bit set
    u32 pt_flags = CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT;
    if(flags & MAP_PAGE_FLAG_DISABLE_CACHE) pt_flags |= CPU_PAGE_TABLE_ENTRY_CACHE_DISABLE;
    if(flags & MAP_PAGE_FLAG_WRITABLE) pt_flags |= CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE;
    if(flags & MAP_PAGE_FLAG_USER) pt_flags |= CPU_PAGE_TABLE_ENTRY_FLAG_USER;
    *pde = (phys & CPU_PAGE_TABLE_ADDRESS_MASK_2MB) | CPU_PAGE_TABLE_ENTRY_FLAG_HUGE | pt_flags;
}

// calls _map_2mb and then flushes TLB
void paging_map_2mb(intp phys, intp virt, u32 flags)
{
    _map_2mb(phys, virt, flags);

    // invlpg invalidates all TLB entries associated with the 2mb page, so only one invlpg is necessary
    __invlpg(virt);
}

void paging_identity_map_region(struct page_table* table_root, intp region_start, u64 region_size, u32 flags)
{
    assert(__alignof(region_start, 4096) == 0, "regions must start on page boundaries");
    assert(__alignof(region_size, 4096) == 0, "region size must be a multiple of page size");

    //fprintf(stderr, "paging: identity mapping whole region 0x%lX-0x%lX\n", region_start, region_start+region_size-1);

    // map 4K pages to start if the address doesn't sit on a 2MiB boundary
    u32 pre = __alignof(region_start, 0x200000);
    if(pre) {
        intp pre_size = min(region_size, 0x200000 - pre); // might be less than 2MiB
        for(intp offs = 0; offs < pre_size; offs += 0x1000) {
            _map_page(table_root, region_start + offs, region_start + offs, flags);
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
        _map_page(table_root, region_start, region_start, flags);
        region_start += 0x1000;
        region_size  -= 0x1000;
    }
}


