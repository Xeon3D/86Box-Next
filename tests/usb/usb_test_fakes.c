/*
 * 86Box-Next  Fakes for the USB controller tests.  See usb_test_fakes.h.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/dma.h>
#include <86box/pci.h>
#include <86box/timer.h>
#include <86box/thread.h>
#include <86box/usb_next.h>
#include "usb_test_fakes.h"

int fake_checks, fake_failures;

/* ---- guest memory ---- */
static uint8_t guest[1 << 20];
uint8_t       *ram = guest;

void dma_bm_read(uint32_t a, uint8_t *d, uint32_t n, int ts) { (void) ts; memcpy(d, guest + a, n); }
void dma_bm_write(uint32_t a, const uint8_t *d, uint32_t n, int ts) { (void) ts; memcpy(guest + a, d, n); }
void     wr32m(uint32_t a, uint32_t v) { memcpy(guest + a, &v, 4); }
uint32_t rd32m(uint32_t a) { uint32_t v; memcpy(&v, guest + a, 4); return v; }

/* ---- PCI ---- */
uint8_t (*fake_pci_rd)(int, int, int, void *);
void (*fake_pci_wr)(int, int, int, uint8_t, void *);
void *fake_pci_priv;
int   fake_irq[5];

void
pci_add_card(uint8_t t, uint8_t (*r)(int, int, int, void *), void (*w)(int, int, int, uint8_t, void *), void *p, uint8_t *slot)
{
    (void) t;
    fake_pci_rd   = r;
    fake_pci_wr   = w;
    fake_pci_priv = p;
    *slot         = 5;
}

void
pci_irq(uint8_t slot, uint8_t pin, int level, int set, uint8_t *st)
{
    (void) slot;
    (void) level;
    if (pin < 5)
        fake_irq[pin] = set;
    *st = set;
}

/* ---- I/O ---- */
static uint16_t (*io_rw)(uint16_t, void *);
static void (*io_ww)(uint16_t, uint16_t, void *);
static void *io_priv;
uint16_t     fake_io_base;

void
io_sethandler(uint16_t base, uint16_t size, uint8_t (*rb)(uint16_t, void *), uint16_t (*rw)(uint16_t, void *),
              uint32_t (*rl)(uint16_t, void *), void (*wb)(uint16_t, uint8_t, void *), void (*ww)(uint16_t, uint16_t, void *),
              void (*wl)(uint16_t, uint32_t, void *), void *priv)
{
    (void) size; (void) rb; (void) rl; (void) wb; (void) wl;
    fake_io_base = base;
    io_rw        = rw;
    io_ww        = ww;
    io_priv      = priv;
}

void
io_removehandler(uint16_t base, uint16_t size, uint8_t (*rb)(uint16_t, void *), uint16_t (*rw)(uint16_t, void *),
                 uint32_t (*rl)(uint16_t, void *), void (*wb)(uint16_t, uint8_t, void *), void (*ww)(uint16_t, uint16_t, void *),
                 void (*wl)(uint16_t, uint32_t, void *), void *priv)
{
    (void) base; (void) size; (void) rb; (void) rw; (void) rl; (void) wb; (void) ww; (void) wl; (void) priv;
    io_rw = NULL;
}

uint16_t fake_io_rw(uint16_t port) { return io_rw(fake_io_base + port, io_priv); }
void     fake_io_ww(uint16_t port, uint16_t val) { io_ww(fake_io_base + port, val, io_priv); }

/* ---- memory-mapped window ---- */
static uint32_t (*mm_rl)(uint32_t, void *);
static void (*mm_wl)(uint32_t, uint32_t, void *);
static void *mm_priv;
uint32_t     fake_mmio_base;

void
mem_mapping_add(mem_mapping_t *m, uint32_t base, uint32_t size, uint8_t (*rb)(uint32_t, void *), uint16_t (*rw)(uint32_t, void *),
                uint32_t (*rl)(uint32_t, void *), void (*wb)(uint32_t, uint8_t, void *), void (*ww)(uint32_t, uint16_t, void *),
                void (*wl)(uint32_t, uint32_t, void *), uint8_t *exec, uint32_t flags, void *priv)
{
    (void) m; (void) base; (void) size; (void) rb; (void) rw; (void) wb; (void) ww; (void) exec; (void) flags;
    mm_rl   = rl;
    mm_wl   = wl;
    mm_priv = priv;
}
void mem_mapping_set_addr(mem_mapping_t *m, uint32_t base, uint32_t size) { (void) m; (void) size; fake_mmio_base = base; }
void mem_mapping_disable(mem_mapping_t *m) { (void) m; fake_mmio_base = 0; }

uint32_t fake_mmio_rl(uint32_t off) { return mm_rl(fake_mmio_base + off, mm_priv); }
void     fake_mmio_wl(uint32_t off, uint32_t val) { mm_wl(fake_mmio_base + off, val, mm_priv); }

/* ---- timers ---- */
static void (*timer_cb[4])(void *);
static void *timer_priv[4];
static int   ntimers;

void
timer_add(pc_timer_t *t, void (*cb)(void *), void *p, int start)
{
    (void) t; (void) start;
    timer_cb[ntimers]   = cb;
    timer_priv[ntimers] = p;
    ntimers++;
}
void timer_on_auto(pc_timer_t *t, double period) { (void) t; (void) period; }

void
fake_frames(int n)
{
    while (n--)
        for (int i = 0; i < ntimers; i++)
            timer_cb[i](timer_priv[i]);
}

/* ---- the rest ---- */
mutex_t *thread_create_mutex(void) { return (mutex_t *) 1; }
int      thread_wait_mutex(mutex_t *m) { (void) m; return 1; }
int      thread_release_mutex(mutex_t *m) { (void) m; return 1; }
void     thread_close_mutex(mutex_t *m) { (void) m; }
void     pclog(const char *fmt, ...) { (void) fmt; }
const device_t device_none = { .name = "None", .internal_name = "none" };
const char *device_get_internal_name(const device_t *d) { return d ? d->internal_name : ""; }
void *device_add(const device_t *d) { (void) d; return NULL; }
usbn_device_t *usbn_host_open(uint16_t v, uint16_t p, char *e, int n) { (void) v; (void) p; snprintf(e, n, "test"); return NULL; }

/* ---- the fake device ----
   GET_DESCRIPTOR(device) on the control pipe (NAKing the first IN packets,
   as a passed-through device in flight does), a stall on endpoint 1 OUT, a
   two-byte short packet on endpoint 2 IN. */
const uint8_t fake_dev_desc[18] = { 18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x34, 0x12, 0x78, 0x56, 0, 1, 1, 2, 0, 1 };

static int
fake_packet(usbn_device_t *d, uint8_t pid, uint8_t ep, uint8_t *buf, int len)
{
    fake_dev_t *f = (fake_dev_t *) d;

    if (ep == 0) {
        if (pid == USB_PID_SETUP) {
            f->ctl_pos = 0;
            return 8;
        }
        if (pid == USB_PID_IN) {
            if (f->naks > 0) {
                f->naks--;
                return USBN_NAK;
            }
            int n = 18 - f->ctl_pos;
            if (n > len)
                n = len;
            memcpy(buf, fake_dev_desc + f->ctl_pos, n);
            f->ctl_pos += n;
            return n;
        }
        return 0;
    }
    if ((ep == 1) && (pid == USB_PID_OUT))
        return USBN_STALL;
    if ((ep == 2) && (pid == USB_PID_IN)) {
        buf[0] = 0xaa;
        buf[1] = 0x55;
        return 2;
    }
    return USBN_NAK;
}

static void fake_reset(usbn_device_t *d) { ((fake_dev_t *) d)->resets++; }
static void fake_destroy(usbn_device_t *d) { ((fake_dev_t *) d)->destroyed++; }

void
fake_dev_init(fake_dev_t *f, const char *name, int speed)
{
    memset(f, 0, sizeof(*f));
    snprintf(f->dev.name, sizeof(f->dev.name), "%s", name);
    f->dev.speed   = speed;
    f->dev.priv    = f;
    f->dev.packet  = fake_packet;
    f->dev.reset   = fake_reset;
    f->dev.destroy = fake_destroy;
}
