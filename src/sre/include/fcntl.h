#ifndef SRE_FCNTL_H
#define SRE_FCNTL_H

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2

#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 00004000

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
#define F_SETLK 6

struct flock {
    short l_type;
    short l_whence;
    long l_start;
    long l_len;
    int l_pid;
};

int fcntl(int fd, int cmd, ...);

#endif
