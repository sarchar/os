#ifndef __SMP_H__
#define __SMP_H__

void smp_init();
void smp_all_stop();

extern bool volatile _ap_all_go;
static __always_inline bool smp_ready() { return _ap_all_go; }

// generic locking functions
#define acquire_lock(lock)     (lock)._f->acquire((intp)&(lock))
#define release_lock(lock)     (lock)._f->release((intp)&(lock))
#define try_lock(lock)         (lock)._f->trylock((intp)&(lock))
#define can_lock(lock)         (lock)._f->canlock((intp)&(lock))
#define wait_condition(lock)   (lock)._f->wait((intp)&(lock))
#define notify_condition(lock) (lock)._f->notify((intp)&(lock))

// TODO rwlocks. generic locking functions can assert if the lock doesn't support rwlocking
// the rwticket lock from http://locklessinc.com/articles/locks/ looks good for my purposes

struct lock_functions {
    void (*acquire)(intp);
    void (*release)(intp);
    bool (*trylock)(intp);
    bool (*canlock)(intp);
    void (*wait)(intp);
    void (*notify)(intp);
};

// spinlocks
struct spinlock {
    u8 _v;
    struct lock_functions* _f;
};

extern struct lock_functions spinlock_functions;

#define SPINLOCK_INITIALIZER { ._v = 0, ._f = &spinlock_functions }
#define declare_spinlock(n) struct spinlock n = SPINLOCK_INITIALIZER

// ticketlocks
struct ticketlock {
    union {
        u64 _v;
        struct {
            u32 ticket;
            u32 users;
        };
    };

    struct lock_functions* _f;
};

extern struct lock_functions ticketlock_functions;

#define TICKETLOCK_INITIALIZER { ._v = 0, ._f = &ticketlock_functions }
#define declare_ticketlock(n) struct ticketlock n = TICKETLOCK_INITIALIZER

// conditions aka semaphores (only applicable in tasks)
// implements: wait_condition, notify_condition, trylock, canlock

struct condition_blocked_task;
struct condition {
    struct ticketlock              internal_lock;
    u64                            waiters;
    u64                            signals;
    struct condition_blocked_task* blocked_tasks;
    struct lock_functions*         _f;
};

#define CONDITION_INITIALIZER(S) {                      \
        .internal_lock     = TICKETLOCK_INITIALIZER,    \
        .waiters           = 0,                         \
        .signals           = (S),                       \
        .blocked_tasks     = null,                      \
        ._f                = &conditionlock_functions,  \
    }

// can use declare_condition_S if you wish to initialize the signal count to something non-zero
#define declare_condition(n) struct condition n = CONDITION_INITIALIZER(0)
#define declare_condition_S(n, S) struct condition n = CONDITION_INITIALIZER(S)

extern struct lock_functions conditionlock_functions;

// mutexes (only applicable in tasks)
// builds upon condition signals so that only one task uses a mutex at a given time
// implements: acquire, release, trylock, canlock
struct task;

struct mutex {
    struct condition       unlock;
    struct lock_functions* _f;
};

extern struct lock_functions mutexlock_functions;

#define MUTEX_INITIALIZER {                             \
        .unlock            = CONDITION_INITIALIZER(1),  \
        ._f                = &mutexlock_functions,      \
    }

#define declare_mutex(n) struct mutex n = MUTEX_INITIALIZER

#endif
