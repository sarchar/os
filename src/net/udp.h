#ifndef __NET_UDP_H__
#define __NET_UDP_H__

struct ipv4_header;
struct net_interface;
struct net_receive_packet_info;

s64  udp_send_packet(struct net_interface* iface, struct net_address* dest_address, u16 source_port, u16 dest_port, u8* udp_payload, u16 udp_payload_length);
void udp_receive_packet(struct net_interface* iface, struct ipv4_header* iphdr, struct net_receive_packet_info*);

#endif
