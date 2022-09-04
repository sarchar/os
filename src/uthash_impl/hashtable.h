#ifndef __HASHTABLE_H__
#define __HASHTABLE_H__

#define uthash_fatal(msg) PANIC(COLOR(255,0,255))
#define uthash_malloc(sz) malloc(sz)
#define uthash_free(ptr,sz) free(ptr)

#include "uthash/src/uthash.h"

#define MAKE_HASH_TABLE \
    UT_hash_handle hh

#define HT_OVERHEAD sizeof(UT_hash_handle)

// support for sizeof() types
// you must be absolutely consistent about the type being used for the key, so that sizeof() is always correct
#define HT_ADD(head,field,add)    HASH_ADD(hh, head, field, sizeof((head)->field), add)
#define HT_DELETE(head,itm)       HASH_DELETE(hh, head, itm)
#define HT_FIND(head,findval,out) HASH_FIND(hh, head, &findval, sizeof(findval), out)

// loop over all the elements in the table
// tbl x and next should all be the same type
// this version allows for safe freeing/removable of x from tbl.
#define HT_FOR_EACH(tbl, x, next) HASH_ITER(hh, tbl, x, next)

#endif
