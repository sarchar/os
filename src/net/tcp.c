// RFC at https://datatracker.ietf.org/doc/html/rfc9293
#include "kernel/common.h"

#include "errno.h"
#include "kernel/buffer.h"
#include "kernel/cpu.h"
#include "kernel/kalloc.h"
#include "kernel/kernel.h"
#include "kernel/task.h"
#include "net/ethernet.h"
#include "net/icmp.h"
#include "net/ipv4.h"
#include "net/net.h"
#include "net/tcp.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

struct tcp_header {
    u16 source_port;
    u16 dest_port;
    u32 sequence_number;
    u32 ack_number;
    union {
        u16 flags;
        struct {
            u16 finish      : 1;
            u16 sync        : 1;
            u16 reset       : 1;
            u16 push        : 1;
            u16 ack         : 1;
            u16 urgent      : 1;
            u16 reserved    : 6;
            u16 data_offset : 4;
        } __packed;
    } __packed;
    u16 window;
    u16 checksum;
    u16 urgent_pointer;
    u8  options[];
} __packed;

enum TCP_OPTIONS {
    TCP_OPTION_END                  = 0,
    TCP_OPTION_NOP                  = 1,
    TCP_OPTION_MAXIMUM_SEGMENT_SIZE = 2,
    TCP_OPTION_WINDOW_SCALE         = 3,
    TCP_OPTION_SACK_PERMITTED       = 4,
    TCP_OPTION_TIMESTAMPS           = 8,
};

enum TCP_OPTION_PRESENT_FLAGS {
    TCP_OPTION_PRESENT_MAXIMUM_SEGMENT_SIZE = 1 << 0,
    TCP_OPTION_PRESENT_WINDOW_SCALE         = 1 << 1,
    TCP_OPTION_PRESENT_SACK_PERMITTED       = 1 << 2,
};

struct tcp_header_options {
    u16 maximum_segment_size;
    u16 window_scale;

    u8  unused0;
    u8  nops;
    u8  padding_bytes;
    u8  present;
};

enum TCP_SOCKET_STATE {
    TCP_SOCKET_STATE_CLOSED        = 0,
    TCP_SOCKET_STATE_LISTEN        = 1,
    TCP_SOCKET_STATE_SYNC_SENT     = 2,
    TCP_SOCKET_STATE_SYNC_RECEIVED = 3,
    TCP_SOCKET_STATE_ESTABLISHED   = 4,
    TCP_SOCKET_STATE_CLOSING       = 5,
    TCP_SOCKET_STATE_CLOSE_WAIT    = 6,
    TCP_SOCKET_STATE_FINISH_WAIT_1 = 7,
    TCP_SOCKET_STATE_FINISH_WAIT_2 = 8,
    TCP_SOCKET_STATE_LAST_ACK      = 9,
    TCP_SOCKET_STATE_TIME_WAIT     = 10
};

enum TCP_BUILD_PACKET_FLAGS {
    TCP_BUILD_PACKET_FLAG_ACK           = 1 << 0,
    TCP_BUILD_PACKET_FLAG_SYNC          = 1 << 1,
    TCP_BUILD_PACKET_FLAG_PUSH          = 1 << 2,
    TCP_BUILD_PACKET_FLAG_PUSH_ON_EMPTY = 1 << 3,
    TCP_BUILD_PACKET_FLAG_RESET         = 1 << 4,
    TCP_BUILD_PACKET_FLAG_FINISH        = 1 << 5,
    TCP_BUILD_PACKET_FLAG_NO_RETRY      = 1 << 6, // when set, do not try to retransmit this packet
    TCP_BUILD_PACKET_FLAG_OPTIONS       = 1 << 7, // include tcp options in the header
};

struct tcp_build_segment_info {
    struct tcp_socket* socket;
    u8*    payload;
    u16    payload_length;
    u16    flags;
    u32    unused0;
    u32    sequence_number;
    u32    ack_number;
};

struct tcp_socket {
    struct net_socket     net_socket;

    // for incoming connections
    struct tcp_socket* pending_accept;
    struct tcp_socket* pending_accept_tail;
    struct ticketlock  accept_lock;

    struct ticketlock  main_lock;

    struct ticketlock               send_segment_queue_lock;
    struct tcp_build_segment_info** send_segment_queue;
    u32    send_segment_queue_head;
    u32    send_segment_queue_tail;
    u32    send_segment_queue_size;
    u32    unused0;

    struct buffer*    send_buffers;
    struct ticketlock send_buffers_lock;

    struct buffer*    receive_buffer;
    struct ticketlock receive_buffer_lock;

    u8     state;
    u8     unused1;
    u16    listen_backlog;
    u16    pending_accept_count;
    u16    unused2;

    u32    my_sequence_number;    // next outgoing SEQ number
    u32    their_sequence_number; // expected next incoming SEQ number

    u32    my_sequence_base; // expected next incoming SEQ number
    u32    their_sequence_base; // expected next incoming SEQ number

    u16    their_maximum_segment_size;
    u16    their_window_scale;
    u32    their_ack_number;

    struct condition receive_ready;
    struct condition connection_established;
};

struct payload_packet_info {
    struct net_receive_packet_info* packet_info;
    u64    flags;
};

// 5000 * ~1500 minus overhead = we can receive about ~7.2MiB of payload data
#define PAYLOAD_MAX_PACKET_COUNT 5000

// This checks if the ack_number in the header is strictly newer than the previously received ACK and a valid ACK according to our sequence number
#define ACK_IS_NEWER(hdr,socket) ((socket->my_sequence_number >= socket->their_ack_number && hdr->ack_number > socket->their_ack_number && hdr->ack_number <= socket->my_sequence_number) \
                               || (socket->my_sequence_number < socket->their_ack_number && (hdr->ack_number > socket->their_ack_number || hdr->ack_number <= socket->my_sequence_number)))

static s64 _queue_segment(struct tcp_socket* socket, struct buffer* payload, u16 max_payload_length, u16 flags);
static s64 _receive_segment(struct tcp_socket* socket, struct tcp_header* hdr, struct tcp_header_options* options, struct net_receive_packet_info* packet_info);

static void _add_pending_accept(struct tcp_socket* owner, struct tcp_socket* pending);

static s64 _process_send_buffers(struct tcp_socket* socket);
static s64 _process_send_segment_queue(struct tcp_socket* socket);

////////////////////////////////////////////////////////////////////////////////
// socket ops definition
////////////////////////////////////////////////////////////////////////////////
static s64                _socket_listen (struct net_socket*, u16);
static struct net_socket* _socket_accept (struct net_socket*);
static s64                _socket_connect(struct net_socket*);
static s64                _socket_update (struct net_socket* net_socket);
static s64                _socket_send   (struct net_socket* net_socket, struct buffer*);
static s64                _socket_receive(struct net_socket* net_socket, struct buffer*, u64 size);
static s64                _socket_close_lock(struct net_socket*);
static void               _socket_destroy(struct net_socket*);

static struct net_socket_ops tcp_socket_ops = {
    .listen  = &_socket_listen,
    .accept  = &_socket_accept,
    .connect = &_socket_connect,
    .close   = &_socket_close_lock,
    .destroy = &_socket_destroy,
    .send    = &_socket_send,
    .receive = &_socket_receive,
    .update  = &_socket_update,
};
////////////////////////////////////////////////////////////////////////////////

struct net_socket* tcp_socket_create(struct net_socket_info* sockinfo)
{
    declare_ticketlock(lock_init);
    declare_condition(condition_init);

    assert(sockinfo->protocol == NET_PROTOCOL_TCP, "required TCP sockinfo");

    // right now only support IPv4. TODO IPv6
    if(sockinfo->source_address.protocol != NET_PROTOCOL_IPv4 ||
       sockinfo->dest_address.protocol != NET_PROTOCOL_IPv4) return null;

    struct tcp_socket* socket = (struct tcp_socket*)kalloc(sizeof(struct tcp_socket)); // use kalloc for fast allocation
    zero(socket);

    socket->main_lock               = lock_init;
    socket->accept_lock             = lock_init;
    socket->receive_buffer_lock     = lock_init;
    socket->send_buffers_lock       = lock_init;
    socket->send_segment_queue_lock = lock_init;
    socket->receive_ready           = condition_init;
    socket->connection_established  = condition_init;

    socket->state                   = TCP_SOCKET_STATE_CLOSED;
    socket->net_socket.ops          = &tcp_socket_ops;

    return &socket->net_socket;
}

// destroy frees up the memory that a socket is using, it does not gracefully shut down a TCP socket
void tcp_socket_destroy(struct net_socket* net_socket)
{
    struct tcp_socket* socket = containerof(net_socket, struct tcp_socket, net_socket);

    if(socket->send_segment_queue) {
        free(socket->send_segment_queue);
    }

    if(socket->receive_buffer) {
        while(buffer_remaining_read(socket->receive_buffer) > 0) {
            // read the payload info
            struct payload_packet_info ppi;
            if(buffer_read(socket->receive_buffer, (u8*)&ppi, sizeof(struct payload_packet_info)) != sizeof(struct payload_packet_info)) { // major error if there's not a full structure in the buffer
                fprintf(stderr, "tcp: major buffer problem with socket 0x%lX, probably leaking memory\n", socket);
                break;
            }

            // free the packet info
            ppi.packet_info->free(ppi.packet_info);
        }

        // free the buffer
        buffer_destroy(socket->receive_buffer);
    }

    kfree(socket, sizeof(struct tcp_socket));
}

static s64 _allocate_send_segment_queue(struct tcp_socket* socket)
{
    socket->send_segment_queue_size = 32;
    socket->send_segment_queue      = (struct tcp_build_segment_info**)malloc(sizeof(struct tcp_build_segment_info*) * socket->send_segment_queue_size);
    socket->send_segment_queue_tail = 0;
    socket->send_segment_queue_head = 0;
    if(socket->send_segment_queue == null) return -ENOMEM;
    return 0;
}

static s64 _socket_listen(struct net_socket* socket, u16 backlog)
{
    struct tcp_socket* tcpsocket = containerof(socket, struct tcp_socket, net_socket);

    assert(socket->socket_info.protocol == NET_PROTOCOL_TCP, "required TCP socket");
    if(tcpsocket->state != TCP_SOCKET_STATE_CLOSED) {
        return -EINVAL;
    }

    tcpsocket->state = TCP_SOCKET_STATE_LISTEN;
    tcpsocket->listen_backlog = backlog;

    return 0;
}

static struct net_socket* _socket_accept(struct net_socket* socket)
{
    struct tcp_socket* tcpsocket = containerof(socket, struct tcp_socket, net_socket);

    while(true) {
        // wait for an incoming socket
        while(tcpsocket->state == TCP_SOCKET_STATE_LISTEN && tcpsocket->pending_accept == null) { // our main socket could at some point be closed
            task_yield(TASK_YIELD_VOLUNTARY); //TODO use events
        }    

        // break out if our socket is closed
        if(tcpsocket->state != TCP_SOCKET_STATE_LISTEN) break;

        // swap out the incoming socket for null and see if we got it
        struct tcp_socket* incoming = (struct tcp_socket*)__xchgq((u64*)&tcpsocket->pending_accept, (u64)null);
        if(incoming == null) continue; // didn't get it, try again

        // got it, so lock and update the pointers
        acquire_lock(tcpsocket->accept_lock);
        tcpsocket->pending_accept = incoming->pending_accept; // pending_accept acts as the 'next' pointer
        if(tcpsocket->pending_accept_tail == incoming) {
            tcpsocket->pending_accept_tail = null;
        }
        __atomic_dec(&tcpsocket->pending_accept_count);
        release_lock(tcpsocket->accept_lock);

        // accept all sockets regardless of their state, and let various calls like send/recv
        // check the validity of the socket and handle errors at that point in time.
        return &incoming->net_socket;
    }

    //TODO errno = -EINVAL; // not a listening socket
    return null;
}

static s64 _socket_connect(struct net_socket* net_socket)
{
    s64 res = 0;
    struct tcp_socket* socket = containerof(net_socket, struct tcp_socket, net_socket);

    acquire_lock(socket->main_lock);
    if(socket->state != TCP_SOCKET_STATE_CLOSED) {
        fprintf(stderr, "tcp: connect called on non-closed socket\n");
        res = -EINVAL;
        goto done;
    }

    // for now, our sequence number will be some magic computed from theirs. 
    // TODO we need an initial sequence number generator that's supposed to cycle with time
    socket->my_sequence_base   = 0xBEA3419C;
    socket->my_sequence_number = socket->my_sequence_base;

    // allocate the receive buffer
    socket->receive_buffer = buffer_create(PAYLOAD_MAX_PACKET_COUNT * sizeof(struct payload_packet_info)); // TODO determine the size better somehow

    // we need a send_segment_queue, so allocate it
    _allocate_send_segment_queue(socket);

    // send a SYN packet to initiate a connection
    if((res = _queue_segment(socket, null, 0, TCP_BUILD_PACKET_FLAG_SYNC | TCP_BUILD_PACKET_FLAG_OPTIONS)) < 0) {
        fprintf(stderr, "tcp: unable to send SYN\n");
        goto done;
    }

    socket->state = TCP_SOCKET_STATE_SYNC_SENT;

    // wait for our socket to connect to close
    release_lock(socket->main_lock);
    wait_condition(socket->connection_established);
    acquire_lock(socket->main_lock);

    if(socket->state != TCP_SOCKET_STATE_ESTABLISHED) {
        res = -ECONNABORTED; // TODO the correct error code
    }

done:
    release_lock(socket->main_lock);
    return res;
}

static s64 _socket_close(struct tcp_socket* socket)
{
    socket->state = TCP_SOCKET_STATE_CLOSED;
    end_condition(socket->receive_ready); // wake up all reading threads

    // send a FIN packet
    if(socket->state == TCP_SOCKET_STATE_ESTABLISHED || socket->state == TCP_SOCKET_STATE_SYNC_RECEIVED) {
        s64 res;
        if((res = _queue_segment(socket, null, 0, TCP_BUILD_PACKET_FLAG_ACK | TCP_BUILD_PACKET_FLAG_FINISH)) < 0) {
            socket->state = TCP_SOCKET_STATE_CLOSED;
            return res;
        } else {
            socket->state = TCP_SOCKET_STATE_FINISH_WAIT_1;
        }
    } else if(socket->state == TCP_SOCKET_STATE_CLOSE_WAIT) {
        s64 res;
        if((res = _queue_segment(socket, null, 0, TCP_BUILD_PACKET_FLAG_FINISH)) < 0) {
            socket->state = TCP_SOCKET_STATE_CLOSED;
            return res;
        } else {
            socket->state = TCP_SOCKET_STATE_LAST_ACK;
        }
    } else if(socket->state == TCP_SOCKET_STATE_LISTEN || socket->state == TCP_SOCKET_STATE_SYNC_SENT) {
        socket->state = TCP_SOCKET_STATE_CLOSED;
    }

    return 0;
}

static s64 _socket_close_lock(struct net_socket* net_socket)
{
    struct tcp_socket* socket = containerof(net_socket, struct tcp_socket, net_socket);
    acquire_lock(socket->main_lock);
    s64 res = _socket_close(socket);
    release_lock(socket->main_lock);
    return res;
}

static void _socket_destroy(struct net_socket* net_socket)
{
    // close the socket, and mark it for being destroyed later
    _socket_close_lock(net_socket);
}

static s64 _socket_send(struct net_socket* net_socket, struct buffer* buf)
{
    // add buf to the tail of the linked list tcp_socket.send_buffers
    struct tcp_socket* socket = containerof(net_socket, struct tcp_socket, net_socket);
    //fprintf(stderr, "tcp: queuing send buffer 0x%lX on socket 0x%lX size %lu\n", buf, socket, buffer_remaining_read(buf));

    acquire_lock(socket->send_buffers_lock);
    DEQUE_PUSH_BACK(socket->send_buffers, buf);
    release_lock(socket->send_buffers_lock);

    net_notify_socket(net_socket);
    return 0;
}

static s64 _socket_receive(struct net_socket* net_socket, struct buffer* dest, u64 size)
{
    s64 res = 0;

    // weird, but allowed
    if(size == 0) return 0;

    // receive should try and read `size` bytes, but can return early if we receive a PUSH
    //fprintf(stderr, "TODO: tcp receive socket 0x%lX dest 0x%lX size %lu/%lu\n", socket, dest, size, buffer_remaining_write(dest));
    struct tcp_socket* socket = containerof(net_socket, struct tcp_socket, net_socket);

    // try to read up until size or until a PUSH is encountered
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
        if(remaining_payloads == 0 || socket->state != TCP_SOCKET_STATE_ESTABLISHED) {
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
        bool was_push = (ppi.packet_info->packet_length == 0) && ((ppi.flags & TCP_BUILD_PACKET_FLAG_PUSH) != 0);

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

// called when there's work to be done on the socket
static s64 _socket_update(struct net_socket* net_socket)
{
    struct tcp_socket* socket = containerof(net_socket, struct tcp_socket, net_socket);
    //fprintf(stderr, "_socket_update: cpu %d\n", get_cpu()->cpu_index);

    s64 ret;
    if((ret = _process_send_buffers(socket)) < 0) return ret;

    if((ret = _process_send_segment_queue(socket)) < 0) return ret;

    // notify the network layer if there's still work left on this socket
    if(socket->send_buffers != null || socket->send_segment_queue_head != socket->send_segment_queue_tail) {
        net_notify_socket(net_socket);
    }

    return 0;
}

static void _add_pending_accept(struct tcp_socket* owner, struct tcp_socket* pending)
{
    acquire_lock(owner->accept_lock);
    if(owner->pending_accept == null) {
        pending->pending_accept = null;
        pending->pending_accept_tail = null;
        owner->pending_accept = pending;
        owner->pending_accept_tail = pending;
    } else {
        owner->pending_accept_tail->pending_accept = pending; // next pointer to the new pending connection
        owner->pending_accept_tail = pending;
    }
    release_lock(owner->accept_lock);
}

static s32 _parse_header_options(struct net_receive_packet_info* packet_info, struct tcp_header* hdr, struct tcp_header_options* options)
{
    zero(options);

    u16 option_offset = offsetof(struct tcp_header, options);
    u16 payload_start = hdr->data_offset * 4;

    u8 length;
    u16 parameter;

    // bail on invalid setup
    if(payload_start > packet_info->packet_length) return -EINVAL;

    // until the end of options
    while(option_offset < payload_start) {
        u8 opt = ((u8*)hdr)[option_offset++];

        switch(opt) {
        case TCP_OPTION_END:
            goto done;

        case TCP_OPTION_NOP:
            options->nops++;
            break;

        case TCP_OPTION_MAXIMUM_SEGMENT_SIZE:
            length = ((u8*)hdr)[option_offset++];
            if(length != 4) return -EINVAL;
            parameter = *(u16*)((u8*)hdr + option_offset);
            option_offset += 2;
            options->maximum_segment_size = ntohs(parameter);
            options->present |= TCP_OPTION_PRESENT_MAXIMUM_SEGMENT_SIZE;
            break;

        case TCP_OPTION_WINDOW_SCALE:
            length = ((u8*)hdr)[option_offset++];
            if(length != 3) return -EINVAL;
            parameter = *((u8*)hdr + option_offset);
            option_offset += 1;
            options->window_scale = parameter;
            options->present |= TCP_OPTION_PRESENT_WINDOW_SCALE;
            break;

        case TCP_OPTION_SACK_PERMITTED:
            length = ((u8*)hdr)[option_offset++];
            if(length != 2) return -EINVAL;
            options->present |= TCP_OPTION_PRESENT_SACK_PERMITTED;
            break;

        case TCP_OPTION_TIMESTAMPS:
            //TODO
            length = ((u8*)hdr)[option_offset++];
            if(length != 10) return -EINVAL;
            option_offset += 8;
            break;
        }
    }

done:
    options->padding_bytes = payload_start - option_offset;
    return payload_start;
}

void tcp_receive_packet(struct net_interface* iface, struct ipv4_header* iphdr, struct net_receive_packet_info* packet_info)
{
    struct tcp_header* hdr = (struct tcp_header*)packet_info->packet;

    if(packet_info->packet_length < sizeof(struct tcp_header)) {
        fprintf(stderr, "tcp: dropping packet (size too small = %d)\n", packet_info->packet_length);
        return;
    }

    hdr->source_port     = ntohs(hdr->source_port);
    hdr->dest_port       = ntohs(hdr->dest_port);
    hdr->sequence_number = ntohl(hdr->sequence_number);
    hdr->ack_number      = ntohl(hdr->ack_number);
    hdr->flags           = ntohs(hdr->flags);
    hdr->window          = ntohs(hdr->window);
    hdr->checksum        = ntohs(hdr->checksum);
    hdr->urgent_pointer  = ntohs(hdr->urgent_pointer);

    struct tcp_header_options options;
    s32 payload_start;
    if((payload_start = _parse_header_options(packet_info, hdr, &options)) < 0) {
        fprintf(stderr, "tcp: dropping packet due to invalid options\n");
        return;
    }

    char buf[16];
    char buf2[16];
    ipv4_format_address(buf, iphdr->source_address);
    ipv4_format_address(buf2, iphdr->dest_address);
    //fprintf(stderr, "tcp: got src=%s:%d dst=%s:%d SEQ=%lu ACK=%lu window=%lu urgent=%lu\n"
    //                "         SYN=%d ACK=%d RST=%d FIN=%d PSH=%d URG=%d\n"
    //                "         data_offset=%d checksum=0x%04X payload_length=%lu\n",
    //        buf, hdr->source_port, buf2, hdr->dest_port, hdr->sequence_number, hdr->ack_number, hdr->window, hdr->urgent_pointer,
    //        hdr->sync, hdr->ack, hdr->reset, hdr->finish, hdr->push, hdr->urgent, hdr->data_offset, hdr->checksum, packet_info->packet_length - hdr->data_offset * 4);

    // The 4-tuple (source_address, source_port, dest_address, dest_port) defines a socket 
    // Look it up first, and take action on whether the socket exists or is new.
    // flip the source and dest addresses to align with our point of view of sockets
    struct net_socket_info sockinfo = {
        .protocol           = NET_PROTOCOL_TCP,
        .source_port        = hdr->dest_port,
        .dest_port          = hdr->source_port,
    };

    zero(&sockinfo.source_address);
    zero(&sockinfo.dest_address);
    sockinfo.dest_address.protocol   = NET_PROTOCOL_IPv4;
    sockinfo.dest_address.ipv4       = iphdr->source_address;
    sockinfo.source_address.protocol = NET_PROTOCOL_IPv4;
    sockinfo.source_address.ipv4     = iphdr->dest_address;

    // Look up the net_socket
    // TODO only create sockets on connection-style segments
    struct net_socket* net_socket = net_socket_lookup(&sockinfo);
    struct tcp_socket* socket = containerof(net_socket, struct tcp_socket, net_socket);
    if(net_socket == null) {
        // look up listening socket. a listening socket will have the dest address of 0.0.0.0 and port as 0,
        // and the dest address can either be one belonging to a net_interface or also 0 to accept all incoming addresses
        sockinfo.dest_address.ipv4 = 0;
        sockinfo.dest_port         = 0;

        net_socket = net_socket_lookup(&sockinfo);
        if(net_socket == null) {
            // no listening socket directed at ip:port, try 0.0.0.0:port
            sockinfo.source_address.ipv4 = 0;
            net_socket = net_socket_lookup(&sockinfo);
        }

        // validate that the socket is in a listening state
        struct tcp_socket* listen_socket = containerof(net_socket, struct tcp_socket, net_socket);
        if(net_socket == null || listen_socket->state != TCP_SOCKET_STATE_LISTEN) {
            // TODO no listening socket, so reply with ICMP host port unreachable
            fprintf(stderr, "tcp: no listening socket on %s:%d found, dropping packet\n", buf2, sockinfo.dest_port);
            goto done;
        }

        u16 incoming_index = __atomic_xinc(&listen_socket->pending_accept_count);
        if(incoming_index >= listen_socket->listen_backlog) {
            // too many incoming connections, so we drop this one
            __atomic_dec(&listen_socket->pending_accept_count);
            fprintf(stderr, "tcp: dropping due to too many incoming connections (%d > %d)\n", incoming_index, listen_socket->listen_backlog);
            goto done;
        }

        // potential new socket formed to a listening socket, so create a new socket and handle the packet
        // the handling of the packet may not persist for very long
        ipv4_format_address(buf2, sockinfo.source_address.ipv4);
        fprintf(stderr, "tcp: found listening socket %s:%d\n", buf2, sockinfo.dest_port);

        // create a new socket for this new data
        sockinfo.dest_address.ipv4   = iphdr->source_address;
        sockinfo.dest_port           = hdr->source_port;
        sockinfo.source_address.ipv4 = iphdr->dest_address;
        struct net_socket* newsocket = net_socket_create(iface, &sockinfo);
        if(newsocket == null) {
            __atomic_dec(&listen_socket->pending_accept_count);
            goto done; // failed to allocate socket, so out of memory or something
        }

        // put new socket on the pending accept list, even if the connection is never fully established
        _add_pending_accept(listen_socket, containerof(newsocket, struct tcp_socket, net_socket));

        // deliver the packet to this socket
        socket = containerof(newsocket, struct tcp_socket, net_socket);
        socket->state = TCP_SOCKET_STATE_LISTEN;
    }

    if(socket != null) {
        // update the packet info before sending it on
        packet_info->packet         = (u8*)hdr + (u16)(payload_start & 0xFFFF);
        packet_info->packet_length -= (u16)(payload_start & 0xFFFF);

        acquire_lock(socket->main_lock);
        _receive_segment(socket, hdr, &options, packet_info);
        release_lock(socket->main_lock);
    } else {
        // packet wasn't used, free it
        packet_info->free(packet_info);
    }

done:
    return;
}

// packet_info has been updated to point to the payload
static s64 _receive_payload(struct tcp_socket* socket, struct tcp_header* hdr, struct net_receive_packet_info* packet_info)
{
    if(packet_info->packet_length == 0 /*TODO || packet_info->packet_length > socket->max_payload*/ ) return 0;

    acquire_lock(socket->receive_buffer_lock);

    u32 before_read_size = buffer_remaining_read(socket->receive_buffer);
    struct payload_packet_info ppi = {
        .packet_info = packet_info,
        .flags       = hdr->push ? TCP_BUILD_PACKET_FLAG_PUSH : 0,
    };

    if(buffer_remaining_write(socket->receive_buffer) < sizeof(ppi)) {
        fprintf(stderr, "tcp: incoming buffer for socket 0x%lX is full, dropping packet\n", socket);
        release_lock(socket->receive_buffer_lock);
        return 0;
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

    // returning the size of the packet indicates it's been accepted
    return packet_info->packet_length;
}

static s64 _receive_segment(struct tcp_socket* socket, struct tcp_header* hdr, struct tcp_header_options* options, struct net_receive_packet_info* packet_info)
{
    s64 res = 0;
    bool free_packet_info = true;

    u16 payload_length = packet_info->packet_length;

    //fprintf(stderr, "tcp: receive segment on socket 0x%lX SYN=%d ACK=%d RST=%d PUSH=%d payload=%d bytes\n", socket, hdr->sync, hdr->ack, hdr->reset, hdr->push, payload_length);

    if(hdr->reset) {
        fprintf(stderr, "tcp: TODO socket 0x%lX got RST..closing socket for now\n", socket);
        goto close_done;
    }

    if(hdr->finish && socket->state != TCP_SOCKET_STATE_ESTABLISHED) {
        // TODO technically a FIN packet could have data, but we would only want to receive that data if a connection is established
        fprintf(stderr, "tcp: TODO socket 0x%lX got FIN before being established..closing socket for now\n", socket);
        goto close_done;
    }

    // ACK value is valid
    if(hdr->ack) {
        //fprintf(stderr, "tcp: received ACK for %lu\n", hdr->ack_number);

        // if their ACK is valid, then handle that
        if((hdr->ack_number != socket->their_ack_number) && !ACK_IS_NEWER(hdr, socket)) {
            fprintf(stderr, "tcp: ACK received was incorrect (got %lu wanted between %lu..%lu)\n", hdr->ack_number, socket->their_ack_number, socket->my_sequence_number);
            goto close_done;
        }

        // update their ACK
        socket->their_ack_number = hdr->ack_number;

        // TODO free retransmission data
    }

    // If these options are sent on any packet other than connect packets, it's invalid
    bool is_connect = hdr->sync && !hdr->reset; // SYN+ACK resposes can have options
    if((options->present & (TCP_OPTION_PRESENT_MAXIMUM_SEGMENT_SIZE | TCP_OPTION_PRESENT_WINDOW_SCALE)) != 0 && !is_connect) {
        fprintf(stderr, "tcp: can only send maximum segment size with SYN packets\n");
        goto close_done;
    }

    switch(socket->state) {
    case TCP_SOCKET_STATE_LISTEN:
        // the only packet in LISTEN state we care about is SYN=1,ACK=0. RST is taken care of above and any payload is ignored
        if(!is_connect) {
            // TODO net_destroy_socket(&socket->net_socket); // I don't think we can destroy the socket while we're in a function using it
                                                             // the destroying process should get moved to a cleanup function
            goto close_done;
        }

        // check for valid segment sizes (must be at least 64, but 0 means unlimited)
        if(options->present & TCP_OPTION_PRESENT_MAXIMUM_SEGMENT_SIZE) {
            if(options->maximum_segment_size != 0 && options->maximum_segment_size < 64) goto close_done;
            socket->their_maximum_segment_size = options->maximum_segment_size;
        }

        // window scale is given as log-2
        if(options->present & TCP_OPTION_PRESENT_WINDOW_SCALE) {
            socket->their_window_scale = options->window_scale;
        }

        // setup their base sequence_number
        socket->their_sequence_base = hdr->sequence_number;

        // SYN flag counts towards sequence number before payload
        socket->their_sequence_number = hdr->sequence_number + hdr->sync;

        // for now, our sequence number will be some magic computed from theirs. 
        // TODO we need an initial sequence number generator that's supposed to cycle with time
        socket->my_sequence_base = ~((hdr->sequence_number >> 16) | ((hdr->sequence_number & 0xFFFF) << 16));
        socket->my_sequence_number = socket->my_sequence_base;

        // start their ACK at our sequence_number before we transmit SYN, as this lets ACK_IS_NEWER work correctly
        socket->their_ack_number = socket->my_sequence_base;

        // allocate the receive buffer
        socket->receive_buffer = buffer_create(PAYLOAD_MAX_PACKET_COUNT * sizeof(struct payload_packet_info)); // TODO determine the size better somehow

        // receive the payload
        if(payload_length > 0) {
            if((res = _receive_payload(socket, hdr, packet_info)) < 0) {
                // error receiving payload
                goto close_done;
            } else if(res == 0) {
                // did not process the entire payload, maybe because the receive buffer was full
                // if this happens during connection, then there's some weird error and we fail out
                goto close_done;
            } else {
                // packet was queued, so don't free it
                free_packet_info = false;
            }
        } else res = 0;

        // update their_sequence_number for the payload, which is technically allowed in a connect segment
        socket->their_sequence_number += res;

        // now is the time that we need a send_segment_queue, so allocate it
        _allocate_send_segment_queue(socket);

        // send SYN+ACK
        if((res = _queue_segment(socket, null, 0, TCP_BUILD_PACKET_FLAG_SYNC | TCP_BUILD_PACKET_FLAG_ACK | TCP_BUILD_PACKET_FLAG_OPTIONS)) < 0) {
            // internal error, we need to error out
            goto close_done;
        }

        // wait for ACK
        socket->state = TCP_SOCKET_STATE_SYNC_RECEIVED;
        break;

    case TCP_SOCKET_STATE_SYNC_SENT:
        if(!hdr->sync || !hdr->ack) {
            fprintf(stderr, "tcp: TODO we should get an SYN+ACK after sending a SYN packet\n");
            goto close_done;
        }

        if(hdr->finish) {
            fprintf(stderr, "tcp: got FIN in SYNC_SENT state\n");
            goto close_done;
        }

        // validate the ACK
        if(hdr->ack_number != socket->my_sequence_number) {
            fprintf(stderr, "tcp: got incorrect ACK in SYNC_SENT state\n");
            goto close_done;
        }

        // accept their base sequence_number
        socket->their_sequence_base = hdr->sequence_number;

        // SYN flag counts towards sequence number before payload
        socket->their_sequence_number = hdr->sequence_number + hdr->sync;

        // their ACK is valid
        socket->their_ack_number = hdr->ack_number;

        // receive the payload
        if(payload_length > 0) {
            if((res = _receive_payload(socket, hdr, packet_info)) < 0) {
                // error receiving payload
                goto close_done;
            } else if(res == 0) {
                // did not process the entire payload, maybe because the receive buffer was full
                // if this happens during connection, then there's some weird error and we fail out
                goto close_done;
            } else {
                // packet was queued, so don't free it
                free_packet_info = false;
            }
        } else res = 0;

        // send ACK
        if((res = _queue_segment(socket, null, 0, TCP_BUILD_PACKET_FLAG_ACK)) < 0) {
            // internal error, we need to error out
            goto close_done;
        }

        // connection established
        //fprintf(stderr, "tcp: socket 0x%lX ESTABLISHED\n", socket);
        socket->state = TCP_SOCKET_STATE_ESTABLISHED;
        notify_condition(socket->connection_established);
        break;

    case TCP_SOCKET_STATE_SYNC_RECEIVED:
        // wait for ACK of our sequence. if we get any packet that doesn't have an ACK
        // then the peer sent something out of order and we need to handle that
        if(hdr->sync || !hdr->ack) {
            fprintf(stderr, "tcp: TODO got second SYN after receiving SYN already\n");
            goto close_done;
        }

        // if we get here, that means the ACK processed above was valid and connection is established
        //fprintf(stderr, "tcp: socket 0x%lX ESTABLISHED\n", socket);
        socket->state = TCP_SOCKET_STATE_ESTABLISHED;
        notify_condition(socket->connection_established);
        break;

    case TCP_SOCKET_STATE_ESTABLISHED:
        {
            // check the sequence # to see if this is new data
            // TODO check that the sequence_number falls within our window of data
            // (if it doesn't, then the peer isn't playing nice and we should bail on them)
            // and then add it to an internal buffer
            // TODO maybe their_sequence_number should be 64-bit and we don't have to worry about overflow, as 4GiB isn't that large now
            // we don't keep packets that come out of order. they're somewhat rare and the overhead isn't terribly significant. when we get a newer
            // packet, we resend the previous ACK.  if the packet is older, then the peer might not have gotten our previous ACK. Only the case
            // where the incoming sequence number matches what we expect do we receive the payload
            if(hdr->sequence_number != socket->their_sequence_number) { // if the incoming packet matches the packet we expect
                fprintf(stderr, "tcp: got old or out of order packet (got sequence %d, expected %d)\n", hdr->sequence_number, socket->their_sequence_number);
                _queue_segment(socket, null, 0, TCP_BUILD_PACKET_FLAG_ACK);
                goto done;
            }

            // new sequence number, check for SYN flag
            if(hdr->sync) {
                fprintf(stderr, "tcp: TODO got SYN in state ESTABLISHED\n");
                goto close_done;
            }

            // deliver a payload if there is one
            if(payload_length > 0) {
                if((res = _receive_payload(socket, hdr, packet_info)) < 0) {
                    // internal error
                    goto close_done;
                } else if(res == 0) {
                    // did not process the entire payload, maybe because the receive buffer was full
                    // in the case that our buffer is full, we just drop the packet but maintain the connection
                } else {
                    // packet was queued, so don't free it
                    free_packet_info = false;
                }
            } else res = 0;

            // packet was received, update their sequence number
            u16 seq_inc = res + hdr->finish;
            socket->their_sequence_number += seq_inc;

            if(seq_inc > 0) { // send an ACK if sequence numbers changed
                _queue_segment(socket, null, 0, TCP_BUILD_PACKET_FLAG_ACK);
            }

            // check if this is a FIN (close) packet after processing data, since TCP allows data in them
            if(hdr->finish) {
                //fprintf(stderr, "tcp: got FIN request\n");
                socket->state = TCP_SOCKET_STATE_CLOSE_WAIT;
                end_condition(socket->receive_ready); // wake up all reading threads
                goto done;
            }
        }
        break;

    case TCP_SOCKET_STATE_CLOSE_WAIT:
        // we just wait here for someone to call net_socket_close()
        break;

    case TCP_SOCKET_STATE_LAST_ACK:
        // wait for an ACK of our fin
        if(hdr->sync) {
            fprintf(stderr, "tcp: got SYN in STATE_LAST_ACK, force closing\n");
            goto close_done;
        }

        if(hdr->ack) {
            if(hdr->ack_number == socket->my_sequence_number) { // wait for the correct ACK
                fprintf(stderr, "tcp: got last ACK\n");
                socket->state = TCP_SOCKET_STATE_CLOSED;
            } // otherwise, ACK was old we can ignore it
        }

        break;

    case TCP_SOCKET_STATE_FINISH_WAIT_1:
        // sent FIN, wait for ACK or FIN

        if(hdr->sync) {
            fprintf(stderr, "tcp: got SYN in STATE_FINISH_WAIT_1, force closing\n");
            goto close_done;
        }

        if(hdr->ack && !hdr->finish) {
            if(hdr->ack_number == socket->my_sequence_number) { // wait for the correct ACK
                fprintf(stderr, "tcp: got ACK of FIN (FINISH_WAIT_1)\n");
                socket->state = TCP_SOCKET_STATE_FINISH_WAIT_2;
            } // otherwise, ACK was old we can ignore it
        } else if(!hdr->ack && hdr->finish) {
            fprintf(stderr, "tcp: got FIN (FINISH_WAIT_1)\n");
            socket->their_sequence_number += 1;
            if((res = _queue_segment(socket, null, 0, TCP_BUILD_PACKET_FLAG_ACK)) < 0) {
                fprintf(stderr, "tcp: error %d queuing segment\n", res);
                goto close_done;
            }
            socket->state = TCP_SOCKET_STATE_CLOSING;
        } else {
            fprintf(stderr, "tcp: got invalid combination of ACK and FIN, force closing (FINISH_WAIT_1)\n");
            socket->state = TCP_SOCKET_STATE_CLOSED;
        }
        break;

    case TCP_SOCKET_STATE_FINISH_WAIT_2:
        // received ACK of FIN, wait for FIN

        if(hdr->sync) {
            fprintf(stderr, "tcp: got SYN in FINISH_WAIT_2, force closing\n");
            goto close_done;
        }

        if(hdr->finish) {
            socket->their_sequence_number += 1;

            if((res = _queue_segment(socket, null, 0, TCP_BUILD_PACKET_FLAG_ACK)) < 0) {
                fprintf(stderr, "tcp: error %d queuing segment\n", res);
                goto close_done;
            }
            socket->state = TCP_SOCKET_STATE_CLOSED;
        }
        break;

    case TCP_SOCKET_STATE_CLOSING:
        // wait for ACK of FIN
        if(hdr->ack) {
            if(hdr->ack_number == socket->my_sequence_number) { // wait for the correct ACK
                fprintf(stderr, "tcp: got ACK of FIN (STATE_CLOSING)\n");
                socket->state = TCP_SOCKET_STATE_CLOSED;
            } // otherwise, ACK was old we can ignore it
        }
        break;

    default:
        fprintf(stderr, "tcp: unhandled state %d\n", socket->state);
        break;
    }

    goto done;

close_done:
    fprintf(stderr, "tcp: closing socket 0x%lX\n", socket);
    _socket_close(socket);

done:
    // free the packet
    if(free_packet_info) packet_info->free(packet_info);
    return res < 0 ? res : 0;
}

static u16 _compute_checksum(u32 source_address, u32 dest_address, u8* data, u16 data_length)
{
    u32 sum = 0;

    u32 src = htonl(source_address);
    u32 dst = htonl(dest_address);

    sum += (*((u8*)&src + 1) << 8) | (*((u8*)&src + 0) << 0);
    sum += (*((u8*)&src + 3) << 8) | (*((u8*)&src + 2) << 0);
    sum += (*((u8*)&dst + 1) << 8) | (*((u8*)&dst + 0) << 0);
    sum += (*((u8*)&dst + 3) << 8) | (*((u8*)&dst + 2) << 0);
    sum += ((u16)IPv4_PROTOCOL_TCP << 8);
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

static s64 _build_tcp_segment(struct net_send_packet_queue_entry* entry, u8* tcp_packet_start, void* userdata)
{
    unused(entry);

    struct tcp_header* hdr              = (struct tcp_header*)tcp_packet_start;
    struct tcp_build_segment_info* info = (struct tcp_build_segment_info*)userdata;
    struct tcp_socket* socket           = info->socket;

    u16 tcp_header_size = sizeof(struct tcp_header); // TODO + options
    u16 tcp_packet_size = tcp_header_size + info->payload_length;

    // start with a zero header
    zero(hdr);

    // setup header
    hdr->sequence_number = htonl(info->sequence_number);
    if(info->flags & TCP_BUILD_PACKET_FLAG_ACK) {
        hdr->ack_number = htonl(info->ack_number);
        hdr->ack        = 1;
    }

    // flip the ports so that destination is going to their source
    hdr->source_port = htons(socket->net_socket.socket_info.source_port);
    hdr->dest_port   = htons(socket->net_socket.socket_info.dest_port);

    hdr->sync   = (info->flags & TCP_BUILD_PACKET_FLAG_SYNC)   != 0 ? 1 : 0;
    hdr->push   = (info->flags & TCP_BUILD_PACKET_FLAG_PUSH)   != 0 ? 1 : 0;
    hdr->finish = (info->flags & TCP_BUILD_PACKET_FLAG_FINISH) != 0 ? 1 : 0;

    hdr->data_offset    = tcp_header_size / 4;
    hdr->window         = htons(1500 * 200 / 64); // TODO scale the window with the # of remaining packet space
    hdr->urgent_pointer = 0;

    hdr->flags          = htons(hdr->flags); // flip the flags byte
    hdr->checksum       = 0;  // initialize checksum to 0

    // TODO options, especially window scale

    // copy payload over
    memcpy(tcp_packet_start + tcp_header_size, info->payload, info->payload_length);

    // compute checksum and convert to network byte order
    // dest_address = them, source_address = us
    hdr->checksum = _compute_checksum(socket->net_socket.socket_info.source_address.ipv4, socket->net_socket.socket_info.dest_address.ipv4, tcp_packet_start, tcp_packet_size);

    return tcp_packet_size;
}

static s64 _queue_segment(struct tcp_socket* socket, struct buffer* payload, u16 max_payload_length, u16 flags)
{
    u16 payload_length = 0;
    u8* payload_buffer = null;

    // create a copy of the payload for use later
    if(payload != null) {
        payload_buffer = (u8*)malloc(max_payload_length);
        payload_length = buffer_read(payload, payload_buffer, max_payload_length);
    }

    // change PUSH_ON_EMPTY to PUSH if payload buffer is empty
    if((flags & TCP_BUILD_PACKET_FLAG_PUSH_ON_EMPTY) != 0) {
        flags &= ~TCP_BUILD_PACKET_FLAG_PUSH_ON_EMPTY;

        if(payload != null && buffer_remaining_read(payload) == 0) {
            flags |= TCP_BUILD_PACKET_FLAG_PUSH;
        }
    }

    // we can't build the packet here since it may need to be retransmitted with different a ACK, among other
    // flags that may change during retransmission
    struct tcp_build_segment_info* info = (struct tcp_build_segment_info*)kalloc(sizeof(struct tcp_build_segment_info));
    info->socket          = socket;
    info->payload         = payload_buffer;
    info->payload_length  = payload_length;
    info->sequence_number = socket->my_sequence_number;
    info->ack_number      = socket->their_sequence_number;
    info->flags           = flags;

    // update my sequence number
    u32 seq_inc = payload_length;
    if(flags & TCP_BUILD_PACKET_FLAG_SYNC) seq_inc++;
    //TODO probably FINISH needs seq_inc too
    socket->my_sequence_number += seq_inc;

    //fprintf(stderr, "tcp: queuing segment payload_length %d SYN=%d,ACK=%d,PSH=%d,FIN=%d seq=%lu ack=%lu\n", payload_length, (flags & TCP_BUILD_PACKET_FLAG_SYNC) ? 1 : 0, 
    //        (flags & TCP_BUILD_PACKET_FLAG_ACK) ? 1 : 0, (flags & TCP_BUILD_PACKET_FLAG_PUSH) ? 1 : 0, 
    //        (flags & TCP_BUILD_PACKET_FLAG_FINISH) ? 1 : 0, info->sequence_number, info->ack_number);

    // stick info into the tx queue
    acquire_lock(socket->send_segment_queue_lock);
    u32 slot = socket->send_segment_queue_tail;
    if(((slot + 1) % socket->send_segment_queue_size) == socket->send_segment_queue_head) {
        // we're blocked, we can't add data to the send queue so we either need to yield or drop the data
        fprintf(stderr, "tcp: TODO send queue is full. handle this case properly"); // TODO
        if(payload_buffer != null) free(payload_buffer);
        kfree(info, sizeof(struct tcp_build_segment_info));
        return -EAGAIN;
    }

    assert(socket->send_segment_queue != null, "must call _allocate_send_segment_queue before _queue_segment");
    socket->send_segment_queue[slot] = info;
    socket->send_segment_queue_tail = (socket->send_segment_queue_tail + 1) % socket->send_segment_queue_size; // only increment after send_segment_queue[slot] is valid
    release_lock(socket->send_segment_queue_lock);

    // tell the network layer this socket has work to do
    net_notify_socket(&socket->net_socket);

    //fprintf(stderr, "tcp: queued slot %d send_segment_queue=%d/%d\n", slot, socket->send_segment_queue_head, socket->send_segment_queue_tail);

    return 0;
}

static s64 _process_send_buffers(struct tcp_socket* socket)
{
    s64 ret = 0;
    
    // only send data if we're in the established data
    if(socket->state != TCP_SOCKET_STATE_ESTABLISHED) return 0;

    acquire_lock(socket->send_buffers_lock);
    while(socket->send_buffers != null) {
        struct buffer* curbuf = socket->send_buffers;

        // remove buffer when it is empty
        if(buffer_remaining_read(curbuf) == 0) {
//            fprintf(stderr, "tcp: freeing buf 0x%lX\n", curbuf);
            DEQUE_POP_FRONT(socket->send_buffers, curbuf);
            buffer_destroy(curbuf);
            continue;
        }

        // read the data from the buffer
//        fprintf(stderr, "tcp: queuing from send buffer 0x%lX on socket 0x%lX (%d remaining)\n", curbuf, socket, buffer_remaining_read(curbuf));

        // and queue up a data packet
        u16 queue_flags = TCP_BUILD_PACKET_FLAG_ACK | TCP_BUILD_PACKET_FLAG_PUSH_ON_EMPTY; // PUSH_ON_EMPTY = PUSH will be added if curbuf is empty

        // TEMP safe TODO use MTU
        // if _queue_segment reads all the data from curbuf, it'll be freed in the next loop
        if((ret = _queue_segment(socket, curbuf, min(1200, buffer_remaining_read(curbuf)), queue_flags)) < 0) break;
    }
    release_lock(socket->send_buffers_lock);

    return ret;
}

// TODO respect the peer's recv window
static s64 _process_send_segment_queue(struct tcp_socket* socket)
{
    s64 ret;

    // loop over the send queue and build as many packets as the network interface will allow
    while(socket->send_segment_queue_head != socket->send_segment_queue_tail) {
        // grab the next packet info to build
        struct tcp_build_segment_info* info = socket->send_segment_queue[socket->send_segment_queue_head];

        // ask the network interface for a tx queue slot
        struct net_send_packet_queue_entry* entry;
        ret = net_request_send_packet_queue_entry(socket->net_socket.net_interface, &socket->net_socket, &entry);
        if(ret == -EAGAIN) return 0; // safe, we will try again later
        else if(ret < 0) return ret; // return other errors

        // build a packet with the given info
        u16 tcp_header_size = sizeof(struct tcp_header) /* + options */;
        u16 tcp_packet_size = tcp_header_size + info->payload_length;
        if((ret = entry->net_interface->wrap_packet(entry, &socket->net_socket.socket_info.dest_address, NET_PROTOCOL_TCP, tcp_packet_size, &_build_tcp_segment, info)) < 0) return ret;

        // packet is ready for transmission, queue it in the network layer
        //fprintf(stderr, "tcp: net send for packet seq=%lu send_segment_queue=%d/%d (processed on task_id=%d)\n", 
        //        info->sequence_number, socket->send_segment_queue_head, socket->send_segment_queue_tail, get_cpu()->current_task->task_id); 
        net_ready_send_packet_queue_entry(entry);

        // packet was delivered to the network layer, and so it should get sent soon. we can mark it 
        // as having been sent. we don't need a lock on send_segment_queue since only a single thread will update
        // a socket at a time
        socket->send_segment_queue_head = (socket->send_segment_queue_head + 1) % socket->send_segment_queue_size;
    }

    return 0;
}

