#include "common.h"

#include "acpi.h"
#include "cpu.h"
#include "kernel.h"
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
    ACPI_APIC_RECORD_TYPE_LOCAL_APIC = 0,
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

struct acpi_apic_record_local_apic {
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

static intp rsdp_base;

static void _parse_apic_table(struct acpi_apic*);

void acpi_set_rsdp_base(intp base)
{
    rsdp_base = base;
    if(memcmp((void*)rsdp_base, "RSD PTR ", 8) != 0) {
        fprintf(stderr, "ACPI RSDP descriptor not valid\n");
        assert(false, "invalid rsdp descriptor pointer");
    }
}

void acpi_init()
{
    u32 sum = 0;
    for(u32 i = 0; i < sizeof(struct rsdp_descriptor); i++) {
        sum += *(u8*)(rsdp_base + i);
    }

    if((sum & 0xFF) != 0) {
        fprintf(stderr, "ACPI RSDP v1 checksum not valid\n");
        assert(false, "invalid rsdp v1 checksum not valid");
    }

    sum = 0;
    for(u32 i = 0; i < sizeof(struct rsdp_descriptorv2) - sizeof(struct rsdp_descriptor); i++) {
        sum += *(u8*)(rsdp_base + sizeof(struct rsdp_descriptor) + i);
    }

    if((sum & 0xFF) != 0) {
        fprintf(stderr, "ACPI RSDP v2 checksum not valid\n");
        assert(false, "invalid rsdp v2 checksum not valid");
    }

    struct rsdp_descriptorv2* desc = (struct rsdp_descriptorv2*)rsdp_base;
    assert(desc->descv1.revision >= 2, "require V2 ACPI");

    char buf[7];
    memcpy(buf, desc->descv1.oem_id, 6);
    buf[6] = 0;
    fprintf(stderr, "apci: oem_id = [%s], revision %d, rsdt = 0x%lX, xsdt = 0x%lX, xsdt length = %d\n", buf, desc->descv1.revision, desc->descv1.rsdt_address, desc->xsdt_address, desc->length);

    // validate the XSDT before parsing it
    struct acpi_xsdt* xsdt = (struct acpi_xsdt*)desc->xsdt_address;
    sum = 0;
    for(u32 i = 0; i < xsdt->header.length; i++) {
        sum += *(u8*)((intp)xsdt + i);
    }

    if((sum & 0xFF) != 0) {
        fprintf(stderr, "acpi: XSDT checksum not valid\n");
        assert(false, "xsdt checksum not valid");
    }

    fprintf(stderr, "xsdt->header.length = %d, sizeof(struct acpi_sdt_header) = %d\n", xsdt->header.length, sizeof(struct acpi_sdt_header));
    u32 ntables = (xsdt->header.length - sizeof(struct acpi_sdt_header)) / sizeof(u64);
    fprintf(stderr, "got %d tables\n", ntables);
    for(u32 table_index = 0; table_index < ntables; table_index++) {
        //fprintf(stderr, "table %d at 0x%lX\n", table_index, xsdt->tables[table_index]);
        struct acpi_sdt_header* hdr = (struct acpi_sdt_header*)xsdt->tables[table_index];

        buf[4] = 0;
        memcpy(buf, hdr->signature, 4);
        fprintf(stderr, "acpi: table %d signature [%s], address = 0x%lX\n", table_index, buf, (intp)hdr);

        // validate the structure
        sum = 0;
        for(u32 i = 0; i < hdr->length; i++) {
            sum += *(u8*)((intp)hdr + i);
        }

        if((sum & 0xFF) != 0) {
            fprintf(stderr, "acpi: [%s] table checksum not valid\n", buf);
            assert(false, "table checksum not valid");
        }

        switch(*(u32*)&hdr->signature[0]) {
        case 0x43495041:  // 'APIC'
            _parse_apic_table((struct acpi_apic*)hdr);
            break;
        }
    }
}

static void _parse_apic_table(struct acpi_apic* apic)
{
    struct acpi_apic_record_local_apic*      local_apic;
    struct acpi_apic_record_ioapic*          ioapic;
    struct acpi_apic_record_local_apic_nmis* local_apic_nmis;
    struct acpi_apic_record_interrupt_source_override* interrupt_source_override;

    fprintf(stderr, "acpi: LAPIC at 0x%lX", apic->lapic_base);
    if(apic->flags & ACPI_APIC_FLAG_HAS_PIC) {
        fprintf(stderr, " (with dual 8259 PICs)");
    }
    fprintf(stderr, "\n");

    u8* current_record = apic->records;
    u8* records_end = (u8*)((intp)apic + apic->header.length);
    while(current_record < records_end) {
        u8 type = current_record[0];

        switch(type) {
        case ACPI_APIC_RECORD_TYPE_LOCAL_APIC:
            local_apic = (struct acpi_apic_record_local_apic*)current_record;
            fprintf(stderr, "acpi: Local APIC acpi_processor_id=%d acpi_id=%d flags=%08X\n", local_apic->acpi_processor_id, local_apic->acpi_id, local_apic->flags);
            break;

        case ACPI_APIC_RECORD_TYPE_IOAPIC:
            ioapic = (struct acpi_apic_record_ioapic*)current_record;
            fprintf(stderr, "acpi: I/O APIC ioapic_id=%d ioapic_address=0x%lX global_system_interrupt_base=0x%lX\n",
                    ioapic->ioapic_id, ioapic->ioapic_address, ioapic->global_system_interrupt_base);
            break;

        case ACPI_APIC_RECORD_TYPE_IOAPIC_INTERRUPT_SOURCE_OVERRIDE:
            interrupt_source_override = (struct acpi_apic_record_interrupt_source_override*)current_record;
            fprintf(stderr, "acpi: Interrupt Source Override bus source=%d irq source=%d global_system_interrupt=0x%08X flags=0x%04X\n",
                    interrupt_source_override->bus_source, interrupt_source_override->irq_source,
                    interrupt_source_override->global_system_interrupt, interrupt_source_override->flags);
            break;

        case ACPI_APIC_RECORD_TYPE_LOCAL_APIC_NMIS:
            local_apic_nmis = (struct acpi_apic_record_local_apic_nmis*)current_record;
            fprintf(stderr, "acpi: Local APIC NMIs acpi_processor_id=%d flags=0x%04X lint_number=%d\n", local_apic_nmis->acpi_processor_id, local_apic_nmis->flags, local_apic_nmis->lint_number);
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

