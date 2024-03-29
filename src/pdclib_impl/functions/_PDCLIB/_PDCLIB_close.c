/* _PDCLIB_close( _PDCLIB_fd_t )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* This is an example implementation of _PDCLIB_close() fit for use with POSIX
   kernels.
*/

#include <stdio.h>

#ifndef REGTEST

#include "pdclib/_PDCLIB_glue.h"

#include "kernel/common.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int close( int fd );

#ifdef __cplusplus
}
#endif

int _PDCLIB_close( int fd )
{
    unused(fd);
    fprintf(stderr, "_PDCLIB_close stub\n");
    assert(false, "stub");
    //return close( fd );
    return 0;
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
