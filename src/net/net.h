#ifndef __NET_H__
#define __NET_H__

#include "hashtable.h"

struct buffer;

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
    NET_PROTOCOL_ICMP,
    NET_PROTOCOL_TCP,
    NET_PROTOCOL_UDP
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

struct net_send_packet_queue_entry {
    struct net_interface* net_interface;
    struct net_socket*    net_socket;
    u8*    packet_start;
    bool   ready;
    bool   sent;
    u16    packet_length;
    u32    unused1;
};

struct net_device;
struct net_interface;
struct net_receive_packet_info;

typedef s64 (net_wrap_packet_callback)(struct net_send_packet_queue_entry* entry, u8*, void*);
typedef s64 (net_device_wrap_packet_function)(struct net_device* ndev, struct net_send_packet_queue_entry* entry, struct net_address* dest, u8 net_protocol, u16 packet_size, net_wrap_packet_callback*, void*);
typedef s64 (net_device_send_packet_function)(struct net_device* ndev, u8* packet, u16 packet_length);
typedef struct net_receive_packet_info* (net_device_receive_packet_function)(struct net_device* ndev);

// TODO generalize this to sending and receiving packets?
struct net_receive_packet_info {
    struct net_device* net_device; // device this packet was received on
    u8*    packet_base;
    u8*    packet; // updated as the packet_info traverses the layers
    u16    packet_length; // length of the remaining packet, updated as the packet_info traverses the layers
    u8     net_protocol;
    u8     unused0;
    u32    unused1;
    void   (*free)(struct net_receive_packet_info*); // call to free this packet after it's contents have been consumed
};

struct net_device_ops {
    net_device_wrap_packet_function*    wrap_packet;    // packet building
    net_device_send_packet_function*    send_packet;    // actual transmission of packets
    net_device_receive_packet_function* receive_packet; // receive packet (and parsing)
};

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

    struct net_device_ops* ops;
};

typedef void (net_interface_receive_packet_function)(struct net_interface*, struct net_receive_packet_info*);
typedef s64  (net_interface_wrap_packet_function)(struct net_send_packet_queue_entry*, struct net_address* dest_address, 
                                                  u8 net_protocol, u16 payload_size, net_wrap_packet_callback*, void*);

// Base class for network interfaces, like IPv4/6, that can be assigned to network devices.
// The interface defines the local address and how to encapsulate packets in the assigned protocol
struct net_interface {
    MAKE_HASH_TABLE;

    struct net_address address;
    struct net_device* net_device;
    u8     protocol;
    u8     unused0[7];

    net_interface_wrap_packet_function*    wrap_packet;
    net_interface_receive_packet_function* receive_packet;
};

struct net_socket_info {
    struct net_address source_address;
    struct net_address dest_address;
    u16    source_port;
    u16    dest_port;
    u8     protocol; // usually NET_PROTOCOL_TCP or NET_PROTOCOL_UDP
    u8     unused0;
    u16    unused1;
};

struct net_socket;
struct net_socket_ops {
    s64                (*listen) (struct net_socket*, u16 backlog);
    struct net_socket* (*accept) (struct net_socket*);
    s64                (*connect)(struct net_socket*);
    s64                (*close)  (struct net_socket*);
    s64                (*send)   (struct net_socket*, struct buffer*);
    s64                (*receive)(struct net_socket*, struct buffer*, u64);
    s64                (*update) (struct net_socket*);
};

struct net_socket {
    MAKE_HASH_TABLE;

    struct net_socket_info socket_info;
    struct net_socket_ops* ops;

    // prev and next that allow the socket to be placed into a list
    struct net_socket* prev;
    struct net_socket* next;
};

void net_init();
bool net_do_work();

void net_init_device(struct net_device* ndev, char* driver_name, u16 driver_index, struct net_address* hardware_address, struct net_device_ops* ops);
void net_device_register_interface(struct net_device* ndev, struct net_interface* iface);
struct net_interface* net_device_find_interface(struct net_device* ndev, struct net_address* search_address);

s64  net_request_send_packet_queue_entry(struct net_interface*, struct net_socket*, struct net_send_packet_queue_entry**);
void net_ready_send_packet_queue_entry(struct net_send_packet_queue_entry*);

struct net_socket* net_create_socket(struct net_socket_info*);
void               net_destroy_socket(struct net_socket*);
struct net_socket* net_lookup_socket(struct net_socket_info*);
void net_notify_socket(struct net_socket*);

// interface/wrappers to net_socket_ops
s64                net_socket_listen (struct net_socket*, u16 backlog);
struct net_socket* net_socket_accept (struct net_socket*);
s64                net_socket_connect(struct net_socket*);
s64                net_socket_close  (struct net_socket*);
s64                net_socket_send   (struct net_socket*, struct buffer*);
s64                net_socket_receive(struct net_socket*, struct buffer*, u16);

//TODO These are temporary
struct net_device* net_device_by_index(u16); //TODO this should be removed eventually and net_device_from_vnode(vfs_find("#netdev=index")) should be used
struct net_interface* net_device_get_interface_by_index(struct net_device*, u8, u8);

//TODO
void net_set_address(struct net_address* addr, u8 net_protocol, u8* data, u8 data_length);
void net_route_packet(struct net_address* dest, u8* payload, u8 payload_length);

#endif
