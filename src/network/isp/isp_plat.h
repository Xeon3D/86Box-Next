/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The virtual ISP's few needs from the host: a thread, a lock,
 *             a wake-up event and a monotonic clock.  Kept apart from
 *             86Box's own thread layer so that isp-server.exe links the core
 *             without the emulator.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef ISP_PLAT_H
#define ISP_PLAT_H

#include <stdint.h>

typedef struct isp_thread isp_thread_t;
typedef struct isp_mutex  isp_mutex_t;
typedef struct isp_event  isp_event_t;

extern isp_thread_t *isp_thread_start(void (*fn)(void *), void *arg);
extern void          isp_thread_join(isp_thread_t *t);

extern isp_mutex_t *isp_mutex_new(void);
extern void         isp_mutex_free(isp_mutex_t *m);
extern void         isp_mutex_lock(isp_mutex_t *m);
extern void         isp_mutex_unlock(isp_mutex_t *m);

/* The process-wide lock behind the address allocator; needs no setup. */
extern void isp_global_lock(void);
extern void isp_global_unlock(void);

/* Auto-reset: one wait consumes one or more sets. */
extern isp_event_t *isp_event_new(void);
extern void         isp_event_free(isp_event_t *e);
extern void         isp_event_set(isp_event_t *e);
extern int          isp_event_wait(isp_event_t *e, uint32_t ms); /* 1 if set */
#ifdef _WIN32
extern void *isp_event_handle(isp_event_t *e); /* the HANDLE, to wait on with sockets */
#else
extern int  isp_event_fd(isp_event_t *e); /* readable when set */
extern void isp_event_clear(isp_event_t *e);
#endif

/* A wake-up that select()/poll() can wait on next to sockets: a loopback
   UDP socket connected to itself on Windows, a pipe elsewhere.  Readable
   while set. */
typedef struct isp_wake isp_wake_t;

extern isp_wake_t *isp_wake_new(void);
extern void        isp_wake_free(isp_wake_t *w);
extern void        isp_wake_set(isp_wake_t *w);
extern void        isp_wake_clear(isp_wake_t *w);
extern int         isp_wake_wait(isp_wake_t *w, uint32_t ms); /* 1 if set; clears it */
#ifdef _WIN32
extern uintptr_t isp_wake_socket(isp_wake_t *w); /* a SOCKET */
#else
extern int isp_wake_fd(isp_wake_t *w);
#endif

extern uint64_t isp_now_ms(void);
extern uint64_t isp_now_ns(void);
extern uint32_t isp_random(void);

#endif /* ISP_PLAT_H */
