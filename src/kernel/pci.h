#ifndef __PCI_H__
#define __PCI_H__

#include "hashtable.h"

enum PCI_CLASS {
    PCI_CLASS_NONE               = 0x00,
    PCI_CLASS_MASS_STORAGE       = 0x01,
    PCI_CLASS_NETWORK            = 0x02,
    PCI_CLASS_DISPLAY            = 0x03,
    PCI_CLASS_MULTIMEDIA         = 0x04,
    PCI_CLASS_MEMORY             = 0x05,
    PCI_CLASS_BRIDGE             = 0x06,
    PCI_CLASS_SIMPLE_COMM        = 0x07,
    PCI_CLASS_BASE_SYSTEM        = 0x08,
    PCI_CLASS_INPUT_DEVICE       = 0x09,
    PCI_CLASS_DOCKING_STATION    = 0x0A,
    PCI_CLASS_PROECSSOR          = 0x0B,
    PCI_CLASS_SERIAL_BUS         = 0x0C,
    PCI_CLASS_WIRELESS           = 0x0D,
    PCI_CLASS_INTELLIGENT        = 0x0E,
    PCI_CLASS_SATELLITE          = 0x0F,
    PCI_CLASS_ENCRYPTION         = 0x10,
    PCI_CLASS_SIGNAL_PROCESSSING = 0x11
};

enum PCI_SUBCLASS_MASS_STORAGE {
    PCI_SUBCLASS_MS_SCSI   = 0x00,
    PCI_SUBCLASS_MS_IDE    = 0x01,
    PCI_SUBCLASS_MS_FLOPPY = 0x02,
    PCI_SUBCLASS_MS_IPI    = 0x03,
    PCI_SUBCLASS_MS_RAID   = 0x04,
    PCI_SUBCLASS_MS_ATA    = 0x05,
    PCI_SUBCLASS_MS_SATA   = 0x06,
    PCI_SUBCLASS_MS_SAS    = 0x07,
    PCI_SUBCLASS_MS_NVM    = 0x08
};

enum PCI_COMMAND_FLAG {
    PCI_COMMAND_FLAG_ENABLE_IO             = 1 << 0,
    PCI_COMMAND_FLAG_ENABLE_MEMORY         = 1 << 1,
    PCI_COMMAND_FLAG_BUS_MASTER            = 1 << 2,
    PCI_COMMAND_FLAG_SPECIAL_CYCLES        = 1 << 3,
    PCI_COMMAND_FLAG_MWINV_ENABLE          = 1 << 4,
    PCI_COMMAND_FLAG_VGA_PALETTE_SNOOP     = 1 << 5,
    PCI_COMMAND_FLAG_PARITY_ERROR_RESPONSE = 1 << 6,
    PCI_COMMAND_FLAG_SERRn_ENABLE          = 1 << 8,
    PCI_COMMAND_FLAG_FAST_B2B_ENABLE       = 1 << 9,
    PCI_COMMAND_FLAG_DISABLE_INTERRUPTS    = 1 << 10
};

struct pci_segment_group {
    struct pci_segment_group* next;
    intp   base_address;
    u16    segment_id;
    u8     start_bus;
    u8     end_bus;
    u32    unused;
};

struct pci_device_info;
struct pci_vendor_info {
    MAKE_HASH_TABLE;

    struct pci_device_info* devices;
    u16    vendor_id;
    u16    unused0;
    u32    unused1;

    u8     unused2[til_next_power_of_2(HT_OVERHEAD+16)];
};

struct pci_inplace_configuration;
struct pci_device_info {
    struct pci_segment_group* group;
    struct pci_vendor_info* vendor;
    struct pci_inplace_configuration* config;

    u16 device_id;
    u16 unused0;

    u8  bus;
    u8  device;
    u8  function;
    u8  unused1;

    MAKE_HASH_TABLE;
    u8  unused2[til_next_power_of_2(HT_OVERHEAD+32)];
} __packed;

// maps PCI configuration space exactly to this struct so that it's convenient to access
struct pci_inplace_configuration {
    u16 vendor_id;
    u16 device_id;

    // command/status register
    u16 command;
    u16 status;

    // class register
    union {
        u32 class_reg;
        struct {
            u8  revision_id;
            u8  prog_if;
            u8  subclass;
            u8  class;
        } __packed;
    };

    // header/other register
    union {
        u32 header_reg;
        struct {
            u8   cache_line_size;
            u8   latency_timer;
            u8   header_type   : 7;
            bool multifunction : 1;
            u8   bist;
        } __packed;
    };

    // the rest of the structure depends on `header_type`, so here's a sweet union
    union {
        // header type 0
        struct {
            u32 bar[6];
            u32 cardbus_cis_pointer;
            u16 subsystem_vendor_id;
            u16 subsystem_id;
            u32 expansion_rom_base_address;
            u8  capability_pointer;
            u8  reserved0[3];
            u32 reserved1;
            u8  interrupt_line;
            u8  interrupt_pin;
            u8  min_grant;
            u8  max_latency;
        } h0 __packed;

        // header type 1
        struct {
            u32 bar[2];
            u8  primary_bus_number;
            u8  secondary_bus_number;
            u8  subordinate_bus_number;
            u8  secondary_latency_timer;
            u8  io_base;
            u8  io_limit;
            u16 secondary_status;
            u16 memory_base;
            u16 memory_limit;
            u16 prefetchable_memory_base;
            u16 prefetchable_memory_limit;
            u32 prefetchable_memory_base_upper32;
            u32 prefetchable_memory_limit_upper32;
            u16 io_base_upper16;
            u16 io_limit_upper16;
            u8  capability_pointer;
            u8  reserved0[3];
            u32 expansion_rom_base_address;
            u8  interrupt_line;
            u8  interrupt_pin;
            u16 bridge_control;
        } h1 __packed;

        // header type 0x2
        struct {
            u32 cardbus_base_address;
            u8  capability_list_offset;
            u8  reserved0;
            u16 secondary_status;
            u8  pci_bus_number;
            u8  cardbus_bus_number;
            u8  subordinate_bus_number;
            u8  cardbus_latency_timer;
            struct { // base address 0, 1, then I/O address 0, 1
                u32 base_address;
                u32 limit;
            } addresses[4];
            u8  interrupt_line;
            u8  interrupt_pin;
            u16 bridge_control;
            u16 subsystem_device_id;
            u16 subsystem_vendor_id;
            u32 legacy_mode_base_address;
        } h2 __packed;
    };
} __packed;

void pci_notify_segment_group(u16 segment_id, intp base_address, u8 start_bus, u8 end_bus);
void pci_init();
void pci_dump_device_list();

typedef bool (*pci_iterate_devices_cb)(struct pci_device_info*, void*);
void pci_iterate_devices(pci_iterate_devices_cb, void*);
void pci_iterate_vendor_devices(u16, pci_iterate_devices_cb, void*);

#endif
