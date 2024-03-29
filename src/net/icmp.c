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
    ICMP_TYPE_UNREACHABLE = 3,
    ICMP_TYPE_ECHO = 8
};

enum ICMP_UNREACHABLE_CODES {
    ICMP_UNREACHABLE_CODE_NET      = 0,
    ICMP_UNREACHABLE_CODE_HOST     = 1,
    ICMP_UNREACHABLE_CODE_PROTOCOL = 2,
    ICMP_UNREACHABLE_CODE_PORT     = 3,
    ICMP_UNREACHABLE_CODE_FRAG     = 4, // fragment needed and Don't Fragment set
    ICMP_UNREACHABLE_CODE_ROUTE    = 5  // source route failed
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
        u16 word = (u16)data[0] << 0;
        sum += word; // add the last byte extended with a 0
    }

    // one's complement of the sum of the data
    return ~sum;
}

struct icmp_build_packet_info {
    u8* payload;
    u16 payload_length;
    u8  type;
    u8  code;
};

static s64 _build_icmp_packet(struct net_send_packet_queue_entry* entry, u8* icmp_packet_start, void* userdata)
{
    unused(entry);

    struct icmp_header* hdr = (struct icmp_header*)icmp_packet_start;
    struct icmp_build_packet_info* info = (struct icmp_build_packet_info*)userdata;
    u16 icmp_packet_size = sizeof(struct icmp_header) + info->payload_length;

    // setup header
    hdr->type     = info->type;
    hdr->code     = info->code;
    hdr->checksum = 0;  // initialize checksum to 0

    // copy payload over
    memcpy(hdr->payload, info->payload, info->payload_length);

    // compute checksum
    hdr->checksum = _compute_checksum(icmp_packet_start, icmp_packet_size);

    return sizeof(struct icmp_header) + info->payload_length;
}

s64 icmp_send_packet(struct net_interface* iface, struct net_address* dest_address, u8 icmp_type, u8 icmp_code, u8* icmp_payload, u16 icmp_payload_length)
{
    u16 icmp_packet_size = sizeof(struct icmp_header) + icmp_payload_length;

    struct icmp_build_packet_info info = { 
        .type           = icmp_type,
        .code           = icmp_code,
        .payload        = icmp_payload, 
        .payload_length = icmp_payload_length
    };

    // ask the network interface for a tx queue slot
    struct net_send_packet_queue_entry* entry;
    s64 ret = net_request_send_packet_queue_entry(iface, null, &entry);
    if(ret < 0) return ret;

    // build a packet with the given info
    if((ret = entry->net_interface->wrap_packet(entry, dest_address, NET_PROTOCOL_ICMP, icmp_packet_size, &_build_icmp_packet, &info)) < 0) return ret;

    // packet is ready for transmission, queue it in the network layer
    net_ready_send_packet_queue_entry(entry);

    return 0;
}

s64 icmp_send_echo(struct net_interface* iface, struct net_address* dest_address, u16 sequence_number)
{
    static u16 next_echo_identifier = 0;

    // build a payload
    u16 icmp_echo_data_length = 56;
    u8  icmp_echo_data[56];
    u8  i = (intp)iface & 0xFF; 
    for(u8 j = 0; j < countof(icmp_echo_data); j++) {
        icmp_echo_data[j] = i++;
    }

    // allocate icmp_echo_header (+data) on the stack
    u16 icmp_echo_length = sizeof(struct icmp_echo) + icmp_echo_data_length;
    struct icmp_echo* icmp_echo = (struct icmp_echo*)__builtin_alloca(icmp_echo_length);

    // only increment next_echo_identifier when sequence_number is 0
    if(sequence_number == 0) __atomic_inc(&next_echo_identifier);

    // setup packet
    icmp_echo->identifier      = htons(next_echo_identifier);
    icmp_echo->sequence_number = htons(sequence_number);
    memcpy(icmp_echo->data, icmp_echo_data, icmp_echo_data_length);

    // send the echo ICMP
    return icmp_send_packet(iface, dest_address, ICMP_TYPE_ECHO, 0, (u8*)icmp_echo, icmp_echo_length);
}

static void _receive_echo(struct net_interface* iface, struct ipv4_header* iphdr, struct net_receive_packet_info* packet_info)
{
    unused(iface);

    // fix up the incoming header
    struct icmp_echo* echo = (struct icmp_echo*)(packet_info->packet + sizeof(struct icmp_header));
    echo->identifier = ntohs(echo->identifier);
    echo->sequence_number = ntohs(echo->sequence_number);

    // determine the length of the echo data
    u16 icmp_echo_data_length = packet_info->packet_length - sizeof(struct icmp_header) - sizeof(struct icmp_echo);

    // print debug message
    char src[16];
    char dest[16];
    ipv4_format_address(src, iphdr->source_address);
    ipv4_format_address(dest, iface->address.ipv4);
    //fprintf(stderr, "icmp: %s got echo from %s: identifier=0x%04X sequence=%d icmp_echo_data_length=%d\n", dest, src, echo->identifier, echo->sequence_number, icmp_echo_data_length);

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
}

static void _receive_echo_reply(struct net_interface* iface, struct ipv4_header* iphdr, struct net_receive_packet_info* packet_info)
{
    unused(iface);

    // fix up the incoming header
    struct icmp_echo* echo = (struct icmp_echo*)(packet_info->packet + sizeof(struct icmp_header));
    echo->identifier       = ntohs(echo->identifier);
    echo->sequence_number  = ntohs(echo->sequence_number);

    // determine the length of the echo data
    u16 icmp_echo_data_length = packet_info->packet_length - sizeof(struct icmp_header) - sizeof(struct icmp_echo);

    // print debug message
    char src[16];
    char dest[16];
    ipv4_format_address(src, iphdr->source_address);
    ipv4_format_address(dest, iface->address.ipv4);
    fprintf(stderr, "icmp: %s got reply from %s: identifier=0x%04X sequence=%d icmp_echo_data_length=%d\n", dest, src, echo->identifier, echo->sequence_number, icmp_echo_data_length);
}

void icmp_receive_packet(struct net_interface* iface, struct ipv4_header* iphdr, struct net_receive_packet_info* packet_info)
{
    struct icmp_header* hdr = (struct icmp_header*)packet_info->packet;

    // fix the checksum
    hdr->checksum = ntohs(hdr->checksum);

    switch(hdr->type) {
    case ICMP_TYPE_ECHO_REPLY:
        _receive_echo_reply(iface, iphdr, packet_info);
        break;

    case ICMP_TYPE_ECHO:
        _receive_echo(iface, iphdr, packet_info);
        break;

    case ICMP_TYPE_UNREACHABLE:
        switch(hdr->code) {
        case ICMP_UNREACHABLE_CODE_NET:
            fprintf(stderr, "icmp: network unreachable\n");
            break;
        case ICMP_UNREACHABLE_CODE_HOST:
            fprintf(stderr, "icmp: host unreachable\n");
            break;
        case ICMP_UNREACHABLE_CODE_PROTOCOL:
            fprintf(stderr, "icmp: protocol unreachable\n");
            break;
        case ICMP_UNREACHABLE_CODE_PORT:
            fprintf(stderr, "icmp: application port unreachable\n");
            break;
        case ICMP_UNREACHABLE_CODE_FRAG:
            fprintf(stderr, "icmp: fragmentation required\n");
            break;
        case ICMP_UNREACHABLE_CODE_ROUTE:
            fprintf(stderr, "icmp: no source route\n");
            break;
        }
        break;

    default:
        fprintf(stderr, "icmp: unknown type=%d, length = %d\n", hdr->type, packet_info->packet_length);
        break;
    }

    // free the packet
    packet_info->free(packet_info);
}
