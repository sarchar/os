#ifndef __NET_H__
#define __NET_H__

// network [short] to host short
__always_inline u16 ntohs(u16 v)
{
//#ifdef __LITTLE_ENDIAN__, etc
    return ((v & 0x00FF) << 8) | ((v & 0xFF00) >> 8);
}

// host to network short
__always_inline u16 htons(u16 v)
{
//#ifdef __LITTLE_ENDIAN__, etc
    return ((v >> 8) & 0x00FF) | ((v & 0x00FF) << 8);
}

// network [long] to host long
__always_inline u32 ntohl(u32 v)
{
//#ifdef __LITTLE_ENDIAN__, etc
    return ((v & 0xFF000000) >> 24) | ((v & 0x00FF0000) >> 8) | ((v & 0x0000FF00) << 8) | ((v & 0x000000FF) << 24);
}

enum NET_DEVICE_HW_TYPE {
    NET_DEVICE_HW_TYPE_ETHERNET
};

enum NET_PROTOCOL {
    NET_PROTOCOL_UNSUPPORTED,
    NET_PROTOCOL_IPv4,
    NET_PROTOCOL_IPv6,
    NET_PROTOCOL_ARP
};

struct net_device;
typedef s64 (net_device_transmit_packet_function)(struct net_device* ndev, u8* dest_address, u8 dest_address_length, u8 net_protocol, u8* packet, u16 packet_length);

// There's a one-to-one mapping from net_device to hardware addresses. They're generally created by network drivers
// but virtual network devices can exist.
struct net_device {
    // read/write/other callback functions
    // vnode pointer?
    u8* hw_address;

    u8  hw_type;
    u8  unused0;
    u16 index;
    u32 unused1;

    net_device_transmit_packet_function* hw_transmit_packet;
};

// Base class for network interfaces, like IPv4/6, that can be assigned to network devices.
// The interface defines the local address and how to encapsulate packets in the assigned protocol
struct net_interface {
    // TODO send_packet, receive_packet, etc
};

struct net_address {
    u8 address_type;
    u8 unused[7];

    union {
        u32 ipv4;
    };
};

void net_init();

void net_create_device(struct net_device* ndev, char* driver_name, u16 driver_index);
void net_set_hw_address(struct net_device* ndev, u8 device_hw_type, u8* hw_address);
void net_set_transmit_packet_function(struct net_device* ndev, net_device_transmit_packet_function*);
void net_receive_packet(struct net_device* ndev, u8 net_protocol, u8* packet, u16 packet_length);
s64  net_transmit_packet(struct net_device* ndev, u8* dest_address, u8 dest_address_length, u8 net_protocol, u8* packet, u16 packet_length);
struct net_device* net_device_by_index(u16); //TODO this should be removed eventually and net_device_from_vnode(vfs_find("#netdev=index")) should be used

//TODO
void net_set_address(struct net_address* addr, u8 net_protocol, u8* data, u8 data_length);
void net_route_packet(struct net_address* dest, u8* payload, u8 payload_length);

#endif
