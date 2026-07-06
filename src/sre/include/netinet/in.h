#ifndef SRE_NETINET_IN_H
#define SRE_NETINET_IN_H

#include <stdint.h>
#include <sys/socket.h>

typedef uint32_t in_addr_t;
struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    sa_family_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};

#define INADDR_ANY ((in_addr_t)0x00000000)

struct in6_addr {
    unsigned char s6_addr[16];
};

struct sockaddr_in6 {
    sa_family_t     sin6_family;
    uint16_t        sin6_port;
    uint32_t        sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;
};

#define IN6ADDR_ANY_INIT { { 0 } }

#define IPPROTO_IP 0
#define IPPROTO_TCP 6
#define IPPROTO_IPV6 41

#define IPV6_UNICAST_HOPS 16
#define IPV6_MULTICAST_HOPS 18
#define IPV6_MULTICAST_LOOP 19
#define IPV6_V6ONLY 26
#define IPV6_ADD_MEMBERSHIP 20
#define IPV6_DROP_MEMBERSHIP 21

#define IP_MULTICAST_IF 32
#define IP_MULTICAST_TTL 33
#define IP_MULTICAST_LOOP 34
#define IP_ADD_MEMBERSHIP 35
#define IP_DROP_MEMBERSHIP 36

struct ip_mreq {
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
};

struct ipv6_mreq {
    struct in6_addr ipv6mr_multiaddr;
    unsigned int     ipv6mr_interface;
};

uint16_t htons(uint16_t hostshort);
uint32_t htonl(uint32_t hostlong);
uint16_t ntohs(uint16_t netshort);
uint32_t ntohl(uint32_t netlong);

#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46

#endif
