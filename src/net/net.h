#ifndef __NET_H__
#define __NET_H__

#include "hashtable.h"

// network [short] to host short
__always_inline u16 ntohs(u16 v)
{
//#ifdef __LITTLE_ENDIAN__, etc
    return __bswap16(v);
}

// host to network short
__always_inline u16 htons(u16 v)
{
//#ifdef __LITTLE_ENDIAN__, etc
    return __bswap16(v);
}

// network [long] to host long
__always_inline u32 ntohl(u32 v)
{
//#ifdef __LITTLE_ENDIAN__, etc
    return __bswap32(v);
}

// network [long] to host long
__always_inline u32 htonl(u32 v)
{
//#ifdef __LITTLE_ENDIAN__, etc
    return __bswap32(v);
}

enum NET_PROTOCOL {
    NET_PROTOCOL_UNSUPPORTED,
    NET_PROTOCOL_ETHERNET,
    NET_PROTOCOL_ARP,
    NET_PROTOCOL_IPv4,
    NET_PROTOCOL_IPv6,
    NET_PROTOCOL_ICMP
};

struct net_address {
    u8 protocol;
    u8 unused[7];

    union {
        u8  mac[6];
        u32 ipv4;
        u16 ipv6[8];
    };
};

struct net_device;
struct net_interface;
typedef s64 (net_wrap_packet_callback)(struct net_interface* iface, u8*, void*);
typedef u8* (net_device_wrap_packet_function)(struct net_device* ndev, struct net_interface* iface, struct net_address* dest, u8 net_protocol, u16 packet_size, net_wrap_packet_callback*, void*, u16*);
typedef s64 (net_device_send_packet_function)(struct net_device* ndev, u8* packet, u16 packet_length);

// There's a one-to-one mapping from net_device to hardware addresses. They're generally created by network drivers
// but virtual network devices can exist.
struct net_device {
    // read/write/other callback functions
    // vnode pointer?
    struct net_address hardware_address;
    struct net_interface* interfaces;

    u16 index; // network device inde
    u16 unused0;
    u32 unused1;

    net_device_wrap_packet_function* wrap_packet;
    net_device_send_packet_function* send_packet;
};

typedef void (net_interface_receive_packet_function)(struct net_interface*, u8* packet_start, u16 packet_length);
typedef u8*  (net_interface_wrap_packet_function)(struct net_interface*, struct net_address* dest_address, 
                                                  u8 net_protocol, u16 payload_size, net_wrap_packet_callback*, void*, u16*);

// Base class for network interfaces, like IPv4/6, that can be assigned to network devices.
// The interface defines the local address and how to encapsulate packets in the assigned protocol
struct net_interface {
    struct net_address address;
    struct net_device* net_device;
    u8     protocol;
    u8     unused0[7];

    net_interface_wrap_packet_function*    wrap_packet;
    net_interface_receive_packet_function* receive_packet;

    // TODO send_packet, receive_packet, etc
    MAKE_HASH_TABLE;
};

void net_init();

void net_init_device(struct net_device* ndev, char* driver_name, u16 driver_index, struct net_address* hardware_address);
void net_device_register_interface(struct net_device* ndev, struct net_interface* iface);
struct net_interface* net_device_find_interface(struct net_device* ndev, struct net_address* search_address);

void net_receive_packet(struct net_device* ndev, u8 net_protocol, u8* packet, u16 packet_length);
s64  net_send_packet(struct net_device* ndev, u8* packet, u16 packet_length);

//TODO These are temporary
struct net_device* net_device_by_index(u16); //TODO this should be removed eventually and net_device_from_vnode(vfs_find("#netdev=index")) should be used
struct net_interface* net_device_get_interface_by_index(struct net_device*, u8, u8);

//TODO
void net_set_address(struct net_address* addr, u8 net_protocol, u8* data, u8 data_length);
void net_route_packet(struct net_address* dest, u8* payload, u8 payload_length);

#endif
