#include "kernel/common.h"

#include "cpu.h"
#include "errno.h"
#include "kernel/cpu.h"
#include "kernel/kernel.h"
#include "net/arp.h"
#include "net/ipv4.h"
#include "net/net.h"
#include "net/tcp.h"
#include "stdio.h"
#include "stdlib.h"

static u16 netdev_next_index = 0;

static struct net_device* netdevs_tmp[256] = { null, }; // TEMP TODO get rid of this and use vfs

// global structure of all open sockets the kernel is aware of
static struct net_socket* global_net_sockets = null;

void net_init()
{
}

static bool net_do_rx_work()
{
    bool res = false;

    // TODO get notification from interfaces that there's data on the line
    u16 num_devs = netdev_next_index;
    for(u16 i = 0; i < num_devs; i++) {
        struct net_device* ndev = netdevs_tmp[i];
        if(ndev == null) continue;

        u8 net_protocol;
        u16 packet_length;
        u8* packet;
        while((packet = ndev->ops->receive_packet(ndev, &net_protocol, &packet_length)) != null) { //TODO limit the # of packets we acquire per call to net_do_rx_work()?
            net_receive_packet(ndev, net_protocol, packet, packet_length);
            res = true;
        }
    }

    return res;
}

static bool net_do_tx_work()
{
    return false;
}

// return true if we "did work"
bool net_do_work()
{
    return net_do_rx_work() || net_do_tx_work();
}

// will create vnode #device=net:N #driver=driver_name:M
void net_init_device(struct net_device* ndev, char* driver_name, u16 driver_index, struct net_address* hardware_address, struct net_device_ops* ops)
{
    zero(ndev);

    ndev->hardware_address = *hardware_address;
    ndev->index            = __atomic_xinc(&netdev_next_index);
    ndev->ops              = ops;

    char buf[256];
    sprintf(buf, "#device=net:%d #driver=%s:%d", ndev->index, driver_name, driver_index);
    //TODO vfs_create_vnode(&ndev->vnode, buf);
    //TODO vfs_register_vnode(&ndev->vnode);
    fprintf(stderr, "net: registered device %s\n", buf);

    //TODO just temp for now
    netdevs_tmp[ndev->index] = ndev;
}

struct net_device* net_device_by_index(u16 netdev_index)
{
    return netdevs_tmp[netdev_index];
}

struct net_interface* net_device_get_interface_by_index(struct net_device* ndev, u8 net_protocol, u8 iface_index)
{
    struct net_interface* iface;
    struct net_interface* next_iface;

    HT_FOR_EACH(ndev->interfaces, iface, next_iface) {
        if(iface->protocol == net_protocol && iface_index-- == 0) return iface;
    }

    return null;
}

void net_set_hardware_address(struct net_device* ndev, struct net_address* address)
{
    ndev->hardware_address = *address;
}

void net_device_register_interface(struct net_device* ndev, struct net_interface* iface)
{
    struct net_interface* tmp;
    HT_FIND(ndev->interfaces, iface->address, tmp);

    if(tmp != null) {
        fprintf(stderr, "net: error interface already registered");
        return;
    }

    iface->net_device = ndev;

    HT_ADD(ndev->interfaces, address, iface);

    if(iface->protocol == NET_PROTOCOL_IPv4) {
        char buf[16];
        ipv4_format_address(buf, iface->address.ipv4);
        fprintf(stderr, "net: registered IPv4 device interface %s\n", buf);
    }
}

struct net_interface* net_device_find_interface(struct net_device* ndev, struct net_address* search_address)
{
    struct net_interface* tmp;
    HT_FIND(ndev->interfaces, *search_address, tmp);
    return tmp;
}

s64 net_send_packet(struct net_device* ndev, u8* packet, u16 packet_length)
{
    if(ndev->ops->send_packet == null) return -ENOTSUP;
    return ndev->ops->send_packet(ndev, packet, packet_length);
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

struct net_socket* net_create_socket(struct net_socket_info* sockinfo)
{
    struct net_socket* tmp;
    HT_FIND(global_net_sockets, *sockinfo, tmp);
    if(tmp != null) return null;

    switch(sockinfo->protocol) {
    case NET_PROTOCOL_TCP:
        // get the size required for the socket structure and allocate it here
        tmp = tcp_create_socket(sockinfo);
        break;

    case NET_PROTOCOL_UDP:
        //TODO
        break;
    }

    if(tmp == null) return null;
    
    // add socket to global sockets
    memcpy(&tmp->socket_info, sockinfo, sizeof(struct net_socket_info)); // copy over sockinfo so *_create_socket() doesn't have to
    HT_ADD(global_net_sockets, socket_info, tmp);

    return tmp;
}

struct net_socket* net_lookup_socket(struct net_socket_info* sockinfo)
{
    struct net_socket* res;
    HT_FIND(global_net_sockets, *sockinfo, res);
    return res;
}

