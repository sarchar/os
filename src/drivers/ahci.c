#include "common.h"

#include "ahci.h"
#include "ata.h"
#include "hpet.h"
#include "interrupts.h"
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

#define CLEAR_ERROR(p) (p)->sata_error = (p)->sata_error
#define HBAP_COMMAND_HEADER_PTR(a,slot) (&((struct hba_command_header*)(a)->command_list_address)[slot])  // virtual memory ptr

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
    HBAP_SATA_CONTROL_FLAG_DET             = 0x0F << 0,
    HBAP_SATA_CONTROL_FLAG_DET_SHIFT       = 0,
    HBAP_SATA_CONTROL_FLAG_DET_INITIALIZE  = 1 << 0,
    HBAP_SATA_CONTROL_FLAG_DET_DISABLE     = 1 << 2,
    HBAP_SATA_CONTROL_FLAG_SPD_GEN1        = 1 << 4, // limit to Gen 1
    HBAP_SATA_CONTROL_FLAG_SPD_GEN2        = 2 << 4, // limit to <=Gen 2
    HBAP_SATA_CONTROL_FLAG_SPD_GEN3        = 3 << 4, // limit to <=Gen 3
    HBAP_SATA_CONTROL_FLAG_IPM_NO_PARTIAL  = 1 << 8,
    HBAP_SATA_CONTROL_FLAG_IPM_NO_SLUMBER  = 1 << 9,
    HBAP_SATA_CONTROL_FLAG_IPM_NO_DEVSLEEP = 1 << 10
};

enum HBAP_SATA_STATUS_FLAGS {
    HBAP_SATA_STATUS_FLAG_DET                       = 0x0F << 0,
    HBAP_SATA_STATUS_FLAG_DET_SHIFT                 = 0,
    HBAP_SATA_STATUS_FLAG_DET_DEVICE_PRESENT_NO_PHY = 1 << 0,
    HBAP_SATA_STATUS_FLAG_DET_DEVICE_PRESENT        = 3 << 0,
    HBAP_SATA_STATUS_FLAG_DET_PHY_OFFLINE           = 4 << 0,
    HBAP_SATA_STATUS_FLAG_SPEED                     = 0x0F << 4,
    HBAP_SATA_STATUS_FLAG_SPEED_SHIFT               = 4,
    HBAP_SATA_STATUS_FLAG_SPEED_GEN1                = 1 << 4,
    HBAP_SATA_STATUS_FLAG_SPEED_GEN2                = 2 << 4,
    HBAP_SATA_STATUS_FLAG_SPEED_GEN3                = 3 << 4,
    HBAP_SATA_STATUS_FLAG_POWER_STATE               = 0x0F << 8,
    HBAP_SATA_STATUS_FLAG_POWER_STATE_SHIFT         = 8,
    HBAP_SATA_STATUS_FLAG_POWER_STATE_ACTIVE        = 1 << 8,
    HBAP_SATA_STATUS_FLAG_POWER_STATE_PARTIAL       = 2 << 8,
    HBAP_SATA_STATUS_FLAG_POWER_STATE_SLUMBER       = 6 << 8,
    HBAP_SATA_STATUS_FLAG_POWER_STATE_DEVSLEEP      = 8 << 8
};

enum HBAP_INTERRUPT_ENABLE_FLAGS {
    HBAP_INTERRUPT_ENABLE_FLAG_D2H_FIS                   = 1 << 0,
    HBAP_INTERRUPT_ENABLE_FLAG_PIO_SETUP_FIS             = 1 << 1,
    HBAP_INTERRUPT_ENABLE_FLAG_DMA_SETUP_FIS             = 1 << 2,
    HBAP_INTERRUPT_ENABLE_FLAG_SET_DEVICE_BITS_FIS       = 1 << 3,
    HBAP_INTERRUPT_ENABLE_FLAG_UNKONWN_FIS               = 1 << 4,
    HBAP_INTERRUPT_ENABLE_FLAG_DESCRIPTOR_PROCESSED      = 1 << 5,
    HBAP_INTERRUPT_ENABLE_FLAG_PORT_CHANGE               = 1 << 6,
    HBAP_INTERRUPT_ENABLE_FLAG_MECHANICAL_PRESENCE       = 1 << 7,
    HBAP_INTERRUPT_ENABLE_FLAG_PHYRDY_CHANGE             = 1 << 22,
    HBAP_INTERRUPT_ENABLE_FLAG_INCORRECT_PORT_MULTIPLIER = 1 << 23,
    HBAP_INTERRUPT_ENABLE_FLAG_OVERFLOW                  = 1 << 24,
    HBAP_INTERRUPT_ENABLE_FLAG_INTERFACE_NONFATAL_ERROR  = 1 << 26,
    HBAP_INTERRUPT_ENABLE_FLAG_INTERFACE_FATAL_ERROR     = 1 << 27,
    HBAP_INTERRUPT_ENABLE_FLAG_HOST_BUS_DATA_ERROR       = 1 << 28,
    HBAP_INTERRUPT_ENABLE_FLAG_HOST_BUS_FATAL_ERROR      = 1 << 29,
    HBAP_INTERRUPT_ENABLE_FLAG_TASK_FILE_ERROR           = 1 << 30,
    HBAP_INTERRUPT_ENABLE_FLAG_COLD_PRESENCE_DETECT      = 1 << 31
};

enum HBAP_INTERRUPT_STATUS_FLAGS {
    HBAP_INTERRUPT_STATUS_FLAG_D2H_REGISTER_FIS = 1 << 0,
    HBAP_INTERRUPT_STATUS_FLAG_PIO_SETUP        = 1 << 1,
};

enum HBAP_TASK_FILE_DATA_FLAGS {
    HBAP_TASK_FILE_DATA_FLAG_STATUS         = 0xFF << 0,
    HBAP_TASK_FILE_DATA_FLAG_STATUS_SHIFT   = 0,
    HBAP_TASK_FILE_DATA_FLAG_STATUS_ERROR   = 1 << 0,
    HBAP_TASK_FILE_DATA_FLAG_STATUS_REQUEST = 1 << 3,
    HBAP_TASK_FILE_DATA_FLAG_STATUS_BUSY    = 1 << 7,
    HBAP_TASK_FILE_DATA_FLAG_ERROR          = 0xFF << 8,
    HBAP_TASK_FILE_DATA_FLAG_ERROR_SHIFT    = 8
};

enum FIS_TYPES {
    FIS_TYPE_REGISTER_H2D = 0x27,
    FIS_TYPE_REGISTER_D2H = 0x34,
    FIS_TYPE_PIO_SETUP    = 0x5F
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
    u8     num_command_slots;
    bool   is_atapi;
    u8     reserved0[6];
    struct ata_identify_device_response* identify_device_response;
};

struct hba_command_header {
    union {
        u8 dw0;
        struct {
            u8  fis_length     : 5; // Command FIS length in DWORDS, 2 ~ 16
            u8  atapi          : 1; // is an ATAPI command
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
 
    u16 prdt_length;               // Physical region descriptor table length in entries
 
    u32 prdt_transfer_count;       // Physical region descriptor byte count transferred
 
    u32 command_table_base;        // Command table descriptor base address
    u32 command_table_base_h;      // Command table descriptor base address upper 32 bits
 
    u32 reserved1[4];              // Reserved
} __packed;

// PRD - Physical Region Descriptor table entry
struct hba_prdt_entry {
    u32 data_base_address;      // Data base address
    u32 data_base_address_h;    // Data base address upper 32 bits
    u32 reserved0;              // Reserved
 
    u32 data_byte_count         : 22;  // Byte count, 4M max
    u32 reserved1               : 9;   // Reserved
    u32 interrupt_on_completion : 1;   // Interrupt on completion
} __packed;

struct hba_command_table {
    u8  command_fis[64];    // Command FIS, up to 64 bytes
    u8  atapi_command[16];  // ATAPI command, 12 or 16 bytes
    u8  reserved0[48];      // Reserved
    struct hba_prdt_entry   prdt_entries[];  // Physical region descriptor table entries, 0 ~ 65535
} __packed;

struct fis_register_host_to_device {
    // word 0
    u8 fis_type;     // FIS_TYPE_REG_D2H

    u8 port_multiplier_port : 4;    // Port multiplier
    u8 reserved0            : 3;    // Reserved
    u8 cmdcntrl             : 1;    // 1 = command, 0 = control

    u8 command;      // command register
    u8 featurel;     // feature register

    // word 1
    u8 lba0;         // LBA low register, 7:0
    u8 lba1;         // LBA mid register, 15:8
    u8 lba2;         // LBA high register, 23:16
    u8 device;       // Device register

    // word 2
    u8 lba3;         // LBA register, 31:24
    u8 lba4;         // LBA register, 39:32
    u8 lba5;         // LBA register, 47:40
    u8 featureh;     // feature register high

    // word 3
    u8 countl;       // Count register, 7:0
    u8 counth;       // Count register, 15:8
    u8 iso;          // isochronous command completion
    u8 control;      // Control register

    // word 4
    u8 reserved4[4]; // Reserved
} __packed;

struct fis {
    u8  blargh[256];
} __packed;

static struct ahci_device_port* ahci_device_ports[32];

static bool _declare_ownership();
static bool _reset_controller();
static struct ahci_device_port* _try_initialize_port(u8, u32);
static inline bool _start_port_processing(struct hba_port volatile*);
static inline bool _stop_port_processing(struct hba_port volatile*);
static void _reset_and_probe_ports();
static void _deactivate_port(u8);
static void _dump_port_registers(u8, char*);
static inline s8 _find_free_command_slot(u8);
static void _identify_device(u8);

static u64 ahci_irq = 0;
static void _ahci_interrupt(intp pc, void* userdata)
{
    unused(pc);
    unused(userdata);
    ahci_irq = 1;
}

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

    fprintf(stderr, "ahci: found device %04X:%04X (irq = %d)\n", dev->config->vendor_id, dev->config->device_id, dev->config->h0.interrupt_line);

    // also known as ABAR in the documentation
    ahci_base_memory = (struct hba_memory*)pci_device_map_bar(dev, 5);

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

    // map the interrupt from the PCI device to a new interrupt callback here in this module
    u32 cpu_irq = pci_setup_msi(dev, 1);
    pci_set_enable_msi(dev, true);
    interrupts_install_handler(cpu_irq, _ahci_interrupt, null);

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

        // device might exist, try enabling it
        ahci_device_ports[i] = _try_initialize_port(i, ncmds); // does not enable command processing just yet
    }

    // Like with hba_ports, writing a 1 to any interrupt bit that was set clears it
    ahci_base_memory->interrupt_status = ahci_base_memory->interrupt_status;

    // enable global interrupt mask in the HBA
    ahci_base_memory->global_host_control |= HBA_GHC_FLAG_INTERRUPT_ENABLE;

    // enable processing on all the ports
    for(u8 i = 0; i < 32; i++) {
        if(ahci_device_ports[i] == null) continue;

        struct hba_port volatile* hba_port = &ahci_base_memory->ports[i];

        // clear interrupt status first
        hba_port->interrupt_status = hba_port->interrupt_status;

        // start up processing on the port
        if(!_start_port_processing(hba_port)) {
            // weird timeout error, don't continue with this port? delete and free all memory
            assert(false, "TODO");
            continue;
        }

        // enable interrupts from that specific port
        hba_port->interrupt_enable |= 0xFFFFFFFF; // TODO enable all for now
    }

    // probe all ports for valid devices
    _reset_and_probe_ports();

    // identify all remaining devices
    for(u8 i = 0; i < 32; i++) {
        if(ahci_device_ports[i] == null) continue;
        _identify_device(i);
    }
}

static bool _declare_ownership()
{
    if((ahci_base_memory->extended_capabilities & HBA_CAPABILITIES_BIOS_HANDOVER) == 0) {
#if 0
        fprintf(stderr, "ahci: HBA doesn't support BIOS handover...assuming ownership.\n");
#endif
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

    fprintf(stderr, "ahci: HBA reset successful\n");
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

    for(u8 i = 0; i < 32; i++) {
        if(ahci_device_ports[i] == null) continue;
        fprintf(stderr, "ahci: port %d registers:\n", i);
        _dump_port_registers(i, "      ");
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

static inline bool _start_port_processing(struct hba_port volatile* hba_port)
{
    assert((hba_port->commandstatus & HBAP_CMDSTAT_FLAG_COMMAND_LIST_RUNNING) == 0, "port should be stopped before calling start");
    hba_port->commandstatus |= (HBAP_CMDSTAT_FLAG_START_COMMAND_LIST | HBAP_CMDSTAT_FLAG_FIS_RECEIVE_ENABLE);
    u64 tmp;
    wait_until_true(hba_port->commandstatus & HBAP_CMDSTAT_FLAG_COMMAND_LIST_RUNNING, 5000000, tmp) return false;
    return true;
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

static struct ahci_device_port* _try_initialize_port(u8 port_index, u32 ncmds)
{
    fprintf(stderr, "ahci: initializing port %d...\n", port_index);

    struct hba_port volatile* hba_port = &ahci_base_memory->ports[port_index];

    struct ahci_device_port* aport = (struct ahci_device_port*)kalloc(sizeof(struct ahci_device_port));
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
    if(!_stop_port_processing(hba_port)) {
        vmem_unmap_page(virt_addr);
        palloc_abandon(phys_page, 0);
        kfree(aport);
        return null;
    }

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
    aport->num_command_slots              = ncmds;

    // disable transitions to sleep states
    hba_port->sata_control |= (HBAP_SATA_CONTROL_FLAG_IPM_NO_PARTIAL | HBAP_SATA_CONTROL_FLAG_IPM_NO_SLUMBER);

    // all of interrupt_status is write '1' to clear, so wherever an interrupt has previously occurred
    // will be a 1 bit. writing to itself will write 1 in that location and clear it
    hba_port->interrupt_status = hba_port->interrupt_status;

    // same for any error bits
    CLEAR_ERROR(hba_port);

    // power on drive, spin up drive, set ICC to active, and enable the Receive FIS buffer
    u32 cmdstat = hba_port->commandstatus;
    cmdstat |= (HBAP_CMDSTAT_FLAG_POWER_ON_DEVICE | HBAP_CMDSTAT_FLAG_SPIN_UP_DEVICE | HBAP_CMDSTAT_FLAG_FIS_RECEIVE_ENABLE);
    cmdstat = (cmdstat & ~HBAP_CMDSTAT_FLAG_INTERFACE_COMMUNICATION_CONTROL) | HBAP_CMDSTAT_FLAG_INTERFACE_COMMUNICATION_CONTROL_ACTIVE;
    hba_port->commandstatus = cmdstat;

    // clear all command issues
    hba_port->command_issue = 0;

    return aport;
}

static bool _reset_port(struct hba_port volatile* hba_port, u8 port_index)
{
    u64 tmp;

    if(!_stop_port_processing(hba_port)) {
        fprintf(stderr, "ahci: failed to stop processing on port %d\n", port_index);
        return false;
    }

    // clear port error field
    CLEAR_ERROR(hba_port);

    // wait for ATA idle, and if it doesn't occur perform COMRESET
    wait_until_false(hba_port->task_file_data & (HBAP_TASK_FILE_DATA_FLAG_STATUS_BUSY | HBAP_TASK_FILE_DATA_FLAG_STATUS_REQUEST), 5000, tmp) {
        fprintf(stderr, "ahci: performing COMRESET on port %d\n", port_index);

        // disallow IPM and set DET to Initialize. this hard resets the port
        hba_port->sata_control = (HBAP_SATA_CONTROL_FLAG_IPM_NO_PARTIAL | HBAP_SATA_CONTROL_FLAG_IPM_NO_SLUMBER | HBAP_SATA_CONTROL_FLAG_DET_INITIALIZE);

        // software needs to wait a minimum of 1ms before continuing
        usleep(2000);

        // clear the initialize command from DET
        hba_port->sata_control &= ~HBAP_SATA_CONTROL_FLAG_DET;
    }

    // re-enable the port
    if(!_start_port_processing(hba_port)) {
        fprintf(stderr, "ahci: failed to start port processing on port %d\n", port_index);
        return false;
    }

    // wait for drive communication to be reestablished
    wait_until_true((hba_port->sata_status & HBAP_SATA_STATUS_FLAG_DET) == HBAP_SATA_STATUS_FLAG_DET_DEVICE_PRESENT, 5000000, tmp) {
        fprintf(stderr, "ahci: port %d timeout waiting on drive communication\n", port_index);
        return false;
    }

    // finally, clear errors
    CLEAR_ERROR(hba_port);

    fprintf(stderr, "ahci: reset of port %d completed\n", port_index);
    return true;
}

// Check to see if the device at this port is working and one we care about
static bool _probe_port(u8 port_index)
{
    u64 tmp;
    struct hba_port volatile* hba_port = &ahci_base_memory->ports[port_index];
    struct ahci_device_port* aport = ahci_device_ports[port_index];
    assert(aport != null, "don't call this function on an inactive port");

    // wait for drive to be ready
    // TODO not sure what this one is doing
    wait_until_true((hba_port->task_file_data & HBAP_TASK_FILE_DATA_FLAG_STATUS) != 0xFF, 10000000, tmp) {
        fprintf(stderr, "ahci: port %d drive timeout for ready state\n", port_index);
        return false;
    }

    switch(hba_port->sata_status & HBAP_SATA_STATUS_FLAG_SPEED) {
    case HBAP_SATA_STATUS_FLAG_SPEED_GEN1:
        fprintf(stderr, "ahci: port %d link speed 1.5Gbps\n", port_index);
        break;

    case HBAP_SATA_STATUS_FLAG_SPEED_GEN2:
        fprintf(stderr, "ahci: port %d link speed 3Gbps\n", port_index);
        break;

    case HBAP_SATA_STATUS_FLAG_SPEED_GEN3:
        fprintf(stderr, "ahci: port %d link speed 6Gbps\n", port_index);
        break;
    
    default:
        fprintf(stderr, "ahci: port %d link speed unknown\n", port_index);
        break;
    }

    switch(hba_port->signature) {
    case SATA_SIG_ATA:
        fprintf(stderr, "ahci: port %d has ATA drive (sig=0x%08X)\n", port_index, hba_port->signature);
        break;

    case SATA_SIG_ATAPI:
        fprintf(stderr, "ahci: port %d has ATAPI drive (sig=0x%08X)\n", port_index, hba_port->signature);
        break;

    default:
        fprintf(stderr, "ahci: port %d sig=0x%08X unknown\n", port_index, hba_port->signature);
        return false;
    }

    // wait until drive isn't busy (up to 30 seconds!)
    // see section 10.12 in the SATA 1.3.1 spec
    if(hba_port->task_file_data & HBAP_TASK_FILE_DATA_FLAG_STATUS_BUSY) {
        fprintf(stderr, "ahci: waiting for port %d drive not clear busy flag (up to 30 seconds)\n", port_index);
    }

    wait_until_false(hba_port->task_file_data & HBAP_TASK_FILE_DATA_FLAG_STATUS_BUSY, 30000000, tmp) {
        fprintf(stderr, "ahci: drive on port %d didn't complete request within 30 seconds\n", port_index);
        return false;
    }

    if((hba_port->sata_status & HBAP_SATA_STATUS_FLAG_DET) != HBAP_SATA_STATUS_FLAG_DET_DEVICE_PRESENT) {
        fprintf(stderr, "ahci: no drive on port %d present (or PHY is not communicating)\n", port_index);
        return false;
    }

    // for ATAPI drives, set the commandstatus bit to tell the HBA to activate the desktop LED (why?)
    aport->is_atapi = (hba_port->signature == SATA_SIG_ATAPI);
    if(aport->is_atapi) {
        hba_port->commandstatus |= HBAP_CMDSTAT_FLAG_ATAPI;
    } else {
        hba_port->commandstatus &= ~HBAP_CMDSTAT_FLAG_ATAPI;
    }

    return true;
}

static bool _reset_and_probe_port(u8 port_index)
{
    struct hba_port volatile* hba_port = &ahci_base_memory->ports[port_index];
    struct ahci_device_port* aport = ahci_device_ports[port_index];
    assert(aport != null, "don't call this function on an inactive port");

    if(!_reset_port(hba_port, port_index)) {
        fprintf(stderr, "ahci: failed to reset port %d\n", port_index);
        return false;
    }

    return _probe_port(port_index);
}

static void _reset_and_probe_ports()
{
    for(u8 i = 0; i < 32; i++) {
        if(ahci_device_ports[i] == null) continue;

        // next device
        if(_reset_and_probe_port(i)) {
            fprintf(stderr, "ahci: port %d is connected to a device and working\n", i);
            continue;
        }

        // disable all future access to this port
        _deactivate_port(i);
    }
}

static void _deactivate_port(u8 port_index)
{
    struct hba_port volatile* hba_port = &ahci_base_memory->ports[port_index];
    _stop_port_processing(hba_port); // even if this times out, we don't care any more

    struct ahci_device_port* aport = ahci_device_ports[port_index];

    // if an IDENTIFY exists, free the memory
    if(aport->identify_device_response) {
        intp phys = vmem_unmap_page((intp)aport->identify_device_response);
        palloc_abandon(phys, 0);
    }

    // unmap the memory used by command_list_address and free the page associated with it
    vmem_unmap_page(aport->command_list_address);
    palloc_abandon(aport->command_list_phys_address, 0); // command_list_phys_address is always at the start of the allocated page

    // free the ahci_device_port node
    kfree(aport);

    // clear the pointer in ahci_device_ports
    ahci_device_ports[port_index] = null;
}

static void _dump_port_registers(u8 port_index, char* prefix)
{
    struct hba_port volatile* hba_port = &ahci_base_memory->ports[port_index];
    struct ahci_device_port* aport = ahci_device_ports[port_index];
    assert(aport != null, "don't call this function on an inactive port");

    fprintf(stderr, "%scommand_list_base_address = 0x%lX\n", prefix, ((u64)hba_port->command_list_base_address_h << 32) | (u64)hba_port->command_list_base_address);
    fprintf(stderr, "%sreceived_fis_base_address = 0x%lX\n", prefix, ((u64)hba_port->received_fis_base_address_h << 32) | (u64)hba_port->received_fis_base_address);
    fprintf(stderr, "%sinterrupt_status = 0x%lX\n", prefix, hba_port->interrupt_status);
    fprintf(stderr, "%scommandstatus = 0x%lX\n", prefix, hba_port->commandstatus);
    fprintf(stderr, "%stask_file_data = 0x%lX\n", prefix, hba_port->task_file_data);
    fprintf(stderr, "%ssignature = 0x%lX\n", prefix, hba_port->signature);
    fprintf(stderr, "%ssata_status = 0x%lX\n", prefix, hba_port->sata_status);
    fprintf(stderr, "%ssata_control = 0x%lX\n", prefix, hba_port->sata_control);
    fprintf(stderr, "%ssata_error = 0x%lX\n", prefix, hba_port->sata_error);
    fprintf(stderr, "%ssata_active = 0x%lX\n", prefix, hba_port->sata_active);
    fprintf(stderr, "%scommand_issue = 0x%lX\n", prefix, hba_port->command_issue);
    fprintf(stderr, "%ssata_notification = 0x%lX\n", prefix, hba_port->sata_notification);
    fprintf(stderr, "%sfis_switch_control = 0x%lX\n", prefix, hba_port->fis_switch_control);
}

// Find a free command list slot
static inline s8 _find_free_command_slot(u8 port_index)
{
    struct hba_port volatile* hba_port = &ahci_base_memory->ports[port_index];
    struct ahci_device_port* aport = ahci_device_ports[port_index];
    assert(aport != null, "don't call this function on an inactive port");

    // a slot is free only if it's not active and not issued
	u32 slots = (hba_port->sata_active | hba_port->command_issue);

	for(u8 i = 0; i < aport->num_command_slots; i++) {
        if((slots & (1 << i)) == 0) return (s8)i;
	}

	return -1;
}

static void _print_device_size(u8 port_index)
{
    struct ahci_device_port* aport = ahci_device_ports[port_index];
    assert(aport != null, "don't call this function on an inactive port");
    assert(aport->identify_device_response != null, "must issue IDENTIFY DEVICE first");

    struct ata_identify_device_response* resp = aport->identify_device_response;

    u64 logical_sector_size = 2*(((u32)resp->logical_sector_size[0] | ((u32)resp->logical_sector_size[1] << 16)));
    if(!resp->logical_sector_longer_than_256_words) logical_sector_size = 512;
    u64 logical_sector_count = (u64)resp->total_logical_sectors_lba48[0] | ((u64)resp->total_logical_sectors_lba48[1] << 16) 
                               | ((u64)resp->total_logical_sectors_lba48[2] << 32) | ((u64)resp->total_logical_sectors_lba48[3] << 48);
    if(!resp->lba48_address_feature_set_supported) logical_sector_count = (u32)resp->total_logical_sectors[0] | ((u64)resp->total_logical_sectors[1] << 16);
    fprintf(stderr, "ahci: port %d device has size=%llu bytes, sector size = %d\n", port_index, logical_sector_count * logical_sector_size, logical_sector_size);
}


static void _identify_device(u8 port_index)
{
    u64 tmp;
    s8 cmdslot;

    struct hba_port volatile* hba_port = &ahci_base_memory->ports[port_index];
    struct ahci_device_port* aport = ahci_device_ports[port_index];
    assert(aport != null, "don't call this function on an inactive port");

    wait_until_false((cmdslot = _find_free_command_slot(port_index)) < 0, 1000000, tmp) {
        fprintf(stderr, "ahci: couldn't find a free command slot for port %d after 1s\n", port_index);
        return;
    }

    struct hba_command_header* hdr = HBAP_COMMAND_HEADER_PTR(aport, cmdslot);
	zero(hdr);

    // allocate space for a command table and PRDs
    intp cmd_table_phys = (intp)palloc_claim_one(); // command table must be 128 byte aligned
    intp cmd_table_virt = vmem_map_page(cmd_table_phys, MAP_PAGE_FLAG_WRITABLE | MAP_PAGE_FLAG_DISABLE_CACHE);

    // point the header at the new command table
    struct hba_command_table* tbl = (struct hba_command_table*)cmd_table_virt;
	zero(tbl);
    hdr->command_table_base       = (u32)(cmd_table_phys & 0xFFFFFFFF);
    hdr->command_table_base_h     = (u32)(cmd_table_phys >> 32);

    // set up the IDENTIFY command
    struct fis_register_host_to_device* command_fis = (struct fis_register_host_to_device*)tbl->command_fis;
    command_fis->fis_type = FIS_TYPE_REGISTER_H2D;
    command_fis->cmdcntrl  = 1; // command, not control
    command_fis->command   = aport->is_atapi ? ATA_COMMAND_IDENTIFY_PACKET_DEVICE : ATA_COMMAND_IDENTIFY_DEVICE;
    command_fis->device    = 0; // master device

    // set up a destination PRDT
    // the memory pointed to by the command table is one page (4096) long
    // but the table itself takes up 128 bytes with the remaining space available to
    // PRDT entries. So (4096-128)/16 = 248 max PRDTs
    struct hba_prdt_entry* prdt_entry = &tbl->prdt_entries[0];
    intp dest_phys = (intp)palloc_claim_one();
    intp dest_virt = vmem_map_page(dest_phys, MAP_PAGE_FLAG_WRITABLE);
    prdt_entry->data_base_address       = (u32)(dest_phys & 0xFFFFFFFF);
    prdt_entry->data_base_address_h     = (u32)(dest_phys >> 32);
    prdt_entry->data_byte_count         = 512; // size of ATA device info block
    prdt_entry->interrupt_on_completion = 1; // tell me when you're done

    // set up the rest of the command header
    hdr->fis_length          = sizeof(*command_fis) / sizeof(u32);
    hdr->host_to_device      = 0; // read from device
	hdr->atapi               = 0;
    hdr->prdt_transfer_count = 0;
    hdr->prdt_length         = 1;

    // OK, signal to the HBA that there's a command on that port
    fprintf(stderr, "ahci: issuing IDENTIFY DEVICE on port %d\n", port_index);
    hba_port->command_issue |= (1 << cmdslot);

    // Wait for completion
    wait_until_false(hba_port->command_issue & (1 << cmdslot), 10000000, tmp) {
        fprintf(stderr, "ahci: command did not activate\n");
        // TODO free memory and return error
    } 

    wait_until_false(hba_port->task_file_data & (HBAP_TASK_FILE_DATA_FLAG_STATUS_BUSY | HBAP_TASK_FILE_DATA_FLAG_STATUS_REQUEST), 1000000, tmp) {
        fprintf(stderr, "ahci: port %d timeout on drive busy\n", port_index);
        // TODO free memory and return error
    } 

    if(hba_port->task_file_data & HBAP_TASK_FILE_DATA_FLAG_ERROR) {
        fprintf(stderr, "ahci: port %d ata command error\n", port_index);
        fprintf(stderr, "ahci: sata_error = 0x%lX\n", hba_port->sata_error);
        // TODO free memory and return error
    }

    // Wait for PIO Setup IRQ
//    wait_until_true(hba_port->interrupt_status & HBAP_INTERRUPT_STATUS_FLAG_D2H_REGISTER_FIS, 1000000, tmp) {
    wait_until_true(ahci_irq, 1000000, tmp) {
        fprintf(stderr, "ahci: port %d timed out waiting for D2H interrupt\n", port_index);
        // TODO free memory and return error
    } 

    // Clear interrupt request
    hba_port->interrupt_status = hba_port->interrupt_status;

#if 0
    ata_dump_identify_device_response(port_index, (struct ata_identify_device_response*)dest_virt);
#endif

    // Save the response
    aport->identify_device_response = (struct ata_identify_device_response*)dest_virt;

    // Calculate the drive space and 
    _print_device_size(port_index);

    // Free the memory associated with the command table
    vmem_unmap_page(cmd_table_virt);
    palloc_abandon(cmd_table_phys, 0);
    hdr->command_table_base   = 0;
    hdr->command_table_base_h = 0;
}


