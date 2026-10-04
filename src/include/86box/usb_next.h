/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             A working USB stack: a PCI UHCI host controller (USB 1.1) with
 *             two root-hub ports, and devices that plug into them -- for now
 *             host devices passed through with libusb.  Upstream's usb.c is
 *             the southbridges' register stub and is left alone.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef EMU_USB_NEXT_H
#define EMU_USB_NEXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USBN_PORTS      2

#define USB_PID_SETUP   0x2d
#define USB_PID_IN      0x69
#define USB_PID_OUT     0xe1

/* packet() returns the bytes moved (>= 0), or one of these. */
#define USBN_NAK        (-1)  /* not now: the TD stays active and is retried  */
#define USBN_STALL      (-2)
#define USBN_NODEV      (-3)
#define USBN_BABBLE     (-4)
#define USBN_IOERROR    (-5)

#define USBN_SPEED_LOW  0
#define USBN_SPEED_FULL 1
#define USBN_SPEED_HIGH 2

typedef struct usbn_device_t {
    char    name[128];
    int     speed;
    uint8_t addr;           /* set by the guest's SET_ADDRESS */
    void   *priv;

    /* One USB transaction.  Called from the emulation thread. */
    int  (*packet)(struct usbn_device_t *dev, uint8_t pid, uint8_t ep, uint8_t *buf, int len);
    void (*reset)(struct usbn_device_t *dev);    /* port reset */
    void (*destroy)(struct usbn_device_t *dev);  /* unplugged */
} usbn_device_t;

/* ---- the host controller (usb_uhci.c) ---- */
extern int usb_card_type;                   /* (C) USB controller card, 0 = none */

extern void        usb_card_reset(void);
extern const char *usb_card_get_internal_name(int card);
extern int         usb_card_get_from_internal_name(const char *s);
extern const char *usb_card_get_name(int card);
extern int         usb_card_count(void);

/* Plug or unplug a device; safe from the UI thread.  The controller takes
   ownership of dev.  Returns 0 when there is no controller or the port is
   taken. */
extern int  usbn_present(void);
extern int  usbn_attach(int port, usbn_device_t *dev);
extern void usbn_detach(int port);
extern int  usbn_port_busy(int port);
extern const char *usbn_port_name(int port, char *buf, int len);

/* ---- host passthrough (usb_host.c) ---- */
typedef struct usbn_host_info_t {
    uint16_t vid, pid;
    uint8_t  bus, port_path[8], port_depth;
    int      speed;
    char     desc[128];
} usbn_host_info_t;

extern int            usbn_host_available(void);   /* built with libusb */
extern int            usbn_host_list(usbn_host_info_t *out, int max);
extern usbn_device_t *usbn_host_open(uint16_t vid, uint16_t pid, char *err, int errlen);

/* Remembered attachments ("vvvv:pppp" per port), for the config file. */
extern char usbn_port_cfg[USBN_PORTS][16];
extern void usbn_restore_ports(void);

#ifdef __cplusplus
}
#endif

#endif /*EMU_USB_NEXT_H*/
