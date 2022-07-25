#include "common.h"

#include "bootmem.h"
#include "cpu.h"
#include "hpet.h"
#include "kernel.h"
#include "paging.h"
#include "stdio.h"

#define HPET_CONF_FLAG_ENABLE_LEGACY_IRQ (1ULL << 1)
#define HPET_CONF_FLAG_ENABLE_TIMER      (1ULL << 0)

#define HPET_CAPABILITIES_REGISTER  0x00
#define HPET_CONFIGURATION_REGISTER 0x10
#define HPET_COUNTER_VALUE_REGISTER 0xF0

#define HPET_COMPARATOR_REGISTERS_BASE        0x100ULL
#define HPET_COMPARATOR_REGISTERS_SIZE        0x020ULL
#define HPET_COMPARATOR_CAPABILITIES_REGISTER 0x00ULL
#define HPET_COMPARATOR_VALUE_REGISTER        0x08ULL
#define HPET_COMPARATOR_FSB_ROUTE_REGISTER    0x10ULL

#define HPET_COMPARATOR_CAP_CONF_WRITABLE_BITS_MASK  0x0000000000007F4EULL

#define HPET_COMPARATOR_CONF_IRQ_TYPE_SHIFT   1
#define HPET_COMPARATOR_CONF_IRQ_TYPE_MASK    (0x01ULL << HPET_COMPARATOR_CONF_IRQ_TYPE_SHIFT)
#define HPET_COMPARATOR_CONF_IRQ_ENABLE_SHIFT 2
#define HPET_COMPARATOR_CONF_IRQ_ENABLE_MASK  (0x01ULL << HPET_COMPARATOR_CONF_IRQ_ENABLE_SHIFT)
#define HPET_COMPARATOR_CONF_MODE_SHIFT       3
#define HPET_COMPARATOR_CONF_MODE_MASK        (0x01ULL << HPET_COMPARATOR_CONF_MODE_SHIFT)
#define HPET_COMPARATOR_CONF_SET_VALUE_SHIFT  6
#define HPET_COMPARATOR_CONF_SET_VALUE_MASK   (0x01ULL << HPET_COMPARATOR_CONF_SET_VALUE_SHIFT)
#define HPET_COMPARATOR_CONF_32BIT_MODE_SHIFT 8
#define HPET_COMPARATOR_CONF_32BIT_MODE_MASK  (0x01ULL << HPET_COMPARATOR_CONF_32BIT_MODE_SHIFT)
#define HPET_COMPARATOR_CONF_APIC_ROUTE_SHIFT 9
#define HPET_COMPARATOR_CONF_APIC_ROUTE_MASK  (0x1FULL << HPET_COMPARATOR_CONF_APIC_ROUTE_SHIFT)
#define HPET_COMPARATOR_CONF_FSB_ENABLE_SHIFT 14
#define HPET_COMPARATOR_CONF_FSB_ENABLE_MASK  (0x01ULL << HPET_COMPARATOR_CONF_FSB_ENABLE_SHIFT)
#define HPET_COMPARATOR_CONF_ALL_MASKS        (HPET_COMPARATOR_CONF_IRQ_TYPE_MASK   | HPET_COMPARATOR_CONF_IRQ_ENABLE_MASK | \
                                               HPET_COMPARATOR_CONF_MODE_MASK       | HPET_COMPARATOR_CONF_SET_VALUE_MASK  | \
                                               HPET_COMPARATOR_CONF_32BIT_MODE_MASK | HPET_COMPARATOR_CONF_APIC_ROUTE_MASK | \
                                               HPET_COMPARATOR_CONF_FSB_ENABLE_MASK)

struct hpet_timer;

struct hpet_comparator {
    struct hpet_timer* timer;
    u8     index;
    bool   in_use;
    u8     unused[6];

    union  {
        u64 cap_conf; // capabilities and configuration
        struct {
            u8  reserved         :  1;
            u8  interrupt_type   :  1; // 0 - edge triggered, 1 - level triggered
            u8  interrupt_enable :  1; // 1 to enable triggering of interrupts from this comparator
            u8  mode             :  1; // 0 - one shot, 1 - periodic mode
            u8  periodic_capable :  1; // 1 if the timer supports periodic mode
            u8  long_counter     :  1; // 1 if the timer has a long comparator
            u8  set_counter      :  1; // 1 to allow setting of the timer's counter directly
            u8  reserved1        :  1;
            u8  use_32bits       :  1; // 1 to have long timers use 32-bit mode
            u8  interrput_route  :  5; // global interrupt number/route to send to I/O APIC
            u8  fsb_enable       :  1; // 1 to enable FSB interrupt mapping
            u8  fsb_capable      :  1; // 1 if this comparator supports FSB interrupt mapping
            u16 reserved2        : 16;
            u32 interrupt_map    : 32; // 1 in the bit position if it can be mapped to that interrupt number
        };
    };
};

struct hpet_timer {
    intp   address;
    u8     number;
    u8     flags;
    u8     comparator_count;
    u8     unused0;
    u16    minimum_tick;
    u16    unused1;

    // capabilities union
    union {
        u64 capabilities;
        struct {
            u8  revision       : 8;
            u8  num_timers     : 5;
            u8  long_counter   : 1;
            u8  reserved       : 1;
            u8  legacy_capable : 1;
            u16 vendor_id      : 16;
            u32 period         : 32;
        };
    };

    struct hpet_comparator comparators[];
};

static struct hpet_timer** timers = null;
static u8 num_timers;
static u8 num_comparators;

static inline u64 _read_register(struct hpet_timer* timer, u8 reg)
{
    return *(u64 volatile*)(timer->address + reg);
}

static inline void _write_register(struct hpet_timer* timer, u8 reg, u64 value)
{
    *(u64 volatile*)(timer->address + reg) = value;
}

static inline u64 _read_comparator_register(struct hpet_comparator* comp, u8 reg)
{
    return *(u64 volatile*)(comp->timer->address + HPET_COMPARATOR_REGISTERS_BASE + comp->index * HPET_COMPARATOR_REGISTERS_SIZE + reg);
}

static inline void _write_comparator_register(struct hpet_comparator* comp, u8 reg, u64 value)
{
    *(u64 volatile*)(comp->timer->address + HPET_COMPARATOR_REGISTERS_BASE + comp->index * HPET_COMPARATOR_REGISTERS_SIZE + reg) = value;
}

static __always_inline u64 _timer_period(struct hpet_timer* timer, u64 time_in_microseconds)
{
    // The period specified in the hpet capabilities is actually the real world time that has to elapse to
    // increment the internal clock counter by 1.  But that value is specified in femtoseconds, so the
    // *actual* real world time for 1 counter tick is time_per_tick_in_femtoseconds*10^15 seconds.  So, 
    // to get the number of clock counter ticks we want to use that corrresponds to a real world time t, 
    // we use t / (time_per_tick_in_femtoseconds * 10^-15).  To avoid using floating point math, multiply
    // by 10^15/10^15 to get t*10^15/time_per_tick_in_femtoseconds.  Now let's specify t in microseconds
    // to get (t/10^6)*10^15/time_per_tick_in_femtoseconds = t*10^9/time_per_tick_in_femtoseconds:
    return (time_in_microseconds * 1000000000ULL) / timer->period;
}

void hpet_notify_timer_count(u8 num_hpets)
{
    assert(timers == null, "only call this function once");
    timers = (struct hpet_timer**)bootmem_alloc(sizeof(struct hpet_timer*) * num_hpets, 8);
    memset64(timers, 0, num_hpets);
    num_timers = num_hpets;
    num_comparators = 0;
    fprintf(stderr, "hpet: %d HPETs found\n", num_timers);
}

void hpet_notify_presence(u8 hpet_number, u8 hardwar_revision_id, u8 comparator_count, u16 minimum_tick,
                          intp address, u8 register_bit_width, u8 register_bit_offset, u8 flags)
{
    unused(hardwar_revision_id); // TODO check this?
    unused(register_bit_width);  // TODO not sure what these are used for, if anything
    unused(register_bit_offset);

    // create one hpet_timer, and comparator_count comparators
    struct hpet_timer* timer = bootmem_alloc(sizeof(struct hpet_timer) + sizeof(struct hpet_comparator) * (comparator_count + 1), 8);
    assert(hpet_number < num_timers, "got an out of bounds hpet number");
    timers[hpet_number] = timer;

    timer->number = hpet_number;
    timer->address = address;
    timer->comparator_count = comparator_count;
    timer->minimum_tick = minimum_tick;
    timer->flags = flags;

    // read in capabilities now
    timer->capabilities = _read_register(timer, HPET_CAPABILITIES_REGISTER);
    assert(timer->num_timers == comparator_count, "comparator_count doesn't match capabilities provided by hpet register");

    fprintf(stderr, "hpet: timer %d address=0x%lX period=0x%08X vendor_id=0x%04X legacy_capable=%d long_counter=%d num_timers=%d(+1) revision=0x%02X\n", 
            timer->number, timer->address, timer->period, timer->vendor_id, timer->legacy_capable, timer->long_counter, timer->num_timers, timer->revision);

    // create a comparator for each
    for(u8 i = 0; i < comparator_count + 1; i++) {
        struct hpet_comparator* comp = &timer->comparators[i];
        comp->index    = i;
        comp->timer    = timer;
        comp->in_use   = false;
        comp->cap_conf = _read_comparator_register(comp, HPET_COMPARATOR_CAPABILITIES_REGISTER);

        //fprintf(stderr, "hpet: comparator %d: periodic_capable=%d long_counter=%d timer_route_cap=0x%08X\n",
        //        i, comp->periodic_capable, comp->long_counter, comp->interrupt_map);
    }
}

void hpet_timer_enable(struct hpet_timer* timer)
{
    _write_register(timer, HPET_CONFIGURATION_REGISTER, HPET_CONF_FLAG_ENABLE_TIMER);
}

static void _enable_kernel_timer(u8 global_interrupt_number)
{
    struct hpet_comparator* comp = &timers[0]->comparators[0];
    u64 timer_period = _timer_period(comp->timer, 1000); // 1ms/1kHz
    //fprintf(stderr, "hpet: 1ms timer requires counter ticks=%u for timer with clock=%ue-15 sec/cycle\n",
    //        timer_period, comp->timer->period);

    assert((comp->interrupt_map & (global_interrupt_number << 1)) != 0, "must be capable of routing that irq");

    u64 configuration = comp->cap_conf & HPET_COMPARATOR_CAP_CONF_WRITABLE_BITS_MASK;

    configuration &= ~HPET_COMPARATOR_CONF_ALL_MASKS;
    configuration |= (global_interrupt_number << HPET_COMPARATOR_CONF_APIC_ROUTE_SHIFT) | 
                     HPET_COMPARATOR_CONF_SET_VALUE_MASK | HPET_COMPARATOR_CONF_MODE_MASK; // MODE_MASK enables periodic mode

    _write_comparator_register(comp, HPET_COMPARATOR_CAPABILITIES_REGISTER, configuration);
    _write_comparator_register(comp, HPET_COMPARATOR_VALUE_REGISTER, _read_register(comp->timer, HPET_COUNTER_VALUE_REGISTER) + timer_period);
    _write_comparator_register(comp, HPET_COMPARATOR_VALUE_REGISTER, timer_period);
    _write_comparator_register(comp, HPET_COMPARATOR_CAPABILITIES_REGISTER, configuration | HPET_COMPARATOR_CONF_IRQ_ENABLE_MASK); // enable interrupts from this timer source

    // update cap_conf
    comp->cap_conf = _read_comparator_register(comp, HPET_COMPARATOR_CAPABILITIES_REGISTER);
}

void hpet_init()
{
    for(u8 t = 0; t < num_timers; t++) {
        struct hpet_timer* timer = timers[t];

        // map the timer base address into virtual memory
        paging_map_page(timer->address, timer->address, MAP_PAGE_FLAG_DISABLE_CACHE | MAP_PAGE_FLAG_WRITABLE);
        paging_debug_address(timer->address);

        // enable the timer
        hpet_timer_enable(timer);
    }

    // TODO find comparator matching criteria (long mode, available irq, periodic mode available)
    // TODO loop over valid irqs (comp->interrupt_map)
    // TODO and convert global irq number to cpu irq number
    // TODO apic_is_irq_slot_available / apic_map_global_to_cpu
    // TODO install interrupt handler with local function
    // then map the timer/comparator to the global irq number
    _enable_kernel_timer(19);
}

u64 hpet_kernel_timer_delta_to_us(u64 start, u64 end)
{
    // one timer value increment == (cycles_per_femptosecond/10^15) seconds
    // formula is seconds_per_tick_value * (end - start)
    //fprintf(stderr, "start = %llu end = %llu\n", start, end);
    return ((end - start) * timers[0]->period) / 1000000000ULL;
}

u64 hpet_kernel_timer_delta_to_ns(u64 start, u64 end)
{
    // one timer value increment == (cycles_per_femptosecond/10^15) seconds
    // formula is seconds_per_tick_value * (end - start)
    //fprintf(stderr, "start = %llu end = %llu\n", start, end);
    return ((end - start) * timers[0]->period) / 1000000;
}


u64 hpet_get_kernel_timer_value()
{
    return _read_register(timers[0], HPET_COUNTER_VALUE_REGISTER);
}

