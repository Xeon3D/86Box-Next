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
    int     fs_view;        /* a high-speed device shown at full speed (on a USB 1.1 port) */
    void   *priv;

    /* One USB transaction.  Called from the emulation thread. */
    int  (*packet)(struct usbn_device_t *dev, uint8_t pid, uint8_t ep, uint8_t *buf, int len);
    /* One isochronous transaction (optional): once per frame, never NAKed
       or retried.  IN returns the bytes delivered (0 when there are none),
       OUT the bytes taken. */
    int  (*iso)(struct usbn_device_t *dev, uint8_t pid, uint8_t ep, uint8_t *buf, int len);
    void (*reset)(struct usbn_device_t *dev);    /* port reset */
    void (*frame)(struct usbn_device_t *dev);    /* once per 1 ms frame (optional) */
    void (*destroy)(struct usbn_device_t *dev);  /* unplugged */
} usbn_device_t;

/* ---- the ports (usb_bus.c) ---- */
/* The controller the ports are wired to.  connect() / disconnect() run on the
   emulation thread; the bus owns the device and destroys it after
   disconnect(). */
typedef struct usbn_root_ops_t {
    void (*connect)(void *priv, int port, usbn_device_t *dev);
    void (*disconnect)(void *priv, int port);
} usbn_root_ops_t;

extern void           usbn_set_root(const usbn_root_ops_t *ops, void *priv, int high_speed);
extern void           usbn_apply(void);             /* once per frame, from the root */
extern int            usbn_bus_high_speed(void);    /* the root takes USB 2.0 devices */
extern usbn_device_t *usbn_port_device(int port);

/* ---- the host controller cards (usb_uhci.c, usb_ehci.c) ---- */
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

/* A UHCI controller as the companion of an EHCI one: it is function 0 of the
   EHCI card, raises INTA on the card's slot, and is told which of the shared
   ports it currently owns. */
typedef struct uhci_t uhci_t;

/* Who a controller says it is on the PCI bus. */
typedef struct usbn_pci_id_t {
    uint16_t vendor, device;
    uint8_t  revision;
    uint16_t sub_vendor, sub_device;
} usbn_pci_id_t;

extern uhci_t *uhci_companion_create(uint8_t *card_slot, const usbn_pci_id_t *id);
extern void    uhci_companion_close(uhci_t *dev);
extern void    uhci_companion_reset(uhci_t *dev);
extern void    uhci_route_port(uhci_t *dev, int port, usbn_device_t *d);   /* NULL: not ours now */
extern uint8_t uhci_pci_read(int func, int addr, int len, void *priv);
extern void    uhci_pci_write(int func, int addr, int len, uint8_t val, void *priv);

/* ---- the USB activity trace (usb_bus.c) ----
   Off unless switched on (the USB menu, or BOX86NEXT_USB_TRACE=1): port events,
   control requests and transfers, each with real time and emulated frame,
   written to usb_trace.txt beside the machine's 86box.cfg. */
extern int      usbn_trace_on;
extern uint32_t usbn_frame_count;
extern void     usbn_trace_enable(int on);
extern void     usbn_trace(const char *fmt, ...);
extern uint32_t usbn_ms(void);     /* real time, milliseconds */

/* ---- a high-speed device at full speed (usb_speed.c) ---- */
extern int  usbn_fs_maxp(int type, int hs_maxp);
extern void usbn_config_to_full_speed(uint8_t *desc, int len);

/* ---- host passthrough (usb_host.c) ---- */
typedef struct usbn_host_info_t {
    uint16_t vid, pid;
    uint8_t  bus, port_path[8], port_depth;
    int      speed;
    char     desc[128];
} usbn_host_info_t;

extern int            usbn_host_available(void);   /* built with libusb */
extern int            usbn_host_uses_usbdk(void);  /* Windows: capture any device through UsbDk */
extern int            usbn_host_list(usbn_host_info_t *out, int max);
extern usbn_device_t *usbn_host_open(uint16_t vid, uint16_t pid, char *err, int errlen);

/* Remembered attachments ("vvvv:pppp" per port), for the config file. */
extern char usbn_port_cfg[USBN_PORTS][16];
extern void usbn_restore_ports(void);

#ifdef __cplusplus
}
#endif

#endif /*EMU_USB_NEXT_H*/
