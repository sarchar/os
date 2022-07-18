#include "common.h"

#include "bootmem.h"
#include "cpu.h"
#include "kernel.h"
#include "pci.h"
#include "stdio.h"

#define PCI_CONF_REG_IDS   0x0000
#define PCI_CONF_REG_CLASS 0x0008

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

static void _enumerate_all();
static void _enumerate_bus(struct pci_segment_group*, u8);
static void _handle_device(struct pci_segment_group*, u8, u8, u16, u16);

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
    u32 v = pci_read_configuration_long(bus, device, 0, PCI_CONF_REG_CLASS, group);
    u8 revision_id = v & 0xFF;
    u8 prog_if = (v >> 8) & 0xFF;
    u8 subclass = (v >> 16) & 0xFF;
    u8 class = (v >> 24) & 0xFF;
    
    // stash this bad boy in a database
    fprintf(stderr, "pci: found device 0x%04X:0x%04X on seg=%d bus=%d dev=%d class=%d subclass=%d prog_IF=%d revision_id=%d\n", 
            vendor_id, device_id, group->segment_id, bus, device, class, subclass, prog_if, revision_id);
}
