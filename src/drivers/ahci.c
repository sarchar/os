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

enum ATA_COMMANDS {
    ATA_COMMAND_IDENTIFY_PACKET_DEVICE = 0xA1,
    ATA_COMMAND_IDENTIFY               = 0xEC
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
    intp   reserved1;
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

// This specification is annoyingly combined from two sources:
// 1. most of the description comes from the ATA8-ACS specification under section 7.16 IDENTIFY DEVICE
// 2. more description is found in the Serial ATA 3.x specification under section 13.2 IDENTIFY (PACKET) DEVICE
// It seems like *2* is authoritative over *1*, i.e., some fields are renamed or reused in *2*.
struct ata_identify_reponse {
    union {
        u16 general_configuration;
        struct {
            u32 reserved00          : 2;
            u32 response_incomplete : 1;
            u32 reserved01          : 12;
            u32 ata_device          : 1;   // 0 == ATA device
        } __packed ata;
        struct {
            u32 command_packet_size : 2;  // 0 = 12-byte command packet, 1 = 16-byte command packet
            u32 response_incomplete : 1;
            u32 reserved00          : 2;
            u32 drq_speed           : 2;  // 0 = device sets DRQ to 1 within 3 ms of receiving PACKET command, 2 = within 50us
            u32 reserved01          : 1;
            u32 command_packet_set  : 5;  // indicates the command packet set used by the device
            u32 reserved02          : 1;
            u32 atapi_device        : 2;  // 2 = ATAPI device, invalid otherwise
        } __packed atapi;
    };

    u16 reserved1;
    u16 specific_configuration;   // optional
    u16 reserved2[7];

    u16 serial_number[10];        // string with byte-swapped words
    u16 reserved3[3];

    u16 firmware_revision[4];
    u16 model_number[20];

    u8 multiple_count;  // value of 0x01 to 0x10 - maximum number of sectors that shall be transfered per interrupt on READ/WRITE MULTIPLE commands
    u8 reserved4;       // should be 0x80 but at least on QEMU, it isn't

    u16 reserved5;

    union {
        u16 capabilities;
        struct {
            u32 reserved60        : 8;
            u32 dma_supported     : 1; // 1 = DMA supported
            u32 lba_supported     : 1; // 1 = LBA supported
            u32 may_disable_iordy : 1; // IORDY may be disabled
            u32 iordy_supported   : 1; // 1 = IORDY is supported, 0 = not supported
            u32 reserved61        : 1;
            u32 standby_timer     : 1; // 1 = standby timer is supported, 0 = timer values are managed by the device
            u32 reserved62        : 2;
        } __packed cap;
    };

    // word #50
    union {
        u16 capabilities2;
        struct {
            u32 standby_timer_minimum : 1; // set to 1 to indicate there's a vendor specific minimum value for the standby timer
            u32 reserved70            : 13; 
            u32 reserved71            : 1; // must be set to 1
            u32 reserved72            : 1; // must be set to 0
        } __packed cap2;
    };

    u16 reserved8[2];

    struct {
        u32 reserved90                     : 1;
        u32 fields_in_words_64_to_70_valid : 1; // 1=the fields reported in words[64:70] are valid, 0=not valid
        u32 fields_in_word_88_valid        : 1; // 1=the fields reported in word[88] are valid, 0=not valid
        u32 reserved91                     : 13;
    } __packed;

    u16 reserved10[5];

    struct {
        u32 sectors_per_drq_data_block     : 8; // the current setting for number of logical sectors that are transfered per DRQ data block
                                                // on READ/WRITE multiple commands
        u32 multiple_sector_setting_valid  : 1;
        u32 reserved11                     : 7;
    } __packed;

    // word #60
    u16 total_logical_sectors[2]; // total number of user addressable logical sectors
    u16 reserved12;

    struct {
        u32 multiword_dma_mode0_supported : 1; 
        u32 multiword_dma_mode1_supported : 1;  // mode 1 and below
        u32 multiword_dma_mode2_supported : 1;  // mode 2 and below
        u32 reserved130                   : 5;
        u32 multiword_dma_mode0_selected  : 1;
        u32 multiword_dma_mode1_selected  : 1;
        u32 multiword_dma_mode2_selected  : 1;
        u32 reserved131                   : 5;
    } __packed;

    struct {
        u32 pio_modes_supported : 8;
        u32 reserved140         : 8;
    } __packed;

    u16 min_multiword_dma_transfer_cycle_time; // per word, in nanoseconds
    u16 mfrs_recommended_multiword_dma_transfer_cycle_time; // per word, in nanoseconds
    u16 min_pio_transfer_cycle_time; // (without flow control) per word, in nanoseconds
    u16 min_pio_transfer_iordy_cycle_time; // (with IORDY flow control) per word, in nanoseconds

    struct {
        u32 reserved150                             : 3;
        u32 extended_number_of_addressable_sectors  : 1; // 1=supports extended # of user addressable logical sectors
        u32 device_encrypts_user_data               : 1; // 1=device does encrypt all data, 0=device might not encrypt
        u32 reserved151                             : 4;
        u32 download_microcode_dma_supported        : 1;
        u32 set_max_password_unlock_dma_supported   : 1; // both SET MAX PASSWORD DMA and SET MAX UNLOCK DMA are supported
        u32 write_buffer_dma_supported              : 1;
        u32 read_buffer_dma_supported               : 1;
        u32 device_conf_identify_dma_supported      : 1; // both DEVICE CONFIGURATION IDENTIFY DMA and DEVICE CONFIGURATION SET DMA are supported
        u32 long_sector_alignment_error_support     : 1; // long physical sector alignment error reporting control is supported
        u32 deterministic_read_after_trim_supported : 1;
        u32 cfast_specification_supported           : 1;
    } __packed;

    // word #70
    u16 reserved16[5];

    struct {
        u32 maximum_queue_depth : 5; // maximum queue depth minus 1
        u32 reserved170         : 11;
    } __packed;
    
    struct {
        u32 reserved180                                    : 1;
        u32 sata_gen1_speed_supported                      : 1;
        u32 sata_gen2_speed_supported                      : 1;
        u32 sata_gen3_speed_supported                      : 1;
        u32 reserved181                                    : 4;
        u32 native_command_queuing_supported               : 1;
        u32 host_power_management_requests_supported       : 1;
        u32 phy_event_counters_supported                   : 1;
        u32 unload_with_ncq_outstanding_supported          : 1;
        u32 native_command_queuing_priority_info_supported : 1;
        u32 host_automatic_partial_to_slumber_supported    : 1;
        u32 device_automatic_partial_to_slumber_supported  : 1;
        u32 read_log_dma_ext_supported                     : 1;
    } __packed;

    u16 sata_additional_features_and_capabilities[3];  // TODO? all be zeros in qemu

    // word #80
    struct {
        u32 reserved190           : 4;
        u32 ata_atapi_v4_support  : 1;
        u32 ata_atapi_v5_support  : 1;
        u32 ata_atapi_v6_support  : 1;
        u32 ata_atapi_v7_support  : 1;
        u32 ata_atapi_v8_support  : 1;
        u32 ata_atapi_v9_support  : 1;
        u32 ata_atapi_v10_support : 1;
        u32 ata_atapi_v11_support : 1;
        u32 ata_atapi_v12_support : 1;
        u32 ata_atapi_v13_support : 1;
        u32 ata_atapi_v14_support : 1;
        u32 reserved191           : 1;
    } __packed;

    u16 minor_version;

    struct {
        u32 smart_feature_supported              : 1;
        u32 security_feature_supported           : 1;
        u32 reserved200                          : 1;
        u32 mandatory_power_management_supported : 1;
        u32 packet_feature_set_supported         : 1;
        u32 volatile_write_cache_supported       : 1;
        u32 read_lookahead_supported             : 1;
        u32 release_interrupt_supported          : 1;
        u32 service_interrupt_supported          : 1;
        u32 device_reset_command_supported       : 1;
        u32 hpa_feature_set_supported            : 1;
        u32 reserved201                          : 1;
        u32 write_buffer_command_supported       : 1;
        u32 read_buffer_command_supported        : 1;
        u32 nop_command_supported                : 1;
        u32 reserved202                          : 1;
    } __packed;

    struct {
        u32 download_microcode_command_supported    : 1;
        u32 tcq_feature_set_supported               : 1;
        u32 cfa_feature_set_supported               : 1;
        u32 apm_feature_set_supported               : 1;
        u32 reserved210                             : 1;
        u32 puis_feature_set_supported              : 1;
        u32 set_features_required_for_spinup        : 1;
        u32 reserved_for_offset_area_boot_method    : 1;
        u32 set_max_security_extension_supported    : 1;
        u32 amm_feature_set_supported               : 1;
        u32 lba48_address_feature_set_supported     : 1;
        u32 dco_feature_set_supported               : 1;
        u32 mandatory_flush_cache_command_supported : 1;
        u32 flush_cache_ext_command_supported       : 1;
        u32 reserved211                             : 1; // must be 1
        u32 reserved212                             : 1; // must be 0
    } __packed;

    struct {
        u32 smart_error_reporting_supported              : 1;
        u32 smart_self_test_supported                    : 1;
        u32 media_serial_number_supported                : 1;
        u32 media_card_passthrough_feature_set_supported : 1;
        u32 streaming_feature_set_supported              : 1;
        u32 gpl_feature_set_supported                    : 1; // general purpose logging
        u32 write_dma_fua_ext_supported                  : 1;
        u32 write_dma_queued_fua_ext_supported           : 1;
        u32 world_wide_name_64bit_supported              : 1;
        u32 reserved220                                  : 4;
        u32 idle_immediate_command_supported             : 1;
        u32 reserved221                                  : 1; // must be set to 1
        u32 reserved222                                  : 1; // must be set to 0
    } __packed;

    // A number of these _enabled bits are likely just copies of the corresponding _supported bit field
    // I just don't know the ATA spec well enough (or at all, really) to understand all of their meanings
    struct {
        u32 smart_feature_enabled              : 1;
        u32 security_feature_enabled           : 1;
        u32 reserved230                        : 1;
        u32 mandatory_power_management_enabled : 1;
        u32 packet_feature_set_enabled         : 1;
        u32 volatile_write_cache_enabled       : 1;
        u32 read_lookahead_enabled             : 1;
        u32 release_interrupt_enabled          : 1;
        u32 service_interrupt_enabled          : 1;
        u32 device_reset_command_enabled       : 1;
        u32 hpa_feature_set_enabled            : 1;
        u32 reserved231                        : 1;
        u32 write_buffer_command_enabled       : 1;
        u32 read_buffer_command_enabled        : 1;
        u32 nop_command_enabled                : 1;
        u32 reserved232                        : 1;
    } __packed;

    struct {
        u32 download_microcode_dma_enabled               : 1;
        u32 tcq_feature_set_enabled                      : 1;
        u32 cfa_feature_set_enabled                      : 1;
        u32 apm_feature_set_enabled                      : 1;
        u32 reserved240                                  : 1;
        u32 puis_feature_set_enabled                     : 1;
        u32 set_features_required_for_spinup_enabled     : 1;
        u32 reserved_for_offset_area_boot_method_enabled : 1;
        u32 set_max_security_extension_enabled           : 1;
        u32 amm_feature_set_enabled                      : 1;
        u32 lba48_address_feature_set_enabled            : 1;
        u32 dco_feature_set_enabled                      : 1;
        u32 mandatory_flush_cache_command_enabled        : 1;
        u32 flush_cache_ext_command_enabled              : 1;
        u32 reserved241                                  : 1;
        u32 words_119_to_120_valid                       : 1;
    } __packed;

    struct {
        u32 smart_error_reporting_enabled              : 1;
        u32 smart_self_test_enabled                    : 1;
        u32 media_serial_number_enabled                : 1;
        u32 media_card_passthrough_feature_set_enabled : 1;
        u32 streaming_feature_set_enabled              : 1;
        u32 gpl_feature_set_enabled                    : 1; // general purpose logging
        u32 write_dma_fua_ext_enabled                  : 1;
        u32 write_dma_queued_fua_ext_enabled           : 1;
        u32 world_wide_name_64bit_enabled              : 1;
        u32 reserved250                                : 4;
        u32 idle_immediate_command_enabled             : 1;
        u32 reserved251                                : 1; // must be set to 1
        u32 reserved252                                : 1; // must be set to 0
    } __packed;

    struct {
        u32 ultra_dma_mode0_supported : 1; // modeX_supported => mode X and below are supported
        u32 ultra_dma_mode1_supported : 1;
        u32 ultra_dma_mode2_supported : 1;
        u32 ultra_dma_mode3_supported : 1;
        u32 ultra_dma_mode4_supported : 1;
        u32 ultra_dma_mode5_supported : 1;
        u32 ultra_dma_mode6_supported : 1;
        u32 reserved260               : 1;
        u32 ultra_dma_mode0_selected  : 1;
        u32 ultra_dma_mode1_selected  : 1;
        u32 ultra_dma_mode2_selected  : 1;
        u32 ultra_dma_mode3_selected  : 1;
        u32 ultra_dma_mode4_selected  : 1;
        u32 ultra_dma_mode5_selected  : 1;
        u32 ultra_dma_mode6_selected  : 1;
        u32 reserved261               : 1;
    } __packed;

    u16 normal_security_erase_unit_time; // nanoseconds?

    // word #90
    u16 enhanced_security_erase_unit_time;
    u16 current_apm_level;
    u16 master_password_identifier;

    struct {
        u32 reserved270                            : 1;  // must be 1
        u32 device0_number_determined_mode         : 2; // 1 = a jumper was used, 2 = CSEL signal, 3 = some other method/unknown
        u32 device0_passed_diagnostics             : 1;
        u32 device0_pdiag_detected                 : 1;
        u32 device0_dasp_detected                  : 1;
        u32 device0_responds_when_device1_selected : 1;
        u32 reserved271                            : 1;
        u32 reserved272                            : 1;  // must be 1
        u32 device1_number_determined_mode         : 2; // 1 = a jumper was used, 2 = CSEL signal, 3 = some other method/unknown
        u32 device1_pdiag_asserted                 : 1;
        u32 reserved273                            : 1;
        u32 device1_detected_cblid_above_vihb      : 1;
        u32 reserved274                            : 1; // set to 1
        u32 reserved275                            : 1; // set to 0
    } __packed;

    struct {
        u32 current_aam_value             : 8;
        u32 vendors_recommended_aam_value : 8;
    } __packed;

    u16 stream_minimum_request_size;
    u16 streaming_dma_transfer_time;
    u16 streaming_access_latency;
    u16 streaming_performance_granularity[2]; // 32-bit

    // word #100...sheesh will this ever end?
    u16 total_logical_sectors_lba48[4]; // total number of user addressable logical sectors for 48-bit commands (64-bit)
    u16 streaming_pio_transfer_time;
    u16 reserved28;

    struct {
        u32 log2_logical_sectors_per_physical_sector     : 4;
        u32 reserved290                                  : 8;
        u32 logical_sector_longer_than_256_words         : 1; // if the device has a logical sector size >256 words (512 bytes), this is set to 1
                                                              // and the actual sector size is listed in logical_sector_size below
        u32 multiple_logical_sectors_per_physical_sector : 1;
        u32 reserved291                                  : 1; // must be 1
        u32 reserved292                                  : 1; // must be 0;
    } __packed;

    u16 reserved30;
    u16 world_wide_name[4];
    u16 reserved31[5];

    // word #117
    u16 logical_sector_size[2]; // u32, only valid when logical_sector_longer_than_256_words is set

    struct {
        u32 reserved320                                     : 1;
        u32 read_write_verify_feature_set_supported         : 1;
        u32 write_uncorrectable_ext_command_supported       : 1;
        u32 read_write_log_dma_ext_commands_supported       : 1;
        u32 download_microcode_command_mode3_supported      : 1;
        u32 free_fall_control_feature_set_supported         : 1;
        u32 extended_status_reporting_feature_set_supported : 1;
        u32 reserved321                                     : 7;
        u32 reserved322                                     : 1; // must be set to 1
        u32 reserved323                                     : 1; // must be set to 0
    } __packed;

    // word #120
    struct {
        u32 reserved330                                    : 1;
        u32 read_write_verify_feature_set_enabled          : 1;
        u32 write_uncorrectable_ext_command_enabled        : 1;
        u32 read_write_log_dma_ext_commands_enabled        : 1;
        u32 download_microcode_command_mode3_enabled       : 1;
        u32 free_fall_control_feature_set_enabled          : 1;
        u32 extended_status_reporting_feature_set_enabled  : 1;
        u32 reserved331                                    : 7;
        u32 reserved332                                    : 1; // must be set to 1
        u32 reserved333                                    : 1; // must be set to 0
    } __packed;

    u16 reserved34[7];

    struct {
        u32 security_supported                : 1;
        u32 security_enabled                  : 1;
        u32 security_locked                   : 1;
        u32 security_frozen                   : 1;
        u32 security_count_expired            : 1;
        u32 enhanced_security_erase_supported : 1;
        u32 reserved350                       : 2;
        u32 master_password_capability        : 1; // 0 = high, 1 = maximum
        u32 reserved351                       : 7;
    } __packed;

    u16 reserved36[31];

    // word #160
    struct {
        u32 maximum_current                            : 12; // in mA
        u32 cfa_power_mode1_disabled                   : 1;
        u32 cfa_power_mode1_required_for_some_commands : 1;
        u32 reserved370                                : 1;
        u32 word_160_supported                         : 1; // this word is only valid if this bit is set to 1
    } __packed;

    u16 reserved38[7]; // for CompactFlash association

    struct {
        u32 device_form_factor : 4;
        u32 reserved390        : 12;
    } __packed;

    struct {
        u32 trim_bit_in_data_set_management_supported : 1;
        u32 reserved400                               : 15;
    } __packed;

    // word 170
    u16 additional_product_identifier[4];
    u16 reserved41[2];
    u16 current_media_serial_number[30];

    // word 206
    struct {
        u32 sct_command_transport_supported              : 1;
        u32 reserved420                                  : 1;
        u32 sct_write_same_command_supported             : 1;
        u32 sct_error_recovery_control_command_supported : 1;
        u32 sct_feature_control_command_supported        : 1;
        u32 sct_data_tables_command_supported            : 1;
        u32 reserved421                                  : 6;
        u32 reserved422                                  : 4; // vendor specific
    } __packed;

    u16 reserved43[2];

    struct {
        u32 logical_sector_offset : 14;
        u32 reserved440           : 1; // must be 1
        u32 reserved441           : 1; // must be 0
    } __packed;

    // word 210
    u16 write_read_verify_mode3_sector_count[2]; // 32-bit
    u16 write_read_verify_mode2_sector_count[2]; // 32-bit

    struct {
        u32 nv_cache_power_mode_feature_set_supported : 1;
        u32 nv_cache_power_mode_feature_set_enabled   : 1;
        u32 reserved450                               : 2;
        u32 nv_cache_feature_set_enabled              : 1;
        u32 reserved451                               : 3;
        u32 nv_cache_power_mode_feature_set_version   : 4;
        u32 nv_cache_feature_set_version              : 4;
    } __packed;

    u16 nv_cache_size[2]; // 32-bit, count in # of logical sectors

    u16 media_rotation_rate;
    u16 reserved46;

    struct {
        u32 estimated_spinup_time : 8; // in seconds
        u32 reserved47            : 8;
    } __packed;

    // word 220
    struct {
        u32 read_write_verify_current_mode : 8;
        u32 reserved48                     : 8;
    } __packed;

    u16 reserved49;

    struct {
        u32 ata8_ast          : 1;
        u32 sata_1_0a         : 1;
        u32 sata_2_extensions : 1;
        u32 sata_rev_2_5      : 1;
        u32 sata_rev_2_6      : 1;
        u32 reserved500       : 7;
        u32 transport_type    : 4; // 0=parallel, 1=serial
    } __packed;

    u16 transport_minor_version;
    u16 reserved51[10];

    // word 234
    u16 minimum_blocks_per_download_microcode_command_mode3;
    u16 maximum_blocks_per_download_microcode_command_mode3;
    u16 reserved52[19];

    struct {
        u16 checksum_validity_indicator : 8;
        u16 checksum                    : 8;
    } __packed;

} __packed;

static char const* const device_form_factor_descriptions[] = {
    "not reported", "5.25 inch", "3.5 inch", "2.5 inch", "1.8 inch", "less than 1.8 inch"
};

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
        // TODO vmem_unmap_page(virt_addr)
        palloc_abandon((void*)phys_page, 0);
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

    // free the allocated memory
    struct ahci_device_port* aport = ahci_device_ports[port_index];

    // TODO unmap the memory
    // vmem_unmap_page(virt_addr);

    // free the page
    palloc_abandon((void*)aport->command_list_phys_address, 0); // command_list_phys_address is always at the start of the allocated page

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

static inline void byte_swapped_ascii(u16* string, u16 inlen, char* output)
{
    while(inlen-- != 0) {
        *output++ = *string >> 8;
        *output++ = *string++ & 0xFF;
    }
    *output = 0;
}

static char const* get_rotation_rate_string(u16 value)
{
    static char buf[128];
    if(value == 0) return "rate not reported";
    if(value == 1) return "non-rotating (solid state)";
    sprintf(buf, "%d rpm", value);
    return (char const*)buf;
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
    intp cmd_table_phys = (intp)palloc_claim_one();
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
    command_fis->command   = aport->is_atapi ? ATA_COMMAND_IDENTIFY_PACKET_DEVICE : ATA_COMMAND_IDENTIFY;
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
    fprintf(stderr, "ahci: issuing command on port %d\n", port_index);
    hba_port->command_issue |= (1 << cmdslot);

    // Wait for completion
    wait_until_false(hba_port->command_issue & (1 << cmdslot), 10000000, tmp) {
        fprintf(stderr, "ahci: command did not activate\n");
    } else {
        fprintf(stderr, "ahci: command active!\n");
    }

    wait_until_false(hba_port->task_file_data & (HBAP_TASK_FILE_DATA_FLAG_STATUS_BUSY | HBAP_TASK_FILE_DATA_FLAG_STATUS_REQUEST), 1000000, tmp) {
        fprintf(stderr, "ahci: port %d timeout on drive busy\n", port_index);
    } else {
        fprintf(stderr, "ahci: port %d drive no longer busy\n", port_index);
    }

    if(hba_port->task_file_data & HBAP_TASK_FILE_DATA_FLAG_ERROR) {
        fprintf(stderr, "ahci: port %d ata command error\n", port_index);
        fprintf(stderr, "ahci: sata_error = 0x%lX\n", hba_port->sata_error);
    }

    // Wait for PIO Setup IRQ
    wait_until_true(hba_port->interrupt_status & HBAP_INTERRUPT_STATUS_FLAG_PIO_SETUP, 1000000, tmp) {
        fprintf(stderr, "ahci: port %d timed out waiting for PIO Setup interrupt\n", port_index);
    } else {
        fprintf(stderr, "ahci: port %d PIO Setup completed\n", port_index);
    }

    struct ata_identify_reponse* ident = (struct ata_identify_reponse*)dest_virt;
    
    char buf[512], buf2[512], buf3[512];
    fprintf(stderr, "ahci: port %d IDENTIFY response:\n", port_index);
    if(ident->ata.ata_device == 0) {
        fprintf(stderr, "    ata_device=%d (0 for ATA) ata.response_incomplete=%d specific_configuration=%d\n", ident->ata.ata_device, ident->ata.response_incomplete, ident->specific_configuration);
    }
    if(ident->atapi.atapi_device == 2) {
        fprintf(stderr, "    atapi_device=%d (2 for ATAPI) atapi.response_incomplete=%d specific_configuration=%d\n", ident->atapi.atapi_device, ident->atapi.response_incomplete, ident->specific_configuration);
    }
    byte_swapped_ascii(ident->serial_number, countof(ident->serial_number), buf);
    byte_swapped_ascii(ident->firmware_revision, countof(ident->firmware_revision), buf2);
    byte_swapped_ascii(ident->model_number, countof(ident->model_number), buf3);
    fprintf(stderr, "    serial_number    =[%s]\n    firmware_revision=[%s]\n    model_number     =[%s]\n", buf, buf2, buf3);
    byte_swapped_ascii(ident->additional_product_identifier, countof(ident->additional_product_identifier), buf);
    fprintf(stderr, "    additional_product_identifier=[%s]\n", buf); // I feel like there should be a _supported bit for this, but not sure which one it is
    byte_swapped_ascii(ident->current_media_serial_number, countof(ident->current_media_serial_number), buf);
    fprintf(stderr, "    current_media_serial_number=[%s]\n", ident->media_serial_number_supported ? buf : "not supported");
    fprintf(stderr, "    device_form_factor=%s\n", (ident->device_form_factor < countof(device_form_factor_descriptions)) ? device_form_factor_descriptions[ident->device_form_factor] : "not valid");
    fprintf(stderr, "    media_rotation_rate=%s\n", get_rotation_rate_string(ident->media_rotation_rate));
    fprintf(stderr, "    estimated_spinup_time=%d\n", ident->estimated_spinup_time);
    fprintf(stderr, "    transport_type=%d ata8_ast=%d sata_1_0a=%d sata_2_extensions=%d sata_rev_2_5=%d sata_rev_2_6=%d\n", 
            ident->transport_type, ident->ata8_ast, ident->sata_1_0a, ident->sata_2_extensions, ident->sata_rev_2_5, ident->sata_rev_2_6);
    fprintf(stderr, "    multiple_count=0x%02X reserved4=0x%02X (will be 0x80 if multiple_count is used)\n", ident->multiple_count, ident->reserved4);
    fprintf(stderr, "    minor_version=0x%04X\n", ident->minor_version);
    fprintf(stderr, "    capabilities=0x%04X\n", ident->capabilities);
    fprintf(stderr, "        cap.lba_supported=%d cap.dma_supported=%d\n", ident->cap.lba_supported, ident->cap.dma_supported);
    fprintf(stderr, "        cap.iordy_supported=%d cap.may_disable_iordy=%d\n", ident->cap.may_disable_iordy, ident->cap.may_disable_iordy);
    fprintf(stderr, "        cap.standby_timer=%d\n", ident->cap.standby_timer);
    fprintf(stderr, "    capabilities2=0x%04X\n", ident->capabilities2);
    fprintf(stderr, "        cap2.standby_timer=%d\n", ident->cap2.standby_timer_minimum);
    fprintf(stderr, "    fields_in_words_64_to_70_valid=%d fields_in_word_88_valid=%d words_119_to_120_valid=%d\n", 
            ident->fields_in_words_64_to_70_valid, ident->fields_in_word_88_valid, ident->words_119_to_120_valid);
    fprintf(stderr, "    sectors_per_drq_data_block=%d multiple_sector_setting_valid=%d\n", ident->sectors_per_drq_data_block, ident->multiple_sector_setting_valid);
    fprintf(stderr, "    total_logical_sectors=%d\n", (u32)ident->total_logical_sectors[0] | ((u32)ident->total_logical_sectors[1] << 16));
    fprintf(stderr, "    total_logical_sectors_lba48=%llu\n", 
            ident->lba48_address_feature_set_supported ?
                            ((u64)ident->total_logical_sectors_lba48[0] | ((u64)ident->total_logical_sectors_lba48[1] << 16) |
                            ((u64)ident->total_logical_sectors_lba48[2] << 32) | ((u64)ident->total_logical_sectors_lba48[3] << 48)) : 0);
    fprintf(stderr, "    logical_sector_offset=%d\n", ident->logical_sector_offset);
    fprintf(stderr, "    log2_logical_sectors_per_physical_sector=%d (2^x = %d)\n",
            ident->log2_logical_sectors_per_physical_sector, (u32)1 << ident->log2_logical_sectors_per_physical_sector);
    fprintf(stderr, "    logical_sector_longer_than_256_words=%d\n", ident->logical_sector_longer_than_256_words);
    fprintf(stderr, "    multiple_logical_sectors_per_physical_sector=%d\n", ident->multiple_logical_sectors_per_physical_sector);
    fprintf(stderr, "    logical_sector_size=%d bytes\n", 
            ident->logical_sector_longer_than_256_words ? 2*(((u32)ident->logical_sector_size[0] | ((u32)ident->logical_sector_size[1] << 16))) : 512);
    fprintf(stderr, "    multiword_dma_mode0_supported=%d multiword_dma_mode0_selected=%d\n", ident->multiword_dma_mode0_supported, ident->multiword_dma_mode0_selected);
    fprintf(stderr, "    multiword_dma_mode1_supported=%d multiword_dma_mode1_selected=%d\n", ident->multiword_dma_mode1_supported, ident->multiword_dma_mode1_selected);
    fprintf(stderr, "    multiword_dma_mode2_supported=%d multiword_dma_mode2_selected=%d\n", ident->multiword_dma_mode2_supported, ident->multiword_dma_mode2_selected);
    fprintf(stderr, "    pio_modes_supported=0x%02X\n", ident->pio_modes_supported);
    fprintf(stderr, "    min_multiword_dma_transfer_cycle_time=%d mfrs_recommended_multiword_dma_transfer_cycle_time=%d\n", 
            ident->min_multiword_dma_transfer_cycle_time, ident->mfrs_recommended_multiword_dma_transfer_cycle_time);
    fprintf(stderr, "    min_pio_transfer_cycle_time=%d min_pio_transfer_iordy_cycle_time=%d\n", ident->min_pio_transfer_cycle_time, ident->min_pio_transfer_iordy_cycle_time);
    fprintf(stderr, "    additional supported features:\n");
    fprintf(stderr, "        extended_number_of_addressable_sectors=%d\n", ident->extended_number_of_addressable_sectors);
    fprintf(stderr, "        device_encrypts_user_data=%d\n", ident->device_encrypts_user_data);
    fprintf(stderr, "        download_microcode_dma_supported=%d\n", ident->download_microcode_dma_supported);
    fprintf(stderr, "        set_max_password_unlock_dma_supported=%d\n", ident->set_max_password_unlock_dma_supported);
    fprintf(stderr, "        write_buffer_dma_supported=%d\n", ident->write_buffer_dma_supported);
    fprintf(stderr, "        read_buffer_dma_supported=%d\n", ident->read_buffer_dma_supported);
    fprintf(stderr, "        device_conf_identify_dma_supported=%d\n", ident->device_conf_identify_dma_supported);
    fprintf(stderr, "        long_sector_alignment_error_support=%d\n", ident->long_sector_alignment_error_support);
    fprintf(stderr, "        deterministic_read_after_trim_supported=%d\n", ident->deterministic_read_after_trim_supported);
    fprintf(stderr, "        cfast_specification_supported=%d\n", ident->cfast_specification_supported);
    fprintf(stderr, "    maximum_queue_depth=%d\n", ident->maximum_queue_depth);
    fprintf(stderr, "    SATA capabilities:\n");
    fprintf(stderr, "        sata_gen1_speed_supported=%d\n", ident->sata_gen1_speed_supported);
    fprintf(stderr, "        sata_gen2_speed_supported=%d\n", ident->sata_gen2_speed_supported);
    fprintf(stderr, "        sata_gen3_speed_supported=%d\n", ident->sata_gen3_speed_supported);
    fprintf(stderr, "        native_command_queuing_supported=%d\n", ident->native_command_queuing_supported);
    fprintf(stderr, "        native_command_queuing_priority_info_supported=%d\n", ident->native_command_queuing_priority_info_supported);
    fprintf(stderr, "        unload_with_ncq_outstanding_supported=%d\n", ident->unload_with_ncq_outstanding_supported);
    fprintf(stderr, "        host_power_management_requests_supported=%d\n", ident->host_power_management_requests_supported);
    fprintf(stderr, "        host_automatic_partial_to_slumber_supported=%d\n", ident->host_automatic_partial_to_slumber_supported);
    fprintf(stderr, "        device_automatic_partial_to_slumber_supported=%d\n", ident->device_automatic_partial_to_slumber_supported);
    fprintf(stderr, "        phy_event_counters_supported=%d\n", ident->phy_event_counters_supported);
    fprintf(stderr, "        read_log_dma_ext_supported=%d\n", ident->read_log_dma_ext_supported);
    fprintf(stderr, "    sata_additional_features_and_capabilities=0x%02X 0x%02X 0x%02X\n",
            ident->sata_additional_features_and_capabilities[0], ident->sata_additional_features_and_capabilities[1], ident->sata_additional_features_and_capabilities[2]);
    fprintf(stderr, "    ata_atapi_v4..v14_support=%d%d%d%d%d%d%d%d%d%d%d\n",
            ident->ata_atapi_v4_support, ident->ata_atapi_v5_support, ident->ata_atapi_v6_support, ident->ata_atapi_v7_support, ident->ata_atapi_v8_support,
            ident->ata_atapi_v9_support, ident->ata_atapi_v10_support, ident->ata_atapi_v11_support, ident->ata_atapi_v12_support, ident->ata_atapi_v13_support,
            ident->ata_atapi_v14_support);
    fprintf(stderr, "    commands and feature sets:\n");
    fprintf(stderr, "        smart_feature_supported=%d\n", ident->smart_feature_supported);
    fprintf(stderr, "        security_feature_supported=%d\n", ident->security_feature_supported);
    fprintf(stderr, "        security_feature_enabled=%d\n", ident->security_feature_enabled);
    fprintf(stderr, "        mandatory_power_management_supported=%d\n", ident->mandatory_power_management_supported);
    fprintf(stderr, "        mandatory_power_management_enabled=%d\n", ident->mandatory_power_management_enabled);
    fprintf(stderr, "        packet_feature_set_supported=%d\n", ident->packet_feature_set_supported);
    fprintf(stderr, "        packet_feature_set_enabled=%d\n", ident->packet_feature_set_enabled);
    fprintf(stderr, "        volatile_write_cache_supported=%d\n", ident->volatile_write_cache_supported);
    fprintf(stderr, "        volatile_write_cache_enabled=%d\n", ident->volatile_write_cache_enabled);
    fprintf(stderr, "        read_lookahead_supported=%d\n", ident->read_lookahead_supported);
    fprintf(stderr, "        read_lookahead_enabled=%d\n", ident->read_lookahead_enabled);
    fprintf(stderr, "        release_interrupt_supported=%d\n", ident->release_interrupt_supported);
    fprintf(stderr, "        release_interrupt_enabled=%d\n", ident->release_interrupt_enabled);
    fprintf(stderr, "        service_interrupt_supported=%d\n", ident->service_interrupt_supported);
    fprintf(stderr, "        service_interrupt_enabled=%d\n", ident->service_interrupt_enabled);
    fprintf(stderr, "        device_reset_command_supported=%d\n", ident->device_reset_command_supported);
    fprintf(stderr, "        device_reset_command_enabled=%d\n", ident->device_reset_command_enabled);
    fprintf(stderr, "        hpa_feature_set_supported=%d\n", ident->hpa_feature_set_supported);
    fprintf(stderr, "        hpa_feature_set_enabled=%d\n", ident->hpa_feature_set_enabled);
    fprintf(stderr, "        write_buffer_command_supported=%d\n", ident->write_buffer_command_supported);
    fprintf(stderr, "        write_buffer_command_enabled=%d\n", ident->write_buffer_command_enabled);
    fprintf(stderr, "        read_buffer_command_supported=%d\n", ident->read_buffer_command_supported);
    fprintf(stderr, "        read_buffer_command_enabled=%d\n", ident->read_buffer_command_enabled);
    fprintf(stderr, "        nop_command_supported=%d\n", ident->nop_command_supported);
    fprintf(stderr, "        nop_command_enabled=%d\n", ident->nop_command_enabled);
    fprintf(stderr, "        download_microcode_dma_supported=%d\n", ident->download_microcode_dma_supported);
    fprintf(stderr, "        download_microcode_dma_enabled=%d\n", ident->download_microcode_dma_enabled);
    fprintf(stderr, "        tcq_feature_set_supported=%d\n", ident->tcq_feature_set_supported);
    fprintf(stderr, "        tcq_feature_set_enabled=%d\n", ident->tcq_feature_set_enabled);
    fprintf(stderr, "        cfa_feature_set_supported=%d\n", ident->cfa_feature_set_supported);
    fprintf(stderr, "        cfa_feature_set_enabled=%d\n", ident->cfa_feature_set_enabled);
    fprintf(stderr, "        apm_feature_set_supported=%d\n", ident->apm_feature_set_supported);
    fprintf(stderr, "        apm_feature_set_enabled=%d\n", ident->apm_feature_set_enabled);
    fprintf(stderr, "        puis_feature_set_supported=%d\n", ident->puis_feature_set_supported);
    fprintf(stderr, "        puis_feature_set_enabled=%d\n", ident->puis_feature_set_enabled);
    fprintf(stderr, "        set_features_required_for_spinup=%d\n", ident->set_features_required_for_spinup);
    fprintf(stderr, "        set_features_required_for_spinup_enabled=%d\n", ident->set_features_required_for_spinup_enabled);
    fprintf(stderr, "        reserved_for_offset_area_boot_method=%d\n", ident->reserved_for_offset_area_boot_method);
    fprintf(stderr, "        reserved_for_offset_area_boot_method_enabled=%d\n", ident->reserved_for_offset_area_boot_method_enabled);
    fprintf(stderr, "        set_max_security_extension_supported=%d\n", ident->set_max_security_extension_supported);
    fprintf(stderr, "        set_max_security_extension_enabled=%d\n", ident->set_max_security_extension_enabled);
    fprintf(stderr, "        amm_feature_set_supported=%d\n", ident->amm_feature_set_supported);
    fprintf(stderr, "        amm_feature_set_enabled=%d\n", ident->amm_feature_set_enabled);
    fprintf(stderr, "        lba48_address_feature_set_supported=%d\n", ident->lba48_address_feature_set_supported);
    fprintf(stderr, "        lba48_address_feature_set_enabled=%d\n", ident->lba48_address_feature_set_enabled);
    fprintf(stderr, "        dco_feature_set_supported=%d\n", ident->dco_feature_set_supported);
    fprintf(stderr, "        dco_feature_set_enabled=%d\n", ident->dco_feature_set_enabled);
    fprintf(stderr, "        mandatory_flush_cache_command_supported=%d\n", ident->mandatory_flush_cache_command_supported);
    fprintf(stderr, "        mandatory_flush_cache_command_enabled=%d\n", ident->mandatory_flush_cache_command_enabled);
    fprintf(stderr, "        flush_cache_ext_command_supported=%d\n", ident->flush_cache_ext_command_supported);
    fprintf(stderr, "        flush_cache_ext_command_enabled=%d\n", ident->flush_cache_ext_command_enabled);
    fprintf(stderr, "        smart_error_reporting_supported=%d\n", ident->smart_error_reporting_supported);
    fprintf(stderr, "        smart_error_reporting_enabled=%d\n", ident->smart_error_reporting_enabled);
    fprintf(stderr, "        smart_self_test_supported=%d\n", ident->smart_self_test_supported);
    fprintf(stderr, "        smart_self_test_enabled=%d\n", ident->smart_self_test_enabled);
    fprintf(stderr, "        media_serial_number_supported=%d\n", ident->media_serial_number_supported);
    fprintf(stderr, "        media_serial_number_enabled=%d\n", ident->media_serial_number_enabled);
    fprintf(stderr, "        media_card_passthrough_feature_set_supported=%d\n", ident->media_card_passthrough_feature_set_supported);
    fprintf(stderr, "        media_card_passthrough_feature_set_enabled=%d\n", ident->media_card_passthrough_feature_set_enabled);
    fprintf(stderr, "        streaming_feature_set_supported=%d\n", ident->streaming_feature_set_supported);
    fprintf(stderr, "        streaming_feature_set_enabled=%d\n", ident->streaming_feature_set_enabled);
    fprintf(stderr, "        gpl_feature_set_supported=%d\n", ident->gpl_feature_set_supported);
    fprintf(stderr, "        gpl_feature_set_enabled=%d\n", ident->gpl_feature_set_enabled);
    fprintf(stderr, "        write_dma_fua_ext_supported=%d\n", ident->write_dma_fua_ext_supported);
    fprintf(stderr, "        write_dma_fua_ext_enabled=%d\n", ident->write_dma_fua_ext_enabled);
    fprintf(stderr, "        write_dma_queued_fua_ext_supported=%d\n", ident->write_dma_queued_fua_ext_supported);
    fprintf(stderr, "        write_dma_queued_fua_ext_enabled=%d\n", ident->write_dma_queued_fua_ext_enabled);
    fprintf(stderr, "        world_wide_name_64bit_supported=%d\n", ident->world_wide_name_64bit_supported);
    fprintf(stderr, "        world_wide_name_64bit_enabled=%d\n", ident->world_wide_name_64bit_enabled);
    fprintf(stderr, "        idle_immediate_command_supported=%d\n", ident->idle_immediate_command_supported);
    fprintf(stderr, "        idle_immediate_command_enabled=%d\n", ident->idle_immediate_command_enabled);
    fprintf(stderr, "        read_write_verify_feature_set_supported=%d\n", ident->read_write_verify_feature_set_supported);
    fprintf(stderr, "        read_write_verify_feature_set_enabled=%d\n", ident->read_write_verify_feature_set_enabled);
    fprintf(stderr, "        write_uncorrectable_ext_command_supported=%d\n", ident->write_uncorrectable_ext_command_supported);
    fprintf(stderr, "        write_uncorrectable_ext_command_enabled=%d\n", ident->write_uncorrectable_ext_command_enabled);
    fprintf(stderr, "        read_write_log_dma_ext_commands_supported=%d\n", ident->read_write_log_dma_ext_commands_supported);
    fprintf(stderr, "        read_write_log_dma_ext_commands_enabled=%d\n", ident->read_write_log_dma_ext_commands_enabled);
    fprintf(stderr, "        download_microcode_command_mode3_supported=%d\n", ident->download_microcode_command_mode3_supported);
    fprintf(stderr, "        download_microcode_command_mode3_enabled=%d\n", ident->download_microcode_command_mode3_enabled);
    fprintf(stderr, "        free_fall_control_feature_set_supported=%d\n", ident->free_fall_control_feature_set_supported);
    fprintf(stderr, "        free_fall_control_feature_set_enabled=%d\n", ident->free_fall_control_feature_set_enabled);
    fprintf(stderr, "        extended_status_reporting_feature_set_supported=%d\n", ident->extended_status_reporting_feature_set_supported);
    fprintf(stderr, "        extended_status_reporting_feature_set_enabled=%d\n", ident->extended_status_reporting_feature_set_enabled);
    fprintf(stderr, "        trim_bit_in_data_set_management_supported=%d\n", ident->trim_bit_in_data_set_management_supported);
    fprintf(stderr, "    ultra dma modes0..6 supported=%d%d%d%d%d%d%d\n", 
            ident->ultra_dma_mode0_supported, ident->ultra_dma_mode1_supported, ident->ultra_dma_mode2_supported, ident->ultra_dma_mode3_supported,
            ident->ultra_dma_mode4_supported, ident->ultra_dma_mode5_supported, ident->ultra_dma_mode6_supported);
    fprintf(stderr, "    ultra dma modes0..6 selected =%d%d%d%d%d%d%d\n", 
            ident->ultra_dma_mode0_selected, ident->ultra_dma_mode1_selected, ident->ultra_dma_mode2_selected, ident->ultra_dma_mode3_selected,
            ident->ultra_dma_mode4_selected, ident->ultra_dma_mode5_selected, ident->ultra_dma_mode6_selected);
    fprintf(stderr, "    normal_security_erase_unit_time=%d enhanced_security_erase_unit_time=%d\n", ident->normal_security_erase_unit_time, ident->enhanced_security_erase_unit_time);
    fprintf(stderr, "    current_apm_level=%d\n", ident->current_apm_level);
    fprintf(stderr, "    master_password_identifier=%d\n", ident->master_password_identifier);
    fprintf(stderr, "    COMRESET result:\n");
    fprintf(stderr, "        device0_number_determined_mode=%d\n", ident->device0_number_determined_mode);
    fprintf(stderr, "        device0_passed_diagnostics=%d\n", ident->device0_passed_diagnostics);
    fprintf(stderr, "        device0_pdiag_detected=%d\n", ident->device0_pdiag_detected);
    fprintf(stderr, "        device0_dasp_detected=%d\n", ident->device0_dasp_detected);
    fprintf(stderr, "        device0_responds_when_device1_selected=%d\n", ident->device0_responds_when_device1_selected);
    fprintf(stderr, "        device1_number_determined_mode=%d\n", ident->device1_number_determined_mode);
    fprintf(stderr, "        device1_pdiag_asserted=%d\n", ident->device1_pdiag_asserted);
    fprintf(stderr, "        device1_detected_cblid_above_vihb=%d\n", ident->device1_detected_cblid_above_vihb);
    fprintf(stderr, "    current_aam_value=%d vendors_recommended_aam_value=%d\n", ident->current_aam_value, ident->vendors_recommended_aam_value);
    fprintf(stderr, "    streaming:\n");
    fprintf(stderr, "        stream_minimum_request_size=%d\n", ident->stream_minimum_request_size);
    fprintf(stderr, "        streaming_dma_transfer_time=%d\n", ident->streaming_dma_transfer_time);
    fprintf(stderr, "        streaming_pio_transfer_time=%d\n", ident->streaming_pio_transfer_time);
    fprintf(stderr, "        streaming_access_latency=%d\n", ident->streaming_access_latency);
    fprintf(stderr, "        streaming_performance_granularity=%d\n", (u32)ident->streaming_performance_granularity[0] | ((u32)ident->streaming_performance_granularity[1] << 16));
    fprintf(stderr, "    security:\n");
    fprintf(stderr, "        security_supported=%d\n", ident->security_supported);
    fprintf(stderr, "        security_enabled=%d\n", ident->security_enabled);
    fprintf(stderr, "        security_locked=%d\n", ident->security_locked);
    fprintf(stderr, "        security_frozen=%d\n", ident->security_frozen);
    fprintf(stderr, "        security_count_expired=%d\n", ident->security_count_expired);
    fprintf(stderr, "        master_password_capability=%d\n", ident->master_password_capability);
    fprintf(stderr, "    CFA power mode1:\n");
    fprintf(stderr, "        word_160_supported=%d\n", ident->word_160_supported);
    fprintf(stderr, "        maximum_current=%d\n", ident->maximum_current);
    fprintf(stderr, "        cfa_power_mode1_disabled=%d\n", ident->cfa_power_mode1_disabled);
    fprintf(stderr, "        cfa_power_mode1_required_for_some_commands=%d\n", ident->cfa_power_mode1_required_for_some_commands);
    fprintf(stderr, "    SCT Command Transport:\n");
    fprintf(stderr, "        sct_command_transport_supported=%d\n", ident->sct_command_transport_supported);
    fprintf(stderr, "        sct_write_same_command_supported=%d\n", ident->sct_write_same_command_supported);
    fprintf(stderr, "        sct_error_recovery_control_command_supported=%d\n", ident->sct_error_recovery_control_command_supported);
    fprintf(stderr, "        sct_feature_control_command_supported=%d\n", ident->sct_feature_control_command_supported);
    fprintf(stderr, "        sct_data_tables_command_supported=%d\n", ident->sct_data_tables_command_supported);
    fprintf(stderr, "    NV Cache:\n");
    fprintf(stderr, "        nv_cache_power_mode_feature_set_supported=%d\n", ident->nv_cache_power_mode_feature_set_supported);
    fprintf(stderr, "        nv_cache_power_mode_feature_set_enabled=%d\n", ident->nv_cache_power_mode_feature_set_enabled);
    fprintf(stderr, "        nv_cache_power_mode_feature_set_version=%d\n", ident->nv_cache_power_mode_feature_set_version);
    fprintf(stderr, "        nv_cache_feature_set_enabled=%d\n", ident->nv_cache_feature_set_enabled);
    fprintf(stderr, "        nv_cache_feature_set_version=%d\n", ident->nv_cache_feature_set_version);
    fprintf(stderr, "        nv_cache_size=%d\n", (u32)ident->nv_cache_size[0] | ((u32)ident->nv_cache_size[1] << 16));
    fprintf(stderr, "    write_read_verify_mode3_sector_count=%d\n", (u32)ident->write_read_verify_mode3_sector_count[0] | ((u32)ident->write_read_verify_mode3_sector_count[1] << 16));
    fprintf(stderr, "    write_read_verify_mode2_sector_count=%d\n", (u32)ident->write_read_verify_mode2_sector_count[0] | ((u32)ident->write_read_verify_mode2_sector_count[1] << 16));
    fprintf(stderr, "    read_write_verify_current_mode=%d\n", ident->read_write_verify_current_mode);
    fprintf(stderr, "    checksum_validity_indicator=0x%02X checksum=0x%02X\n", ident->checksum_validity_indicator, ident->checksum);

    //// Dump 512 bytes at dest_virt
    //for(u32 i = 0; i < 512; i++) {
    //    for(u32 j = 0; j < 16; j++, i++) {
    //        fprintf(stderr, "%02X ", ((u8*)dest_virt)[i]);
    //    }
    //    fprintf(stderr, "\n");
    //}
}

