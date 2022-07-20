#include "common.h"

#include "bootmem.h"
#include "cpu.h"
#include "hashtable.h"
#include "kalloc.h"
#include "kernel.h"
#include "pci.h"
#include "stdio.h"

#define PCI_CONF_REG_IDS     0x0000
#define PCI_CONF_REG_COMMAND 0x0004
#define PCI_CONF_REG_CLASS   0x0008
#define PCI_CONF_REG_HEADER  0x000C

#define PCI_CONF_ADDRESS(group, bus, device, func, offset) \
    (u32 volatile*)((group)->base_address + ((((bus) - (group)->start_bus) << 20) | ((device) << 15) | ((func) << 12) | ((offset) & 0xFFC)))

struct pci_segment_group {
    struct pci_segment_group* next;
    intp   base_address;
    u16    segment_id;
    u8     start_bus;
    u8     end_bus;
    u32    unused;
};

static struct pci_segment_group* pci_segment_groups = null;
static struct pci_segment_group* pci_segment_group_zero = null;

struct pci_vendor_info;

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

static_assert(is_power_of_2(sizeof(struct pci_device_info)), "must be power of 2 sized struct");

struct pci_vendor_info {
    MAKE_HASH_TABLE;

    struct pci_device_info* devices;
    u16    vendor_id;
    u16    unused0;
    u32    unused1;

    u8     unused2[til_next_power_of_2(HT_OVERHEAD+16)];
};

struct pci_vendor_info* pci_device_vendors = null;

static void _enumerate_all();
static void _enumerate_bus(struct pci_segment_group*, u8);
static void _handle_device(struct pci_segment_group*, u8, u8, u16, u16);
static void _check_function(struct pci_segment_group*, u8, u8, u8, u16, u16);

void pci_notify_segment_group(u16 segment_id, intp base_address, u8 start_bus, u8 end_bus)
{
    struct pci_segment_group* newgroup = bootmem_alloc(sizeof(struct pci_segment_group), 8);

    newgroup->base_address = base_address;
    newgroup->segment_id   = segment_id;
    newgroup->start_bus    = start_bus;
    newgroup->end_bus      = end_bus;
    newgroup->next         = pci_segment_groups;
    pci_segment_groups     = newgroup;

    if(segment_id == 0) pci_segment_group_zero = newgroup;
}

void pci_init()
{
    if(pci_segment_group_zero == null) {
        fprintf(stderr, "pci: no segment group with ID 0 found, disabling PCI");
        return;
    }

    _enumerate_all();
}

// group can be null, in that case use pci_segment_group_zero
u32 pci_read_configuration_long(u8 bus, u8 device, u8 function, u16 offset, struct pci_segment_group* group)
{
    if(group == null) group = pci_segment_group_zero;
    return *PCI_CONF_ADDRESS(group, bus, device, function, offset);
}

static void _enumerate_all()
{
    struct pci_segment_group* group = pci_segment_groups;
    while(group != null) {
        for(u16 bus = group->start_bus; bus <= group->end_bus; bus++) {
            _enumerate_bus(group, (u8)bus);
        }
        group = group->next;
    }
}

static void _enumerate_bus(struct pci_segment_group* group, u8 bus)
{
    for(u32 device = 0; device < 32; device++) {
        u32 ids = pci_read_configuration_long(bus, device, 0, PCI_CONF_REG_IDS, group);
        u16 vendor_id = ids & 0xFFFF;
        u16 device_id = ids >> 16;

        if(vendor_id == 0xFFFF) continue;

        _handle_device(group, bus, device, vendor_id, device_id);
    }
}

static void _handle_device(struct pci_segment_group* group, u8 bus, u8 device, u16 vendor_id, u16 device_id)
{
    _check_function(group, bus, device, 0, vendor_id, device_id);

    // check the header type
    u32 v = pci_read_configuration_long(bus, device, 0, PCI_CONF_REG_HEADER, group);
    u8 header_type = (v >> 16) & 0xFF;
    bool multifunction = (header_type & 0x80) != 0;

    if(!multifunction) return;

    for(u8 func = 1; func < 8; func++) {
        u32 ids = pci_read_configuration_long(bus, device, func, PCI_CONF_REG_IDS, group);
        u16 func_vendor_id = ids & 0xFFFF;
        u16 func_device_id = ids >> 16;

        if(func_vendor_id == 0xFFFF) continue;

        _check_function(group, bus, device, func, func_vendor_id, func_device_id);
    }
}

static void _check_function(struct pci_segment_group* group, u8 bus, u8 device, u8 func, u16 vendor_id, u16 device_id)
{
    // first lookup the vendor to see if it already exists
    struct pci_vendor_info* vnd;
    HT_FIND(pci_device_vendors, vendor_id, vnd);

    if(vnd == null) { // vendor not found, create one
        vnd = (struct pci_vendor_info*)kalloc(sizeof(struct pci_vendor_info));
        vnd->vendor_id = vendor_id;
        vnd->devices   = null;
        HT_ADD(pci_device_vendors, vendor_id, vnd);
    }

    // then add the device to the vendor's list
    struct pci_device_info* dev = (struct pci_device_info*)kalloc(sizeof(struct pci_device_info));
    
    dev->group     = group;
    dev->device_id = device_id;
    dev->bus       = bus;
    dev->device    = device;
    dev->function  = func;

    // read the class of the function
    dev->class_reg = pci_read_configuration_long(bus, device, func, PCI_CONF_REG_CLASS, group);

    // get the header info
    dev->header_reg = pci_read_configuration_long(bus, device, 0, PCI_CONF_REG_HEADER, group);
 
    // stash this bad boy in the hash table
    dev->vendor = vnd;
    HT_ADD(vnd->devices, device_id, dev);
}

void pci_dump_device_list()
{
    struct pci_vendor_info* vnd;

    HT_FOR_EACH(pci_device_vendors, vnd) {
        struct pci_device_info* dev;

        HT_FOR_EACH(vnd->devices, dev) {
            fprintf(stderr, "pci: found device 0x%04X:0x%04X on seg=%d bus=%d dev=%d func=%d class=%d subclass=%d prog_if=%d revision_id=%d\n", 
                    vnd->vendor_id, dev->device_id, dev->group->segment_id, dev->bus, dev->device, dev->function, dev->class, dev->subclass, dev->prog_if, dev->revision_id);
            fprintf(stderr, "     header_type=0x%02X%s cache_line_size=%d latency_timer=%d bist=%d\n",
                    dev->header_type, (dev->function == 0 && dev->multifunction) ? " (multifunction)" : "", dev->cache_line_size, dev->latency_timer, dev->bist);
        }
    }

    fprintf(stderr, "sizeof pci_vendor_info: %d\n", sizeof(struct pci_vendor_info));
    fprintf(stderr, "sizeof pci_device_info: %d\n", sizeof(struct pci_device_info));
}
