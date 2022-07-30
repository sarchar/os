#ifndef __ATA_H__
#define __ATA_H__

enum ATA_COMMANDS {
    ATA_COMMAND_READ_DMA_EXT           = 0x25,
    ATA_COMMAND_WRITE_DMA_EXT          = 0x35,
    ATA_COMMAND_IDENTIFY_PACKET_DEVICE = 0xA1,
    ATA_COMMAND_IDENTIFY_DEVICE        = 0xEC
};

// This specification is annoyingly combined from two sources:
// 1. most of the description comes from the ATA8-ACS specification under section 7.16 IDENTIFY DEVICE
// 2. more description is found in the Serial ATA 3.x specification under section 13.2 IDENTIFY (PACKET) DEVICE
// It seems like *2* is authoritative over *1*, i.e., some fields are renamed or reused in *2*.
struct ata_identify_device_response {
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

    // word 10
    u16 serial_number[10];        // string with byte-swapped words

    // word 20
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
    u16 total_logical_sectors[2]; // total number of user addressable logical sectors (not available for ATAPI)
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

void ata_dump_identify_device_response(u8, struct ata_identify_device_response*);

#endif
