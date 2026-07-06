#ifndef SRE_TIME_H
#define SRE_TIME_H

#include <stddef.h>

typedef long int time_t;
typedef long int clock_t;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

time_t time(time_t* timer);
clock_t clock(void);
double difftime(time_t time1, time_t time0);
time_t mktime(struct tm* timeptr);
char* asctime(const struct tm* timeptr);
char* ctime(const time_t* timer);
struct tm* gmtime(const time_t* timer);
struct tm* localtime(const time_t* timer);
size_t strftime(char* ptr, size_t maxsize, const char* format, const struct tm* timeptr);

struct timespec {
    long tv_sec;
    long tv_nsec;
};

int nanosleep(const struct timespec* req, struct timespec* rem);

#define CLOCKS_PER_SEC 1000000L

#endif
