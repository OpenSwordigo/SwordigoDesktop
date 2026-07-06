#ifndef SRE_UNISTD_H
#define SRE_UNISTD_H

#include <stddef.h>

int close(int fd);
long int lseek(int fd, long int offset, int whence);
int rmdir(const char* path);
int chdir(const char* path);
char* getcwd(char* buf, size_t size);
int symlink(const char* target, const char* linkpath);
int unlink(const char* pathname);
int link(const char* oldpath, const char* newpath);
long int readlink(const char* pathname, char* buf, size_t bufsiz);
int gethostname(char* name, size_t len);
long int read(int fd, void* buf, size_t count);
long int write(int fd, const void* buf, size_t count);

#endif
