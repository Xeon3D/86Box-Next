/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             USB host passthrough: a real device on the host, handed to the
 *             guest through libusb.  Connecting a device to the virtual
 *             machine claims every one of its interfaces (detaching the
 *             host's own driver where the OS allows it), so while the VM has
 *             it the host does not; disconnecting releases it back.
 *
 *             Transfers are asynchronous.  A packet that has to go to the
 *             device is submitted and answered with NAK; the controller
 *             retries the TD every frame, and the retry after the device has
 *             answered completes it.  Control requests that change the
 *             host-side device state are done here rather than forwarded:
 *             SET_ADDRESS (the host already gave it one), SET_CONFIGURATION
 *             and SET_INTERFACE (claim / alternate setting) and
 *             CLEAR_FEATURE(ENDPOINT_HALT).
 *
 *             On Windows there are two ways in.  With UsbDk installed (a
 *             signed filter driver, installed once -- the same idea as
 *             VirtualBox's and VMware's USB monitor), any device is captured
 *             on demand and given back when released.  Without it, libusb can
 *             only open devices that use the WinUSB (or libusbK) driver,
 *             which Zadig installs per device.  Device names come from
 *             Windows' own device database, so listing never opens (and,
 *             under UsbDk, never captures) a device.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/thread.h>
#include <86box/plat.h>
#include <86box/plat_unused.h>
#include <86box/usb_next.h>

#ifndef USE_LIBUSB

int
usbn_host_available(void)
{
    return 0;
}

int
usbn_host_list(UNUSED(usbn_host_info_t *out), UNUSED(int max))
{
    return 0;
}

int
usbn_host_uses_usbdk(void)
{
    return 0;
}

usbn_device_t *
usbn_host_open(uint16_t vid, uint16_t pid, char *err, int errlen)
{
    snprintf(err, errlen, "this build has no USB passthrough (built without libusb)");
    (void) vid;
    (void) pid;
    return NULL;
}

#else

#include <libusb.h>

#ifdef ENABLE_USB_HOST_LOG
int usb_host_do_log = ENABLE_USB_HOST_LOG;

static void
host_log(const char *fmt, ...)
{
    va_list ap;

    if (usb_host_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define host_log(fmt, ...)
#endif

#define XFER_IDLE    0
#define XFER_PENDING 1
#define XFER_DONE    2

#define CTL_MAX      4096
#define DATA_MAX     20480    /* an EHCI qTD: five 4 KB pages */

/* One pipe direction's transfer, as the guest side sees it.  The libusb
   transfer in flight has its own buffer (sub_t below); the callback copies an
   answer in here and sets state last, with release ordering, so the
   emulation thread that reads state with acquire ordering sees the data. */
typedef struct xfer_t {
    struct libusb_transfer *t;          /* in flight, or NULL */
    uint32_t                t_submit;   /* trace: real ms and frame at submission */
    uint32_t                f_submit;
    int                     state;      /* XSTATE() to read across threads */
    int                     status;
    int                     actual;
    uint8_t                 buf[DATA_MAX + 8];
} xfer_t;

#define XSTATE(x) __atomic_load_n(&(x)->state, __ATOMIC_ACQUIRE)

/* A transfer in flight.  Cancelling one does not wait for libusb to finish
   with it: the guest side lets go (x = NULL) and carries on, and when the
   transfer does come back its callback only frees it. */
typedef struct sub_t {
    struct host_dev_t *hd;
    xfer_t            *x;               /* NULL: abandoned */
} sub_t;

typedef struct host_dev_t {
    usbn_device_t         dev;
    libusb_device_handle *h;
    uint16_t              vid, pid;

    uint8_t  ep_type[32];      /* LIBUSB_TRANSFER_TYPE_*, [ep | (in ? 16 : 0)] */
    uint16_t ep_maxp[32];      /* the device's own (high-speed) packet sizes */
    int     claimed[32];       /* interfaces we hold */
    int     nclaimed;

    /* control pipe */
    uint8_t setup[8];
    int     ctl_active;
    int     ctl_in;            /* data stage is device-to-host */
    int     ctl_len;           /* wLength */
    int     ctl_pos;           /* IN: bytes handed back; OUT: bytes gathered */
    uint8_t ctl_out[CTL_MAX];
    xfer_t  ctl;

    xfer_t in[16], out[16];
    int    inflight;           /* libusb transfers not yet called back, abandoned ones too */

    /* A high-speed device shown at full speed (dev.fs_view): the guest's
       full-speed packets, gathered into or cut out of high-speed ones. */
    int ctl_patched;           /* the configuration descriptor was rewritten */
    int out_acc[16];           /* OUT bytes gathered, not yet sent          */
    int out_idle[16];          /* frames since the last OUT packet           */
    int in_len[16], in_pos[16];/* the last high-speed IN packet, handed out */
    int in_zlp[16];            /* it was short but ended on a full-speed packet boundary */
} host_dev_t;

static libusb_context *ctx;
static int             use_usbdk;
static thread_t       *event_thread;
static volatile int    event_run;
static int             open_count;
static mutex_t        *ctx_lock;
static mutex_t        *xfer_lock;      /* callbacks against abandoning */

/* --------------------------------------------------------- the context --- */

static void
host_event_loop(UNUSED(void *priv))
{
    while (event_run) {
        struct timeval tv = { 0, 50000 };
        libusb_handle_events_timeout_completed(ctx, &tv, NULL);
    }
}

static int
host_ctx_init(void)
{
    if (xfer_lock == NULL)
        xfer_lock = thread_create_mutex();
    if (ctx)
        return 1;
    if (libusb_init(&ctx) != 0) {
        ctx = NULL;
        return 0;
    }
#ifdef _WIN32
    /* Must come straight after init; fails when UsbDk is not installed. */
    use_usbdk = (libusb_set_option(ctx, LIBUSB_OPTION_USE_USBDK) == LIBUSB_SUCCESS);
    pclog("USB: host passthrough through %s\n", use_usbdk ? "UsbDk" : "WinUSB");
#endif
    return 1;
}

int
usbn_host_uses_usbdk(void)
{
    return host_ctx_init() && use_usbdk;
}

static void
host_ctx_get(void)
{
    if (ctx_lock == NULL)
        ctx_lock = thread_create_mutex();
    thread_wait_mutex(ctx_lock);
    if (open_count++ == 0) {
        event_run    = 1;
        event_thread = thread_create(host_event_loop, NULL);
    }
    thread_release_mutex(ctx_lock);
}

static void
host_ctx_put(void)
{
    thread_wait_mutex(ctx_lock);
    if (--open_count == 0) {
        event_run = 0;
        libusb_interrupt_event_handler(ctx);
        thread_wait(event_thread);
        event_thread = NULL;
    }
    thread_release_mutex(ctx_lock);
}

int
usbn_host_available(void)
{
    return host_ctx_init();
}

/* ------------------------------------------------------- enumeration --- */

static int
map_speed(int s)
{
    switch (s) {
        case LIBUSB_SPEED_LOW:
            return USBN_SPEED_LOW;
        case LIBUSB_SPEED_FULL:
        case LIBUSB_SPEED_UNKNOWN:
            return USBN_SPEED_FULL;
        default:
            return USBN_SPEED_HIGH;
    }
}

/* Device names cost an open and two string requests, and the UI polls the
   list every couple of seconds: remember them by location and ID. */
static struct {
    uint8_t  bus, depth, path[8];
    uint16_t vid, pid;
    char     desc[128];
} name_cache[64];
static int name_cache_n;

#ifdef _WIN32
#    include <windows.h>
#    include <setupapi.h>
#    include <initguid.h>  /* define, not just declare, the DEVPKEY below */
#    include <devpkey.h>

/* The name Windows shows for the device: what the device itself reports
   (its product string, which Windows keeps as the "bus reported device
   description"), else the driver's description. */
static int
win_device_name(uint16_t vid, uint16_t pid, char *out, int len)
{
    char            want[32];
    int             found = 0;
    SP_DEVINFO_DATA di    = { .cbSize = sizeof(SP_DEVINFO_DATA) };
    HDEVINFO        set   = SetupDiGetClassDevsA(NULL, "USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);

    if (set == INVALID_HANDLE_VALUE)
        return 0;
    snprintf(want, sizeof(want), "VID_%04X&PID_%04X", vid, pid);
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &di); i++) {
        char  ids[1024];
        DWORD type;

        if (!SetupDiGetDeviceRegistryPropertyA(set, &di, SPDRP_HARDWAREID, &type, (BYTE *) ids, sizeof(ids), NULL))
            continue;
        if (!strstr(ids, want))
            continue;
        WCHAR      wname[256];
        DEVPROPTYPE pt;
        if (SetupDiGetDevicePropertyW(set, &di, &DEVPKEY_Device_BusReportedDeviceDesc, &pt, (BYTE *) wname, sizeof(wname), NULL, 0)
            && wname[0]) {
            WideCharToMultiByte(CP_UTF8, 0, wname, -1, out, len, NULL, NULL);
            found = 1;
        } else if (SetupDiGetDeviceRegistryPropertyA(set, &di, SPDRP_DEVICEDESC, &type, (BYTE *) out, len, NULL))
            found = 1;
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}
#endif

static const char *
cached_name(libusb_device *d, const struct libusb_device_descriptor *dd, uint8_t bus, const uint8_t *path, int depth)
{
    for (int i = 0; i < name_cache_n; i++) {
        if ((name_cache[i].bus == bus) && (name_cache[i].depth == depth) && !memcmp(name_cache[i].path, path, depth)
            && (name_cache[i].vid == dd->idVendor) && (name_cache[i].pid == dd->idProduct))
            return name_cache[i].desc;
    }

    int slot = (name_cache_n < 64) ? name_cache_n++ : 63;
    char  man[64] = "", prod[64] = "";
    libusb_device_handle *h;

#ifdef _WIN32
    (void) h;
    (void) d;
    win_device_name(dd->idVendor, dd->idProduct, prod, sizeof(prod));
#else
    if (libusb_open(d, &h) == 0) {
        if (dd->iManufacturer)
            libusb_get_string_descriptor_ascii(h, dd->iManufacturer, (unsigned char *) man, sizeof(man));
        if (dd->iProduct)
            libusb_get_string_descriptor_ascii(h, dd->iProduct, (unsigned char *) prod, sizeof(prod));
        libusb_close(h);
    }
#endif
    name_cache[slot].bus   = bus;
    name_cache[slot].depth = depth;
    memcpy(name_cache[slot].path, path, depth);
    name_cache[slot].vid = dd->idVendor;
    name_cache[slot].pid = dd->idProduct;
    if (prod[0])
        snprintf(name_cache[slot].desc, sizeof(name_cache[slot].desc), "%s%s%s", man, man[0] ? " " : "", prod);
    else
        snprintf(name_cache[slot].desc, sizeof(name_cache[slot].desc), "USB device %04X:%04X", dd->idVendor, dd->idProduct);
    return name_cache[slot].desc;
}

int
usbn_host_list(usbn_host_info_t *out, int max)
{
    libusb_device **list;
    ssize_t         n;
    int             count = 0;

    if (!host_ctx_init())
        return 0;
    n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; (i < n) && (count < max); i++) {
        struct libusb_device_descriptor dd;
        uint8_t                         path[8];
        int                             depth;

        if (libusb_get_device_descriptor(list[i], &dd) != 0)
            continue;
        if (dd.bDeviceClass == LIBUSB_CLASS_HUB)
            continue;
        depth = libusb_get_port_numbers(list[i], path, sizeof(path));
        if (depth <= 0)
            continue;   /* a root hub or something without a port */

        usbn_host_info_t *e = &out[count++];
        e->vid        = dd.idVendor;
        e->pid        = dd.idProduct;
        e->bus        = libusb_get_bus_number(list[i]);
        e->port_depth = (uint8_t) depth;
        memcpy(e->port_path, path, depth);
        e->speed = map_speed(libusb_get_device_speed(list[i]));
        snprintf(e->desc, sizeof(e->desc), "%s", cached_name(list[i], &dd, e->bus, path, depth));
    }
    libusb_free_device_list(list, 1);
    return count;
}

/* -------------------------------------------------------- interfaces --- */

static void
host_note_endpoints(host_dev_t *hd, const struct libusb_interface_descriptor *alt)
{
    for (int e = 0; e < alt->bNumEndpoints; e++) {
        const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
        const int i = (ep->bEndpointAddress & 0x0f) | ((ep->bEndpointAddress & 0x80) ? 16 : 0);
        hd->ep_type[i] = ep->bmAttributes & 3;
        hd->ep_maxp[i] = ep->wMaxPacketSize & 0x7ff;
    }
}

static void
host_release_all(host_dev_t *hd)
{
    for (int i = 0; i < hd->nclaimed; i++)
        libusb_release_interface(hd->h, hd->claimed[i]);
    hd->nclaimed = 0;
}

/* Take every interface of the active configuration: the device is the VM's
   now, and the host's drivers let go of it. */
static void
host_claim_all(host_dev_t *hd)
{
    struct libusb_config_descriptor *cfg;

    host_release_all(hd);
    memset(hd->ep_type, LIBUSB_TRANSFER_TYPE_BULK, sizeof(hd->ep_type));
    if (libusb_get_active_config_descriptor(libusb_get_device(hd->h), &cfg) != 0)
        return;
    for (int i = 0; (i < cfg->bNumInterfaces) && (hd->nclaimed < 32); i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        if (itf->num_altsetting < 1)
            continue;
        int num = itf->altsetting[0].bInterfaceNumber;
        int r   = libusb_claim_interface(hd->h, num);
        if (r == 0)
            hd->claimed[hd->nclaimed++] = num;
        else
            pclog("USB: %s: interface %d not claimed: %s\n", hd->dev.name, num, libusb_error_name(r));
        host_note_endpoints(hd, &itf->altsetting[0]);
    }
    libusb_free_config_descriptor(cfg);
}

static void
host_set_alt(host_dev_t *hd, int iface, int alt)
{
    struct libusb_config_descriptor *cfg;

    libusb_set_interface_alt_setting(hd->h, iface, alt);
    if (libusb_get_active_config_descriptor(libusb_get_device(hd->h), &cfg) != 0)
        return;
    for (int i = 0; i < cfg->bNumInterfaces; i++)
        for (int a = 0; a < cfg->interface[i].num_altsetting; a++)
            if ((cfg->interface[i].altsetting[a].bInterfaceNumber == iface) && (cfg->interface[i].altsetting[a].bAlternateSetting == alt))
                host_note_endpoints(hd, &cfg->interface[i].altsetting[a]);
    libusb_free_config_descriptor(cfg);
}

/* --------------------------------------------------------- transfers --- */

static const char *
xfer_status_name(int s)
{
    switch (s) {
        case LIBUSB_TRANSFER_COMPLETED: return "ok";
        case LIBUSB_TRANSFER_ERROR:     return "ERROR";
        case LIBUSB_TRANSFER_TIMED_OUT: return "TIMEOUT";
        case LIBUSB_TRANSFER_CANCELLED: return "cancelled";
        case LIBUSB_TRANSFER_STALL:     return "STALL";
        case LIBUSB_TRANSFER_NO_DEVICE: return "NO DEVICE";
        case LIBUSB_TRANSFER_OVERFLOW:  return "OVERFLOW";
        default:                        return "?";
    }
}

static void LIBUSB_CALL
host_xfer_cb(struct libusb_transfer *t)
{
    sub_t    *sub = (sub_t *) t->user_data;
    const int ctl = (t->type == LIBUSB_TRANSFER_TYPE_CONTROL);

    thread_wait_mutex(xfer_lock);
    xfer_t *x = sub->x;
    if (usbn_trace_on)
        usbn_trace("  done   ep %02X: %s, %d of %d bytes, %u ms, %u frames%s", t->endpoint, xfer_status_name(t->status),
                   t->actual_length, ctl ? t->length - 8 : t->length, x ? usbn_ms() - x->t_submit : 0,
                   x ? usbn_frame_count - x->f_submit : 0, x ? "" : " (abandoned)");
    if (x) {
        int n = t->actual_length;
        if (n > 0)
            memcpy(x->buf + (ctl ? 8 : 0), t->buffer + (ctl ? 8 : 0), n);
        x->status = t->status;
        x->actual = n;
        x->t      = NULL;
        __atomic_store_n(&x->state, XFER_DONE, __ATOMIC_RELEASE);
    }
    sub->hd->inflight--;
    thread_release_mutex(xfer_lock);

    free(t->buffer);
    free(sub);
    libusb_free_transfer(t);
}

static int
host_status_to_ret(int status)
{
    switch (status) {
        case LIBUSB_TRANSFER_COMPLETED:
            return 0;
        case LIBUSB_TRANSFER_STALL:
            return USBN_STALL;
        case LIBUSB_TRANSFER_OVERFLOW:
            return USBN_BABBLE;
        case LIBUSB_TRANSFER_NO_DEVICE:
            return USBN_NODEV;
        default:
            return USBN_IOERROR;
    }
}

/* Let go of a pipe's transfer without waiting for it. */
static void
host_cancel(xfer_t *x)
{
    thread_wait_mutex(xfer_lock);
    if (x->t) {
        ((sub_t *) x->t->user_data)->x = NULL;
        libusb_cancel_transfer(x->t);
        x->t = NULL;
    }
    x->state = XFER_IDLE;
    thread_release_mutex(xfer_lock);
}

/* An endpoint starts over (CLEAR_FEATURE(HALT), a new interface or
   configuration, a reset): nothing in flight, nothing buffered. */
static void
host_reset_endpoint(host_dev_t *hd, uint8_t epaddr)
{
    const int i = epaddr & 0x0f;

    if (epaddr & 0x80) {
        host_cancel(&hd->in[i]);
        hd->in_len[i] = hd->in_pos[i] = hd->in_zlp[i] = 0;
    } else {
        host_cancel(&hd->out[i]);
        hd->out_acc[i] = hd->out_idle[i] = 0;
    }
}

static void
host_reset_endpoints(host_dev_t *hd)
{
    for (int e = 1; e < 16; e++) {
        host_reset_endpoint(hd, e);
        host_reset_endpoint(hd, e | 0x80);
    }
}

static void
host_cancel_all(host_dev_t *hd)
{
    host_cancel(&hd->ctl);
    host_reset_endpoints(hd);
    hd->ctl_active = 0;
}

/* Only for closing the device: what was abandoned must have come back before
   the handle goes, at most a second. */
static void
host_wait_inflight(host_dev_t *hd)
{
    uint32_t t0 = usbn_ms();

    for (int tries = 0; tries < 100; tries++) {
        if (__atomic_load_n(&hd->inflight, __ATOMIC_ACQUIRE) <= 0)
            return;
        plat_delay_ms(10);
    }
    usbn_trace("  gave up after %u ms waiting for %d transfer(s)", usbn_ms() - t0, hd->inflight);
}

/* Submit a transfer with its own copy of what goes out. */
static int
host_submit_raw(host_dev_t *hd, xfer_t *x, struct libusb_transfer *t, uint8_t *data)
{
    sub_t *sub = malloc(sizeof(sub_t));

    sub->hd      = hd;
    sub->x       = x;
    t->user_data = sub;

    thread_wait_mutex(xfer_lock);
    x->t        = t;
    x->state    = XFER_PENDING;
    x->t_submit = usbn_ms();
    x->f_submit = usbn_frame_count;
    hd->inflight++;
    thread_release_mutex(xfer_lock);

    if (libusb_submit_transfer(t) != 0) {
        thread_wait_mutex(xfer_lock);
        x->t     = NULL;
        x->state = XFER_IDLE;
        hd->inflight--;
        thread_release_mutex(xfer_lock);
        free(sub);
        free(data);
        libusb_free_transfer(t);
        return 0;
    }
    return 1;
}

static int
host_submit(host_dev_t *hd, xfer_t *x, uint8_t ep, int len)
{
    int                     type = hd->ep_type[(ep & 0x0f) | ((ep & 0x80) ? 16 : 0)];
    uint8_t                *data = malloc(len ? len : 1);
    struct libusb_transfer *t    = libusb_alloc_transfer(0);

    if (!(ep & 0x80))
        memcpy(data, x->buf, len);
    if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT)
        libusb_fill_interrupt_transfer(t, hd->h, ep, data, len, host_xfer_cb, NULL, 0);
    else
        libusb_fill_bulk_transfer(t, hd->h, ep, data, len, host_xfer_cb, NULL, 0);
    if (usbn_trace_on)
        usbn_trace("  submit ep %02X: %s %d bytes", ep, (type == LIBUSB_TRANSFER_TYPE_INTERRUPT) ? "interrupt" : "bulk", len);
    if (!host_submit_raw(hd, x, t, data)) {
        usbn_trace("  submit ep %02X FAILED", ep);
        return 0;
    }
    return 1;
}

static int
host_submit_control(host_dev_t *hd)
{
    uint8_t                *data = malloc(8 + hd->ctl_len);
    struct libusb_transfer *t    = libusb_alloc_transfer(0);

    memcpy(data, hd->setup, 8);
    if (!hd->ctl_in && hd->ctl_len)
        memcpy(data + 8, hd->ctl_out, hd->ctl_len);
    libusb_fill_control_transfer(t, hd->h, data, host_xfer_cb, NULL, 5000);
    if (!host_submit_raw(hd, &hd->ctl, t, data)) {
        usbn_trace("  control submit FAILED");
        return 0;
    }
    return 1;
}

/* ----------------------------------------------------------- control --- */

#define REQ(bm, r) (((bm) << 8) | (r))

/* Requests done here at the status stage instead of being forwarded.
   Returns 1 when the request was one of them. */
static int
host_local_request(host_dev_t *hd)
{
    uint8_t  bm   = hd->setup[0];
    uint8_t  req  = hd->setup[1];
    uint16_t wval = hd->setup[2] | (hd->setup[3] << 8);
    uint16_t widx = hd->setup[4] | (hd->setup[5] << 8);

    switch (REQ(bm, req)) {
        case REQ(0x00, LIBUSB_REQUEST_SET_ADDRESS):
            hd->dev.addr = wval & 0x7f;
            usbn_trace("  SET_ADDRESS %d (done here)", wval & 0x7f);
            return 1;
        case REQ(0x00, LIBUSB_REQUEST_SET_CONFIGURATION): {
            uint32_t t0  = usbn_ms();
            int      cur = -1;

            /* The host configured the device when it enumerated it, nearly
               always as configuration 1.  Asking again is not free -- it can
               make the host re-configure or re-enumerate the device -- so it
               is only done when the guest wants a different one. */
            libusb_get_configuration(hd->h, &cur);
            host_reset_endpoints(hd);
            if (cur != (wval & 0xff)) {
                host_release_all(hd);
                libusb_set_configuration(hd->h, wval & 0xff);   /* may be refused on Windows; harmless */
                host_claim_all(hd);
            }
            usbn_trace("  SET_CONFIGURATION %d (was %d): %s, %u ms", wval & 0xff, cur,
                       (cur != (wval & 0xff)) ? "set on the device" : "already so, nothing to do", usbn_ms() - t0);
            return 1;
        }
        case REQ(0x01, LIBUSB_REQUEST_SET_INTERFACE): {
            uint32_t t0 = usbn_ms();
            host_reset_endpoints(hd);
            host_set_alt(hd, widx, wval);
            usbn_trace("  SET_INTERFACE %d alt %d: %u ms", widx, wval, usbn_ms() - t0);
            return 1;
        }
        case REQ(0x02, LIBUSB_REQUEST_CLEAR_FEATURE):
            if (wval == 0) {   /* ENDPOINT_HALT */
                uint32_t t0 = usbn_ms();
                host_reset_endpoint(hd, widx & 0xff);
                libusb_clear_halt(hd->h, widx & 0xff);
                usbn_trace("  CLEAR_FEATURE(HALT) ep %02X: %u ms", widx & 0xff, usbn_ms() - t0);
                return 1;
            }
            return 0;
        default:
            return 0;
    }
}

static int
host_control(host_dev_t *hd, uint8_t pid, uint8_t *buf, int len)
{
    xfer_t *x = &hd->ctl;
    int     r;

    if (pid == USB_PID_SETUP) {
        if (len != 8)
            return USBN_STALL;
        host_cancel(x);   /* a new request replaces one still going */
        memcpy(hd->setup, buf, 8);
        usbn_trace("%s: SETUP %02X %02X wValue %04X wIndex %04X wLength %d", hd->dev.name, buf[0], buf[1],
                   buf[2] | (buf[3] << 8), buf[4] | (buf[5] << 8), buf[6] | (buf[7] << 8));
        hd->ctl_in     = buf[0] & 0x80;
        hd->ctl_len    = buf[6] | (buf[7] << 8);
        hd->ctl_pos     = 0;
        hd->ctl_active  = 1;
        hd->ctl_patched = 0;
        /* A SETUP is always accepted.  A read longer than the buffer (Linux
           asks some descriptors for up to 64 KB) is asked of the device as a
           read of the buffer's size, which no descriptor exceeds; a longer
           write fails at its data stage instead. */
        if (hd->ctl_in && (hd->ctl_len > CTL_MAX)) {
            hd->ctl_len  = CTL_MAX;
            hd->setup[6] = CTL_MAX & 0xff;
            hd->setup[7] = CTL_MAX >> 8;
        }
        return 8;
    }
    if (!hd->ctl_active)
        return USBN_STALL;

    if (hd->ctl_in) {
        if (pid == USB_PID_OUT) {        /* status stage of an IN request */
            hd->ctl_active = 0;
            host_cancel(x);              /* a read the guest stopped waiting for */
            return 0;
        }
        /* data stage: fetch the whole answer once, hand it out per packet */
        if (XSTATE(x) == XFER_IDLE) {
            if (!host_submit_control(hd))
                return USBN_STALL;
            return USBN_NAK;
        }
        if (XSTATE(x) == XFER_PENDING)
            return USBN_NAK;
        if ((r = host_status_to_ret(x->status)) < 0) {
            hd->ctl_active = 0;
            return r;
        }
        /* GET_DESCRIPTOR(CONFIGURATION) for a high-speed device on a full-
           speed port: the guest gets the full-speed version. */
        if (hd->dev.fs_view && !hd->ctl_patched && (hd->setup[0] == 0x80) && (hd->setup[1] == LIBUSB_REQUEST_GET_DESCRIPTOR)
            && (hd->setup[3] == LIBUSB_DT_CONFIG)) {
            usbn_config_to_full_speed(x->buf + 8, x->actual);
            hd->ctl_patched = 1;
        }
        r = x->actual - hd->ctl_pos;
        if (r > len)
            r = len;
        if (r < 0)
            r = 0;
        memcpy(buf, x->buf + 8 + hd->ctl_pos, r);
        hd->ctl_pos += r;
        return r;
    }

    if (pid == USB_PID_OUT) {            /* data stage of an OUT request */
        if (hd->ctl_pos + len > CTL_MAX)
            return USBN_STALL;
        memcpy(hd->ctl_out + hd->ctl_pos, buf, len);
        hd->ctl_pos += len;
        return len;
    }

    /* status stage (IN, zero length) of an OUT or no-data request: now the
       request goes to the device, or is done here. */
    if (XSTATE(x) == XFER_IDLE) {
        if (host_local_request(hd)) {
            hd->ctl_active = 0;
            return 0;
        }
        if (!host_submit_control(hd))
            return USBN_STALL;
        return USBN_NAK;
    }
    if (XSTATE(x) == XFER_PENDING)
        return USBN_NAK;
    hd->ctl_active = 0;
    x->state       = XFER_IDLE;
    r              = host_status_to_ret(x->status);
    return (r < 0) ? r : 0;
}

/* ----------------------------------------- high speed at full speed --- */

/* Send what has been gathered on an OUT endpoint. */
static int
host_out_flush(host_dev_t *hd, uint8_t ep)
{
    const int i = ep & 0x0f;
    const int n = hd->out_acc[i];

    if ((n == 0) || (XSTATE(&hd->out[i]) == XFER_PENDING))
        return 1;
    hd->out_acc[i] = 0;
    hd->out[i].state = XFER_IDLE;
    return host_submit(hd, &hd->out[i], ep, n);
}

/* Gathered bytes that are not a whole high-speed packet wait for the end of
   the guest's transfer, which a full-speed packet stream does not mark when
   it ends on a 64-byte boundary.  So they go when the guest turns to another
   endpoint (a disk's status read, say) -- host_packet() -- or when no more
   arrive for a few frames.  A transfer that spans frames keeps arriving every
   frame, so it is not cut short. */
static void
host_frame(usbn_device_t *dev)
{
    host_dev_t *hd = (host_dev_t *) dev->priv;

    if (!dev->fs_view)
        return;
    for (int e = 1; e < 16; e++) {
        if (hd->out_acc[e] && (++hd->out_idle[e] >= 3))
            host_out_flush(hd, e);
    }
}

/* The guest sends full-speed packets; the device takes high-speed ones.
   Gather them, and send when the guest's transfer ends (a short packet) or a
   whole high-speed packet is ready -- so the device sees the same byte
   stream with the end of every transfer in the same place.  Each guest
   packet is acknowledged at once; a send still in flight NAKs the next one,
   and a failed send is reported on it. */
static int
host_out_fs(host_dev_t *hd, uint8_t ep, uint8_t *buf, int len)
{
    xfer_t   *x    = &hd->out[ep];
    const int i    = ep & 0x0f;
    const int hs   = hd->ep_maxp[i] ? hd->ep_maxp[i] : 512;
    const int fs   = usbn_fs_maxp(hd->ep_type[i], hs);
    int       r;

    if (XSTATE(x) == XFER_PENDING)
        return USBN_NAK;
    if (XSTATE(x) == XFER_DONE) {
        x->state = XFER_IDLE;
        if ((r = host_status_to_ret(x->status)) < 0) {
            hd->out_acc[i] = 0;
            return r;
        }
    }
    if (hd->out_acc[i] + len > DATA_MAX)
        len = DATA_MAX - hd->out_acc[i];
    memcpy(x->buf + hd->out_acc[i], buf, len);
    hd->out_acc[i] += len;
    hd->out_idle[i] = 0;

    if ((len < fs) || ((hd->out_acc[i] % hs) == 0) || (hd->out_acc[i] + fs > DATA_MAX)) {
        const int n    = hd->out_acc[i];
        hd->out_acc[i] = 0;
        if (!host_submit(hd, x, ep, n))
            return USBN_STALL;
    }
    return len;
}

/* The device sends high-speed packets; the guest takes full-speed ones.  Read
   one high-speed packet at a time and hand it out in full-speed pieces; when
   it was short but happens to end on a full-speed boundary, the guest gets a
   zero-length packet after it, so its transfer ends where the device's did. */
static int
host_in_fs(host_dev_t *hd, uint8_t ep, uint8_t *buf, int len)
{
    xfer_t   *x  = &hd->in[ep];
    const int i  = ep & 0x0f;
    const int hs = hd->ep_maxp[i | 16] ? hd->ep_maxp[i | 16] : 512;
    const int fs = usbn_fs_maxp(hd->ep_type[i | 16], hs);
    int       r;

    if (hd->in_pos[i] < hd->in_len[i]) {
        int n = hd->in_len[i] - hd->in_pos[i];
        if (n > len)
            n = len;
        memcpy(buf, x->buf + hd->in_pos[i], n);
        hd->in_pos[i] += n;
        return n;
    }
    if (hd->in_zlp[i]) {
        hd->in_zlp[i] = 0;
        return 0;
    }

    if (XSTATE(x) == XFER_PENDING)
        return USBN_NAK;
    if (XSTATE(x) == XFER_DONE) {
        x->state = XFER_IDLE;
        if ((r = host_status_to_ret(x->status)) < 0)
            return r;
        hd->in_len[i] = x->actual;
        hd->in_pos[i] = 0;
        hd->in_zlp[i] = (x->actual > 0) && (x->actual < hs) && ((x->actual % fs) == 0);
        if (x->actual == 0)
            return 0;
        return host_in_fs(hd, ep, buf, len);
    }
    if (!host_submit(hd, x, ep | 0x80, hs))
        return USBN_STALL;
    return USBN_NAK;
}

/* -------------------------------------------------------- the device --- */

static int
host_packet(usbn_device_t *dev, uint8_t pid, uint8_t ep, uint8_t *buf, int len)
{
    host_dev_t *hd = (host_dev_t *) dev->priv;
    xfer_t     *x;
    int         r;

    if (ep == 0)
        return host_control(hd, pid, buf, len);
    if (pid == USB_PID_SETUP)
        return USBN_STALL;

    if (dev->fs_view) {
        /* The guest has turned elsewhere: what another OUT endpoint gathered
           was the whole of its transfer. */
        for (int e = 1; e < 16; e++)
            if (hd->out_acc[e] && ((e != ep) || (pid != USB_PID_OUT)))
                host_out_flush(hd, e);

        const int type = hd->ep_type[(ep & 0x0f) | ((pid == USB_PID_IN) ? 16 : 0)];
        if ((type == LIBUSB_TRANSFER_TYPE_BULK) || (type == LIBUSB_TRANSFER_TYPE_INTERRUPT))
            return (pid == USB_PID_IN) ? host_in_fs(hd, ep, buf, len) : host_out_fs(hd, ep, buf, len);
    }

    x = (pid == USB_PID_IN) ? &hd->in[ep] : &hd->out[ep];

    if (XSTATE(x) == XFER_DONE) {
        x->state = XFER_IDLE;
        if ((r = host_status_to_ret(x->status)) < 0)
            return r;
        if (pid == USB_PID_OUT)
            return len;
        if (x->actual > len)
            return USBN_BABBLE;
        memcpy(buf, x->buf, x->actual);
        return x->actual;
    }
    if (XSTATE(x) == XFER_PENDING)
        return USBN_NAK;

    if (len > DATA_MAX)
        len = DATA_MAX;
    if (pid == USB_PID_OUT)
        memcpy(x->buf, buf, len);
    if (!host_submit(hd, x, (pid == USB_PID_IN) ? (ep | 0x80) : ep, len))
        return USBN_STALL;
    return USBN_NAK;
}

static void
host_reset(usbn_device_t *dev)
{
    host_dev_t *hd = (host_dev_t *) dev->priv;

    usbn_trace("%s: port reset", dev->name);
    host_cancel_all(hd);
}

static void
host_destroy(usbn_device_t *dev)
{
    host_dev_t *hd = (host_dev_t *) dev->priv;

    host_cancel_all(hd);
    host_wait_inflight(hd);  /* the handle must outlive its transfers */
    host_release_all(hd);    /* the host's drivers get the device back */
    libusb_close(hd->h);
    pclog("USB: %04X:%04X given back to the host\n", hd->vid, hd->pid);
    free(hd);
    host_ctx_put();
}

usbn_device_t *
usbn_host_open(uint16_t vid, uint16_t pid, char *err, int errlen)
{
    libusb_device **list;
    libusb_device  *found = NULL;
    ssize_t         n;
    host_dev_t     *hd;
    int             r;

    if (!host_ctx_init()) {
        snprintf(err, errlen, "libusb could not be initialised");
        return NULL;
    }
    n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        if ((libusb_get_device_descriptor(list[i], &dd) == 0) && (dd.idVendor == vid) && (dd.idProduct == pid)) {
            found = libusb_ref_device(list[i]);
            break;
        }
    }
    libusb_free_device_list(list, 1);
    if (!found) {
        snprintf(err, errlen, "the device is not plugged in");
        return NULL;
    }

    int speed = map_speed(libusb_get_device_speed(found));

    hd = calloc(1, sizeof(host_dev_t));
    r  = libusb_open(found, &hd->h);
    libusb_unref_device(found);
    if (r != 0) {
#ifdef _WIN32
        if (use_usbdk)
            snprintf(err, errlen, "UsbDk could not capture it (%s)", libusb_error_name(r));
        else
            snprintf(err, errlen, "it could not be opened (%s). Install UsbDk (github.com/daynix/UsbDk) once to pass any device through, "
                                  "or give this device the WinUSB driver with Zadig (zadig.akeo.ie)", libusb_error_name(r));
#else
        snprintf(err, errlen, "it could not be opened (%s); check the permissions on its device node", libusb_error_name(r));
#endif
        free(hd);
        return NULL;
    }

    libusb_set_auto_detach_kernel_driver(hd->h, 1);
    hd->vid         = vid;
    hd->pid         = pid;
    hd->dev.speed   = speed;
    hd->dev.priv    = hd;
    hd->dev.packet  = host_packet;
    hd->dev.reset   = host_reset;
    hd->dev.frame   = host_frame;
    hd->dev.destroy = host_destroy;
    {
        libusb_device                  *ud = libusb_get_device(hd->h);
        struct libusb_device_descriptor dd;
        uint8_t                         path[8];
        int                             depth = libusb_get_port_numbers(ud, path, sizeof(path));

        libusb_get_device_descriptor(ud, &dd);
        snprintf(hd->dev.name, sizeof(hd->dev.name), "%s",
                 cached_name(ud, &dd, libusb_get_bus_number(ud), path, (depth > 0) ? depth : 0));
    }
    host_claim_all(hd);
    host_ctx_get();
    usbn_trace("%s (%04X:%04X): opened%s, %s speed, %d interface(s) claimed", hd->dev.name, vid, pid,
               use_usbdk ? " through UsbDk" : "", (speed == USBN_SPEED_HIGH) ? "high" : ((speed == USBN_SPEED_LOW) ? "low" : "full"),
               hd->nclaimed);
    host_log("USB: %04X:%04X opened, %d interface(s) claimed\n", vid, pid, hd->nclaimed);
    return &hd->dev;
}

#endif /* USE_LIBUSB */
