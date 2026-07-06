#ifndef SRE_SYS_SOCKET_H
#define SRE_SYS_SOCKET_H

#include <sys/types.h>
#include <sys/select.h>

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char __ss_padding[128 - sizeof(sa_family_t)];
};

#define AF_UNSPEC 0
#define AF_INET 2
#define AF_INET6 10

#define PF_UNSPEC AF_UNSPEC
#define PF_INET AF_INET
#define PF_INET6 AF_INET6

#define SOCK_STREAM 1
#define SOCK_DGRAM 2

#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_ERROR 4
#define SO_BROADCAST 6
#define SO_KEEPALIVE 9
#define SO_DONTROUTE 5
#define SO_LINGER 13

struct linger {
    int l_onoff;
    int l_linger;
};

int socket(int domain, int type, int protocol);
int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
ssize_t send(int sockfd, const void* buf, size_t len, int flags);
ssize_t recv(int sockfd, void* buf, size_t len, int flags);
ssize_t sendto(int sockfd, const void* buf, size_t len, int flags, const struct sockaddr* dest_addr, socklen_t addrlen);
ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags, struct sockaddr* src_addr, socklen_t* addrlen);
int getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen);
int setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen);
int getsockname(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
int getpeername(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
int shutdown(int sockfd, int how);

#endif
