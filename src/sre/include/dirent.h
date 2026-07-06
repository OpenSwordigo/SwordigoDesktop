#ifndef SRE_DIRENT_H
#define SRE_DIRENT_H

#include <sys/types.h>

typedef struct DIR DIR;

struct dirent {
    ino_t          d_ino;
    off_t          d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[256];
};

DIR* opendir(const char* name);
struct dirent* readdir(DIR* dirp);
int closedir(DIR* dirp);

#endif
