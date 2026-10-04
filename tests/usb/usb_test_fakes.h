/*
 * 86Box-Next  The emulator around the USB controllers, faked for their tests:
 *             guest memory, the PCI bus, I/O ports, a memory-mapped window,
 *             the frame timers, mutexes -- and a USB device.
 */
#ifndef USB_TEST_FAKES_H
#define USB_TEST_FAKES_H

#include <stdint.h>
#include <stdio.h>
#include <86box/usb_next.h>

extern uint8_t *ram;

/* PCI */
extern uint8_t (*fake_pci_rd)(int, int, int, void *);
extern void (*fake_pci_wr)(int, int, int, uint8_t, void *);
extern void *fake_pci_priv;
extern int   fake_irq[5];                 /* level per pin, INTA = 1 */

/* I/O ports (the UHCI registers) */
uint16_t fake_io_rw(uint16_t port);
void     fake_io_ww(uint16_t port, uint16_t val);
extern uint16_t fake_io_base;

/* the memory-mapped window (the EHCI registers) */
uint32_t fake_mmio_rl(uint32_t off);
void     fake_mmio_wl(uint32_t off, uint32_t val);
extern uint32_t fake_mmio_base;

/* frame timers: every controller's, in the order they were added */
void fake_frames(int n);

/* guest memory helpers */
void     wr32m(uint32_t a, uint32_t v);
uint32_t rd32m(uint32_t a);

/* the fake device */
typedef struct fake_dev_t {
    usbn_device_t dev;
    int           naks;       /* NAK this many IN packets on ep 0 first */
    int           resets, destroyed;
    int           in2_calls;  /* IN packets that reached endpoint 2 */
    int           iso_out_len, iso_out_first;   /* the last isochronous OUT */
    int           ctl_pos;
} fake_dev_t;

extern const uint8_t fake_dev_desc[18];
void fake_dev_init(fake_dev_t *f, const char *name, int speed);

/* checks */
extern int fake_checks, fake_failures;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        fake_checks++;                                      \
        if (!(cond)) {                                      \
            fake_failures++;                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

#endif
