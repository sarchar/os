#include "kernel/common.h"

#include "kernel/cpu.h"
#include "kernel/kernel.h"
#include "net/ethernet.h"
#include "net/icmp.h"
#include "net/ipv4.h"
#include "net/net.h"
#include "net/udp.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

struct udp_header {
    u16 source_port;
    u16 dest_port;
    u16 length;
    u16 checksum;
    u8  payload[];
} __packed;

static u16 _compute_checksum(u32 source_address, u32 dest_address, u8* data, u16 data_length)
{
    u32 sum = 0;

    u32 src = htonl(source_address);
    u32 dst = htonl(dest_address);

    sum += (*((u8*)&src + 1) << 8) | (*((u8*)&src + 0) << 0);
    sum += (*((u8*)&src + 3) << 8) | (*((u8*)&src + 2) << 0);
    sum += (*((u8*)&dst + 1) << 8) | (*((u8*)&dst + 0) << 0);
    sum += (*((u8*)&dst + 3) << 8) | (*((u8*)&dst + 2) << 0);
    sum += ((u16)IPv4_PROTOCOL_UDP << 8);
    sum += htons(data_length);

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
    if(sum == 0xFFFF) return 0xFFFF;
    return ~sum;
}

struct udp_build_packet_info {
    u32 dest_address;
    u16 source_port;
    u16 dest_port;
    u8* payload;
    u16 payload_length;
};

static s64 _build_udp_packet(struct net_send_packet_queue_entry* entry, u8* udp_packet_start, void* userdata)
{
    struct udp_header* hdr = (struct udp_header*)udp_packet_start;
    struct udp_build_packet_info* info = (struct udp_build_packet_info*)userdata;
    u16 udp_packet_size = sizeof(struct udp_header) + info->payload_length;

    // setup header
    hdr->source_port = htons(info->source_port);
    hdr->dest_port   = htons(info->dest_port);
    hdr->length      = htons(udp_packet_size);
    hdr->checksum    = 0;  // initialize checksum to 0

    // copy payload over
    memcpy(hdr->payload, info->payload, info->payload_length);

    // compute checksum and convert to network byte order
    hdr->checksum = _compute_checksum(entry->net_interface->address.ipv4, info->dest_address, udp_packet_start, udp_packet_size);

    return udp_packet_size;
}

// TODO convert to use a udp_socket, similar to tcp_socket
s64 udp_send_packet(struct net_interface* iface, struct net_address* dest_address, u16 source_port, u16 dest_port, u8* udp_payload, u16 udp_payload_length)
{
    u16 udp_packet_size = sizeof(struct udp_header) + udp_payload_length;

    struct udp_build_packet_info info = { 
        .dest_address   = dest_address->ipv4,
        .source_port    = source_port,
        .dest_port      = dest_port,
        .payload        = udp_payload, 
        .payload_length = udp_payload_length
    };

    // ask the network interface for a tx queue slot
    struct net_send_packet_queue_entry* entry;
    s64 ret = net_request_send_packet_queue_entry(iface, null, &entry);
    if(ret < 0) return ret;

    // build a packet with the given info
    if((ret = entry->net_interface->wrap_packet(entry, dest_address, NET_PROTOCOL_UDP, udp_packet_size, &_build_udp_packet, &info)) < 0) return ret;

    // packet is ready for transmission, queue it in the network layer
    net_ready_send_packet_queue_entry(entry);

    return 0;
}

void udp_receive_packet(struct net_interface* iface, struct ipv4_header* iphdr, struct net_receive_packet_info* packet_info)
{
    unused(iface);

    struct udp_header* hdr = (struct udp_header*)packet_info->packet;

    if(packet_info->packet_length < sizeof(struct udp_header)) {
        fprintf(stderr, "udp: dropping packet (size too small = %d)\n", packet_info->packet_length);
        return;
    }

    // immediately validate the checksum on the packet, if it's set
    u16 length = min(ntohs(hdr->length), packet_info->packet_length); // actual available data needs to be no greater than the actual packet_length

    if(hdr->checksum) {
        u16 header_checksum = hdr->checksum; // clear to zero for computing the checksum
        hdr->checksum = 0;

        u16 computed_checksum;
        if(header_checksum != (computed_checksum = _compute_checksum(iphdr->source_address, iphdr->dest_address, packet_info->packet, length))) {
            fprintf(stderr, "udp: checksum error, dropping packet (computed 0x%04X, header says 0x%04X)\n", computed_checksum, header_checksum);
            return;
        }

        hdr->checksum = header_checksum;
    }

    // fix the endianness of the header
    hdr->source_port = ntohs(hdr->source_port);
    hdr->dest_port   = ntohs(hdr->dest_port);
    hdr->length      = ntohs(hdr->length);
    hdr->checksum    = ntohs(hdr->checksum);

    char buf[16], buf2[16];
    ipv4_format_address(buf, iphdr->source_address);
    ipv4_format_address(buf2, iphdr->dest_address);
    fprintf(stderr, "udp: got packet from %s:%d to %s:%d length=%d checksum=0x%04X\n",
            buf, hdr->source_port, buf2, hdr->dest_port, hdr->length, hdr->checksum);

    // todo: verify checksum before passing data onwards

    // free the packet memory
    packet_info->free(packet_info);
}
