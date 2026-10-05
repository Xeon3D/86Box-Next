/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             One PPP link, the ISP's end: LCP (RFC 1661), optional PAP
 *             (RFC 1334), IPCP with DNS (RFC 1332, RFC 1877).  Takes frames
 *             and a millisecond clock, gives frames and IPv4 packets through
 *             callbacks; no sockets, no threads.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef PPP_SESSION_H
#define PPP_SESSION_H

#include <stddef.h>
#include <stdint.h>

#define PPP_PROTO_IP   0x0021
#define PPP_PROTO_IPCP 0x8021
#define PPP_PROTO_LCP  0xc021
#define PPP_PROTO_PAP  0xc023

#define PPP_DEFAULT_MRU 1500
#define PPP_MIN_MRU     128

/* Codes shared by LCP and IPCP. */
enum {
    PPP_CONF_REQ = 1,
    PPP_CONF_ACK,
    PPP_CONF_NAK,
    PPP_CONF_REJ,
    PPP_TERM_REQ,
    PPP_TERM_ACK,
    PPP_CODE_REJ,
    PPP_PROT_REJ, /* LCP only from here */
    PPP_ECHO_REQ,
    PPP_ECHO_REP,
    PPP_DISC_REQ
};

/* RFC 1661 4.2. */
enum {
    FSM_INITIAL = 0,
    FSM_STARTING,
    FSM_CLOSED,
    FSM_STOPPED,
    FSM_CLOSING,
    FSM_STOPPING,
    FSM_REQSENT,
    FSM_ACKRCVD,
    FSM_ACKSENT,
    FSM_OPENED
};

enum {
    PPP_PHASE_DEAD = 0,
    PPP_PHASE_ESTABLISH,
    PPP_PHASE_AUTHENTICATE,
    PPP_PHASE_NETWORK,
    PPP_PHASE_TERMINATE
};

typedef struct ppp ppp_t;
typedef struct ppp_fsm ppp_fsm_t;

typedef struct ppp_fsm_ops {
    void (*reset)(ppp_t *ppp);                                    /* our request, from scratch */
    size_t (*build_req)(ppp_t *ppp, uint8_t *out);                /* our Configure-Request's options */
    void (*got_nak)(ppp_t *ppp, const uint8_t *opt, size_t len);
    void (*got_rej)(ppp_t *ppp, const uint8_t *opt, size_t len);
    /* The peer's request: fills `reply` with the options of the answer and
       returns its code (CONF_ACK, CONF_NAK or CONF_REJ).  With `reject_naks`
       set, too many naks have gone unheeded and what would be naked is
       rejected instead (RFC 1661 Max-Failure). */
    int (*got_req)(ppp_t *ppp, const uint8_t *opt, size_t len, uint8_t *reply, size_t *reply_len,
                   int reject_naks);
    void (*up)(ppp_t *ppp);
    void (*down)(ppp_t *ppp);
    void (*finished)(ppp_t *ppp);
    /* Codes beyond Code-Reject; returns 0 for one it does not know. */
    int (*extcode)(ppp_t *ppp, int code, int id, const uint8_t *data, size_t len);
} ppp_fsm_ops_t;

struct ppp_fsm {
    const char          *name;
    uint16_t             protocol;
    const ppp_fsm_ops_t *ops;
    int                  state;
    int                  passive;     /* wait for the peer's request before sending ours */
    uint8_t              id;          /* the next identifier we use */
    uint8_t              req_id;      /* the identifier of our outstanding request */
    uint8_t              req[256];    /* ...and its options, which an Ack must repeat */
    size_t               req_len;
    int                  restart;     /* the restart counter */
    int                  timer_on;
    uint32_t             timer_at;
    int                  naks;        /* naks sent in a row, until they turn into rejects */
    int                  term_sent;   /* a Terminate-Request of ours is outstanding */
};

typedef struct ppp_config {
    uint32_t local_ip; /* our end of the link, host order */
    uint32_t peer_ip;  /* the address the guest is given  */
    uint32_t dns[2];   /* what it is told to resolve with */
    int      require_pap;
    uint32_t echo_interval_ms; /* 0: never ask the peer for an echo */
    int      echo_fails;       /* unanswered echoes that end the link */
    uint32_t magic_seed;       /* our LCP magic numbers start from here */
} ppp_config_t;

typedef struct ppp_callbacks {
    /* One frame out.  `accm` is the ACCM to encode it with: the default for
       LCP, the negotiated one for everything else. */
    void (*send)(void *opaque, uint16_t protocol, const uint8_t *info, size_t len, uint32_t accm);
    void (*ip_up)(void *opaque);
    void (*ip_down)(void *opaque);
    void (*ip_input)(void *opaque, const uint8_t *packet, size_t len);
    /* LCP is down for good: the far end should hang up. */
    void (*finished)(void *opaque, const char *why);
    void (*log)(void *opaque, const char *msg);
} ppp_callbacks_t;

struct ppp {
    ppp_config_t           cfg;
    const ppp_callbacks_t *cb;
    void                  *opaque;
    uint32_t               now;
    int                    phase;
    int                    was_open;  /* LCP has been opened once */
    int                    finish_reported;

    ppp_fsm_t lcp;
    ppp_fsm_t ipcp;

    /* LCP, what we ask for. */
    int      want_magic;
    uint32_t magic;
    int      want_accm;
    uint32_t want_accm_val;
    int      want_pap;
    /* LCP, what the peer asked for (in effect once opened). */
    uint32_t peer_mru;
    uint32_t peer_accm;
    uint32_t peer_magic;
    uint32_t tx_accm; /* applied to every non-LCP frame we send */
    uint32_t rx_accm; /* control characters the peer's line may add */

    /* PAP. */
    int      authenticated;
    uint32_t auth_deadline;

    /* IPCP, what we ask for. */
    int      want_addr;
    uint32_t local_ip;

    /* Echo keepalive. */
    uint32_t echo_at;
    int      echo_pending;

    /* Counters for the log. */
    uint32_t ip_in;
    uint32_t ip_out;
    uint32_t ip_dropped;
    uint32_t prot_rejects;

    char peer_user[64]; /* the name the guest gave PAP, for the status pages */

    char why[96]; /* why the link went down, for finished() */
};

extern void ppp_init(ppp_t *ppp, const ppp_config_t *cfg, const ppp_callbacks_t *cb, void *opaque, uint32_t now);
/* The line is up: LCP waits, passively, for the peer's first request. */
extern void ppp_lower_up(ppp_t *ppp, uint32_t now);
/* One received frame, FCS removed. */
extern void ppp_input(ppp_t *ppp, const uint8_t *frame, size_t len, uint32_t now);
/* An IPv4 packet for the peer; dropped unless IPCP is open. */
extern void ppp_send_ip(ppp_t *ppp, const uint8_t *packet, size_t len);
extern void ppp_tick(ppp_t *ppp, uint32_t now);
/* Milliseconds until ppp_tick() has work, or UINT32_MAX. */
extern uint32_t ppp_next_timeout(const ppp_t *ppp, uint32_t now);
/* End the link politely (LCP Terminate-Request). */
extern void ppp_close(ppp_t *ppp, const char *why, uint32_t now);
extern int  ppp_ip_open(const ppp_t *ppp);

#endif /* PPP_SESSION_H */
