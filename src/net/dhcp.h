#ifndef __DHCP_H__
#define __DHCP_H__

struct net_interface;

// the only function we'll ever need
s64 dhcp_configure_network(struct net_interface*, bool wait_for_network);

#endif
