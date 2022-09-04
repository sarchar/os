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

// current gateway
u8 gateway_mac_address[] = { 0x00, 0x15, 0x5d, 0xa2, 0x97, 0x22 }; // TODO use ARP to determine the gateway's ethernet address
u8 gateway_ip_address[]  = { 172, 21, 160, 1 };

//u8* ipv4_create_packet(u32 dest_address, u8 ipv4_protocol, u16 payload_size, u8* payload_start, u8* packet_size)
//{
//    // IPv4 packets are passed through to the net device, so no higher layers exist
//    // TODO would be nice to 'claim' a memory location from the network device driver, since it 
//    // has outgoing data storage
//}

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

void ipv4_handle_device_packet(struct net_device* ndev, u8* packet, u16 packet_length)
{
    // immediately return if our packet is too short
    if(packet_length < 20) return;

    // get the IP header
    struct ipv4_header* hdr = (struct ipv4_header*)packet;
    _fixup_ipv4_header(hdr);

    char buf1[16], buf2[16];
    ipv4_format_address(buf1, hdr->source_address);
    ipv4_format_address(buf2, hdr->destination_address);
    fprintf(stderr, "ip: got packet 0x%04X protocol %d from %s to %s\n", hdr->identification, hdr->protocol, buf1, buf2);
    u16 payload_length = packet_length - ((u16)hdr->header_length * 4);
    if(hdr->total_length != packet_length || payload_length >= packet_length) { // payload_length will overflow if hdr->header_length is invalid
        fprintf(stderr, "ip: dropping packet 0x%04X due to invalid size\n", hdr->identification);
        return;
    }

    // TODO look up the net_interface that handles packets addressed to hdr->destination_address
    // if no interface is found, we can ignore the packet. otherwise, deliver the packet to that interface

    // pass the payload to the proper protocol handler
    u8* payload = packet + ((u16)hdr->header_length * 4);

    switch(hdr->protocol) {
    case IPv4_PROTOCOL_ICMP:
        icmp_receive_packet(ndev, hdr, payload, payload_length);
        break;

    default:
        // unknown packet
        break;
    }
}

void ipv4_format_address(char* buf, u32 address)
{
    sprintf(buf, "%d.%d.%d.%d", (address >> 24) & 0xFF, (address >> 16) & 0xFF, (address >> 8) & 0xFF, (address >> 0) & 0xFF);
}


