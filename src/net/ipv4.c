#include "kernel/common.h"

#include "errno.h"
#include "kernel/cpu.h"
#include "kernel/kernel.h"
#include "kernel/paging.h"
#include "net/arp.h"
#include "net/ethernet.h"
#include "net/icmp.h"
#include "net/ipv4.h"
#include "net/net.h"
#include "net/tcp.h"
#include "net/udp.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

struct ipv4_build_packet_info {
    struct net_address*          dest_address;
    u16                          identification;
    u8                           ipv4_protocol;
    net_wrap_packet_callback*    build_payload;
    void*                        payload_userdata;
};

// current gateway
static char const* gateway_ip_address = "192.168.53.1"; //TODO use DHCP

static void _ipv4_interface_receive_packet(struct net_interface* iface, struct net_receive_packet_info*);
static void _ipv4_header_to_host(struct ipv4_header* hdr);
static void _ipv4_header_to_network(struct ipv4_header* hdr);

// identical to the ICMP checksum, how coincidental
static u16 _ipv4_compute_checksum(u8* data, u16 data_length)
{
    u32 sum = 0;

    while(data_length > 1) {
        u16 word = ((u16)data[0] << 0) | ((u16)data[1] << 8);
        sum += word;
        data += 2;
        data_length -= 2;

        while(sum > 0xFFFF) {
            u16 high = sum >> 16;
            sum &= 0xFFFF;
            sum += high;
        }
    }

    if(data_length) {
        u16 word = (u16)data[0] << 0;
        sum += word; // add the last byte extended with a 0
    }

    // one's complement of the sum of the data
    return ~sum;
}

static inline bool _is_broadcast(struct net_address* address)
{
    if(address->protocol != NET_PROTOCOL_IPv4) return false;

    if((address->ipv4 & 0x000000FF) == 0x000000FF) return true;
    if((address->ipv4 & 0x0000FFFF) == 0x0000FFFF) return true;
    if((address->ipv4 & 0x00FFFFFF) == 0x00FFFFFF) return true;
    if((address->ipv4 & 0xFFFFFFFF) == 0xFFFFFFFF) return true;

    return false;
}

static s64 _build_ipv4_packet(struct net_send_packet_queue_entry* entry, u8* ipv4_packet_start, void* userdata)
{
    struct net_interface* iface = entry->net_interface;
    struct ipv4_header* hdr = (struct ipv4_header*)ipv4_packet_start;
    struct ipv4_build_packet_info* info = (struct ipv4_build_packet_info*)userdata;

    // some layers need the payload built in order to compute things like checksum, but ipv4 doesn't.
    // however, to stay somewhat consistent across the code, we'll build the packets inner layers first
    s64 payload_length = info->build_payload(entry, ipv4_packet_start + sizeof(struct ipv4_header), info->payload_userdata);
    if(payload_length < 0) return payload_length;
    u64 total_length = payload_length + sizeof(struct ipv4_header);

    // construct the IPv4 header
    hdr->header_length   = sizeof(struct ipv4_header) / 4;
    hdr->version         = 4;
    hdr->type_of_service = 0;    // normal delivery
    hdr->total_length    = total_length;
    hdr->identification  = info->identification;
    hdr->fragment_offset = 0;
    hdr->flags           = IPv4_HEADER_FLAG_DONT_FRAGMENT | IPv4_HEADER_FLAG_LAST_FRAGMENT;
    hdr->time_to_live    = 64;
    hdr->protocol        = info->ipv4_protocol;
    hdr->header_checksum = 0;    // start at 0 to compute the checksum
    hdr->source_address  = iface->address.ipv4;
    hdr->dest_address    = info->dest_address->ipv4;

    // and fix up the header's endianness
    _ipv4_header_to_network(hdr);

    // compute the checksum of the header before fixing endianness
    hdr->header_checksum = _ipv4_compute_checksum((u8*)hdr, hdr->header_length * 4);

    return total_length;
}

s64 ipv4_wrap_packet(struct net_send_packet_queue_entry* sq_entry, struct net_address* dest_address, u8 payload_protocol, u16 payload_size, net_wrap_packet_callback* build_payload, void* userdata)
{
    static u16 identification = 0;

    struct net_interface* iface = sq_entry->net_interface;
    assert(iface->protocol == NET_PROTOCOL_IPv4, "can only call ipv4_wrap_packet on IPv4 network interfaces");

    u8 ipv4_protocol;
    if(payload_protocol == NET_PROTOCOL_ICMP) ipv4_protocol = IPv4_PROTOCOL_ICMP;
    else if(payload_protocol == NET_PROTOCOL_TCP) ipv4_protocol = IPv4_PROTOCOL_TCP;
    else if(payload_protocol == NET_PROTOCOL_UDP) ipv4_protocol = IPv4_PROTOCOL_UDP;
    else {
        assert(false, "unsupported protocol");
        return -ENOTSUP;
    }

    struct ipv4_build_packet_info info = {
        .dest_address     = dest_address,
        .identification   = __atomic_xinc(&identification),
        .ipv4_protocol    = ipv4_protocol,
        .build_payload    = build_payload,
        .payload_userdata = userdata
    };

    u16 packet_size = sizeof(struct ipv4_header) + payload_size;

    // IPv4 is a toplevel layer, so it calls directly into the network device for packet memory
    struct net_address hw_dest;

    // If we know how to deliver to dest_address directly via ethernet, use it. otherwise, look up the gateway and use that
    s64 err;
    if(_is_broadcast(dest_address)) {
        // with a broadcast address we want to send to all MACs on the switch
        hw_dest.protocol = NET_PROTOCOL_ETHERNET;
        memset(hw_dest.mac, 0xFF, 6);
    } else if((err = arp_lookup(dest_address, &hw_dest)) < 0 && err == -ENOENT) {
        struct net_address gateway_address;
        ipv4_parse_address_string(&gateway_address, gateway_ip_address);
        if((err = arp_lookup(&gateway_address, &hw_dest)) < 0) {
            // TODO with no hardware address available, we should send out an ARP right here and 
            // then sleep the task waiting on a response

            char buf[16];
            ipv4_format_address(buf, gateway_address.ipv4);
            fprintf(stderr, "ip: no hardware address available for %s..dropping packet\n", buf);
        }
    }

    if(err < 0) return err;

    return iface->net_device->ops->wrap_packet(iface->net_device, sq_entry, &hw_dest, NET_PROTOCOL_IPv4, packet_size, &_build_ipv4_packet, &info);
}

void ipv4_parse_address_string(struct net_address* addr, char const* buf)
{
    zero(addr);

    u8 octets[4];
//TOOD #ifdef LITTLE_ENDIAN ...
    sscanf(buf, "%hhu.%hhu.%hhu.%hhu", &octets[3], &octets[2], &octets[1], &octets[0]);

    addr->protocol = NET_PROTOCOL_IPv4;
    addr->ipv4 = (octets[3] << 24) | (octets[2] << 16) | (octets[1] << 8) | (octets[0] << 0);
}

struct net_interface* ipv4_create_interface(struct net_address* local_address)
{
    struct ipv4_interface* iface = (struct ipv4_interface*)malloc(sizeof(struct ipv4_interface));
    zero(iface);

    assert(local_address->protocol == NET_PROTOCOL_IPv4, "need an IPv4 address for IPv4 interfaces");

    iface->net_interface.address        = *local_address;
    iface->net_interface.protocol       = NET_PROTOCOL_IPv4;
    iface->net_interface.receive_packet = &_ipv4_interface_receive_packet;
    iface->net_interface.wrap_packet    = &ipv4_wrap_packet;

    return &iface->net_interface;
}

void ipv4_set_gateway(struct net_interface* net_interface, struct net_address* addr)
{
    struct ipv4_interface* iface = containerof(net_interface, struct ipv4_interface, net_interface);
    iface->gateway_address = *addr;
}

// this and _ipv4_header_to_network is identical, but are separate for readability
static void _ipv4_header_to_host(struct ipv4_header* hdr)
{
    hdr->total_length              = ntohs(hdr->total_length);
    hdr->identification            = ntohs(hdr->identification);
    hdr->fragment_offset_and_flags = ntohs(hdr->fragment_offset_and_flags);
    hdr->header_checksum           = ntohs(hdr->header_checksum);
    hdr->source_address            = ntohl(hdr->source_address);
    hdr->dest_address              = ntohl(hdr->dest_address);

    //fprintf(stderr, "version=%d header_length=%d type_of_service=0x%02X total_length=%d\n", hdr->version, hdr->header_length, hdr->type_of_service, hdr->total_length);
    //fprintf(stderr, "identification=0x%04X flags=0x%02X fragment_offset=%d\n", hdr->identification, hdr->flags, hdr->fragment_offset);
    //fprintf(stderr, "time_to_live=%d protocol=0x%02X header_checksum=0x%04X\n", hdr->time_to_live, hdr->protocol, hdr->header_checksum);
    //fprintf(stderr, "source_address=0x%08X dest_address=0x%08X\n", hdr->source_address, hdr->dest_address);
}

static void _ipv4_header_to_network(struct ipv4_header* hdr)
{
    hdr->total_length              = htons(hdr->total_length);
    hdr->identification            = htons(hdr->identification);
    hdr->fragment_offset_and_flags = htons(hdr->fragment_offset_and_flags);
    hdr->header_checksum           = htons(hdr->header_checksum);
    hdr->source_address            = htonl(hdr->source_address);
    hdr->dest_address              = htonl(hdr->dest_address);
}

void ipv4_handle_device_packet(struct net_receive_packet_info* packet_info)
{
    // immediately return if our packet is too short
    if(packet_info->packet_length < 20) return;

    // get the IP header
    struct ipv4_header* hdr = (struct ipv4_header*)packet_info->packet;
    _ipv4_header_to_host(hdr);

    char buf1[16], buf2[16];
    ipv4_format_address(buf1, hdr->source_address);
    ipv4_format_address(buf2, hdr->dest_address);
    //fprintf(stderr, "ip: got packet 0x%04X protocol %d from %s to %s size %d\n", hdr->identification, hdr->protocol, buf1, buf2, packet_info->packet_length);

    // look up the net_interface that handles packets addressed to hdr->dest_address
    // if no interface is found, we can ignore the packet. otherwise, deliver the packet to that interface
    struct net_address search_address;
    zero(&search_address);
    search_address.protocol = NET_PROTOCOL_IPv4;
    search_address.ipv4 = hdr->dest_address;

    // look up the device, else drop the packet if it's not bound for our interface on this device
    struct net_interface* iface = net_device_find_interface(packet_info->net_device, &search_address);
    if(iface == null) {
        // no interface was found, but if we have an unbound interface that's accepting all data packets,
        // we can deliver to that interface instead
        search_address.ipv4 = 0;
        iface = net_device_find_interface(packet_info->net_device, &search_address);
        if(iface != null && !iface->accept_all) iface = null;
    }

    if(iface != null) {
        assert(iface->protocol == NET_PROTOCOL_IPv4, "must be");
        iface->receive_packet(iface, packet_info);
    }
}

static void _ipv4_interface_receive_packet(struct net_interface* iface, struct net_receive_packet_info* packet_info)
{
    // get the IP header (already endian fixed)
    struct ipv4_header* hdr = (struct ipv4_header*)packet_info->packet;

    // determine payload
    u8* payload = packet_info->packet + ((u16)hdr->header_length * 4);
    u16 min_length = min(hdr->total_length, packet_info->packet_length);
    u16 payload_length = min_length - ((u16)hdr->header_length * 4);
    if(hdr->total_length > packet_info->packet_length || payload_length >= min_length) { // payload_length will overflow if hdr->header_length is invalid
        fprintf(stderr, "ip: dropping packet 0x%04X due to invalid size (hdr->total_length=%d, packet_length=%d, payload_length=%d)\n", hdr->identification, hdr->total_length, packet_info->packet_length, payload_length);
        return;
    }

    // update the pointers for the next layer
    packet_info->packet        = payload;
    packet_info->packet_length = payload_length;

    //TODO hash table the hdr->protocol and call the handler that way?
    switch(hdr->protocol) {
    case IPv4_PROTOCOL_ICMP:
        icmp_receive_packet(iface, hdr, packet_info);
        break;

    case IPv4_PROTOCOL_TCP:
        tcp_receive_packet(iface, hdr, packet_info);
        break;

    case IPv4_PROTOCOL_UDP:
        udp_receive_packet(iface, hdr, packet_info);
        break;

    default:
        // unknown packet, free memory and return
        fprintf(stderr, "ip: unknown or unsupported IPv4 protocol %d\n", hdr->protocol);
        packet_info->free(packet_info);
        break;
    }
}

void ipv4_format_address(char* buf, u32 address)
{
    sprintf(buf, "%d.%d.%d.%d", (address >> 24) & 0xFF, (address >> 16) & 0xFF, (address >> 8) & 0xFF, (address >> 0) & 0xFF);
}


