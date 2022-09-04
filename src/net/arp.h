#ifndef __NET_ARP_H__
#define __NET_ARP_H__

struct net_device;

u8*  arp_create_request(struct net_device* ndev, u8 net_protocol, u8* lookup_address, u64* reslen);
void arp_receive_packet(struct net_device* ndev, u8* packet, u16 packet_length);

#endif
