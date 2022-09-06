#!/bin/bash
ip link add br0 type bridge
ip link set dev eth0 master br0
ip addr add 192.168.53.1/24 dev br0
ip link set dev br0 up

# enable masquerade
# if one day you want DHCP or BOOTP, there's some info here:
# https://wiki.qemu.org/Documentation/Networking/NAT
iptables -t nat -A POSTROUTING -s 192.168.53.0/24 \! -d 192.168.53.0/24 -j MASQUERADE
