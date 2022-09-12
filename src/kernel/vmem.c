#include "common.h"

#include "cpu.h"
#include "kalloc.h"
#include "kernel.h"
#include "palloc.h"
#include "paging.h"
#include "rbtree.h"
#include "stdio.h"
#include "vmem.h"

// verbosity levels 1, 2 or 3
#define VMEM_VERBOSE 1

struct vmem_node {
    MAKE_RB_TREE;

    intp base; 
    u64  length;
};

struct vmem {
    struct vmem_node* free_areas;
    struct page_table* page_table;
    struct ticketlock lock;
};

struct vmem* kernel_vmem = null;

// compare two regions using their base address
static s64 _vmem_node_cmp_bases(struct vmem_node const* a, struct vmem_node const* b)
{
    // we'll maintain an invariant in our tree that no two vmem_nodes overlap
    // that allows a very simple comparison on the regions:
    return (b->base - a->base);
}

// compare two regions using their end address
static s64 _vmem_node_cmp_ends(struct vmem_node const* a, struct vmem_node const* b)
{
    intp a_end = a->base + a->length;
    intp b_end = b->base + b->length;
    return (b_end - a_end);
}

void vmem_init()
{
    kernel_vmem = (struct vmem*)kalloc(sizeof(struct vmem));
    zero(kernel_vmem);

    declare_ticketlock(lock_init);
    kernel_vmem->lock = lock_init;

    // when mapping/unmapping, make sure to use the kernel page table
    kernel_vmem->page_table = paging_get_kernel_page_table();

    struct vmem_node* node = (struct vmem_node*)kalloc(sizeof(struct vmem_node));
    zero(node);
    node->base = 0xFFFF800000000000;
    node->length = (u64)&_kernel_vma_base - (u64)node->base;

    RB_TREE_INSERT(kernel_vmem->free_areas, node, _vmem_node_cmp_bases);

#if VMEM_VERBOSE > 0
    fprintf(stderr, "vmem: initialized virtual memory for area 0x%lX-0x%lX\n", kernel_vmem->free_areas->base, kernel_vmem->free_areas->base + kernel_vmem->free_areas->length);
#endif
}

intp vmem_create_private_memory(struct page_table* page_table)
{
    struct vmem* private_vmem = (struct vmem*)kalloc(sizeof(struct vmem));
    zero(private_vmem);

    declare_ticketlock(lock_init);
    private_vmem->lock = lock_init;

    private_vmem->page_table = page_table;

    struct vmem_node* node = (struct vmem_node*)kalloc(sizeof(struct vmem_node));
    zero(node);
    node->base = 0x0000400000000000; // the PML4 page table has entries that are 0x80_00000000 (512GiB) in size, and the first 64TiB is for identity mapping memory
                                     // which leaves 64TiB for user space memory
    node->length = 0x0000800000000000ULL - (u64)node->base; // user land virtual memory goes up to the last valid canonical address with high bit 0 set

    RB_TREE_INSERT(private_vmem->free_areas, node, _vmem_node_cmp_bases);

#if VMEM_VERBOSE > 0
    fprintf(stderr, "vmem: initialized private virtual memory area 0x%lX-0x%lX\n", private_vmem->free_areas->base, private_vmem->free_areas->base + private_vmem->free_areas->length - 1);
#endif
    return (intp)private_vmem;
}

intp vmem_map_pages(intp _vmem, intp phys, u64 npages, u32 flags)
{
    struct vmem* vmem = (struct vmem*)(_vmem == 0 ? kernel_vmem : (void*)_vmem);

    intp virtual_address = 0;
    u64 wanted_size = npages << PAGE_SHIFT;

    // loop over free areas looking for a large enough node
    acquire_lock(vmem->lock);
    struct vmem_node* iter;
    RB_TREE_FOREACH(vmem->free_areas, iter) {
        virtual_address = iter->base;

        if(iter->length > wanted_size) {
            // increment the base address, easy
            iter->base += wanted_size;
            iter->length -= wanted_size;
            break;
        } else if(iter->length == wanted_size) {
            // remove the node
            RB_TREE_REMOVE(vmem->free_areas, iter);
            kfree(iter, sizeof(struct vmem_node));
            break;
        }

        // region too small, go onto the next
        continue;
    }
    release_lock(vmem->lock);

#if VMEM_VERBOSE > 1
    fprintf(stderr, "vmem: mapping %d pages start 0x%lX to 0x%lX-0x%lX\n", npages, phys, virtual_address, virtual_address+wanted_size);
#endif
    for(u64 offs = 0; offs < wanted_size; offs += PAGE_SIZE) {
        paging_map_page(vmem->page_table, phys + offs, virtual_address + offs, flags);
    }

    return virtual_address;
}

intp vmem_unmap_pages(intp _vmem, intp virt, u64 npages)
{
    struct vmem* vmem = (struct vmem*)(_vmem == 0 ? kernel_vmem : (void*)_vmem);

    intp ret;
    assert(npages != 0, "must unmap at least one page");

    intp size = npages * PAGE_SIZE;
    intp end = virt + size;

#if VMEM_VERBOSE > 1
    fprintf(stderr, "vmem: unmapping %d pages at 0x%lX-0x%lX\n", npages, virt, end);
#endif

    // look up the end of the freed region to see if it matches the beginning of any other region
    struct vmem_node lookup = { .base = end };
    struct vmem_node* result;
    acquire_lock(vmem->lock);
    if(RB_TREE_FIND(vmem->free_areas, result, lookup, _vmem_node_cmp_bases)) {
        // extend result downward, that's it
#if VMEM_VERBOSE > 2
        fprintf(stderr, "    extending 0x%lX downward to 0x%lX\n", result->base, result->base - size);
#endif
        result->base -= size;    
        result->length += size;

        // look for a node that ends with result->base
        lookup.base = 0;
        lookup.length = result->base;
        struct vmem_node* result2;
        while(RB_TREE_FIND(vmem->free_areas, result2, lookup, _vmem_node_cmp_ends)) {
            // so result2 is a node that ends where result begins, so they can be merged
            RB_TREE_REMOVE(vmem->free_areas, result2);

            // update result to include the region
            result->base = result2->base;
            result->length += result2->length;
#if VMEM_VERBOSE > 2
            fprintf(stderr, "    merging node 0x%lX-0x%lX into 0x%lX-0x%lX\n", result2->base, result2->base + result2->length, result->base, result->base + result->length);
#endif

            // update the lookup to the new base
            lookup.length = result->base;

            // free old memory
            kfree(result2, sizeof(struct vmem_node));
        }

        // we just extended downward as much as possible, so there's no more free regions to merge
        goto done;
    }

    // if we get here, no region extended upward will be mergeable with any other,
    // so we can just extend a single region upward without having to check for others
    lookup.base = 0; // will have to match a region that ends with the new address
    lookup.length = virt;
    if(RB_TREE_FIND(vmem->free_areas, result, lookup, _vmem_node_cmp_ends)) {
        // a region was found, so we can expand upwards
#if VMEM_VERBOSE > 2
        fprintf(stderr, "    extending 0x%lX-0x%lX upward to 0x%lX\n", result->base, result->base+result->length, result->base+result->length+size);
#endif
        result->length += size;
        goto done;
    }

    // no node was found for extending, so make a new one
    struct vmem_node* newnode = (struct vmem_node*)kalloc(sizeof(struct vmem_node));
    zero(newnode);
    newnode->base = virt;
    newnode->length = size;
#if VMEM_VERBOSE > 2
    fprintf(stderr, "    inserting new vmem_node 0x%lX-0x%lX\n", newnode->base, newnode->base+newnode->length);
#endif
    RB_TREE_INSERT(vmem->free_areas, newnode, _vmem_node_cmp_bases);

done:
    release_lock(vmem->lock);

    ret = paging_unmap_page(vmem->page_table, virt); // unmap the first page to get the physical return address
    for(u64 offs = PAGE_SIZE; offs < size; offs += PAGE_SIZE) {
        paging_unmap_page(vmem->page_table, virt + offs);
    }

    return ret;
}


