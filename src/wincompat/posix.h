/*
 * wincompat/posix.h — MSVC POSIX-compatibility shim for C++ translation units.
 *
 * Provides the small set of POSIX declarations/functions the emulator's host
 * code relies on but that Microsoft's CRT/UCRT omits:
 *   - read/write/lseek/dup/close/access/mkdir on CRT file descriptors
 *   - clock_gettime / gettimeofday / lrand48
 *   - ssize_t / mode_t
 *
 * Include as the FIRST include in a translation unit (it pulls <winsock2.h>
 * in before <windows.h>). No-op on Linux / MinGW.
 */
#ifndef _SWORDIGO_WINCOMPAT_POSIX_H
#define _SWORDIGO_WINCOMPAT_POSIX_H

#if defined(_WIN32) && !defined(__MINGW32__)
#if defined(__cplusplus)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <io.h>        /* _read/_write/_lseek/_dup/_close/_access/_mkdir   */
#include <process.h>
#include <direct.h>
#include <sys/stat.h>
#include <time.h>
#include <stdio.h>      /* SEEK_SET etc.                                    */
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>

typedef SSIZE_T ssize_t;
typedef unsigned int mode_t;

/* Only define a shim when the real header isn't available. */
#ifndef _WINSOCK2API_

struct timeval {
    long tv_sec;
    long tv_usec;
};

#endif /* !_WINSOCK2API_ */

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 0
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 1
#endif

inline int clock_gettime(int, struct timespec *ts) {
    return timespec_get(ts, TIME_UTC) == 0 ? -1 : 0;
}

inline int gettimeofday(struct timeval *tv, void *) {
    FILETIME ft;
    ULARGE_INTEGER us;
    if (!tv) return 0;
    GetSystemTimeAsFileTime(&ft);
    us.LowPart  = ft.dwLowDateTime;
    us.HighPart = ft.dwHighDateTime;
    us.QuadPart /= 10;
    us.QuadPart -= 11644473600000000ULL;
    tv->tv_sec  = (long)(us.QuadPart / 1000000);
    tv->tv_usec = (long)(us.QuadPart % 1000000);
    return 0;
}

inline long lrand48(void) {
    return ((long)rand() << 16) ^ (long)rand();
}

/* File-descriptor aliases. */
inline int read(int fd, void *buf, unsigned int cnt)  { return _read(fd, buf, cnt); }
inline int write(int fd, const void *buf, unsigned int cnt) { return _write(fd, buf, (unsigned int)cnt); }
inline long lseek(int fd, long off, int whence)        { return _lseek(fd, off, whence); }
inline int dup(int fd)                                 { return _dup(fd); }
inline int close(int fd)                               { return _close(fd); }
inline int access(const char *p, int m)                { return _access(p, m); }
inline int mkdir(const char *p, mode_t /*m*/)          { return _mkdir(p); }

/* String case-insensitive compare. */
inline int strcasecmp(const char *a, const char *b)      { return _stricmp(a, b); }
inline int strncasecmp(const char *a, const char *b, size_t n) { return _strnicmp(a, b, (size_t)n); }

#ifndef F_OK
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#endif

inline int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    if (!req) return 0;
    DWORD ms = (DWORD)((req->tv_sec * 1000) + (req->tv_nsec / 1000000));
    Sleep(ms);
    return 0;
}

inline struct tm *localtime_r(const time_t *t, struct tm *out) {
    return localtime_s(out, t) == 0 ? out : nullptr;
}

typedef unsigned long nfds_t;

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

inline ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    ssize_t total = 0;
    for (int k = 0; k < iovcnt; k++) {
        int n = _write(fd, iov[k].iov_base, (unsigned int)iov[k].iov_len);
        if (n < 0) return -1;
        total += n;
    }
    return total;
}

inline ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    ssize_t total = 0;
    for (int k = 0; k < iovcnt; k++) {
        int n = _read(fd, iov[k].iov_base, (unsigned int)iov[k].iov_len);
        if (n < 0) return -1;
        total += n;
    }
    return total;
}

#include <immintrin.h>
#ifndef __builtin_ia32_pause
#define __builtin_ia32_pause() _mm_pause()
#endif

/* --- Networking extras (WSAPoll-equivalent) ---------------------------- */
#ifndef POLLIN
#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020
#endif

typedef uint32_t in_addr_t;

/* poll(): Winsock ships `struct pollfd` + WSAPoll(). */
inline int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    return WSAPoll((WSAPOLLFD *)fds, (ULONG)nfds, timeout);
}

/* ioctl/fcntl: not meaningfully available on Winsock fds; stubs for build. */
inline int ioctl(int fd, unsigned long req, void *argp) {
    (void)fd; (void)req; (void)argp;
    return -1;
}
inline int fcntl(int fd, int cmd, ...) {
    (void)fd; (void)cmd;
    return -1;
}

#endif /* __cplusplus */
#endif /* _WIN32 && !MINGW */
#endif /* include guard */