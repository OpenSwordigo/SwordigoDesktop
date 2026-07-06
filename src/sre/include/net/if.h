#ifndef SRE_NET_IF_H
#define SRE_NET_IF_H

#include <sys/socket.h>

#define IFNAMSIZ 16

struct ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        struct sockaddr ifru_addr;
        struct sockaddr ifru_dstaddr;
        struct sockaddr ifru_broadaddr;
        struct sockaddr ifru_netmask;
        struct sockaddr ifru_hwaddr;
        short           ifru_flags;
        int             ifru_ivalue;
        int             ifru_mtu;
        char            ifru_slave[IFNAMSIZ];
        char            ifru_newname[IFNAMSIZ];
        void*           ifru_data;
    } ifr_ifru;
};

#define ifr_flags ifr_ifru.ifru_flags

#endif
