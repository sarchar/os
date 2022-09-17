#include "common.h"

#include "apic.h"
#include "cpu.h"
#include "deque.h"
#include "gdt.h"
#include "hashtable.h"
#include "hpet.h"
#include "idt.h"
#include "kernel.h"
#include "paging.h"
#include "palloc.h"
#include "smp.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "task.h"

#define AP_BOOT_PAGE 8

static_assert(sizeof(struct mutex) <= 64, "if struct mutex is larger than 64 bytes, struct _PDCLIB_mtx_t in _PDCLIB_config.h needs to be updated to match");

// defined in linker.ld
extern intp _ap_boot_start, _ap_boot_size;

// defined in ap_boot.asm, where we store the top of stack for the cpu
extern intp _ap_boot_stack_top;
static intp _ap_boot_stack_bottom;
extern intp _ap_page_table;

// synchronization for bootup
bool volatile _ap_boot_ack;
bool volatile _ap_all_go;
bool volatile _ap_all_stop;

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
        _ap_boot_stack_bottom = task_allocate_stack((intp)null, &stack_size, false); // vmem=null means kernel virtual memory
        *(u64*)&_ap_boot_stack_top = _ap_boot_stack_bottom + stack_size; // 4096*2^2 = 16KiB

        // tell the bootstrap code what to use for the kernel page table
        *(u64*)&_ap_page_table = paging_get_cpu_table(PAGING_KERNEL);

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
//    _ap_gdt_fixup((intp)&_kernel_vma_base);
    _ap_all_go = true;

    // enable the timer on the BSP too
    apic_enable_local_apic_timer();

    fprintf(stderr, "smp: done\n");
}

void smp_all_stop()
{
    _ap_all_stop = true;
}

static void _create_cpu(u8 cpu_index)
{
    struct cpu* cpu = (struct cpu*)malloc(sizeof(struct cpu));
    zero(cpu);
    cpu->this = cpu;
    cpu->cpu_index = cpu_index;

    // switch to the kernel gdt. changing the gdt messes with GSBase, so
    // we have to do this before set_cpu(), which has to happen before, like, everything else
    gdt_install(cpu_index);

    // set the cpu struct in KernelGSBase
    set_cpu(cpu);
    __swapgs(); // put cpu struct into GSBase

    // tell apic so that this cpu structure is available to other cpus
    apic_set_cpu();

    // we need a tss stack so the kernel can switch from ring 3 to 0 safely
    cpu->tss_stack_bottom = palloc_claim(2); // allocate 16KiB for stack
    gdt_set_tss_rsp0(cpu->tss_stack_bottom + (1 << 14));

    // initialize the ipcall lock
    declare_ticketlock(lock_init);
    cpu->ipcall_lock = lock_init;
    cpu->ipcall = null;

    // "become" the currently running task
    task_become();
}

extern struct mutex test_mutex;

__noreturn void ap_main(u8 cpu_index)
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

    // enable interrupts
    idt_install(); // load the idt for this cpu
    apic_initialize_local_apic(); // enable the local APIC
    __sti(); // enable interrupts

    // enable the local apic timer (and thus preemptive multitasking)
    apic_enable_local_apic_timer();

    // go do kernel work and never return
    kernel_do_work();
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
    .wait    = null,
    .notify  = null,
    .end     = null,
};

static void ticketlock_acquire(struct ticketlock* tkt)
{
    u32 me = __atomic_xadd(&tkt->users, (u32)1);
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
    u64 cmp    = ((u64)me << 32) | me;
    u64 cmpnew = ((u64)next << 32) | me;

    // essentially check if there are no open locks, and if so, add 1 to both the ticket and users
    __barrier();
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
    .wait    = null,
    .notify  = null,
    .end     = null,
};

static void mutex_acquire(struct mutex* m)
{
    wait_condition(m->unlock); // mutex condition signals start at 1, so this returns immediately on the first call
                               // subsequent calls block on the wait condition
    return;
}

static void mutex_release(struct mutex* m)
{
    notify_condition(m->unlock);
}

static bool mutex_trylock(struct mutex* m)
{
    return try_lock(m->unlock);
}

static bool mutex_canlock(struct mutex* m)
{
    return can_lock(m->unlock);
}

struct lock_functions mutexlock_functions = {
    .acquire = (void(*)(intp))&mutex_acquire,
    .release = (void(*)(intp))&mutex_release,
    .trylock = (bool(*)(intp))&mutex_trylock,
    .canlock = (bool(*)(intp))&mutex_canlock,
    .wait    = null,
    .notify  = null,
    .end     = null,
};

struct condition_blocked_task {
    struct task* task;
    MAKE_DEQUE(struct condition_blocked_task);
};

static bool condition_trylock(struct condition* cond)
{
    bool res = false;
    acquire_lock(cond->internal_lock);

    __barrier();
    if(cond->waiters < cond->signals) {
        __atomic_inc(&cond->waiters);
        res = true;
    }

    release_lock(cond->internal_lock);
    return res;
}

static bool condition_canlock(struct condition* cond)
{
    return cond->waiters < cond->signals;
}

void condition_wait(struct condition* cond)
{
    // try locking the ticketlock, this means multiple waiters will be served in order
    acquire_lock(cond->internal_lock);
    
    // if the lock has ended, return immediately
    if(cond->signals == (u64)-1) {
        release_lock(cond->internal_lock);
        return;
    }

    // increment the waiter index
    u32 me = __atomic_xinc(&cond->waiters);

    // if there are enough signals pending for us, we can safely return
    __barrier();
    if(me < cond->signals) {
        release_lock(cond->internal_lock);
        return;
    }

    // otherwise, we need to block this task. create a new blocked task structure on the stack,
    // and since this stack stays valid for the duration of the blocked task, we can reference the structure safely
    // in condition_notify
    // TODO: this will probably not be good if waiting tasks are killed or exit somehow
    assert(get_cpu() != null && get_cpu()->current_task, "must be in smp");

    struct condition_blocked_task* bt = (struct condition_blocked_task*)__builtin_alloca(sizeof(struct condition_blocked_task));
    zero(bt);
    bt->task = get_cpu()->current_task;

    // add bt to blocked tasks
    DEQUE_PUSH_BACK(cond->blocked_tasks, bt);

    // prevent preemption between now and the call to task_yield
    u64 cpu_flags = __cli_saveflags();
    release_lock(cond->internal_lock);

    // place the task into a BLOCKED state so that the scheduler doesn't try to run the task until condition_notify wakes it up
    task_yield(TASK_YIELD_WAIT_CONDITION); // when the task wakes up, it means condition_notify has increased the signal count
    assert(me < cond->signals, "must be true");
    __restoreflags(cpu_flags);
}

void condition_notify(struct condition* cond)
{
    acquire_lock(cond->internal_lock);
    if(cond->signals == (u64)-1) {
        release_lock(cond->internal_lock);
        return;
    }

    __atomic_inc(&cond->signals);

    // since we have internal_lock, condition_wait can't modify cond->blocked_tasks
    // so if there are no blocked tasks, we can safely return
    struct condition_blocked_task* bt = cond->blocked_tasks;
    DEQUE_POP_FRONT(cond->blocked_tasks, bt);
    if(bt == null) { // no waiting tasks, return
        release_lock(cond->internal_lock);
        return;
    }

    // safe to release the condition lock now
    release_lock(cond->internal_lock);

    // we don't have to verify that task->state has became BLOCKED before calling task_unblock:
    while(*(enum TASK_STATE volatile*)&bt->task->state != TASK_STATE_BLOCKED) __pause();

    // if the blocked task is on *this* cpu, task_yield would have been called without a race condition
    // if the blocked task was on another cpu, it may not have entered task_yield yet, but 
    // it's interrupts must be disabled (due to the __cli_saveflags above), and so an IPI
    // won't be received until task_yield has executed
    task_unblock(bt->task); // IPI for non-local tasks
}

// wake up all blocked threads and prevent any more from getting blocked
static void condition_end(struct condition* cond)
{
    acquire_lock(cond->internal_lock);
    cond->signals = (u64)-1;

    // since we have internal_lock, condition_wait can't modify cond->blocked_tasks
    // so if there are no blocked tasks, we can safely return
    struct condition_blocked_task* bt = cond->blocked_tasks;
    DEQUE_POP_FRONT(cond->blocked_tasks, bt);
    while(bt != null) {
        while(*(enum TASK_STATE volatile*)&bt->task->state != TASK_STATE_BLOCKED) __pause();
        task_unblock(bt->task); // IPI for non-local tasks
        DEQUE_POP_FRONT(cond->blocked_tasks, bt);
    }

    release_lock(cond->internal_lock);
}

struct lock_functions conditionlock_functions = {
    .acquire = null,
    .release = null,
    .trylock = (bool(*)(intp))&condition_trylock,
    .canlock = (bool(*)(intp))&condition_canlock,
    .wait    = (void(*)(intp))&condition_wait,
    .notify  = (void(*)(intp))&condition_notify,
    .end     = (void(*)(intp))&condition_end,
};

