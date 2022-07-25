#include "common.h"

#include "ahci.h"
#include "hpet.h"
#include "kalloc.h"
#include "paging.h"
#include "palloc.h"
#include "pci.h"
#include "stdio.h"

#define SATA_SIG_ATA    0x00000101  // SATA drive
#define SATA_SIG_ATAPI  0xEB140101  // SATAPI drive
#define SATA_SIG_SEMB   0xC33C0101  // enclosure management bridge
#define SATA_SIG_PM     0x96690101  // port multiplier

#define HBA_PORT_IPM_ACTIVE 1
#define HBA_PORT_DET_PRESENT 3

enum HBA_CAPABILITIES {
    HBA_CAPABILITIES_NUMBER_OF_PORTS                  = 0x1F << 0,
    HBA_CAPABILITIES_NUMBER_OF_PORTS_SHIFT            = 0,
    HBA_CAPABILITIES_EXTERNAL_SATA                    = 1 << 5,
    HBA_CAPABILITIES_ENCLOSURE_MANAGEMENT             = 1 << 6,
    HBA_CAPABILITIES_COMMAND_COMPLETION_COALESCING    = 1 << 7,
    HBA_CAPABILITIES_NUMBER_OF_COMMAND_SLOTS          = 0x1F << 8,
    HBA_CAPABILITIES_NUMBER_OF_COMMAND_SLOTS_SHIFT    = 8,
    HBA_CAPABILITIES_PARTIAL_STATE                    = 1 << 13,
    HBA_CAPABILITIES_SLUMBER_STATE                    = 1 << 14,
    HBA_CAPABILITIES_PIO_MULTIPLE_DRQ_BLOCK           = 1 << 15,
    HBA_CAPABILITIES_FIS_BASED_SWITCHING              = 1 << 16,
    HBA_CAPABILITIES_PORT_MULTIPLIER                  = 1 << 17,
    HBA_CAPABILITIES_AHCI_MODE_ONLY                   = 1 << 18,
    HBA_CAPABILITIES_INTERFACE_SPEED                  = 0x0F << 20,
    HBA_CAPABILITIES_INTERFACE_SPEED_SHIFT            = 20,
    HBA_CAPABILITIES_COMMAND_LIST_OVERRIDE            = 1 << 24,
    HBA_CAPABILITIES_ACTIVITY_LED                     = 1 << 25,
    HBA_CAPABILITIES_AGGRESSIVE_LINK_POWER_MANAGEMENT = 1 << 26,
    HBA_CAPABILITIES_STAGGERED_SPINUP                 = 1 << 27,
    HBA_CAPABILITIES_MECHANICAL_PRESENCE_SWITCH       = 1 << 28,
    HBA_CAPABILITIES_SNOTIFICATION_REGISTER           = 1 << 29,
    HBA_CAPABILITIES_NATIVE_COMMAND_QUEUEING          = 1 << 30,
    HBA_CAPABILITIES_64BIT_ADDRESSING                 = 1 << 31
};

enum HBA_EXTENDED_CAPABILITIES {
    HBA_CAPABILITIES_BIOS_HANDOVER                           = 1 << 0,
    HBA_CAPABILITIES_NVMHCI_PRESENT                          = 1 << 1,
    HBA_CAPABILITIES_AUTOMATIC_PARTIAL_TO_SLUMBER_TRANSITION = 1 << 2,
    HBA_CAPABILITIES_DEVICE_SLEEP                            = 1 << 3,
    HBA_CAPABILITIES_AGGRESSIVE_DEVICE_SLEEP_MANAGEMENT      = 1 << 4,
    HBA_CAPABILITIES_DEVSLEEP_ENTRANCE_FROM_SLUMBER_ONLY     = 1 << 5
};

enum HBA_GHC_FLAGS {
    HBA_GHC_FLAG_RESET                        = 1 << 0,
    HBA_GHC_FLAG_INTERRUPT_ENABLE             = 1 << 1,
    HBA_GHC_FLAG_MSI_REVERT_TO_SINGLE_MESSAGE = 1 << 2,
    HBA_GHC_FLAG_AHCI_ENABLE                  = 1 << 31
};

enum HBA_BIOS_HANDOFF_FLAGS {
    HBA_BIOS_HANDOFF_BIOS_OWNED                        = 1 << 0,  // BOS
    HBA_BIOS_HANDOFF_OS_OWNED                          = 1 << 0,  // OOS
    HBA_BIOS_HANDOFF_MSI_ON_OS_OWNERSHIP_CHANGE_ENABLE = 1 << 2,  // SOOE
    HBA_BIOS_HANDOFF_OS_OWNERSHIP_CHANGE               = 1 << 3,  // OOC
    HBA_BIOS_HANDOFF_BIOS_BUSY                         = 1 << 4   // BB
};

enum HBAP_CMDSTAT_FLAGS {
    HBAP_CMDSTAT_FLAG_START_COMMAND_LIST                               = 1 << 0,
    HBAP_CMDSTAT_FLAG_SPIN_UP_DEVICE                                   = 1 << 1,
    HBAP_CMDSTAT_FLAG_POWER_ON_DEVICE                                  = 1 << 2,
    HBAP_CMDSTAT_FLAG_COMMAND_LIST_OVERRIDE                            = 1 << 3,
    HBAP_CMDSTAT_FLAG_FIS_RECEIVE_ENABLE                               = 1 << 4,
    HBAP_CMDSTAT_FLAG_CURRENT_COMMAND_SLOT                             = 0x1F << 8,
    HBAP_CMDSTAT_FLAG_CURRENT_COMMAND_SLOT_SHIFT                       = 8,
    HBAP_CMDSTAT_FLAG_MECHANICAL_PRESENCE_SWITCH_STATE                 = 1 << 13,
    HBAP_CMDSTAT_FLAG_FIS_RECEIVE_RUNNING                              = 1 << 14,
    HBAP_CMDSTAT_FLAG_COMMAND_LIST_RUNNING                             = 1 << 15,
    HBAP_CMDSTAT_FLAG_COLD_PRESENCE_STATE                              = 1 << 16,
    HBAP_CMDSTAT_FLAG_PORT_MULTIPLIER_ATTACHED                         = 1 << 17,
    HBAP_CMDSTAT_FLAG_HOT_PLUG_CAPABLE                                 = 1 << 18,
    HBAP_CMDSTAT_FLAG_MECHANICAL_PRESENCE_SWITCH_ATTACHED              = 1 << 19,
    HBAP_CMDSTAT_FLAG_COLD_PRESENCE_DETECTION                          = 1 << 20,
    HBAP_CMDSTAT_FLAG_EXTERNAL_SATA                                    = 1 << 21,
    HBAP_CMDSTAT_FLAG_FIS_SWITCHING_CAPABLE                            = 1 << 22,
    HBAP_CMDSTAT_FLAG_AUTOMATIC_PARTIAL_TO_SLUMBER_TRANSITIONS_ENABLED = 1 << 23,
    HBAP_CMDSTAT_FLAG_ATAPI                                            = 1 << 24,
    HBAP_CMDSTAT_FLAG_DRIVE_LED_ATAPI                                  = 1 << 25,
    HBAP_CMDSTAT_FLAG_AGGRESSIVE_LINK_POWER_MANAGEMENT                 = 1 << 26,
    HBAP_CMDSTAT_FLAG_AGGRESSIVE_SLUMBER_OR_PARTIAL                    = 1 << 27,
    HBAP_CMDSTAT_FLAG_INTERFACE_COMMUNICATION_CONTROL                  = 0x0F << 28,
    HBAP_CMDSTAT_FLAG_INTERFACE_COMMUNICATION_CONTROL_ACTIVE           = 0x01 << 28,
    HBAP_CMDSTAT_FLAG_INTERFACE_COMMUNICATION_CONTROL_SHIFT            = 28
};

enum HBAP_SATA_CONTROL_FLAGS {
    HBAP_SATA_CONTROL_FLAG_DET_INITIALIZE  = 1 << 0,
    HBAP_SATA_CONTROL_FLAG_DET_DISABLE     = 1 << 2,
    HBAP_SATA_CONTROL_FLAG_SPD_GEN1        = 1 << 4, // limit to Gen 1
    HBAP_SATA_CONTROL_FLAG_SPD_GEN2        = 2 << 4, // limit to <=Gen 2
    HBAP_SATA_CONTROL_FLAG_SPD_GEN3        = 3 << 4, // limit to <=Gen 3
    HBAP_SATA_CONTROL_FLAG_IPM_NO_PARTIAL  = 1 << 8,
    HBAP_SATA_CONTROL_FLAG_IPM_NO_SLUMBER  = 1 << 9,
    HBAP_SATA_CONTROL_FLAG_IPM_NO_DEVSLEEP = 1 << 10
};

struct hba_port {
    u32 command_list_base_address;
    u32 command_list_base_address_h;
    u32 received_fis_base_address;
    u32 received_fis_base_address_h;
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
    u32    ports_implemented;
    u32    version;
    u32    ccc_control;
    u32    ccc_ports;
    u32    em_location;
    u32    em_control;
    u32    extended_capabilities;
    u32    bios_handoff;
    u8     reserved0[0x74];
    u8     vendor[0x60];
    struct hba_port ports[];
} __packed* ahci_base_memory;

static struct {
    u32    capabilities;
    u32    ports_implemented;
} ahci_cached_memory;

struct ahci_device_port {
    intp   command_list_phys_address;
    intp   command_list_address;
    intp   received_fis_phys_address;
    intp   received_fis_address;
    intp   free_mem_phys_address;
    intp   free_mem_address;
    intp   reserved0;
    intp   reserved1;
};

struct hba_command_header {
    union {
        u8 dw0;
        struct {
            u8  fis_length     : 5; // Command FIS length in DWORDS, 2 ~ 16
            u8  atapi          : 1; // ATAPI
            u8  host_to_device : 1; // Write, 1: H2D, 0: D2H
            u8  prefetchable   : 1; // Prefetchable
        } __packed;
    };
 
    union {
        u8 dw1;
        struct {
            u8  reset                : 1; // Reset
            u8  bist                 : 1; // BIST
            u8  clear_busy           : 1; // Clear busy upon R_OK
            u8  reserved0            : 1; // Reserved
            u8  port_multiplier_port : 4; // Port multiplier port
        } __packed;
    };
 
    u16 physical_rdt_length;       // Physical region descriptor table length in entries
 
    u32 physical_rdt_xfer_count;   // Physical region descriptor byte count transferred
 
    u32 command_table_base;        // Command table descriptor base address
    u32 command_table_base_h;      // Command table descriptor base address upper 32 bits
 
    u32 reserved1[4];              // Reserved
} __packed;

// PRD - Physical Region Descriptor table entry
struct hba_prdt_entry {
    u32 data_base_address;      // Data base address
    u32 data_base_address_h;     // Data base address upper 32 bits
    u32 reserved0;              // Reserved
 
    u32 data_byte_count         : 22;  // Byte count, 4M max
    u32 reserved1               : 9;   // Reserved
    u32 interrupt_on_completion : 1;   // Interrupt on completion
};

struct hba_command_table {
    u8  command_fis[64];    // Command FIS, up to 64 bytes
    u8  atapi_command[16];  // ATAPI command, 12 or 16 bytes
    u8  reserved0[48];      // Reserved
    struct hba_prdt_entry   prdt_entries[];  // Physical region descriptor table entries, 0 ~ 65535
};

struct fis {
    u8  blargh[256];
} __packed;

static struct ahci_device_port* ahci_device_ports[32];

static bool _declare_ownership();
static bool _reset_controller();
static void _try_initialize_port(u8, u32);

static bool _find_ahci_device_cb(struct pci_device_info* dev, void* userinfo)
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
    pci_iterate_vendor_devices(0x8086, _find_ahci_device_cb, &dev);

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

    // copy over a few cached items that will be lost upon HBA reset
    ahci_cached_memory.capabilities = ahci_base_memory->capabilities;
    ahci_cached_memory.ports_implemented = ahci_base_memory->ports_implemented;

    // enable interrupts, use memory mapped access, enable bus master
    u32 cmd = dev->config->command & ~(PCI_COMMAND_FLAG_ENABLE_IO | PCI_COMMAND_FLAG_DISABLE_INTERRUPTS);
    dev->config->command = cmd | (PCI_COMMAND_FLAG_ENABLE_MEMORY | PCI_COMMAND_FLAG_BUS_MASTER);

    // disable interrupts in the global hba control
    ahci_base_memory->global_host_control &= ~HBA_GHC_FLAG_INTERRUPT_ENABLE;

    // perform OS handover
    _declare_ownership();

    // TODO map the interrupt from the PCI device to a new interrupt callback here in this module

    // reset the HBA
    if(!_reset_controller()) return;

    // determine # of commands supported
    u32 caps = ahci_cached_memory.capabilities;
    u32 ncmds = (caps & HBA_CAPABILITIES_NUMBER_OF_COMMAND_SLOTS) >> HBA_CAPABILITIES_NUMBER_OF_COMMAND_SLOTS_SHIFT;
 
    for(u8 i = 0; i < 32; i++) {
        ahci_device_ports[i] = null;

        if((ahci_base_memory->ports_implemented & (1 << i)) == 0) continue;

        volatile struct hba_port* port = &ahci_base_memory->ports[i];
        u8 ipm = (port->sata_status >> 8) & 0x0F;
        u8 det = port->sata_status & 0x0F;

        if(det != HBA_PORT_DET_PRESENT) continue;
        if(ipm != HBA_PORT_IPM_ACTIVE) continue;

        // device exists
        _try_initialize_port(i, ncmds);
    }
}

static bool _declare_ownership()
{
    if((ahci_base_memory->extended_capabilities & HBA_CAPABILITIES_BIOS_HANDOVER) == 0) {
        fprintf(stderr, "ahci: HBA doesn't support BIOS handover...assuming ownership.\n");
        return true; // no error, assume ownership already
    }

    assert(false, "TODO: implement bios handover control. is that even necessary?");

    return true;
}

static bool _reset_controller()
{
    u64 tmp;

    // set the HBA reset bit and wait for it to be reset to 0
    ahci_base_memory->global_host_control |= HBA_GHC_FLAG_RESET;
    wait_until_false(ahci_base_memory->global_host_control & HBA_GHC_FLAG_RESET, 1000000, tmp) {
        // timeout
        fprintf(stderr, "ahci: HBA reset timed out\n");
        return false;
    }

    // enable AHCI
    ahci_base_memory->global_host_control |= HBA_GHC_FLAG_AHCI_ENABLE;

    fprintf(stderr, "ahci: HBA reset successful pi=%08X\n", ahci_cached_memory.ports_implemented);
    return true;
}

void ahci_dump_registers()
{
    u32 cap = ahci_cached_memory.capabilities;

    fprintf(stderr, "ahci: ahci_base_memory=0x%lX\n", (intp)ahci_base_memory);
    fprintf(stderr, "ahci: capabilities:\n");

    fprintf(stderr, "    number of ports = %d\n", (cap & HBA_CAPABILITIES_NUMBER_OF_PORTS) >> HBA_CAPABILITIES_NUMBER_OF_PORTS_SHIFT);

    if(ahci_base_memory->capabilities & HBA_CAPABILITIES_EXTERNAL_SATA) {
        fprintf(stderr, "    supports external SATA\n");
    }
    if(ahci_base_memory->capabilities & HBA_CAPABILITIES_ENCLOSURE_MANAGEMENT) {
        fprintf(stderr, "    supports enclosure management\n");
    }
    if(ahci_base_memory->capabilities & HBA_CAPABILITIES_COMMAND_COMPLETION_COALESCING) {
        fprintf(stderr, "    supports command completion coalescing\n");
    }
    fprintf(stderr, "    number of command slots = %d\n", (cap & HBA_CAPABILITIES_NUMBER_OF_COMMAND_SLOTS) >> HBA_CAPABILITIES_NUMBER_OF_COMMAND_SLOTS_SHIFT);
    if(ahci_base_memory->capabilities & HBA_CAPABILITIES_PARTIAL_STATE) {
        fprintf(stderr, "    supports partial state\n");
    }
    if(ahci_base_memory->capabilities & HBA_CAPABILITIES_SLUMBER_STATE) {
        fprintf(stderr, "    supports slumber state\n");
    }
    if(ahci_base_memory->capabilities & HBA_CAPABILITIES_PIO_MULTIPLE_DRQ_BLOCK) {
        fprintf(stderr, "    supports PIO multiple DRQ blocks\n");
    }
    if(ahci_base_memory->capabilities & HBA_CAPABILITIES_FIS_BASED_SWITCHING) {
        fprintf(stderr, "    supports FIS-based switching\n");
    }
    if(ahci_base_memory->capabilities & HBA_CAPABILITIES_PORT_MULTIPLIER) {
        fprintf(stderr, "    supports port multiplier\n");
    }
    if(ahci_base_memory->capabilities & HBA_CAPABILITIES_AHCI_MODE_ONLY) {
        fprintf(stderr, "    AHCI-only mode (no legacy)\n");
    } else {
        fprintf(stderr, "    supports legacy mode\n");
    }
    u8 speed = (cap & HBA_CAPABILITIES_INTERFACE_SPEED) >> HBA_CAPABILITIES_INTERFACE_SPEED_SHIFT;
    if(speed == 1) fprintf(stderr, "    supports Gen 1 speed (1.5Gbps)\n");
    else if(speed == 2) fprintf(stderr, "    supports Gen 2 speed (3Gbps)\n");
    else if(speed == 3) fprintf(stderr, "    supports Gen 3 speed (6Gbps)\n");
    if(ahci_base_memory->capabilities & HBA_CAPABILITIES_COMMAND_LIST_OVERRIDE) {
        fprintf(stderr, "    supports command list override\n");
    }
    if(ahci_base_memory->capabilities & HBA_CAPABILITIES_ACTIVITY_LED) {
        fprintf(stderr, "    supports activity LED\n");
    }
    if(ahci_base_memory->capabilities & HBA_CAPABILITIES_AGGRESSIVE_LINK_POWER_MANAGEMENT) {
        fprintf(stderr, "    supports aggressive link power management\n");
    }
    if(ahci_base_memory->capabilities & HBA_CAPABILITIES_STAGGERED_SPINUP) {
        fprintf(stderr, "    supports staggered spin-up\n");
    }
    if(ahci_base_memory->capabilities & HBA_CAPABILITIES_MECHANICAL_PRESENCE_SWITCH) {
        fprintf(stderr, "    supports mechanical presence switch\n");
    }
    if(ahci_base_memory->capabilities & HBA_CAPABILITIES_SNOTIFICATION_REGISTER) {
        fprintf(stderr, "    supports SNotification register\n");
    }
    if(ahci_base_memory->capabilities & HBA_CAPABILITIES_NATIVE_COMMAND_QUEUEING) {
        fprintf(stderr, "    supports native command queuing\n");
    }
    if(ahci_base_memory->capabilities & HBA_CAPABILITIES_64BIT_ADDRESSING) {
        fprintf(stderr, "    supports 64-bit addressing\n");
    }

#define SHOW_SIZEOF(strct) \
    fprintf(stderr, "sizeof(struct " #strct ")=%d\n", sizeof(struct strct))
    SHOW_SIZEOF(ahci_device_port);
    SHOW_SIZEOF(hba_command_header);
    SHOW_SIZEOF(hba_command_table);
    SHOW_SIZEOF(hba_prdt_entry);
    SHOW_SIZEOF(fis);
#undef SHOW_SIZEOF
}

static inline bool _stop_port_processing(struct hba_port volatile* hba_port)
{
    hba_port->commandstatus &= ~(HBAP_CMDSTAT_FLAG_FIS_RECEIVE_ENABLE | HBAP_CMDSTAT_FLAG_START_COMMAND_LIST);
    u64 tmp;
    wait_until_false(hba_port->commandstatus & (HBAP_CMDSTAT_FLAG_FIS_RECEIVE_RUNNING | HBAP_CMDSTAT_FLAG_COMMAND_LIST_RUNNING), 5000000, tmp) {
        return false;
    }
    return true;
}

static void _try_initialize_port(u8 port_index, u32 ncmds)
{
    fprintf(stderr, "ahci: initializing port %d...\n", port_index);

    struct hba_port volatile* hba_port = &ahci_base_memory->ports[port_index];

    ahci_device_ports[port_index] = (struct ahci_device_port*)kalloc(sizeof(struct ahci_device_port));
    struct ahci_device_port* aport = ahci_device_ports[port_index];
    zero(aport);
   
    // we need uncached memory for the command list and FIS, so we allocate a page of memory
    // instead of using kalloc(), since kalloc returns already mapped memory
    intp phys_page = (intp)palloc_claim_one(); // phys_page is also identity mapped, but we need access to the memory without cache
    intp virt_addr = vmem_map_page(phys_page, MAP_PAGE_FLAG_WRITABLE | MAP_PAGE_FLAG_DISABLE_CACHE);

    // allocate space for command list
    assert(1024 >= sizeof(struct hba_command_header) * ncmds, "too many command entries requested");
	intp command_list_base_address = (intp)phys_page; // always allocate a chunk of 1024 regardless of the # of commands, as 
                                                      // this guarantees alignment of 1024, a requirement for the command list
    assert(__alignof(command_list_base_address, 1024) == 0, "alignment must be 1024");

    // allocate space for return FIS
    assert(sizeof(struct fis) == 256, "FIS must be 256 bytes");
    intp fis_base_address = phys_page + 1024;
    assert(__alignof(fis_base_address, 256) == 0, "alignment must be 256");

    // before we can change pointers in the hba_port, we must disable the receive FIS buffer and wait for the FIS engine to stop
    _stop_port_processing(hba_port);

    // set the pointers in hba_port
    // kalloc() returns an identity mapped address
    hba_port->command_list_base_address_h = (u32)(command_list_base_address >> 32);
    hba_port->command_list_base_address   = (u32)(command_list_base_address & 0xFFFFFFFF);
    hba_port->received_fis_base_address_h = (u32)(fis_base_address >> 32);
    hba_port->received_fis_base_address   = (u32)(fis_base_address & 0xFFFFFFFF);

    // save the address pointers in the ahci_device_port
    aport->command_list_phys_address      = phys_page;
    aport->command_list_address           = virt_addr;
    aport->received_fis_phys_address      = phys_page + 1024;
    aport->received_fis_address           = virt_addr + 1024;
    aport->free_mem_phys_address          = phys_page + 1024 + sizeof(struct fis);
    aport->free_mem_address               = virt_addr + 1024 + sizeof(struct fis);

    // disable transitions to sleep states
    hba_port->sata_control |= (HBAP_SATA_CONTROL_FLAG_IPM_NO_PARTIAL | HBAP_SATA_CONTROL_FLAG_IPM_NO_SLUMBER);

    // all of interrupt_status is write '1' to clear, so wherever an interrupt has previously occurred
    // will be a 1 bit. writing to itself will write 1 in that location and clear it
    hba_port->interrupt_status = hba_port->interrupt_status;

    // same for any error bits
    hba_port->sata_error = hba_port->sata_error;

    // power on drive, spin up drive, set ICC to active, and enable the Receive FIS buffer
    u32 cmdstat = hba_port->commandstatus;
    cmdstat |= (HBAP_CMDSTAT_FLAG_POWER_ON_DEVICE | HBAP_CMDSTAT_FLAG_SPIN_UP_DEVICE | HBAP_CMDSTAT_FLAG_FIS_RECEIVE_ENABLE);
    cmdstat = (cmdstat & ~HBAP_CMDSTAT_FLAG_INTERFACE_COMMUNICATION_CONTROL) | HBAP_CMDSTAT_FLAG_INTERFACE_COMMUNICATION_CONTROL_ACTIVE;
    hba_port->commandstatus = cmdstat;

    switch(hba_port->signature) {
    case 0xEB140101:
        fprintf(stderr, "ahci: port %d has SATA drive (sig=0x%08X)\n", port_index, hba_port->signature);
        break;

    default:
        fprintf(stderr, "ahci: port %d sig=0x%08X unknown\n", port_index, hba_port->signature);
        break;
    }
}

