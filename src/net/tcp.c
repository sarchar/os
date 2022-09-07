#include "kernel/common.h"

#include "kernel/cpu.h"
#include "kernel/kalloc.h"
#include "kernel/kernel.h"
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
    TCP_BUILD_PACKET_FLAG_ACK     = 1 << 0,
    TCP_BUILD_PACKET_FLAG_SYNC    = 1 << 1,
    TCP_BUILD_PACKET_FLAG_PUSH    = 1 << 2,
    TCP_BUILD_PACKET_FLAG_RESET   = 1 << 3,
    TCP_BUILD_PACKET_FLAG_FINISH  = 1 << 4
};

struct tcp_build_packet_info {
    struct tcp_socket* socket;
    u8*    payload;
    u16    payload_length;
    u16    flags;
};

struct tcp_socket {
    struct net_socket net_socket;

    u8  state;
    u8  unused0;
    u16 listen_backlog;
    u32 their_ack_number;

    u32 my_sequence_number;    // next outgoing SEQ number
    u32 their_sequence_number; // expected next incoming SEQ number
};

static s64 _socket_listen(struct net_socket*, u16);

static s64 _send_segment(struct net_interface* iface, struct tcp_socket* socket, u8* payload, u16 payload_length, u16 flags);
static s64 _receive_packet(struct net_interface* iface, struct tcp_socket* socket, struct tcp_header* hdr, u16 packet_length);

static struct net_socket_ops tcp_socket_ops = {
    .listen  = &_socket_listen,
    .accept  = null,
    .connect = null,
    .close   = null,
    .send    = null,
    .receive = null
};

struct net_socket* tcp_create_socket(struct net_socket_info* sockinfo)
{
    assert(sockinfo->protocol == NET_PROTOCOL_TCP, "required TCP sockinfo");

    // right now only support IPv4. TODO IPv6
    if(sockinfo->source_address.protocol != NET_PROTOCOL_IPv4 ||
       sockinfo->dest_address.protocol != NET_PROTOCOL_IPv4) return null;

    struct tcp_socket* sock = (struct tcp_socket*)kalloc(sizeof(struct tcp_socket)); // use kalloc for fast allocation
    zero(sock);

    sock->state           = TCP_SOCKET_STATE_CLOSED;
    sock->net_socket.ops  = &tcp_socket_ops;

    return &sock->net_socket;
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

void tcp_receive_packet(struct net_interface* iface, struct ipv4_header* iphdr, u8* packet, u16 packet_length)
{
    struct tcp_header* hdr = (struct tcp_header*)packet;

    if(packet_length < sizeof(struct tcp_header)) {
        fprintf(stderr, "tcp: dropping packet (size too small = %d)\n", packet_length);
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

    //TODO parse options: _packet_parse_options(hdr, &options);

    char buf[16];
    char buf2[16];
    ipv4_format_address(buf, iphdr->source_address);
    ipv4_format_address(buf2, iphdr->dest_address);
    fprintf(stderr, "tcp: got source=%s:%d dest=%s:%d seq_number=%lu ack_number=%d window=%d urgent_pointer=%d\n",
            buf, hdr->source_port, buf2, hdr->dest_port, hdr->sequence_number, hdr->ack_number, hdr->window, hdr->urgent_pointer);
    fprintf(stderr, "        syn=%d ack=%d rst=%d fin=%d push=%d urgent=%d data_offset=%d checksum=0x%04X\n",
            hdr->sync, hdr->ack, hdr->reset, hdr->finish, hdr->push, hdr->urgent, hdr->data_offset, hdr->checksum);

    // The 4-tuple (source_address, source_port, (dest_address, dest_port) defines a socket 
    // Look it up first, and take action on whether the socket exists or is new.
    struct net_socket_info sockinfo = {
        .protocol           = NET_PROTOCOL_TCP,
        .source_port        = hdr->source_port,
        .dest_port          = hdr->dest_port,
    };
    zero(&sockinfo.source_address);
    zero(&sockinfo.dest_address);
    sockinfo.source_address.protocol = NET_PROTOCOL_IPv4;
    sockinfo.source_address.ipv4     = iphdr->source_address;
    sockinfo.dest_address.protocol   = NET_PROTOCOL_IPv4;
    sockinfo.dest_address.ipv4       = iphdr->dest_address;

    // Look up the socket
    struct net_socket* socket = net_lookup_socket(&sockinfo);
    struct tcp_socket* tcpsocket = containerof(socket, struct tcp_socket, net_socket);
    if(socket == null) {
        // look up listening socket. a listening socket will have the source address of 0.0.0.0 and port as 0,
        // and the dest address can either be one belonging to a net_interface or also 0 to accept all incoming addresses
        sockinfo.source_address.ipv4 = 0;
        sockinfo.source_port         = 0;

        socket = net_lookup_socket(&sockinfo);
        if(socket == null) {
            // no listening socket directed at ip:port, try 0.0.0.0:port
            sockinfo.dest_address.ipv4 = 0;
            socket = net_lookup_socket(&sockinfo);
        }

        // validate that the socket is in a listening state
        tcpsocket = containerof(socket, struct tcp_socket, net_socket);
        if(socket == null || tcpsocket->state != TCP_SOCKET_STATE_LISTEN) {
            // TODO no listening socket, so reply with ICMP host port unreachable
            fprintf(stderr, "tcp: no listening socket on %s:%d found, dropping packet\n", buf2, sockinfo.dest_port);
            goto done;
        }

        // potential new socket formed to a listening socket, so create a new socket and handle the packet
        // the handling of the packet may not persist for very long
        ipv4_format_address(buf2, sockinfo.dest_address.ipv4);
        fprintf(stderr, "tcp: found listening socket %s:%d\n", buf2, sockinfo.dest_port);

        // create a new socket for this new data
        sockinfo.source_address.ipv4 = iphdr->source_address;
        sockinfo.source_port         = hdr->source_port;
        sockinfo.dest_address.ipv4   = iphdr->dest_address;
        struct net_socket* newsocket = net_create_socket(&sockinfo);
        if(newsocket == null) goto done; // failed to allocate socket, so out of memory or something

        // deliver the packet to this socket
        tcpsocket = containerof(newsocket, struct tcp_socket, net_socket);
    }

    _receive_packet(iface, tcpsocket, hdr, packet_length);

done:
    return;
}

static s64 _receive_packet(struct net_interface* iface, struct tcp_socket* socket, struct tcp_header* hdr, u16 packet_length)
{
    s64 res = 0;

    //fprintf(stderr, "tcp: got packet on socket 0x%lX syn=%d ack=%d\n", socket, hdr->sync, hdr->ack);

    if(hdr->reset) {
        fprintf(stderr, "tcp: socket 0x%lX got RST..ignoring\n", socket);
        return 0;
    }

    // ACK value is valid
    if(hdr->ack) {
        fprintf(stderr, "tcp: received ACK 0x%08X\n", hdr->ack_number);
    }

    switch(socket->state) {
    case TCP_SOCKET_STATE_CLOSED:
        // the only packet in CLOSED state we care about is SYN=1,ACK=0
        if(!hdr->sync || hdr->ack) goto done;

        // read the sync value (+1 for the sync sequence)
        socket->their_sequence_number = hdr->sequence_number + 1;

        // for now, our sequence number will be some magic computed from theirs. 
        // TODO we need an initial sequence number generator that's supposed to cycle with time
        socket->my_sequence_number = ~((hdr->sequence_number >> 16) | ((hdr->sequence_number & 0xFFFF) << 16));

        // send SYN-ACK
        if((res = _send_segment(iface, socket, null, 0, TCP_BUILD_PACKET_FLAG_SYNC | TCP_BUILD_PACKET_FLAG_ACK)) < 0) {
            // internal error, we need to error out
            // TODO close the net_socket?
            goto done;
        }

        socket->state = TCP_SOCKET_STATE_SYNC_RECEIVED;
        break;

    case TCP_SOCKET_STATE_SYNC_RECEIVED:
        // wait for ACK of our sequence
        if(hdr->sync || !hdr->ack) {
            assert(false, "second SYN after receiving SYN already. TODO");
            socket->state = TCP_SOCKET_STATE_CLOSED;
            goto done;
        }

        // if their ACK matches doesn't sequence number, then handle that
        if(hdr->ack_number != socket->my_sequence_number) {
            assert(false, "ACK was invalid");
            socket->state = TCP_SOCKET_STATE_CLOSED;
            goto done;
        }

        fprintf(stderr, "tcp: socket 0x%lX ESTABLISHED\n", socket);
        socket->state = TCP_SOCKET_STATE_ESTABLISHED;
        goto done;

    case TCP_SOCKET_STATE_ESTABLISHED:
        {
            u16 payload_length = packet_length - hdr->data_offset * 4;
            fprintf(stderr, "tcp: got data packet size %d\n", payload_length);

            // compute their next sequence number
            u16 seq_inc = payload_length;
            if(hdr->sync) seq_inc += 1;
            socket->their_sequence_number += seq_inc;

            // send an ACK
            if(seq_inc > 0) {
                if(payload_length > 0) {
                    _send_segment(iface, socket, (u8*)hdr + hdr->data_offset * 4, (hdr->sync) ? seq_inc - 1 : seq_inc, TCP_BUILD_PACKET_FLAG_ACK | TCP_BUILD_PACKET_FLAG_PUSH);
                } else {
                    _send_segment(iface, socket, null, 0, TCP_BUILD_PACKET_FLAG_ACK);
                }
            }
        }
        break;

    default:
        fprintf(stderr, "tcp: unhandled state %d\n", socket->state);
        break;
    }

done:
    return res;
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

static s64 _build_tcp_packet(struct net_interface* iface, u8* tcp_packet_start, void* userdata)
{
    unused(iface);


    struct tcp_header* hdr             = (struct tcp_header*)tcp_packet_start;
    struct tcp_build_packet_info* info = (struct tcp_build_packet_info*)userdata;
    struct tcp_socket* socket          = info->socket;

    u16 tcp_packet_size = sizeof(struct tcp_header) + info->payload_length;

    // start with a zero header
    zero(hdr);

    // setup header
    hdr->sequence_number = htonl(socket->my_sequence_number);
    if(info->flags & TCP_BUILD_PACKET_FLAG_ACK) {
        hdr->ack_number = htonl(socket->their_sequence_number);
        hdr->ack        = 1;
    }

    // flip the ports so that destination is going to their source
    hdr->source_port = htons(socket->net_socket.socket_info.dest_port);
    hdr->dest_port   = htons(socket->net_socket.socket_info.source_port);

    hdr->sync = (info->flags & TCP_BUILD_PACKET_FLAG_SYNC) != 0 ? 1 : 0;
    hdr->push = (info->flags & TCP_BUILD_PACKET_FLAG_PUSH) != 0 ? 1 : 0;

    hdr->data_offset = sizeof(struct tcp_header) / 4; //htons(tcp_packet_size);
    hdr->window      = htons(32 * 1024); // TODO temp use window scale option
    hdr->urgent_pointer = 0;

    hdr->flags       = htons(hdr->flags); // flip the flags byte
    hdr->checksum    = 0;  // initialize checksum to 0

    // copy payload over
    fprintf(stderr, "tcp: sending payload length = %d\n", info->payload_length);
    if(info->payload_length > 0) {
        for(u16 i = 0; i < info->payload_length; i += 16) {
            for(u16 j = 0; j < 16 && (i+j) < info->payload_length; j++) {
                fprintf(stderr, "%02X ", info->payload[i+j]);
            }
        }
    }
    memcpy(tcp_packet_start + sizeof(struct tcp_header), info->payload, info->payload_length);

    // compute checksum and convert to network byte order
    // dest_address = US, source_address = THEM, on a net_socket.
    hdr->checksum = _compute_checksum(socket->net_socket.socket_info.dest_address.ipv4, socket->net_socket.socket_info.source_address.ipv4, tcp_packet_start, tcp_packet_size);

    return tcp_packet_size;
}

static s64 _send_segment(struct net_interface* iface, struct tcp_socket* socket, u8* payload, u16 payload_length, u16 flags)
{
    u16 tcp_packet_size = sizeof(struct tcp_header) + payload_length;

    struct tcp_build_packet_info info = { 
        .socket         = socket,
        .payload        = payload, 
        .payload_length = payload_length,
        .flags          = flags
    };

    u16 packet_length;
    u8* packet_start = iface->wrap_packet(iface, &socket->net_socket.socket_info.source_address, NET_PROTOCOL_TCP, tcp_packet_size, &_build_tcp_packet, &info, &packet_length);
    if(packet_start == null) return errno;

    // if we get here, the packet is ready and we need to update our sequence numbers
    u32 seq_inc = payload_length;
    if(flags & TCP_BUILD_PACKET_FLAG_SYNC) seq_inc++;
    //TODO probably FINISH needs seq_inc too

    socket->my_sequence_number += seq_inc;

    return net_send_packet(iface->net_device, packet_start, packet_length); // TODO net_queue_packet so that it can be retransmitted?
}


