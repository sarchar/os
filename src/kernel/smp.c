#include "common.h"

#include "apic.h"
#include "cpu.h"
#include "hashtable.h"
#include "hpet.h"
#include "idt.h"
#include "kalloc.h"
#include "kernel.h"
#include "paging.h"
#include "palloc.h"
#include "smp.h"
#include "stdio.h"
#include "string.h"
#include "task.h"

#define AP_BOOT_PAGE 8

// defined in linker.ld
extern intp _ap_boot_start, _ap_boot_size;

// defined in ap_boot.asm, where we store the top of stack for the cpu
extern intp _ap_boot_stack_top;
static intp _ap_boot_stack_bottom;

// synchronization for bootup
bool volatile _ap_boot_ack;
bool volatile _ap_all_go;

// for GDT fixup
void _ap_gdt_fixup(intp kernel_vma_base);
void _ap_reload_gdt(intp kernel_vma_base);

static void _create_cpu(u8 cpu_index);

// only called once on the BSP
void smp_init()
{
    u64 tmp;
    u32 bspcpu = apic_current_cpu_index();
    u32 ncpus = apic_num_local_apics();

    fprintf(stderr, "smp: init %d cpus _ap_boot_start=0x%lX _ap_boot_size=0x%lX\n", ncpus, &_ap_boot_start, &_ap_boot_size);

    // copy over the trampoline
    memcpy((void*)(AP_BOOT_PAGE * PAGE_SIZE), (void*)&_ap_boot_start, (u64)&_ap_boot_size);
    _ap_all_go = false;

    // loop over each cpu and boot it, waiting for ack before booting the next
    for(u32 i = 0; i < ncpus; i++) {
        // the only thing we need to do for the BSP is create the cpu structure
        if(i == bspcpu) {
            _create_cpu(i);
            continue;
        }

        // start boot ACK at 0
        _ap_boot_ack = false;

        // allocate stack, pointing to the end of memory
        u64 stack_size;
        _ap_boot_stack_bottom = task_allocate_stack(&stack_size);
        *(u64*)&_ap_boot_stack_top = _ap_boot_stack_bottom + stack_size; // 4096*2^2 = 16KiB

        // try to boot the cpu
        if(apic_boot_cpu(i, AP_BOOT_PAGE) < 0) {
            fprintf(stderr, "smp: couldn't boot cpu %d\n", i);
            palloc_abandon((intp)&_ap_boot_stack_top, 2);
            continue;
        }

        // wait for ACK from the AP
        wait_until_true(_ap_boot_ack, 1000000, tmp) {
            // timed out starting CPU
            fprintf(stderr, "smp: timed out starting cpu %d\n", i, i);
            assert(false, "");
        } else {
            fprintf(stderr, "smp: cpu %d started\n", i);
        }
    }

    // gdt has to be fixed up to use _kernel_vma_base before switching the AP page tables and interrupts over to highmem
    _ap_gdt_fixup((intp)&_kernel_vma_base);
    _ap_all_go = true;

    // enable the timer on the BSP too
    apic_enable_local_apic_timer();

    fprintf(stderr, "smp: done\n");
}

static void _create_cpu(u8 cpu_index)
{
    struct cpu* cpu = (struct cpu*)kalloc(sizeof(struct cpu));
    zero(cpu);
    cpu->this = cpu;
    cpu->cpu_index = cpu_index;

    // set the cpu struct in KernelGSBase
    set_cpu(cpu);
    __swapgs(); // put cpu struct into GSBase

    // tell apic so that this cpu structure is available to other cpus
    apic_set_cpu();

    // initialize the lock
    declare_ticketlock(lock_init);
    cpu->ipcall_lock = lock_init;
    cpu->ipcall = null;

    // "become" the currently running task
    task_become();
}

extern struct mutex test_mutex;

static s64 idle(struct task* task) { unused(task); while(true) __hlt(); return 0; }

void ap_start(u8 cpu_index)
{
    // from here until _ap_all_go is set, all other CPUs are in a spinlock so we have safe access to the entire system
    //fprintf(stderr, "ap%d: started\n", cpu_index);

    // initialize our cpu struct
    _create_cpu(cpu_index);

    struct cpu* cpu = get_cpu();
    assert(cpu->cpu_index == cpu_index, "GSBase not working");

    // save stack bottom
    cpu->current_task->stack_bottom = _ap_boot_stack_bottom;

    // tell the BSP that we're ready and wait for the all-go signal
    _ap_boot_ack = true;
    while(!_ap_all_go) asm volatile("pause");

    // reload gdt to use upper memory address
    _ap_reload_gdt((intp)&_kernel_vma_base);

    // set the kernel page table
    paging_set_kernel_page_table();

    // enable interrupts
    idt_install(); // load the idt for this cpu
    apic_initialize_local_apic(); // enable the local APIC
    __sti(); // enable interrupts

    // enable the local apic timer (and thus preemptive multitasking)
    apic_enable_local_apic_timer();

    // I think this could probably just run task_exit(), and then
    // the scheduler will sit idle until there's stuff to run

    // need to have another task running so that this core doesn't go idle
    task_enqueue(&cpu->current_task, task_create(&idle, (intp)null));

    // damnit task_exit is crashing again and I don't know why
    task_set_priority(-20); while(1) __hlt(); // temp

    task_exit(0, false);
}

static void spinlock_acquire(struct spinlock* lock)
{
    while(true) {
        // try to acquire the lock by swapping 1 in. The return value will be 1 if its already locked, 0 otherwise
        if(__xchgb(&lock->_v, 1) == 0) return;

        // if the lock value was 1, wait until it's not
        while(lock->_v) __pause_barrier();
    }
}

static void spinlock_release(struct spinlock* lock)
{
    __barrier();
    lock->_v = 0;
}

static bool spinlock_trylock(struct spinlock* lock)
{
    return __xchgb(&lock->_v, 1) == 0;
}

static bool spinlock_canlock(struct spinlock* lock)
{
    struct spinlock copy = *lock;
    __barrier();
    return copy._v == 0;
}

struct lock_functions spinlock_functions = {
    .acquire = (void(*)(intp))&spinlock_acquire,
    .release = (void(*)(intp))&spinlock_release,
    .trylock = (bool(*)(intp))&spinlock_trylock,
    .canlock = (bool(*)(intp))&spinlock_canlock,
};

static void ticketlock_acquire(struct ticketlock* tkt)
{
    u16 me = __atomic_xadd(&tkt->users, 1);
    while(tkt->ticket != me) __pause_barrier();
}

static void ticketlock_release(struct ticketlock* tkt)
{
    __barrier();
    tkt->ticket++;
}

static bool ticketlock_trylock(struct ticketlock* tkt)
{
    u32 me = tkt->users;
    u32 next = me + 1;

    // TODO this may not work on big endian
    u64 cmp = ((u64)me << 32) | me;
    u64 cmpnew = ((u64)next << 32) | me;

    // essentially check if there are no open locks, and if so, add 1 to both the ticket and users
    if(__compare_and_exchange(&tkt->_v, cmp, cmpnew) == cmp) return true;

    return false;
}

static bool ticketlock_canlock(struct ticketlock* tkt)
{
    struct ticketlock copy = *tkt;
    __barrier();
    return (copy.users == copy.ticket);
}

struct lock_functions ticketlock_functions = {
    .acquire = (void(*)(intp))&ticketlock_acquire,
    .release = (void(*)(intp))&ticketlock_release,
    .trylock = (bool(*)(intp))&ticketlock_trylock,
    .canlock = (bool(*)(intp))&ticketlock_canlock,
};

struct mutex_blocked_task {
    MAKE_HASH_TABLE;
    u16 user_id;
    u16 unused0;
    u32 unused1;
    struct task* task;
};

static void mutex_acquire(struct mutex* m)
{
    if(try_lock(m->lock)) return;

    // couldn't acquire lock, so get a ticket
    acquire_lock(m->internal_lock);

    // create a new blocked task
    struct mutex_blocked_task* bt = (struct mutex_blocked_task*)__builtin_alloca(sizeof(struct mutex_blocked_task));
    bt->user_id = __atomic_xadd(&m->lock.users, 1);
    bt->task = get_cpu()->current_task;

    // can't get the lock, so this task now must be blocked
    // place it in an empty slot
    HT_ADD(m->blocked_tasks, user_id, bt);

    m->num_blocked_tasks++;
    release_lock(m->internal_lock);

    // if the task wakes up, it means mutex_release believes we're the proper owner of the next ticket
    task_yield(TASK_YIELD_MUTEX_BLOCK);

    // remove us from the hash table
    acquire_lock(m->internal_lock);
    assert(m->lock.ticket == bt->user_id, "why did we wake up??");
    HT_DELETE(m->blocked_tasks, bt);
    release_lock(m->internal_lock);
}

static void mutex_release(struct mutex* m)
{
    struct task* unblock_task;

    acquire_lock(m->internal_lock); // lock the internal lock first, so that we can unblock some tasks before any other acquire happens
    release_lock(m->lock);

    __barrier();
    u16 next_ticket = m->lock.ticket;

    struct mutex_blocked_task* bt;
    HT_FIND(m->blocked_tasks, next_ticket, bt);
    release_lock(m->internal_lock);

    if(bt != null) {
        unblock_task = bt->task;
        release_lock(m->internal_lock);
        task_unblock(unblock_task);
    } else {
        // no task found
        release_lock(m->internal_lock);
    }
}

static bool mutex_trylock(struct mutex* m)
{
    return try_lock(m->lock);
}

static bool mutex_canlock(struct mutex* m)
{
    return can_lock(m->lock);
}

struct lock_functions mutexlock_functions = {
    .acquire = (void(*)(intp))&mutex_acquire,
    .release = (void(*)(intp))&mutex_release,
    .trylock = (bool(*)(intp))&mutex_trylock,
    .canlock = (bool(*)(intp))&mutex_canlock,
};

