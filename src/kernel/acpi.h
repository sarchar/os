#ifndef __ACPI_H__
#define __ACPI_H__

void acpi_set_rsdp_base(intp base);
void acpi_init();
void acpi_init_lai();
void acpi_reset();

// return a virtual memory address pointer to the ACPI table matching signature 'sig'
// If there are multiple tables with that signature, return the 'index'th table.
void* acpi_find_table(char const* sig, u8 index);

#endif
