#ifndef __ACPI_H__
#define __ACPI_H__

struct acpi_sdt_header {
    u8  signature[4];
    u32 length;
    u8  revision;
    u8  checksum;
    u8  oem_id[6];
    u8  oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
} __packed;

struct acpi_xsdt {
    struct acpi_sdt_header header;
    u64    tables[];
} __packed;

struct acpi_apic {
    struct acpi_sdt_header header;
    u32    lapic_base;
    u32    flags;
    u8     records[];
} __packed;

struct acpi_apic_record_header {
    u8     type;
    u8     length;
} __packed;

struct acpi_apic_record_processor_local_apic {
    struct acpi_apic_record_header header;
    u8     acpi_processor_id;
    u8     apic_id;
    u32    flags;
} __packed;

struct acpi_apic_record_ioapic {
    struct acpi_apic_record_header header;
    u8     ioapic_id;
    u8     reserved;
    u32    ioapic_address;
    u32    global_system_interrupt_base;
} __packed;

struct acpi_apic_record_interrupt_source_override {
    struct acpi_apic_record_header header;
    u8     bus_source;
    u8     irq_source;
    u32    global_system_interrupt;
    u16    flags;
} __packed;

struct acpi_apic_record_local_apic_nmis {
    struct acpi_apic_record_header header;
    u8     acpi_processor_id;
    u16    flags;
    u8     lint_number;
} __packed;

struct acpi_address {
    u8     address_space_id;
    u8     register_bit_width;
    u8     register_bit_offset;
    u8     reserved;
    u64    address;
} __packed;

struct acpi_hpet {
    struct acpi_sdt_header   header;
    u8     hardware_revision_id;

    u8     comparator_count   : 5;
    u8     counter_size       : 1;
    u8     reserved           : 1;
    u8     legacy_replacement : 1;
    
    u16    pci_vendor_id;
    struct acpi_address address;
    u8     hpet_number;
    u16    minimum_tick;

    u8     page_protection    : 4;
    u8     oem_attributes     : 4;
} __packed;

struct acpi_fadt
{
    struct   acpi_sdt_header header;
    u32      firmware_control;
    u32      dsdt;
 
    // field used in ACPI 1.0; no longer in use, for compatibility only
    u8       reserved;
 
    u8       preferred_power_management_profile;
    u16      sci_interrupt;
    u32      smi_command_port;
    u8       acpi_enable;
    u8       acpi_disable;
    u8       s4bios_req;
    u8       pstate_control;
    u32      pm1a_event_block;
    u32      pm1b_event_block;
    u32      pm1a_control_block;
    u32      pm1b_control_block;
    u32      pm2_control_block;
    u32      pm_timer_block;
    u32      gpe0_block;
    u32      gpe1_block;
    u8       pm1_event_length;
    u8       pm1_control_length;
    u8       pm2_control_length;
    u8       pm_timer_length;
    u8       gpe0_length;
    u8       gpe1_length;
    u8       gpe1_base;
    u8       cstate_control;
    u16      worst_c2_latency;
    u16      worst_c3_latency;
    u16      flush_size;
    u16      flush_stride;
    u8       duty_offset;
    u8       duty_width;
    u8       day_alarm;
    u8       month_alarm;
    u8       century;
 
    u16      boot_architecture_flags; // reserved in ACPI 1.0; used since ACPI 2.0+
 
    u8       reserved2;
    u32      flags;
 
    struct acpi_address reset_reg;
 
    u8       reset_value;
    u8       reserved3[3];
 
    // 64bit pointers - Available on ACPI 2.0+
    u64      x_firmware_control;
    u64      x_dsdt;
 
    struct acpi_address x_pm1a_event_block;
    struct acpi_address x_pm1b_event_block;
    struct acpi_address x_pm1a_control_block;
    struct acpi_address x_pm1b_control_block;
    struct acpi_address x_pm2_control_block;
    struct acpi_address x_pm_timer_block;
    struct acpi_address x_gpe0_block;
    struct acpi_address x_gpe1_block;
} __packed;

struct acpi_mcfg_configuration_space {
    u64    base_address;
    u16    pci_segment_group;
    u8     start_bus;
    u8     end_bus;
    u32    reserved;
} __packed;

struct acpi_mcfg {
    struct acpi_sdt_header   header;
    u64    reserved;
    struct acpi_mcfg_configuration_space spaces[];
} __packed;

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
#define ACPI_APIC_FLAG_HAS_PIC (1 << 0)

enum {
    ACPI_APIC_RECORD_PROCESSOR_LOCAL_APIC = 0,
    ACPI_APIC_RECORD_TYPE_IOAPIC = 1,
    ACPI_APIC_RECORD_TYPE_IOAPIC_INTERRUPT_SOURCE_OVERRIDE = 2,
    ACPI_APIC_RECORD_TYPE_IOAPIC_NMI_SOURCE = 3,
    ACPI_APIC_RECORD_TYPE_LOCAL_APIC_NMIS = 4,
    ACPI_APIC_RECORD_TYPE_LOCAL_APIC_ADDRESS_OVERRIDE = 5,
    ACPI_APIC_RECORD_TYPE_LOCAL_X2APIC = 9
};

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
void acpi_set_rsdp_base(intp base);
void acpi_init();
void acpi_init_lai();
void acpi_reset();

// return a virtual memory address pointer to the ACPI table matching signature 'sig'
// If there are multiple tables with that signature, return the 'index'th table.
void* acpi_find_table(char const* sig, u8 index);

#endif
