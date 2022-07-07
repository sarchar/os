#include "common.h"

#include "cpu.h"
#include "kernel.h"
#include "paging.h"
#include "palloc.h"
#include "stdio.h"

static u64**** page_table_level_4;

void paging_init()
{
    // TODO build a new page table using memory allocated from palloc and switch to it
    page_table_level_4 = (u64****)palloc_claim_one(); // 4096 / sizeof(u64) = 512
    assert((intp)page_table_level_4 < 0x100000000, "TODO for now, page table must be in low mem");
    memset64(page_table_level_4, 0, 512);

    // set up a mapping to the first 4GiB of memory (0-0xFFFFFFFF) using level 2 1GiB huge pages
    page_table_level_4[0] = (u64***)palloc_claim_one();
    assert((intp)page_table_level_4[0] < 0x100000000, "TODO for now, page table must be in low mem");
    memset64(page_table_level_4[0], 0, 512);

    // starting at address 0, fill out four level 2 page table directories
    u64 base = 0;
    for(u32 i = 0; i < 4; i++) {   // need four level 2 tables, each covering 1GiB
        // ptd is actually a level 2 table, but using huge pages we don't assign pointers
        u64* ptd = (u64*)palloc_claim_one();
        assert((intp)ptd < 0x100000000, "TODO for now, page table must be in low mem");
        for(u32 j = 0; j < 512; j++) {
            ptd[j] = base | 0x83;  // huge page, present, writable
            base += 512 * 4096;    // using huge pages, one entry is 2MiB in size
        }

        // place it into the level 3 page table
        page_table_level_4[0][i] = (u64**)((intp)ptd | 0x03);
    }

    // to map the addresses from 0xFFFF_FFFE_0000_0000-0xFFFF_FFFE_FFFF_FFFF, we need another level 3 page table
    // in the very last (0xFF80_0000_0000) block of the level 4 page table
    page_table_level_4[511] = (u64***)palloc_claim_one();
    assert((intp)page_table_level_4[511] < 0x100000000, "TODO for now, page table must be in low mem");

    // then we can just reuse the page tables from the loop above at index 504-7
    page_table_level_4[511][504] = page_table_level_4[0][0];
    page_table_level_4[511][505] = page_table_level_4[0][1];
    page_table_level_4[511][506] = page_table_level_4[0][2];
    page_table_level_4[511][507] = page_table_level_4[0][3];

    // TODO and a ghetto fixup for present, writable bits
    page_table_level_4[0]   = (u64***)((intp)page_table_level_4[0] | 0x03);
    page_table_level_4[511] = (u64***)((intp)page_table_level_4[511] | 0x03);

    // and finally load the page table address into 
    __wrcr3((u64)page_table_level_4);

    fprintf(stderr, "paging: initialized (page_table_level_4=$%lX)\n", page_table_level_4);
}

