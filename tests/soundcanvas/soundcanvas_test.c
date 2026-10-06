/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Tests of the 88emu glue behind the Roland Sound Canvas MIDI
 *             device. The board tests need ROM images: set BOX86NEXT_SC_ROMS
 *             to a folder holding a complete SC-55mkII set, or they are
 *             skipped.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#    include <direct.h>
#    define make_dir(p) _mkdir(p)
#else
#    include <sys/stat.h>
#    define make_dir(p) mkdir(p, 0777)
#endif

#include "emu88_host.h"

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static void
test_catalogue(void)
{
    int w;
    int h;

    CHECK(emu88h_model_count() == 24);
    for (int i = 0; i < emu88h_model_count(); i++) {
        const int model = emu88h_model_at(i);
        CHECK(emu88h_model_name(model) != NULL);
        CHECK(emu88h_model_from_key(emu88h_model_key(model)) == model);
    }
    CHECK(emu88h_model_from_key("nonsense") == -1);
    CHECK(emu88h_model_name(99) == NULL);

    CHECK(emu88h_model_panel(EMU88H_SC55MK2) == EMU88H_PANEL_SC88);
    CHECK(emu88h_model_panel(EMU88H_SC8850) == EMU88H_PANEL_SC8850);
    CHECK(emu88h_model_panel(EMU88H_MT32_NEW) == EMU88H_PANEL_CM);
    CHECK(emu88h_model_panel(EMU88H_SC8820) == EMU88H_PANEL_NONE);
    CHECK(emu88h_model_power_standby(EMU88H_SC55MK2));
    CHECK(!emu88h_model_power_standby(EMU88H_SC88PRO));
    CHECK(emu88h_model_has_knob(EMU88H_MT32_OLD));
    /* The SC-55's panel has no map, preview or USER INST switches; the SC-88Pro's POWER is the mains. */
    CHECK(!(emu88h_model_button_mask(EMU88H_SC55MK2) & (1u << 7)));
    CHECK(emu88h_model_button_mask(EMU88H_SC55MK2) & (1u << 6));
    CHECK(!(emu88h_model_button_mask(EMU88H_SC88PRO) & 1u));
    CHECK(emu88h_model_button_mask(EMU88H_CM32P) == 0);
    CHECK(!strcmp(emu88h_model_artwork(EMU88H_SC155MK2), "sc55mk2"));

    CHECK(emu88h_lcd_size(EMU88H_SC55MK2, 0, &w, &h) && w == 741 && h == 268);
    CHECK(emu88h_lcd_size(EMU88H_SC8850, 0, &w, &h) && w == 640 && h == 256);
    CHECK(emu88h_lcd_size(EMU88H_CM64, 1, &w, &h) && w == 1000 && h == 92);
    CHECK(!emu88h_lcd_size(EMU88H_SC55MK2, 1, &w, &h));
    CHECK(!emu88h_lcd_size(EMU88H_SC8820, 0, &w, &h));
}

/* A folder with nothing in it: every board unavailable, every required image red. */
static void
test_empty_folder(void)
{
    const char  *tmp = getenv("TEMP") ? getenv("TEMP") : "/tmp";
    char         path[1024];
    const char  *dirs[1];
    emu88h_rom_t roms[32];
    int          n;
    int          alternatives = 0;

    snprintf(path, sizeof(path), "%s/soundcanvas_test_empty", tmp);
    make_dir(path);
    dirs[0] = path;
    emu88h_set_rom_dirs(dirs, 1);
    for (int i = 0; i < emu88h_model_count(); i++)
        CHECK(!emu88h_model_available(emu88h_model_at(i)));

    n = emu88h_model_roms(EMU88H_SC55MK2, roms, 32);
    CHECK(n == 4);
    for (int i = 0; i < n && i < 32; i++) {
        CHECK(roms[i].status == EMU88H_ROM_MISSING);
        CHECK(roms[i].file[0] == 0);
        CHECK(roms[i].md5[0] == 0);
        CHECK(!strncmp(roms[i].known, "MD5 ", 4)); /* every SC-55mkII image is a known dump */
        CHECK(roms[i].size > 0);
    }
    CHECK(!strcmp(roms[1].label, "Program ROM"));
    CHECK(!strcmp(roms[1].names, "sc55mk2_program.bin"));

    /* The CM-32L's chip pair stands in for its whole wave image: unlit, not red. */
    n = emu88h_model_roms(EMU88H_CM32L, roms, 32);
    for (int i = 0; i < n && i < 32; i++)
        alternatives += roms[i].status == EMU88H_ROM_ALTERNATIVE;
    CHECK(alternatives == 2);

    /* The CM-64 lists both of its boards. */
    CHECK(emu88h_model_roms(EMU88H_CM64, roms, 32) == emu88h_model_roms(EMU88H_CM32L, NULL, 0) + emu88h_model_roms(EMU88H_CM32P, NULL, 0));
}

static double
rms(emu88h_t *h, int blocks)
{
    static float buf[480 * 2];
    double       e = 0;

    for (int b = 0; b < blocks; b++) {
        emu88h_render(h, buf, 480, 48000);
        for (int i = 0; i < 960; i++)
            e += buf[i] * buf[i];
    }
    return sqrt(e / (blocks * 960.0));
}

static void
test_board(const char *roms_dir)
{
    const char   *dirs[1] = { roms_dir };
    const uint8_t chord[] = { 0x90, 60, 100, 0x90, 64, 100, 0x90, 67, 100 };
    emu88h_rom_t  roms[8];
    uint32_t     *lcd;
    uint64_t      rev = 0;
    int           powered;
    int           dots = 0;
    double        loud;
    emu88h_t     *h;
    int           n;

    emu88h_set_rom_dirs(dirs, 1);
    if (!emu88h_model_available(EMU88H_SC55MK2)) {
        printf("SKIP: no complete SC-55mkII set in %s\n", roms_dir);
        return;
    }
    n = emu88h_model_roms(EMU88H_SC55MK2, roms, 8);
    for (int i = 0; i < n && i < 8; i++) {
        CHECK(roms[i].status == EMU88H_ROM_OK || roms[i].status == EMU88H_ROM_UNVERIFIED);
        CHECK(strlen(roms[i].md5) == 32);
        /* A known dump's checksum is one of the known ones (unless stored word-swapped). */
        if (roms[i].status == EMU88H_ROM_OK && strstr(roms[i].known, roms[i].md5) == NULL)
            printf("note: %s %s is a word-swapped dump\n", roms[i].label, roms[i].file);
    }

    h = emu88h_create(EMU88H_SC55MK2, 1, 1);
    CHECK(emu88h_state(h) == EMU88H_STATE_BOOTING);
    CHECK(rms(h, 1) < 1e-6);
    CHECK(emu88h_state(h) == EMU88H_STATE_ON);
    rms(h, 300);

    /* MIDI in, sound out. */
    emu88h_midi(h, 0, chord, sizeof(chord));
    loud = rms(h, 50);
    printf("chord rms %.4f\n", loud);
    CHECK(loud > 0.005);
    emu88h_set_gain(h, 0);
    CHECK(rms(h, 5) < 1e-6);
    emu88h_set_gain(h, 100);

    /* The display: drawn once, then only when it changes. The part screen has dots on. */
    lcd = calloc(741 * 268, 4);
    CHECK(emu88h_lcd_render(h, 0, lcd, &rev, &powered) == 1);
    CHECK(powered);
    for (int i = 0; i < 741 * 268; i++)
        dots += lcd[i] == 0xff000000u;
    CHECK(dots > 1000);
    CHECK(emu88h_lcd_render(h, 0, lcd, &rev, &powered) == 0);

    /* The supply: off is silent and dark, on boots afresh. */
    emu88h_set_power(h, 0, 0);
    CHECK(emu88h_state(h) == EMU88H_STATE_OFF);
    emu88h_midi(h, 0, chord, sizeof(chord));
    CHECK(rms(h, 5) < 1e-6);
    CHECK(emu88h_lcd_render(h, 0, lcd, &rev, &powered) == 1);
    CHECK(!powered);
    emu88h_set_power(h, 1, 0);
    CHECK(emu88h_state(h) == EMU88H_STATE_BOOTING);
    rms(h, 300);
    CHECK(emu88h_state(h) == EMU88H_STATE_ON);
    emu88h_midi(h, 0, chord, sizeof(chord));
    CHECK(rms(h, 50) > 0.005);

    /* POWER is a key on this board: pressed, it goes to standby and cuts the display supply. */
    emu88h_set_buttons(h, 1u);
    rms(h, 20);
    emu88h_set_buttons(h, 0);
    rms(h, 50);
    CHECK(emu88h_lcd_render(h, 0, lcd, &rev, &powered) == 1);
    CHECK(!powered);

    free(lcd);
    emu88h_release(h);
}

int
main(void)
{
    const char *roms = getenv("BOX86NEXT_SC_ROMS");

    test_catalogue();
    test_empty_folder();
    if (roms && *roms)
        test_board(roms);
    else
        printf("SKIP: board tests (set BOX86NEXT_SC_ROMS to a ROM folder)\n");
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        printf("all passed\n");
    return failures ? 1 : 0;
}
