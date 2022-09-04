#include "kernel/common.h"

#include "kernel/cpu.h"
#include "kernel/kernel.h"
#include "net/ethernet.h"
#include "net/ipv4.h"
#include "net/net.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

#define ARP_OPCODE_REQUEST 0x0001
#define ARP_OPCODE_REPLY   0x0002

#define ARP_HARDWARE_TYPE_ETHERNET 0x0001

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

static u8 my_addr[] = { 172, 21, 169, 20 };
static u8 ZERO_MAC[] = { 0, 0, 0, 0, 0, 0 };

u8* _new_arp_packet(u16 opcode,
                    u16 hardware_type, u16 protocol_type, 
                    u8* source_hardware_address, 
                    u8* source_protocol_address,
                    u8* dest_hardware_address,
                    u8* dest_protocol_address,
                    u64* reslen)
{
    struct arp_packet tmp;

    // setup the hardware type and address size
    tmp.hardware_type = htons(hardware_type);
    if(hardware_type == 0x0001) {
        tmp.hardware_address_length = 6; // MAC addresses are 6 bytes
    } else {
        assert(false, "other hardware types aren't supported");
    }

    // setup the protocol type and size
    tmp.protocol_type = htons(protocol_type);
    if(protocol_type == ETHERTYPE_IPv4) {
        tmp.protocol_address_length = 4;
    } else if(protocol_type == ETHERTYPE_IPv6) {
        tmp.protocol_address_length = 6;
        assert(false, "incomplete"); //TODO ipv6 arp
    }

    // compute the packet allocation size and allocate storage
    u32 packet_size = sizeof(struct arp_packet) + 2 * (tmp.hardware_address_length + tmp.protocol_address_length);
    struct arp_packet* arp = (struct arp_packet*)malloc(packet_size);

    // copy over the types and sizes
    *arp = tmp;

    // opcode 1 is ARP request, 2 is reply
    arp->opcode = htons(opcode);

    // start with the source_hardware_address
    u8* addresses = arp->addresses;
    memcpy(addresses, source_hardware_address, arp->hardware_address_length);
    addresses += arp->hardware_address_length;

    // next is source_protocol_address
    // TEMP TODO encoding 172.21.169.20
    memcpy(addresses, source_protocol_address, arp->protocol_address_length);
    addresses += arp->protocol_address_length;

    // next is dest_hardware_address
    memcpy(addresses, dest_hardware_address, arp->hardware_address_length);
    addresses += arp->hardware_address_length;

    // last is dest_protocol_address
    memcpy(addresses, dest_protocol_address, arp->protocol_address_length);

    // done
    *reslen = packet_size;
    return (u8*)arp;
}


u8* arp_create_request(struct net_device* ndev, u8 net_protocol, u8* lookup_address, u64* reslen)
{
    u16 hardware_type;
    u16 protocol_type;

    // setup the hardware type and address size
    if(ndev->hardware_address.protocol == NET_PROTOCOL_ETHERNET) {
        hardware_type = ARP_HARDWARE_TYPE_ETHERNET;
    } else {
        assert(false, "other hardware types aren't supported");
    }

    // setup the protocol type and size
    if(net_protocol == NET_PROTOCOL_IPv4) {
        protocol_type = ETHERTYPE_IPv4;
        fprintf(stderr, "arp: creating request packet for %d.%d.%d.%d\n", lookup_address[0], lookup_address[1], lookup_address[2], lookup_address[3]);
    } else if(net_protocol == NET_PROTOCOL_IPv6) {
        protocol_type = ETHERTYPE_IPv6;
        //fprintf(stderr, "arp: creating request packet for %d.%d.%d.%d\n", lookup_address[0], lookup_address[1], lookup_address[2], lookup_address[3]);
        assert(false, "incomplete"); //TODO ipv6 arp
    } else {
        assert(false, "unknown protocol");
    }

    return _new_arp_packet(ARP_OPCODE_REQUEST, hardware_type, protocol_type, 
                           ndev->hardware_address.ethernet, // source_hardware_address
                           my_addr,          // source_protocol_address
                           ZERO_MAC,         // dest_hardware_address (zeroed for the ARP request)
                           lookup_address,   // dest_protocol_address (the address we're requesting ARP for)
                           reslen);
}

u8* arp_create_reply(struct net_interface* iface, u8 net_protocol, u8* dest_hardware_address, u8* dest_protocol_address, u64* reslen)
{
    u16 hardware_type;
    u16 protocol_type;

    // get the network device this interface is attached to
    struct net_device* ndev = iface->net_device;

    // setup the hardware type and address size
    if(ndev->hardware_address.protocol == NET_PROTOCOL_ETHERNET) {
        hardware_type = ARP_HARDWARE_TYPE_ETHERNET;
    } else {
        assert(false, "other hardware types aren't supported");
    }

    // setup the protocol type and size
    if(net_protocol == NET_PROTOCOL_IPv4) {
        protocol_type = ETHERTYPE_IPv4;
        fprintf(stderr, "arp: creating reply packet to %d.%d.%d.%d\n", dest_protocol_address[0], dest_protocol_address[1], dest_protocol_address[2], dest_protocol_address[3]);
    } else if(net_protocol == NET_PROTOCOL_IPv6) {
        protocol_type = ETHERTYPE_IPv6;
        //fprintf(stderr, "arp: creating reply packet for %d.%d.%d.%d\n", dest_protocol_address[0], dest_protocol_address[1], dest_protocol_address[2], dest_protocol_address[3]);
        assert(false, "incomplete"); //TODO ipv6 arp
    }

    // setup source_protocol_address
    u8 source_protocol_address[4];
    source_protocol_address[0] = (iface->address.ipv4 >> 24) & 0xFF;
    source_protocol_address[1] = (iface->address.ipv4 >> 16) & 0xFF;
    source_protocol_address[2] = (iface->address.ipv4 >>  8) & 0xFF;
    source_protocol_address[3] = (iface->address.ipv4 >>  0) & 0xFF;

    return _new_arp_packet(ARP_OPCODE_REPLY, hardware_type, protocol_type, 
                           ndev->hardware_address.ethernet,  // source_hardware_address
                           source_protocol_address,          // source_protocol_address
                           dest_hardware_address,            // dest_hardware_address
                           dest_protocol_address,            // dest_protocol_address (the address we're requesting ARP for)
                           reslen);
}

void arp_handle_device_packet(struct net_device* ndev, u8* packet, u16 packet_length)
{
    struct arp_packet* inp = (struct arp_packet*)packet;
    u16 opcode = htons(inp->opcode);

    // verify packet length
    // first, hardware address length
    if(ntohs(inp->hardware_type) != ARP_HARDWARE_TYPE_ETHERNET) return; // must be type Ethernet

    // must have 6 byte ethernet hardware addresses
    if(inp->hardware_address_length != 6) return;

    // verify the protocol
    u16 protocol_type = htons(inp->protocol_type);
    if(protocol_type == ETHERTYPE_IPv4) {
        if(inp->protocol_address_length != 4) return;
    } else if(protocol_type == ETHERTYPE_IPv6) {
        //if(inp->protocol_address_length != 16) return;
        return; //TODO IPv6
    } else {
        return; // unsupported protocol
    }

    // validate the packet size. ethernet will pad packets to at least 64 bytes, so we have to allow packets larger than what we really need
    u16 wanted_packet_length = sizeof(struct arp_packet) + 2 * (inp->hardware_address_length + inp->protocol_address_length);
    if(wanted_packet_length > packet_length) {
        fprintf(stderr, "arp: incoming packet of size %d incorrect (wanted %d)\n", packet_length, wanted_packet_length);
        return;
    }

    // set up address pointers
    u8* source_hardware_address = inp->addresses;
    u8* source_protocol_address = source_hardware_address + inp->hardware_address_length;
    u8* dest_hardware_address   = source_protocol_address + inp->protocol_address_length;
    u8* dest_protocol_address   = dest_hardware_address   + inp->hardware_address_length;

    if(opcode == ARP_OPCODE_REQUEST && protocol_type == ETHERTYPE_IPv4) { // && memcmp(dest_protocol_address, my_addr, 4) == 0) {
        // the incoming response forms an IPv4 address, so let's see if we have an interface for that device
        struct net_address search_address;
        zero(&search_address);
        search_address.protocol = NET_PROTOCOL_IPv4;
        search_address.ipv4 = (dest_protocol_address[0] << 24) | (dest_protocol_address[1] << 16) | (dest_protocol_address[2] << 8) | (dest_protocol_address[3] << 0);

        struct net_interface* iface = net_device_find_interface(ndev, &search_address); // TODO search all interfaces, not just ones on this device?
        if(iface == null) {
            char buf[16];
            ipv4_format_address(buf, search_address.ipv4);
            fprintf(stderr, "arp: no device found for IP address %s\n", buf);
            return;
        }

        fprintf(stderr, "arp: sending response\n");

        // TODO might we ever need to route the response to a different network device? if so, we'll need to route 
        // based on the source_protocol_address, perhaps.

        // someone is looking for our address, tell them
        u64 response_packet_length;
        u8* response_packet = arp_create_reply(iface, NET_PROTOCOL_IPv4, source_hardware_address, source_protocol_address, &response_packet_length);
        if(response_packet != null) {
            net_transmit_packet(ndev, source_hardware_address, inp->hardware_address_length, NET_PROTOCOL_ARP, response_packet, response_packet_length); // transmit ARP packet
            free(response_packet);
        }
    } else {
        // TODO see if it was a reply to one of our requests
    }
}