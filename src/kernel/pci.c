#include "common.h"

#include "apic.h"
#include "bootmem.h"
#include "cpu.h"
#include "hashtable.h"
#include "kalloc.h"
#include "kernel.h"
#include "paging.h"
#include "pci.h"
#include "stdio.h"

#define PCI_CONF_REG_IDS     0x0000
#define PCI_CONF_REG_COMMAND 0x0004
#define PCI_CONF_REG_CLASS   0x0008
#define PCI_CONF_REG_HEADER  0x000C

#define PCI_CONF_ADDRESS(group, bus, device, func, offset) \
    (u32 volatile*)((group)->base_address + ((((bus) - (group)->start_bus) << 20) | ((device) << 15) | ((func) << 12) | ((offset) & 0xFFC)))

static_assert(is_power_of_2(sizeof(struct pci_device_info)), "must be power of 2 sized struct");

static struct pci_segment_group* pci_segment_groups     = null;
static struct pci_segment_group* pci_segment_group_zero = null;
static struct pci_vendor_info*   pci_device_vendors     = null;

static void _map_all_groups();
static void _enumerate_all();
static void _enumerate_bus(struct pci_segment_group*, u8);
static void _handle_device(struct pci_segment_group*, u8, u8, struct pci_inplace_configuration*);
static void _check_function(struct pci_segment_group*, u8, u8, u8, struct pci_inplace_configuration*);
static void _check_capabilities(struct pci_device_info*);

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

    _map_all_groups();
}

void pci_enumerate_devices()
{
    _enumerate_all();
}

// group can be null, in that case use pci_segment_group_zero
u32 pci_read_configuration_u32(u8 bus, u8 device, u8 function, u16 offset, struct pci_segment_group* group)
{
    if(group == null) group = pci_segment_group_zero;
    return *PCI_CONF_ADDRESS(group, bus, device, function, offset);
}

u16 pci_read_configuration_u16(u8 bus, u8 device, u8 function, u16 offset, struct pci_segment_group* group)
{
    if(group == null) group = pci_segment_group_zero;
    return *(u16 volatile*)PCI_CONF_ADDRESS(group, bus, device, function, offset);
}

u8 pci_read_configuration_u8(u8 bus, u8 device, u8 function, u16 offset, struct pci_segment_group* group)
{
    if(group == null) group = pci_segment_group_zero;
    return *(u8 volatile*)PCI_CONF_ADDRESS(group, bus, device, function, offset);
}

static void _map_all_groups()
{
    struct pci_segment_group* group = pci_segment_groups;
    while(group != null) {
        intp end_address = (intp)PCI_CONF_ADDRESS(group, group->end_bus + 1, 0, 0, 0);
        paging_identity_map_region(group->base_address, end_address - group->base_address, MAP_PAGE_FLAG_WRITABLE | MAP_PAGE_FLAG_DISABLE_CACHE);
        group = group->next;
    }
}

static void _enumerate_all()
{
    for(struct pci_segment_group* group = pci_segment_groups; group != null; group = group->next) {
        for(u16 bus = group->start_bus; bus <= group->end_bus; bus++) {
            _enumerate_bus(group, (u8)bus);
        }
    }
}

static void _enumerate_bus(struct pci_segment_group* group, u8 bus)
{
    for(u32 device = 0; device < 32; device++) {
        struct pci_inplace_configuration* config = (struct pci_inplace_configuration*)PCI_CONF_ADDRESS(group, bus, device, 0, 0);

        if(config->vendor_id == 0xFFFF) continue;

        _handle_device(group, bus, device, config);
    }
}

static void _handle_device(struct pci_segment_group* group, u8 bus, u8 device, struct pci_inplace_configuration* config)
{
    _check_function(group, bus, device, 0, config);

    // check other functions if they exist
    if(!config->multifunction) return;

    for(u8 func = 1; func < 8; func++) {
        struct pci_inplace_configuration* next_function_config = (struct pci_inplace_configuration*)PCI_CONF_ADDRESS(group, bus, device, func, 0);

        if(next_function_config->vendor_id == 0xFFFF) continue;

        _check_function(group, bus, device, func, next_function_config);
    }
}

static void _check_function(struct pci_segment_group* group, u8 bus, u8 device, u8 func, struct pci_inplace_configuration* config)
{
    // first lookup the vendor to see if it already exists
    struct pci_vendor_info* vnd;
    HT_FIND(pci_device_vendors, config->vendor_id, vnd);

    if(vnd == null) { // vendor not found, create one
        vnd = (struct pci_vendor_info*)kalloc(sizeof(struct pci_vendor_info));
        vnd->vendor_id = config->vendor_id;
        vnd->devices   = null;
        HT_ADD(pci_device_vendors, vendor_id, vnd);
    }

    // then add the device to the vendor's list
    struct pci_device_info* dev = (struct pci_device_info*)kalloc(sizeof(struct pci_device_info));

    dev->group     = group;
    dev->bus       = bus;
    dev->device    = device;
    dev->function  = func;
    dev->config    = config;

    // TODO get all BAR sizes. Requires knowing if addresses are 32/64 in size
    if(dev->config->header_type == PCI_HEADER_TYPE_GENERIC) {
        for(u8 i = 0; i < 6; i++) {
        }
    }

    // stash this bad boy in the hash table
    dev->vendor = vnd;
    HT_ADD(vnd->devices, config->device_id, dev);

    // parse and set up capabilities
    _check_capabilities(dev);
}

static void _check_capabilities(struct pci_device_info* dev)
{
    if((dev->config->status & PCI_STATUS_FLAG_CAPABILITIES_LIST) == 0) return;

    struct pci_capability_header volatile* caphdr = 
        (struct pci_capability_header volatile*)PCI_CONF_ADDRESS(dev->group, dev->bus, dev->device, dev->function, dev->config->h0.capability_pointer);
    fprintf(stderr, "     capabilities_list=0x%lX caphdr=0x%lX:\n", dev->config->h0.capability_pointer, (intp)caphdr);

    for(;;) {
        fprintf(stderr, "        id=%d next_pointer=0x%02X\n", caphdr->capability_id, caphdr->next_pointer);

        switch(caphdr->capability_id) {
        case PCI_CAPABILITY_ID_MSI:
            dev->msi = (struct pci_msi volatile*)caphdr;
            {
                // go ahead an enable MSI
//                dev->msi->enable = 1;
//                dev->msi->message_data = (84 & 0xFF) | (1 << 14);               // GSI 20, rising edge trigger
//                dev->msi->message_address = (intp)0xFEE00000 | ((intp)0 << 12); // LAPIC 0
                fprintf(stderr, "            msi_enable=%d multiple_message_capable=%d multiple_message_enable=%d address_64bit=%d per_vector_masking_capable=%d\n",
                        dev->msi->enable, dev->msi->multiple_message_capable, dev->msi->multiple_message_enable, dev->msi->address_64bit, dev->msi->per_vector_masking_capable);
                fprintf(stderr, "            message_address=0x%lX\n", dev->msi->message_address);
            }
            break;
        default:
            fprintf(stderr, "pci: unknown capability %d for device %d:%d.%d\n", caphdr->capability_id, dev->bus, dev->device, dev->function);
            break;
        }

        if(caphdr->next_pointer == 0) break;

        caphdr = (struct pci_capability_header volatile*)PCI_CONF_ADDRESS(dev->group, dev->bus, dev->device, dev->function, caphdr->next_pointer);
    }
}

static bool _dump_device_info(struct pci_device_info* dev, void* userdata)
{
    unused(userdata);

    fprintf(stderr, "pci: found device 0x%04X:0x%04X on seg=%d bus=%d dev=%d func=%d class=%d subclass=%d prog_if=%d revision_id=%d\n", 
            dev->vendor->vendor_id, dev->config->device_id, dev->group->segment_id, dev->bus, dev->device, dev->function, dev->config->class, 
            dev->config->subclass, dev->config->prog_if, dev->config->revision_id);
    fprintf(stderr, "     header_type=0x%02X%s cache_line_size=%d latency_timer=%d bist=%d\n",
            dev->config->header_type, (dev->function == 0 && dev->config->multifunction) ? " (multifunction)" : "", dev->config->cache_line_size, 
            dev->config->latency_timer, dev->config->bist);

    return true;
}

void pci_dump_device_list()
{
    pci_iterate_devices(_dump_device_info, null);
    fprintf(stderr, "sizeof pci_vendor_info: %d\n", sizeof(struct pci_vendor_info));
    fprintf(stderr, "sizeof pci_device_info: %d\n", sizeof(struct pci_device_info));
}

void pci_iterate_devices(pci_iterate_devices_cb cb, void* userdata)
{
    struct pci_vendor_info* vnd;
    struct pci_vendor_info* nextvnd;

    HT_FOR_EACH(pci_device_vendors, vnd, nextvnd) {
        struct pci_device_info* dev;
        struct pci_device_info* nextdev;

        HT_FOR_EACH(vnd->devices, dev, nextdev) {
            if(!cb(dev, userdata)) break;
        }
    }
}

// this version can greatly reduce the # of cb calls, especially for 
// drivers that only implement a specific vendor's devices
void pci_iterate_vendor_devices(u16 vendor_id, pci_iterate_devices_cb cb, void* userdata)
{
    struct pci_vendor_info* vnd;
    HT_FIND(pci_device_vendors, vendor_id, vnd);

    // if vendor doesn't exist, there's nothing to iterate over
    if(vnd == null) return;

    struct pci_device_info* dev;
    struct pci_device_info* nextdev;

    HT_FOR_EACH(vnd->devices, dev, nextdev) {
        if(!cb(dev, userdata)) break;
    }
}

u64 pci_device_get_bar_size(struct pci_device_info* dev, u8 bar_index)
{
    // TODO cache this value in dev->bar_sizes ?
    //
    // write -1 to the bar, read back the result
    // we can use h0 since both headers have bars at the same address
    intp addr = (intp)dev->config->h0.bar[bar_index];
    intp addrh;

    if((addr & PCI_BAR_TYPE) == PCI_BAR_TYPE_64BIT) {
        addrh = (intp)dev->config->h0.bar[bar_index + 1];
        dev->config->h0.bar[bar_index + 1] = 0xFFFFFFFF;
    }

    dev->config->h0.bar[bar_index] = 0xFFFFFFFF;

    u64 res = (u64)dev->config->h0.bar[bar_index];
    if((addr & PCI_BAR_TYPE) == PCI_BAR_TYPE_64BIT) {
        res |= (u64)dev->config->h0.bar[bar_index + 1] << 32;
        dev->config->h0.bar[bar_index + 1] = (u32)addrh;
    } else {
        res |= 0xFFFFFFFF00000000ULL;
    }

    dev->config->h0.bar[bar_index] = (u32)addr;

    return ~(res - 1);
}

intp pci_device_map_bar(struct pci_device_info* dev, u8 bar_index)
{
    if(dev->config->header_type == 0) {
        assert(bar_index < countof(dev->config->h0.bar), "bar_index must be valid");
    } else if(dev->config->header_type == 1) {
        assert(bar_index < countof(dev->config->h1.bar), "bar_index must be valid");
    } else {
        assert(false, "BARs only exist on header type 0 and 1");
    }

    // because BARs are at the same offset for both header types we can use h0
    u64 size = pci_device_get_bar_size(dev, bar_index);
    intp addr = (intp)dev->config->h0.bar[bar_index];

    if((addr & PCI_BAR_TYPE) == PCI_BAR_TYPE_64BIT) {
        addr |= (intp)dev->config->h0.bar[bar_index + 1] << 32;
    }

    fprintf(stderr, "pci: device 0x%04X:0x%04X bar %d at 0x%lX size 0x%lX\n", dev->config->vendor_id, dev->config->device_id, bar_index, addr, size);

    // memory map one page at ABAR
    u8 map_flags = MAP_PAGE_FLAG_WRITABLE;
    if((addr & PCI_BAR_PREFETCHABLE) == 0) map_flags |= MAP_PAGE_FLAG_DISABLE_CACHE;

    addr &= ~(intp)PCI_BAR_NON_ADDRESS_BITS;
    assert(__alignof(addr, 4096) == 0, "BAR address isn't page aligned");

    for(intp start = addr; start < addr + size; start += 0x1000) {
        paging_map_page(start, start, map_flags); // TODO use vmem?
    }

    return addr;
}

void pci_device_unmap_bar(struct pci_device_info* dev, u8 bar_index, intp virt)
{
    u64 size = pci_device_get_bar_size(dev, bar_index);

    for(intp start = virt; start < virt + size; start += 0x1000) {
        paging_unmap_page(start); // TODO use vmem?
    }
}

u32 pci_setup_msi(struct pci_device_info* dev, u8 num_irqs)
{
    if(dev->msi == null) return 0;
    if(num_irqs > (1 << dev->msi->multiple_message_capable)) return 0;

    // TODO allocate an IRQ with alignment to match num_irqs
    u8 cpu_irq = 100 + dev->device * 8 + dev->function; 

    dev->msi->message_data = (cpu_irq & 0xFF) | (1 << 14);          // rising edge trigger
    dev->msi->message_address = apic_get_lapic_base(0); //(intp)0xFEE00000 | ((intp)0 << 12); // LAPIC 0

    return cpu_irq;
}

void pci_set_enable_msi(struct pci_device_info* dev, bool enabled)
{
    if(dev->msi == null) return;
    dev->msi->enable = enabled ? 1 : 0;
}
