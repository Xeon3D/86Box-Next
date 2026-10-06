/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          88emu glue: the C interface the "Roland Sound Canvas" MIDI device
 *          (midi_soundcanvas.c) and its Qt windows use to reach gearmulator's
 *          88lib, which emulates the Sound Canvas family, the MT-32 and the CM
 *          boards by running their firmware.
 *
 *          A board is created powered off and boots on the audio thread, in
 *          the first emu88h_render() after a power-on: booting runs the
 *          firmware offline for a while, which must not stall the emulation
 *          or the UI. Every other call may come from any thread.
 */
#ifndef EMU88_HOST_H
#define EMU88_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The devices, by 88lib's stable DeviceModel values. */
enum {
    EMU88H_SC88      = 0,
    EMU88H_SC88VL    = 1,
    EMU88H_SC88PRO   = 2,
    EMU88H_SC8850    = 3,
    EMU88H_SC55MK2   = 4,
    EMU88H_SC55      = 5,
    EMU88H_SC55ST    = 6,
    EMU88H_CM300     = 7,
    EMU88H_SCB55     = 8,
    EMU88H_RLP3237   = 9,
    EMU88H_SC155     = 10,
    EMU88H_SC155MK2  = 11,
    EMU88H_XPGS      = 12,
    EMU88H_SC8820    = 13,
    EMU88H_CM32P     = 14,
    EMU88H_VEGSPRO   = 15,
    EMU88H_SCC1A     = 16,
    EMU88H_CM64      = 17,
    EMU88H_CM32L     = 18,
    EMU88H_NU10B     = 19,
    EMU88H_MIIG5     = 20,
    EMU88H_MT32_OLD  = 21,
    EMU88H_MT32_NEW  = 22,
    EMU88H_CM32LN    = 23
};

/* What a model's front panel is, for the panel window. */
enum {
    EMU88H_PANEL_SC88 = 0, /* the SC-88 / SC-88Pro layout; the SC-55 family uses part of it */
    EMU88H_PANEL_SC8850,
    EMU88H_PANEL_CM,      /* the CM bezel (CM-32L/LN, CM-32P, CM-64, the MT-32s), switches on the keyboard */
    EMU88H_PANEL_NONE     /* no switches on the artwork */
};

/* == Catalogue ======================================================= */

int         emu88h_model_count(void);
int         emu88h_model_at(int index);     /* in 88emu's menu order */
const char *emu88h_model_name(int model);   /* "SC-55mkII", NULL for an unknown model */
const char *emu88h_model_key(int model);    /* config key, 88emu's CLI id: "sc55mk2" */
int         emu88h_model_from_key(const char *key); /* -1 when unknown */
int         emu88h_model_panel(int model);  /* EMU88H_PANEL_* */
int         emu88h_model_has_lcd(int model);
int         emu88h_model_has_second_lcd(int model);
int         emu88h_model_power_standby(int model); /* POWER is a key the firmware reads */
int         emu88h_model_has_knob(int model);      /* the MT-32's VOLUME/VALUE knob */
/* The switch bits the model's panel really has, of those emu88h_set_buttons() takes. */
uint32_t    emu88h_model_button_mask(int model);
/* Which panel artwork the model wears: "sc55mk2" for sc55mk2_panel.png. */
const char *emu88h_model_artwork(int model);

/* == ROMs ============================================================ */

/* Replaces the ROM search folders (searched with their subfolders) and rescans them. */
void emu88h_set_rom_dirs(const char *const *dirs, int count);
void emu88h_rescan(void);
int  emu88h_model_available(int model);

enum {
    EMU88H_ROM_MISSING = 0,  /* required and not found */
    EMU88H_ROM_OK,           /* found, a known dump (MD5 and size) */
    EMU88H_ROM_UNVERIFIED,   /* found under its standard name and size, but not a known dump */
    EMU88H_ROM_DERIVED,      /* supplied by another image: a chip pair, a donor board's waves */
    EMU88H_ROM_ALTERNATIVE   /* one of a chip pair, not needed while the whole image is missing too */
};

typedef struct {
    char     label[64];  /* "Wave ROM 2", "CM-32P: Program ROM" */
    char     names[192]; /* standard filenames, " / " between them */
    char     file[260];  /* the file in use, without its folder; empty when none */
    char     md5[33];    /* that file's MD5, as it is on disk; empty when none */
    char     known[1024];/* the known dumps for this image, a line each: "<digest> <revision>" */
    uint32_t size;       /* bytes */
    int      status;     /* EMU88H_ROM_* */
} emu88h_rom_t;

/* The images the model needs; returns how many there are, filling at most max. */
int emu88h_model_roms(int model, emu88h_rom_t *out, int max);
/* Why a model whose every image was found is still unavailable (empty when it is not). */
size_t emu88h_model_note(int model, char *buf, size_t size);

/* == A board ========================================================= */

typedef struct emu88h emu88h_t;

/* Created powered on (it boots in the first render). The creator holds one reference. */
emu88h_t *emu88h_create(int model, int factory_reset, int fast_boot);
void      emu88h_retain(emu88h_t *h);
void      emu88h_release(emu88h_t *h);
int       emu88h_model(const emu88h_t *h);

/* Interleaved stereo float at out_rate; boots a board waiting for it first. */
void emu88h_render(emu88h_t *h, float *out, uint32_t frames, uint32_t out_rate);
/* Raw MIDI bytes for part group port (A = 0). Running status is honoured. */
void emu88h_midi(emu88h_t *h, unsigned port, const uint8_t *data, size_t len);

/* 0..200, 100 is unity. */
void emu88h_set_gain(emu88h_t *h, int percent);
int  emu88h_gain(emu88h_t *h);

/* The switches held, a bit per switch in the board's own numbering. */
void emu88h_set_buttons(emu88h_t *h, uint32_t buttons);
void emu88h_turn_encoder(emu88h_t *h, int detents);

enum {
    EMU88H_STATE_OFF = 0,
    EMU88H_STATE_BOOTING,
    EMU88H_STATE_ON,
    EMU88H_STATE_FAILED
};
int  emu88h_state(emu88h_t *h);
/* The power supply: off, or on again (booting with buttons held, which skips the factory reset
   and intro skipping as on the hardware). */
void emu88h_set_power(emu88h_t *h, int on, uint32_t held_buttons);

/* Panel lamps, a bit per lamp in the board's numbering. */
uint32_t emu88h_leds(emu88h_t *h);

/* The LCD texture of a screen (1 = the CM-64's second one): its size, 0 when there is none. */
int emu88h_lcd_size(int model, unsigned screen, int *w, int *h);
/* Draws screen into argb (premultiplied-free 0xAARRGGBB, w * h from emu88h_lcd_size()) when
   the display has changed since *revision; returns 1 then, 0 when nothing changed. powered
   says whether the glass has its supply (a board in standby cuts it). */
int emu88h_lcd_render(emu88h_t *h, unsigned screen, uint32_t *argb, uint64_t *revision, int *powered);

#ifdef __cplusplus
}
#endif

#endif /* EMU88_HOST_H */
