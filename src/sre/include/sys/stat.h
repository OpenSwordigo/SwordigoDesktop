#ifndef SRE_SYS_STAT_H
#define SRE_SYS_STAT_H

#include <sys/types.h>

struct stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned int st_uid;
    unsigned int st_gid;
    unsigned long st_rdev;
    long st_size;
    long st_blksize;
    long st_blocks;
    long st_atime;
    long st_atime_nsec;
    long st_mtime;
    long st_mtime_nsec;
    long st_ctime;
    long st_ctime_nsec;
};

#define st_atime_sec st_atime
#define st_mtime_sec st_mtime
#define st_ctime_sec st_ctime

int stat(const char* path, struct stat* buf);
int lstat(const char* path, struct stat* buf);
int mkdir(const char* path, mode_t mode);

#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001

#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFLNK  0120000
#define S_IFSOCK 0140000
#define S_IFIFO  0010000
#define S_IFCHR  0020000
#define S_IFBLK  0060000

#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)

#endif
