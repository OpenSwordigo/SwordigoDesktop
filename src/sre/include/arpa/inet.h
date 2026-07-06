#ifndef SRE_ARPA_INET_H
#define SRE_ARPA_INET_H

#include <netinet/in.h>

in_addr_t inet_addr(const char* cp);
char* inet_ntoa(struct in_addr in);
int inet_aton(const char* cp, struct in_addr* inp);
int inet_pton(int af, const char* src, void* dst);

#endif
