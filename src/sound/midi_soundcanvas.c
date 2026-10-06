/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Roland Sound Canvas MIDI output: the boards 88emu emulates (the
 *          Sound Canvas family, the MT-32 and the CM modules), running their
 *          own firmware from ROM images in roms/soundcanvas. The board is
 *          picked in its configuration window (qt_soundcanvas.cpp), and its
 *          front panel opens as a window of its own while the machine runs.
 *          The emulation is gearmulator's 88lib, see emu88/.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <86box/86box.h>
#include <86box/device.h>
#include <86box/midi.h>
#include <86box/mem.h>
#include <86box/path.h>
#include <86box/plat.h>
#include <86box/rom.h>
#include <86box/sound.h>
#include <86box/thread.h>
#include <86box/ui.h>
#include <86box/plat_unused.h>

#include "emu88/emu88_host.h"

#define RENDER_RATE     100 /* midi_poll() runs every 10 ms */
#define BUFFER_SEGMENTS 10
#define FRAMES          (SOUND_FREQ / RENDER_RATE)

extern void    al_set_midi(int freq, int buf_size);
extern uint8_t MIDI_evt_len[256];

typedef struct soundcanvas_t {
    emu88h_t *board;
    int       opts[3]; /* model, factory reset, fast boot */

    float   *buffer;       /* BUFFER_SEGMENTS segments of FRAMES stereo frames */
    int16_t *buffer_int16;
    int      buf_size;     /* bytes */

    thread_t *thread_h;
    event_t  *event;
    event_t  *start_event;
    volatile int on;
} soundcanvas_t;

static soundcanvas_t *scdev = NULL;

/* The board outlives the device: 86Box closes and re-creates its devices on every hard reset,
   but an external module keeps running through a reset of the PC. A closed device parks its
   board, and the next one takes it back when it asks for the same board; the panel window
   (which polls soundcanvas_get_board()) stays open meanwhile. A board parked for longer than a
   reset takes is let go: the MIDI device was changed or the machine stopped. */
#define PARK_TIMEOUT_MS 3000

static mutex_t  *board_mutex  = NULL;
static emu88h_t *board_shared = NULL; /* the running board, or the parked one */
static int       parked       = 0;
static uint32_t  parked_at;
static int       parked_opts[3];      /* model, factory reset, fast boot */

static void
board_lock(void)
{
    if (!board_mutex)
        board_mutex = thread_create_mutex();
    thread_wait_mutex(board_mutex);
}

void *
soundcanvas_get_board(void)
{
    emu88h_t *board = NULL;

    if (!board_mutex)
        return NULL;
    thread_wait_mutex(board_mutex);
    if (parked && ((plat_get_ticks() - parked_at) > PARK_TIMEOUT_MS)) {
        emu88h_release(board_shared);
        board_shared = NULL;
        parked       = 0;
    }
    board = board_shared;
    if (board)
        emu88h_retain(board);
    thread_release_mutex(board_mutex);
    return board;
}

/* A board for these options: the parked one when it matches, a new one otherwise. */
static emu88h_t *
take_board(int model, int factory_reset, int fast_boot)
{
    emu88h_t *board;

    board_lock();
    if (board_shared && parked && (parked_opts[0] == model) && (parked_opts[1] == factory_reset) && (parked_opts[2] == fast_boot))
        board = board_shared;
    else {
        if (board_shared)
            emu88h_release(board_shared);
        board        = emu88h_create(model, factory_reset, fast_boot);
        board_shared = board;
    }
    parked = 0;
    thread_release_mutex(board_mutex);
    return board;
}

static void
park_board(emu88h_t *board, int model, int factory_reset, int fast_boot)
{
    board_lock();
    if (board_shared == board) {
        parked         = 1;
        parked_at      = plat_get_ticks();
        parked_opts[0] = model;
        parked_opts[1] = factory_reset;
        parked_opts[2] = fast_boot;
    } else
        emu88h_release(board);
    thread_release_mutex(board_mutex);
}

/* The configured board. The CLAP-based device this replaced saved "model" as a number: 0 for
   auto-detect, 1-5 for SC-55 firmware revisions and 6 for the SC-55mkII. */
int
soundcanvas_config_model(const char *value)
{
    int model = emu88h_model_from_key(value);

    if ((model < 0) && value && (value[0] >= '0') && (value[0] <= '6') && !value[1])
        model = ((value[0] >= '1') && (value[0] <= '5')) ? EMU88H_SC55 : EMU88H_SC55MK2;
    return model;
}

/* roms/soundcanvas under every ROM path. */
void
soundcanvas_set_rom_dirs(void)
{
    char        paths[8][1024];
    const char *dirs[8];
    int         n = 0;

    for (rom_path_t *rp = &rom_paths; rp && (n < 8); rp = rp->next) {
        if (!rp->path[0])
            continue;
        path_append_filename(paths[n], rp->path, "soundcanvas");
        dirs[n] = paths[n];
        n++;
    }
    emu88h_set_rom_dirs(dirs, n);
}

static void
soundcanvas_poll(void)
{
    if (scdev)
        thread_set_event(scdev->event);
}

static void
soundcanvas_thread(void *param)
{
    soundcanvas_t *dev     = (soundcanvas_t *) param;
    const int      seg     = FRAMES * 2;  /* samples per segment */
    int            pos     = 0;           /* samples into the buffer */
    float          render[FRAMES * 2];

    thread_set_event(dev->start_event);

    while (dev->on) {
        thread_wait_event(dev->event, -1);
        thread_reset_event(dev->event);
        if (!dev->on)
            break;

        emu88h_render(dev->board, render, FRAMES, SOUND_FREQ);

        /* Apply sound card MIDI volume and filters */
        if (filter_midi != NULL) {
            for (int i = 0; i < seg; i += 2) {
                double dl = render[i];
                double dr = render[i + 1];
                filter_midi(0, &dl, filter_midi_p);
                filter_midi(1, &dr, filter_midi_p);
                render[i]     = (float) dl;
                render[i + 1] = (float) dr;
            }
        }

        if (sound_is_float)
            memcpy(dev->buffer + pos, render, sizeof(render));
        else
            for (int i = 0; i < seg; i++) {
                float s = render[i] * 32767.0f;
                if (s > 32767.0f)
                    s = 32767.0f;
                if (s < -32768.0f)
                    s = -32768.0f;
                dev->buffer_int16[pos + i] = (int16_t) s;
            }

        pos += seg;
        if (pos >= seg * BUFFER_SEGMENTS) {
            if (sound_is_float)
                givealbuffer_midi(dev->buffer, seg * BUFFER_SEGMENTS);
            else
                givealbuffer_midi(dev->buffer_int16, seg * BUFFER_SEGMENTS);
            pos = 0;
        }
    }
}

static void
soundcanvas_msg(uint8_t *msg)
{
    int len = MIDI_evt_len[msg[0]];

    if (scdev && len)
        emu88h_midi(scdev->board, 0, msg, len);
}

static void
soundcanvas_sysex(uint8_t *data, unsigned int len)
{
    if (scdev)
        emu88h_midi(scdev->board, 0, data, len);
}

static void *
soundcanvas_init(UNUSED(const device_t *info))
{
    soundcanvas_t *dev;
    midi_device_t *mdev;
    int            model = soundcanvas_config_model(device_get_config_string("model"));
    char           msg[512];

    soundcanvas_set_rom_dirs();
    if ((model < 0) || !emu88h_model_available(model)) {
        snprintf(msg, sizeof(msg),
                 "The ROM images of the %s are missing from roms/soundcanvas.\n\n"
                 "Open Settings > Sound > MIDI Out device > Configure to see which files it needs.",
                 (model >= 0) ? emu88h_model_name(model) : "selected synthesizer");
        ui_msgbox_header(MBX_ERROR, "Roland Sound Canvas", msg);
        return NULL;
    }

    dev          = calloc(1, sizeof(soundcanvas_t));
    dev->opts[0] = model;
    dev->opts[1] = device_get_config_int("factory_reset");
    dev->opts[2] = device_get_config_int("fast_boot");
    dev->board   = take_board(dev->opts[0], dev->opts[1], dev->opts[2]);
    emu88h_set_gain(dev->board, device_get_config_int("output_gain"));

    dev->buf_size = FRAMES * 2 * BUFFER_SEGMENTS * (sound_is_float ? sizeof(float) : sizeof(int16_t));
    if (sound_is_float)
        dev->buffer = calloc(1, dev->buf_size);
    else
        dev->buffer_int16 = calloc(1, dev->buf_size);
    al_set_midi(SOUND_FREQ, dev->buf_size);

    mdev             = calloc(1, sizeof(midi_device_t));
    mdev->play_msg   = soundcanvas_msg;
    mdev->play_sysex = soundcanvas_sysex;
    mdev->poll       = soundcanvas_poll;
    midi_out_init(mdev);

    scdev            = dev;
    dev->on          = 1;
    dev->start_event = thread_create_event();
    dev->event       = thread_create_event();
    dev->thread_h    = thread_create(soundcanvas_thread, dev);
    thread_wait_event(dev->start_event, -1);
    thread_reset_event(dev->start_event);

    return dev;
}

static void
soundcanvas_close(void *priv)
{
    soundcanvas_t *dev = (soundcanvas_t *) priv;

    if (!dev)
        return;

    dev->on = 0;
    thread_set_event(dev->event);
    thread_wait(dev->thread_h);
    thread_destroy_event(dev->event);
    thread_destroy_event(dev->start_event);
    scdev = NULL;

    park_board(dev->board, dev->opts[0], dev->opts[1], dev->opts[2]);
    free(dev->buffer);
    free(dev->buffer_int16);
    free(dev);
}

static const device_config_t soundcanvas_config[] = {
    // clang-format off
    {
        /* Set in the device's own configuration window, which lists the boards and their ROMs. */
        .name           = "model",
        .description    = "Synthesizer",
        .type           = CONFIG_STRING,
        .default_string = "sc55mk2",
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "factory_reset",
        .description    = "Factory reset at power-on",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 1,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "fast_boot",
        .description    = "Fast boot (skip intro)",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        /* The panel's VOLUME knob. */
        .name           = "output_gain",
        .description    = "Volume",
        .type           = CONFIG_SPINNER,
        .default_string = NULL,
        .default_int    = 100,
        .file_filter    = NULL,
        .spinner        = { .min = 0, .max = 200 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
    // clang-format on
};

const device_t soundcanvas_device = {
    .name          = "Roland Sound Canvas",
    .internal_name = "soundcanvas",
    .flags         = 0,
    .local         = 0,
    .init          = soundcanvas_init,
    .close         = soundcanvas_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = soundcanvas_config
};
