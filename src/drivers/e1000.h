#ifndef __E1000_H__
#define __E1000_H__

struct net_device;

void e1000_load();

s64 e1000_net_transmit_packet(struct net_device* ndev, u8 net_protocol, u8* dest_address, u8 dest_address_length, u8* packet, u16 packet_length);

#endif
