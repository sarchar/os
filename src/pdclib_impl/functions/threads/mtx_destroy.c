/* mtx_destroy( mtx_t * )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

#ifndef REGTEST

#include <threads.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "kernel/common.h"

/* Implicitly casting the parameter. */
//extern int pthread_mutex_destroy( mtx_t * );

#ifdef __cplusplus
}
#endif

void mtx_destroy( mtx_t * mtx )
{
    fprintf(stderr, "mtx_destroy stub\n");
    assert(false, "stub");
    //pthread_mutex_destroy( mtx );
}

#endif

#ifdef TEST

#include "_PDCLIB_test.h"

int main( void )
{
#ifndef REGTEST
    TESTCASE( NO_TESTDRIVER );
#endif
    return TEST_RESULTS;
}

#endif
