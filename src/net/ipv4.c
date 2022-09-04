#include "kernel/common.h"

#include "kernel/cpu.h"
#include "kernel/kernel.h"
#include "net/ethernet.h"
#include "net/ipv4.h"
#include "net/net.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

struct ipv4_header {
    u8  header_length   : 4;     // lower 4 bits of first header byte
    u8  version         : 4;     // upper 4 bits of first header byte
    u8  type_of_service;         // entire second header byte
    u16 total_length;            // third and fourth bytes must be converted from network
    u16 identification;          // fifth and sixth bytes must be converted from network
    union {                      // 7th and 8th bytes must be converted from network
        u16 fragment_offset_and_flags;
        struct {
            u16 fragment_offset : 13;
            u16 flags           : 3;
        };
    };
    u8  time_to_live;            // 9th byte of the header
    u8  protocol;                // 10th byte of the header
    u16 header_checksum;         // 11th and 12th bytes must be converted from network
    u32 source_address;          // bytes 13-16 need to be converted from network
    u32 destination_address;     // bytes 17-20 need to be converted from network
    u8  options[];
};

// current gateway
u8 gateway_mac_address[] = { 0x00, 0x15, 0x5d, 0xa2, 0x97, 0x22 }; // TODO use ARP to determine the gateway's ethernet address
u8 gateway_ip_address[]  = { 172, 21, 160, 1 };

static void _fixup_ipv4_header(struct ipv4_header* hdr)
{
    hdr->total_length              = ntohs(hdr->total_length);
    hdr->identification            = ntohs(hdr->identification);
    hdr->fragment_offset_and_flags = ntohs(hdr->fragment_offset_and_flags);
    hdr->header_checksum           = ntohs(hdr->header_checksum);
    hdr->source_address            = ntohl(hdr->source_address);
    hdr->destination_address       = ntohl(hdr->destination_address);

    //fprintf(stderr, "version=%d header_length=%d type_of_service=0x%02X total_length=%d\n", hdr->version, hdr->header_length, hdr->type_of_service, hdr->total_length);
    //fprintf(stderr, "identification=0x%04X flags=0x%02X fragment_offset=%d\n", hdr->identification, hdr->flags, hdr->fragment_offset);
    //fprintf(stderr, "time_to_live=%d protocol=0x%02X header_checksum=0x%04X\n", hdr->time_to_live, hdr->protocol, hdr->header_checksum);
    //fprintf(stderr, "source_address=0x%08X destination_address=0x%08X\n", hdr->source_address, hdr->destination_address);
}

static void _format_address(char* buf, u32 address)
{
    sprintf(buf, "%d.%d.%d.%d", (address >> 24) & 0xFF, (address >> 16) & 0xFF, (address >> 8) & 0xFF, (address >> 0) & 0xFF);
}

void ipv4_receive_packet(struct net_device* ndev, u8* packet, u16 packet_length)
{
    // immediately return if our packet is too short
    if(packet_length < 20) return;

    // get the IP header
    struct ipv4_header* hdr = (struct ipv4_header*)packet;
    _fixup_ipv4_header(hdr);

    char buf1[16], buf2[16];
    _format_address(buf1, hdr->source_address);
    _format_address(buf2, hdr->destination_address);
    fprintf(stderr, "ip: got packet 0x%04X protocol %d from %s to %s\n", hdr->identification, hdr->protocol, buf1, buf2);
    u16 payload_length = packet_length - ((u16)hdr->header_length * 4);
    if(hdr->total_length != packet_length || payload_length >= packet_length) { // payload_length will overflow if hdr->header_length is invalid
        fprintf(stderr, "ip: dropping packet 0x%04X due to invalid size\n", hdr->identification);
        return;
    }

    // pass the payload to the proper protocol handler
    u8* payload = packet + ((u16)hdr->header_length * 4);

    switch(hdr->protocol) {
    case IPv4_PROTOCOL_ICMP:
        fprintf(stderr, "ip: got icmp packet, payload size = %d\n", payload_length);
        break;

    default:
        // unknown packet
        break;
    }
}
