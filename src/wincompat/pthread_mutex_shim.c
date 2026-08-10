/*
 * Minimal pthread_mutex implementation for MSVC builds.
 *
 * Lua (llimits.h / lstate.c) and the bundled thread-pool code declare
 * `extern int pthread_mutex_*(void*)` and hand in aligned raw storage
 * buffers.  Real pthreads are not available on Windows (and there is no
 * -pthread on the MSVC toolchain), so we model each mutex as a Win32
 * CRITICAL_SECTION kept in a small static registry keyed by storage address.
 *
 * Present in both executables via swcore.  Only compiled when _WIN32 and
 * only when the platform lacks real pthreads.
 */

#if defined(_WIN32) && !defined(SWORDIGO_PTHREAD_SHIM_DISABLED)

#include <windows.h>
#include <stdint.h>

#define SWIC_PMUTEX_MAX 256

typedef struct {
    volatile LONG    used;
    void*            key;
    CRITICAL_SECTION cs;
} swic_pmutex_slot;

static swic_pmutex_slot g_swic_pmutex[SWIC_PMUTEX_MAX];
static SRWLOCK         g_swic_reg_lock = SRWLOCK_INIT;

static CRITICAL_SECTION* swic_pmutex_find(void* mutex)
{
    for (int i = 0; i < SWIC_PMUTEX_MAX; ++i) {
        if (g_swic_pmutex[i].used && g_swic_pmutex[i].key == mutex)
            return &g_swic_pmutex[i].cs;
    }
    return NULL;
}

static CRITICAL_SECTION* swic_pmutex_acquire(void* mutex)
{
    CRITICAL_SECTION* cs;
    AcquireSRWLockExclusive(&g_swic_reg_lock);
    cs = swic_pmutex_find(mutex);
    if (cs == NULL) {
        for (int i = 0; i < SWIC_PMUTEX_MAX; ++i) {
            if (!g_swic_pmutex[i].used) {
                InitializeCriticalSection(&g_swic_pmutex[i].cs);
                g_swic_pmutex[i].key = mutex;
                MemoryBarrier();
                g_swic_pmutex[i].used = 1;
                cs = &g_swic_pmutex[i].cs;
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_swic_reg_lock);
    return cs;
}

/* ---------------------------------------------------------------- public - */

int pthread_mutexattr_init(void* attr)
{
    (void)attr;
    return 0;
}

int pthread_mutexattr_settype(void* attr, int type)
{
    (void)attr;
    (void)type;
    return 0;
}

int pthread_mutexattr_destroy(void* attr)
{
    (void)attr;
    return 0;
}

int pthread_mutex_init(void* mutex, const void* attr)
{
    (void)attr;
    return swic_pmutex_acquire(mutex) != NULL ? 0 : -1;
}

int pthread_mutex_lock(void* mutex)
{
    CRITICAL_SECTION* cs = swic_pmutex_acquire(mutex);
    if (cs == NULL) return -1;
    EnterCriticalSection(cs);
    return 0;
}

int pthread_mutex_unlock(void* mutex)
{
    CRITICAL_SECTION* cs = swic_pmutex_find(mutex);
    if (cs == NULL) return -1;
    LeaveCriticalSection(cs);
    return 0;
}

int pthread_mutex_destroy(void* mutex)
{
    CRITICAL_SECTION* cs = NULL;
    AcquireSRWLockExclusive(&g_swic_reg_lock);
    for (int i = 0; i < SWIC_PMUTEX_MAX; ++i) {
        if (g_swic_pmutex[i].used && g_swic_pmutex[i].key == mutex) {
            cs = &g_swic_pmutex[i].cs;
            g_swic_pmutex[i].used = 0;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_swic_reg_lock);
    if (cs == NULL) return -1;
    DeleteCriticalSection(cs);
    return 0;
}

#endif /* _WIN32 */