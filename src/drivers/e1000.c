// Based off of instruction https://wiki.osdev.org/Intel_Ethernet_i217
// and constants from https://elixir.bootlin.com/linux/latest/source/drivers/net/ethernet/intel/e1000/e1000_main.c
#include "common.h"

#include "apic.h"
#include "cpu.h"
#include "e1000.h"
#include "errno.h"
#include "interrupts.h"
#include "kalloc.h"
#include "kernel.h"
#include "net/ethernet.h"
#include "net/net.h"
#include "paging.h"
#include "palloc.h"
#include "pci.h"
#include "stdio.h"
#include "stdlib.h"
#include "vmem.h"

#define E1000_DEV     0x100E
#define E1000_I217    0x153A
#define E1000_82577LM 0x10EA

#define E1000_IO_ADDRESS_OFFSET 0
#define E1000_IO_DATA_OFFSET    4

enum E1000_REG {
    E1000_REG_CONTROL               = 0x0000,
    E1000_REG_STATUS                = 0x0008,
    E1000_REG_EEPROM                = 0x0014,
    E1000_REG_CONTROL_EXT           = 0x0018,
    E1000_REG_INTERRUPT_CAUSE_CLEAR = 0x00C0, // read(only) to get and clear cause
    E1000_REG_INTERRUPT_CAUSE_SET   = 0x00C8, // read-write
    E1000_REG_INTERRUPT_MASK_SET    = 0x00D0, // read-write
    E1000_REG_INTERRUPT_MASK_CLEAR  = 0x00D0, // write-only
    E1000_REG_RXCONTROL             = 0x0100,
    E1000_REG_RXDESC_ADDR_L         = 0x2800,
    E1000_REG_RXDESC_ADDR_H         = 0x2804,
    E1000_REG_RXDESC_LEN            = 0x2808,
    E1000_REG_RXDESC_HEAD           = 0x2810,
    E1000_REG_RXDESC_TAIL           = 0x2818,
    E1000_REG_TXCONTROL             = 0x0400,
    E1000_REG_TXDESC_ADDR_L         = 0x3800,
    E1000_REG_TXDESC_ADDR_H         = 0x3804,
    E1000_REG_TXDESC_LEN            = 0x3808,
    E1000_REG_TXDESC_HEAD           = 0x3810,
    E1000_REG_TXDESC_TAIL           = 0x3818,
    E1000_REG_MAC                   = 0x5400
};

enum E1000_IFLAG {
    E1000_IFLAG_TX_DESC_WRITTEN_BACK     = 1 << 0,
    E1000_IFLAG_TX_QUEUE_EMPTY           = 1 << 1,
    E1000_IFLAG_LINK_STATUS_CHANGE       = 1 << 2,
    E1000_IFLAG_RX_SEQUENCE_ERROR        = 1 << 3,
    E1000_IFLAG_RX_DESC_MIN_THRESHOLD_0  = 1 << 4,
    E1000_IFLAG_RX_OVERRUN               = 1 << 6,
    E1000_IFLAG_RX_TIMER_INT_RING_0      = 1 << 7,
    E1000_IFLAG_MDIO_ACCESS_COMPLETE     = 1 << 9,
};

enum E1000_RXTXCONTROL_FLAGS {
    E1000_RXTXCONTROL_FLAG_RESET                        = 1 << 0,
    E1000_RXTXCONTROL_FLAG_ENABLE                       = 1 << 1,
    E1000_RXCONTROL_FLAG_STORE_BAD_PACKET               = 1 << 2,
    E1000_TXCONTROL_FLAG_BUSY_CHECK_ENABLE              = 1 << 2,
    E1000_RXCONTROL_FLAG_UNICAST_PROMISCUOUS_ENABLE     = 1 << 3,
    E1000_TXCONTROL_FLAG_PAD_SHORT_PACKETS              = 1 << 3,
    E1000_RXCONTROL_FLAG_MULTICAST_PROMISCUOUS_ENABLE   = 1 << 4,
    E1000_TXCONTROL_FLAG_COLLISION_THRESHOLD_SHIFT      = 4,
    E1000_RXTXCONTROL_FLAG_LONG_PACKET_ENABLE           = 1 << 5,
    E1000_RXTXCONTROL_FLAG_LOOPBACK_MODE_NONE           = 0 << 6,
    E1000_RXTXCONTROL_FLAG_LOOPBACK_MODE_MAC            = 1 << 6,
    E1000_RXTXCONTROL_FLAG_LOOPBACK_MODE_SERIAL_LINK    = 2 << 6,
    E1000_RXTXCONTROL_FLAG_LOOPBACK_MODE_TCVR           = 3 << 6,
    E1000_RXTXCONTROL_FLAG_RX_DESC_THRESHOLD_HALF       = 0 << 8,
    E1000_RXTXCONTROL_FLAG_RX_DESC_THRESHOLD_QUARTER    = 1 << 8,
    E1000_RXTXCONTROL_FLAG_RX_DESC_THRESHOLD_EIGHTH     = 2 << 8,
    E1000_TXCONTROL_FLAG_COLLISION_DISTANCE_SHIFT       = 12,
    E1000_RXTXCONTROL_FLAG_BROADCAST_ENABLE             = 1 << 15,
    E1000_RXTXCONTROL_FLAG_DESC_SIZE_2048               = 0 << 16,
    E1000_RXTXCONTROL_FLAG_DESC_SIZE_1024               = 1 << 16,
    E1000_RXTXCONTROL_FLAG_DESC_SIZE_512                = 2 << 16,
    E1000_RXTXCONTROL_FLAG_DESC_SIZE_256                = 3 << 16,
    E1000_RXTXCONTROL_FLAG_DESC_SIZE_16384              = 1 << 16, // these next 3 require BUFFER_SIZE_EXTENSION set
    E1000_RXTXCONTROL_FLAG_DESC_SIZE_8192               = 2 << 16,
    E1000_RXTXCONTROL_FLAG_DESC_SIZE_4096               = 3 << 16,
    E1000_TXCONTROL_FLAG_RETRANSMIT_ON_LATE_COLLISION   = 1 << 24,
    E1000_RXTXCONTROL_FLAG_BUFFER_SIZE_EXTENSION        = 1 << 25,
    E1000_RXTXCONTROL_FLAG_STRIP_ETHERNET_CRC           = 1 << 26,
};

enum E1000_RXTXDESC_STATUS_FLAGS {
    E1000_RXTXDESC_STATUS_FLAG_DONE          = 1 << 0,
    E1000_RXTXDESC_STATUS_FLAG_END_OF_PACKET = 1 << 1,
    E1000_RXDESC_STATUS_FLAG_IGNORE_CHECKSUM = 1 << 2,
};

enum E1000_TXDESC_COMMAND_FLAGS {
    E1000_TXDESC_COMMAND_FLAG_END_OF_PACKET          = 1 << 0,
    E1000_TXDESC_COMMAND_FLAG_INSERT_FCS             = 1 << 1,
    E1000_TXDESC_COMMAND_FLAG_INSERT_CHECKSUM        = 1 << 2,
    E1000_TXDESC_COMMAND_FLAG_REPORT_STATUS          = 1 << 3,
    E1000_TXDESC_COMMAND_FLAG_REPORT_PACKET_SENT     = 1 << 4,
    E1000_TXDESC_COMMAND_FLAG_VLAN_PACKET_ENABLE     = 1 << 6,
    E1000_TXDESC_COMMAND_FLAG_INTERRUPT_DELAY_ENABLE = 1 << 7,
};

#define E1000_DEFAULT_IFLAGS \
    (E1000_IFLAG_LINK_STATUS_CHANGE      | \
     E1000_IFLAG_RX_TIMER_INT_RING_0     | \
     E1000_IFLAG_RX_DESC_MIN_THRESHOLD_0 | \
     E1000_IFLAG_RX_SEQUENCE_ERROR       | \
     E1000_IFLAG_TX_DESC_WRITTEN_BACK)

#define FLUSH_WRITE(edev) (_read_command(edev, E1000_REG_STATUS))
     
struct e1000_rx_desc {
    u64 volatile address;
    u16 volatile length;
    u16 volatile checksum;
    u8  volatile status;
    u8  volatile errors;
    u16 volatile special;
} __packed;

static_assert(sizeof(struct e1000_rx_desc) == 16, "e1000_rx_desc is a fixed structure of size 16");

struct e1000_tx_desc {
    u64 volatile address;
    u16 volatile length;
    u8  volatile cso;
    u8  volatile command;
    u8  volatile status;
    u8  volatile css;
    u16 volatile special;
} __packed;

static_assert(sizeof(struct e1000_tx_desc) == 16, "e1000_tx_desc is a fixed structure of size 16");

struct e1000_device {
    struct net_device       net_device;
    struct pci_device_info* pci_device;

    intp bar0;

    u8   mac[6];
    u8   bar0_mmio;
    bool has_eeprom;

    struct e1000_rx_desc* rx_desc;
    struct e1000_tx_desc* tx_desc;

    u16  rx_desc_count;
    u16  tx_desc_count;
    u16  rx_desc_next;
    u16  tx_desc_next;
};

static void _initialize_e1000(struct pci_device_info* dev, u8);
static void _enable_interrupts(struct e1000_device*);
static void _disable_interrupts(struct e1000_device*);
static void _e1000_interrupt(struct interrupt_stack_registers* regs, intp pc, void* userdata);
static void _receive_packets(struct e1000_device*);
static s64  _net_transmit_e1000_packet(struct net_device*, u8*, u8, u8, u8*, u16);

static bool _find_e1000_devices_cb(struct pci_device_info* dev, void* userinfo)
{
    if(dev->config->device_id == E1000_DEV ||
       dev->config->device_id == E1000_I217 ||
       dev->config->device_id == E1000_82577LM) {
        // allocate a new node for the list
        intp* found_dev = (intp*)kalloc(sizeof(intp) * 2);
        // set up next pointer
        found_dev[0] = *(intp*)userinfo;
        *(intp*)userinfo = (intp)found_dev;
        // and a pointer to the pci device
        found_dev[1] = (intp)dev;

        fprintf(stderr, "e1000: found device 0x%04X:0x%04X\n", dev->config->vendor_id, dev->config->device_id);
    }

    return true;
}

void e1000_load()
{
    struct _e1000_dev {
        struct _e1000_dev* next;
        struct pci_device_info* dev;
    }* e1000_devices = null;

    pci_iterate_vendor_devices(0x8086, _find_e1000_devices_cb, &e1000_devices);

    u8 eth_index = 0;
    while(e1000_devices) {
        _initialize_e1000(e1000_devices->dev, eth_index);

        struct _e1000_dev* next = e1000_devices->next;
        kfree(e1000_devices);
        e1000_devices = next;
        eth_index++;
    }
}

static void _write_command(struct e1000_device* edev, enum E1000_REG reg, u32 val)
{
    if(edev->bar0_mmio) {
        *(u32*)(edev->bar0 + (u16)reg) = val;
    } else {
        __outl(edev->bar0 + E1000_IO_ADDRESS_OFFSET, (u32)reg);
        __outl(edev->bar0 + E1000_IO_DATA_OFFSET, (u32)val);
    }
}

static u32 _read_command(struct e1000_device* edev, enum E1000_REG reg)
{
    if(edev->bar0_mmio) {
        return *(u32*)(edev->bar0 + (u16)reg);
    } else {
        __outl(edev->bar0 + E1000_IO_ADDRESS_OFFSET, (u32)reg);
        return __inl(edev->bar0 + E1000_IO_DATA_OFFSET);
    }
}

static u16 _read_eeprom(struct e1000_device* edev, u8 offset)
{
    u32 v;

    _write_command(edev, E1000_REG_EEPROM, ((u32)offset << 8) | 1);
    while(((v = _read_command(edev, E1000_REG_EEPROM)) & 0x10) == 0) ;

    return (v >> 16) & 0xFFFF;
}

static void _detect_eeprom(struct e1000_device* edev)
{
    _write_command(edev, E1000_REG_EEPROM, 0x01);

    for(u64 i = 0; i < 1000 && !edev->has_eeprom; i++) {
        edev->has_eeprom = (_read_command(edev, E1000_REG_EEPROM) & 0x10) != 0;
        //__pause(); // not necessary?
    }

    fprintf(stderr, "e1000: EEPROM %sdetected\n", edev->has_eeprom ? "" : "not ");
}

static s64 _read_mac_address(struct e1000_device* edev)
{
    if(edev->has_eeprom) { // read MAC from the EEPROM
        u32 v = _read_eeprom(edev, 0);
        edev->mac[0] = v & 0xFF;
        edev->mac[1] = (v >> 8) & 0xFF;
        v = _read_eeprom(edev, 1);
        edev->mac[2] = v & 0xFF;
        edev->mac[3] = (v >> 8) & 0xFF;
        v = _read_eeprom(edev, 2);
        edev->mac[4] = v & 0xFF;
        edev->mac[5] = (v >> 8) & 0xFF;
    } else { // read MAC from registers
        u32* macptr = (u32*)(edev->bar0 + E1000_REG_MAC); 
        if(*macptr == 0) return -EINVAL;

        edev->mac[0] = (macptr[0] >>  0) & 0xFF;
        edev->mac[1] = (macptr[0] >>  8) & 0xFF;
        edev->mac[2] = (macptr[0] >> 16) & 0xFF;
        edev->mac[3] = (macptr[0] >> 24) & 0xFF;
        edev->mac[4] = (macptr[1] >>  0) & 0xFF;
        edev->mac[5] = (macptr[1] >>  8) & 0xFF;
    }

    fprintf(stderr, "e1000: device has MAC %02x:%02x:%02x:%02x:%02x:%02x\n", edev->mac[0], edev->mac[1], edev->mac[2], edev->mac[3], edev->mac[4], edev->mac[5]);
    return 0;
}

static void _setup_rx(struct e1000_device* edev)
{
    // allocate one page for rx descriptors
    intp ptr = palloc_claim_one();
    edev->rx_desc = (struct e1000_rx_desc*)vmem_map_page(VMEM_KERNEL, ptr, MAP_PAGE_FLAG_WRITABLE | MAP_PAGE_FLAG_DISABLE_CACHE);
    edev->rx_desc_count = PAGE_SIZE / sizeof(struct e1000_rx_desc);

    for(u32 d = 0; d < edev->rx_desc_count; d++) {
        struct e1000_rx_desc* desc = &edev->rx_desc[d];
        zero(desc);

        // for now we're going to assume MTU is <2kb (page size / 2), so there will be enough room for the ethernet header and frame
        // so we can use 1 page for two descriptors
        if((d & 0x01) == 0) {
            desc->address = palloc_claim_one();
        } else {
            desc->address = edev->rx_desc[d & ~0x01].address + (PAGE_SIZE >> 1);
        }
    }

    // disable RX before changing buffers
    u32 rxcontrol = _read_command(edev, E1000_REG_RXCONTROL);
    _write_command(edev, E1000_REG_RXCONTROL, rxcontrol & ~E1000_RXTXCONTROL_FLAG_ENABLE);

    // TODO set RX delay timer ?

    _write_command(edev, E1000_REG_RXDESC_LEN, edev->rx_desc_count * sizeof(struct e1000_rx_desc)); // set length in bytes of the descriptors array
    _write_command(edev, E1000_REG_RXDESC_ADDR_H, ptr >> 32);          // 64-bit physical address to the desc array
    _write_command(edev, E1000_REG_RXDESC_ADDR_L, ptr & 0xFFFFFFFF);
    _write_command(edev, E1000_REG_RXDESC_TAIL, edev->rx_desc_count - 1);  // tail points to the last element
    edev->rx_desc_next = 0;
    _write_command(edev, E1000_REG_RXDESC_HEAD, edev->rx_desc_next);

    // configure and enable the rx ring buffer
    rxcontrol |= E1000_RXCONTROL_FLAG_STORE_BAD_PACKET;
    rxcontrol |= E1000_RXCONTROL_FLAG_UNICAST_PROMISCUOUS_ENABLE | E1000_RXCONTROL_FLAG_MULTICAST_PROMISCUOUS_ENABLE;
    rxcontrol |= E1000_RXTXCONTROL_FLAG_LOOPBACK_MODE_NONE;
    rxcontrol |= E1000_RXTXCONTROL_FLAG_RX_DESC_THRESHOLD_HALF;
    rxcontrol |= E1000_RXTXCONTROL_FLAG_BROADCAST_ENABLE;
    rxcontrol |= E1000_RXTXCONTROL_FLAG_DESC_SIZE_2048;
    rxcontrol &= ~E1000_RXTXCONTROL_FLAG_BUFFER_SIZE_EXTENSION;

    _write_command(edev, E1000_REG_RXCONTROL, rxcontrol | E1000_RXTXCONTROL_FLAG_ENABLE);

    fprintf(stderr, "e1000: rx ring buffer initialized with %d descriptors\n", edev->rx_desc_count);
}

static void _setup_tx(struct e1000_device* edev)
{
    // allocate one page for tx descriptors
    intp ptr = palloc_claim_one();
    edev->tx_desc = (struct e1000_tx_desc*)vmem_map_page(VMEM_KERNEL, ptr, MAP_PAGE_FLAG_WRITABLE | MAP_PAGE_FLAG_DISABLE_CACHE);
    edev->tx_desc_count = PAGE_SIZE / sizeof(struct e1000_tx_desc);

    for(u32 d = 0; d < edev->tx_desc_count; d++) {
        struct e1000_tx_desc* desc = &edev->tx_desc[d];
        zero(desc);

        // for now we're going to assume MTU is <2kb (page size / 2), so there will be enough room for the ethernet header and frame
        // so we can use 1 page for two descriptors
        if((d & 0x01) == 0) {
            desc->address = palloc_claim_one();
        } else {
            desc->address = edev->tx_desc[d & ~0x01].address + (PAGE_SIZE >> 1);
        }
    }

    // disable TX before changing buffers
    u32 txcontrol = _read_command(edev, E1000_REG_TXCONTROL);
    _write_command(edev, E1000_REG_TXCONTROL, txcontrol & ~E1000_RXTXCONTROL_FLAG_ENABLE);

    // TODO set TX delay timer ?

    _write_command(edev, E1000_REG_TXDESC_LEN, edev->tx_desc_count * sizeof(struct e1000_tx_desc)); // set length in bytes of the descriptors array
    _write_command(edev, E1000_REG_TXDESC_ADDR_H, ptr >> 32);          // 64-bit physical address to the desc array
    _write_command(edev, E1000_REG_TXDESC_ADDR_L, ptr & 0xFFFFFFFF);
    edev->tx_desc_next = 0;
    _write_command(edev, E1000_REG_TXDESC_TAIL, edev->tx_desc_next);
    _write_command(edev, E1000_REG_TXDESC_HEAD, edev->tx_desc_next);

    // configure and enable the tx ring buffer
    txcontrol |= E1000_TXCONTROL_FLAG_PAD_SHORT_PACKETS;
    txcontrol |= (15 << E1000_TXCONTROL_FLAG_COLLISION_THRESHOLD_SHIFT);
    txcontrol |= (64 << E1000_TXCONTROL_FLAG_COLLISION_DISTANCE_SHIFT);
    txcontrol |= E1000_TXCONTROL_FLAG_RETRANSMIT_ON_LATE_COLLISION;

    _write_command(edev, E1000_REG_TXCONTROL, txcontrol | E1000_RXTXCONTROL_FLAG_ENABLE);

    fprintf(stderr, "e1000: tx ring buffer initialized with %d descriptors\n", edev->tx_desc_count);
}

#include "net/ipv4.h" // TEMP
static void _register_network_device(struct e1000_device* edev, u8 eth_index)
{
    // set the hardware addres on the network device
    struct net_address hardware_address;
    hardware_address.protocol = NET_PROTOCOL_ETHERNET;
    memcpy(hardware_address.ethernet, edev->mac, 6);

    // initialize the network device
    net_init_device(&edev->net_device, "e1000", eth_index, &hardware_address); // will create vnode #device=net:N #driver=e1000:M
    net_set_transmit_packet_function(&edev->net_device, &_net_transmit_e1000_packet);

    // TODO TEMP create an IPv4 interface on this device
    struct net_address local_addr;
    ipv4_parse_address_string(&local_addr, "172.21.160.20");
    struct net_interface* iface = ipv4_create_interface(&local_addr); // TODO will create #interface=ipv4:N
    net_device_register_interface(&edev->net_device, iface);
}

static void _initialize_e1000(struct pci_device_info* pci_dev, u8 eth_index)
{
    fprintf(stderr, "e1000: initializing device %04X:%04X (interrupt_line = %d)\n", pci_dev->config->vendor_id, pci_dev->config->device_id, pci_dev->config->h0.interrupt_line);
    struct e1000_device* edev = (struct e1000_device*)malloc(sizeof(struct e1000_device));
    zero(edev);

    edev->pci_device = pci_dev;
    edev->bar0_mmio  = pci_device_is_bar_mmio(pci_dev, 0);
    edev->bar0       = pci_device_map_bar(pci_dev, 0);
    fprintf(stderr, "e1000: bar0 (type = %s) at addr 0x%lX\n", edev->bar0_mmio ? "mmio" : "io", edev->bar0);
    _detect_eeprom(edev);
    _read_mac_address(edev);

    // TODO: what's this doing?
    for(u32 i = 0; i < 0x80; i++) {
        _write_command(edev, 0x5200 + i * 4, 0);
    }

    // setup rx/tx buffers
    _setup_rx(edev);
    _setup_tx(edev);

    // map the interrupt from the PCI device to a new interrupt callback here in this module
    u64 cpu_flags = __cli_saveflags();
    u32 cpu_irq = pci_setup_msi(pci_dev, 1);
    if(cpu_irq != 0) { // device supports MSI
        fprintf(stderr, "e1000: device supports MSI\n");

        pci_set_enable_msi(pci_dev, true);
    } else {
        fprintf(stderr, "e1000: device does not support MSI, mapping global interrupt %d\n", pci_dev->config->h0.interrupt_line);

        // Install a redirection from the PCI irq line to a new cpu interrupt
        // TODO: allocate cpu irqs!!
        cpu_irq = 60;
        apic_set_io_apic_redirection(pci_dev->config->h0.interrupt_line, cpu_irq,
                                     IO_APIC_REDIRECTION_FLAG_DELIVERY_NORMAL,
                                     IO_APIC_REDIRECTION_DESTINATION_PHYSICAL,
                                     IO_APIC_REDIRECTION_ACTIVE_HIGH,
                                     IO_APIC_REDIRECTION_EDGE_SENSITIVE,
                                     true, apic_get_apic_id(0)); // map to first cpu
    }

    interrupts_install_handler(cpu_irq, _e1000_interrupt, (void*)edev);

    // allow PCI to trigger interrupts (and enable bus mastering)
    u32 cmd = edev->pci_device->config->command & ~PCI_COMMAND_FLAG_DISABLE_INTERRUPTS;
    edev->pci_device->config->command = cmd | PCI_COMMAND_FLAG_BUS_MASTER;

    // restore cpu interrupts
    __restoreflags(cpu_flags);

    // create the network device
    _register_network_device(edev, eth_index);

    // enable interrupts on the device
    _enable_interrupts(edev);

    // trigger a link change interrupt
    //_write_command(edev, E1000_REG_INTERRUPT_CAUSE_SET, E1000_IFLAG_LINK_STATUS_CHANGE);

    //!// TEMP send a packet
    //!// the buffer below is an ICMP ping packet from 192.168.1.32 to 192.168.1.64
    //!s64 e1000_transmit_packet(struct e1000_device* edev, u8* dest_mac, u16 ethertype, intp data, u16 length);
    //!u8 buf[] = {
    //!        0x45, 0x00, 0x00, 0x3C, 0x82, 0x47, 0x00, 0x00, 0x20, 0x01, 0x94, 0xC9, 0xC0, 0xA8, 0x01, 0x20, 
    //!        0xC0, 0xA8, 0x01, 0x40, 0x08, 0x00, 0x48, 0x5C, 0x01, 0x00, 0x04, 0x00, 0x61, 0x62, 0x63, 0x64, 
    //!        0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 
    //!        0x75, 0x76, 0x77, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69
    //!};
    //!u8 dest[6] = { 0x00, 0x15, 0x5d, 0x89, 0xad, 0x11 };
    //!e1000_transmit_packet(edev, dest, ETHERTYPE_IPv4, (intp)buf, sizeof(buf));
}

static void _enable_interrupts(struct e1000_device* edev)
{
    _read_command(edev, E1000_REG_INTERRUPT_CAUSE_CLEAR); // clear any outstanding interrupt cause

    // enable interrupts on the nic
    _write_command(edev, E1000_REG_INTERRUPT_MASK_SET, E1000_DEFAULT_IFLAGS);
    //_write_command(edev, E1000_REG_INTERRUPT_MASK_SET, 0xFFUL & ~0x04); //TODO wtf is this

    FLUSH_WRITE(edev);
}

static void _disable_interrupts(struct e1000_device* edev)
{
    // clear the iflags by writing which bits to clear to the clear register
    _write_command(edev, E1000_REG_INTERRUPT_MASK_CLEAR, ~0);
    FLUSH_WRITE(edev);
}

static void _e1000_interrupt(struct interrupt_stack_registers* regs, intp pc, void* userdata)
{
    unused(regs);
    unused(pc);
    unused(_disable_interrupts);

    struct e1000_device* edev = (struct e1000_device*)userdata;
    u32 cause = _read_command(edev, E1000_REG_INTERRUPT_CAUSE_CLEAR);

    if(cause & E1000_IFLAG_RX_TIMER_INT_RING_0) {
        // packet received
        _receive_packets(edev);
        cause &= ~E1000_IFLAG_RX_TIMER_INT_RING_0;
    }

    if(cause & E1000_IFLAG_LINK_STATUS_CHANGE) {
        fprintf(stderr, "e1000: unhandled link status change\n");

        cause &= ~E1000_IFLAG_LINK_STATUS_CHANGE;
    }

    if(cause & E1000_IFLAG_TX_DESC_WRITTEN_BACK) {
        fprintf(stderr, "e1000: transmit packet completed\n");

        cause &= ~E1000_IFLAG_TX_DESC_WRITTEN_BACK;
    }

    if(cause & E1000_IFLAG_TX_QUEUE_EMPTY) {
        fprintf(stderr, "e1000: tx queue empty\n");

        cause &= ~E1000_IFLAG_TX_QUEUE_EMPTY;
    }

    if(cause != 0) {
        fprintf(stderr, "e1000: unhandled interrupt cause 0x%lX\n", cause);
    }
}

static void _handle_packet(struct e1000_device* edev, struct e1000_rx_desc* desc)
{
    // safety check for bogus packets
    if(desc->length > (PAGE_SIZE >> 1)) {
        fprintf(stderr, "e1000: invalid packet of size %d found, dropping\n", desc->length);
        return;
    }

    u8* data = (u8*)desc->address;
    u16 ethertype = ntohs(*(u16*)&data[12]);
    u16 payload_size = desc->length - 14 - 4; // 14 bytes for header, 4 bytes for FCS (ethernet CRC)
    u32 ethernet_crc = ntohl(*(u32*)&data[desc->length - 4]);

    fprintf(stderr, "e1000: packet start (length = %d)\n", desc->length);
    fprintf(stderr, "        status          = 0x%02X\n", desc->status);
    fprintf(stderr, "        destination MAC = %02x:%02x:%02x:%02x:%02x:%02x\n", data[0], data[1], data[2], data[3], data[4], data[5]);
    fprintf(stderr, "        source MAC      = %02x:%02x:%02x:%02x:%02x:%02x\n", data[6], data[7], data[8], data[9], data[10], data[11]);
    fprintf(stderr, "        ethertype       = 0x%04X (%s)\n", ethertype, (ethertype == ETHERTYPE_IPv4) ? "IPv4" 
                                                                            : ((ethertype == ETHERTYPE_IPv6) ? "IPv6" 
                                                                                : ((ethertype == ETHERTYPE_ARP) ? "ARP" : "unknown")));
    fprintf(stderr, "        payload size    = %d\n", payload_size);
    fprintf(stderr, "        ethernet crc    = 0x%08X%s\n", ethernet_crc, (desc->status & E1000_RXDESC_STATUS_FLAG_IGNORE_CHECKSUM) ? " (ignored)" : "");

    //if(ethertype == ETHERTYPE_ARP) {
    //    fprintf(stderr, "        data            =\n");
    //    for(u32 i = 0; i < desc->length; i++) {
    //        fprintf(stderr, "%02X ", data[i]);
    //    }
    //}

    // drop this packet if it's too large
    if(payload_size > 1500) return;

    //TODO CRCs?
    u8 net_protocol = NET_PROTOCOL_UNSUPPORTED;
    if     (ethertype == ETHERTYPE_IPv4) net_protocol = NET_PROTOCOL_IPv4;
    else if(ethertype == ETHERTYPE_IPv6) net_protocol = NET_PROTOCOL_IPv6;
    else if(ethertype == ETHERTYPE_ARP)  net_protocol = NET_PROTOCOL_ARP;

    net_receive_packet(&edev->net_device, net_protocol, &data[14], payload_size);
}

static void _receive_packets(struct e1000_device* edev)
{
    struct e1000_rx_desc* desc;

    while(((desc = &edev->rx_desc[edev->rx_desc_next])->status & E1000_RXTXDESC_STATUS_FLAG_DONE) != 0) {
        //fprintf(stderr, "e1000: got packet length %d, status = 0x%lX\n", desc->length, desc->status);
        assert(desc->status & E1000_RXTXDESC_STATUS_FLAG_END_OF_PACKET, "multi-frame packets not supported atm. EOP must be set on all packets");

        // process the packet before updating the tail pointer. this allows the packet to be copied before memory is reused
        _handle_packet(edev, desc);

        // tell the hardware the packet is processed
        desc->status = 0;  // clear FLAG_DONE especially
        _write_command(edev, E1000_REG_RXDESC_TAIL, edev->rx_desc_next);
        edev->rx_desc_next = (edev->rx_desc_next + 1) % edev->rx_desc_count;
    }

    // update the tail to be 1 behind our read pointer
    //_write_command(edev, E1000_REG_RXDESC_TAIL, (edev->rx_desc_next - 1) % edev->rx_desc_count);
}

static void _transmit_packet(struct e1000_device* edev, intp data, u16 length)
{
    // drop packets that are too large
    if(length > 1500) {
        fprintf(stderr, "e1000: dropped tx packet (size %d too large)\n", length);
        return;
    }

    struct e1000_tx_desc* desc = &edev->tx_desc[edev->tx_desc_next];
    memcpy((void*)desc->address, (void*)data, length);
    desc->length = length;
    desc->status = 0; // clear status, as FLAG_DONE indicates the hw has transmitted the packet
    desc->command |= E1000_TXDESC_COMMAND_FLAG_END_OF_PACKET |
                     E1000_TXDESC_COMMAND_FLAG_INSERT_FCS    |
                     E1000_TXDESC_COMMAND_FLAG_REPORT_STATUS;

    edev->tx_desc_next = (edev->tx_desc_next + 1) % edev->tx_desc_count;
    _write_command(edev, E1000_REG_TXDESC_TAIL, edev->tx_desc_next);
}

s64 e1000_transmit_packet(struct e1000_device* edev, u8* dest_mac, u16 ethertype, intp data, u16 length)
{
    u8* buf = (u8*)__builtin_alloca(length + 14);

    // destination mac address
    memcpy(&buf[0], dest_mac, 6);

    // source mac address
    memcpy(&buf[6], edev->mac, 6);

    // set the ethertype
    *(u16*)&buf[12] = htons(ethertype);

    // copy the data
    memcpy(&buf[14], (void*)data, length);

    _transmit_packet(edev, (intp)buf, length + 14);

    return 0;
}

static s64 _net_transmit_e1000_packet(struct net_device* ndev, u8* dest_address, u8 dest_address_length, u8 net_protocol, u8* packet, u16 packet_length)
{
    //struct e1000_device* edev = container_of(struct e1000_device, net_device, ndev);

    struct e1000_device* edev = (struct e1000_device*)((intp)ndev - offsetof(struct e1000_device, net_device));
    if(dest_address_length != 6) return -EINVAL;

    u16 ethertype;
    if     (net_protocol == NET_PROTOCOL_IPv4) ethertype = ETHERTYPE_IPv4;
    else if(net_protocol == NET_PROTOCOL_IPv6) ethertype = ETHERTYPE_IPv6;
    else if(net_protocol == NET_PROTOCOL_ARP)  ethertype = ETHERTYPE_ARP;
    else   return -EINVAL;

    return e1000_transmit_packet(edev, dest_address, ethertype, (intp)packet, packet_length);
}
