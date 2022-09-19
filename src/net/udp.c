#include "kernel/common.h"

#include "kernel/buffer.h"
#include "kernel/cpu.h"
#include "kernel/kalloc.h"
#include "kernel/kernel.h"
#include "net/ethernet.h"
#include "net/icmp.h"
#include "net/ipv4.h"
#include "net/net.h"
#include "net/udp.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

// 5000 * ~1500 minus overhead = we can receive about ~7.2MiB of payload data
#define PAYLOAD_MAX_PACKET_COUNT 5000

struct udp_header {
    u16 source_port;
    u16 dest_port;
    u16 length;
    u16 checksum;
    u8  payload[];
} __packed;

struct udp_build_packet_info {
    u32 dest_address;
    u16 source_port;
    u16 dest_port;
    struct buffer* payload;
    u16 payload_length;
};

struct udp_socket {
    struct net_socket net_socket;

    struct ticketlock main_lock;

    struct buffer*    send_buffers;
    struct ticketlock send_buffers_lock;

    struct buffer*    receive_buffer;
    struct ticketlock receive_buffer_lock;
    struct condition  receive_ready;

    bool   closed;
};

struct payload_packet_info {
    struct net_receive_packet_info* packet_info;
    u16    dest_port;
    u16    source_port;
    u32    dest_address;
    u32    source_address;
} __packed;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// socket ops definition
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static s64   _socket_update    (struct net_socket* net_socket);
static s64   _socket_send      (struct net_socket* net_socket, struct buffer*);
static s64   _socket_receive   (struct net_socket* net_socket, struct buffer*, u64 size);
static s64   _socket_close_lock(struct net_socket*);
static void  _socket_destroy   (struct net_socket*);

static struct net_socket_ops udp_socket_ops = {
    .listen  = null,
    .accept  = null,
    .connect = null,
    .close   = &_socket_close_lock,
    .destroy = &_socket_destroy,
    .send    = &_socket_send,
    .receive = &_socket_receive,
    .update  = &_socket_update,
};
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void _receive_packet(struct udp_socket*, struct ipv4_header*, struct udp_header*, struct net_receive_packet_info*);

static u16 _compute_checksum(u32 source_address, u32 dest_address, u8* data, u16 data_length)
{
    u32 sum = 0;

    u32 src = htonl(source_address);
    u32 dst = htonl(dest_address);

    sum += (*((u8*)&src + 1) << 8) | (*((u8*)&src + 0) << 0);
    sum += (*((u8*)&src + 3) << 8) | (*((u8*)&src + 2) << 0);
    sum += (*((u8*)&dst + 1) << 8) | (*((u8*)&dst + 0) << 0);
    sum += (*((u8*)&dst + 3) << 8) | (*((u8*)&dst + 2) << 0);
    sum += ((u16)IPv4_PROTOCOL_UDP << 8);
    sum += htons(data_length);

    while(data_length > 1) {
        u16 word = ((u16)data[0] << 0) | ((u16)data[1] << 8);
        sum += word;
        data += 2;
        data_length -= 2;

        while(sum > 0xFFFF) {
            u16 high = sum >> 16;
            sum &= 0xFFFF;
            sum += high;
        }
    }

    if(data_length) {
        u16 word = (u16)data[0] << 0;
        sum += word; // add the last byte extended with a 0
    }

    // one's complement of the sum of the data
    if(sum == 0xFFFF) return 0xFFFF;
    return ~sum;
}

static s64 _build_udp_packet(struct net_send_packet_queue_entry* entry, u8* udp_packet_start, void* userdata)
{
    struct udp_header* hdr = (struct udp_header*)udp_packet_start;
    struct udp_build_packet_info* info = (struct udp_build_packet_info*)userdata;
    u16 udp_packet_size = sizeof(struct udp_header) + info->payload_length;

    // setup header
    hdr->source_port = htons(info->source_port);
    hdr->dest_port   = htons(info->dest_port);
    hdr->length      = htons(udp_packet_size);
    hdr->checksum    = 0;  // initialize checksum to 0

    // copy payload over
    buffer_read(info->payload, hdr->payload, info->payload_length);

    // compute checksum and convert to network byte order
    hdr->checksum = _compute_checksum(entry->net_interface->address.ipv4, info->dest_address, udp_packet_start, udp_packet_size);

    return udp_packet_size;
}

// TODO convert to use a udp_socket, similar to tcp_socket
static s64 _send_packet(struct udp_socket* socket, struct buffer* udp_payload, u16 udp_payload_length)
{
    u16 udp_packet_size = sizeof(struct udp_header) + udp_payload_length;

    struct udp_build_packet_info info = { 
        .dest_address   = socket->net_socket.socket_info.dest_address.ipv4,
        .source_port    = socket->net_socket.socket_info.source_port,
        .dest_port      = socket->net_socket.socket_info.dest_port,
        .payload        = udp_payload, 
        .payload_length = udp_payload_length
    };

    // ask the network interface for a tx queue slot
    struct net_send_packet_queue_entry* entry;
    s64 ret = net_request_send_packet_queue_entry(socket->net_socket.net_interface, null, &entry);
    if(ret < 0) return ret;

    // build a packet with the given info
    if((ret = entry->net_interface->wrap_packet(entry, &socket->net_socket.socket_info.dest_address, NET_PROTOCOL_UDP, udp_packet_size, &_build_udp_packet, &info)) < 0) return ret;

    // packet is ready for transmission, queue it in the network layer
    net_ready_send_packet_queue_entry(entry);

    return 0;
}

void udp_receive_packet(struct net_interface* iface, struct ipv4_header* iphdr, struct net_receive_packet_info* packet_info)
{
    unused(iface);

    struct udp_header* hdr = (struct udp_header*)packet_info->packet;

    if(packet_info->packet_length < sizeof(struct udp_header)) {
        fprintf(stderr, "udp: dropping packet (size too small = %d)\n", packet_info->packet_length);
        return;
    }

    // immediately validate the checksum on the packet, if it's set
    u16 length = min(ntohs(hdr->length), packet_info->packet_length); // actual available data needs to be no greater than the actual packet_length

    if(hdr->checksum) {
        u16 header_checksum = hdr->checksum; // clear to zero for computing the checksum
        hdr->checksum = 0;

        u16 computed_checksum;
        if(header_checksum != (computed_checksum = _compute_checksum(iphdr->source_address, iphdr->dest_address, packet_info->packet, length))) {
            fprintf(stderr, "udp: checksum error, dropping packet (computed 0x%04X, header says 0x%04X)\n", computed_checksum, header_checksum);
            return;
        }

        hdr->checksum = header_checksum;
    }

    // compute checksum and convert to network byte order
    u16 old_checksum = hdr->checksum;
    hdr->checksum = 0;
    u16 new_checksum = _compute_checksum(iphdr->source_address, iphdr->dest_address, packet_info->packet, packet_info->packet_length);

    // fix the endianness of the header
    hdr->source_port = ntohs(hdr->source_port);
    hdr->dest_port   = ntohs(hdr->dest_port);
    hdr->length      = ntohs(hdr->length);
    hdr->checksum    = ntohs(hdr->checksum);

    //char buf[16], buf2[16];
    //ipv4_format_address(buf, iphdr->source_address);
    //ipv4_format_address(buf2, iphdr->dest_address);
    //fprintf(stderr, "udp: got packet from %s:%d to %s:%d length=%d checksum=0x%04X (checksum %s)\n",
    //        buf, hdr->source_port, buf2, hdr->dest_port, hdr->length, old_checksum, (new_checksum == old_checksum) ? "valid" : "invalid");

    // verify checksum before passing data onwards
    if(new_checksum != old_checksum) {
        packet_info->free(packet_info);
        return;
    }

    // for UDP, a socket can receive data from any source
    // flip the source and dest addresses to align with our point of view of sockets
    struct net_socket_info sockinfo = {
        .protocol           = NET_PROTOCOL_UDP,
        .source_port        = hdr->dest_port,
        .dest_port          = 0,
    };

    zero(&sockinfo.source_address);
    zero(&sockinfo.dest_address);
    sockinfo.dest_address.protocol   = NET_PROTOCOL_IPv4;
    sockinfo.dest_address.ipv4       = 0;
    sockinfo.source_address.protocol = NET_PROTOCOL_IPv4;
    sockinfo.source_address.ipv4     = iphdr->dest_address;

    // Look up the net_socket
    struct net_socket* net_socket = net_socket_lookup(&sockinfo);
    struct udp_socket* socket = containerof(net_socket, struct udp_socket, net_socket);
    if(net_socket == null) {
        // No bound sockets to the ip:port, See if there's a socket bound to 0.0.0.0:port
        sockinfo.source_address.ipv4 = 0;

        net_socket = net_socket_lookup(&sockinfo);
        socket = containerof(net_socket, struct udp_socket, net_socket);
    }
    
    if(net_socket != null) {
        // update the packet info before sending it on
        packet_info->packet         = (u8*)hdr + (u16)sizeof(struct udp_header);
        packet_info->packet_length -= (u16)sizeof(struct udp_header);

        acquire_lock(socket->main_lock);
        _receive_packet(socket, iphdr, hdr, packet_info);
        release_lock(socket->main_lock);
    } else {
        // free the packet memory
        fprintf(stderr, "no socket found for receiving the packet\n");
        packet_info->free(packet_info);
    }
}

// deliver the payload to the socket receive buffers
static void _receive_packet(struct udp_socket* socket, struct ipv4_header* iphdr, struct udp_header* hdr, struct net_receive_packet_info* packet_info)
{
    // nothing to deliver?
    if(packet_info->packet_length == 0) {
        packet_info->free(packet_info);
        return;
    }

    acquire_lock(socket->receive_buffer_lock);

    u32 before_read_size = buffer_remaining_read(socket->receive_buffer);
    struct payload_packet_info ppi = {
        .packet_info    = packet_info,
        .dest_address   = iphdr->dest_address,
        .dest_port      = hdr->dest_port,
        .source_address = iphdr->source_address,
        .source_port    = hdr->source_port,
    };

    if(buffer_remaining_write(socket->receive_buffer) < sizeof(ppi)) {
        fprintf(stderr, "udp: incoming buffer for socket 0x%lX is full, dropping packet\n", socket);
        packet_info->free(packet_info);
        release_lock(socket->receive_buffer_lock);
        return;
    }

    u32 v = buffer_write(socket->receive_buffer, (u8*)&ppi, sizeof(ppi));
    //fprintf(stderr, "payload added one entry (length %d) to buffer, now has %d entries\n", packet_info->packet_length, buffer_remaining_read(socket->receive_buffer) / sizeof(ppi));
    assert(v == sizeof(ppi), "what happened that there was enough space and then there wasn't?");

    // only when we go from 0 to non-zero do we signal data is ready
    if(before_read_size == 0) {
        //fprintf(stderr, "tcp: notifying receive_ready\n");
        notify_condition(socket->receive_ready);
    }

    release_lock(socket->receive_buffer_lock);
}

struct net_socket* udp_socket_create(struct net_socket_info* sockinfo)
{
    declare_condition(condition_init);
    declare_ticketlock(lock_init);

    assert(sockinfo->protocol == NET_PROTOCOL_UDP, "required UDP sockinfo");

    // right now only support IPv4. TODO IPv6
    if(sockinfo->source_address.protocol != NET_PROTOCOL_IPv4 ||
       sockinfo->dest_address.protocol != NET_PROTOCOL_IPv4) return null;

    struct udp_socket* socket = (struct udp_socket*)kalloc(sizeof(struct udp_socket)); // use kalloc for fast allocation
    zero(socket);

    socket->main_lock           = lock_init;
    socket->receive_buffer_lock = lock_init;
    socket->send_buffers_lock   = lock_init;
    socket->receive_ready       = condition_init;
    socket->net_socket.ops      = &udp_socket_ops;

    // allocate the receive buffer
    socket->receive_buffer = buffer_create(PAYLOAD_MAX_PACKET_COUNT * sizeof(struct payload_packet_info)); // TODO determine the size better somehow

    return &socket->net_socket;
}

static s64 _process_send_buffers(struct udp_socket* socket)
{
    s64 ret = 0;
    
    acquire_lock(socket->send_buffers_lock);
    while(socket->send_buffers != null) {
        struct buffer* curbuf = socket->send_buffers;

        // remove buffer when it is empty
        if(buffer_remaining_read(curbuf) == 0) {
            // remove the empty buffer from the list
            DEQUE_POP_FRONT(socket->send_buffers, curbuf);
            buffer_destroy(curbuf);
            continue;
        }

        // read the data from the buffer
        //fprintf(stderr, "udp: queuing from send buffer 0x%lX on socket 0x%lX (%d remaining)\n", curbuf, socket, buffer_remaining_read(curbuf));

        // TEMP safe TODO use MTU
        // if _queue_segment reads all the data from curbuf, it'll be freed in the next loop
        if((ret = _send_packet(socket, curbuf, min(1400, buffer_remaining_read(curbuf)))) < 0) break;
    }
    release_lock(socket->send_buffers_lock);

    return ret;
}


static s64 _socket_update(struct net_socket* net_socket)
{
    struct udp_socket* socket = containerof(net_socket, struct udp_socket, net_socket);
    //fprintf(stderr, "_socket_update: cpu %d\n", get_cpu()->cpu_index);

    s64 ret;
    if((ret = _process_send_buffers(socket)) < 0) return ret;

    // notify the network layer if there's still work left on this socket
    if(socket->send_buffers != null) {
        net_notify_socket(net_socket);
    }

    return 0;
}

static s64 _socket_send(struct net_socket* net_socket, struct buffer* src)
{
    // add buf to the tail of the linked list udp_socket.send_buffers
    struct udp_socket* socket = containerof(net_socket, struct udp_socket, net_socket);
    //fprintf(stderr, "udp: queuing send buffer 0x%lX on socket 0x%lX size %lu\n", buf, socket, buffer_remaining_read(buf));

    acquire_lock(socket->send_buffers_lock);
    DEQUE_PUSH_BACK(socket->send_buffers, src);
    release_lock(socket->send_buffers_lock);

    net_notify_socket(net_socket);
    return 0;
}

static s64 _socket_receive(struct net_socket* net_socket, struct buffer* dest, u64 size)
{
    s64 res = 0;

    // weird, but allowed
    if(size == 0) return 0;

    // receive should try and read `size` bytes, but can return early if we read an entire packet
    //fprintf(stderr, "TODO: udp receive socket 0x%lX dest 0x%lX size %lu/%lu\n", socket, dest, size, buffer_remaining_write(dest));
    struct udp_socket* socket = containerof(net_socket, struct udp_socket, net_socket);

    // try to read up until size
    while(res >= 0 && (u64)res < size) {
        //fprintf(stderr, "socket 0x%lX waiting on receive_ready condition\n");
        wait_condition(socket->receive_ready); // receive_ready is only triggered when receive buffer goes from empty to non-empty (not each time data is received)
                                               // so we will receive data, and if the buffer is non-empty after we receive data, we can notify_condition since we've 
                                               // consumed one signal to let successive calls and other threads read successfully
        //fprintf(stderr, "socket 0x%lX woke up from receive_ready condition\n");

        // we need a lock to protect receive_buffer between here and _receive_payload
        acquire_lock(socket->receive_buffer_lock);
        u32 remaining_payloads = buffer_remaining_read(socket->receive_buffer) / sizeof(struct payload_packet_info);

        // we can wake up due to a lost connection and then we have to return no data, informing the caller that the socket has closed
        if(remaining_payloads == 0 || socket->closed) {
            // condition must have triggered due to closed socket, return
            release_lock(socket->receive_buffer_lock);
            break;
        }

        // get the current payload info
        struct payload_packet_info ppi;
        u32 v = buffer_peek(socket->receive_buffer, (u8*)&ppi, sizeof(ppi));
        assert(v == sizeof(ppi), "must be the case");

        // perform the data copy
        u64 max_read    = min(size, ppi.packet_info->packet_length);
        u32 actual_read = buffer_write(dest, ppi.packet_info->packet, max_read);
        res += actual_read;

        // update ppi and if necessary the receive_buffer too
        ppi.packet_info->packet        += actual_read;
        ppi.packet_info->packet_length -= actual_read;
        //u16 last_packet_length = ppi.packet_info->packet_length;

        // our "push" will be when the payload is fully read
        bool was_push = (ppi.packet_info->packet_length == 0);

        if(ppi.packet_info->packet_length == 0) { // no more payload in this packet
            ppi.packet_info->free(ppi.packet_info); // free the network packet
            v = buffer_read(socket->receive_buffer, null, sizeof(struct payload_packet_info)); // clear the entry from the receive_buffer
            assert(v == sizeof(struct payload_packet_info), "must be the case");
        }

        //fprintf(stderr, "read %d, payload has %d bytes left, buffer has %d entries remaining\n", actual_read, last_packet_length, buffer_remaining_read(socket->receive_buffer) / sizeof(struct payload_packet_info));

        // notify any remaining data available
        if(buffer_remaining_read(socket->receive_buffer) > 0) {
            notify_condition(socket->receive_ready);
        }

        release_lock(socket->receive_buffer_lock);

        // two conditions make us break out, otherwise we continue waiting for data:
        // 1) PSH flag was set in the packet that delivered this payload, but only once we read the entire packet
        // 2) the destination buffer is full, detectable by writing less data to it than there is available
        if(was_push || actual_read < max_read) break;
        // otherwise, continue reading data
    }

    return res;
}

static s64 _socket_close_lock(struct net_socket* net_socket)
{
    unused(net_socket);
    assert(false, "TODO"); // TODO
    return -EINVAL;
}

static void _socket_destroy(struct net_socket* net_socket)
{
    struct udp_socket* socket = containerof(net_socket, struct udp_socket, net_socket);
    fprintf(stderr, "udp: TODO destroy socket 0x%lX\n", socket);

//!    if(socket->receive_buffer) {
//!        while(buffer_remaining_read(socket->receive_buffer) > 0) {
//!            // read the payload info
//!            struct payload_packet_info ppi;
//!            if(buffer_read(socket->receive_buffer, (u8*)&ppi, sizeof(struct payload_packet_info)) != sizeof(struct payload_packet_info)) { // major error if there's not a full structure in the buffer
//!                fprintf(stderr, "tcp: major buffer problem with socket 0x%lX, probably leaking memory\n", socket);
//!                break;
//!            }
//!
//!            // free the packet info
//!            ppi.packet_info->free(ppi.packet_info);
//!        }
//!
//!        // free the buffer
//!        buffer_destroy(socket->receive_buffer);
//!    }
}


