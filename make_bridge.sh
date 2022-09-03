#!/bin/bash
ip link add br0 type bridge
ip link set dev eth0 master br0
ip addr delete 172.21.169.3/20 dev eth0
ip addr add 172.21.169.3/20 dev br0
ip route add default via 172.21.160.1 dev br0
