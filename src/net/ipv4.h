#ifndef __NET_IPv4_H__
#define __NET_IPv4_H__

struct net_device;

enum IPv4_PROTOCOL {
    IPv4_PROTOCOL_ICMP = 1,
    IPv4_PROTOCOL_TCP  = 6,
    IPv4_PROTOCOL_UDP  = 17,
};

void ipv4_receive_packet(struct net_device* ndev, u8* packet, u16 packet_length);

#endif
