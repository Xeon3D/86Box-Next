/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The virtual ISP's core, for isp-server.exe (isp_server.c): the
 *             emulator's modems reach it over their TCP line and are not
 *             linked with it.  One session is one call: PPP on a byte
 *             stream, a private /24, and a user-mode NAT (libslirp) out to
 *             the host's Internet; calls on one ISP can also reach each
 *             other (guest LAN).
 *
 *             Each session runs on a thread of its own; these calls only
 *             move bytes through locked queues and never wait on the
 *             network.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef ISP_H
#define ISP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ISP_MAX_SESSIONS 254
#define ISP_MAX_FORWARDS 16

typedef struct isp_session isp_session_t;

typedef struct isp_settings {
    uint32_t base_net;     /* host order; session n gets base_net + n * 256, a /24 (default 10.86.0.0) */
    int      max_sessions; /* 1 to ISP_MAX_SESSIONS                                                   */
    int      require_pap;  /* ask the guest to authenticate (any name and password will do)           */
    uint32_t echo_secs;    /* LCP keepalive interval; 0 sends none                                    */
    int      guest_lan;    /* calls reach each other at their addresses, as on one real ISP          */
    int      throttle;     /* hold each call to its modem's speed rather than the serial port's      */
    uint32_t default_rate; /* the speed, in bit/s, of a call whose frontend does not say (isp-server)  */
} isp_settings_t;

/* Process-wide.  Address, PAP and keepalive apply to calls that open after
   the change; guest LAN and throttle at once. */
extern void isp_get_settings(isp_settings_t *s);
extern void isp_set_settings(const isp_settings_t *s);

/* A host port that reaches the guest: anything connecting to the host's
   `host_port` (on loopback, or on every interface) arrives at the guest's
   `guest_port`.  Bound while the call is on line. */
typedef struct isp_forward {
    int      udp;
    int      all_interfaces;
    uint16_t host_port;
    uint16_t guest_port;
} isp_forward_t;

enum {
    ISP_CALL_WAITING = 0,   /* connected, the guest has not started PPP */
    ISP_CALL_NEGOTIATING,   /* LCP or IPCP under way                    */
    ISP_CALL_AUTHENTICATING,
    ISP_CALL_ONLINE,        /* IPCP open: on the Internet               */
    ISP_CALL_CLOSING,       /* PPP terminating                          */
    ISP_CALL_ENDED          /* the ISP has hung up, or is about to      */
};

enum {
    ISP_FORWARD_PENDING = 0, /* the call is not on line yet  */
    ISP_FORWARD_BOUND,
    ISP_FORWARD_FAILED       /* the host port is taken       */
};

/* One call, as the status pages show it. */
typedef struct isp_call_info {
    int           number;
    char          label[64];  /* what the frontend calls it: "COM2", "127.0.0.1:61958" */
    uint32_t      guest_ip;   /* host order */
    uint32_t      dns_ip;
    int           state;      /* ISP_CALL_* */
    char          user[64];   /* the PAP name, if the guest sent one */
    uint32_t      seconds;    /* since the call came in */
    uint64_t      bytes_from_guest;
    uint64_t      bytes_to_guest;
    uint32_t      bad_frames; /* bad FCS, oversized, runts, aborts */
    uint32_t      dropped;    /* frames for the guest dropped on a full queue */
    uint32_t      rate;       /* bit/s it is held to; 0 when not throttled */
    int           n_forwards;
    isp_forward_t forwards[ISP_MAX_FORWARDS];
    int           forward_state[ISP_MAX_FORWARDS]; /* ISP_FORWARD_* */
} isp_call_info_t;

/* Every call in progress, by number; returns how many were filled in. */
extern int isp_list_calls(isp_call_info_t *out, int max);
/* The ISP disconnects a call: LCP Terminate, then the carrier goes.  0 if
   there is no such call. */
extern int isp_hangup_call(int number);
/* Replaces a call's port forwards, at once if it is on line. */
extern int isp_set_call_forwards(int number, const isp_forward_t *fwd, int n);

extern const char *isp_call_state_name(int state);
/* "tcp:2121:21 udp:*:5000:5000" and back; returns the number parsed. */
extern int  isp_forwards_parse(const char *s, isp_forward_t *fwd, int max);
extern void isp_forwards_format(const isp_forward_t *fwd, int n, char *buf, size_t len);

typedef struct isp_session_callbacks {
    /* From the session's thread: output is waiting, or the call has ended.
       Optional, for a frontend that would rather wait than poll. */
    void (*notify)(void *opaque);
    /* Progress, one line at a time: LCP and IPCP, the guest's address,
       failures.  From the session's thread or the caller's.  Optional. */
    void (*log)(void *opaque, const char *msg);
    void *opaque;
} isp_session_callbacks_t;

/* A call comes in.  PPP waits for the guest to start it, so this can be
   opened while the modem is still dialling.  NULL, with the reason in `err`,
   when every address is taken or the thread will not start. */
extern isp_session_t *isp_session_open(const isp_session_callbacks_t *cb, char *err, size_t err_len);
/* The call is over: PPP, NAT, address and queues go, all of them. */
extern void isp_session_close(isp_session_t *s);

/* Bytes from the guest's modem; returns how many were taken (fewer when the
   queue is full: hold the rest back). */
extern size_t isp_session_write(isp_session_t *s, const uint8_t *buf, size_t len);
/* Bytes for the guest's modem; returns how many. */
extern size_t isp_session_read(isp_session_t *s, uint8_t *buf, size_t len);
/* 1 once PPP has finished and its last byte has been read: hang up. */
extern int isp_session_ended(isp_session_t *s);

extern int      isp_session_number(const isp_session_t *s);
extern uint32_t isp_session_guest_ip(const isp_session_t *s); /* host order */

/* What the status page calls this call. */
extern void isp_session_set_label(isp_session_t *s, const char *label);
/* The speed a throttled call is held to, in bit/s (a byte per ten bits each
   way, and V.90's 33600 at most from the guest); 0 for the settings'
   default_rate. */
extern void isp_session_set_rate(isp_session_t *s, uint32_t bps);
extern void isp_session_set_forwards(isp_session_t *s, const isp_forward_t *fwd, int n);

#ifdef __cplusplus
}
#endif

#endif /* ISP_H */
