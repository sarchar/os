/* _PDCLIB_morecore( _PDCLIB_fd_t )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* This is the implementation of morecore() for my OS project */

#include <stdio.h>

#ifndef REGTEST

#include "pdclib/_PDCLIB_glue.h"

#include "kernel/common.h"
#include "kernel/palloc.h"
#include "kernel/paging.h"
#include "kernel/vmem.h"

static intp morecore_lastcall_end = (intp)-1;

void* _PDCLIB_morecore( _PDCLIB_intptr_t size )
{
//    fprintf(stderr, "_PDCLIB_morecore(0x%lX/%ld)\n", size, size);

    if(size == 0) { // return the end of the current program break
//        fprintf(stderr, "_PDCLIB_morecore: morecore_lastcall_end = 0x%lX\n", morecore_lastcall_end);

        // if no previous call exists, return an error
        if(morecore_lastcall_end == (intp)-1) errno = ENOMEM;

        // return the end of the previous allocation, which might be more than the requested size
        return (void*)morecore_lastcall_end;
    } else if(size > 0) {
        // determine the number of requested pages
        intp npages = (intp)__alignup(size, PAGE_SIZE) >> PAGE_SHIFT;

        // figure out the page order to allocate, as this allocation will be contiguous
        u8 order = 0;
        if(npages > 1) order = next_power_of_2(npages);

        // claim pages
        intp phys = palloc_claim(order);

        // map physical pages into kernel virtual memory
        intp virt = vmem_map_pages(VMEM_KERNEL, phys, 1 << order, MAP_PAGE_FLAG_WRITABLE);

        // calculate end of allocated region
        morecore_lastcall_end = virt + ((1 << order) << PAGE_SHIFT);

        // return start of newly allocated region
        fprintf(stderr, "_PDCLIB_morecore: alloc size=%ld virt=0x%lX morecore_lastcall_end=0x%lX\n", size, virt, morecore_lastcall_end);
        return (void*)virt;
    } else {
        assert(false, "can't call _PDCLIB_morecore with negative argument yet");

        errno = ENOMEM;
        return (void*)-1;
    }
}

#endif

#ifdef TEST

#include "_PDCLIB_test.h"

int main( void )
{
    /* No testdriver; tested in driver for _PDCLIB_open(). */
    return TEST_RESULTS;
}

#endif
