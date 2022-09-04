#include "kernel/common.h"

#include "cpu.h"
#include "errno.h"
#include "kernel/cpu.h"
#include "kernel/kernel.h"
#include "net/arp.h"
#include "net/ipv4.h"
#include "net/net.h"
#include "stdio.h"
#include "stdlib.h"

static int netdev_next_index = 0;

static struct net_device* netdevs_tmp[256] = { null, }; // TEMP TODO get rid of this and use vfs

void net_init()
{
    fprintf(stderr, "net: init %d\n", offsetof(struct net_device, hw_type));
}

// will create vnode #device=net:N #driver=driver_name:M
void net_create_device(struct net_device* ndev, char* driver_name, u16 driver_index)
{
    zero(ndev);

    u16 netdev_index = __atomic_xinc(&netdev_next_index);

    char buf[256];
    sprintf(buf, "#device=net:%d #driver=%s:%d", netdev_index, driver_name, driver_index);
    //TODO vfs_create_vnode(&ndev->vnode, buf);
    //TODO vfs_register_vnode(&ndev->vnode);

    netdevs_tmp[netdev_index] = ndev;
}

struct net_device* net_device_by_index(u16 netdev_index)
{
    return netdevs_tmp[netdev_index];
}

void net_set_hw_address(struct net_device* ndev, u8 device_hw_type, u8* hw_address)
{
    ndev->hw_type    = device_hw_type;
    ndev->hw_address = hw_address;
}

void net_set_transmit_packet_function(struct net_device* ndev, net_device_transmit_packet_function* func)
{
    ndev->hw_transmit_packet = func;
}

s64 net_transmit_packet(struct net_device* ndev, u8* dest_address, u8 dest_address_length, u8 net_protocol, u8* packet, u16 packet_length)
{
    if(ndev->hw_transmit_packet == null) return -ENOTSUP;

    return ndev->hw_transmit_packet(ndev, dest_address, dest_address_length, net_protocol, packet, packet_length);
}

void net_receive_packet(struct net_device* ndev, u8 net_protocol, u8* packet, u16 packet_length)
{
    //TODO queue packet and wake up the network thread using an event lock
    switch(net_protocol) {
    case NET_PROTOCOL_IPv4:
        ipv4_handle_device_packet(ndev, packet, packet_length);
        break;

    case NET_PROTOCOL_IPv6:
        //ipv6_handle_device_packet(...)
        break;

    case NET_PROTOCOL_ARP:
        arp_handle_device_packet(ndev, packet, packet_length);
        break;

    default:
        fprintf(stderr, "net: unknown packet protocol 0x%02X\n", net_protocol);
        break;
    }
}

