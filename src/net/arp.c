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

//!u8* arp_create_request(struct net_device* ndev, u8 net_protocol, u8* lookup_address, u64* reslen)
//!{
//!    u16 hardware_type;
//!    u16 protocol_type;
//!
//!    // setup the hardware type and address size
//!    if(ndev->hardware_address.protocol == NET_PROTOCOL_ETHERNET) {
//!        hardware_type = ARP_HARDWARE_TYPE_ETHERNET;
//!    } else {
//!        assert(false, "other hardware types aren't supported");
//!    }
//!
//!    // setup the protocol type and size
//!    if(net_protocol == NET_PROTOCOL_IPv4) {
//!        protocol_type = ETHERTYPE_IPv4;
//!        fprintf(stderr, "arp: creating request packet for %d.%d.%d.%d\n", lookup_address[0], lookup_address[1], lookup_address[2], lookup_address[3]);
//!    } else if(net_protocol == NET_PROTOCOL_IPv6) {
//!        protocol_type = ETHERTYPE_IPv6;
//!        //fprintf(stderr, "arp: creating request packet for %d.%d.%d.%d\n", lookup_address[0], lookup_address[1], lookup_address[2], lookup_address[3]);
//!        assert(false, "incomplete"); //TODO ipv6 arp
//!    } else {
//!        assert(false, "unknown protocol");
//!    }
//!
//!    return _new_arp_packet(ARP_OPCODE_REQUEST, hardware_type, protocol_type, 
//!                           ndev->hardware_address.ethernet, // source_hardware_address
//!                           my_addr,          // source_protocol_address
//!                           ZERO_MAC,         // dest_hardware_address (zeroed for the ARP request)
//!                           lookup_address,   // dest_protocol_address (the address we're requesting ARP for)
//!                           reslen);
//!}

struct arp_build_packet_info {
    struct arp_packet arp;
    u8*    source_hardware_address;
    u8*    source_protocol_address;
    u8*    dest_hardware_address;
    u8*    dest_protocol_address;
};

static s64 _build_arp_packet(struct net_interface* iface, u8* packet, void* userdata)
{
    unused(iface);

    struct arp_packet* arp = (struct arp_packet*)packet;
    struct arp_build_packet_info* info = (struct arp_build_packet_info*)userdata;

    // copy over protocol type, address sizes, and opcode and adjust endianness
    arp->hardware_address_length = info->arp.hardware_address_length;
    arp->protocol_address_length = info->arp.protocol_address_length;
    arp->hardware_type = htons(info->arp.hardware_type);
    arp->protocol_type = htons(info->arp.protocol_type);
    arp->opcode        = htons(info->arp.opcode);

    // copy addresses, starting with the source_hardware_address
    u8* addresses = arp->addresses;
    memcpy(addresses, info->source_hardware_address, arp->hardware_address_length);
    addresses += arp->hardware_address_length;

    // next is source_protocol_address
    memcpy(addresses, info->source_protocol_address, arp->protocol_address_length);
    addresses += arp->protocol_address_length;

    // next is dest_hardware_address
    memcpy(addresses, info->dest_hardware_address, arp->hardware_address_length);
    addresses += arp->hardware_address_length;

    // last is dest_protocol_address
    memcpy(addresses, info->dest_protocol_address, arp->protocol_address_length);

    return (sizeof(struct arp_packet) + 2 * (arp->hardware_address_length + arp->protocol_address_length));
}

// Create an ARP packet from iface_info and deliver it over ndev
s64 arp_send_reply(struct net_device* ndev, struct net_interface* iface_info, struct net_address* dest_hardware_address, struct net_address* dest_protocol_address)
{
    struct arp_build_packet_info info;
    info.arp.opcode = ARP_OPCODE_REPLY;

    // get the network device the iface is attached to
    struct net_device* iface_ndev = iface_info->net_device;

    // setup the hardware type and address size
    if(iface_ndev->hardware_address.protocol == NET_PROTOCOL_ETHERNET) {
        info.arp.hardware_type = ARP_HARDWARE_TYPE_ETHERNET;
        info.arp.hardware_address_length = 6;
    } else {
        assert(false, "other hardware types aren't supported");
    }

    // setup the protocol type and size
    if(iface_info->protocol == NET_PROTOCOL_IPv4) {
        info.arp.protocol_type = ETHERTYPE_IPv4;
        info.arp.protocol_address_length = 4;

        char buf[16];
        ipv4_format_address(buf, dest_protocol_address->ipv4);
        fprintf(stderr, "arp: creating reply packet to %s\n", buf);
    } else if(iface_info->protocol == NET_PROTOCOL_IPv6) {
        info.arp.protocol_type = ETHERTYPE_IPv6;
        info.arp.protocol_address_length = 6;

        //fprintf(stderr, "arp: creating reply packet for %d.%d.%d.%d\n", dest_protocol_address[0], dest_protocol_address[1], dest_protocol_address[2], dest_protocol_address[3]);
        assert(false, "incomplete"); //TODO ipv6 arp
    }

    u32 arp_packet_size = sizeof(struct arp_packet) + 2 * (info.arp.hardware_address_length + info.arp.protocol_address_length);

    // addresses
    info.source_hardware_address = ndev->hardware_address.mac;
    info.dest_hardware_address   = dest_hardware_address->mac;

    u32 _source_protocol_address = htonl(iface_info->address.ipv4);
    u32 _dest_protocol_address   = htonl(dest_protocol_address->ipv4);

    info.source_protocol_address = (u8*)&_source_protocol_address;
    info.dest_protocol_address   = (u8*)&_dest_protocol_address;

    // ARP is a toplevel layer and interfaces with the hardware device directly
    u16 packet_length;
    u8* packet = ndev->wrap_packet(ndev, null, dest_hardware_address, NET_PROTOCOL_ARP, arp_packet_size, &_build_arp_packet, &info, &packet_length);
    if(packet == null) return errno;

    // Deliver the packet to the hardware
    s64 r = net_send_packet(ndev, packet, packet_length);
    free(packet);
    return r;

//    // setup source_protocol_address
//    u32 source_protocol_address = htonl(iface_info->address.ipv4);
//
//    return _new_arp_packet(ARP_OPCODE_REPLY, hardware_type, protocol_type, 
//                           ndev->hardware_address.mac,  // source_hardware_address
//                           &source_protocol_address,    // source_protocol_address
//                           dest_hardware_address,       // dest_hardware_address
//                           dest_protocol_address,       // dest_protocol_address (the address we're requesting ARP for)
//                           reslen);
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

        // build the destination addresses
        struct net_address _dest_hardware_address, _dest_protocol_address;
        zero(&_dest_hardware_address);
        zero(&_dest_protocol_address);
        _dest_hardware_address.protocol = NET_PROTOCOL_ETHERNET;
        memcpy(_dest_hardware_address.mac, source_hardware_address, 6);
        _dest_protocol_address.protocol = NET_PROTOCOL_IPv4;
        _dest_protocol_address.ipv4 = ntohl(*(u32*)source_protocol_address);

        // someone is looking for our address, tell them
        arp_send_reply(ndev, iface, &_dest_hardware_address, &_dest_protocol_address);
    } else {
        // TODO see if it was a reply to one of our requests
    }
}
