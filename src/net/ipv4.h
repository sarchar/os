#ifndef __NET_IPv4_H__
#define __NET_IPv4_H__

#include "net/net.h"

enum IPv4_PROTOCOL {
    IPv4_PROTOCOL_ICMP = 1,
    IPv4_PROTOCOL_TCP  = 6,
    IPv4_PROTOCOL_UDP  = 17,
};

enum IPv4_HEADER_FLAGS {
    IPv4_HEADER_FLAG_MAY_FRAGMENT   = 0 << 1,
    IPv4_HEADER_FLAG_DONT_FRAGMENT  = 1 << 1,
    IPv4_HEADER_FLAG_LAST_FRAGMENT  = 0 << 2,
    IPv4_HEADER_FLAG_MORE_FRAGMENTS = 1 << 2,
};

struct ipv4_interface {
    struct net_interface net_interface;
};

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
    u32 dest_address;            // bytes 17-20 need to be converted from network
    u8  options[];
} __packed;

struct net_interface* ipv4_create_interface(struct net_address* local_address);

void ipv4_handle_device_packet(struct net_device* ndev, u8* packet, u16 packet_length);

void ipv4_format_address(char* buf, u32 address);
void ipv4_parse_address_string(struct net_address* addr, char const*);
void net_set_address(struct net_address* addr, u8 net_protocol, u8* data, u8 data_length);

s64 ipv4_wrap_packet(struct net_send_packet_queue_entry*, struct net_address* dest_address, 
                     u8 payload_protocol, u16 payload_size, net_wrap_packet_callback* build_packet, void* userdata);

#endif
