/* _PDCLIB_remove( const char * )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* This is an example implementation of _PDCLIB_remove() fit for use with
   POSIX kernels.
*/

#include <stdio.h>

#ifndef REGTEST

#include "pdclib/_PDCLIB_glue.h"

#include "kernel/common.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int unlink( const char * );

#ifdef __cplusplus
}
#endif

int _PDCLIB_remove( const char * pathname )
{
    unused(pathname);
    fprintf(stderr, "_PDCLIB_remove stub\n");
    assert(false, "stub");
    //return unlink( pathname );
    return 0;
}

#endif

#ifdef TEST

#include "_PDCLIB_test.h"

int main( void )
{
    /* Testing covered by ftell.c (and several others) */
    return TEST_RESULTS;
}

#endif
