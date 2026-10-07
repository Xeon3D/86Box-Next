/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The virtual ISP's way out: one libslirp instance per call, fed
 *             the IPv4 packets PPP carries.  PPP has no MAC addresses and
 *             libslirp speaks Ethernet, so this wraps each packet in a frame
 *             from a made-up guest MAC, answers libslirp's ARP for the guest
 *             itself, and unwraps what comes back.  Every call into an
 *             instance, its timers included, is made by the one thread that
 *             owns it.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef ISP_NAT_SLIRP_H
#define ISP_NAT_SLIRP_H

#include <stddef.h>
#include <stdint.h>
#include "isp.h"
#include "isp_plat.h"

typedef struct isp_nat isp_nat_t;

typedef struct isp_nat_config {
    uint32_t net;      /* the /24, host order, e.g. 10.86.1.0 */
    uint32_t gateway;  /* .2: libslirp's own address, the host's loopback seen from the guest */
    uint32_t dns;      /* .3: libslirp's DNS relay */
    uint32_t guest;    /* .15: the guest's */
    uint32_t mtu;      /* the largest packet the guest takes (its MRU) */
    void (*deliver)(void *opaque, const uint8_t *packet, size_t len);
    void *opaque;
} isp_nat_config_t;

/* Returns NULL, with the reason in `err`, if libslirp will not start. */
extern isp_nat_t *isp_nat_new(const isp_nat_config_t *cfg, char *err, size_t err_len);
extern void       isp_nat_free(isp_nat_t *nat);
extern void       isp_nat_input(isp_nat_t *nat, const uint8_t *packet, size_t len);

/* A host port to the guest (libslirp's hostfwd): 0 if bound, -1 if the host
   port cannot be had. */
extern int  isp_nat_add_forward(isp_nat_t *nat, const isp_forward_t *f);
extern void isp_nat_remove_forward(isp_nat_t *nat, const isp_forward_t *f);

/* One pass of the owner's loop: wait for libslirp's sockets or for `wake`,
   at most `timeout_ms` and no later than libslirp's next deadline, then let
   libslirp handle what is ready and run its due timers.  A set `wake` is
   cleared. */
extern void isp_nat_run(isp_nat_t *nat, isp_wake_t *wake, uint32_t timeout_ms);

#endif /* ISP_NAT_SLIRP_H */
