echo -en '\xFF\x01' | socat - udp-datagram:255.255.255.255:1500,bind=:6666,broadcast,reuseaddr

