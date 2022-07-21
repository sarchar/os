#ifndef __PCI_H__
#define __PCI_H__

#include "hashtable.h"

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

struct pci_device_info {
    struct pci_segment_group* group;
    struct pci_vendor_info* vendor;

    u16 device_id;
    u16 unused0;

    u8  bus;
    u8  device;
    u8  function;
    u8  unused1;

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

    // header register
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
    
    MAKE_HASH_TABLE;
    u8  unused2[til_next_power_of_2(HT_OVERHEAD+32)];
} __packed;

void pci_notify_segment_group(u16 segment_id, intp base_address, u8 start_bus, u8 end_bus);
void pci_init();
void pci_dump_device_list();

typedef bool (*pci_iterate_devices_cb)(struct pci_device_info*, void*);
void pci_iterate_devices(pci_iterate_devices_cb, void*);
void pci_iterate_vendor_devices(u16, pci_iterate_devices_cb, void*);

#endif
