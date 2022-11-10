// This dhcp module probably won't be in the kernel long term, as it seems best as a userland application
// But for now, it'll be extremely useful to configure the network automatically at boot
#include "kernel/common.h"

#include "arp.h"
#include "dhcp.h"
#include "dns.h"
#include "errno.h"
#include "hpet.h"
#include "ipv4.h"
#include "net.h"
#include "kernel/buffer.h"
#include "kernel/cpu.h"
#include "kernel/kernel.h"
#include "kernel/smp.h"
#include "kernel/task.h"
#include "stdio.h"

#define DHCP_MAGIC_COOKIE 0x63825363

enum DHCP_OPCODE {
    DHCP_OPCODE_BOOTREQUEST = 1,
    DHCP_OPCODE_BOOTREPLY   = 2
};

enum DHCP_HARDWARE_ADDRESS_TYPES {
    DHCP_HARDWARE_ADDRESS_TYPE_ETHERNET = 1,
};

enum DHCP_FLAGS {
    DHCP_FLAG_BROADCAST = 1 << 0,
};

enum BOOTP_OPTION_TYPES {
    BOOTP_OPTION_TYPE_PAD                    = 0,
    BOOTP_OPTION_TYPE_SUBNET_MASK            = 1,
    BOOTP_OPTION_TYPE_TIME_OFFSET            = 2,
    BOOTP_OPTION_TYPE_ROUTER                 = 3,
    BOOTP_OPTION_TYPE_TIME_SERVER            = 4,
    BOOTP_OPTION_TYPE_NAME_SERVER            = 5,
    BOOTP_OPTION_TYPE_DNS_SERVER             = 6,
    BOOTP_OPTION_TYPE_LOG_SERVER             = 7,
    BOOTP_OPTION_TYPE_COOKIE_SERVER          = 8,
    BOOTP_OPTION_TYPE_LPR_SERVER             = 9,
    BOOTP_OPTION_TYPE_IMPRESS_SERVER         = 10,
    BOOTP_OPTION_TYPE_RESLOC_SERVER          = 11, // resource location server
    BOOTP_OPTION_TYPE_HOST_NAME              = 12,
    BOOTP_OPTION_TYPE_BOOT_FILE_SIZE         = 13,
    BOOTP_OPTION_TYPE_MERIT_DUMP_FILE        = 14,
    BOOTP_OPTION_TYPE_DOMAIN_NAME            = 15,
    BOOTP_OPTION_TYPE_SWAP_SERVER            = 16,
    BOOTP_OPTION_TYPE_ROOT_PATH              = 17,
    BOOTP_OPTION_TYPE_EXTENSIONS_PATH        = 18,
    BOOTP_OPTION_TYPE_BROADCAST_ADDRESS      = 28,
    BOOTP_OPTION_TYPE_REQUESTED_IP_ADDRESS   = 50,
    BOOTP_OPTION_TYPE_DHCP_LEASE_TIME        = 51,
    BOOTP_OPTION_TYPE_DHCP_MESSAGE_TYPE      = 53,
    BOOTP_OPTION_TYPE_SERVER_IDENTIFIER      = 54,
    BOOTP_OPTION_TYPE_PARAMETER_REQUEST_LIST = 55,
    BOOTP_OPTION_TYPE_DHCP_RENEWAL_TIME      = 58,
    BOOTP_OPTION_TYPE_DHCP_REBINDING_TIME    = 59,
    // options 64 and over can't be used in the options field of dhcp_build_packet_info
    BOOTP_OPTION_TYPE_END                    = 255
};

enum DHCP_MESSAGE_TYPES {
    DHCP_MESSAGE_TYPE_DISCOVER = 1,
    DHCP_MESSAGE_TYPE_OFFER    = 2,
    DHCP_MESSAGE_TYPE_REQUEST  = 3,
    DHCP_MESSAGE_TYPE_DECLINE  = 4,
    DHCP_MESSAGE_TYPE_ACK      = 5,
    DHCP_MESSAGE_TYPE_NACK     = 6,
    DHCP_MESSAGE_TYPE_RELEASE  = 7,
    DHCP_MESSAGE_TYPE_INFORM   = 8
};

static char const* const dhcp_message_types[] = {
    [DHCP_MESSAGE_TYPE_DISCOVER] = "DHCPDISCOVER",
    [DHCP_MESSAGE_TYPE_OFFER]    = "DHCPOFFER",
    [DHCP_MESSAGE_TYPE_REQUEST]  = "DHCPREQUEST",
    [DHCP_MESSAGE_TYPE_DECLINE]  = "DHCPDECLINE",
    [DHCP_MESSAGE_TYPE_ACK]      = "DHCPACK",
    [DHCP_MESSAGE_TYPE_NACK]     = "DHCPNACK",
    [DHCP_MESSAGE_TYPE_RELEASE]  = "DHCPRELEASE",
    [DHCP_MESSAGE_TYPE_INFORM]   = "DHCPINFORM"
};

enum DHCP_CLIENT_STATE {
    DHCP_CLIENT_STATE_DISCOVERY,
    DHCP_CLIENT_STATE_OFFERS,
    DHCP_CLIENT_STATE_REQUESTED,
};

struct dhcp_header {
    u8  opcode;
    u8  hardware_address_type;
    u8  hardware_address_length;
    u8  hops;
    u32 xid;
    u16 configure_seconds;
    u16 flags;
    u32 client_address;      // ciaddr
    u32 your_address;        // yiaddr
    u32 next_server_address; // siaddr
    u32 relay_agent_address; // giaddr
    u8  client_hardware_address[16];
    u8  server_hostname[64];
    u8  boot_filename[128];
    u8  options[];
};

struct dhcp_options {
    u8   message_type;

    u32 lease_time;
    u32 server_identifier;

    u32 renewal_time;
    u32 rebinding_time;

    u32 subnet_mask;
    u32 broadcast_address;
    
    u32 router;

    u32 dns_server1;
    u32 dns_server2;
    u32 dns_server3;
    u32 dns_server4;

    u32 requested_ip_address;
};

struct dhcp {
    struct net_socket* broadcast_socket;
    struct net_socket* incoming_socket;
    struct net_socket* unicast_socket;
    u32    next_xid;
    u64    configure_start_time;

    u64    request_sent_time;
    u8     client_state;
};

struct dhcp_build_packet_info {
    struct dhcp* dhcp;
    struct dhcp_options* options;
    u64    options_flags;
    u32    xid;
    u32    configure_seconds;
    u8     dhcp_message_type;
    bool   is_renew;
};

static s64  _send_discover(struct dhcp*);
static s64  _send_request(struct dhcp*, struct dhcp_header*, struct dhcp_options*, bool);

static bool _handle_packet(struct dhcp*, struct dhcp_header*, u16);
static void _print_options(struct dhcp_options*);

static s64  dhcp_main(struct task*);

static declare_condition(network_ready);

s64 dhcp_configure_network(struct net_interface* iface, bool wait_for_network)
{
    struct task* dhcp_task = task_create(dhcp_main, (intp)iface, false);
    if(dhcp_task == null) return -ENOMEM;

    task_enqueue(&get_cpu()->current_task, dhcp_task);

    if(wait_for_network) wait_condition(network_ready);

    return 0;
}

static s64 dhcp_main(struct task* task)
{
    struct net_interface* iface = (struct net_interface*)task->userdata;
    s64 res = 0;

    // enable accept_all on the interface
    iface->accept_all = true;

    // setup dhcp
    struct dhcp dhcp;
    zero(&dhcp);
    dhcp.next_xid             = rand();       // random number for XID
    dhcp.configure_start_time = global_ticks; // time that network configuration started
    dhcp.client_state         = DHCP_CLIENT_STATE_DISCOVERY;

    // we need a "listening" socket on UDP port 68
    struct net_socket_info sockinfo;
    zero(&sockinfo);
    sockinfo.protocol                = NET_PROTOCOL_UDP;
    sockinfo.dest_address.protocol   = NET_PROTOCOL_IPv4; // no specified peer address
    sockinfo.dest_address.ipv4       = 0;
    sockinfo.dest_port               = 0;
    sockinfo.source_address.protocol = NET_PROTOCOL_IPv4; // local address is 0.0.0.0:68
    sockinfo.source_address.ipv4     = 0x00000000; 
    sockinfo.source_port             = 68;

    dhcp.incoming_socket = net_socket_create(iface, &sockinfo);
    if(dhcp.incoming_socket == null) return -ENOMEM;

    // create udp socket broadcast to 255.255.255.255:67 and bind iface to it
    // dest is broadcast port 67
    sockinfo.dest_address.protocol   = NET_PROTOCOL_IPv4;
    sockinfo.dest_address.ipv4       = 0xFFFFFFFF; // broadcast 255.255.255.255
    sockinfo.dest_port               = 67;
    sockinfo.source_address.protocol = NET_PROTOCOL_IPv4;
    sockinfo.source_address.ipv4     = 0x00000000; // will be set by the IP layer
    sockinfo.source_port             = (rand() % 50000) + 10000;

    dhcp.broadcast_socket = net_socket_create(iface, &sockinfo);
    if(dhcp.broadcast_socket == null) {
        net_socket_destroy(dhcp.incoming_socket);
        return -ENOMEM;
    }

    // send discover to start
    _send_discover(&dhcp);

    // loop forever processing packets
    while(true) {
        struct dhcp_header* hdr = (struct dhcp_header*)__builtin_alloca(1500);
        struct buffer* recvbuf = buffer_create_with((u8*)hdr, 1500, 0);
        res = net_socket_receive(dhcp.incoming_socket, recvbuf, buffer_remaining_write(recvbuf));
        if(res <= 0) goto done; // error reading from socket, bail

        // got a packet on port 68, try parsing the packet as dhcp
        if(!_handle_packet(&dhcp, hdr, (u16)res)) break;

        buffer_destroy(recvbuf);
    }

done:
    // destroy socket
    net_socket_destroy(dhcp.broadcast_socket);
    net_socket_destroy(dhcp.incoming_socket);

    // disable accept_all on the interface
    iface->accept_all = true;

    return res;
}

static bool _parse_options(struct dhcp_header* hdr, u16 packet_size, struct dhcp_options* options)
{
    zero(options);
    u8* opt = hdr->options;

    // first four bytes must be DHCP_MAGIC_COOKIE
    if(ntohl(*(u32*)opt) != DHCP_MAGIC_COOKIE) return false;
    opt += sizeof(u32);

    // next option must be BOOTP_OPTION_TYPE_DHCP_MESSAGE_TYPE with length 1
    if(*opt != BOOTP_OPTION_TYPE_DHCP_MESSAGE_TYPE || *(opt + 1) != 1) return false;
    options->message_type = *(opt + 2);
    opt += 3;

    // the rest of the options can be any order
    u32 plen = sizeof(hdr) + sizeof(u32) + 3;
    while(plen + 2 < packet_size) { // option type + option length
        u8 option_type = *opt++;
        u8 option_len = *opt++;

        switch(option_type) {
        case BOOTP_OPTION_TYPE_SUBNET_MASK:
            if(option_len != 4) return false;
            options->subnet_mask = ntohl(*(u32*)opt);
            break;

        case BOOTP_OPTION_TYPE_BROADCAST_ADDRESS:
            if(option_len != 4) return false;
            options->broadcast_address = ntohl(*(u32*)opt);
            break;

        case BOOTP_OPTION_TYPE_ROUTER:
            if(option_len != 4) return false;
            options->router = ntohl(*(u32*)opt);
            break;

        case BOOTP_OPTION_TYPE_DNS_SERVER:
            if(option_len != 4) return false;
            {
                u32 dns_server = ntohl(*(u32*)opt);
                if(options->dns_server1 == 0) options->dns_server1 = dns_server;
                else if(options->dns_server2 == 0) options->dns_server2 = dns_server;
                else if(options->dns_server3 == 0) options->dns_server3 = dns_server;
                else if(options->dns_server4 == 0) options->dns_server4 = dns_server;
            }
            break;

        case BOOTP_OPTION_TYPE_DHCP_LEASE_TIME:
            if(option_len != 4) return false;
            options->lease_time = ntohl(*(u32*)opt);
            break;

        case BOOTP_OPTION_TYPE_SERVER_IDENTIFIER:
            if(option_len != 4) return false;
            options->server_identifier = ntohl(*(u32*)opt);
            break;

        case BOOTP_OPTION_TYPE_DHCP_RENEWAL_TIME:
            if(option_len != 4) return false;
            options->renewal_time = ntohl(*(u32*)opt);
            break;

        case BOOTP_OPTION_TYPE_DHCP_REBINDING_TIME:
            if(option_len != 4) return false;
            options->rebinding_time = ntohl(*(u32*)opt);
            break;

        case BOOTP_OPTION_TYPE_END:
            if(option_len != 0) return false;
            // done
            goto done;

        default:
            fprintf(stderr, "dhcp: unhandled option type %d len %d\n", option_type, option_len);
            break;
        }

        opt += option_len;
        plen += option_len + 2;
    }

done:
    return true;
}

static bool _handle_packet(struct dhcp* dhcp, struct dhcp_header* hdr, u16 packet_size)
{
    if(packet_size < sizeof(hdr) + 8) return false; // minumum of at least 8 bytes for options are required

    // fixup some endianness
    hdr->xid                 = ntohl(hdr->xid);
    hdr->client_address      = ntohl(hdr->client_address);
    hdr->your_address        = ntohl(hdr->your_address);
    hdr->next_server_address = ntohl(hdr->next_server_address);
    hdr->relay_agent_address = ntohl(hdr->relay_agent_address);

    // must be a BOOTP reply message
    if(hdr->opcode != DHCP_OPCODE_BOOTREPLY || hdr->hardware_address_type != DHCP_HARDWARE_ADDRESS_TYPE_ETHERNET || hdr->hardware_address_length != 6) return false;

    // if the hardware address isn't for us, it's invalid
    if(memcmp(hdr->client_hardware_address, dhcp->incoming_socket->net_interface->net_device->hardware_address.mac, 6) != 0) {
        fprintf(stderr, "dhcp: invalid hardware address in received packet\n");
        return false;
    }

    // parse the header options
    struct dhcp_options options;
    if(!_parse_options(hdr, packet_size, &options)) return false;

    // bail on invalid message type
    if(options.message_type >= countof(dhcp_message_types) || dhcp_message_types[options.message_type] == null) return false;

    switch(options.message_type) {
    case DHCP_MESSAGE_TYPE_OFFER:
        {
            char buf[16];
            ipv4_format_address(buf, hdr->your_address);
            fprintf(stderr, "dhcp: got DHCPOFFER %s\n", buf);
        }

        // send a REQUEST and wait for ACK
        if(dhcp->request_sent_time == 0) {
            dhcp->request_sent_time = global_ticks;
            options.requested_ip_address = hdr->your_address;
            _send_request(dhcp, hdr, &options, false);
        }
        break;

    case DHCP_MESSAGE_TYPE_ACK:
        {
            char buf[16];
            ipv4_format_address(buf, hdr->your_address);
            fprintf(stderr, "dhcp: got DHCPACK %s\n", buf);
        }

        // configure network and routes
        {
            _print_options(&options);
            if(hdr->your_address == 0 || options.subnet_mask == 0 || options.router == 0 || options.dns_server1 == 0 || options.lease_time == 0) {
                fprintf(stderr, "dhcp: invalid ACK\n");
                return false;
            }

            // configure a default broadcast address
            if(options.broadcast_address == 0) {
                options.broadcast_address = ~options.subnet_mask | (hdr->your_address & options.subnet_mask);
            }

            // don't use dhcp->broadcast_socket->net_interface because that's the UDP layer
            // instead use net_device
            struct net_device* ndev = dhcp->broadcast_socket->net_interface->net_device;
            struct net_interface* iface = net_device_get_interface_by_index(ndev, NET_PROTOCOL_IPv4, 0); // grab the IPv4 interface

            // only if our address changed
            if(iface->address.ipv4 != hdr->your_address) {
                net_device_unregister_interface(iface);
                iface->address.ipv4 = hdr->your_address;
                iface->netmask.ipv4 = options.subnet_mask;
                net_device_register_interface(ndev, iface);

                // set the gateway IP
                struct net_address gateway_address = {
                    .protocol = NET_PROTOCOL_IPv4,
                    .ipv4     = options.router,
                };
                ipv4_set_gateway(iface, &gateway_address);

                // set the DNS server
                struct net_address dns_server = {
                    .protocol = NET_PROTOCOL_IPv4,
                    .ipv4     = options.dns_server1,
                };
                dns_set_server(&dns_server);

                // send an ARP packet to discover where the router is
                arp_send_request(iface, &gateway_address);

                // notify waiting threads that the network is ready
                end_condition(network_ready);
            }

            // free the old unicast socket before recreating it
            if(dhcp->unicast_socket != null) net_socket_destroy(dhcp->unicast_socket); // free any previous socket

            // create a unicast socket at the dhcp server
            struct net_socket_info sockinfo;
            zero(&sockinfo);
            sockinfo.protocol                = NET_PROTOCOL_UDP;
            sockinfo.dest_address.protocol   = NET_PROTOCOL_IPv4; // no specified peer address
            sockinfo.dest_address.ipv4       = options.server_identifier;
            sockinfo.dest_port               = 67;
            sockinfo.source_address.protocol = NET_PROTOCOL_IPv4; // local address is 0.0.0.0:68
            sockinfo.source_address.ipv4     = 0x00000000; 
            sockinfo.source_port             = 0;
            dhcp->unicast_socket = net_socket_create(dhcp->incoming_socket->net_interface, &sockinfo);
            if(dhcp->unicast_socket == null) task_exit(-ENOMEM);

            // wait for the renewal_time to elapse and then request our address again
            u64 start = timer_now();
            while(timer_since(start) < options.renewal_time * 1000000) {
                task_yield(TASK_YIELD_VOLUNTARY);
            }

            // change the transaction id
            hdr->xid = __atomic_xinc(&dhcp->next_xid);

            // request renew
            _send_request(dhcp, hdr, &options, true);
        }
        break;

    case DHCP_MESSAGE_TYPE_NACK:
        // wait 60 seconds and retry discovery
        fprintf(stderr, "dhcp: got DHCPNACK\n");
        sleep(60);
        _send_discover(dhcp);
        break;
    
    default:
        fprintf(stderr, "dhcp: unhandled %s\n", dhcp_message_types[options.message_type]);
        break;
    }

    return true;
}

static void _print_options(struct dhcp_options* options)
{
    char buf[16];

    ipv4_format_address(buf, options->server_identifier);
    fprintf(stderr, "dhcp: server identifier: %s\n", buf);

    ipv4_format_address(buf, options->subnet_mask);
    fprintf(stderr, "dhcp: subnet mask: %s\n", buf);

    ipv4_format_address(buf, options->broadcast_address);
    fprintf(stderr, "dhcp: broadcast address: %s\n", buf);

    ipv4_format_address(buf, options->router);
    fprintf(stderr, "dhcp: default gateway: %s\n", buf);

    ipv4_format_address(buf, options->dns_server1);
    fprintf(stderr, "dhcp: dns server: %s\n", buf);

    if(options->dns_server2 != 0) {
        ipv4_format_address(buf, options->dns_server2);
        fprintf(stderr, "dhcp: dns server: %s\n", buf);
    }

    if(options->dns_server3 != 0) {
        ipv4_format_address(buf, options->dns_server3);
        fprintf(stderr, "dhcp: dns server: %s\n", buf);
    }

    if(options->dns_server4 != 0) {
        ipv4_format_address(buf, options->dns_server4);
        fprintf(stderr, "dhcp: dns server: %s\n", buf);
    }

    fprintf(stderr, "dhcp: lease time: %u\n", options->lease_time);
    fprintf(stderr, "dhcp: IP renewal time: %d\n", options->renewal_time);
    fprintf(stderr, "dhcp: IP rebinding time: %d\n", options->rebinding_time);
}


static __always_inline u64 _get_dhcp_options_size(struct dhcp_build_packet_info* info)
{
    u64 count = sizeof(u32) + 3 + 2; // 3 for message type, 2 for END option

    if((info->options_flags & (1ULL << BOOTP_OPTION_TYPE_SERVER_IDENTIFIER))      != 0) count += 6;
    if((info->options_flags & (1ULL << BOOTP_OPTION_TYPE_REQUESTED_IP_ADDRESS))   != 0) count += 6;
    if((info->options_flags & (1ULL << BOOTP_OPTION_TYPE_PARAMETER_REQUEST_LIST)) != 0) count += 6;

    return count;
}

static s64 _send_packet(struct dhcp_build_packet_info* info)
{
    // renew requests are sent directly to the dhcp server
    struct net_socket* socket = info->is_renew ? info->dhcp->unicast_socket : info->dhcp->broadcast_socket;

    // create a buffer for the payload
    u16 dhcp_packet_size = sizeof(struct dhcp_header) + _get_dhcp_options_size(info);
    struct buffer* packet = buffer_create(dhcp_packet_size);

    // alloocate temp header on the stack
    struct dhcp_header* hdr = (struct dhcp_header*)__builtin_alloca(dhcp_packet_size);

    // start with a zero header
    zero(hdr);

    // setup header
    hdr->opcode                  = DHCP_OPCODE_BOOTREQUEST;
    hdr->hardware_address_type   = DHCP_HARDWARE_ADDRESS_TYPE_ETHERNET;
    hdr->hardware_address_length = 6;
    hdr->hops                    = 0;
    hdr->xid                     = htonl(info->xid);
    hdr->configure_seconds       = htons(info->configure_seconds);
    hdr->flags                   = 0; // we don't need DHCP_FLAG_BROADCAST, the only flag in DHCP

    // our MAC address
    memcpy(hdr->client_hardware_address, socket->net_interface->net_device->hardware_address.mac, 6);

    // For renewing requests, we need to fill in client_address
    if(info->dhcp_message_type == DHCP_MESSAGE_TYPE_REQUEST && info->is_renew) {
        // use the IPv4 interface, not socket->net_interface, which is UDP
        struct net_device* ndev = socket->net_interface->net_device;
        struct net_interface* iface = net_device_get_interface_by_index(ndev, NET_PROTOCOL_IPv4, 0); // grab the IPv4 interface
        if(iface->address.ipv4 != 0) hdr->client_address = htonl(iface->address.ipv4);
    }

    // begin options
    u8* options = &hdr->options[0];

    // first option is always a 32-bit magic cookie
    *(u32*)options = htonl(DHCP_MAGIC_COOKIE);
    options += sizeof(u32);

    // the next is always the DHCP message type
    *options++ = BOOTP_OPTION_TYPE_DHCP_MESSAGE_TYPE;
    *options++ = 1; // length
    *options++ = info->dhcp_message_type;

    // server id
    if((info->options_flags & (1ULL << BOOTP_OPTION_TYPE_SERVER_IDENTIFIER)) != 0) {
        *options++ = BOOTP_OPTION_TYPE_SERVER_IDENTIFIER;
        *options++ = sizeof(u32);
        *(u32*)options = htonl(info->options->server_identifier);
        options += sizeof(u32);
    }

    // requested ip
    if((info->options_flags & (1ULL << BOOTP_OPTION_TYPE_REQUESTED_IP_ADDRESS)) != 0) {
        *options++ = BOOTP_OPTION_TYPE_REQUESTED_IP_ADDRESS;
        *options++ = sizeof(u32);
        *(u32*)options = htonl(info->options->requested_ip_address);
        options += sizeof(u32);
    }

    // parameter list
    if((info->options_flags & (1ULL << BOOTP_OPTION_TYPE_PARAMETER_REQUEST_LIST)) != 0) {
        *options++ = BOOTP_OPTION_TYPE_PARAMETER_REQUEST_LIST;
        *options++ = 4;
        *options++ = BOOTP_OPTION_TYPE_DNS_SERVER;
        *options++ = BOOTP_OPTION_TYPE_ROUTER;
        *options++ = BOOTP_OPTION_TYPE_SUBNET_MASK;
        *options++ = BOOTP_OPTION_TYPE_BROADCAST_ADDRESS;
    }

    // the end flag
    *options++ = BOOTP_OPTION_TYPE_END;
    *options++ = 0;

    // write the header into the buffer and send it
    buffer_write(packet, (u8*)hdr, dhcp_packet_size);
    return net_socket_send(socket, packet);
}

static s64 _send_discover(struct dhcp* dhcp)
{
    u32 xid = __atomic_xinc(&dhcp->next_xid);

    struct dhcp_build_packet_info info = {
        .dhcp              = dhcp,
        .xid               = xid,
        .configure_seconds = (global_ticks - dhcp->configure_start_time + 500) / 1000,
        .dhcp_message_type = DHCP_MESSAGE_TYPE_DISCOVER,
        .options_flags     = (1ULL << BOOTP_OPTION_TYPE_PARAMETER_REQUEST_LIST),
    };

    _send_packet(&info);
    fprintf(stderr, "dhcp: sent DHCPDISCOVER\n");
    return 0;
}

static s64 _send_request(struct dhcp* dhcp, struct dhcp_header* hdr, struct dhcp_options* options, bool is_renew)
{
    struct dhcp_build_packet_info info = {
        .dhcp              = dhcp,
        .xid               = hdr->xid,
        .configure_seconds = (global_ticks - dhcp->configure_start_time + 500) / 1000,
        .dhcp_message_type = DHCP_MESSAGE_TYPE_REQUEST,
        .options           = options,
        .is_renew          = is_renew,
    };

    // for rewewals, don't send server identifier or requested ip options
    if(is_renew) {
        info.options_flags = (1ULL << BOOTP_OPTION_TYPE_PARAMETER_REQUEST_LIST);
    } else{
        info.options_flags =   (1ULL << BOOTP_OPTION_TYPE_SERVER_IDENTIFIER)
                             | (1ULL << BOOTP_OPTION_TYPE_REQUESTED_IP_ADDRESS)
                             | (1ULL << BOOTP_OPTION_TYPE_PARAMETER_REQUEST_LIST);
    }

    _send_packet(&info);
    fprintf(stderr, "dhcp: sent DHCPREQUEST\n");
    return 0;
}

