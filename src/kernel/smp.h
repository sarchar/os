#ifndef __SMP_H__
#define __SMP_H__

void smp_init();

// spinlocks
struct spinlock {
    u8 _v;
} __packed;

#define declare_spinlock(t) struct spinlock t = { 0 }

static inline void spinlock_acquire(struct spinlock* lock)
{
    while(true) {
        // try to acquire the lock by swapping 1 in. The return value will be 1 if its already locked, 0 otherwise
        if(__xchgb(&lock->_v, 1) == 0) return;
        // if the lock was 1, wait until it's not
        while(lock->_v) __pause_barrier();
    }
}

static inline void spinlock_release(struct spinlock* lock)
{
    __barrier();
    lock->_v = 0;
}

static inline bool spinlock_tryacquire(struct spinlock* lock)
{
    return __xchgb(&lock->_v, 1) == 0;
}

#endif
