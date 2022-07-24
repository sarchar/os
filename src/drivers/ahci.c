#include "common.h"

#include "ahci.h"
#include "paging.h"
#include "pci.h"
#include "stdio.h"

#define SATA_SIG_ATA    0x00000101  // SATA drive
#define SATA_SIG_ATAPI  0xEB140101  // SATAPI drive
#define SATA_SIG_SEMB   0xC33C0101  // enclosure management bridge
#define SATA_SIG_PM     0x96690101  // port multiplier

#define HBA_PORT_IPM_ACTIVE 1
#define HBA_PORT_DET_PRESENT 3

struct hba_port {
    u32 command_list_base_address;
    u32 command_list_base_address_h;
    u32 fis_base_address;
    u32 fis_base_address_h;
    u32 interrupt_status;
    u32 interrupt_enable;
    u32 commandstatus;
    u32 reserved0;
    u32 task_file_data;
    u32 signature;
    u32 sata_status;
    u32 sata_control;
    u32 sata_error;
    u32 sata_active;
    u32 command_issue;
    u32 sata_notification;
    u32 fis_switch_control;
    u32 reserved1[11];
    u32 vendor[4];
} __packed;

static volatile struct hba_memory {
    u32    capabilities;
    u32    global_host_control;
    u32    interrupt_status;
    u32    port_implemented;
    u32    version;
    u32    ccc_control;
    u32    ccc_ports;
    u32    em_location;
    u32    em_control;
    u32    ext_capabilities; // extended capabilities
    u32    bios_handoff;
    u8     reserved0[0x74];
    u8     vendor[0x60];
    struct hba_port ports[];
} __packed* ahci_base_memory;

static bool _find_ahci_device(struct pci_device_info* dev, void* userinfo)
{
    if(dev->config->class == PCI_CLASS_MASS_STORAGE && dev->config->subclass == PCI_SUBCLASS_MS_SATA) {
        *(struct pci_device_info**)userinfo = dev;
        return false;
    }

    return true;
}

void ahci_load()
{
    struct pci_device_info* dev = null;
    pci_iterate_vendor_devices(0x8086, _find_ahci_device, &dev);

    if(dev == null) {
        fprintf(stderr, "ahci: not found");
        return;
    }

    fprintf(stderr, "ahci: found device %04X:%04X\n", dev->vendor->vendor_id, dev->device_id);
    fprintf(stderr, "ahci: BAR[5]=0x%08X\n", dev->config->h0.bar[5]);
    fprintf(stderr, "ahci: sizeof(struct hba_memory)=%d\n", sizeof(struct hba_memory));
    fprintf(stderr, "ahci: sizeof(struct hba_port)=%d\n", sizeof(struct hba_port));

    // also known as ABAR in the documentation
    ahci_base_memory = (struct hba_memory*)(intp)dev->config->h0.bar[5];

    // memory map one page at ABAR
    paging_map_page((intp)ahci_base_memory, (intp)ahci_base_memory, MAP_PAGE_FLAG_WRITABLE | MAP_PAGE_FLAG_DISABLE_CACHE);

    // enable interrupts, use memory mapped access, enable bus master
    u32 cmd = dev->config->command & ~(PCI_COMMAND_FLAG_ENABLE_IO | PCI_COMMAND_FLAG_DISABLE_INTERRUPTS);
    dev->config->command = cmd | (PCI_COMMAND_FLAG_ENABLE_MEMORY | PCI_COMMAND_FLAG_BUS_MASTER);

    for(u8 i = 0; i < 32; i++) {
        if((ahci_base_memory->port_implemented & (1 << i)) == 0) continue;

        volatile struct hba_port* port = &ahci_base_memory->ports[i];
        u8 ipm = (port->sata_status >> 8) & 0x0F;
        u8 det = port->sata_status & 0x0F;

        if(det != HBA_PORT_DET_PRESENT) continue;
        if(ipm != HBA_PORT_IPM_ACTIVE) continue;

        switch(port->signature) {
        case 0xEB140101:
            fprintf(stderr, "ahci: port %d has SATA drive (sig=0x%08X)\n", i, ahci_base_memory->ports[i].signature);
            break;
        default:
            fprintf(stderr, "ahci: port %d sig=0x%08X unknown\n", i, ahci_base_memory->ports[i].signature);
            break;
        }
    }
}

