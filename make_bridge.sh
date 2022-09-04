#!/bin/bash
ip link add br0 type bridge
ip link set dev eth0 master br0

addrs=$(ip addr show dev eth0 | grep -w inet | awk '{print $2}')
for addr in $addr; do
    ip addr del $addr dev eth0
    ip addr add $addr dev br0
done

ip link set dev br0 up
ip route add 172.21.160.0/20 dev br0
ip route add default via 172.21.160.1 dev br0

