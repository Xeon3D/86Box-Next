/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             A high-speed (USB 2.0) device on a full-speed (USB 1.1) port.
 *
 *             Real USB 2.0 devices fall back to full speed on a USB 1.1 port.
 *             A passed-through one cannot: it stays enumerated at high speed
 *             on the host.  So the guest is shown a full-speed device -- its
 *             configuration descriptor rewritten to full-speed limits here --
 *             and usb_host.c regroups the guest's full-speed packets into the
 *             device's high-speed ones and back, keeping every transfer's end
 *             where it was.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdint.h>
#include <86box/usb_next.h>

#define DESC_ENDPOINT 5
#define EP_ISO        1
#define EP_BULK       2
#define EP_INTERRUPT  3

/* The packet size the guest uses on a full-speed port for an endpoint whose
   high-speed packet size is hs_maxp: bulk is at most 64 at full speed,
   interrupt at most 64, isochronous at most 1023. */
int
usbn_fs_maxp(int type, int hs_maxp)
{
    hs_maxp &= 0x7ff;   /* the high-bandwidth multiplier is high speed only */
    switch (type) {
        case EP_BULK:
            return 64;
        case EP_INTERRUPT:
            return (hs_maxp > 64) ? 64 : hs_maxp;
        case EP_ISO:
            return (hs_maxp > 1023) ? 1023 : hs_maxp;
        default:
            return hs_maxp;
    }
}

/* Rewrite a configuration descriptor set (as much of it as the guest asked
   for) from high-speed to full-speed terms: endpoint packet sizes, and
   interrupt intervals from 2^(n-1) microframes to milliseconds. */
void
usbn_config_to_full_speed(uint8_t *d, int len)
{
    for (int i = 0; (i + 2 <= len) && (d[i] >= 2); i += d[i]) {
        if ((d[i + 1] != DESC_ENDPOINT) || (d[i] < 7) || (i + 7 > len))
            continue;

        int type = d[i + 3] & 3;
        int mp   = usbn_fs_maxp(type, d[i + 4] | (d[i + 5] << 8));

        d[i + 4] = mp & 0xff;
        d[i + 5] = mp >> 8;
        if (type == EP_INTERRUPT) {
            int n  = d[i + 6];
            if (n < 1)
                n = 1;
            if (n > 16)
                n = 16;
            int ms = (1 << (n - 1)) / 8;
            d[i + 6] = (ms < 1) ? 1 : ((ms > 255) ? 255 : ms);
        }
    }
}
