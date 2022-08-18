/* mtx_unlock( mtx_t * mtx )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

#ifndef REGTEST

#include <stdio.h>
#include <threads.h>

#include "kernel/common.h"
#include "kernel/cpu.h"
#include "kernel/smp.h"
#include "kernel/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Implicity casting the parameter. */
extern int pthread_mutex_unlock( mtx_t * );

#ifdef __cplusplus
}
#endif

//extern struct _PDCLIB_file_t* stderr;

int mtx_unlock( mtx_t * mtx )
{
    struct _internal_mtx_t* imtx = (struct _internal_mtx_t*)mtx;

    if(_ap_all_go && __atomic_dec(&imtx->lock_count) == 0) {
        assert(imtx->owner_task_id == get_cpu()->current_task->task_id, "what");
        struct mutex* kmutex = (struct mutex*)imtx->mutex_lock;
        imtx->owner_task_id = (unsigned long long)-1;
        release_lock((*kmutex));
    }

    return thrd_success;
}

#endif

#ifdef TEST

#include "_PDCLIB_test.h"

int main( void )
{
    /* Tested by the mtx_lock test driver. */
    return TEST_RESULTS;
}

#endif
