#ifndef __SMP_H__
#define __SMP_H__

void smp_init();

// generic locking functions
#define acquire_lock(lock) lock._f->acquire((intp)&lock)
#define release_lock(lock) lock._f->release((intp)&lock)
#define try_lock(lock)     lock._f->trylock((intp)&lock)
#define canlock_lock(lock) lock._f->canlock((intp)&lock)

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
} __packed;

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
} __packed;

extern struct lock_functions ticketlock_functions;

#define TICKETLOCK_INITIALIZER { ._v = 0, ._f = &ticketlock_functions }
#define declare_ticketlock(n) struct ticketlock n = TICKETLOCK_INITIALIZER

// mutexes (only applicable in tasks TODO and user space)
struct task;
struct mutex {
    struct spinlock lock;
    struct spinlock internal_lock;
    struct lock_functions* _f;

    // this lock always has 16 slots for waiting tasks, but can dynamically grow (TODO some limit) if necessary
    struct task*  blocked_tasks[16];
    struct task** blocked_tasks_dyn;
    u32           blocked_tasks_dyn_count;
    u32           num_blocked_tasks;
} __packed;

extern struct lock_functions mutexlock_functions;

#define MUTEX_INITIALIZER {                    \
        .lock = SPINLOCK_INITIALIZER,          \
        .internal_lock = SPINLOCK_INITIALIZER, \
        .blocked_tasks = { null, },            \
        .blocked_tasks_dyn = null,             \
        .blocked_tasks_dyn_count = 0,          \
        ._f = &mutexlock_functions,            \
        .num_blocked_tasks = 0,                \
    }

#define declare_mutex(n) struct mutex n = MUTEX_INITIALIZER

#endif
