#include "kernel/common.h"

#include "errno.h"
#include "hashtable.h"
#include "kernel/cpu.h"
#include "kernel/kernel.h"
#include "kernel/smp.h"
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

struct arp_build_packet_info {
    struct arp_packet arp;
    u8*    source_hardware_address;
    u8*    source_protocol_address;
    u8*    dest_hardware_address;
    u8*    dest_protocol_address;
};

// TODO arp tables map a network address, like an IPv4 address, to a MAC receipient who will deliver that packet
// however, we also need to know which local MAC services that remote MAC, so we should build a hardware routing table
// at some point too. maybe not in arp.c, but macmap.c or something. That would map a remote mac to a local mac, and
// the local mac and be used to look up the delivery layer.
struct arp_table_entry {
    struct net_address protocol_address;
    struct net_address hardware_address;

    MAKE_HASH_TABLE;
};

static struct arp_table_entry* global_arp_table = null;
static declare_ticketlock(global_arp_table_lock);

static s64 _build_arp_packet(struct net_send_packet_queue_entry* entry, u8* arp_packet_start, void* userdata)
{
    unused(entry);

    struct arp_packet* arp = (struct arp_packet*)arp_packet_start;
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

// Create and broadcast an ARP request packet
s64 arp_send_request(struct net_interface* iface, struct net_address* lookup_address)
{
    struct arp_build_packet_info info;
    info.arp.opcode = ARP_OPCODE_REQUEST;

    // need the hardware device to build an ARP packet
    struct net_device* ndev = iface->net_device;

    // setup the hardware type and address size
    if(ndev->hardware_address.protocol == NET_PROTOCOL_ETHERNET) {
        info.arp.hardware_type = ARP_HARDWARE_TYPE_ETHERNET;
        info.arp.hardware_address_length = 6;
    } else {
        assert(false, "other hardware types aren't supported");
    }

    // setup the protocol type and size
    if(lookup_address->protocol == NET_PROTOCOL_IPv4) {
        info.arp.protocol_type = ETHERTYPE_IPv4;
        info.arp.protocol_address_length = 4;

        char buf[16];
        ipv4_format_address(buf, lookup_address->ipv4);
        //fprintf(stderr, "arp: creating request packet for %s\n", buf);
    } else if(lookup_address->protocol == NET_PROTOCOL_IPv6) {
        info.arp.protocol_type = ETHERTYPE_IPv6;
        info.arp.protocol_address_length = 6;

        //fprintf(stderr, "arp: creating reply packet for %d.%d.%d.%d\n", dest_protocol_address[0], dest_protocol_address[1], dest_protocol_address[2], dest_protocol_address[3]);
        assert(false, "incomplete"); //TODO ipv6 arp
    }

    u32 arp_packet_size = sizeof(struct arp_packet) + 2 * (info.arp.hardware_address_length + info.arp.protocol_address_length);

    // addresses
    struct net_address zero_address;
    zero(&zero_address);

    info.source_hardware_address = ndev->hardware_address.mac;
    info.dest_hardware_address   = zero_address.mac;

    u32 _source_protocol_address = htonl(iface->address.ipv4);
    u32 _dest_protocol_address   = htonl(lookup_address->ipv4);

    info.source_protocol_address = (u8*)&_source_protocol_address;
    info.dest_protocol_address   = (u8*)&_dest_protocol_address;

    // set the hardware address to broadcast
    struct net_address broadcast_address;
    zero(&broadcast_address);
    broadcast_address.protocol   = NET_PROTOCOL_ETHERNET;
    memset(broadcast_address.mac, 0xFF, 6);

    // ask the network interface for a tx queue slot
    struct net_send_packet_queue_entry* entry;
    s64 ret = net_request_send_packet_queue_entry(iface, null, &entry);
    if(ret < 0) return ret;

    // ARP is a toplevel layer and interfaces with the hardware device directly
    ret = ndev->ops->wrap_packet(ndev, entry, &broadcast_address, NET_PROTOCOL_ARP, arp_packet_size, &_build_arp_packet, &info);
    if(ret < 0) return ret;

    // packet is ready for transmission, queue it in the network layer
    net_ready_send_packet_queue_entry(entry);

    return ret;
}


// Create an ARP packet from iface and deliver it over ndev
s64 arp_send_reply(struct net_device* ndev, struct net_interface* iface, struct net_address* dest_hardware_address, struct net_address* dest_protocol_address)
{
    struct arp_build_packet_info info;
    info.arp.opcode = ARP_OPCODE_REPLY;

    // get the network device the iface is attached to
    struct net_device* iface_ndev = iface->net_device;

    // setup the hardware type and address size
    if(iface_ndev->hardware_address.protocol == NET_PROTOCOL_ETHERNET) {
        info.arp.hardware_type = ARP_HARDWARE_TYPE_ETHERNET;
        info.arp.hardware_address_length = 6;
    } else {
        assert(false, "other hardware types aren't supported");
    }

    // setup the protocol type and size
    if(iface->protocol == NET_PROTOCOL_IPv4) {
        info.arp.protocol_type = ETHERTYPE_IPv4;
        info.arp.protocol_address_length = 4;

        char buf[16];
        ipv4_format_address(buf, dest_protocol_address->ipv4);
        //fprintf(stderr, "arp: creating reply packet to %s\n", buf);
    } else if(iface->protocol == NET_PROTOCOL_IPv6) {
        info.arp.protocol_type = ETHERTYPE_IPv6;
        info.arp.protocol_address_length = 6;

        //fprintf(stderr, "arp: creating reply packet for %d.%d.%d.%d\n", dest_protocol_address[0], dest_protocol_address[1], dest_protocol_address[2], dest_protocol_address[3]);
        assert(false, "incomplete"); //TODO ipv6 arp
    }

    u32 arp_packet_size = sizeof(struct arp_packet) + 2 * (info.arp.hardware_address_length + info.arp.protocol_address_length);

    // addresses
    info.source_hardware_address = ndev->hardware_address.mac;
    info.dest_hardware_address   = dest_hardware_address->mac;

    u32 _source_protocol_address = htonl(iface->address.ipv4);
    u32 _dest_protocol_address   = htonl(dest_protocol_address->ipv4);

    info.source_protocol_address = (u8*)&_source_protocol_address;
    info.dest_protocol_address   = (u8*)&_dest_protocol_address;

    // ask the network interface for a tx queue slot
    struct net_send_packet_queue_entry* entry;
    s64 ret = net_request_send_packet_queue_entry(iface, null, &entry);
    if(ret < 0) return ret;

    // ARP is a toplevel layer and interfaces with the hardware device directly
    ret = ndev->ops->wrap_packet(ndev, entry, dest_hardware_address, NET_PROTOCOL_ARP, arp_packet_size, &_build_arp_packet, &info);
    if(ret < 0) return ret;

    // packet is ready for transmission, queue it in the network layer
    net_ready_send_packet_queue_entry(entry);

    return 0;
}

void arp_handle_device_packet(struct net_receive_packet_info* packet_info)
{
    struct arp_packet* inp = (struct arp_packet*)packet_info->packet;
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
    if(wanted_packet_length > packet_info->packet_length) {
        fprintf(stderr, "arp: incoming packet of size %d incorrect (wanted %d)\n", packet_info->packet_length, wanted_packet_length);
        return;
    }

    // set up address pointers
    u8* source_hardware_address = inp->addresses;
    u8* source_protocol_address = source_hardware_address + inp->hardware_address_length;
    u8* dest_hardware_address   = source_protocol_address + inp->protocol_address_length;
    u8* dest_protocol_address   = dest_hardware_address   + inp->hardware_address_length;

    // set up address structs
    struct net_address _source_hardware_address;
    struct net_address _source_protocol_address;
    zero(&_source_hardware_address);
    zero(&_source_protocol_address);
    _source_hardware_address.protocol = NET_PROTOCOL_ETHERNET;
    _source_protocol_address.protocol = NET_PROTOCOL_IPv4;
    memcpy(_source_hardware_address.mac, source_hardware_address, 6);
    _source_protocol_address.ipv4 = ntohl(*(u32*)source_protocol_address);

    if(opcode == ARP_OPCODE_REQUEST) {
        // TODO an incoming request also declares a mac address to device mapping, so add that to the global arp table

        acquire_lock(global_arp_table_lock);

        // first look up if we already have the address in our table
        struct arp_table_entry* arpent = null;
        HT_FIND(global_arp_table, _source_protocol_address, arpent);
        if(arpent != null) {
            //fprintf(stderr, "arp: entry already in table, updating\n");
            memcpy(&arpent->hardware_address, &_source_hardware_address, sizeof(struct net_address));
        } else {
            arpent = (struct arp_table_entry*)malloc(sizeof(struct arp_table_entry));
            memcpy(&arpent->protocol_address, &_source_protocol_address, sizeof(struct net_address));
            memcpy(&arpent->hardware_address, &_source_hardware_address, sizeof(struct net_address));
            //fprintf(stderr, "arp: new entry added to table\n");
            HT_ADD(global_arp_table, protocol_address, arpent);
        }

        release_lock(global_arp_table_lock);
        // the incoming response forms an IPv4 address, so let's see if we have an interface for that device
        struct net_address search_address;
        zero(&search_address);
        search_address.protocol = NET_PROTOCOL_IPv4;
        search_address.ipv4 = ntohl(*(u32*)dest_protocol_address);

        struct net_interface* iface = net_device_find_interface(packet_info->net_device, &search_address); // TODO search all interfaces, not just ones on this device?
        if(iface == null) {
            char buf[16];
            ipv4_format_address(buf, search_address.ipv4);
            //fprintf(stderr, "arp: no device found for IP address %s\n", buf);
            return;
        }

        char buf[16];
        ipv4_format_address(buf, search_address.ipv4);
        //fprintf(stderr, "arp: request received for %s, sending response\n", buf);

        // TODO might we ever need to route the response to a different network device? if so, we'll need to route 
        // based on the source_protocol_address, perhaps.

        // send the response to the source address
        arp_send_reply(packet_info->net_device, iface, &_source_hardware_address, &_source_protocol_address);
    } else if(opcode == ARP_OPCODE_REPLY) {
        // TODO see if it was a reply to one of our requests
        char buf[16];
        ipv4_format_address(buf, _source_protocol_address.ipv4);
        //fprintf(stderr, "arp: got response for %s: %02x:%02x:%02x:%02x:%02x:%02x\n", buf,
        //        _source_hardware_address.mac[0], _source_hardware_address.mac[1], _source_hardware_address.mac[2],
        //        _source_hardware_address.mac[3], _source_hardware_address.mac[4], _source_hardware_address.mac[5]);

        acquire_lock(global_arp_table_lock);

        // first look up if we already have the address in our table
        struct arp_table_entry* arpent = null;
        HT_FIND(global_arp_table, _source_protocol_address, arpent);
        if(arpent != null) {
            //fprintf(stderr, "arp: entry already in table, updating\n");
            memcpy(&arpent->hardware_address, &_source_hardware_address, sizeof(struct net_address));
        } else {
            arpent = (struct arp_table_entry*)malloc(sizeof(struct arp_table_entry));
            memcpy(&arpent->protocol_address, &_source_protocol_address, sizeof(struct net_address));
            memcpy(&arpent->hardware_address, &_source_hardware_address, sizeof(struct net_address));
            //fprintf(stderr, "arp: new entry added to table\n");
            HT_ADD(global_arp_table, protocol_address, arpent);
        }

        release_lock(global_arp_table_lock);
    }

    // free the packet
    packet_info->free(packet_info);
}

s64 arp_lookup(struct net_address* protocol_address, struct net_address* hardware_address)
{
    struct arp_table_entry* arpent = null;
    s64 ret = 0;

    acquire_lock(global_arp_table_lock);

    HT_FIND(global_arp_table, *protocol_address, arpent);
    if(arpent == null) {
        ret = -ENOENT;
        goto done;
    }

    memcpy(hardware_address, &arpent->hardware_address, sizeof(struct net_address));

done:
    release_lock(global_arp_table_lock);
    return ret;
}
