#include "common.h"

#include "acpi.h"
#include "apic.h"
#include "cpu.h"
#include "hpet.h"
#include "kernel.h"
#include "multiboot2.h"
#include "stdio.h"
#include "string.h"

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

struct acpi_sdt_header {
    u8  signature[4];
    u32 length;
    u8  revision;
    u8  checksum;
    u8  oem_id[6];
    u8  oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
} __packed;

struct acpi_xsdt {
    struct acpi_sdt_header header;
    u64    tables[];
} __packed;

#define ACPI_APIC_FLAG_HAS_PIC (1 << 0)

enum {
    ACPI_APIC_RECORD_PROCESSOR_LOCAL_APIC = 0,
    ACPI_APIC_RECORD_TYPE_IOAPIC = 1,
    ACPI_APIC_RECORD_TYPE_IOAPIC_INTERRUPT_SOURCE_OVERRIDE = 2,
    ACPI_APIC_RECORD_TYPE_IOAPIC_NMI_SOURCE = 3,
    ACPI_APIC_RECORD_TYPE_LOCAL_APIC_NMIS = 4,
    ACPI_APIC_RECORD_TYPE_LOCAL_APIC_ADDRESS_OVERRIDE = 5,
    ACPI_APIC_RECORD_TYPE_LOCAL_X2APIC = 9
};

struct acpi_apic {
    struct acpi_sdt_header header;
    u32    lapic_base;
    u32    flags;
    u8     records[];
} __packed;

struct acpi_apic_record_header {
    u8     type;
    u8     length;
} __packed;

struct acpi_apic_record_processor_local_apic {
    struct acpi_apic_record_header header;
    u8     acpi_processor_id;
    u8     acpi_id;
    u32    flags;
} __packed;

struct acpi_apic_record_ioapic {
    struct acpi_apic_record_header header;
    u8     ioapic_id;
    u8     reserved;
    u32    ioapic_address;
    u32    global_system_interrupt_base;
} __packed;

struct acpi_apic_record_interrupt_source_override {
    struct acpi_apic_record_header header;
    u8     bus_source;
    u8     irq_source;
    u32    global_system_interrupt;
    u16    flags;
} __packed;

struct acpi_apic_record_local_apic_nmis {
    struct acpi_apic_record_header header;
    u8     acpi_processor_id;
    u16    flags;
    u8     lint_number;
} __packed;

struct acpi_hpet_address {
    u8     address_space_id;
    u8     register_bit_width;
    u8     register_bit_offset;
    u8     reserved;
    u64    address;
} __packed;

struct acpi_hpet {
    struct acpi_sdt_header   header;
    u8     hardware_revision_id;

    u8     comparator_count   : 5;
    u8     counter_size       : 1;
    u8     reserved           : 1;
    u8     legacy_replacement : 1;
    
    u16    pci_vendor_id;
    struct acpi_hpet_address address;
    u8     hpet_number;
    u16    minimum_tick;

    u8     page_protection    : 4;
    u8     oem_attributes     : 4;
} __packed;

static void _parse_apic_table(struct acpi_apic*);
static void _parse_hpet_table(struct acpi_hpet*);

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
    fprintf(stderr, "apci: oem_id = [%s], revision %d, rsdt = 0x%lX, xsdt = 0x%lX, xsdt length = %d\n", buf, desc->descv1.revision, desc->descv1.rsdt_address, desc->xsdt_address, desc->length);


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
    }
    if(num_hpets > 0) hpet_notify_timer_count(num_hpets);

    // now loop over and parse the tables
    for(u32 table_index = 0; table_index < ntables; table_index++) {
        //fprintf(stderr, "table %d at 0x%lX\n", table_index, xsdt->tables[table_index]);
        struct acpi_sdt_header* hdr = (struct acpi_sdt_header*)xsdt->tables[table_index];

        buf[4] = 0;
        memcpy(buf, hdr->signature, 4);
        //fprintf(stderr, "acpi: table %d signature [%s], address = 0x%lX\n", table_index, buf, (intp)hdr);

        // validate the structure
        _validate_checksum((intp)hdr, hdr->length, "table checksum not valid");

        switch(*(u32*)&hdr->signature[0]) {
        case 0x43495041:  // 'APIC'
            _parse_apic_table((struct acpi_apic*)hdr);
            break;

        case 0x54455048: // 'HPET'
            _parse_hpet_table((struct acpi_hpet*)hdr);
            break;

        default:
            fprintf(stderr, "acpi: unhandled table [%s], address = 0x%lX\n", buf, (intp)hdr);
        }
    }
}

static void _parse_apic_table(struct acpi_apic* apic)
{
    struct acpi_apic_record_processor_local_apic*      local_apic;
    struct acpi_apic_record_ioapic*                    ioapic;
    struct acpi_apic_record_local_apic_nmis*           local_apic_nmis;
    struct acpi_apic_record_interrupt_source_override* interrupt_source_override;

    fprintf(stderr, "acpi: LAPIC at 0x%lX", apic->lapic_base);
    if(apic->flags & ACPI_APIC_FLAG_HAS_PIC) {
        fprintf(stderr, " (with dual 8259 PICs)");
    }
    fprintf(stderr, "\n");

    // configure the local 
    apic_notify_acpi_local_apic(apic->lapic_base, apic->flags & ACPI_APIC_FLAG_HAS_PIC);

    // loop over all the records in the MADT table
    // TODO loop twice to count the # of cpus and ioapics first, to allocate dynamic storage in apic.c
    u8* current_record = apic->records;
    u8* records_end = (u8*)((intp)apic + apic->header.length);
    while(current_record < records_end) {
        u8 type = current_record[0];

        switch(type) {
        case ACPI_APIC_RECORD_PROCESSOR_LOCAL_APIC:
            local_apic = (struct acpi_apic_record_processor_local_apic*)current_record;
            //fprintf(stderr, "acpi: Local APIC acpi_processor_id=%d acpi_id=%d flags=%08X\n", local_apic->acpi_processor_id, local_apic->acpi_id, local_apic->flags);
            if((local_apic->flags & 0x03) != 0) {
                apic_register_processor_lapic(local_apic->acpi_processor_id, local_apic->acpi_id, (local_apic->flags & 0x01) != 0);
            }
            break;

        case ACPI_APIC_RECORD_TYPE_IOAPIC:
            ioapic = (struct acpi_apic_record_ioapic*)current_record;
            //fprintf(stderr, "acpi: I/O APIC ioapic_id=%d ioapic_address=0x%lX global_system_interrupt_base=0x%lX\n",
            //        ioapic->ioapic_id, ioapic->ioapic_address, ioapic->global_system_interrupt_base);
            apic_notify_acpi_io_apic(ioapic->ioapic_id, ioapic->ioapic_address, ioapic->global_system_interrupt_base);
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
    fprintf(stderr, "acpi: HPET timer hardware_revision_id=%d\n", hpet->hardware_revision_id);
    fprintf(stderr, "    comparator_count=%d counter_size=%d legacy_replacement=%d\n", hpet->comparator_count, hpet->counter_size, hpet->legacy_replacement);
    fprintf(stderr, "    pci_vendor_id=0x%04X hpet_number=%d minimum_tick=%d page_protection=%d\n", hpet->pci_vendor_id, hpet->hpet_number, hpet->minimum_tick, hpet->page_protection);
    fprintf(stderr, "    address_space_id=%d register_bit_width=%d register_bit_offset=%d address=0x%lX\n", 
            hpet->address.address_space_id, hpet->address.register_bit_width, hpet->address.register_bit_offset, hpet->address.address);

    u8 flags = 0;
    if(hpet->address.address_space_id == 1) flags |= HPET_FLAG_ADDRESS_IO;

    hpet_notify_presence(hpet->hpet_number, hpet->hardware_revision_id, hpet->comparator_count, hpet->minimum_tick, (intp)hpet->address.address, 
                         hpet->address.register_bit_width, hpet->address.register_bit_offset, flags);
}

