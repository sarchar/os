#include "kernel/common.h"

#include "cpu.h"
#include "errno.h"
#include "kernel/buffer.h"
#include "kernel/cpu.h"
#include "kernel/kalloc.h"
#include "kernel/kernel.h"
#include "kernel/paging.h"
#include "kernel/palloc.h"
#include "net/arp.h"
#include "net/ipv4.h"
#include "net/net.h"
#include "net/tcp.h"
#include "net/udp.h"
#include "stdio.h"
#include "stdlib.h"

static u16 netdev_next_index = 0;

static struct net_device* netdevs_tmp[256] = { null, }; // TEMP TODO get rid of this and use vfs

// global structure of all open sockets the kernel is aware of
static struct net_socket* global_net_sockets = null;
declare_spinlock(global_net_sockets_lock);

// list of "notified" sockets, that have work pending
static struct net_socket* notified_net_sockets = null;
static declare_spinlock(notify_socket_lock);

// the global send queue
#define SEND_QUEUE_PAGE_ORDER 1 // 8192 bytes / 8 = 1024 entries
static struct net_send_packet_queue_entry** send_queue;
static u32    send_queue_size;
static u32    send_queue_head;
static u32    send_queue_tail;
static declare_spinlock(send_queue_lock);

static void _receive_packet(struct net_receive_packet_info*);

void net_init()
{
    send_queue_size = (1 << (PAGE_SHIFT + SEND_QUEUE_PAGE_ORDER)) / sizeof(struct net_send_packet_queue_entry*);
    send_queue = (struct net_send_packet_queue_entry**)palloc_claim(SEND_QUEUE_PAGE_ORDER);
    send_queue_head = send_queue_tail = 0;
}

static bool net_do_rx_work()
{
    // TODO get notification from interfaces that there's data on the line
    u16 num_devs = netdev_next_index;
    for(u16 i = 0; i < num_devs; i++) {
        struct net_device* ndev = netdevs_tmp[i];
        if(ndev == null) continue;

        struct net_receive_packet_info* packet_info;
        if((packet_info = ndev->ops->receive_packet(ndev)) != null) {
            _receive_packet(packet_info);
            return true;
        }
    }

    return false;
}

static bool net_do_tx_work()
{
    // need a lock here since multiple threads are calling net_do_tx_work
    acquire_lock(send_queue_lock);
    while(send_queue_head != send_queue_tail) {
        // free all packets that have been sent that are the beginning of the list
        if(send_queue[send_queue_head]->sent) {
            struct net_send_packet_queue_entry* entry = send_queue[send_queue_head];
            free(entry->packet_start);
            kfree(entry, sizeof(struct net_send_packet_queue_entry));
            send_queue_head = (send_queue_head + 1) % send_queue_size;
            continue;
        }

        // find first not-sent ready packet, which might not be at the beginning of the list
        u32 ri = send_queue_head;
        while(ri != send_queue_tail && (!send_queue[ri]->ready || send_queue[ri]->sent)) {
            ri = (ri + 1) % send_queue_size;
        }

        // if there's no not-sent ready packets, we're done
        if(ri == send_queue_tail) {
            release_lock(send_queue_lock);
            return false;
        }

        // have a valid queue entry 
        struct net_send_packet_queue_entry* entry = send_queue[ri];
        bool can_free = false;
        if(ri == send_queue_head) { // best case
            send_queue_head = (send_queue_head + 1) % send_queue_size;
            can_free = true;
        }

        // we can send this packet now, and release the lock to let other threads send packets too
        entry->sent = true;
        release_lock(send_queue_lock); 

        struct net_device* ndev = entry->net_interface->net_device;
        if(ndev->ops->send_packet != null) {
            s64 ret = ndev->ops->send_packet(ndev, entry->packet_start, entry->packet_length);
            if(ret < 0) return false;
        }

        if(can_free) {
            free(entry->packet_start);
            kfree(entry, sizeof(struct net_send_packet_queue_entry));
        }

        return true;
    }

    release_lock(send_queue_lock);
    return false;
}

static bool net_do_notify_sockets()
{
    while(notified_net_sockets != null) { // check if any socket exists with work to do
        // grab the socket, maybe another cpu will get it first
        acquire_lock(notify_socket_lock);
        struct net_socket* notify_socket = notified_net_sockets;
        if(notify_socket == null) {
            release_lock(notify_socket_lock);
            continue;
        }

        // save to remove the item from the list here
        if(notify_socket == notify_socket->next) {
            notified_net_sockets = null;
        } else {
            notified_net_sockets = notify_socket->next;
            notified_net_sockets->prev = notify_socket->prev;
            notified_net_sockets->prev->next = notified_net_sockets;
        }
        release_lock(notify_socket_lock);

        notify_socket->next = notify_socket->prev = null;
        notify_socket->ops->update(notify_socket);
        
        return true;
    }

    return false;
}

// return true if we "did work"
declare_spinlock(net_work_lock);
bool net_do_work()
{
    bool loop = true;
    bool did_work;

    // if we can't get the lock, return to do other work
    if(!try_lock(net_work_lock)) return false;

    while(loop) {
        loop = false;
        loop = net_do_rx_work()        || loop;
        loop = net_do_notify_sockets() || loop;
        loop = net_do_tx_work()        || loop;

        did_work = loop || did_work;
    }

    release_lock(net_work_lock);

    return did_work;
}

// will create vnode #device=net:N #driver=driver_name:M
void net_init_device(struct net_device* ndev, char* driver_name, u16 driver_index, struct net_address* hardware_address, struct net_device_ops* ops)
{
    declare_ticketlock(lock_init);

    zero(ndev);

    ndev->hardware_address = *hardware_address;
    ndev->index            = __atomic_xinc(&netdev_next_index);
    ndev->ops              = ops;
    ndev->interfaces_lock  = lock_init;

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
    acquire_lock(ndev->interfaces_lock);
    HT_FIND(ndev->interfaces, iface->address, tmp);

    if(tmp != null) {
        release_lock(ndev->interfaces_lock);
        fprintf(stderr, "net: error interface already registered");
        return;
    }

    iface->net_device = ndev;
    HT_ADD(ndev->interfaces, address, iface);
    release_lock(ndev->interfaces_lock);

    if(iface->protocol == NET_PROTOCOL_IPv4) {
        char addr[16];
        ipv4_format_address(addr, iface->address.ipv4);
        char netmask[16];
        ipv4_format_address(netmask, iface->netmask.ipv4);
        fprintf(stderr, "net: registered IPv4 device interface %s/%s\n", addr, netmask);
    }
}

void net_device_unregister_interface(struct net_interface* iface)
{
    struct net_device* ndev = iface->net_device;
    acquire_lock(ndev->interfaces_lock);
    HT_DELETE(ndev->interfaces, iface);
    release_lock(ndev->interfaces_lock);
    iface->net_device = null;
}

struct net_interface* net_device_find_interface(struct net_device* ndev, struct net_address* search_address)
{
    struct net_interface* tmp;
    acquire_lock(ndev->interfaces_lock);
    HT_FIND(ndev->interfaces, *search_address, tmp);
    release_lock(ndev->interfaces_lock);
    return tmp;
}

static void _receive_packet(struct net_receive_packet_info* packet_info)
{
    switch(packet_info->net_protocol) {
    case NET_PROTOCOL_IPv4:
        ipv4_handle_device_packet(packet_info);
        break;

    case NET_PROTOCOL_IPv6:
        //ipv6_handle_device_packet(...)
        break;

    case NET_PROTOCOL_ARP:
        arp_handle_device_packet(packet_info);
        break;

    default:
        fprintf(stderr, "net: unknown packet protocol 0x%02X\n", packet_info->net_protocol);
        break;
    }
}

struct net_socket* net_socket_create(struct net_interface* iface, struct net_socket_info* sockinfo)
{
    struct net_socket* tmp;
    acquire_lock(global_net_sockets_lock);
    HT_FIND(global_net_sockets, *sockinfo, tmp);
    if(tmp != null) {
        release_lock(global_net_sockets_lock);
        return null;
    }

    release_lock(global_net_sockets_lock); // releasing the lock lets socket create create other, possibly nested, sockets
    switch(sockinfo->protocol) {
    case NET_PROTOCOL_TCP:
        // get the size required for the socket structure and allocate it here
        tmp = tcp_socket_create(sockinfo);
        break;

    case NET_PROTOCOL_UDP:
        tmp = udp_socket_create(sockinfo);
        break;
    }

    if(tmp == null) return null;
    
    // add socket to global sockets
    memcpy(&tmp->socket_info, sockinfo, sizeof(struct net_socket_info)); // copy over sockinfo so *_create_socket() doesn't have to
    tmp->net_interface = iface;
    acquire_lock(global_net_sockets_lock);
    HT_ADD(global_net_sockets, socket_info, tmp);
    release_lock(global_net_sockets_lock);

    return tmp;
}

struct net_socket* net_socket_lookup(struct net_socket_info* sockinfo)
{
    struct net_socket* res;
    acquire_lock(global_net_sockets_lock);
    HT_FIND(global_net_sockets, *sockinfo, res);
    release_lock(global_net_sockets_lock);
    return res;
}

void net_notify_socket(struct net_socket* socket)
{
    acquire_lock(notify_socket_lock);

    if(notified_net_sockets == null) {
        socket->next = socket->prev = socket;
        notified_net_sockets = socket;
    } else {
        // insert it at the end of the queue
        notified_net_sockets->prev->next = socket;
        socket->prev = notified_net_sockets->prev;
        socket->next = notified_net_sockets;
        notified_net_sockets->prev = socket;
    }

    release_lock(notify_socket_lock);
}

// interface/wrappers to net_socket_ops
s64 net_socket_listen(struct net_socket* socket, u16 backlog)
{
    return socket->ops->listen(socket, backlog);
}

struct net_socket* net_socket_accept (struct net_socket* socket)
{
    return socket->ops->accept(socket);
}

s64 net_socket_connect(struct net_socket* socket)
{
    return socket->ops->connect(socket);
}

s64 net_socket_close(struct net_socket* socket)
{
    return socket->ops->close(socket);
}

s64 net_socket_send(struct net_socket* socket, struct buffer* buf)
{
    return socket->ops->send(socket, buf);
}

s64 net_socket_receive(struct net_socket* socket, struct buffer* buf, u16 size)
{
    return socket->ops->receive(socket, buf, size);
}

void net_socket_destroy(struct net_socket* socket)
{
    socket->ops->destroy(socket);
}

// actually removes the socket from the global socket pool
void net_socket_finish_destroy(struct net_socket* socket)
{
    struct net_socket* tmp;
    acquire_lock(global_net_sockets_lock);
    HT_FIND(global_net_sockets, socket->socket_info, tmp);
    assert(tmp != null, "all sockets should be in global_net_sockets");
    HT_DELETE(global_net_sockets, tmp);
    release_lock(global_net_sockets_lock);
}

s64 net_request_send_packet_queue_entry(struct net_interface* iface, struct net_socket* socket, struct net_send_packet_queue_entry** ret)
{
    // can we even send on this device?
    if(iface->net_device->ops->send_packet == null) return -ENOTSUP;

    // look for a slot in the send queue
    acquire_lock(send_queue_lock);
    u32 slot = send_queue_tail;
    if(((slot + 1) % send_queue_size) == send_queue_head) {
        release_lock(send_queue_lock);
        return -EAGAIN;
    }

    // allocate memory for the entry (don't like that we hold the lock here, but whatever, this should be fast)
    *ret = (struct net_send_packet_queue_entry*)kalloc(sizeof(struct net_send_packet_queue_entry));
    if(*ret == null) {
        release_lock(send_queue_lock);
        return -ENOMEM;
    }

    struct net_send_packet_queue_entry* entry = *ret;
    zero(entry);
    send_queue[slot] = entry;
    send_queue_tail = (send_queue_tail + 1) % send_queue_size;

    release_lock(send_queue_lock);

    entry->net_interface = iface;
    entry->net_socket    = socket;

    return 0;
}

void net_ready_send_packet_queue_entry(struct net_send_packet_queue_entry* entry)
{
    entry->ready = true;
}

