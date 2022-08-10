#ifndef __APIC_H__
#define __APIC_H__

// delivery_mode
enum {
    IO_APIC_REDIRECTION_FLAG_DELIVERY_NORMAL       = 0,
    IO_APIC_REDIRECTION_FLAG_DELIVERY_LOW_PRIORITY = 1,
    IO_APIC_REDIRECTION_FLAG_DELIVERY_SYSTEM       = 2,
    IO_APIC_REDIRECTION_FLAG_DELIVERY_NMI          = 3,
    IO_APIC_REDIRECTION_FLAG_DELIVERY_INIT         = 5, // TODO?
    IO_APIC_REDIRECTION_FLAG_DELIVERY_EXTERNAL     = 7  // TODO?
};

// destination_mode
enum {
    IO_APIC_REDIRECTION_DESTINATION_PHYSICAL = 0,
    IO_APIC_REDIRECTION_DESTINATION_LOGICAL  = 1
};

// active_level
enum {
    IO_APIC_REDIRECTION_ACTIVE_HIGH = 0,
    IO_APIC_REDIRECTION_ACTIVE_LOW  = 1
};

// trigger_mode
enum {
    IO_APIC_REDIRECTION_EDGE_SENSITIVE = 0,
    IO_APIC_REDIRECTION_LEVEL_SENSITIVE = 1
};

// apic_init() will call apic_initialize_local_apic().
// use apic_initialize_local_apic() only on the APs
void apic_init();
void apic_initialize_local_apic();

void apic_set_cpu();

void apic_map();
void apic_notify_acpi_io_apic(u8 io_apic_id, intp io_apic_base, u8 global_system_interrupt_base);
void apic_notify_acpi_io_apic_interrupt_source_override(u8 bus_source, u8 irq_source, u8 global_system_interrupt, u8 flags);
void apic_notify_acpi_local_apic_base(intp lapic_base, bool system_has_pic);
void apic_notify_num_local_apics(u32 num_lapics);
void apic_register_processor_lapic(u8 acpi_processor_id, u8 apic_id, bool enabled);
void apic_notify_acpi_lapic_nmis(u8 acpi_processor_id, u8 lint_number, u8 flags);

void apic_set_io_apic_redirection(u8 io_apic_irq, u8 cpu_irq, u8 delivery_mode, u8 destination_mode, u8 active_level, u8 trigger_mode, bool enabled, u8 destination);
void apic_io_apic_enable_interrupt(u8 io_apic_irq);
void apic_io_apic_disable_interrupt(u8 io_apic_irq);

u32 apic_current_cpu_index();
u32 apic_num_local_apics();
intp apic_get_lapic_base(u8 lapic_index);

s64 apic_boot_cpu(u32, u8);

#endif
