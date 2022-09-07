#ifndef __NET_TCP_H__
#define __NET_TCP_H__

struct ipv4_header;
struct net_interface;
struct net_socket;
struct net_socket_info;

struct net_socket* tcp_create_socket(struct net_socket_info*);

void tcp_receive_packet(struct net_interface* iface, struct ipv4_header* iphdr, u8* packet, u16 packet_length);

#endif
