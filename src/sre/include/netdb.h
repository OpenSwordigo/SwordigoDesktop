#ifndef SRE_NETDB_H
#define SRE_NETDB_H

#include <sys/socket.h>

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    sa_family_t ai_addrlen; // sa_family_t is size of socklen_t (4 bytes)
    struct sockaddr* ai_addr;
    char* ai_canonname;
    struct addrinfo* ai_next;
};

struct hostent {
    char* h_name;
    char** h_aliases;
    int h_addrtype;
    int h_length;
    char** h_addr_list;
};
#define h_addr h_addr_list[0]

#define NI_MAXHOST 1025
#define NI_MAXSERV 32
#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2

#define AI_PASSIVE 1
#define AI_NUMERICHOST 4
#define AI_NUMERICSERV 1024

#define HOST_NOT_FOUND 1
#define TRY_AGAIN 2
#define NO_RECOVERY 3
#define NO_DATA 4

#define EAI_AGAIN 2
#define EAI_BADFLAGS 3
#define EAI_FAIL 4
#define EAI_FAMILY 5
#define EAI_MEMORY 6
#define EAI_NONAME 8
#define EAI_OVERFLOW 9
#define EAI_SERVICE 10
#define EAI_SOCKTYPE 11
#define EAI_SYSTEM 12

extern int h_errno;

int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res);
void freeaddrinfo(struct addrinfo* res);
const char* gai_strerror(int errcode);
int getnameinfo(const struct sockaddr* sa, socklen_t salen, char* host, socklen_t hostlen, char* serv, socklen_t servlen, int flags);
struct hostent* gethostbyaddr(const void* addr, socklen_t len, int type);
struct hostent* gethostbyname(const char* name);
const char* hstrerror(int err);

#endif
