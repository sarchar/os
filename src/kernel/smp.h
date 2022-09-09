#ifndef __SMP_H__
#define __SMP_H__

void smp_init();
void smp_all_stop();

extern bool volatile _ap_all_go;
static __always_inline bool smp_ready() { return _ap_all_go; }

// generic locking functions
#define acquire_lock(lock) lock._f->acquire((intp)&lock)
#define release_lock(lock) lock._f->release((intp)&lock)
#define try_lock(lock)     lock._f->trylock((intp)&lock)
#define can_lock(lock)     lock._f->canlock((intp)&lock)

// TODO rwlocks. generic locking functions can assert if the lock doesn't support rwlocking
// the rwticket lock from http://locklessinc.com/articles/locks/ looks good for my purposes

struct lock_functions {
    void (*acquire)(intp);
    void (*release)(intp);
    bool (*trylock)(intp);
    bool (*canlock)(intp);
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

// mutexes (only applicable in tasks TODO and user space)
struct task;
struct mutex_blocked_task;

struct mutex {
    struct ticketlock      lock;
    struct spinlock        internal_lock;
    struct lock_functions* _f;

    // hash table maps a user id into a task, so we know which task to unblock
    struct mutex_blocked_task* blocked_tasks;
    u32 num_blocked_tasks;
};

extern struct lock_functions mutexlock_functions;

#define MUTEX_INITIALIZER {                           \
        .lock              = TICKETLOCK_INITIALIZER,  \
        .internal_lock     = SPINLOCK_INITIALIZER,    \
        .blocked_tasks     = null,                    \
        .num_blocked_tasks = 0,                       \
        ._f                = &mutexlock_functions,    \
    }

#define declare_mutex(n) struct mutex n = MUTEX_INITIALIZER

#endif
