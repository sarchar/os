#!/bin/bash
ip link add br0 type bridge
ip addr add 192.168.53.1/24 dev br0
ip link set dev br0 up

# enable masquerade
# if one day you want DHCP or BOOTP, there's some info here:
# https://wiki.qemu.org/Documentation/Networking/NAT
echo 1 > /proc/sys/net/ipv4/ip_forward
iptables -t nat -A POSTROUTING -s 192.168.53.0/24 \! -d 192.168.53.0/24 -j MASQUERADE

# start dnsmasq
dnsmasq --strict-order --except-interface=lo --interface=br0 --listen-address=192.168.53.1 --bind-interfaces --dhcp-range=192.168.53.10,192.168.53.254 --conf-file="" --pid-file=/tmp/dnsmasq.pid --dhcp-leasefile=/tmp/leases --dhcp-no-override
