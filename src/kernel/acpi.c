#include "lai/core.h" // include before common.h
#include "lai/helpers/sci.h"

#include "common.h"

#include "acpi.h"
#include "apic.h"
#include "cpu.h"
#include "hpet.h"
#include "kernel.h"
#include "multiboot2.h"
#include "pci.h"
#include "stdio.h"
#include "string.h"

#define SIG_TO_INT(str) ((u32)(str)[0] | ((u32)(str)[1] << 8) | ((u32)(str)[2] << 16) | ((u32)(str)[3] << 24))
#define CHECK_SIG(var,str) (*(u32 *)(var) == SIG_TO_INT(str))

struct rsdp_descriptor {
    char signature[8];
    u8   checksum;
    char oem_id[6];
    u8   revision;
    u32  rsdt_address;
} __packed;

struct rsdp_descriptorv2 {
    struct rsdp_descriptor descv1;

    u32 length;
    u64 xsdt_address;
    u8  checksum;
    u8  reserved[3];
} __packed;

static void _parse_apic_table(struct acpi_apic*);
static void _parse_hpet_table(struct acpi_hpet*);
static void _parse_mcfg_table(struct acpi_mcfg*);
static void _parse_fadt_table(struct acpi_fadt*);

static void _validate_checksum(intp base, u64 size, char* msg)
{
    u32 sum = 0;
    for(u64 i = 0; i < size; i++) {
        sum += *(u8*)(base + i);
    }

    if((sum & 0xFF) != 0) {
        fprintf(stderr, "acpi: checksum not valid: %s\n", msg);
        assert(false, "invalid checksum");
    }
}

void acpi_init()
{
    intp rsdp_base = multiboot2_acpi_get_rsdp();
    if(memcmp((void*)rsdp_base, "RSD PTR ", 8) != 0) {
        fprintf(stderr, "ACPI RSDP descriptor not valid\n");
        assert(false, "invalid rsdp descriptor pointer");
    }

    // validate the main descriptor
    _validate_checksum(rsdp_base, sizeof(struct rsdp_descriptor), "RSDP v1 checksum not valid");
    _validate_checksum(rsdp_base + sizeof(struct rsdp_descriptor), sizeof(struct rsdp_descriptorv2) - sizeof(struct rsdp_descriptor), "RSDP v2 checksum not valid");

    struct rsdp_descriptorv2* desc = (struct rsdp_descriptorv2*)rsdp_base;
    assert(desc->descv1.revision >= 2, "require V2 ACPI");

    char buf[7];
    memcpy(buf, desc->descv1.oem_id, 6);
    buf[6] = 0;
    //fprintf(stderr, "apci: oem_id = [%s], revision %d, rsdt = 0x%lX, xsdt = 0x%lX, xsdt length = %d\n", buf, desc->descv1.revision, desc->descv1.rsdt_address, desc->xsdt_address, desc->length);

    // validate the XSDT before parsing it
    struct acpi_xsdt* xsdt = (struct acpi_xsdt*)desc->xsdt_address;
    _validate_checksum((intp)xsdt, xsdt->header.length, "XSDT checksum not valid");

    u32 ntables = (xsdt->header.length - sizeof(struct acpi_sdt_header)) / sizeof(u64);

    // counter the number of HPET tables first. doesn't matter much if they're invalid,
    // that'll be checked in the next loop
    u8 num_hpets = 0;
    for(u32 table_index = 0; table_index < ntables; table_index++) {
        struct acpi_sdt_header* hdr = (struct acpi_sdt_header*)xsdt->tables[table_index];
        if(*(u32*)&hdr->signature[0] == 0x54455048) num_hpets++; // 'HPET' table
        //fprintf(stderr, "acpi: table %c%c%c%c at 0x%lX\n", hdr->signature[0], hdr->signature[1], hdr->signature[2], hdr->signature[3], (intp)hdr);
    }
    if(num_hpets > 0) hpet_notify_timer_count(num_hpets);

    // now loop over and parse the tables
    for(u32 table_index = 0; table_index < ntables; table_index++) {
        //fprintf(stderr, "table %d at 0x%lX\n", table_index, xsdt->tables[table_index]);
        struct acpi_sdt_header* hdr = (struct acpi_sdt_header*)xsdt->tables[table_index];

        buf[4] = 0;
        memcpy(buf, hdr->signature, 4);
        fprintf(stderr, "acpi: table %d signature [%s][0x%X], address = 0x%lX\n", table_index, buf, SIG_TO_INT(buf), (intp)hdr);

        // validate the structure
        _validate_checksum((intp)hdr, hdr->length, "table checksum not valid");

        u32* table_sig = (u32*)&hdr->signature[0];
        if(CHECK_SIG(table_sig, "APIC")) {
            _parse_apic_table((struct acpi_apic*)hdr);
        } else if(CHECK_SIG(table_sig, "HPET")) {
            _parse_hpet_table((struct acpi_hpet*)hdr);
        } else if(CHECK_SIG(table_sig, "MCFG")) { 
            _parse_mcfg_table((struct acpi_mcfg*)hdr);
        } else if(CHECK_SIG(table_sig, "FACP")) {
            _parse_fadt_table((struct acpi_fadt*)hdr);
        } else {
            fprintf(stderr, "acpi: unhandled table [%s], address = 0x%lX\n", buf, (intp)hdr);
        }
    }

}

void _enumerate_namespace(lai_state_t* state, lai_nsnode_t* node)
{
    //char newpath[512];
    //sprintf(newpath, "%s\%s", path, lai_stringify_node_path(node));
    fprintf(stderr, "node: %s (type=%d)\n", lai_stringify_node_path(node), lai_ns_get_node_type(node));

    if(lai_ns_get_node_type(node) == 2 && strstr(lai_stringify_node_path(node), "_HID") != null) {
        lai_variable_t id = LAI_VAR_INITIALIZER;
        //lai_nsnode_t* handle = lai_resolve_path(node, "_HID");
        if (node) {
            lai_api_error_t err;
            if ((err = lai_eval(&id, node, state)) == LAI_ERROR_NONE) {
                fprintf(stderr, "_HID type = %d\n", lai_obj_get_type(&id));
                if(lai_obj_get_type(&id) == LAI_TYPE_INTEGER) {
                    u64 _hid = 0;
                    lai_obj_get_integer(&id, &_hid);
                    fprintf(stderr, "_hid = 0x%lX\n", _hid);
                } else if(lai_obj_get_type(&id) == LAI_TYPE_STRING) {
                    fprintf(stderr, "_hid = %s\n", lai_exec_string_access(&id));
                }
            } else {
                fprintf(stderr, "Could not evaluate _HID of device\n");
            }
        }
        lai_var_finalize(&id);
    }

    struct lai_ns_child_iterator child_iter;
    lai_initialize_ns_child_iterator(&child_iter, node);
    lai_nsnode_t* child;

    while((child = lai_ns_child_iterate(&child_iter)) != null) {
        _enumerate_namespace(state, child);
    }
}

void acpi_reset()
{
    struct acpi_fadt* fadt = acpi_find_table("FACP", 0);
    if(fadt == null) return;

    fprintf(stderr, "fadt->reset_reg.address = 0x%lX\n", fadt->reset_reg.address);
    *(u64*)fadt->reset_reg.address = 1;
}

// LAI requires memory allocation, so this function is called after the kernel has initialized paging and kalloc
void acpi_init_lai()
{
    struct rsdp_descriptorv2* desc = (struct rsdp_descriptorv2*)multiboot2_acpi_get_rsdp();

    //lai_enable_tracing(LAI_TRACE_NS | LAI_TRACE_IO | LAI_TRACE_OP);
    lai_set_acpi_revision(desc->descv1.revision);
    lai_create_namespace();
    if(lai_enable_acpi(1) != 0) {
        fprintf(stderr, "acpi: error trying to enable ACPI\n");
        assert(false, "couldn't enable ACPI, debug me");
    } else {
        fprintf(stderr, "acpi: ACPI enabled\n");
    }

#if 0
    lai_state_t state;
    lai_init_state(&state);

    struct lai_ns_iterator iter;
    lai_initialize_ns_iterator(&iter);
    lai_nsnode_t* node;
    while((node = lai_ns_iterate(&iter)) != null) {
        //determine depth
        if(lai_ns_get_parent(node) == lai_ns_get_root()) {
            _enumerate_namespace(&state, node);
        }
    }

    lai_finalize_state(&state);
#endif
}

// return a virtual memory address pointer to the ACPI table matching signature 'sig'
// If there are multiple tables with that signature, return the 'index'th table.
void* acpi_find_table(char const* sig, u8 index)
{
    // Check for DSDT, as it's not a standard table but still gets searched for using acpi_find_table.
    if(CHECK_SIG(sig, "DSDT")) {
        struct acpi_fadt* fadt = acpi_find_table("FACP", 0);
        if(fadt == null) return null;
        if(fadt->dsdt != 0) return (void*)(u64)fadt->dsdt;
        return (void*)fadt->x_dsdt;
    }

    // get the RSDP pointer
    struct rsdp_descriptorv2* desc = (struct rsdp_descriptorv2*)multiboot2_acpi_get_rsdp();

    // get the XSDT table
    struct acpi_xsdt* xsdt = (struct acpi_xsdt*)desc->xsdt_address;

    // compute # of tables in the XSDT
    u32 ntables = (xsdt->header.length - sizeof(struct acpi_sdt_header)) / sizeof(u64);

    // loop over tables looking for wanted index
    for(u32 table_index = 0; table_index < ntables; table_index++) {
        struct acpi_sdt_header* hdr = (struct acpi_sdt_header*)xsdt->tables[table_index];
        if(*(u32*)&hdr->signature[0] == *(u32*)&sig[0]) {
            if(index-- == 0) return (void*)hdr;
        }
    }

    return null;
}

static void _parse_apic_table(struct acpi_apic* apic)
{
    struct acpi_apic_record_processor_local_apic*      local_apic;
    struct acpi_apic_record_ioapic*                    ioapic;
    struct acpi_apic_record_local_apic_nmis*           local_apic_nmis;
    struct acpi_apic_record_interrupt_source_override* interrupt_source_override;

    // configure the local apic base
    apic_notify_acpi_local_apic_base(apic->lapic_base, apic->flags & ACPI_APIC_FLAG_HAS_PIC);

    // pre-loop over the records to count the # of apics
    u32 num_lapics = 0;
    u32 num_ioapics = 0;
    u8* current_record = apic->records;
    u8* records_end = (u8*)((intp)apic + apic->header.length);
    while(current_record < records_end) {
        u8 type = current_record[0];
        switch(type) {
        case ACPI_APIC_RECORD_PROCESSOR_LOCAL_APIC:
            num_lapics += 1;
            break;

        case ACPI_APIC_RECORD_TYPE_IOAPIC:
            num_ioapics += 1;
            break;

        case ACPI_APIC_RECORD_TYPE_IOAPIC_INTERRUPT_SOURCE_OVERRIDE:
        case ACPI_APIC_RECORD_TYPE_LOCAL_APIC_NMIS:
        default:
            break;
        }

        // next record
        current_record = (u8*)((intp)current_record + current_record[1]);
    }

    if(num_ioapics > 1) {
        fprintf(stderr, "acpi: warning: more than one I/O apic not supported right now, ignoring...\n");
        num_ioapics = 1;
    }

    apic_notify_num_local_apics(num_lapics);

    // loop over all the records again and notify modules
    current_record = apic->records;
    records_end = (u8*)((intp)apic + apic->header.length);
    while(current_record < records_end) {
        u8 type = current_record[0];

        switch(type) {
        case ACPI_APIC_RECORD_PROCESSOR_LOCAL_APIC:
            local_apic = (struct acpi_apic_record_processor_local_apic*)current_record;
            //fprintf(stderr, "acpi: Local APIC acpi_processor_id=%d apic_id=%d flags=%08X\n", local_apic->acpi_processor_id, local_apic->apic_id, local_apic->flags);
            if((local_apic->flags & 0x03) != 0) {
                bool enabled = (local_apic->flags & 0x01) != 0;
                apic_register_processor_lapic(local_apic->acpi_processor_id, local_apic->apic_id, enabled);
            }
            break;

        case ACPI_APIC_RECORD_TYPE_IOAPIC:
            ioapic = (struct acpi_apic_record_ioapic*)current_record;
            //fprintf(stderr, "acpi: I/O APIC ioapic_id=%d ioapic_address=0x%lX global_system_interrupt_base=0x%lX\n",
            //        ioapic->ioapic_id, ioapic->ioapic_address, ioapic->global_system_interrupt_base);
            if(num_ioapics-- == 1) {
                apic_notify_acpi_io_apic(ioapic->ioapic_id, ioapic->ioapic_address, ioapic->global_system_interrupt_base);
            }
            break;

        case ACPI_APIC_RECORD_TYPE_IOAPIC_INTERRUPT_SOURCE_OVERRIDE:
            interrupt_source_override = (struct acpi_apic_record_interrupt_source_override*)current_record;
            //fprintf(stderr, "acpi: Interrupt Source Override bus source=%d irq source=%d global_system_interrupt=%d flags=0x%04X\n",
            //        interrupt_source_override->bus_source, interrupt_source_override->irq_source,
            //        interrupt_source_override->global_system_interrupt, interrupt_source_override->flags);
            apic_notify_acpi_io_apic_interrupt_source_override(interrupt_source_override->bus_source,
                    interrupt_source_override->irq_source, interrupt_source_override->global_system_interrupt,
                    interrupt_source_override->flags);
            break;

        case ACPI_APIC_RECORD_TYPE_LOCAL_APIC_NMIS:
            local_apic_nmis = (struct acpi_apic_record_local_apic_nmis*)current_record;
            //fprintf(stderr, "acpi: Local APIC NMIs acpi_processor_id=%d flags=0x%04X lint_number=%d\n", local_apic_nmis->acpi_processor_id, local_apic_nmis->flags, local_apic_nmis->lint_number);
            apic_notify_acpi_lapic_nmis(local_apic_nmis->acpi_processor_id, local_apic_nmis->lint_number, local_apic_nmis->flags);
            break;

        default:
            fprintf(stderr, "acpi: unhandled APIC record type %d\n", type);
            assert(false, "handle me");
            break;
        }

        // next record
        current_record = (u8*)((intp)current_record + current_record[1]);
    }
}

static void _parse_hpet_table(struct acpi_hpet* hpet)
{
    //fprintf(stderr, "acpi: HPET timer hardware_revision_id=%d\n", hpet->hardware_revision_id);
    //fprintf(stderr, "    comparator_count=%d counter_size=%d legacy_replacement=%d\n", hpet->comparator_count, hpet->counter_size, hpet->legacy_replacement);
    //fprintf(stderr, "    pci_vendor_id=0x%04X hpet_number=%d minimum_tick=%d page_protection=%d\n", hpet->pci_vendor_id, hpet->hpet_number, hpet->minimum_tick, hpet->page_protection);
    //fprintf(stderr, "    address_space_id=%d register_bit_width=%d register_bit_offset=%d address=0x%lX\n", 
    //        hpet->address.address_space_id, hpet->address.register_bit_width, hpet->address.register_bit_offset, hpet->address.address);

    u8 flags = 0;
    if(hpet->address.address_space_id == 1) flags |= HPET_FLAG_ADDRESS_IO;

    hpet_notify_presence(hpet->hpet_number, hpet->hardware_revision_id, hpet->comparator_count, hpet->minimum_tick, (intp)hpet->address.address, 
                         hpet->address.register_bit_width, hpet->address.register_bit_offset, flags);
}

static void _parse_mcfg_table(struct acpi_mcfg* mcfg)
{
    u32 nspaces = (mcfg->header.length - sizeof(struct acpi_mcfg)) / sizeof(struct acpi_mcfg_configuration_space);
    
    for(u32 i = 0; i < nspaces; i++) {
        struct acpi_mcfg_configuration_space* cs = &mcfg->spaces[i];
    
        fprintf(stderr, "acpi: PCI extended configuration space base=0x%lX pci_segment_group=%d start_bus=%d end_bus=%d\n", 
                cs->base_address, cs->pci_segment_group, cs->start_bus, cs->end_bus);

        pci_notify_segment_group(cs->pci_segment_group, cs->base_address, cs->start_bus, cs->end_bus);
    }
}

static void _parse_fadt_table(struct acpi_fadt* fadt)
{
    fprintf(stderr, "acpi: century register = 0x%02X\n", fadt->century);
    //rtc_notify_century_register(fadt->century);
}
