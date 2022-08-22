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
    fprintf(stderr, "paging: userland text at 0x%lX, size=0x%lX\n", (intp)&_userland_text_start, (intp)&_userland_text_end - (intp)&_userland_text_start);
    fprintf(stderr, "paging: userland data at 0x%lX, size=0x%lX\n", (intp)&_userland_data_start, (intp)&_userland_data_end - (intp)&_userland_data_start);

    // map only the pages the kernel occupies into virtual memory
    for(intp offs = 0; offs < kernel_size; offs += PAGE_SIZE) {
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

    // TEMP TEMP userland memory!
    // map only the pages userland occupies into virtual memory with user flag
    for(intp offs = (intp)&_userland_text_start; offs < (intp)&_userland_text_end; offs += PAGE_SIZE) {
        intp virt = offs;
        intp phys = virt - (intp)&_kernel_vma_base;
        _map_page(phys, virt, MAP_PAGE_FLAG_WRITABLE | MAP_PAGE_FLAG_USER);
    }

    // map only the pages userland occupies into virtual memory with user flag
    for(intp offs = (intp)&_userland_data_start; offs < (intp)&_userland_data_end; offs += PAGE_SIZE) {
        intp virt = offs;
        intp phys = virt - (intp)&_kernel_vma_base;
        _map_page(phys, virt, MAP_PAGE_FLAG_WRITABLE | MAP_PAGE_FLAG_USER);
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
        paging_root->_cpu_table[pml4_index] = (intp)pdpt->_cpu_table | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE | CPU_PAGE_TABLE_ENTRY_FLAG_USER;
        paging_root->num_entries++;
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
        paging_root->_cpu_table[pml4_index] = (intp)pdpt->_cpu_table | CPU_PAGE_TABLE_ENTRY_FLAG_PRESENT | CPU_PAGE_TABLE_ENTRY_FLAG_WRITEABLE | CPU_PAGE_TABLE_ENTRY_FLAG_USER;
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
#include "rbtree.h"

struct vmem_node {
    MAKE_RB_TREE;

    intp base; 
    u64  length;
};

struct vmem_node* vmem_free_areas = null;

static s64 _vmem_node_cmp_bases(struct vmem_node const* a, struct vmem_node const* b)
{
    // we'll maintain an invariant in our tree that no two vmem_nodes overlap
    // that allows a very simple comparison on the regions:
    return (b->base - a->base);
}

static s64 _vmem_node_cmp_ends(struct vmem_node const* a, struct vmem_node const* b)
{
    intp a_end = a->base + a->length;
    intp b_end = b->base + b->length;

    // we'll maintain an invariant in our tree that no two vmem_nodes overlap
    // that allows a very simple comparison on the regions:
    return (b_end - a_end);
}


void vmem_init()
{
    struct vmem_node* node = (struct vmem_node*)kalloc(sizeof(struct vmem_node));
    zero(node);
    node->base = 0xFFFF800000000000;
    node->length = (u64)&_kernel_vma_base - (u64)node->base;

    RB_TREE_INSERT(vmem_free_areas, node, _vmem_node_cmp_bases);

    fprintf(stderr, "vmem: initialized virtual memory for area 0x%lX-0x%lX\n", vmem_free_areas->base, vmem_free_areas->base + vmem_free_areas->length);
}

intp vmem_create_private()
{
    struct vmem_node* private_vmem = null;

    struct vmem_node* node = (struct vmem_node*)kalloc(sizeof(struct vmem_node));
    zero(node);
    node->base = 0x0000000100000000;
    node->length = 0x0000800000000000 - (u64)node->base;

    RB_TREE_INSERT(private_vmem, node, _vmem_node_cmp_bases);

    fprintf(stderr, "vmem: initialized private virtual memory area 0x%lX-0x%lX\n", private_vmem->base, private_vmem->base + private_vmem->length);
    return (intp)private_vmem;
}

intp vmem_map_pages(intp phys, u64 npages, u32 flags)
{
    intp virtual_address;
    u64 wanted_size = npages << PAGE_SHIFT;

    // loop over free areas looking for a large enough node
    struct vmem_node* iter;
    RB_TREE_FOREACH(vmem_free_areas, iter) {
        virtual_address = iter->base;

        if(iter->length > wanted_size) {
            // increment the base address, easy
            iter->base += wanted_size;
            iter->length -= wanted_size;
        } else if(iter->length == wanted_size) {
            // remove the node
            RB_TREE_REMOVE(vmem_free_areas, iter);
            kfree(iter);
        }

        // done either way
        break;
    }

    fprintf(stderr, "vmem: mapping %d pages start 0x%lX to 0x%lX-0x%lX\n", npages, phys, virtual_address, virtual_address+wanted_size);
    for(u64 offs = 0; offs < wanted_size; offs += PAGE_SIZE) {
        paging_map_page(phys + offs, virtual_address + offs, flags);
    }

    return virtual_address;
}

intp vmem_unmap_pages(intp virt, u64 npages)
{
    intp ret;
    assert(npages != 0, "must unmap at least one page");

    intp size = npages * PAGE_SIZE;
    intp end = virt + size;

    fprintf(stderr, "vmem: unmapping %d pages at 0x%lX-0x%lX\n", npages, virt, end);

    // look up the end of the freed region to see if it matches the beginning of any other region
    struct vmem_node lookup = { .base = end };
    struct vmem_node* result;
    if(RB_TREE_FIND(vmem_free_areas, result, lookup, _vmem_node_cmp_bases)) {
        // extend result downward, that's it
        fprintf(stderr, "    extending 0x%lX downward to 0x%lX\n", result->base, result->base - size);
        result->base -= size;    
        result->length += size;

        // look for a node that ends with result->base
        lookup.base = 0;
        lookup.length = result->base;
        struct vmem_node* result2;
        while(RB_TREE_FIND(vmem_free_areas, result2, lookup, _vmem_node_cmp_ends)) {
            // so result2 is a node that ends where result begins, so they can be merged
            RB_TREE_REMOVE(vmem_free_areas, result2);

            // update result to include the region
            result->base = result2->base;
            result->length += result2->length;
            fprintf(stderr, "    merging node 0x%lX-0x%lX into 0x%lX-0x%lX\n", result2->base, result2->base + result2->length, result->base, result->base + result->length);

            // update the lookup to the new base
            lookup.length = result->base;

            // free old memory
            kfree(result2);
        }

        // we just extended downward as much as possible, so there's no more free regions to merge
        goto done;
    }

    // if we get here, no region extended upward will be mergeable with any other,
    // so we can just extend a single region upward without having to check for others
    lookup.base = 0; // will have to match a region that ends with the new address
    lookup.length = virt;
    if(RB_TREE_FIND(vmem_free_areas, result, lookup, _vmem_node_cmp_ends)) {
        // a region was found, so we can expand upwards
        fprintf(stderr, "    extending 0x%lX-0x%lX upward to 0x%lX\n", result->base, result->base+result->length, result->base+result->length+size);
        result->length += size;
        goto done;
    }

    // no node was found for extending, so make a new one
    struct vmem_node* newnode = (struct vmem_node*)kalloc(sizeof(struct vmem_node));
    zero(newnode);
    newnode->base = virt;
    newnode->length = size;
    fprintf(stderr, "    inserting new vmem_node 0x%lX-0x%lX\n", newnode->base, newnode->base+newnode->length);
    RB_TREE_INSERT(vmem_free_areas, newnode, _vmem_node_cmp_bases);

done:
    ret = paging_unmap_page(virt); // unmap the first page to get the physical return address

    for(u64 offs = PAGE_SIZE; offs < size; offs += PAGE_SIZE) {
        paging_unmap_page(virt + offs);
    }

    return ret;
}
