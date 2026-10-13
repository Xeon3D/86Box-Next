/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          86Box Guest Tools, the emulator side: a two-port channel to an
 *          agent in the guest (see guest_tools_proto.h) for pointer
 *          integration -- the agent puts the guest pointer where the host
 *          pointer is, so the mouse needs no capture -- and clipboard text
 *          both ways.
 *
 * Authors: Marcos Alves, <xeon4d@gmail.com>
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/io.h>
#include <86box/device.h>
#include <86box/mouse.h>
#include <86box/plat.h>
#include <86box/plat_unused.h>
#include <86box/guest_tools.h>

#define GT_REQ_MAX (GT_CLIP_MAX_BYTES + 64)

typedef struct guest_tools_t {
    uint8_t *req;
    uint32_t req_len;
    uint32_t req_size;
    int      req_overflow;

    uint8_t *reply;
    uint32_t reply_len;
    uint32_t reply_size;
    uint32_t reply_pos;

    int      hello;
    uint16_t agent_os;
    uint32_t agent_caps;
    uint32_t clip_seq_delivered;

    uint32_t features;
} guest_tools_t;

int guest_tools_ports_open = 0;

/* Shared with the UI thread. */
static atomic_int  gt_features;      /* GT_FEAT_* of the device in, 0 without one */
static atomic_int  gt_pointer_ready; /* the agent follows the host pointer */
static atomic_uint gt_last_poll;     /* plat_get_ticks() of the agent's last POLL */
static atomic_uint gt_pointer;       /* host pointer: x << 16 | y, 0..GT_POINTER_MAX */
static atomic_int  gt_pointer_inside;
static atomic_uint gt_clip_seq;      /* bumped on every host clipboard change */

static void (*gt_set_host_clipboard)(const uint16_t *text, size_t chars);
static uint16_t *(*gt_get_host_clipboard)(size_t *chars);

/* debug_cmd's stand-in host clipboard (emulation thread only). */
static uint16_t *gt_debug_clip;
static size_t    gt_debug_clip_chars;
static int       gt_debug;

#ifdef ENABLE_GUEST_TOOLS_LOG
int guest_tools_do_log = ENABLE_GUEST_TOOLS_LOG;

static void
guest_tools_log(const char *fmt, ...)
{
    va_list ap;

    if (guest_tools_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define guest_tools_log(fmt, ...)
#endif

/* ---- UI side ---------------------------------------------------------- */

int
guest_tools_pointer_active(void)
{
    if (!(atomic_load(&gt_features) & GT_FEAT_POINTER) || !atomic_load(&gt_pointer_ready))
        return 0;
    if (mouse_input_mode != 0)
        return 0;

    return (uint32_t) (plat_get_ticks() - atomic_load(&gt_last_poll)) < GT_AGENT_TIMEOUT_MS;
}

int
guest_tools_keyboard(void)
{
    return atomic_load(&gt_pointer_inside) && guest_tools_pointer_active();
}

void
guest_tools_host_pointer(double x, double y, int inside)
{
    uint32_t ix;
    uint32_t iy;

    if (inside) {
        x  = (x < 0.0) ? 0.0 : ((x > 1.0) ? 1.0 : x);
        y  = (y < 0.0) ? 0.0 : ((y > 1.0) ? 1.0 : y);
        ix = (uint32_t) (x * GT_POINTER_MAX + 0.5);
        iy = (uint32_t) (y * GT_POINTER_MAX + 0.5);
        atomic_store(&gt_pointer, (ix << 16) | iy);
    }
    atomic_store(&gt_pointer_inside, !!inside);
}

void
guest_tools_host_clipboard_changed(void)
{
    atomic_fetch_add(&gt_clip_seq, 1);
}

void
guest_tools_set_clipboard_handlers(void (*set_host)(const uint16_t *text, size_t chars),
                                   uint16_t *(*get_host)(size_t *chars))
{
    gt_set_host_clipboard = set_host;
    gt_get_host_clipboard = get_host;
}

/* debug_cmd "gt ...": drive the device without the host's pointer or
   clipboard. Once used, the host clipboard is left alone until restart. */
void
guest_tools_debug(const char *args)
{
    double x;
    double y;

    if (sscanf(args, "pointer %lf %lf", &x, &y) == 2) {
        guest_tools_host_pointer(x, y, x >= 0.0);
        always_log("Guest Tools: host pointer %.3f, %.3f\n", x, y);
    } else if (!strncmp(args, "clip ", 5)) {
        const char *text = args + 5;
        size_t      len  = strlen(text);

        free(gt_debug_clip);
        gt_debug_clip = malloc((len + 1) * sizeof(uint16_t));
        if (gt_debug_clip != NULL) {
            for (size_t i = 0; i < len; i++)
                gt_debug_clip[i] = (text[i] == '|') ? '\n' : (uint8_t) text[i];
        }
        gt_debug_clip_chars = gt_debug_clip ? len : 0;
        gt_debug            = 1;
        guest_tools_host_clipboard_changed();
        always_log("Guest Tools: host clipboard \"%s\"\n", text);
    } else if (sscanf(args, "buttons %lf", &x) == 1) {
        /* As the UI does while integrated: buttons, no motion. */
        mouse_set_buttons_ex((int) x);
        always_log("Guest Tools: host buttons %d\n", (int) x);
    } else if (!strcmp(args, "state")) {
        uint32_t pos = atomic_load(&gt_pointer);

        always_log("Guest Tools: features %X, agent ready %d, last poll %u ms ago, pointer active %d, "
                   "host pointer %u,%u inside %d, clip seq %u\n",
                   atomic_load(&gt_features), atomic_load(&gt_pointer_ready),
                   plat_get_ticks() - atomic_load(&gt_last_poll), guest_tools_pointer_active(),
                   pos >> 16, pos & 0xffff, atomic_load(&gt_pointer_inside), atomic_load(&gt_clip_seq));
    } else
        always_log("Guest Tools: gt pointer x y | gt buttons mask | gt clip text | gt state\n");
}

/* ---- Request and reply buffers ---------------------------------------- */

static void
gt_req_put(guest_tools_t *dev, uint8_t val)
{
    if (dev->req_len >= GT_REQ_MAX) {
        dev->req_overflow = 1;
        return;
    }
    if (dev->req_len >= dev->req_size) {
        uint32_t size = dev->req_size ? (dev->req_size * 2) : 256;
        uint8_t *req;

        if (size > GT_REQ_MAX)
            size = GT_REQ_MAX;
        req = realloc(dev->req, size);
        if (req == NULL) {
            dev->req_overflow = 1;
            return;
        }
        dev->req      = req;
        dev->req_size = size;
    }
    dev->req[dev->req_len++] = val;
}

static int
gt_reply_reserve(guest_tools_t *dev, uint32_t len)
{
    if ((dev->reply_len + len) > dev->reply_size) {
        uint32_t size = dev->reply_size ? dev->reply_size : 256;
        uint8_t *reply;

        while (size < (dev->reply_len + len))
            size *= 2;
        reply = realloc(dev->reply, size);
        if (reply == NULL)
            return 0;
        dev->reply      = reply;
        dev->reply_size = size;
    }
    return 1;
}

static void
gt_reply_u16(guest_tools_t *dev, uint16_t val)
{
    if (!gt_reply_reserve(dev, 2))
        return;
    dev->reply[dev->reply_len++] = val & 0xff;
    dev->reply[dev->reply_len++] = val >> 8;
}

static void
gt_reply_u32(guest_tools_t *dev, uint32_t val)
{
    gt_reply_u16(dev, val & 0xffff);
    gt_reply_u16(dev, val >> 16);
}

static uint16_t
gt_get_u16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

static uint32_t
gt_get_u32(const uint8_t *p)
{
    return gt_get_u16(p) | ((uint32_t) gt_get_u16(p + 2) << 16);
}

/* ---- Commands --------------------------------------------------------- */

static uint32_t
gt_cmd_hello(guest_tools_t *dev)
{
    if (dev->req_len < 8)
        return GT_ERR_REQUEST;

    dev->agent_os   = gt_get_u16(dev->req + 2);
    dev->agent_caps = gt_get_u32(dev->req + 4);
    dev->hello      = 1;
    /* Hand the agent the host clipboard as it is now. */
    dev->clip_seq_delivered = atomic_load(&gt_clip_seq) - 1;
    atomic_store(&gt_pointer_ready, 0);
    atomic_store(&gt_last_poll, plat_get_ticks());

    guest_tools_log("Guest Tools: agent v%u, OS %u, caps %08X\n",
                    gt_get_u16(dev->req), dev->agent_os, dev->agent_caps);

    gt_reply_u32(dev, GT_OK);
    gt_reply_u16(dev, GT_PROTO_VERSION);
    gt_reply_u16(dev, 0);
    gt_reply_u32(dev, dev->features);
    return GT_OK;
}

static uint32_t
gt_cmd_poll(guest_tools_t *dev)
{
    uint32_t flags = 0;
    uint32_t pos   = atomic_load(&gt_pointer);
    uint32_t seq   = atomic_load(&gt_clip_seq);
    uint32_t state;

    if (dev->req_len < 4)
        return GT_ERR_REQUEST;
    if (!dev->hello)
        return GT_ERR_NO_AGENT;

    state = gt_get_u32(dev->req);
    atomic_store(&gt_pointer_ready, (dev->features & GT_FEAT_POINTER) && (state & GT_STATE_POINTER));
    atomic_store(&gt_last_poll, plat_get_ticks());

    if ((dev->features & GT_FEAT_POINTER) && (mouse_input_mode == 0)) {
        flags |= GT_POLL_POINTER;
        if (atomic_load(&gt_pointer_inside))
            flags |= GT_POLL_INSIDE;
    }
    if ((dev->features & GT_FEAT_CLIP_IN) && (seq != dev->clip_seq_delivered))
        flags |= GT_POLL_CLIP;

    gt_reply_u32(dev, GT_OK);
    gt_reply_u32(dev, flags);
    gt_reply_u16(dev, pos >> 16);
    gt_reply_u16(dev, pos & 0xffff);
    gt_reply_u32(dev, seq);
    return GT_OK;
}

static uint32_t
gt_cmd_clip_get(guest_tools_t *dev)
{
    uint32_t  seq   = atomic_load(&gt_clip_seq);
    uint16_t *text  = NULL;
    size_t    chars = 0;
    uint32_t  bytes;

    if (!dev->hello)
        return GT_ERR_NO_AGENT;
    if (!(dev->features & GT_FEAT_CLIP_IN))
        return GT_ERR_DISABLED;

    if (gt_debug) {
        if (gt_debug_clip != NULL) {
            text = malloc((gt_debug_clip_chars + 1) * sizeof(uint16_t));
            if (text != NULL) {
                memcpy(text, gt_debug_clip, gt_debug_clip_chars * sizeof(uint16_t));
                chars = gt_debug_clip_chars;
            }
        }
    } else if (gt_get_host_clipboard != NULL)
        text = gt_get_host_clipboard(&chars);
    if (chars > (GT_CLIP_MAX_BYTES / 2))
        chars = GT_CLIP_MAX_BYTES / 2;
    bytes = (uint32_t) chars * 2;
    dev->clip_seq_delivered = seq;

    gt_reply_u32(dev, GT_OK);
    gt_reply_u32(dev, (text != NULL) ? GT_CLIP_TEXT : GT_CLIP_NONE);
    gt_reply_u32(dev, (text != NULL) ? bytes : 0);
    if ((text != NULL) && gt_reply_reserve(dev, bytes)) {
        for (size_t i = 0; i < chars; i++)
            gt_reply_u16(dev, text[i]);
    }
    free(text);

    guest_tools_log("Guest Tools: host clipboard to guest, %u chars\n", (unsigned) chars);
    return GT_OK;
}

static uint32_t
gt_cmd_clip_set(guest_tools_t *dev)
{
    uint32_t  format;
    uint32_t  bytes;
    uint16_t *text;
    size_t    chars;

    if (!dev->hello)
        return GT_ERR_NO_AGENT;
    if (!(dev->features & GT_FEAT_CLIP_OUT))
        return GT_ERR_DISABLED;
    if (dev->req_len < 8)
        return GT_ERR_REQUEST;

    format = gt_get_u32(dev->req);
    bytes  = gt_get_u32(dev->req + 4);
    if ((format != GT_CLIP_TEXT) || (bytes & 1) || (bytes > (dev->req_len - 8)))
        return GT_ERR_REQUEST;

    chars = bytes / 2;
    text  = malloc((chars + 1) * sizeof(uint16_t));
    if (text == NULL)
        return GT_ERR_REQUEST;
    for (size_t i = 0; i < chars; i++)
        text[i] = gt_get_u16(dev->req + 8 + (i * 2));
    text[chars] = 0;

    if (gt_debug) {
        char  *ascii = malloc(chars + 1);

        if (ascii != NULL) {
            for (size_t i = 0; i < chars; i++)
                ascii[i] = ((text[i] >= 0x20) && (text[i] < 0x7f)) ? (char) text[i] : ((text[i] == '\n') ? '|' : '?');
            ascii[chars] = '\0';
            always_log("Guest Tools: guest clipboard (%u chars): %s\n", (unsigned) chars, ascii);
            free(ascii);
        }
    } else if (gt_set_host_clipboard != NULL)
        gt_set_host_clipboard(text, chars);
    free(text);

    guest_tools_log("Guest Tools: guest clipboard to host, %u chars\n", (unsigned) chars);
    gt_reply_u32(dev, GT_OK);
    return GT_OK;
}

static void
gt_command(guest_tools_t *dev, uint8_t cmd)
{
    uint32_t status;

    dev->reply_len = 0;
    dev->reply_pos = 0;

    if (dev->req_overflow)
        status = GT_ERR_REQUEST;
    else switch (cmd) {
        case GT_CMD_HELLO:
            status = gt_cmd_hello(dev);
            break;
        case GT_CMD_POLL:
            status = gt_cmd_poll(dev);
            break;
        case GT_CMD_CLIP_GET:
            status = gt_cmd_clip_get(dev);
            break;
        case GT_CMD_CLIP_SET:
            status = gt_cmd_clip_set(dev);
            break;
        case GT_CMD_BYE:
            dev->hello = 0;
            atomic_store(&gt_pointer_ready, 0);
            gt_reply_u32(dev, GT_OK);
            status = GT_OK;
            break;
        default:
            status = GT_ERR_COMMAND;
            break;
    }

    /* A failed command answers with its status alone. */
    if (status != GT_OK) {
        dev->reply_len = 0;
        gt_reply_u32(dev, status);
    }

    dev->req_len      = 0;
    dev->req_overflow = 0;
}

/* ---- I/O -------------------------------------------------------------- */

static uint8_t
gt_reply_get(guest_tools_t *dev)
{
    if (dev->reply_pos >= dev->reply_len)
        return 0x00;
    return dev->reply[dev->reply_pos++];
}

static uint8_t
gt_inb(uint16_t port, void *priv)
{
    guest_tools_t *dev = (guest_tools_t *) priv;

    if (port < GT_PORT_DATA)
        return (GT_MAGIC >> ((port - GT_PORT_CMD) * 8)) & 0xff;
    if (port == GT_PORT_DATA)
        return gt_reply_get(dev);
    return 0xff;
}

static uint16_t
gt_inw(uint16_t port, void *priv)
{
    guest_tools_t *dev = (guest_tools_t *) priv;
    uint16_t       val;

    if (port == GT_PORT_DATA) {
        val = gt_reply_get(dev);
        return val | (gt_reply_get(dev) << 8);
    }
    return gt_inb(port, priv) | (gt_inb(port + 1, priv) << 8);
}

static uint32_t
gt_inl(uint16_t port, void *priv)
{
    if (port == GT_PORT_DATA)
        return gt_inw(port, priv) | ((uint32_t) gt_inw(port, priv) << 16);
    if (port == GT_PORT_CMD)
        return GT_MAGIC;
    return 0xffffffff;
}

static void
gt_outb(uint16_t port, uint8_t val, void *priv)
{
    guest_tools_t *dev = (guest_tools_t *) priv;

    if (port == GT_PORT_CMD)
        gt_command(dev, val);
    else if (port == GT_PORT_DATA)
        gt_req_put(dev, val);
}

static void
gt_outw(uint16_t port, uint16_t val, void *priv)
{
    guest_tools_t *dev = (guest_tools_t *) priv;

    if (port == GT_PORT_CMD)
        gt_command(dev, val & 0xff);
    else if (port == GT_PORT_DATA) {
        gt_req_put(dev, val & 0xff);
        gt_req_put(dev, val >> 8);
    }
}

static void
gt_outl(uint16_t port, uint32_t val, void *priv)
{
    if (port == GT_PORT_CMD)
        gt_outb(port, val & 0xff, priv);
    else if (port == GT_PORT_DATA) {
        gt_outw(port, val & 0xffff, priv);
        gt_outw(port, val >> 16, priv);
    }
}

/* ---- Device ----------------------------------------------------------- */

static void
guest_tools_reset(void *priv)
{
    guest_tools_t *dev = (guest_tools_t *) priv;

    dev->hello        = 0;
    dev->req_len      = 0;
    dev->req_overflow = 0;
    dev->reply_len    = 0;
    dev->reply_pos    = 0;
    atomic_store(&gt_pointer_ready, 0);
}

static void *
guest_tools_init(UNUSED(const device_t *info))
{
    guest_tools_t *dev = calloc(1, sizeof(guest_tools_t));
    int            clip = device_get_config_int("clipboard");

    if (device_get_config_int("pointer"))
        dev->features |= GT_FEAT_POINTER;
    if (clip & 1)
        dev->features |= GT_FEAT_CLIP_IN;
    if (clip & 2)
        dev->features |= GT_FEAT_CLIP_OUT;

    io_sethandler(GT_PORT_BASE, GT_PORT_COUNT,
                  gt_inb, gt_inw, gt_inl, gt_outb, gt_outw, gt_outl, dev);

    atomic_store(&gt_pointer_ready, 0);
    atomic_store(&gt_features, (int) dev->features);
    guest_tools_ports_open = 1;

    return dev;
}

static void
guest_tools_close(void *priv)
{
    guest_tools_t *dev = (guest_tools_t *) priv;

    guest_tools_ports_open = 0;
    atomic_store(&gt_features, 0);
    atomic_store(&gt_pointer_ready, 0);

    io_removehandler(GT_PORT_BASE, GT_PORT_COUNT,
                     gt_inb, gt_inw, gt_inl, gt_outb, gt_outw, gt_outl, dev);

    free(dev->req);
    free(dev->reply);
    free(dev);
}

static const device_config_t guest_tools_config[] = {
  // clang-format off
    {
        .name           = "pointer",
        .description    = "Mouse integration (no capture)",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 1,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "clipboard",
        .description    = "Shared clipboard",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 3,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "Disabled",        .value = 0 },
            { .description = "Host to guest",   .value = 1 },
            { .description = "Guest to host",   .value = 2 },
            { .description = "Bidirectional",   .value = 3 },
            { .description = ""                            }
        },
        .bios           = { { 0 } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
  // clang-format on
};

const device_t guest_tools_device = {
    .name          = "86Box Guest Tools",
    .internal_name = "guest_tools",
    .flags         = 0,
    .local         = 0,
    .init          = guest_tools_init,
    .close         = guest_tools_close,
    .reset         = guest_tools_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = guest_tools_config
};
