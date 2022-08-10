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

#define declare_spinlock(n) struct spinlock n = { ._v = 0, ._f = &spinlock_functions }

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

#define declare_ticketlock(n) struct ticketlock n = { ._v = 0, ._f = &ticketlock_functions }

#endif
