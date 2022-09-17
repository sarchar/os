#ifndef __NET_TCP_H__
#define __NET_TCP_H__

struct ipv4_header;
struct net_interface;
struct net_receive_packet_info;
struct net_socket;
struct net_socket_info;

struct net_socket* tcp_socket_create(struct net_socket_info*);
void               tcp_destroy_socket(struct net_socket*);

void tcp_receive_packet(struct net_interface* iface, struct ipv4_header* iphdr, struct net_receive_packet_info*);

void __tcp_set_socket_iface(struct net_socket*, struct net_interface*); // TODO get rid of this

#endif
