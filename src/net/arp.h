#ifndef __NET_ARP_H__
#define __NET_ARP_H__

struct net_address;
struct net_interface;
struct net_receive_packet_info;

s64  arp_send_request(struct net_interface* ndev, struct net_address* lookup_address);
void arp_handle_device_packet(struct net_receive_packet_info*);

// lookup protocol_address and return hardware_address, return 0 on success
s64 arp_lookup(struct net_address* protocol_address, struct net_address* hardware_address);

#endif
