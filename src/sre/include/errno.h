#ifndef SRE_ERRNO_H
#define SRE_ERRNO_H

extern int* __errno(void);
#define errno (*__errno())

#define ENOENT 2
#define EINTR 4
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EEXIST 17
#define EINVAL 22
#define EMFILE 24
#define ERANGE 34
#define EWOULDBLOCK EAGAIN

#define EPIPE 32
#define EADDRINUSE 98
#define ECONNABORTED 103
#define ECONNRESET 104
#define EISCONN 106
#define ECONNREFUSED 111
#define ETIMEDOUT 110
#define EINPROGRESS 115

#endif
