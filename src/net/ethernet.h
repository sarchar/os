#ifndef __ETHERNET_H__
#define __ETHERNET_H__

enum ETHERTYPE {
    ETHERTYPE_IPv4 = 0x0800,
    ETHERTYPE_ARP  = 0x0806,
    ETHERTYPE_IPv6 = 0x86DD,
};

#endif
