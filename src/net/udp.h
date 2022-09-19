#ifndef __NET_UDP_H__
#define __NET_UDP_H__

struct ipv4_header;
struct net_interface;
struct net_receive_packet_info;

struct net_socket* udp_socket_create(struct net_socket_info*);

void udp_receive_packet(struct net_interface* iface, struct ipv4_header* iphdr, struct net_receive_packet_info*);

#endif
