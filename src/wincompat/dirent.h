/*
 * wincompat/dirent.h — minimal POSIX <dirent.h> for MSVC.
 *
 * Only the subset used by the Windows host code (src/android/asset_manager*.c)
 * is provided: opendir/readdir/closedir reading a directory of file names.
 *
 * Include path is added on Windows only; on Linux the real <dirent.h> is used.
 */
#ifndef _SWORDIGO_WINCOMPAT_DIRENT_H
#define _SWORDIGO_WINCOMPAT_DIRENT_H

#ifdef _WIN32
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

struct dirent {
    char d_name[NAME_MAX + 1];
};

typedef struct _dirdesc {
    intptr_t hfind;        /* _findfirst64/_findnext64 handle, -1 when done */
    struct __finddata64_t data;
    struct dirent de;      /* entry handed back by readdir() */
} DIR;

static DIR *opendir(const char *path)
{
    DIR *d;
    const char *p;
    size_t len = 0;
    char *match;

    if (!path) return NULL;
    p = path;
    while (*p) { len++; p++; }
    while (len > 0 && (p[-1] == '/' || p[-1] == '\\')) { p--; len--; }

    match = (char *)malloc(len + 4);
    if (!match) return NULL;
    memcpy(match, path, len);
    match[len]   = '\\';
    match[len+1] = '*';
    match[len+2] = '\0';

    d = (DIR *)calloc(1, sizeof(DIR));
    if (!d) { free(match); return NULL; }

    d->hfind = _findfirst64(match, &d->data);
    if (d->hfind == (intptr_t)-1) {
        free(match);
        free(d);
        return NULL;
    }
    free(match);
    return d;
}

static struct dirent *readdir(DIR *d)
{
    size_t k;
    const char *name;
    if (!d || d->hfind == (intptr_t)-1) return NULL;

    name = d->data.name;
    if (name) {
        for (k = 0; k < NAME_MAX && name[k]; k++) d->de.d_name[k] = name[k];
        d->de.d_name[k] = '\0';
    } else {
        d->de.d_name[0] = '\0';
    }

    if (_findnext64(d->hfind, &d->data) != 0) {
        _findclose(d->hfind);
        d->hfind = (intptr_t)-1;
    }
    return &d->de;
}

static int closedir(DIR *d)
{
    if (!d) return -1;
    if (d->hfind != (intptr_t)-1) _findclose(d->hfind);
    free(d);
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif /* _SWORDIGO_WINCOMPAT_DIRENT_H */