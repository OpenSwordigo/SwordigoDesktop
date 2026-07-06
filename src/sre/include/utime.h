#ifndef SRE_UTIME_H
#define SRE_UTIME_H

struct utimbuf {
    long actime;
    long modtime;
};

int utime(const char* filename, const struct utimbuf* times);

#endif
