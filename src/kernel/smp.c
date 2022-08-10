#include "common.h"

#include "apic.h"
#include "cpu.h"
#include "hpet.h"
#include "idt.h"
#include "kalloc.h"
#include "kernel.h"
#include "paging.h"
#include "palloc.h"
#include "smp.h"
#include "stdio.h"
#include "string.h"

#define AP_BOOT_PAGE 8

// defined in linker.ld
extern intp _ap_boot_start, _ap_boot_size;

// defined in ap_boot.asm, where we store the top of stack for the cpu
extern intp _ap_boot_stack_top;

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
        *(u64*)&_ap_boot_stack_top = palloc_claim(2) + (1 << 14); // 4096*2^2 = 16KiB

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
}

declare_spinlock(ap_work);

void ap_start(u8 cpu_index)
{
    // from here until _ap_all_go is set, all other CPUs are in a spinlock so we have safe access to the entire system
    fprintf(stderr, "ap%d: started\n", cpu_index);

    // initialize our cpu struct
    _create_cpu(cpu_index);
    assert(get_cpu()->cpu_index == cpu_index, "GSBase not working");

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

    while(1) {
        if(spinlock_tryacquire(&ap_work)) {
            fprintf(stderr, "cpu %d got lock\n", get_cpu()->cpu_index);
            spinlock_release(&ap_work);
        }
        __pause();
    }
}

