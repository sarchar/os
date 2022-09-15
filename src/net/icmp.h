#ifndef __NET_ICMP_H__
#define __NET_ICMP_H__

#include "net/net.h"

struct ipv4_header;

void icmp_receive_packet(struct net_interface* iface, struct ipv4_header* iphdr, struct net_receive_packet_info*);
s64  icmp_send_echo(struct net_interface* iface, struct net_address* dest_address, u16 sequence_number);

#endif
