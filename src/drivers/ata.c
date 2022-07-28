#include "common.h"

#include "ata.h"

#include "stdio.h"
#include "string.h"

static char const* const device_form_factor_descriptions[] = {
    "not reported", "5.25 inch", "3.5 inch", "2.5 inch", "1.8 inch", "less than 1.8 inch"
};

static inline void _ata_string(u16* string, u16 inlen, char* output)
{
    while(inlen-- != 0) {
        *output++ = *string >> 8;
        *output++ = *string++ & 0xFF;
    }
    *output = 0;
}

static char const* _get_rotation_rate_string(u16 value)
{
    static char buf[128];
    if(value == 0) return "rate not reported";
    if(value == 1) return "non-rotating (solid state)";
    sprintf(buf, "%d rpm", value);
    return (char const*)buf;
}

void ata_dump_identify_device_response(u8 port_index, struct ata_identify_device_response* ident)
{
    char buf[512], buf2[512], buf3[512];
    fprintf(stderr, "ahci: port %d IDENTIFY response:\n", port_index);
    if(ident->ata.ata_device == 0) {
        fprintf(stderr, "    ata_device=%d (0 for ATA) ata.response_incomplete=%d specific_configuration=%d\n", ident->ata.ata_device, ident->ata.response_incomplete, ident->specific_configuration);
    }
    if(ident->atapi.atapi_device == 2) {
        fprintf(stderr, "    atapi_device=%d (2 for ATAPI) atapi.response_incomplete=%d specific_configuration=%d\n", ident->atapi.atapi_device, ident->atapi.response_incomplete, ident->specific_configuration);
    }
    _ata_string(ident->serial_number, countof(ident->serial_number), buf);
    _ata_string(ident->firmware_revision, countof(ident->firmware_revision), buf2);
    _ata_string(ident->model_number, countof(ident->model_number), buf3);
    fprintf(stderr, "    serial_number    =[%s]\n    firmware_revision=[%s]\n    model_number     =[%s]\n", buf, buf2, buf3);
    _ata_string(ident->additional_product_identifier, countof(ident->additional_product_identifier), buf);
    fprintf(stderr, "    additional_product_identifier=[%s]\n", buf); // I feel like there should be a _supported bit for this, but not sure which one it is
    _ata_string(ident->current_media_serial_number, countof(ident->current_media_serial_number), buf);
    fprintf(stderr, "    current_media_serial_number=[%s]\n", ident->media_serial_number_supported ? buf : "not supported");
    fprintf(stderr, "    device_form_factor=%s\n", (ident->device_form_factor < countof(device_form_factor_descriptions)) ? device_form_factor_descriptions[ident->device_form_factor] : "not valid");
    fprintf(stderr, "    media_rotation_rate=%s\n", _get_rotation_rate_string(ident->media_rotation_rate));
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

