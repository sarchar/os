#ifndef __HPET_H__
#define __HPET_H__

enum {
    HPET_FLAG_ADDRESS_IO  = (1 << 0),   // address given is system I/O if present, memory mapped if not
};

void hpet_notify_timer_count(u8 num_hpets);

void hpet_notify_presence(u8 hpet_number, u8 hardwar_revision_id, u8 comparator_count, u16 minimum_tick,
                          intp address, u8 register_bit_width, u8 register_bit_offset, u8 flags);

void hpet_init();

u64  hpet_get_kernel_timer_value();
u64  hpet_kernel_timer_delta_to_us(u64 start, u64 end);
u64  hpet_kernel_timer_delta_to_ns(u64 start, u64 end);

#endif
