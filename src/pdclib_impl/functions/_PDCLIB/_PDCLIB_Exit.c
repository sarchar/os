/* _PDCLIB_Exit( int )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* This is an example implementation of _PDCLIB_Exit() fit for use with POSIX
   kernels.
*/

#include <stdlib.h>
#include <stdio.h>

#ifndef REGTEST

#include "kernel/common.h"

#include "pdclib/_PDCLIB_glue.h"

#ifdef __cplusplus
extern "C" {
#endif

extern _PDCLIB_Noreturn void _exit( int status ) _PDCLIB_NORETURN;

#ifdef __cplusplus
}
#endif

void _PDCLIB_Exit( int status )
{
    unused(status);
    fprintf(stderr, "_PDCLIB_Exit stub\n");
    assert(false, "stub");
    //_exit( status );
    while(1) ; 
}

#endif

#ifdef TEST

#include "_PDCLIB_test.h"

int main( void )
{
#ifndef REGTEST
    int UNEXPECTED_RETURN = 0;
    _PDCLIB_Exit( 0 );
    TESTCASE( UNEXPECTED_RETURN );
#endif
    return TEST_RESULTS;
}

#endif
