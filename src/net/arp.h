#ifndef __NET_ARP_H__
#define __NET_ARP_H__

struct net_device;

u8* arp_create_request(struct net_device* ndev, u8 net_protocol, u8* address, u8 address_length, u64* reslen);

#endif
