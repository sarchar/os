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
    u16  sum = 0;
    u16* worddata = (u16*)data;

    while(data_length > 1) {
        sum += *worddata;
        worddata++;
        data_length -= 2;
    }

    if(data_length) sum += (u16)*(u8*)worddata; // add the last byte extended with a 0

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

static void _handle_echo(struct net_device* ndev, struct ipv4_header* iphdr, u8* packet, u16 packet_length)
{
    // fix up the incoming header
    struct icmp_echo* echo = (struct icmp_echo*)(packet + sizeof(struct icmp_header));
    echo->identifier = ntohs(echo->identifier);
    echo->sequence_number = ntohs(echo->sequence_number);

    // determine the length of the echo data
    u16 icmp_echo_data_length = packet_length - sizeof(struct icmp_header) - sizeof(struct icmp_echo);

    char src[16];
    ipv4_format_address(src, iphdr->source_address);
    fprintf(stderr, "icmp: got echo from %s: identifier=0x%04X sequence=%d icmp_echo_data_length=%d\n", src, echo->identifier, echo->sequence_number, icmp_echo_data_length);

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

void icmp_receive_packet(struct net_device* ndev, struct ipv4_header* iphdr, u8* packet, u16 packet_length)
{
    struct icmp_header* hdr = (struct icmp_header*)packet;

    // fix the checksum
    hdr->checksum = ntohs(hdr->checksum);

    switch(hdr->type) {
    case ICMP_TYPE_ECHO:
        _handle_echo(ndev, iphdr, packet, packet_length);
        break;

    default:
        fprintf(stderr, "icmp: unknown type=%d, length = %d\n", hdr->type, packet_length);
        break;
    }
}
