#ifndef __NET_IPv4_H__
#define __NET_IPv4_H__

struct net_device;

enum IPv4_PROTOCOL {
    IPv4_PROTOCOL_ICMP = 1,
    IPv4_PROTOCOL_TCP  = 6,
    IPv4_PROTOCOL_UDP  = 17,
};

struct ipv4_interface {
    //struct net_interface interface; // base class?
    struct net_device* owner_device;

    u32 assigned_address;
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
    u32 destination_address;     // bytes 17-20 need to be converted from network
    u8  options[];
} __packed;

void ipv4_handle_device_packet(struct net_device* ndev, u8* packet, u16 packet_length);

void ipv4_format_address(char* buf, u32 address);

u8* ipv4_create_packet(u32 dest_address, u8 ipv4_protocol, u16 payload_size, u8* payload_start, u8* packet_size);

#endif
