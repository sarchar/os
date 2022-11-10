#include "kernel/common.h"

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

enum DNS_TYPES {
    DNS_TYPE_A     = 1,
    DNS_TYPE_NS    = 2,
    DNS_TYPE_MD    = 3,
    DNS_TYPE_MF    = 4,
    DNS_TYPE_CNAME = 5,
    DNS_TYPE_SOA   = 6,
    DNS_TYPE_PTR   = 12,
    DNS_TYPE_HINFO = 13,
    DNS_TYPE_MX    = 15,
    DNS_TYPE_TXT   = 16,

    DNS_TYPE_AAAA  = 28,

    // Following are only valid for QTYPE fields
    DNS_TYPE_ANY   = 255
};

enum DNS_CLASSES {
    DNS_CLASS_IN = 1,  // Internet

    // Only valid in QCLASS fields
    DNS_CLASS_ANY = 255
};

struct dns_header {
    u16 id;
    union {
        u16 flags;
        struct {
            u16 query_response      : 1;
            u16 opcode              : 4;
            u16 authoritative       : 1;
            u16 truncated           : 1;
            u16 recursion_desired   : 1;
            u16 recursion_available : 1;
            u16 reserved0           : 3;
            u16 response_code       : 4;
        };
    };

    u16 question_count;
    u16 answer_count;
    u16 nameserver_count;
    u16 additional_count;
} __packed;

static struct net_address global_dns_server;

static s64 _query_address(struct net_socket*, char const*, u16*);
static struct dns_result* _handle_response(struct dns_header*, u16);

// return the number of bytes needed for a NAME field in DNS record from a given string hostname
static __always_inline u64 _name_length(char const* hostname)
{
    // separating the hostname by '.' and replacing the period with a length, just gives us the length of the string
    // plus the length for the first label, and a terminating 0
    return strlen(hostname) + 2;
}

static u8* _convert_to_name(u8* dest, char const* hostname)
{
    while(*hostname != '\0') {
        u8 label_length = 0;

        // determine a label and length
        u8* p = dest + 1;
        while(*hostname != '.' && *hostname != '\0') {
            *p++ = *hostname++;
            label_length++;
        }

        // skip the period
        if(*hostname == '.') hostname++;

        // first byte is the length
        *dest = label_length;

        // next label
        dest = p;

        // if a domain with two periods was given, we'll have a segment of length 0, so terminate
        if(label_length == 0) break;
    }

    // terminate the list with a length 0 entry
    *dest++ = 0;

    return dest;
}

static u8* _read_dns_name(char* out, u16 max_size, u8* src, u8* end, u8* dns_packet_start, u16 dns_packet_length)
{
    bool first = true;

    while(src < end && max_size > 1) { // require at least one extra byte in the output available for the null terminator
        u8 len = *src++;

        // break on 0
        if(len == 0) break;

        // handle "compression" but don't allow recursion
        if((len & 0xC0) == 0xC0 && dns_packet_start != null) {
            // 14 bit offset, but input data was in network order, so fix that up
            u16 offs = ntohs(*(u16*)(src - 1)) & 0x3FFF;
            src++;

            // finish using the new location
            if(offs < dns_packet_length) _read_dns_name(out, max_size, dns_packet_start + offs, end, null, 0);

            // but return the end of this "string", which was only 2 bytes
            return src;
        }

        // top 2 bits should be 0 anyway, and we will just ignore them
        len &= 0x3F;

        // don't overflow the output buffer
        len = min(len, max_size - 1);
        max_size -= len;

        // place a period between each label
        if(!first && max_size > 1) {
            *out++ = '.';
            max_size--;
        }

        first = false;

        // copy over len bytes to the output
        while(len-- && src < end) *out++ = *src++;
    }

    // null terminate the output
    *out = 0;

    // return the end of the string
    return src;
}

void dns_set_server(struct net_address* dns_server)
{
    memcpy(&global_dns_server, dns_server, sizeof(struct net_address));
}

struct dns_result* dns_lookup(char const* hostname)
{
    struct dns_result* dns_result = null;
    struct net_socket* dns_socket = null;

    struct net_device* ndev = net_device_by_index(0); // grab the first network adapter
    if(ndev == null) {
        fprintf(stderr, "dns: no network device available\n");
        goto done;
    }

    struct net_interface* iface = net_device_get_interface_by_index(ndev, NET_PROTOCOL_IPv4, 0); // grab the first IPv4 interface
    if(iface == null || iface->address.ipv4 == 0) {
        fprintf(stderr, "dns: no network interface available\n");
        goto done;
    }

    // create a UDP socket on port 53
    struct net_socket_info sockinfo;
    zero(&sockinfo);
    sockinfo.protocol       = NET_PROTOCOL_UDP;
    sockinfo.dest_address   = global_dns_server;
    sockinfo.dest_port      = 53;
    sockinfo.source_address = iface->address;
    sockinfo.source_port    = 10000 + (rand() % 50000);

    dns_socket = net_socket_create(iface, &sockinfo);
    if(dns_socket == null) goto done;

    // fire off the query
    s64 res;
    u16 id;
    if((res = _query_address(dns_socket, hostname, &id)) < 0) goto done;

    // wait for a response
    struct buffer* recvbuf = buffer_create(1500); // 1500 is the max DNS response packet length
    res = net_socket_receive(dns_socket, recvbuf, buffer_remaining_write(recvbuf));
    if(res >= (s64)sizeof(struct dns_header)) { // got a response
        // fix up the endianness of the header
        struct dns_header* hdr = (struct dns_header*)recvbuf->buf;
        hdr->id                = ntohs(hdr->id);
        hdr->flags             = ntohs(hdr->flags);
        hdr->question_count    = ntohs(hdr->question_count);
        hdr->answer_count      = ntohs(hdr->answer_count);
        hdr->nameserver_count  = ntohs(hdr->nameserver_count);
        hdr->additional_count  = ntohs(hdr->additional_count);

        // build the records array if this was in response to our message
        if(hdr->id == id) dns_result = _handle_response(hdr, (u16)res);
    }

done:
    if(dns_socket != null) net_socket_destroy(dns_socket);
    return dns_result;
}

void dns_result_destroy(struct dns_result* dns_result)
{
    if(dns_result != null) free(dns_result);
}

// Send a DNS packet with 1 question for the address(es) of a given hostname
static s64 _query_address(struct net_socket* dns_socket, char const* hostname, u16* id)
{
    u16 question_length    = _name_length(hostname) + 2 * sizeof(u16);
    u16 packet_length      = sizeof(struct dns_header) + question_length;
    struct dns_header* hdr = (struct dns_header*)__builtin_alloca(packet_length);
    
    *id = (u16)(rand() % 0x10000);
    
    hdr->id                = htons(*id);

    hdr->flags             = 0; // default all flags to zero
    hdr->recursion_desired = 1;
    hdr->flags             = htons(hdr->flags); // fix up flags field

    hdr->question_count    = htons(1);
    hdr->answer_count      = htons(0);
    hdr->nameserver_count  = htons(0);
    hdr->additional_count  = htons(0);

    // fill in the question
    u8* questions = (u8*)hdr + sizeof(struct dns_header);

    questions = _convert_to_name(questions, hostname);
    *(u16*)questions = htons(DNS_TYPE_ANY);
    questions += sizeof(u16);
    *(u16*)questions = htons(DNS_CLASS_IN);
    questions += sizeof(u16);

    struct buffer* sendbuf = buffer_create(packet_length);
    buffer_write(sendbuf, (u8*)hdr, packet_length);
    return net_socket_send(dns_socket, sendbuf);
}

static struct dns_result* _handle_response(struct dns_header* hdr, u16 packet_length)
{
    char* tmpbuf = (char*)__builtin_alloca(packet_length); // temp storage for names

    //fprintf(stderr, "dns: got response id  = 0x%04X\n", hdr->id);
    //fprintf(stderr, "dns: question_count   = %d\n"    , hdr->question_count);
    //fprintf(stderr, "dns: answer_count     = %d\n"    , hdr->answer_count);
    //fprintf(stderr, "dns: nameserver_count = %d\n"    , hdr->nameserver_count);
    //fprintf(stderr, "dns: additional_count = %d\n"    , hdr->additional_count);

    u8* ptr = (u8*)hdr + sizeof(struct dns_header);
    u8* end = (u8*)hdr + packet_length;

    u32 dns_result_size = sizeof(struct dns_result) + sizeof(struct dns_record) * hdr->answer_count;
    struct dns_result* dns_result = (struct dns_result*)malloc(dns_result_size);
    dns_result->num_records = hdr->answer_count;

    // should only be 1 question in the response
    if(hdr->question_count != 1 || (ptr >= end)) goto error;

    // move the data pointer past the question field
    ptr = _read_dns_name(tmpbuf, packet_length, ptr, end, (u8*)hdr, packet_length);
    if(ptr >= end) goto error;

    // print the question
    //{
    //    u16 type  = ntohs(*(u16*)(ptr + 0));
    //    u16 class = ntohs(*(u16*)(ptr + 2));
    //    fprintf(stderr, "dns: query question: [%s] type:%d class:%d\n", tmpbuf, type, class);
    //}
    ptr += 4;

    // print the answers
    for(u16 i = 0; (ptr < end) && i < hdr->answer_count; i++) {
        struct dns_record* dns_record = &dns_result->records[i];

        ptr = _read_dns_name(dns_record->name, countof(dns_record->name), ptr, end, (u8*)hdr, packet_length);

        // need at least 10 bytes now
        if(ptr + 10 > end) goto error;

        // fixup endianness for the record fields
        dns_record->internal_type = ntohs(*(u16*)(ptr + 0));
        u16 class       = ntohs(*(u16*)(ptr + 2));
        dns_record->ttl = ntohl(*(u32*)(ptr + 4));
        u16 data_length = ntohs(*(u16*)(ptr + 8));

        ptr += 10;

        // only accept Internet class DNS
        if(class != DNS_CLASS_IN) goto error;
        dns_record->class = DNS_RECORD_CLASS_INTERNET;

        // make sure there's enough room for the payload
        if(ptr + data_length > end) goto error;

        // process data
        //char data[256];
        switch(dns_record->internal_type) {
        case DNS_TYPE_A:
            if(data_length != 4) goto error;
            dns_record->type             = DNS_RECORD_TYPE_ADDRESS;
            dns_record->address.protocol = NET_PROTOCOL_IPv4;
            dns_record->address.ipv4     = ntohl(*(u32*)ptr);
            //ipv4_format_address(data, dns_record->address.ipv4);
            break;

        case DNS_TYPE_AAAA:
            if(data_length != 16) goto error;
            dns_record->type             = DNS_RECORD_TYPE_ADDRESS;
            dns_record->address.protocol = NET_PROTOCOL_IPv6;
            for(u32 j = 0; j < 8; j++) {
                dns_record->address.ipv6[j]  = ntohs(*(u16*)(ptr + j * 2));
            }
            //snprintf(data, countof(data), "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
            //        dns_record->address.ipv6[0], dns_record->address.ipv6[1], 
            //        dns_record->address.ipv6[2], dns_record->address.ipv6[3], 
            //        dns_record->address.ipv6[4], dns_record->address.ipv6[5], 
            //        dns_record->address.ipv6[6], dns_record->address.ipv6[7]);
            break;

        case DNS_TYPE_NS:
            dns_record->type = DNS_RECORD_TYPE_NAMESERVER;
            u8* resptr = _read_dns_name(dns_record->ptr, countof(dns_record->ptr), ptr, end, (u8*)hdr, packet_length);
            if(resptr != (ptr + data_length)) goto error; // the remaining data should be consumed by the name
            //strcpy(data, dns_record->ptr);
            break;

        case DNS_TYPE_HINFO:
            dns_record->type = DNS_RECORD_TYPE_UNIMPL;
            //u8 cpulen = *(u8*)ptr;
            //strcpy(data, "CPU: ");
            //strncat(&data[5], (char*)(ptr + 1), cpulen);
            //strcat(&data[5+cpulen], " OS: ");
            //u8 oslen = *(u8*)(ptr + 1 + cpulen);
            //strncat(&data[10+cpulen], (char*)(ptr + 1 + cpulen + 1), oslen);
            break;

        default:
            dns_record->type = DNS_RECORD_TYPE_UNIMPL;
            //strcpy(data, "<unknown type>");
            break;
        }

        ptr += data_length;

        //fprintf(stderr, "dns: answer %d: [%s] type:%d class:%d ttl:%u data:[%s]\n", i, dns_record->name, dns_record->internal_type, class, dns_record->ttl, data);
    }

    goto done;

error:
    if(dns_result != null) free(dns_result);
    dns_result = null;

done:
    return dns_result;
}
