#ifndef __NET_ICMP_H__
#define __NET_ICMP_H__

#include "net/net.h"

struct ipv4_header;

void icmp_receive_packet(struct net_interface* ndev, struct ipv4_header* iphdr, u8* packet, u16 packet_length);

#endif
