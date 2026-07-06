#ifndef SRE_SYS_TIME_H
#define SRE_SYS_TIME_H

#include <time.h>

#ifndef _STRUCT_TIMEVAL
#define _STRUCT_TIMEVAL
struct timeval {
    long tv_sec;
    long tv_usec;
};
#endif

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

int gettimeofday(struct timeval* tv, struct timezone* tz);

#endif
