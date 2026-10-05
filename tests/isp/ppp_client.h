/*
 * 86Box-Next: a scripted PPP client -- the guest's side of a dial-up link,
 * as much of it as the virtual ISP's tests need.  It negotiates the way
 * Windows Dial-Up Networking does (asking for compression, callback, VJ and
 * WINS first, which the ISP must reject), then sends and receives IPv4.
 * Bytes go through callbacks, so the same client talks to an ISP session
 * directly, to a modem's UART, or to isp-server over TCP.
 */
#ifndef PPP_CLIENT_H
#define PPP_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include "ppp_framing.h"

#define PPPC_MAX_IP 32

typedef struct ppp_client {
    /* The line. */
    size_t (*write)(void *opaque, const uint8_t *buf, size_t len);
    size_t (*read)(void *opaque, uint8_t *buf, size_t len);
    void (*idle)(void *opaque); /* between polls: sleep, or pump a modem */
    void *opaque;
    int   verbose;

    /* Authentication, if the ISP asks. */
    const char *user;
    const char *password;

    ppp_rx_t rx;
    uint8_t  id;
    uint32_t magic;

    /* LCP. */
    int      lcp_opened;
    int      lcp_ack_sent;
    int      lcp_ack_rcvd;
    int      lcp_round;      /* 0: everything Windows asks for; 1: what was not rejected */
    uint8_t  lcp_rejected[16]; /* option types the ISP rejected */
    int      lcp_rejected_n;
    uint8_t  isp_lcp_req[256]; /* the ISP's last Configure-Request, options only */
    size_t   isp_lcp_req_len;
    int      isp_wants_pap;
    int      pap_acked;
    int      terminated;     /* the ISP sent Terminate-Ack or -Request */

    /* IPCP. */
    int      ipcp_opened;
    int      ipcp_ack_sent;
    int      ipcp_ack_rcvd;
    int      ipcp_round;
    uint8_t  ipcp_rejected[16];
    int      ipcp_rejected_n;
    uint32_t my_ip;   /* host order */
    uint32_t dns;
    uint32_t dns2;
    uint32_t isp_ip;  /* the ISP's end, from its Configure-Request */

    uint32_t last_req_ms;
    uint16_t ip_id;

    /* IPv4 packets received, oldest first. */
    uint8_t  ip[PPPC_MAX_IP][1600];
    size_t   ip_len[PPPC_MAX_IP];
    int      ip_n;

    uint32_t prot_rejects; /* LCP Protocol-Rejects received */
    uint32_t echo_replies;
} ppp_client_t;

extern void ppp_client_init(ppp_client_t *c);
/* Negotiate until IPCP is open in both directions.  1 on success. */
extern int ppp_client_connect(ppp_client_t *c, uint32_t timeout_ms);
/* Read and handle whatever has arrived. */
extern void ppp_client_poll(ppp_client_t *c);
/* Send a frame of any protocol (for Protocol-Reject and echo tests). */
extern void ppp_client_send(ppp_client_t *c, uint16_t proto, const uint8_t *info, size_t len);
extern void ppp_client_send_ip(ppp_client_t *c, const uint8_t *pkt, size_t len);
/* LCP Terminate-Request, then wait for the ack.  1 if it came. */
extern int ppp_client_terminate(ppp_client_t *c, uint32_t timeout_ms);

/* UDP, IPv4 only, no checksum (which IPv4 allows). */
extern void ppp_client_send_udp(ppp_client_t *c, uint32_t dst, uint16_t sport, uint16_t dport,
                                const uint8_t *data, size_t len);
/* Waits for a UDP datagram to `sport`; returns its payload length or -1. */
extern int ppp_client_recv_udp(ppp_client_t *c, uint16_t sport, uint8_t *data, size_t size,
                               uint32_t *src, uint16_t *src_port, uint32_t timeout_ms);
/* A DNS A query through the ISP's DNS; returns the first address or 0. */
extern uint32_t ppp_client_resolve(ppp_client_t *c, const char *name, uint32_t timeout_ms);
/* A minimal TCP client: connect, send `req`, read until FIN or `size`.
   No retransmission: for a link that does not lose packets.  Returns the
   number of bytes received or -1. */
extern int ppp_client_tcp_fetch(ppp_client_t *c, uint32_t dst, uint16_t dport, const char *req,
                                uint8_t *resp, size_t size, uint32_t timeout_ms);

extern uint32_t ppp_client_ms(void);
extern const char *ppp_client_ip_str(uint32_t ip); /* static buffer */

#endif
