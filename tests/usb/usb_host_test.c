/*
 * 86Box-Next  Tests for USB host passthrough (src/usb/usb_host.c), in
 *             particular a high-speed device shown at full speed.
 *
 *             usb_host.c is compiled against the real libusb.h, with every
 *             libusb function it calls faked here: one high-speed device with
 *             512-byte bulk endpoints and a 1024-byte interrupt endpoint,
 *             whose transfers complete at once and are recorded.  The guest's
 *             side is driven packet by packet, as a USB 1.1 controller would.
 *
 *             No test framework: build with CTest (BUILD_TESTING=ON).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb.h>
#include <86box/86box.h>
#include <86box/thread.h>
#include <86box/usb_next.h>

static int checks, failures;
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

/* ------------------------------------------------------- the emulator */

void       pclog(const char *fmt, ...) { (void) fmt; }
thread_t  *thread_create_named(void (*f)(void *), void *p, const char *n) { (void) f; (void) p; (void) n; return (thread_t *) 1; }
int        thread_wait(thread_t *t) { (void) t; return 0; }
mutex_t   *thread_create_mutex(void) { return (mutex_t *) 1; }
int        thread_wait_mutex(mutex_t *m) { (void) m; return 1; }
int        thread_release_mutex(mutex_t *m) { (void) m; return 1; }
void       plat_delay_ms(uint32_t ms) { (void) ms; }
int        usbn_bus_high_speed(void) { return 0; }
int        usbn_trace_on;
uint32_t   usbn_frame_count;
void       usbn_trace(const char *fmt, ...) { (void) fmt; }
uint32_t   usbn_ms(void) { return 0; }

/* ------------------------------------------------------- the fake device */

static const struct libusb_endpoint_descriptor eps[3] = {
    { .bLength = 7, .bDescriptorType = 5, .bEndpointAddress = 0x81, .bmAttributes = 2, .wMaxPacketSize = 512 },
    { .bLength = 7, .bDescriptorType = 5, .bEndpointAddress = 0x02, .bmAttributes = 2, .wMaxPacketSize = 512 },
    { .bLength = 7, .bDescriptorType = 5, .bEndpointAddress = 0x83, .bmAttributes = 3, .wMaxPacketSize = 1024, .bInterval = 4 },
};
/* Interface 1, USB audio: 48 kHz 16-bit stereo, 192 bytes a millisecond each way */
static const struct libusb_endpoint_descriptor aud[2] = {
    { .bLength = 9, .bDescriptorType = 5, .bEndpointAddress = 0x84, .bmAttributes = 1, .wMaxPacketSize = 192, .bInterval = 1 },
    { .bLength = 9, .bDescriptorType = 5, .bEndpointAddress = 0x05, .bmAttributes = 1, .wMaxPacketSize = 192, .bInterval = 1 },
};
static const struct libusb_interface_descriptor alt0 = { .bLength = 9, .bDescriptorType = 4, .bInterfaceNumber = 0, .bNumEndpoints = 3, .endpoint = eps };
static const struct libusb_interface_descriptor alt1 = { .bLength = 9, .bDescriptorType = 4, .bInterfaceNumber = 1, .bNumEndpoints = 2, .endpoint = aud };
static const struct libusb_interface            itfs[2] = { { .altsetting = &alt0, .num_altsetting = 1 }, { .altsetting = &alt1, .num_altsetting = 1 } };
static struct libusb_config_descriptor          cfg  = { .bLength = 9, .bDescriptorType = 2, .bNumInterfaces = 2, .interface = itfs };

/* The same configuration as the bytes GET_DESCRIPTOR returns. */
static const uint8_t cfg_bytes[39] = {
    9, 2, 39, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 3, 8, 6, 0x50, 0,
    7, 5, 0x81, 2, 0x00, 0x02, 0,
    7, 5, 0x02, 2, 0x00, 0x02, 0,
    7, 5, 0x83, 3, 0x00, 0x04, 4,
};

static int dev_tag, handle_tag, ctx_tag;
#define FAKE_DEV ((libusb_device *) &dev_tag)
#define FAKE_H   ((libusb_device_handle *) &handle_tag)

/* What the device was sent, transfer by transfer, and what it will send. */
static int     out_lens[32], n_out;
static uint8_t out_data[4096];
static int     out_total;
static uint8_t in_stream[4096];
static int     in_left, in_off;   /* the device's current IN transfer */
static int     hold_in;           /* IN transfers stay in flight until released */
static struct libusb_transfer *held;
static int     last_wlength;      /* what the device was asked for last */

int  libusb_init(libusb_context **c) { *c = (libusb_context *) &ctx_tag; return 0; }
int  libusb_set_option(libusb_context *c, enum libusb_option o, ...) { (void) c; (void) o; return LIBUSB_ERROR_NOT_FOUND; }
int  libusb_handle_events_timeout_completed(libusb_context *c, struct timeval *t, int *d) { (void) c; (void) t; (void) d; return 0; }
void libusb_interrupt_event_handler(libusb_context *c) { (void) c; }
const char *libusb_error_name(int e) { (void) e; return "fake"; }

ssize_t
libusb_get_device_list(libusb_context *c, libusb_device ***list)
{
    static libusb_device *l[2] = { FAKE_DEV, NULL };
    (void) c;
    *list = l;
    return 1;
}
void           libusb_free_device_list(libusb_device **l, int u) { (void) l; (void) u; }
libusb_device *libusb_ref_device(libusb_device *d) { return d; }
void           libusb_unref_device(libusb_device *d) { (void) d; }

int
libusb_get_device_descriptor(libusb_device *d, struct libusb_device_descriptor *dd)
{
    (void) d;
    memset(dd, 0, sizeof(*dd));
    dd->idVendor  = 0x1234;
    dd->idProduct = 0x5678;
    return 0;
}
int            libusb_get_device_speed(libusb_device *d) { (void) d; return LIBUSB_SPEED_HIGH; }
uint8_t        libusb_get_bus_number(libusb_device *d) { (void) d; return 1; }
int            libusb_get_port_numbers(libusb_device *d, uint8_t *p, int n) { (void) d; (void) n; p[0] = 3; return 1; }
int            libusb_open(libusb_device *d, libusb_device_handle **h) { (void) d; *h = FAKE_H; return 0; }
void           libusb_close(libusb_device_handle *h) { (void) h; }
libusb_device *libusb_get_device(libusb_device_handle *h) { (void) h; return FAKE_DEV; }
int            libusb_set_auto_detach_kernel_driver(libusb_device_handle *h, int e) { (void) h; (void) e; return 0; }
int            libusb_claim_interface(libusb_device_handle *h, int i) { (void) h; (void) i; return 0; }
int            libusb_release_interface(libusb_device_handle *h, int i) { (void) h; (void) i; return 0; }
int            libusb_set_configuration(libusb_device_handle *h, int c) { (void) h; (void) c; return 0; }
int            libusb_get_configuration(libusb_device_handle *h, int *c) { (void) h; *c = 1; return 0; }
int            libusb_set_interface_alt_setting(libusb_device_handle *h, int i, int a) { (void) h; (void) i; (void) a; return 0; }
int            libusb_clear_halt(libusb_device_handle *h, unsigned char e) { (void) h; (void) e; return 0; }
int            libusb_get_string_descriptor_ascii(libusb_device_handle *h, uint8_t i, unsigned char *s, int n) { (void) h; (void) i; (void) s; (void) n; return 0; }
int            libusb_get_active_config_descriptor(libusb_device *d, struct libusb_config_descriptor **c) { (void) d; *c = &cfg; return 0; }
void           libusb_free_config_descriptor(struct libusb_config_descriptor *c) { (void) c; }

struct libusb_transfer *
libusb_alloc_transfer(int n)
{
    return calloc(1, sizeof(struct libusb_transfer) + n * sizeof(struct libusb_iso_packet_descriptor));
}

/* Isochronous transfers stay on the "device" until fake_iso_round() says a
   round of frames has passed, as on real hardware. */
static struct libusb_transfer *iso_pending[64];
static int                     n_iso_pending;
static int                     iso_in_size = 188, iso_seq;
static int                     iso_out_pkts[256], iso_out_first[256], n_iso_out;

static void
fake_iso_round(void)
{
    struct libusb_transfer *round[64];
    int                     n = n_iso_pending;

    memcpy(round, iso_pending, n * sizeof(round[0]));
    n_iso_pending = 0;
    for (int k = 0; k < n; k++) {
        struct libusb_transfer *t = round[k];
        for (int i = 0; i < t->num_iso_packets; i++) {
            struct libusb_iso_packet_descriptor *d = &t->iso_packet_desc[i];
            d->status = LIBUSB_TRANSFER_COMPLETED;
            if (t->endpoint & 0x80) {
                memset(libusb_get_iso_packet_buffer_simple(t, i), iso_seq++ & 0xff, iso_in_size);
                d->actual_length = iso_in_size;
            } else {
                iso_out_pkts[n_iso_out]  = d->length;
                iso_out_first[n_iso_out] = d->length ? libusb_get_iso_packet_buffer_simple(t, i)[0] : -1;
                n_iso_out++;
                d->actual_length = d->length;
            }
        }
        t->status = LIBUSB_TRANSFER_COMPLETED;
        t->callback(t);
    }
}

static void
fake_iso_abandon_round(void)   /* what the device does with cancelled ones */
{
    struct libusb_transfer *round[64];
    int                     n = n_iso_pending;

    memcpy(round, iso_pending, n * sizeof(round[0]));
    n_iso_pending = 0;
    for (int k = 0; k < n; k++) {
        round[k]->status = LIBUSB_TRANSFER_CANCELLED;
        round[k]->callback(round[k]);
    }
}
void                    libusb_free_transfer(struct libusb_transfer *t) { free(t); }
int                     libusb_cancel_transfer(struct libusb_transfer *t) { (void) t; return 0; }

/* Transfers complete at once, as if the device were instant. */
int
libusb_submit_transfer(struct libusb_transfer *t)
{
    if (t->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
        iso_pending[n_iso_pending++] = t;
        return 0;
    }
    t->status        = LIBUSB_TRANSFER_COMPLETED;
    t->actual_length = 0;
    if ((t->endpoint & 0x80) && hold_in) {
        held = t;          /* completes when the test says so */
        return 0;
    }
    if (t->endpoint == 0) {
        struct libusb_control_setup *s = (struct libusb_control_setup *) t->buffer;
        last_wlength = s->wLength;
        if ((s->bRequest == LIBUSB_REQUEST_GET_DESCRIPTOR) && ((s->wValue >> 8) == LIBUSB_DT_CONFIG)) {
            int n = (s->wLength < (int) sizeof(cfg_bytes)) ? s->wLength : (int) sizeof(cfg_bytes);
            memcpy(t->buffer + 8, cfg_bytes, n);
            t->actual_length = n;
        }
    } else if (t->endpoint & 0x80) {
        /* One read takes up to what was asked from the device's transfer;
           a high-speed device sends whole 512-byte packets and a short last. */
        int n = (in_left < t->length) ? in_left : t->length;
        memcpy(t->buffer, in_stream + in_off, n);
        in_off += n;
        in_left -= n;
        t->actual_length = n;
    } else {
        out_lens[n_out++] = t->length;
        memcpy(out_data + out_total, t->buffer, t->length);
        out_total += t->length;
        t->actual_length = t->length;
    }
    t->callback(t);
    return 0;
}

/* ---------------------------------------------------------------- tests */

static usbn_device_t *dev;

/* A packet the way a controller retries it: until it is not a NAK. */
static int
xact(uint8_t pid, uint8_t ep, uint8_t *buf, int len)
{
    int r;
    for (int tries = 0; tries < 4; tries++)
        if ((r = dev->packet(dev, pid, ep, buf, len)) != USBN_NAK)
            return r;
    return r;
}

static void
test_descriptor(void)
{
    uint8_t setup[8] = { 0x80, 6, 0, 2, 0, 0, 39, 0 };
    uint8_t buf[64];

    CHECK(dev->packet(dev, USB_PID_SETUP, 0, setup, 8) == 8, "SETUP accepted");
    CHECK(dev->packet(dev, USB_PID_IN, 0, buf, 64) == USBN_NAK, "first IN NAKs while the device answers");
    int n = xact(USB_PID_IN, 0, buf, 64);
    CHECK(n == 39, "configuration returned (%d bytes)", n);
    CHECK(buf[18 + 4] == 64 && buf[18 + 5] == 0, "bulk IN shown as 64 bytes");
    CHECK(buf[25 + 4] == 64 && buf[25 + 5] == 0, "bulk OUT shown as 64 bytes");
    CHECK(buf[32 + 4] == 64 && buf[32 + 5] == 0, "interrupt IN shown as 64 bytes");
    CHECK(buf[32 + 6] == 1, "interrupt interval 2^3 microframes -> 1 ms (%d)", buf[32 + 6]);
    CHECK(!memcmp(buf, cfg_bytes, 18), "the rest is untouched");
    CHECK(xact(USB_PID_OUT, 0, buf, 0) == 0, "status stage");
}

static void
test_out_regroup(void)
{
    uint8_t pkt[64];

    /* 512 bytes in eight full-speed packets: one high-speed packet. */
    n_out = out_total = 0;
    for (int i = 0; i < 8; i++) {
        memset(pkt, i, 64);
        CHECK(xact(USB_PID_OUT, 2, pkt, 64) == 64, "OUT packet %d acknowledged", i);
    }
    CHECK(n_out == 1 && out_lens[0] == 512, "eight packets went as one 512-byte transfer (%d, %d)", n_out, out_lens[0]);
    CHECK(out_data[0] == 0 && out_data[511] == 7, "in order");

    /* A short packet ends the guest's transfer: it goes as it is. */
    memset(pkt, 0xcb, 31);
    CHECK(xact(USB_PID_OUT, 2, pkt, 31) == 31, "short OUT acknowledged");
    CHECK(n_out == 2 && out_lens[1] == 31, "a 31-byte transfer (%d)", out_lens[1]);

    /* 576 bytes: 512 at once, and the last 64 when the guest turns to
       another endpoint (a disk reading its status). */
    for (int i = 0; i < 9; i++)
        xact(USB_PID_OUT, 2, pkt, 64);
    CHECK(n_out == 3 && out_lens[2] == 512, "first 512 sent");
    in_left = 13;
    in_off  = 0;
    memset(in_stream, 0x5a, 13);
    uint8_t buf[64];
    int     n = xact(USB_PID_IN, 1, buf, 64);
    CHECK(n_out == 4 && out_lens[3] == 64, "the remaining 64 went before the IN (%d)", n_out >= 4 ? out_lens[3] : -1);
    CHECK(n == 13, "and the IN answered (%d)", n);

    /* A lone 64 with nothing after it goes after a few idle frames. */
    xact(USB_PID_OUT, 2, pkt, 64);
    dev->frame(dev);
    dev->frame(dev);
    CHECK(n_out == 4, "not yet after two frames");
    dev->frame(dev);
    CHECK(n_out == 5 && out_lens[4] == 64, "sent after three idle frames");
}

static void
test_in_split(void)
{
    uint8_t buf[64];
    int     n, got = 0, ok = 1;

    /* The device sends 576 bytes: 512 and a short 64. */
    for (int i = 0; i < 576; i++)
        in_stream[i] = (uint8_t) i;
    in_left = 576;
    in_off  = 0;
    for (int i = 0; i < 9; i++) {
        n = xact(USB_PID_IN, 1, buf, 64);
        ok &= (n == 64) && (buf[0] == (uint8_t) (i * 64));
        got += (n > 0) ? n : 0;
    }
    CHECK(ok && got == 576, "576 bytes as nine 64-byte packets, in order (%d)", got);
    n = xact(USB_PID_IN, 1, buf, 64);
    CHECK(n == 0, "then a zero-length packet: the device's transfer ended there (%d)", n);

    /* 100 bytes: 64 and a short 36. */
    in_left = 100;
    in_off  = 0;
    CHECK(xact(USB_PID_IN, 1, buf, 64) == 64, "64");
    CHECK(xact(USB_PID_IN, 1, buf, 64) == 36, "then a short 36 ends it");
}

/* A port reset while a read is in flight must not wait for libusb, and the
   read, when it does come back, must not land in what the guest does next. */
static void
test_reset_does_not_wait(void)
{
    uint8_t buf[64];

    hold_in = 1;
    CHECK(dev->packet(dev, USB_PID_IN, 1, buf, 64) == USBN_NAK, "a read goes out and stays in flight");
    struct libusb_transfer *late = held;
    CHECK(late != NULL, "the device has it");
    dev->reset(dev);   /* returns at once: nothing here can call it back */
    hold_in = 0;

    /* The abandoned read comes back, with data, after the reset. */
    memset(late->buffer, 0xee, 64);
    late->status        = LIBUSB_TRANSFER_COMPLETED;
    late->actual_length = 64;
    late->callback(late);

    for (int i = 0; i < 64; i++)
        in_stream[i] = 0x11;
    in_left = 64;
    in_off  = 0;
    int n   = xact(USB_PID_IN, 1, buf, 64);
    CHECK(n == 64 && buf[0] == 0x11, "the next read gets the device's new data, not the abandoned read's (%d, %02X)", n, buf[0]);
    CHECK(xact(USB_PID_IN, 1, buf, 64) == 0, "and a zero-length packet ends it (64 < 512 on a 64-byte boundary)");
}

/* CLEAR_FEATURE(ENDPOINT_HALT) starts an endpoint over: what is left of a
   high-speed packet half handed out must not reach the guest afterwards. */
static void
test_clear_halt_drops_buffered(void)
{
    uint8_t buf[64];
    uint8_t clear[8] = { 0x02, 0x01, 0x00, 0x00, 0x81, 0x00, 0x00, 0x00 };

    for (int i = 0; i < 512; i++)
        in_stream[i] = 0x22;
    in_left = 512;
    in_off  = 0;
    CHECK(xact(USB_PID_IN, 1, buf, 64) == 64, "first 64 of a 512-byte packet");

    CHECK(dev->packet(dev, USB_PID_SETUP, 0, clear, 8) == 8, "CLEAR_FEATURE(HALT) on 81");
    CHECK(xact(USB_PID_IN, 0, buf, 0) == 0, "its status stage");

    for (int i = 0; i < 64; i++)
        in_stream[i] = 0x33;
    in_left = 64;
    in_off  = 0;
    int n   = xact(USB_PID_IN, 1, buf, 64);
    CHECK(n == 64 && buf[0] == 0x33, "after it, new data -- not the 448 bytes left over (%d, %02X)", n, buf[0]);
}

/* A SETUP is never refused: a read longer than the buffer is capped. */
static void
test_long_control_read(void)
{
    uint8_t setup[8] = { 0x80, 6, 0, 2, 0, 0, 0xff, 0xff };
    uint8_t buf[64];

    CHECK(dev->packet(dev, USB_PID_SETUP, 0, setup, 8) == 8, "SETUP with wLength 65535 accepted");
    int n = xact(USB_PID_IN, 0, buf, 64);
    CHECK(n == 39, "the descriptor comes back (%d)", n);
    CHECK(last_wlength == 4096, "the device was asked for 4096 (%d)", last_wlength);
    CHECK(xact(USB_PID_OUT, 0, buf, 0) == 0, "status stage");
}

static void
test_native_speed(void)
{
    uint8_t buf[512];

    dev->fs_view = 0;
    in_left      = 512;
    in_off       = 0;
    CHECK(xact(USB_PID_IN, 1, buf, 512) == 512, "at high speed a 512-byte packet passes as it is");
}

/* USB audio: a microphone (iso IN 84) and speakers (iso OUT 05). */
static void
test_iso_in(void)
{
    uint8_t buf[192];
    int     ok = 1;

    CHECK(dev->iso != NULL, "the passthrough streams isochronous endpoints");
    CHECK(dev->iso(dev, USB_PID_IN, 4, buf, 192) == 0, "first frame: nothing yet (silence)");
    CHECK(n_iso_pending == 4, "the stream put four transfers on the device (%d)", n_iso_pending);

    iso_seq = 0;
    fake_iso_round();   /* 4 transfers x 8 packets of 188 bytes */
    CHECK(n_iso_pending == 4, "and each went straight back (%d)", n_iso_pending);
    for (int i = 0; i < 32; i++) {
        int n = dev->iso(dev, USB_PID_IN, 4, buf, 192);
        ok &= (n == 188) && (buf[0] == i) && (buf[187] == i);
    }
    CHECK(ok, "32 packets of 188 bytes, one per frame, in order");
    CHECK(dev->iso(dev, USB_PID_IN, 4, buf, 192) == 0, "queue empty: an empty packet, not a stall");

    /* The device runs ahead (the emulation is slow): the latency is bounded
       by dropping the oldest. */
    fake_iso_round();
    fake_iso_round();   /* 64 packets, 32 fit */
    int n = dev->iso(dev, USB_PID_IN, 4, buf, 192);
    CHECK(n == 188 && buf[0] == 64, "after 64 packets unread the oldest kept is #64 (%d)", buf[0]);
}

static void
test_iso_out(void)
{
    uint8_t buf[192];

    n_iso_out = 0;
    for (int i = 0; i < 15; i++) {
        memset(buf, 0x40 + i, 176);
        CHECK(dev->iso(dev, USB_PID_OUT, 5, buf, 176) == 176, "OUT packet %d taken", i);
    }
    int before = n_iso_pending;
    memset(buf, 0x40 + 15, 176);
    dev->iso(dev, USB_PID_OUT, 5, buf, 176);
    CHECK(n_iso_pending - before == 2, "a 16-packet cushion goes out as two transfers (%d)", n_iso_pending - before);

    fake_iso_round();   /* also runs the IN stream's round */
    int ok = (n_iso_out == 16);
    for (int i = 0; i < n_iso_out && i < 16; i++)
        ok &= (iso_out_pkts[i] == 176) && (iso_out_first[i] == 0x40 + i);
    CHECK(ok, "the device played 16 packets of 176 bytes, in order (%d)", n_iso_out);
}

static void
test_iso_stops_on_set_interface(void)
{
    uint8_t buf[192];
    uint8_t setif[8] = { 0x01, 0x0b, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };   /* alt 0: stop streaming */

    CHECK(n_iso_pending > 0, "streams running");

    /* A bulk read on interface 0 in flight across the switch of interface 1. */
    hold_in = 1;
    CHECK(dev->packet(dev, USB_PID_IN, 1, buf, 64) == USBN_NAK, "a bulk read on interface 0 goes out");
    struct libusb_transfer *bulk = held;
    hold_in = 0;

    CHECK(dev->packet(dev, USB_PID_SETUP, 0, setif, 8) == 8, "SET_INTERFACE 1 alt 0");
    CHECK(xact(USB_PID_IN, 0, buf, 0) == 0, "its status stage");
    fake_iso_abandon_round();   /* the cancelled transfers come back */
    CHECK(n_iso_pending == 0, "nothing resubmitted after the stop");

    memset(bulk->buffer, 0x77, 64);
    bulk->status        = LIBUSB_TRANSFER_COMPLETED;
    bulk->actual_length = 64;
    bulk->callback(bulk);
    int n = xact(USB_PID_IN, 1, buf, 64);
    CHECK(n == 64 && buf[0] == 0x77, "interface 0's read was not touched by it (%d, %02X)", n, buf[0]);

    CHECK(dev->iso(dev, USB_PID_IN, 4, buf, 192) == 0, "a new stream starts empty");
    CHECK(n_iso_pending == 4, "with its own four transfers");
    fake_iso_abandon_round();
}

int
main(void)
{
    char err[256];

    dev = usbn_host_open(0x1234, 0x5678, err, sizeof(err));
    CHECK(dev != NULL, "a high-speed device opens for a USB 1.1 controller: %s", dev ? "" : err);
    if (!dev)
        return 1;
    CHECK(dev->speed == USBN_SPEED_HIGH, "it says high speed");
    dev->fs_view = 1;   /* what a USB 1.1 port does on connect */

    test_descriptor();
    test_out_regroup();
    test_in_split();
    test_reset_does_not_wait();
    test_clear_halt_drops_buffered();
    test_long_control_read();
    test_native_speed();
    test_iso_in();
    test_iso_out();
    test_iso_stops_on_set_interface();
    dev->destroy(dev);

    printf("host: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
