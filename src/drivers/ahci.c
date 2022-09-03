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
#include "vmem.h"

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

// An hba_command_table that's allocated on 1 physical page of memory can hold up to 248 PRDTs
// Allocate consecutive pages to get another 256 PRDTs per page
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
//static void _read_device(u8);
//static void _write_device(u8);

static u64 ahci_irq = 0;
typedef void (hba_port_interrupt_handler)(u8 port_index, struct hba_port volatile*);

static void _ahci_port_interrupt_d2h(u8 port_index, struct hba_port volatile* hba_port)
{
    unused(port_index);
    unused(hba_port);

    // queue a thing onto the global kernel processing list..?
    ahci_irq = 1;
}

static hba_port_interrupt_handler* hba_port_interrupt_table[32] = {
    [HBAP_INTERRUPT_STATUS_FLAG_D2H_REGISTER_FIS] = &_ahci_port_interrupt_d2h,
};

static void _ahci_port_interrupt(u8 port_index)
{
    struct hba_port volatile* hba_port = &ahci_base_memory->ports[port_index];
    u32 is = hba_port->interrupt_status;

    while(is != 0) {
        for(u8 bit = 0; bit < 32; bit++) {
            if((is & (1 << bit)) != 0 && hba_port_interrupt_table[bit] != null) hba_port_interrupt_table[bit](port_index, hba_port);
        }

        // clear interrupt status
        // make sure to use `is` JIC some other bits were set during processing
        hba_port->interrupt_status = is;
        is = hba_port->interrupt_status;
    }
}

static void _ahci_interrupt(struct interrupt_stack_registers* regs, intp pc, void* userdata)
{
    unused(regs);
    unused(pc);
    unused(userdata);
    u32 port_interrupt_status = ahci_base_memory->interrupt_status;

    while(port_interrupt_status != 0) {
        // find out which ports have an interrupt pending and process them
        for(u8 i = 0; i < 32; i++) {
            if(ahci_base_memory->interrupt_status & (1 << i)) _ahci_port_interrupt(i);
        }

        // clear interrupt status flag
        ahci_base_memory->interrupt_status = port_interrupt_status;
        port_interrupt_status = ahci_base_memory->interrupt_status;
    }
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

    fprintf(stderr, "ahci: found device %04X:%04X (interrupt_line = %d)\n", dev->config->vendor_id, dev->config->device_id, dev->config->h0.interrupt_line);

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

        struct hba_port volatile* port = &ahci_base_memory->ports[i];
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

        if(!ahci_device_ports[i]->is_atapi) {
            //_write_device(i);
            //_read_device(i);
        }
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

u32 ahci_get_device_sector_size(u8 port_index)
{
    struct ahci_device_port* aport = ahci_device_ports[port_index];
    assert(aport != null, "don't call this function on an inactive port");
    assert(aport->identify_device_response != null, "must issue IDENTIFY DEVICE first");
    struct ata_identify_device_response* resp = aport->identify_device_response;

    u32 logical_sector_size = 2*(((u32)resp->logical_sector_size[0] | ((u32)resp->logical_sector_size[1] << 16)));
    if(!resp->logical_sector_longer_than_256_words) logical_sector_size = 512;

    return logical_sector_size;
}

u32 ahci_get_first_nonpacket_device_port()
{
    for(u32 i = 0; i < 32; i++) {
        if(ahci_device_ports[i] == null) continue;
        if(ahci_device_ports[i]->is_atapi) continue;
        return i;
    }

    return (u32)-1;
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
    intp virt_addr = vmem_map_page(VMEM_KERNEL, phys_page, MAP_PAGE_FLAG_WRITABLE | MAP_PAGE_FLAG_DISABLE_CACHE);

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
        vmem_unmap_page(VMEM_KERNEL, virt_addr);
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
            //fprintf(stderr, "ahci: port %d is connected to a device and working\n", i);
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
        intp phys = vmem_unmap_page(VMEM_KERNEL, (intp)aport->identify_device_response);
        palloc_abandon(phys, 0);
    }

    // unmap the memory used by command_list_address and free the page associated with it
    vmem_unmap_page(VMEM_KERNEL, aport->command_list_address);
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

static struct hba_command_header* _setup_new_command(u8 port_index, u8 host_to_device, u8 is_atapi_command, u8 prdt_transfer_count, u8* cmdslot)
{
    // First, find a command slot
    u64 tmp;
    s8 c;
    wait_until_false((c = _find_free_command_slot(port_index)) < 0, 1000000, tmp) {
        fprintf(stderr, "ahci: couldn't find a free command slot for port %d after 1s\n", port_index);
        return null;
    }

    // found command slot
    *cmdslot = c;

    // get pointer to the command list entry/header
    struct ahci_device_port* aport = ahci_device_ports[port_index];
    struct hba_command_header* hdr = HBAP_COMMAND_HEADER_PTR(aport, *cmdslot);
	zero(hdr);

    // set up the rest of the command header
    hdr->host_to_device      = host_to_device;
	hdr->atapi               = is_atapi_command;
    hdr->prdt_length         = prdt_transfer_count;
    hdr->prdt_transfer_count = 0; // reset the byte count transfer

    return hdr;
}

static struct hba_command_table* _create_command_table(struct hba_command_header* hdr, u32 num_prdts)
{
    u8 palloc_order = 0;
    if(num_prdts > 248) { // number of PRDT entries in the first page
        u8 more_pages = 0;
        more_pages = ((num_prdts - 248) + 255) >> 8; // round up

        palloc_order = next_power_of_2(1 + more_pages); // 1 for the first command table page
        fprintf(stderr, "ahci: num_prdts=%d palloc_order=%d\n", num_prdts, palloc_order);

        assert(false, "not entirely implememnted yet. need vmem_map_region to get a virtual contiguous address space");
    }

    // allocate space for a command table and PRDs
    // TODO use valloc() -- allocates virtual space and then vmem_get_phys()?
    intp phys = (intp)palloc_claim(palloc_order); // command table must be 128 byte aligned

    // the base address is where the table will go
    // TODO we need vmem_map_region, and we can't use vmalloc because physical memory must be contiguous
    struct hba_command_table* tbl = (struct hba_command_table*)vmem_map_page(VMEM_KERNEL, phys, MAP_PAGE_FLAG_WRITABLE | MAP_PAGE_FLAG_DISABLE_CACHE);
	zero(tbl);

    // point the header at the new command table
    hdr->command_table_base   = (u32)(phys & 0xFFFFFFFF);
    hdr->command_table_base_h = (u32)(phys >> 32);

    return tbl;
}

static void _free_command_table(struct hba_command_header* hdr, struct hba_command_table* tbl, u32 num_prdts)
{
    u8 palloc_order = 0;
    if(num_prdts > 248) { // number of PRDT entries in the first page
        u8 more_pages = 0;
        more_pages = ((num_prdts - 248) + 255) >> 8; // round up

        palloc_order = next_power_of_2(1 + more_pages); // 1 for the first command table page
        fprintf(stderr, "ahci: num_prdts=%d palloc_order=%d\n", num_prdts, palloc_order);

        assert(false, "not entirely implememnted yet. need vmem_map_region to get a virtual contiguous address space");
    }

    intp phys = (intp)hdr->command_table_base | ((intp)hdr->command_table_base_h << 32);
    hdr->command_table_base   = 0;
    hdr->command_table_base_h = 0;

    vmem_unmap_page(VMEM_KERNEL, (intp)tbl);
    palloc_abandon(phys, palloc_order);
}

static void _set_h2d_fis(struct hba_command_header* hdr, struct hba_command_table* tbl, u8 cmdcntrl, u8 ata_command, u8 device, u64 lba, u16 count)
{
    // the register fis is memory in the command table
    struct fis_register_host_to_device* h2d_fis = (struct fis_register_host_to_device*)tbl->command_fis;
    h2d_fis->fis_type = FIS_TYPE_REGISTER_H2D;

    h2d_fis->cmdcntrl  = cmdcntrl; 
    h2d_fis->command   = ata_command;
    h2d_fis->device    = device;

    // Set up the start and length of the operation
    h2d_fis->lba0      = lba & 0xFF;
    h2d_fis->lba1      = (lba >> 8) & 0xFF;
    h2d_fis->lba2      = (lba >> 16) & 0xFF;
    h2d_fis->lba3      = (lba >> 24) & 0xFF;
    h2d_fis->lba4      = (lba >> 32) & 0xFF;
    h2d_fis->lba5      = (lba >> 40) & 0xFF;
    h2d_fis->countl    = count & 0xFF;
    h2d_fis->counth    = (count >> 8) & 0xFF;

    // set the length of the FIS in the command header
    hdr->fis_length    = sizeof(*h2d_fis) / sizeof(u32);
}

static void _set_prdt_entry(struct hba_command_table* tbl, u16 prdt_index, intp phys, u32 count, bool interrupt_on_completion)
{
    struct hba_prdt_entry* prdt_entry = &tbl->prdt_entries[prdt_index];

    prdt_entry->data_base_address       = (u32)(phys & 0xFFFFFFFF);
    prdt_entry->data_base_address_h     = (u32)(phys >> 32);
    prdt_entry->data_byte_count         = count - 1;
    prdt_entry->interrupt_on_completion = (interrupt_on_completion) ? 1 : 0; // 
}

__always_inline static void _issue_command(struct hba_port volatile* hba_port, u8 command_slot)
{
    hba_port->command_issue |= (1 << command_slot);

    // Wait for acknowledgement (bit will be set to 0, and sata_active should be set high)
    u64 tmp;
    wait_until_false(hba_port->command_issue & (1 << command_slot), 10000000, tmp) {
        fprintf(stderr, "ahci: command did not activate\n");
        // TODO free memory and return error
    } 
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
    u8 cmdslot;

    struct hba_port volatile* hba_port = &ahci_base_memory->ports[port_index];
    struct ahci_device_port* aport = ahci_device_ports[port_index];
    assert(aport != null, "don't call this function on an inactive port");

    // Set up a new command header
    struct hba_command_header* hdr = _setup_new_command(port_index, 0, 0, 1, &cmdslot);
    if(hdr == null) {
        fprintf(stderr, "ahci: port %d failed to find free command list entry\n", port_index);
        return;
    }

    // Create a new table for the command
    struct hba_command_table* tbl = _create_command_table(hdr, 1);

    // set up the IDENTIFY command
    _set_h2d_fis(hdr, tbl, 1, aport->is_atapi ? ATA_COMMAND_IDENTIFY_PACKET_DEVICE : ATA_COMMAND_IDENTIFY_DEVICE, 0, 0, 0);

    // set up a destination PRDT
    intp dest_phys = (intp)kalloc(512);
    intp dest_virt = dest_phys;
    _set_prdt_entry(tbl, 0, dest_phys, 512, true);

    // TODO is this necessary?
    // wait until the drive is idle
    //wait_until_false(hba_port->task_file_data & (HBAP_TASK_FILE_DATA_FLAG_STATUS_BUSY | HBAP_TASK_FILE_DATA_FLAG_STATUS_REQUEST), 1000000, tmp) {
    //    fprintf(stderr, "ahci: port %d timeout on drive busy\n", port_index);
    //    // TODO free memory and return error
    //} 

    // OK, signal to the HBA that there's a command on that port
    //fprintf(stderr, "ahci: issuing IDENTIFY DEVICE on port %d\n", port_index);
    _issue_command(hba_port, cmdslot);

    // Wait for the D2H response irq
    wait_until_true(ahci_irq, 1000000, tmp) {
        fprintf(stderr, "ahci: port %d timed out waiting for D2H interrupt\n", port_index);
        // TODO free memory and return error
    } 

    // Check for error
    if(hba_port->task_file_data & HBAP_TASK_FILE_DATA_FLAG_ERROR) {
        fprintf(stderr, "ahci: port %d ata command error\n", port_index);
        fprintf(stderr, "ahci: sata_error = 0x%lX\n", hba_port->sata_error);
        // TODO free memory and return error
    }

#if 0
    ata_dump_identify_device_response(port_index, (struct ata_identify_device_response*)dest_virt);
#endif

    // Save the response
    aport->identify_device_response = (struct ata_identify_device_response*)dest_virt;

    // Calculate the drive space and 
    _print_device_size(port_index);

    // Free the memory associated with the command table
    _free_command_table(hdr, tbl, 1);
}

#if 0
static void _read_device(u8 port_index)
{
    u64 tmp;
    u8 cmdslot;

    struct hba_port volatile* hba_port = &ahci_base_memory->ports[port_index];
    assert(ahci_device_ports[port_index] != null, "don't call this function on an inactive port");

    // Set up a new command header
    struct hba_command_header* hdr = _setup_new_command(port_index, 0, 0, 1, &cmdslot);
    if(hdr == null) {
        fprintf(stderr, "ahci: port %d failed to find free command list entry\n", port_index);
        return;
    }

    // Create a new table for the command
    struct hba_command_table* tbl = _create_command_table(hdr, 1);

    // set up the READ DMA EXT command
    u64 start_lba = 2;           // read from offset 1024
    u16 num_sectors = 4096/512;  // read 8 sectors == 4096 bytes == 1 page
    _set_h2d_fis(hdr, tbl, 1, ATA_COMMAND_READ_DMA_EXT, (1 << 6), start_lba, num_sectors);

    // allocate memory for the read destination
    // TODO should be provided by the calling function
    intp dest_phys = (intp)palloc_claim_one(); // allocate memory for the read
    intp dest_virt = vmem_map_page(dest_phys, MAP_PAGE_FLAG_WRITABLE);

    // set up a the read destination in a prdt
    // read one full page into prdt 0 at dest_phys and signal interrupt after completed
    _set_prdt_entry(tbl, 0, dest_phys, 4096, true);

    // OK, signal to the HBA that there's a command on that port
    fprintf(stderr, "ahci: issuing READ DMA EXT on port %d\n", port_index);
    _issue_command(hba_port, cmdslot);

    // Wait for D2H response which will be notified via an interrupt
    wait_until_true(ahci_irq, 1000000, tmp) {
        fprintf(stderr, "ahci: port %d timed out waiting for D2H interrupt\n", port_index);
        // TODO free memory and return error
    } 

    // Check for read error
    if(hba_port->task_file_data & HBAP_TASK_FILE_DATA_FLAG_ERROR) {
        fprintf(stderr, "ahci: port %d ata command error\n", port_index);
        fprintf(stderr, "ahci: sata_error = 0x%lX\n", hba_port->sata_error);
        // TODO free memory and return error
    }

    // print 512 bytes in 16 byte lines
#if 0
    fprintf(stderr, "ahci: port %d received:\n", port_index);
    for(u32 offs = 0; offs < 512; offs += 16) {
        fprintf(stderr, "    %04X: ", offs);
        for(u32 i = 0; i < 16; i++) {
            fprintf(stderr, "%02X ", ((u8 volatile*)dest_virt)[offs+i]);
        }
        fprintf(stderr, "- ");
        for(u32 i = 0; i < 16; i++) {
            u8 c = ((u8 volatile*)dest_virt)[offs+i];
            if(c >= 0x20 && c <= 0x7f) {
                fprintf(stderr, "%c", c);
            } else {
                fprintf(stderr, ".");
            }
        }
        fprintf(stderr, "\n");
    }
#endif

    // Free the read destination memory
    vmem_unmap_page(dest_virt);
    palloc_abandon(dest_phys, 0);

    // Free command table
    _free_command_table(hdr, tbl, 1);
}

static void _write_device(u8 port_index)
{
    u64 tmp;
    u8 cmdslot;

    struct hba_port volatile* hba_port = &ahci_base_memory->ports[port_index];
    assert(ahci_device_ports[port_index] != null, "don't call this function on an inactive port");

    // Set up a new command header
    struct hba_command_header* hdr = _setup_new_command(port_index, 1, 0, 1, &cmdslot);
    if(hdr == null) {
        fprintf(stderr, "ahci: port %d failed to find free command list entry\n", port_index);
        return;
    }

    // Create a new table for the command
    struct hba_command_table* tbl = _create_command_table(hdr, 1);

    // set up the WRITE DMA EXT command
    u64 start_lba = 0;           // read from offset 0
    u16 num_sectors = 4096/512;  // read 8 sectors == 4096 bytes == 1 page
    _set_h2d_fis(hdr, tbl, 1, ATA_COMMAND_WRITE_DMA_EXT, (1 << 6), start_lba, num_sectors);

    // allocate memory for the write data source
    // TODO should be provided by the calling function
    intp src_phys = (intp)palloc_claim_one(); // allocate memory for the read
    intp src_virt = vmem_map_page(src_phys, MAP_PAGE_FLAG_WRITABLE);

    // set up a the write source in a prdt
    // write one full page from prdt 0 at src_phys and signal interrupt after completed
    _set_prdt_entry(tbl, 0, src_phys, 4096, true);

    // put some data in the memory
    memcpy((void*)src_virt, (void*)"!!!!!HELLO!!!!!!", 16);
    asm volatile("clflush %0\n" : : "m"(src_virt));

    // OK, signal to the HBA that there's a command on that port
    fprintf(stderr, "ahci: issuing WRITE DMA EXT on port %d\n", port_index);
    _issue_command(hba_port, cmdslot);

    // Wait for D2H response
    wait_until_true(ahci_irq, 1000000, tmp) {
        fprintf(stderr, "ahci: port %d timed out waiting for D2H interrupt\n", port_index);
        // TODO free memory and return error
    } 

    // Check for write error
    if(hba_port->task_file_data & HBAP_TASK_FILE_DATA_FLAG_ERROR) {
        fprintf(stderr, "ahci: port %d ata command error\n", port_index);
        fprintf(stderr, "ahci: sata_error = 0x%lX\n", hba_port->sata_error);
        // TODO free memory and return error
    }

    // Done
    fprintf(stderr, "ahci: write to port %d finished\n", port_index);

    // Free the source memory
    vmem_unmap_page(src_virt);
    palloc_abandon(src_phys, 0);

    // Free command table
    _free_command_table(hdr, tbl, 1);
}
#endif


bool ahci_read_device_sectors(u8 port_index, u64 start_lba, u64 num_sectors, intp dest)
{
    u64 tmp;
    u8 cmdslot;

    struct hba_port volatile* hba_port = &ahci_base_memory->ports[port_index];
    assert(ahci_device_ports[port_index] != null, "don't call this function on an inactive port");

    // Set up a new command header
    struct hba_command_header* hdr = _setup_new_command(port_index, 0, 0, 1, &cmdslot);
    if(hdr == null) {
        fprintf(stderr, "ahci: port %d failed to find free command list entry\n", port_index);
        return false;
    }

    // Figure out the # of PRDTs we'll need
    u64 sector_size = ahci_get_device_sector_size(port_index);
    u64 read_length = num_sectors * sector_size;
    u64 num_prdts = 0;
    while(read_length > 0) {
        read_length -= min(4*1024*1024, read_length); // one PRDT can refer to up to 4MB of data
        num_prdts++;
    }

    // Create a new table for the command
    struct hba_command_table* tbl = _create_command_table(hdr, num_prdts);

    // set up the READ DMA EXT command
    _set_h2d_fis(hdr, tbl, 1, ATA_COMMAND_READ_DMA_EXT, (1 << 6), start_lba, num_sectors);

    // set up a the read destination in a prdt
    // read one full page into prdt 0 at dest_phys and signal interrupt after completed
    // TODO should call vmem_get_phys(dest), but right now it's identity mapped
    for(u32 prdt_index = 0; prdt_index < num_prdts; prdt_index++) {
        u32 prdt_size = min(num_sectors * sector_size, 4*1024*1024);
        _set_prdt_entry(tbl, prdt_index, dest, prdt_size, true);
        dest += prdt_size;
        num_sectors -= prdt_size;
    }

    // OK, signal to the HBA that there's a command on that port
    //fprintf(stderr, "ahci: issuing READ DMA EXT on port %d, start_lba=%llu\n", port_index, start_lba);
    _issue_command(hba_port, cmdslot);

    // Wait for D2H response which will be notified via an interrupt
    wait_until_true(ahci_irq, 1000000, tmp) {
        fprintf(stderr, "ahci: port %d timed out waiting for D2H interrupt\n", port_index);
        // TODO free memory and return error
    } 

    // Check for read error
    if(hba_port->task_file_data & HBAP_TASK_FILE_DATA_FLAG_ERROR) {
        fprintf(stderr, "ahci: port %d ata command error\n", port_index);
        fprintf(stderr, "ahci: sata_error = 0x%lX\n", hba_port->sata_error);
        // TODO free memory and return error
    }

    // print 512 bytes in 16 byte lines
#if 0
    fprintf(stderr, "ahci: port %d received:\n", port_index);
    for(u32 offs = 0; offs < 512; offs += 16) {
        fprintf(stderr, "    %04X: ", offs);
        for(u32 i = 0; i < 16; i++) {
            fprintf(stderr, "%02X ", ((u8 volatile*)dest_virt)[offs+i]);
        }
        fprintf(stderr, "- ");
        for(u32 i = 0; i < 16; i++) {
            u8 c = ((u8 volatile*)dest_virt)[offs+i];
            if(c >= 0x20 && c <= 0x7f) {
                fprintf(stderr, "%c", c);
            } else {
                fprintf(stderr, ".");
            }
        }
        fprintf(stderr, "\n");
    }
#endif

    // Free command table
    _free_command_table(hdr, tbl, num_prdts);

    return true;
}

bool ahci_write_device_sectors(u8 port_index, u64 start_lba, u64 num_sectors, intp src)
{
    u64 tmp;
    u8 cmdslot;

    struct hba_port volatile* hba_port = &ahci_base_memory->ports[port_index];
    assert(ahci_device_ports[port_index] != null, "don't call this function on an inactive port");

    // Set up a new command header
    struct hba_command_header* hdr = _setup_new_command(port_index, 1, 0, 1, &cmdslot);
    if(hdr == null) {
        fprintf(stderr, "ahci: port %d failed to find free command list entry\n", port_index);
        return false;
    }

    // Figure out the # of PRDTs we'll need
    u64 sector_size = ahci_get_device_sector_size(port_index);
    u64 write_length = num_sectors * sector_size;
    u64 num_prdts = 0;
    while(write_length > 0) {
        write_length -= min(4*1024*1024, write_length); // one PRDT can refer to up to 4MB of data
        num_prdts++;
    }

    // Create a new table for the command
    struct hba_command_table* tbl = _create_command_table(hdr, num_prdts);

    // set up the WRITE DMA EXT command
    _set_h2d_fis(hdr, tbl, 1, ATA_COMMAND_WRITE_DMA_EXT, (1 << 6), start_lba, num_sectors);

    // set up a the read destination in a prdt
    // write one full page into prdt 0 at dest_phys and signal interrupt after completed
    // TODO should call vmem_get_phys(dest), but right now it's identity mapped
    for(u32 prdt_index = 0; prdt_index < num_prdts; prdt_index++) {
        u32 prdt_size = min(num_sectors * sector_size, 4*1024*1024);
        _set_prdt_entry(tbl, prdt_index, src, prdt_size, true);
        src += prdt_size;
        num_sectors -= prdt_size;
    }

    // OK, signal to the HBA that there's a command on that port
    //fprintf(stderr, "ahci: issuing WRITE DMA EXT on port %d\n", port_index);
    _issue_command(hba_port, cmdslot);

    // Wait for D2H response which will be notified via an interrupt
    wait_until_true(ahci_irq, 1000000, tmp) {
        fprintf(stderr, "ahci: port %d timed out waiting for D2H interrupt\n", port_index);
        // TODO free memory and return error
    } 

    // Check for read error
    if(hba_port->task_file_data & HBAP_TASK_FILE_DATA_FLAG_ERROR) {
        fprintf(stderr, "ahci: port %d ata command error\n", port_index);
        fprintf(stderr, "ahci: sata_error = 0x%lX\n", hba_port->sata_error);
        // TODO free memory and return error
    }

    // Free command table
    _free_command_table(hdr, tbl, num_prdts);

    return true;
}

