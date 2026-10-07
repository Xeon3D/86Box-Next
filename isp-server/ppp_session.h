/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             One PPP link, the ISP's end: LCP (RFC 1661); authentication
 *             with PAP (RFC 1334), CHAP with MD5 (RFC 1994) or SHA-1/2/3,
 *             MS-CHAP (RFC 2433) and MS-CHAP-2 (RFC 2759); IPCP with DNS
 *             and WINS (RFC 1332, RFC 1877); CCP (RFC 1962) with MPPE
 *             encryption and the compressions of ppp_comp.c; and Multilink
 *             (RFC 1990), a link either carrying its bundle's network layer
 *             or handing everything to the link that does.  Takes frames
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
#include "ppp_auth.h"
#include "ppp_comp.h"
#include "ppp_mp.h"

#define PPP_PROTO_IP   0x0021
#define PPP_PROTO_IPCP 0x8021
#define PPP_PROTO_LCP  0xc021
#define PPP_PROTO_PAP  0xc023
#define PPP_PROTO_CHAP 0xc223

#define PPP_DEFAULT_MRU 1500
#define PPP_MIN_MRU     128

/* Codes shared by LCP, IPCP and CCP. */
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
#define CCP_RESET_REQ 14
#define CCP_RESET_ACK 15

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

/* Who gets in. */
enum {
    PPP_AUTH_NONE = 0, /* nobody is asked                                         */
    PPP_AUTH_ANY,      /* asked, and any name and password will do (no MS-CHAP-2) */
    PPP_AUTH_ACCOUNTS  /* only the accounts' names and passwords                  */
};

enum {
    PPP_MPPE_OFF = 0,
    PPP_MPPE_ALLOWED, /* when the guest asks, after MS-CHAP                     */
    PPP_MPPE_REQUIRED /* or no IP: MS-CHAP and MPPE, or the link is ended       */
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
    int                  bundle;      /* goes over the bundle (IPCP, CCP), not the link  */
    uint8_t              id;          /* the next identifier we use */
    uint8_t              req_id;      /* the identifier of our outstanding request */
    uint8_t              req[256];    /* ...and its options, which an Ack must repeat */
    size_t               req_len;
    int                  restart;     /* the restart counter */
    int                  timer_on;
    uint32_t             timer_at;
    int                  naks;        /* naks sent in a row, until they turn into rejects */
    int                  resend;      /* got_req changed what we ask for: a new request */
};

typedef struct ppp_config {
    uint32_t local_ip; /* our end of the link, host order */
    uint32_t peer_ip;  /* the address the guest is given  */
    uint32_t dns[2];   /* what it is told to resolve with */
    uint32_t wins[2];  /* its WINS servers; 0: none (the option is rejected) */
    int      auth;        /* PPP_AUTH_* */
    uint32_t auth_protos; /* 1 << PPP_AP_*: the methods it may use */
    int      mppe;        /* PPP_MPPE_* */
    uint32_t mppe_bits;   /* MPPX_L, MPPX_M, MPPX_S allowed; MPPX_H: stateless allowed */
    uint32_t compression; /* 1 << PPP_COMP_* */
    int      multilink;   /* accept MRRU: links of one guest make a bundle */
    uint32_t echo_interval_ms; /* 0: never ask the peer for an echo */
    int      echo_fails;       /* unanswered echoes that end the link */
    uint32_t magic_seed;       /* our LCP magic numbers start from here */
} ppp_config_t;

typedef struct ppp_callbacks {
    /* One frame out on this link.  `accm` is the ACCM to encode it with: the
       default for LCP, the negotiated one for everything else. */
    void (*send)(void *opaque, uint16_t protocol, const uint8_t *info, size_t len, uint32_t accm);
    void (*ip_up)(void *opaque);
    void (*ip_down)(void *opaque);
    void (*ip_input)(void *opaque, const uint8_t *packet, size_t len);
    /* LCP is down for good: the far end should hang up. */
    void (*finished)(void *opaque, const char *why);
    void (*log)(void *opaque, const char *msg);
    /* An account's password: 1 and copied if `user` has one.  Optional. */
    int (*get_secret)(void *opaque, const char *user, char *secret, size_t len);
    /* A Multilink link is open and its guest known: 0 to bring the network
       layer up here, 1 if it joined another link's bundle (from then on its
       bundle's packets go to to_bundle()).  Optional. */
    int (*link_ready)(void *opaque);
    /* A member link's packet for its bundle's owner (ppp_bundle_input()). */
    void (*to_bundle)(void *opaque, uint16_t protocol, const uint8_t *data, size_t len);
    /* The owner's MP fragment for a member link: link is its slot, 1 up. */
    void (*link_send)(void *opaque, int link, uint16_t protocol, const uint8_t *info, size_t len);
} ppp_callbacks_t;

struct ppp {
    ppp_config_t           cfg;
    const ppp_callbacks_t *cb;
    void                  *opaque;
    uint32_t               now;
    int                    phase;
    int                    was_open;  /* LCP has been opened once */
    int                    finish_reported;
    char                   close_why[96]; /* a close asked for from inside a handler */

    ppp_fsm_t lcp;
    ppp_fsm_t ipcp;
    ppp_fsm_t ccp;

    /* LCP, what we ask for. */
    int      want_magic;
    uint32_t magic;
    int      want_accm;
    uint32_t want_accm_val;
    int      want_ap;       /* PPP_AP_* we ask for, -1: none */
    uint32_t tried_aps;     /* the ones the guest has turned down */
    int      auth_refused;  /* it will not authenticate at all */
    int      want_mrru;     /* Multilink: asked for once the guest offers it */
    /* LCP, what the peer asked for (in effect once opened). */
    uint32_t peer_mru;
    uint32_t peer_accm;
    uint32_t peer_magic;
    uint32_t peer_mrru;     /* 0: no Multilink */
    int      peer_ssn;      /* it wants 12-bit MP sequence numbers */
    uint8_t  peer_ed[21];   /* Endpoint Discriminator: class, address */
    int      peer_ed_len;
    uint32_t tx_accm; /* applied to every non-LCP frame we send */
    uint32_t rx_accm; /* control characters the peer's line may add */
    /* (staged by lcp_got_req until the request is acknowledged) */
    uint32_t st_mrru;
    int      st_ssn;
    uint8_t  st_ed[21];
    int      st_ed_len;

    /* Authentication. */
    int      auth_ap;       /* the method in use, -1 if none */
    int      authenticated;
    uint32_t auth_deadline;
    int      auth_tries;
    uint8_t  chap_id;
    uint8_t  chap_challenge[16];
    size_t   chap_clen;
    int      chap_sends;
    uint32_t chap_at;
    int      chap_timer;
    uint8_t  chap_last_resp_id;
    char     chap_success[96]; /* sent again if the guest asks again */
    /* MPPE's start keys, from MS-CHAP. */
    int      key_kind;      /* 0: none, 1: MS-CHAP, 2: MS-CHAP-2 */
    uint8_t  key_send[16];
    uint8_t  key_recv[16];
    uint8_t  key_lm[16];    /* MS-CHAP's 40- and 56-bit key */

    /* IPCP, what we ask for. */
    int      want_addr;
    uint32_t local_ip;

    /* CCP. */
    int               ccp_offer;    /* which of our offers is out: 0 MPPE/MPPC, 1 Deflate, 2 BSD, 3 Predictor-1, 4 none */
    uint32_t          want_mppx;
    int               want_deflate;
    int               want_bsd;
    ppp_comp_params_t tx_params;    /* what we compress with (the peer asked for it) */
    int               tx_set;
    ppp_comp_params_t st_tx;        /* staged by ccp_got_req */
    int               st_tx_set;
    ppp_comp_t       *comp_tx;
    ppp_comp_t       *comp_rx;
    uint8_t           reset_id;
    uint32_t          reset_at;
    int               reset_sent;

    /* Multilink. */
    int      mp;           /* negotiated on this link */
    int      member;       /* this link belongs to another link's bundle */
    uint32_t links;        /* the owner: the bundle's link slots, a bit each (bit 0: this one) */
    mp_rx_t  mprx;
    mp_tx_t  mptx;
    int      mp_ready;

    /* Echo keepalive. */
    uint32_t echo_at;
    int      echo_pending;

    /* Counters for the log. */
    uint32_t ip_in;
    uint32_t ip_out;
    uint32_t ip_dropped;
    uint32_t prot_rejects;

    char peer_user[64]; /* the name the guest authenticated with */

    char why[96]; /* why the link went down, for finished() */
};

extern void ppp_init(ppp_t *ppp, const ppp_config_t *cfg, const ppp_callbacks_t *cb, void *opaque, uint32_t now);
/* Frees what CCP and Multilink hold. */
extern void ppp_free(ppp_t *ppp);
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

/* The bundle's owner: a member link's packet (its slot, 1 up), the links
   now in the bundle (a bit per slot, bit 0 the owner), and one gone. */
extern void ppp_bundle_input(ppp_t *ppp, int link, uint16_t protocol, const uint8_t *data, size_t len, uint32_t now);
extern void ppp_bundle_links(ppp_t *ppp, uint32_t links);

/* For the status page. */
extern const char *ppp_auth_name(const ppp_t *ppp);              /* "MS-CHAP-2", "" if none */
extern void        ppp_ccp_describe(const ppp_t *ppp, char *buf, size_t len); /* "" if none */
extern int         ppp_encrypted(const ppp_t *ppp);

#endif /* PPP_SESSION_H */
