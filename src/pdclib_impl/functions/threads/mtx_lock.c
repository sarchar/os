/* mtx_lock( void )

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

/* Implicitly casting the parameter. */
extern int pthread_mutex_lock( mtx_t * );

#ifdef __cplusplus
}
#endif

//extern struct _PDCLIB_file_t* stderr;

int mtx_lock( mtx_t * mtx )
{
    // we have to implement a recursive lock from our non-recursive kernel mutex. 
    // that's not too difficult -- we just have to keep track of the owning task id
    // and keep track of how many locks/unlocks occur
    struct _internal_mtx_t* imtx = (struct _internal_mtx_t*)mtx;
    struct mutex* kmutex = (struct mutex*)imtx->mutex_lock;

    if(!_ap_all_go) { // without SMP/multithreading, there are no other threads, we can just not use the mutex
        return thrd_success;
    }

    // with multithreading, we need to support recursive locks, so we keep track of when we obtain the lock
    // and the # of times it's obtained
    bool locked = try_lock((*kmutex));
    if(locked) {
        // lock was acquired
        assert(imtx->lock_count == 0, "lock_count should have been 0 here");
        imtx->owner_task_id = get_cpu()->current_task->task_id;
        imtx->lock_count = 1;
    } else {
        // couldn't obtain the lock. if we're the owner of said lock, increment count and continue, otherwise block
        if(get_cpu()->current_task->task_id == imtx->owner_task_id) {
            __atomic_inc(&imtx->lock_count);
        } else {
            // block since we're not the owner of the lock
            acquire_lock((*kmutex)); // will allow other tasks to run and eventually give us the lock
            assert(imtx->lock_count == 0, "lock_count should have been 0 here");
            imtx->owner_task_id = get_cpu()->current_task->task_id;
            imtx->lock_count = 1;
        }
    }

    return thrd_success;
}

#endif

#ifdef TEST

#include "_PDCLIB_test.h"

#ifndef REGTEST

#define COUNT 10
thrd_t g_thread[COUNT];
mtx_t g_mutex;

static int func( void * arg )
{
    TESTCASE( mtx_lock( &g_mutex ) == thrd_success );
    thrd_yield();
    TESTCASE( mtx_unlock( &g_mutex ) == thrd_success );
    return 0;
}

#endif

int main( void )
{
#ifndef REGTEST
    TESTCASE( mtx_init( &g_mutex, mtx_plain ) == thrd_success );

    for ( unsigned i = 0; i < COUNT; ++i )
    {
        TESTCASE( thrd_create( &g_thread[i], func, NULL ) == thrd_success );
    }

    for ( unsigned i = 0; i < COUNT; ++i )
    {
        TESTCASE( thrd_join( g_thread[i], NULL ) == thrd_success );
    }

#endif
    return TEST_RESULTS;
}

#endif
