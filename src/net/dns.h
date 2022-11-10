#ifndef __DNS_H__
#define __DNS_H__

#include "net/net.h"

enum DNS_RECORD_TYPE {
    DNS_RECORD_TYPE_ADDRESS    = 0, // IPv4/6 determined by address.protocol
    DNS_RECORD_TYPE_NAMESERVER = 1,

    DNS_RECORD_TYPE_UNIMPL     = 0xFF // record types not yet implemented
};

enum DNS_RECORD_CLASS {
    DNS_RECORD_CLASS_INTERNET  = 0,
};

struct dns_record {
    union {
        struct net_address address;
        char               ptr[256];
    };

    char name[256];
    u8   type;
    u8   internal_type;
    u8   class;
    u16  ttl;
};

struct dns_result {
    u8     num_records;
    struct dns_record records[];
};

void dns_set_server(struct net_address*);

struct dns_result* dns_lookup(char const*);
void               dns_result_destroy(struct dns_result*);

#endif
