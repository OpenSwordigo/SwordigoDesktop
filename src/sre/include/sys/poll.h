#ifndef SRE_SYS_POLL_H
#define SRE_SYS_POLL_H

#define POLLIN 0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008

struct pollfd {
    int fd;
    short events;
    short revents;
};

typedef unsigned int nfds_t;

int poll(struct pollfd* fds, nfds_t nfds, int timeout);

#endif
