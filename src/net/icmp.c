#include "kernel/common.h"

#include "kernel/cpu.h"
#include "kernel/kernel.h"
#include "net/ethernet.h"
#include "net/icmp.h"
#include "net/ipv4.h"
#include "net/net.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

enum ICMP_TYPE {
    ICMP_TYPE_ECHO_REPLY = 0,
    ICMP_TYPE_ECHO = 8
};

struct icmp_header {
    u8  type;
    u8  code;
    u16 checksum;
    u8  payload[];
} __packed;

struct icmp_echo {
    u16 identifier;
    u16 sequence_number;
    u8  data[];
} __packed;

static u16 _compute_checksum(u8* data, u16 data_length)
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
        u16 word = (u16)data[0] << 8;
        sum += word; // add the last byte extended with a 0
    }

    // one's complement of the sum of the data
    return ~sum;
}

//u8* icmp_create_packet(u32 dest_address, u8 type, u8 code, u16 icmp_data_length, u8* icmp_packet_start, u16* packet_size)
//{
//    u16 icmp_packet_size = sizeof(struct icmp_header) + icmp_data_length;
//    u8* packet_start = ipv4_create_packet(dest_address, IPv4_PROTOCOL_ICMP, icmp_packet_size, &icmp_packet_start, packet_size); // TODO should this be net_interface->create_packet()?
//    
//    if(packet_start != null) {
//        struct icmp_header* icmp_header = (struct icmp_header*)icmp_packet_start;
//        icmp_header->type     = type;
//        icmp_header->code     = code;
//        icmp_header->checksum = 0;
//    }
//
//    return packet_start;
//}

struct icmp_build_packet_info {
    u8* payload;
    u16 payload_length;
    u8  type;
    u8  code;
};

static s64 _build_icmp_packet(struct net_interface* iface, u8* icmp_packet_start, void* userdata)
{
    unused(iface);

    struct icmp_header* hdr = (struct icmp_header*)icmp_packet_start;
    struct icmp_build_packet_info* info = (struct icmp_build_packet_info*)userdata;
    u16 icmp_packet_size = sizeof(struct icmp_header) + info->payload_length;

    // setup header
    hdr->type     = info->type;
    hdr->code     = info->code;
    hdr->checksum = 0;  // initialize checksum to 0

    // copy payload over
    memcpy(hdr->payload, info->payload, info->payload_length);

    // compute checksum and convert to network byte order
    hdr->checksum = _compute_checksum(icmp_packet_start, icmp_packet_size);

    return sizeof(struct icmp_header) + info->payload_length;
}

//TODO maybe an icmp_wrap_packet would call a callback to load in the payload
// and icmp_send_packet would make use of wrap_packet
s64 icmp_send_packet(struct net_interface* iface, struct net_address* dest_address, u8 icmp_type, u8 icmp_code, u8* icmp_payload, u16 icmp_payload_length)
{
    u16 icmp_packet_size = sizeof(struct icmp_header) + icmp_payload_length;

    struct icmp_build_packet_info info = { 
        .type           = icmp_type,
        .code           = icmp_code,
        .payload        = icmp_payload, 
        .payload_length = icmp_payload_length
    };

    u16 packet_length;
    u8* packet_start = iface->wrap_packet(iface, dest_address, NET_PROTOCOL_ICMP, icmp_packet_size, &_build_icmp_packet, &info, &packet_length);
    if(packet_start == null) return errno;

    return net_send_packet(iface->net_device, packet_start, packet_length);
}

static void _handle_echo(struct net_interface* iface, struct ipv4_header* iphdr, u8* packet, u16 packet_length)
{
    unused(iface);

    // fix up the incoming header
    struct icmp_echo* echo = (struct icmp_echo*)(packet + sizeof(struct icmp_header));
    echo->identifier = ntohs(echo->identifier);
    echo->sequence_number = ntohs(echo->sequence_number);

    // determine the length of the echo data
    u16 icmp_echo_data_length = packet_length - sizeof(struct icmp_header) - sizeof(struct icmp_echo);

    // print debug message
    char src[16];
    char dest[16];
    ipv4_format_address(src, iphdr->source_address);
    ipv4_format_address(dest, iface->address.ipv4);
    fprintf(stderr, "icmp: %s got echo from %s: identifier=0x%04X sequence=%d icmp_echo_data_length=%d\n", dest, src, echo->identifier, echo->sequence_number, icmp_echo_data_length);

    // build the reply address
    struct net_address reply_address;
    zero(&reply_address);
    reply_address.protocol = NET_PROTOCOL_IPv4;
    reply_address.ipv4     = iphdr->source_address;

    // allocate icmp_echo_header (+data) on the stack
    u16 icmp_echo_reply_length = sizeof(struct icmp_echo) + icmp_echo_data_length;
    struct icmp_echo* icmp_echo_reply = (struct icmp_echo*)__builtin_alloca(icmp_echo_reply_length);

    // setup packet
    icmp_echo_reply->identifier      = htons(echo->identifier);
    icmp_echo_reply->sequence_number = htons(echo->sequence_number);
    memcpy(icmp_echo_reply->data, echo->data, icmp_echo_data_length);

    // send the reply ICMP
    icmp_send_packet(iface, &reply_address, ICMP_TYPE_ECHO_REPLY, 0, (u8*)icmp_echo_reply, icmp_echo_reply_length);

//    // create an echo reply packet
//    // TODO make packet creation some kind of recursive callback system? some layers need the data to create checksums
//    u16 packet_size;
//    u8* icmp_packet_start;
//    u8* packet_start = icmp_create_packet(iphdr->source_address, ICMP_TYPE_ECHO_REPLY, 0, sizeof(struct icmp_echo) + icmp_echo_data_length, &icmp_packet_start, &packet_size);
//
//    // fill in the packet data
//    struct icmp_header* icmp_reply_header = (struct icmp_header*)icmp_packet_start;
//    struct icmp_echo* echo_reply = (struct icmp_echo*)icmp_reply_header->payload;
//    echo_reply->identifier       = echo->identifier;
//    echo_reply->sequence_number  = echo->sequence_number;
//    memcpy(echo_reply->data, echo->data, icmp_echo_data_length);
//
//    // compute checksum
//    u16 checksum = _compute_checksum(icmp_reply_header, sizeof(struct icmp_header) + sizeof(struct icmp_echo) + icmp_echo_data_length);
//
//    // fixup the echo packet
//    icmp_reply_header->checksum = htons(checksum);
//    echo_reply->identifier      = htons(echo_reply->identifier);
//    echo_reply->sequence_number = htons(echo_reply->sequence_number);
//
//    // send the packet
//    icmp_send_packet(ndev, iphdr->source_address, packet_start, packet_size);
}

void icmp_receive_packet(struct net_interface* iface, struct ipv4_header* iphdr, u8* packet, u16 packet_length)
{
    struct icmp_header* hdr = (struct icmp_header*)packet;

    // fix the checksum
    hdr->checksum = ntohs(hdr->checksum);

    switch(hdr->type) {
    case ICMP_TYPE_ECHO:
        _handle_echo(iface, iphdr, packet, packet_length);
        break;

    default:
        fprintf(stderr, "icmp: unknown type=%d, length = %d\n", hdr->type, packet_length);
        break;
    }
}
