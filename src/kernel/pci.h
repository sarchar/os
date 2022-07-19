#ifndef __PCI_H__
#define __PCI_H__

void pci_notify_segment_group(u16 segment_id, intp base_address, u8 start_bus, u8 end_bus);
void pci_init();
void pci_dump_device_list();

#endif
