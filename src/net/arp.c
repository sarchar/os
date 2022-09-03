#include "kernel/common.h"

#include "kernel/cpu.h"
#include "kernel/kernel.h"
#include "net/ethernet.h"
#include "net/net.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

struct arp_packet {
    u16 hardware_type;
    u16 protocol_type;
    u8  hardware_address_length;
    u8  protocol_address_length;
    u16 opcode;

    // addresses is 4 variable length addresses:
    // u8 source_hardware_address[hardware_address_length];
    // u8 source_protocol_address[protocol_address_length];
    // u8 dest_hardware_address[hardware_address_length];
    // u8 dest_protocol_address[protocol_address_length];
    u8  addresses[];
};

u8* arp_create_request(struct net_device* ndev, u8 net_protocol, u8* address, u8 address_length, u64* reslen)
{
    struct arp_packet tmp;

    // setup the hardware type and address size
    if(ndev->hw_type == NET_DEVICE_HW_TYPE_ETHERNET) {
        tmp.hardware_type = htons(0x0001);
        tmp.hardware_address_length = 6; // MAC addresses are 6 bytes
    } else {
        assert(false, "other hardware types aren't supported");
    }

    // setup the protocol type and size
    if(net_protocol == NET_PROTOCOL_IPv4) {
        tmp.protocol_type = htons(ETHERTYPE_IPv4);
        tmp.protocol_address_length = 4;

        if(address_length != tmp.protocol_address_length) return 0;
        fprintf(stderr, "arp: creating request packet for %d.%d.%d.%d\n", address[0], address[1], address[2], address[3]);
    } else if(net_protocol == NET_PROTOCOL_IPv6) {
        //TODO ipv6 arp
        tmp.protocol_type = htons(ETHERTYPE_IPv6);
        tmp.protocol_address_length = 6;

        if(address_length != tmp.protocol_address_length) return 0;
        //fprintf(stderr, "arp: creating request packet for %d.%d.%d.%d\n", address[0], address[1], address[2], address[3]);
        assert(false, "incomplete");
    }

    // compute the packet allocation size 
    u32 packet_size = sizeof(struct arp_packet) + 2 * (tmp.hardware_address_length + tmp.protocol_address_length);
    struct arp_packet* arp = (struct arp_packet*)malloc(packet_size);

    // copy over the types and sizes
    *arp = tmp;

    // opcode 1 is ARP request
    arp->opcode = htons(0x0001);

    // start with the source_hardware_address
    u8* addresses = arp->addresses;
    memcpy(addresses, ndev->hw_address, arp->hardware_address_length);
    addresses += arp->hardware_address_length;

    // next is source_protocol_address
    // TEMP TODO encoding 172.21.169.20
    u8 my_addr[] = { 172, 21, 169, 20 };
    memcpy(addresses, my_addr, arp->protocol_address_length);
    addresses += arp->protocol_address_length;

    // next is dest_hardware_address, which we just zero out
    memset(addresses, 0, arp->hardware_address_length);
    addresses += arp->hardware_address_length;

    // last is dest_protocol_address, the address we're looking up
    memcpy(addresses, address, arp->protocol_address_length);

    // done
    *reslen = packet_size;
    return (u8*)arp;
}

