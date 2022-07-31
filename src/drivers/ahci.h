#ifndef __AHCI_H__
#define __AHCI_H__

void ahci_load();
void ahci_dump_registers();

bool ahci_read_device_sectors(u8 port_index, u64 start_lba, u64 num_sectors, intp dest);
bool ahci_write_device_sectors(u8 port_index, u64 start_lba, u64 num_sectors, intp src);
u32 ahci_get_device_sector_size(u8 port_index);
u32 ahci_get_first_nonpacket_device_port();

#endif
