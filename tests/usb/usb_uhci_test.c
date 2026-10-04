/*
 * 86Box-Next  Tests for the UHCI controller (src/usb/usb_uhci.c).
 *
 *             The controller runs against a fake guest memory, PCI bus, I/O
 *             space, timer and mutex, with a fake USB device on its port.  The
 *             tests build real UHCI schedules -- frame list, queue head,
 *             transfer descriptors -- the way a guest driver would, step the
 *             1 ms frame by hand and check what the controller wrote back,
 *             against the UHCI Design Guide 1.1.
 *
 *             Self-contained (no test framework): build and run with
 *               gcc -I src/include -I build/src/include tests/usb/usb_uhci_test.c \
 *                   src/usb/usb_uhci.c -o usb_uhci_test && ./usb_uhci_test
 *             or through CTest with BUILD_TESTING=ON.
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

#define UNUSED_TS /* parameters the fakes do not need */

/* ------------------------------------------------------------- the fakes */

static uint8_t guest[1 << 20];   /* guest memory */
uint8_t       *ram = guest;

void dma_bm_read(uint32_t a, uint8_t *d, uint32_t n, UNUSED_TS int ts) { memcpy(d, ram + a, n); }
void dma_bm_write(uint32_t a, const uint8_t *d, uint32_t n, UNUSED_TS int ts) { memcpy(ram + a, d, n); }

static uint8_t (*pci_rd)(int, int, int, void *);
static void (*pci_wr)(int, int, int, uint8_t, void *);
static void   *pci_priv;
static int     irq_level;

void
pci_add_card(UNUSED_TS uint8_t t, uint8_t (*r)(int, int, int, void *), void (*w)(int, int, int, uint8_t, void *), void *p, uint8_t *slot)
{
    pci_rd = r;
    pci_wr = w;
    pci_priv = p;
    *slot = 5;
}

void
pci_irq(UNUSED_TS uint8_t slot, UNUSED_TS uint8_t pin, UNUSED_TS int level, int set, uint8_t *st)
{
    irq_level = set;
    *st       = set;
}

static uint16_t (*io_rw)(uint16_t, void *);
static void (*io_ww)(uint16_t, uint16_t, void *);
static void    *io_priv;
static uint16_t io_base;

void
io_sethandler(uint16_t base, UNUSED_TS uint16_t size, UNUSED_TS uint8_t (*rb)(uint16_t, void *), uint16_t (*rw)(uint16_t, void *),
              UNUSED_TS uint32_t (*rl)(uint16_t, void *), UNUSED_TS void (*wb)(uint16_t, uint8_t, void *),
              void (*ww)(uint16_t, uint16_t, void *), UNUSED_TS void (*wl)(uint16_t, uint32_t, void *), void *priv)
{
    io_base = base;
    io_rw   = rw;
    io_ww   = ww;
    io_priv = priv;
}

void
io_removehandler(UNUSED_TS uint16_t base, UNUSED_TS uint16_t size, UNUSED_TS uint8_t (*rb)(uint16_t, void *),
                 UNUSED_TS uint16_t (*rw)(uint16_t, void *), UNUSED_TS uint32_t (*rl)(uint16_t, void *),
                 UNUSED_TS void (*wb)(uint16_t, uint8_t, void *), UNUSED_TS void (*ww)(uint16_t, uint16_t, void *),
                 UNUSED_TS void (*wl)(uint16_t, uint32_t, void *), UNUSED_TS void *priv)
{
    io_rw = NULL;
}

static void (*frame_cb)(void *);
static void *frame_priv;

void
timer_add(UNUSED_TS pc_timer_t *t, void (*cb)(void *), void *p, UNUSED_TS int start)
{
    frame_cb   = cb;
    frame_priv = p;
}
void timer_on_auto(UNUSED_TS pc_timer_t *t, UNUSED_TS double period) { }

mutex_t *thread_create_mutex(void) { return (mutex_t *) 1; }
int      thread_wait_mutex(UNUSED_TS mutex_t *m) { return 1; }
int      thread_release_mutex(UNUSED_TS mutex_t *m) { return 1; }
void     thread_close_mutex(UNUSED_TS mutex_t *m) { }

void pclog(UNUSED_TS const char *fmt, ...) { }
const device_t device_none = { .name = "None", .internal_name = "none" };
const char *device_get_internal_name(const device_t *d) { return d ? d->internal_name : ""; }
void *device_add(UNUSED_TS const device_t *d) { return NULL; }
usbn_device_t *usbn_host_open(UNUSED_TS uint16_t v, UNUSED_TS uint16_t p, char *e, int n) { snprintf(e, n, "test"); return NULL; }

extern const device_t usb_uhci_via_device;

/* A fake full-speed device: answers GET_DESCRIPTOR(device) on the control
   pipe (NAKing the first IN, as a passed-through device in flight would),
   stalls endpoint 1 OUT, and returns a short packet on endpoint 2 IN. */
static const uint8_t dev_desc[18] = { 18, 1, 0x10, 0x01, 0, 0, 0, 8, 0x34, 0x12, 0x78, 0x56, 0, 1, 1, 2, 0, 1 };
static int           fake_naks, fake_resets, fake_destroyed;
static int           ctl_pos;

static int
fake_packet(usbn_device_t *d, uint8_t pid, uint8_t ep, uint8_t *buf, int len)
{
    (void) d;
    if (ep == 0) {
        if (pid == USB_PID_SETUP) {
            ctl_pos = 0;
            return 8;
        }
        if (pid == USB_PID_IN) {
            if (fake_naks-- > 0)
                return USBN_NAK;
            int n = 18 - ctl_pos;
            if (n > len)
                n = len;
            memcpy(buf, dev_desc + ctl_pos, n);
            ctl_pos += n;
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

static void fake_reset(usbn_device_t *d) { (void) d; fake_resets++; }
static void fake_destroy(usbn_device_t *d) { (void) d; fake_destroyed++; }

static usbn_device_t fake = { .name = "fake", .speed = USBN_SPEED_FULL, .packet = fake_packet, .reset = fake_reset, .destroy = fake_destroy };

/* ---------------------------------------------------------- the helpers */

static int failures, checks;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            failures++;                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

static uint16_t rdw(int r) { return io_rw(io_base + r, io_priv); }
static void     wrw(int r, uint16_t v) { io_ww(io_base + r, v, io_priv); }
static void     wrl_mem(uint32_t a, uint32_t v) { memcpy(ram + a, &v, 4); }
static uint32_t rdl_mem(uint32_t a) { uint32_t v; memcpy(&v, ram + a, 4); return v; }
static void     frames(int n) { while (n--) frame_cb(frame_priv); }

#define FL   0x10000
#define QH   0x11000
#define TD0  0x11100
#define BUF  0x12000

static void
td(uint32_t at, uint32_t link, uint32_t ctrl, uint8_t pid, uint8_t addr, uint8_t ep, int maxlen, uint32_t buf)
{
    wrl_mem(at, link);
    wrl_mem(at + 4, ctrl | 0x00800000 | 0x18000000);   /* active, C_ERR = 3 */
    wrl_mem(at + 8, pid | (addr << 8) | (ep << 15) | ((uint32_t) ((maxlen - 1) & 0x7ff) << 21));
    wrl_mem(at + 12, buf);
}

static void
schedule(uint32_t first_td)
{
    for (int i = 0; i < 1024; i++)
        wrl_mem(FL + i * 4, QH | 2);
    wrl_mem(QH, 1);
    wrl_mem(QH + 4, first_td);
}

/* --------------------------------------------------------------- tests */

static void
test_pci_and_registers(void)
{
    CHECK(pci_rd(0, 0, 1, pci_priv) == 0x06 && pci_rd(0, 1, 1, pci_priv) == 0x11, "vendor is VIA");
    CHECK(pci_rd(0, 0x0b, 1, pci_priv) == 0x0c && pci_rd(0, 0x0a, 1, pci_priv) == 0x03 && pci_rd(0, 0x09, 1, pci_priv) == 0x00,
          "class is serial bus / USB / UHCI");
    pci_wr(0, 0x20, 1, 0x00, pci_priv);
    pci_wr(0, 0x21, 1, 0xe0, pci_priv);
    pci_wr(0, 0x04, 1, 0x05, pci_priv);
    CHECK(io_rw != NULL && io_base == 0xe000, "BAR4 maps the registers at E000 (got %04X)", io_base);
    CHECK(rdw(0x02) & 0x20, "halted after reset");
    CHECK((rdw(0x10) & 0x80) && (rdw(0x12) & 0x80), "two ports, bit 7 reads 1");
    CHECK(!(rdw(0x14) & 0x80), "no third port");
}

static void
test_connect_and_enable(void)
{
    CHECK(usbn_attach(0, &fake), "attach accepted");
    frames(1);   /* plugs are applied at the frame */
    uint16_t p = rdw(0x10);
    CHECK((p & 0x0003) == 0x0003, "connect status and its change bit set (%04X)", p);
    CHECK(!(p & 0x0100), "full speed: no low-speed bit");
    wrw(0x10, 0x0002);                       /* clear CSC */
    CHECK(!(rdw(0x10) & 0x0002), "CSC is write-1-to-clear");
    wrw(0x10, 0x0200);                       /* reset */
    wrw(0x10, 0x0000);
    CHECK(fake_resets == 1, "port reset reaches the device");
    wrw(0x10, 0x0004);                       /* enable */
    CHECK(rdw(0x10) & 0x0004, "port enabled");
}

static void
test_control_transfer(void)
{
    /* GET_DESCRIPTOR(device, 18): SETUP, IN 8, IN 8, IN 2, status OUT. */
    static const uint8_t setup[8] = { 0x80, 6, 0, 1, 0, 0, 18, 0 };
    memcpy(ram + BUF, setup, 8);
    memset(ram + BUF + 0x100, 0, 32);
    td(TD0 + 0x00, TD0 + 0x20 | 4, 0, USB_PID_SETUP, 0, 0, 8, BUF);
    td(TD0 + 0x20, TD0 + 0x40 | 4, 0, USB_PID_IN, 0, 0, 8, BUF + 0x100);
    td(TD0 + 0x40, TD0 + 0x60 | 4, 0, USB_PID_IN, 0, 0, 8, BUF + 0x108);
    td(TD0 + 0x60, TD0 + 0x80 | 4, 0, USB_PID_IN, 0, 0, 8, BUF + 0x110);
    td(TD0 + 0x80, 1, 0x01000000, USB_PID_OUT, 0, 0, 0x800, 0);   /* zero length, IOC */
    schedule(TD0);

    wrw(0x04, 0x000f);                 /* all interrupts */
    wrl_mem(0, 0);
    wrw(0x08, FL & 0xffff);
    wrw(0x0a, FL >> 16);
    fake_naks = 2;
    wrw(0x00, 0x0001);                 /* run */
    CHECK(!(rdw(0x02) & 0x20), "running");

    frames(1);
    CHECK(!(rdl_mem(TD0 + 4) & 0x00800000), "SETUP done in the first frame");
    CHECK(rdl_mem(TD0 + 0x24) & 0x00800000, "first IN still active after a NAK");
    CHECK(rdl_mem(TD0 + 0x24) & 0x00080000, "NAK bit set on it");
    CHECK(rdl_mem(QH + 4) == (TD0 + 0x20 | 4), "queue element points at the NAKed TD");
    CHECK(!irq_level, "no interrupt yet");

    frames(3);
    CHECK(rdl_mem(QH + 4) & 1, "queue ran to its end (%08X)", rdl_mem(QH + 4));
    CHECK((rdl_mem(TD0 + 0x24) & 0x7ff) == 7, "first IN moved 8 bytes (ActLen 7)");
    CHECK((rdl_mem(TD0 + 0x64) & 0x7ff) == 1, "last IN moved 2 bytes (ActLen 1)");
    CHECK((rdl_mem(TD0 + 0x84) & 0x7ff) == 0x7ff, "status stage zero length (ActLen 7FF)");
    CHECK(!memcmp(ram + BUF + 0x100, dev_desc, 18), "descriptor in guest memory");
    CHECK(rdw(0x02) & 0x0001, "USBINT from the IOC TD");
    CHECK(irq_level, "IRQ asserted");
    wrw(0x02, 0x0001);
    CHECK(!irq_level, "IRQ drops when USBINT is cleared");
    CHECK(rdw(0x06) == 4, "frame number advanced to 4 (%d)", rdw(0x06));
}

static void
test_stall(void)
{
    /* SET_ADDRESS is the device's business; give it an address the way a
       driver would, then stall an OUT. */
    fake.addr = 3;
    td(TD0, 1, 0x01000000, USB_PID_OUT, 3, 1, 4, BUF);
    schedule(TD0);
    frames(1);
    uint32_t c = rdl_mem(TD0 + 4);
    CHECK(!(c & 0x00800000) && (c & 0x00400000), "STALL: inactive with the stalled bit (%08X)", c);
    CHECK(rdw(0x02) & 0x0002, "error interrupt status");
    CHECK(rdl_mem(QH + 4) == TD0, "queue stops at the stalled TD");
    wrw(0x02, 0x0003);
}

static void
test_short_packet(void)
{
    td(TD0, TD0 + 0x20 | 4, 0x20000000, USB_PID_IN, 3, 2, 8, BUF + 0x200);   /* SPD */
    td(TD0 + 0x20, 1, 0, USB_PID_IN, 3, 2, 8, BUF + 0x208);
    schedule(TD0);
    frames(1);
    CHECK((rdl_mem(TD0 + 4) & 0x7ff) == 1 && !(rdl_mem(TD0 + 4) & 0x00800000), "short packet: 2 bytes, done");
    CHECK(ram[BUF + 0x200] == 0xaa && ram[BUF + 0x201] == 0x55, "short packet data");
    CHECK(rdl_mem(QH + 4) == TD0, "short packet with SPD stops the queue");
    CHECK(rdl_mem(TD0 + 0x24) & 0x00800000, "the next TD is untouched");
    CHECK(rdw(0x02) & 0x0001, "short packet raises USBINT");
    wrw(0x02, 0x0003);
}

static void
test_no_device(void)
{
    td(TD0, 1, 0, USB_PID_IN, 9, 1, 8, BUF);
    schedule(TD0);
    frames(1);
    uint32_t c = rdl_mem(TD0 + 4);
    CHECK(!(c & 0x00800000) && (c & 0x00040000), "nobody at address 9: CRC/timeout (%08X)", c);
    wrw(0x02, 0x0003);
}

static void
test_disconnect(void)
{
    usbn_detach(0);
    frames(1);
    uint16_t p = rdw(0x10);
    CHECK(!(p & 0x0001) && (p & 0x0002), "unplugged: no connection, change bit set (%04X)", p);
    CHECK(!(p & 0x0004) && (p & 0x0008), "port disabled with its change bit");
    CHECK(fake_destroyed == 1, "the device is released");
}

static void
test_global_reset(void)
{
    wrw(0x00, 0x0002);   /* HCRESET */
    CHECK(rdw(0x02) & 0x20, "halted after HCRESET");
    CHECK(rdw(0x00) == 0, "command register cleared");
}

int
main(void)
{
    void *dev = usb_uhci_via_device.init(&usb_uhci_via_device);

    test_pci_and_registers();
    test_connect_and_enable();
    test_control_transfer();
    test_stall();
    test_short_packet();
    test_no_device();
    test_disconnect();
    test_global_reset();
    usb_uhci_via_device.close(dev);

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
