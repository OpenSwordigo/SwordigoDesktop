#ifndef SRE_SYS_SELECT_H
#define SRE_SYS_SELECT_H

#include <time.h>
#include <sys/types.h>

#define FD_SETSIZE 1024
typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define FD_ZERO(s) do { \
    unsigned int __i; \
    for (__i = 0; __i < sizeof(*(s)) / sizeof(unsigned long); __i++) \
        (s)->fds_bits[__i] = 0; \
} while(0)

#define FD_SET(d, s) ((s)->fds_bits[(d) / (8 * sizeof(unsigned long))] |= (1UL << ((d) % (8 * sizeof(unsigned long)))))
#define FD_CLR(d, s) ((s)->fds_bits[(d) / (8 * sizeof(unsigned long))] &= ~(1UL << ((d) % (8 * sizeof(unsigned long)))))
#define FD_ISSET(d, s) (((s)->fds_bits[(d) / (8 * sizeof(unsigned long))] & (1UL << ((d) % (8 * sizeof(unsigned long))))) != 0)

#ifndef _STRUCT_TIMEVAL
#define _STRUCT_TIMEVAL
struct timeval {
    long tv_sec;
    long tv_usec;
};
#endif

int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, struct timeval* timeout);

#endif
