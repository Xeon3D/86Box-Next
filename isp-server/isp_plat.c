/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The virtual ISP's host layer: Win32 or POSIX threads, locks,
 *             events and clock.  See isp_plat.h.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <winsock2.h>
#    include <windows.h>
#    include <process.h>
#else
#    include <errno.h>
#    include <fcntl.h>
#    include <poll.h>
#    include <pthread.h>
#    include <unistd.h>
#endif
#include "isp_plat.h"

struct isp_thread {
#ifdef _WIN32
    HANDLE handle;
#else
    pthread_t handle;
#endif
    void (*fn)(void *);
    void *arg;
};

struct isp_mutex {
#ifdef _WIN32
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t mutex;
#endif
};

struct isp_event {
#ifdef _WIN32
    HANDLE handle;
#else
    int fds[2];
#endif
};

#ifdef _WIN32
static unsigned __stdcall
isp_thread_main(void *arg)
{
    isp_thread_t *t = (isp_thread_t *) arg;

    t->fn(t->arg);
    return 0;
}
#else
static void *
isp_thread_main(void *arg)
{
    isp_thread_t *t = (isp_thread_t *) arg;

    t->fn(t->arg);
    return NULL;
}
#endif

isp_thread_t *
isp_thread_start(void (*fn)(void *), void *arg)
{
    isp_thread_t *t = (isp_thread_t *) calloc(1, sizeof(isp_thread_t));

    if (t == NULL)
        return NULL;
    t->fn  = fn;
    t->arg = arg;
#ifdef _WIN32
    t->handle = (HANDLE) _beginthreadex(NULL, 0, isp_thread_main, t, 0, NULL);
    if (t->handle == NULL) {
#else
    if (pthread_create(&t->handle, NULL, isp_thread_main, t) != 0) {
#endif
        free(t);
        return NULL;
    }
    return t;
}

void
isp_thread_join(isp_thread_t *t)
{
    if (t == NULL)
        return;
#ifdef _WIN32
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
#else
    pthread_join(t->handle, NULL);
#endif
    free(t);
}

isp_mutex_t *
isp_mutex_new(void)
{
    isp_mutex_t *m = (isp_mutex_t *) calloc(1, sizeof(isp_mutex_t));

    if (m == NULL)
        return NULL;
#ifdef _WIN32
    InitializeCriticalSection(&m->cs);
#else
    pthread_mutex_init(&m->mutex, NULL);
#endif
    return m;
}

void
isp_mutex_free(isp_mutex_t *m)
{
    if (m == NULL)
        return;
#ifdef _WIN32
    DeleteCriticalSection(&m->cs);
#else
    pthread_mutex_destroy(&m->mutex);
#endif
    free(m);
}

void
isp_mutex_lock(isp_mutex_t *m)
{
#ifdef _WIN32
    EnterCriticalSection(&m->cs);
#else
    pthread_mutex_lock(&m->mutex);
#endif
}

void
isp_mutex_unlock(isp_mutex_t *m)
{
#ifdef _WIN32
    LeaveCriticalSection(&m->cs);
#else
    pthread_mutex_unlock(&m->mutex);
#endif
}

#ifdef _WIN32
static SRWLOCK isp_global = SRWLOCK_INIT;

void
isp_global_lock(void)
{
    AcquireSRWLockExclusive(&isp_global);
}

void
isp_global_unlock(void)
{
    ReleaseSRWLockExclusive(&isp_global);
}
#else
static pthread_mutex_t isp_global = PTHREAD_MUTEX_INITIALIZER;

void
isp_global_lock(void)
{
    pthread_mutex_lock(&isp_global);
}

void
isp_global_unlock(void)
{
    pthread_mutex_unlock(&isp_global);
}
#endif

isp_event_t *
isp_event_new(void)
{
    isp_event_t *e = (isp_event_t *) calloc(1, sizeof(isp_event_t));

    if (e == NULL)
        return NULL;
#ifdef _WIN32
    e->handle = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (e->handle == NULL) {
#else
    if (pipe(e->fds) != 0) {
#endif
        free(e);
        return NULL;
    }
#ifndef _WIN32
    for (int i = 0; i < 2; i++) {
        fcntl(e->fds[i], F_SETFD, FD_CLOEXEC);
        fcntl(e->fds[i], F_SETFL, O_NONBLOCK);
    }
#endif
    return e;
}

void
isp_event_free(isp_event_t *e)
{
    if (e == NULL)
        return;
#ifdef _WIN32
    CloseHandle(e->handle);
#else
    close(e->fds[0]);
    close(e->fds[1]);
#endif
    free(e);
}

void
isp_event_set(isp_event_t *e)
{
#ifdef _WIN32
    SetEvent(e->handle);
#else
    (void) !write(e->fds[1], "e", 1);
#endif
}

int
isp_event_wait(isp_event_t *e, uint32_t ms)
{
#ifdef _WIN32
    return WaitForSingleObject(e->handle, ms) == WAIT_OBJECT_0;
#else
    struct pollfd pfd = { .fd = e->fds[0], .events = POLLIN };

    if (poll(&pfd, 1, (int) ms) <= 0)
        return 0;
    isp_event_clear(e);
    return 1;
#endif
}

#ifdef _WIN32
void *
isp_event_handle(isp_event_t *e)
{
    return e->handle;
}
#else
int
isp_event_fd(isp_event_t *e)
{
    return e->fds[0];
}

void
isp_event_clear(isp_event_t *e)
{
    char buf[64];

    while (read(e->fds[0], buf, sizeof(buf)) > 0)
        ;
}
#endif

struct isp_wake {
#ifdef _WIN32
    SOCKET s;
#else
    int fds[2];
#endif
};

isp_wake_t *
isp_wake_new(void)
{
    isp_wake_t *w = (isp_wake_t *) calloc(1, sizeof(isp_wake_t));

    if (w == NULL)
        return NULL;
#ifdef _WIN32
    {
        struct sockaddr_in sa;
        int                len = sizeof(sa);
        u_long             yes = 1;
        WSADATA            wsa;

        /* Balanced by nothing: Winsock stays up for the life of the process,
           as it does for the emulator's other users of it. */
        WSAStartup(MAKEWORD(2, 2), &wsa);
        w->s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        memset(&sa, 0, sizeof(sa));
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        /* Bound to loopback and connected to itself: it hears only itself. */
        if ((w->s == INVALID_SOCKET) || (bind(w->s, (struct sockaddr *) &sa, sizeof(sa)) != 0) ||
            (getsockname(w->s, (struct sockaddr *) &sa, &len) != 0) ||
            (connect(w->s, (struct sockaddr *) &sa, sizeof(sa)) != 0)) {
            if (w->s != INVALID_SOCKET)
                closesocket(w->s);
            free(w);
            return NULL;
        }
        ioctlsocket(w->s, FIONBIO, &yes);
    }
#else
    if (pipe(w->fds) != 0) {
        free(w);
        return NULL;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(w->fds[i], F_SETFD, FD_CLOEXEC);
        fcntl(w->fds[i], F_SETFL, O_NONBLOCK);
    }
#endif
    return w;
}

void
isp_wake_free(isp_wake_t *w)
{
    if (w == NULL)
        return;
#ifdef _WIN32
    closesocket(w->s);
#else
    close(w->fds[0]);
    close(w->fds[1]);
#endif
    free(w);
}

void
isp_wake_set(isp_wake_t *w)
{
#ifdef _WIN32
    (void) send(w->s, "w", 1, 0);
#else
    (void) !write(w->fds[1], "w", 1);
#endif
}

void
isp_wake_clear(isp_wake_t *w)
{
    char buf[64];

#ifdef _WIN32
    while (recv(w->s, buf, sizeof(buf), 0) > 0)
        ;
#else
    while (read(w->fds[0], buf, sizeof(buf)) > 0)
        ;
#endif
}

int
isp_wake_wait(isp_wake_t *w, uint32_t ms)
{
    int ret;

#ifdef _WIN32
    fd_set         rd;
    struct timeval tv = { (long) (ms / 1000), (long) ((ms % 1000) * 1000) };

    FD_ZERO(&rd);
    FD_SET(w->s, &rd);
    ret = select(0, &rd, NULL, NULL, &tv) > 0;
#else
    struct pollfd pfd = { .fd = w->fds[0], .events = POLLIN };

    ret = poll(&pfd, 1, (int) ms) > 0;
#endif
    if (ret)
        isp_wake_clear(w);
    return ret;
}

#ifdef _WIN32
uintptr_t
isp_wake_socket(isp_wake_t *w)
{
    return (uintptr_t) w->s;
}
#else
int
isp_wake_fd(isp_wake_t *w)
{
    return w->fds[0];
}
#endif

uint64_t
isp_now_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER        now;

    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    /* Split so that the multiplication cannot overflow. */
    return ((uint64_t) (now.QuadPart / freq.QuadPart) * 1000000000ull) +
           ((uint64_t) (now.QuadPart % freq.QuadPart) * 1000000000ull / (uint64_t) freq.QuadPart);
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t) ts.tv_sec * 1000000000ull) + (uint64_t) ts.tv_nsec;
#endif
}

uint64_t
isp_now_ms(void)
{
    return isp_now_ns() / 1000000ull;
}

/* For LCP magic numbers: unique per link, not secret. */
uint32_t
isp_random(void)
{
    static uint32_t state;
    uint32_t        x;

    isp_global_lock();
    if (state == 0)
        state = (uint32_t) isp_now_ns() ^ ((uint32_t) time(NULL) * 2654435761u) ^ 0x86b0c5a1u;
    x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    isp_global_unlock();
    return x;
}
