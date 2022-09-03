#include "common.h"

#include "apic.h"
#include "bootmem.h"
#include "cpu.h"
#include "hpet.h"
#include "idt.h"
#include "interrupts.h"
#include "kernel.h"
#include "paging.h"
#include "palloc.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "task.h"

#define IA32_APIC_BASE_MSR 0x1B
#define IA32_APIC_BASE_MSR_ENABLE 0x800

#define IO_APIC_ID_REG             0x00
#define IO_APIC_VERSION_REG        0x01
#define IO_APIC_ARBITRATION_REG    0x02
#define IO_APIC_REDIERCTION_REG(n) (0x10 + (n << 1))

#define LOCAL_APIC_TIMER_INTERRUPT  49
#define LOCAL_APIC_IPCALL_INTERRUPT 50

enum LAPIC_REGISTERS {
    LAPIC_REG_LOCAL_APIC_ID                         = 0x20,
    LAPIC_REG_LOCAL_APIC_ID_VERSION                 = 0x30,
    LAPIC_REG_TASK_PRIORITY                         = 0x80,
    LAPIC_REG_ARBITRATION_PRIORITY                  = 0x90,
    LAPIC_REG_PROCESSOR_PRIORITY                    = 0xA0,
    LAPIC_REG_END_OF_INTERRUPT                      = 0xB0,
    LAPIC_REG_LOGICAL_DESTINATION                   = 0xD0,
    LAPIC_REG_DESTINATION_FORMAT                    = 0xE0,
    LAPIC_REG_SPURIOUS_INTERRUPT_VECOTR             = 0xF0,
    LAPIC_REG_IN_SERVICE                            = 0x100,
    LAPIC_REG_TRIGGER_MODE                          = 0x180,
    LAPIC_REG_INTERRUPT_REQUEST                     = 0x200,
    LAPIC_REG_ERROR_STATUS                          = 0x280,
    LAPIC_REG_LVT_CORRECTED_MACHINE_CHECK_INTERRUPT = 0x2F0,
    LAPIC_REG_INTERRUPT_COMMAND_L                   = 0x300,
    LAPIC_REG_INTERRUPT_COMMAND_H                   = 0x310,
    LAPIC_REG_LVT_TIMER                             = 0x320,
    LAPIC_REG_LVT_THERMAL_SENSOR                    = 0x330,
    LAPIC_REG_LVT_PERFORMANCE_MONITORING_COUNTERS   = 0x340,
    LAPIC_REG_LVT_LINT0                             = 0x350,
    LAPIC_REG_LVT_LINT1                             = 0x360,
    LAPIC_REG_LVT_ERROR                             = 0x370,
    LAPIC_REG_INITIAL_COUNT                         = 0x380,
    LAPIC_REG_CURRENT_COUNT                         = 0x390,
    LAPIC_REG_DIVIDE_CONFIGURATION                  = 0x3E0
};

enum LAPIC_INTERRUPT_COMMAND_FIELDS {
    LAPIC_INTERRUPT_COMMAND_STATUS = (1 << 12),
    LAPIC_INTERRUPT_COMMAND_LEVEL  = (1 << 14)
};

enum LAPIC_DELIVERY_MODES {
     LAPIC_DELIVERY_MODE_NORMAL  = 0x00,
     LAPIC_DELIVERY_MODE_INIT    = 0x05,
     LAPIC_DELIVERY_MODE_STARTUP = 0x06,
     LAPIC_DELIVERY_MODE_SHIFT   = 8,
};

#define LOCAL_APIC_LVT_TIMER_PERIODIC (1 << 17)
#define LOCAL_APIC_LVT_MASK_BIT       (1 << 16)

struct local_apic {
    u8     acpi_processor_id;
    u8     apic_id;
    u8     padding0;
    bool   enabled;
    u32    padding1;
    struct cpu* cpu;
}; 

static intp local_apic_base = (intp)-1;
static struct local_apic** local_apics = null;
static u32 num_local_apics = 0;

static struct {
    u8   apic_id;
    u8   global_system_interrupt_base;
    u8   version;
    u8   num_interrupts;
    u32  reserved;
    intp base;
} io_apic = { .base = (intp)-1 };

void _send_lapic_eoi();
static void _local_apic_ipcall_interrupt(struct interrupt_stack_registers*, intp, void*);

static inline bool _has_lapic()
{
    //u64 a, b, c, d;
    //__cpuid(1, &a, &b, &c, &d);
    //return (d & CPUID_FEAT_EDX_APIC) != 0;
    return local_apic_base != (intp)-1;
}

static inline void _set_lapic_base(intp base) 
{
    local_apic_base = base;
    __wrmsr(IA32_APIC_BASE_MSR, (base & 0xFFFFFFFFFFFF000ULL) | IA32_APIC_BASE_MSR_ENABLE);
}

static inline void _write_lapic(u16 reg, u32 value)
{
    *(u32 volatile*)(local_apic_base + reg) = value;
}

static inline void _write_lapic_command(u64 value)
{
    _write_lapic(LAPIC_REG_INTERRUPT_COMMAND_H, value >> 32);
    _write_lapic(LAPIC_REG_INTERRUPT_COMMAND_L, value & 0xFFFFFFFF);
}

static inline void _write_io_apic(u8 reg, u32 value)
{
    // write register number to IOAPICBASE
    *(u32 volatile*)(io_apic.base + 0) = (u32)reg;
    // write data register
    *(u32 volatile*)(io_apic.base + 0x10) = value;
}

static inline u32 _read_io_apic(u8 reg)
{
    // write register number to IOAPICBASE
    *(u32 volatile*)(io_apic.base + 0) = (u32)reg;
    // read data register
    return *(u32 volatile*)(io_apic.base + 0x10);
}

static inline u32 _read_lapic(u16 reg)
{
    return *((u32 volatile*)(local_apic_base + reg));
}

static void _local_apic_timer_interrupt(struct interrupt_stack_registers* regs, intp pc, void* userdata)
{
    unused(regs);
    unused(pc);
    unused(userdata);

    //__cli();
    extern bool volatile _ap_all_stop;
    if(_ap_all_stop) {
        __cli(); // with interrupts disabled, __hlt will never return (except by NMI, etc)
        while(true) __hlt();
    }

    struct cpu* cpu = get_cpu();
    cpu->ticks++;

    // if no tasks are running, can't do any task switching
    if(cpu->current_task == null) return;

    // first add up runtime on current task
    u64 gt = global_ticks;
    cpu->current_task->runtime += (gt - cpu->current_task->last_global_ticks);
    cpu->current_task->last_global_ticks = gt;

    // if there's no other task other than the current one, keep running
    if(cpu->current_task->next == cpu->current_task) return;

    // before switching tasks, send EOI first
    _send_lapic_eoi();

    // then yield. when we return, rflags is popped from the iretq instruction and interrupts are re-enabled
    task_yield(TASK_YIELD_PREEMPT);
}

void apic_initialize_local_apic()
{
    assert(_has_lapic(), "only APIC-supported systems for now");

    // enable it (probably already enabled)
    __wrmsr(IA32_APIC_BASE_MSR, __rdmsr(IA32_APIC_BASE_MSR) | IA32_APIC_BASE_MSR_ENABLE);

    //_write_lapic(LAPIC_REG_DESTINATION_FORMAT, 0x0FFFFFFF);
    _write_lapic(LAPIC_REG_DESTINATION_FORMAT, 0xFFFFFFFF); // flat mode
    //_write_lapic(LAPIC_REG_LOGICAL_DESTINATION, (_read_lapic(LAPIC_REG_LOGICAL_DESTINATION) & 0x00FFFFFF) | 0x01);

    // disable all interrupts for now
    _write_lapic(LAPIC_REG_LVT_PERFORMANCE_MONITORING_COUNTERS, LOCAL_APIC_LVT_MASK_BIT);
    _write_lapic(LAPIC_REG_LVT_THERMAL_SENSOR, LOCAL_APIC_LVT_MASK_BIT);
    _write_lapic(LAPIC_REG_LVT_ERROR, LOCAL_APIC_LVT_MASK_BIT);
//    _write_lapic(LAPIC_REG_LVT_LINT0, LOCAL_APIC_LVT_MASK_BIT);
//    _write_lapic(LAPIC_REG_LVT_LINT1, LOCAL_APIC_LVT_MASK_BIT);
    _write_lapic(LAPIC_REG_LVT_TIMER, LOCAL_APIC_LVT_MASK_BIT);
    _write_lapic(LAPIC_REG_TASK_PRIORITY, 0);

    // enable the lapic and set the spurious interrupt vector to 255
    _write_lapic(LAPIC_REG_SPURIOUS_INTERRUPT_VECOTR, 0x1FF);
}

// must be called with interrupts enabled
static u64 _determine_timer_frequency()
{
    u64 const timing_duration = 250; // in ms

    // disable the timer
    _write_lapic(LAPIC_REG_LVT_TIMER, LOCAL_APIC_LVT_MASK_BIT);

    // set the divider
    u8 divider = 7;
    _write_lapic(LAPIC_REG_DIVIDE_CONFIGURATION, ((divider - 1) & 0x03) | (((divider - 1) & 0x04) << 1)); // weird register

    // set the initial count to -1
    _write_lapic(LAPIC_REG_INITIAL_COUNT, 0xFFFFFFFF);

    // wait for some time, ~250ms
    u64 start = global_ticks;
    while((global_ticks - start) < timing_duration) __barrier();

    // immediately read the current count register
    s32 lapic_timer_count = 0xFFFFFFFF - _read_lapic(LAPIC_REG_CURRENT_COUNT);
    lapic_timer_count = 100000 * ((lapic_timer_count + 99999) / 100000); // round up to the nearest 100k

    return (((s64)lapic_timer_count * (1 << divider)) * 1000) / timing_duration;
}

// must be called with interrupts enabled
void apic_enable_local_apic_timer()
{
    struct cpu* cpu = get_cpu();

    // first measure the timer frequency
    if(cpu->timer_frequency == 0) cpu->timer_frequency = _determine_timer_frequency();

    // use a divider frequency that makes sense
    u8 divider = 4; // divide by 16
    _write_lapic(LAPIC_REG_DIVIDE_CONFIGURATION, ((divider - 1) & 0x03) | (((divider - 1) & 0x04) << 1)); // weird register

    // determine the clock count to get 10ms (100Hz)
    u32 count = (u32)(cpu->timer_frequency / 100);

    // adjust for divider
    count >>= divider;

    // enable the timer
    _write_lapic(LAPIC_REG_LVT_TIMER, LOCAL_APIC_LVT_TIMER_PERIODIC | LOCAL_APIC_TIMER_INTERRUPT);

    // set the count
    _write_lapic(LAPIC_REG_INITIAL_COUNT, count);
}

static void _initialize_ioapic()
{
    // enable keyboard interrupt (IRQ1 == global system interrupt 1) map to cpu irq 33
    apic_set_io_apic_redirection(1, 33,
                             IO_APIC_REDIRECTION_FLAG_DELIVERY_NORMAL,
                             IO_APIC_REDIRECTION_DESTINATION_PHYSICAL,
                             IO_APIC_REDIRECTION_ACTIVE_HIGH,
                             IO_APIC_REDIRECTION_EDGE_SENSITIVE,
                             true,
                             local_apics[0]->apic_id); // TEMP for now send to cpu 1

    // HPET currently configured to trigger global system interrupt 19
    // map it to 48 in the IDT
    apic_set_io_apic_redirection(19, 48,
                             IO_APIC_REDIRECTION_FLAG_DELIVERY_NORMAL,
                             IO_APIC_REDIRECTION_DESTINATION_PHYSICAL,
                             IO_APIC_REDIRECTION_ACTIVE_HIGH,
                             IO_APIC_REDIRECTION_EDGE_SENSITIVE,
                             false, // don't enable interrupts yet
                             local_apics[0]->apic_id);

    apic_io_apic_enable_interrupt(19); // enable the interrupt in the redirection register
}

// https://blog.wesleyac.com/posts/ioapic-interrupts describes the process for getting that first
// keyboard interrupt via the APIC
void apic_init()
{
    apic_initialize_local_apic();
    _initialize_ioapic();

}

struct cpu* apic_get_cpu(u8 cpu_index)
{
    assert(cpu_index < num_local_apics, "index out of range");
    return local_apics[cpu_index]->cpu;
}

u8 apic_get_apic_id(u8 cpu_index)
{
    assert(cpu_index < num_local_apics, "index out of range");
    return local_apics[cpu_index]->apic_id;
}

void apic_set_cpu()
{
    struct cpu* cpu = get_cpu();
    local_apics[cpu->cpu_index]->cpu = cpu;
}

void apic_map()
{
    paging_map_page(PAGING_KERNEL, io_apic.base   , io_apic.base   , MAP_PAGE_FLAG_WRITABLE | MAP_PAGE_FLAG_DISABLE_CACHE);
    paging_map_page(PAGING_KERNEL, local_apic_base, local_apic_base, MAP_PAGE_FLAG_WRITABLE | MAP_PAGE_FLAG_DISABLE_CACHE);

    // TODO move to a location more specific to initializing the BSP lapic
    u32 v = _read_lapic(LAPIC_REG_LOCAL_APIC_ID_VERSION);
    fprintf(stderr, "apic: local apic version=%d max_lvt=%d\n", v & 0xFF, (v >> 16) & 0xFF);

    v = _read_lapic(LAPIC_REG_LOCAL_APIC_ID);
    fprintf(stderr, "apic: local apic id=%d\n", (v >> 24) & 0x0F);

    // install interrupt handlers here, before smp, after interrupts_init().
    // global to all the local apics is the same interrupt handler for the timer
    interrupts_install_handler(LOCAL_APIC_TIMER_INTERRUPT, _local_apic_timer_interrupt, null);

    // install the ipcall interrupt handler
    interrupts_install_handler(LOCAL_APIC_IPCALL_INTERRUPT, _local_apic_ipcall_interrupt, null);
}

// See https://wiki.osdev.org/APIC#IO_APIC_Configuration
void apic_set_io_apic_redirection(u8 io_apic_irq, u8 cpu_irq, u8 delivery_mode, u8 destination_mode, u8 active_level, u8 trigger_mode, bool enabled, u8 destination)
{
    u64 cntrl = ((u64)destination << 56) | ((u64)trigger_mode << 15) | ((u64)active_level << 13) | ((u64)destination_mode << 11) | ((u64)delivery_mode << 8) | (u64)cpu_irq;

    // set IRQ enabled bit if requested
    if(!enabled) {
        cntrl |= 1 << 16;
    }

    // TODO determine which I/O APIC is responsible for the io_apic_irq

    // write 64-bit value to IO_APIC_REDIERCTION_REG corresponding to io_apic_irq
    _write_io_apic(IO_APIC_REDIERCTION_REG(io_apic_irq) + 0, (cntrl & 0xFFFFFFFF)); 
    _write_io_apic(IO_APIC_REDIERCTION_REG(io_apic_irq) + 1, cntrl >> 32);
}

void apic_io_apic_enable_interrupt(u8 io_apic_irq)
{
    u32 low = _read_io_apic(IO_APIC_REDIERCTION_REG(io_apic_irq));
    low &= ~(1 << 16);
    _write_io_apic(IO_APIC_REDIERCTION_REG(io_apic_irq), low);
}

void apic_io_apic_disable_interrupt(u8 io_apic_irq)
{
    u32 low = _read_io_apic(IO_APIC_REDIERCTION_REG(io_apic_irq));
    low |= 1 << 16;
    _write_io_apic(IO_APIC_REDIERCTION_REG(io_apic_irq), low);
}

void apic_notify_acpi_io_apic(u8 io_apic_id, intp io_apic_base, u8 global_system_interrupt_base)
{
    assert(io_apic.base == (intp)-1, "don't notify two I/O APICs"); // TODO?
    io_apic.apic_id = io_apic_id;
    io_apic.base    = io_apic_base;
    io_apic.global_system_interrupt_base = global_system_interrupt_base;

    u32 c = _read_io_apic(IO_APIC_VERSION_REG);
    io_apic.version = c & 0xFF;
    io_apic.num_interrupts = (c >> 16) & 0xFF;

    fprintf(stderr, "apic: I/O APIC id=%d version=0x%X handles interrupts %d..%d\n", io_apic.apic_id, io_apic.version, 
            io_apic.global_system_interrupt_base, io_apic.global_system_interrupt_base+io_apic.num_interrupts);
}

void apic_notify_acpi_io_apic_interrupt_source_override(u8 bus_source, u8 irq_source, u8 global_system_interrupt, u8 flags)
{
    // TODO will one day need to use these overrides
    unused(flags); // TODO - edge triggered, high/low trigger, etc
    unused(bus_source);
    unused(irq_source);
    unused(global_system_interrupt);
    fprintf(stderr, "apic: registering interrupt source override bus=%d irq=%d gsi=%d flags=%d\n", bus_source, irq_source, global_system_interrupt, flags);
}

void apic_notify_acpi_local_apic_base(intp lapic_base, bool system_has_pic)
{
    unused(system_has_pic);
    local_apic_base = lapic_base;

    fprintf(stderr, "apic: local_apic_base at 0x%lX%s\n", lapic_base, system_has_pic ? " (with dual PICs)" : "");
}

void apic_notify_num_local_apics(u32 num_lapics)
{
    fprintf(stderr, "apic: found %d processors\n", num_lapics);

    num_local_apics = num_lapics;
    local_apics = (struct local_apic**)bootmem_alloc(sizeof(intp) * num_lapics, 8);

    for(u32 i = 0; i < num_lapics; i++) {
        local_apics[i] = (struct local_apic*)bootmem_alloc(sizeof(struct local_apic), 8);
        local_apics[i]->acpi_processor_id = -1;
    }
}

void apic_register_processor_lapic(u8 acpi_processor_id, u8 apic_id, bool enabled)
{
    static u32 current_lapic = 0;

    fprintf(stderr, "apic: found Local APIC acpi_processor_id=%d apic_id=%d enabled=%d\n", acpi_processor_id, apic_id, enabled);
        
    if(current_lapic < num_local_apics) {
        local_apics[current_lapic]->acpi_processor_id = acpi_processor_id;
        local_apics[current_lapic]->apic_id = apic_id;
        local_apics[current_lapic]->enabled = enabled;
        current_lapic++;
    }
}

void apic_notify_acpi_lapic_nmis(u8 acpi_processor_id, u8 lint_number, u8 flags)
{
    // TODO register the lint_number in the CPU's local vector table
    // and then map that local interrupt into a cpu interrupt. 
    // See https://wiki.osdev.org/APIC#Local_Vector_Table_Registers
    // It's similar to the I/O APIC redirection table
    // Flags seems to contain edge/level triggered, etc., information
    unused(acpi_processor_id);
    unused(lint_number);
    unused(flags);
}

void _send_lapic_eoi()
{
    _write_lapic(LAPIC_REG_END_OF_INTERRUPT, 0);
}

intp apic_get_lapic_base(u8 lapic_index)
{
    assert(_has_lapic(), "must be initialized before this call");
    assert(lapic_index == 0, "TODO only supports lapic 0 for now");
    return local_apic_base;
}

static inline u64 _build_lapic_command(bool is_physical_address, u8 apic_id, u8 irq_vector, u8 delivery_mode)
{
    assert(is_physical_address, "only physical addresses supported currently");

    // high byte only has the destination apic id
    u32 high_byte = (apic_id << 24);

    // level is always 1 except for INIT de-asserts
    u32 low_byte = LAPIC_INTERRUPT_COMMAND_LEVEL | ((u32)delivery_mode << LAPIC_DELIVERY_MODE_SHIFT) | irq_vector;

    return ((u64)high_byte << 32) | (u64)low_byte;
}

static inline u64 _build_init_ipi_command(u8 destination_apic_id)
{
    return _build_lapic_command(true, destination_apic_id, 0, LAPIC_DELIVERY_MODE_INIT);
}

static inline u64 _build_init_deassert_command(u8 destination_apic_id)
{
    u64 cmd = _build_lapic_command(true, destination_apic_id, 0, LAPIC_DELIVERY_MODE_INIT);
    cmd &= ~LAPIC_INTERRUPT_COMMAND_LEVEL;
    return cmd;
}

// startup_page is the page number to start executing at
static inline u64 _build_startup_command(u8 destination_apic_id, u16 startup_page)
{
    return _build_lapic_command(true, destination_apic_id, startup_page, LAPIC_DELIVERY_MODE_STARTUP);
}

// only called on the bootstrap processor
s64 apic_boot_cpu(u32 cpu_index, u8 boot_page)
{
    assert(cpu_index < num_local_apics, "index out of range");
    struct local_apic* lapic = local_apics[cpu_index];
    u64 tmp;

    // clear our error status
    _write_lapic(LAPIC_REG_ERROR_STATUS, 0);

    // send INIT IPI
    u64 cmd = _build_init_ipi_command(lapic->apic_id);
    _write_lapic_command(cmd);

    // wait for delivery, don't care about timeout
    wait_until_false(_read_lapic(LAPIC_REG_INTERRUPT_COMMAND_L) & LAPIC_INTERRUPT_COMMAND_STATUS, 200000, tmp) {
        fprintf(stderr, "apic: delivery of INIT IPI to cpu %d timed out\n", cpu_index);
        return -1;
    }

    // send INIT de-assert IPI
    cmd = _build_init_deassert_command(lapic->apic_id);
    _write_lapic_command(cmd);

    // wait for delivery, don't care about timeout
    wait_until_false(_read_lapic(LAPIC_REG_INTERRUPT_COMMAND_L) & LAPIC_INTERRUPT_COMMAND_STATUS, 200000, tmp) {
        fprintf(stderr, "apic: delivery of INIT de-assert IPI to cpu %d timed out\n", cpu_index);
        return -1;
    }

    // send the STARTUP IPI twice
    for(u8 j = 0; j < 2; j++) {
        // clear error status
        _write_lapic(LAPIC_REG_ERROR_STATUS, 0);

        // send the command
        cmd = _build_startup_command(lapic->apic_id, boot_page);
        _write_lapic_command(cmd);

        // wait 10ms before checking delivery status
        usleep(10000);

        // wait for delivery, don't care about timeout
        wait_until_false(_read_lapic(LAPIC_REG_INTERRUPT_COMMAND_L) & LAPIC_INTERRUPT_COMMAND_STATUS, 200000, tmp) {
            fprintf(stderr, "apic: delivery of STARTUP IPI to cpu %d timed out\n", cpu_index);
            return -1;
        }
    }

    return 0;
}

u32 apic_current_cpu_index()
{
    u8 my_apic_id = (_read_lapic(LAPIC_REG_LOCAL_APIC_ID) >> 24) & 0x0F;
    
    for(u32 i = 0; i < num_local_apics; i++) {
        if(local_apics[i]->apic_id == my_apic_id) return i;
    }

    assert(false, "bad");
}

u32 apic_num_local_apics()
{
    return num_local_apics;
}

struct ipcall {
    u32   function;
    u32   source_cpu_index;
    void* payload;
};

struct ipcall* apic_ipcall_build(enum IPCALL_FUNCTIONS func, void* payload)
{
    struct ipcall* ipc = malloc(sizeof(struct ipcall));
    ipc->function = (u32)func;
    ipc->payload = payload;
    ipc->source_cpu_index = get_cpu()->cpu_index;
    return ipc;
}

s64 apic_ipcall_send(u32 dest, struct ipcall* sendipc)
{
    struct cpu* dest_cpu = apic_get_cpu(dest);
    u64 tmp;

    while(true) {
        wait_until_true(dest_cpu->ipcall == null, 100000, tmp) {
            // major problem
            fprintf(stderr, "apic: target CPU %d never cleared previous IPCALL\n", dest);
            return -1;
        }

        acquire_lock(dest_cpu->ipcall_lock);
        if(dest_cpu->ipcall != null) {
            // another cpu beat us to the punch, try again
            release_lock(dest_cpu->ipcall_lock);
            continue;
        }

        break;
    }

    // now we have a lock and ipcall is null
    dest_cpu->ipcall = sendipc;
    release_lock(dest_cpu->ipcall_lock);
    
    u64 cmd = _build_lapic_command(true, local_apics[dest]->apic_id, LOCAL_APIC_IPCALL_INTERRUPT, LAPIC_DELIVERY_MODE_NORMAL);
    _write_lapic_command(cmd);

    return 0;
}

static void _local_apic_ipcall_interrupt(struct interrupt_stack_registers* regs, intp pc, void* userdata)
{
    unused(regs);
    unused(pc);
    unused(userdata);

    struct cpu* cpu = get_cpu();
    struct ipcall* ipc = (struct ipcall*)__xchgq((u64*)&cpu->ipcall, (u64)null);

    // spurious? weird.
    if(ipc == null) return;

    // valid, so do whatever we're told
    switch((enum IPCALL_FUNCTIONS)ipc->function) {
    case IPCALL_TASK_ENQUEUE:
        {
            struct task* target = (struct task*)ipc->payload;
            task_enqueue(&cpu->current_task, target);
        }
        break;

    case IPCALL_TASK_UNBLOCK:
        {
            struct task* target = (struct task*)ipc->payload;
            task_unblock(target);
        }
        break;
    }

    // free the ipcall structure
    free(ipc);
}

