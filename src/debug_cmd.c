/*
 * 86Box-Next    Debug command file, for driving a guest in automated tests
 *               without touching the host's keyboard focus.
 *
 *               Off unless BOX86NEXT_DEBUG_CMD names a file. The emulation
 *               thread looks for that file about ten times a second; when it
 *               exists, its lines are run and the file is deleted:
 *
 *                 key 1c,9c        scancodes (hex; 9c = release), one every 20 ms
 *                 type text        letters, digits and spaces (pressed and released)
 *                 shot path.bmp    the emulated screen as a 32-bit BMP
 *                 log text         a line in the 86Box log
 *                 dev args         passed to the device that registered a hook
 *
 *               Each command is logged ("debug_cmd: ..."), so a test can wait
 *               for the file to vanish and then read the log.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <86box/86box.h>
#include <86box/keyboard.h>
#include <86box/video.h>
#include <86box/debug_cmd.h>

#define DEBUG_CMD_QUEUE 512

static char     debug_cmd_path[1024];
static int      debug_cmd_state = -1; /* -1 unknown, 0 off, 1 on */
static uint32_t debug_cmd_ticks;
static uint16_t debug_cmd_keys[DEBUG_CMD_QUEUE];
static int      debug_cmd_key_head;
static int      debug_cmd_key_tail;
static int      debug_cmd_key_wait;
static void   (*debug_cmd_device_hook)(const char *args);

void
debug_cmd_set_device_hook(void (*hook)(const char *args))
{
    debug_cmd_device_hook = hook;
}

static void
debug_cmd_queue_key(uint16_t code)
{
    int next = (debug_cmd_key_tail + 1) % DEBUG_CMD_QUEUE;

    if (next == debug_cmd_key_head)
        return;
    debug_cmd_keys[debug_cmd_key_tail] = code;
    debug_cmd_key_tail                 = next;
}

/* Set 1 make codes for what "type" accepts. */
static uint16_t
debug_cmd_scancode(char c)
{
    static const char    *rows[]  = { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" };
    static const uint16_t first[] = { 0x02, 0x10, 0x1e, 0x2c };

    c = (char) tolower((unsigned char) c);
    if (c == ' ')
        return 0x39;
    for (int r = 0; r < 4; r++) {
        const char *p = strchr(rows[r], c);
        if (p != NULL)
            return first[r] + (uint16_t) (p - rows[r]);
    }
    return 0;
}

static void
debug_cmd_shot(const char *path)
{
    const monitor_t *mon = &monitors[0];
    const bitmap_t  *bmp = mon->target_buffer;
    int              w, h;
    FILE            *f;
    uint8_t          hdr[54] = { 0 };

    if (bmp == NULL) {
        always_log("debug_cmd: shot %s: no screen\n", path);
        return;
    }
    w = mon->mon_xsize + 32;
    h = mon->mon_ysize + 32;
    if (w > bmp->w)
        w = bmp->w;
    if (h > bmp->h)
        h = bmp->h;
    if ((w <= 0) || (h <= 0) || ((f = fopen(path, "wb")) == NULL)) {
        always_log("debug_cmd: shot %s: cannot write (%dx%d)\n", path, w, h);
        return;
    }

    uint32_t size = 54 + (uint32_t) (w * h * 4);
    hdr[0]        = 'B';
    hdr[1]        = 'M';
    memcpy(&hdr[2], &size, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(&hdr[18], &w, 4);
    int32_t neg_h = -h; /* top-down rows */
    memcpy(&hdr[22], &neg_h, 4);
    hdr[26] = 1;
    hdr[28] = 32;
    fwrite(hdr, 1, sizeof(hdr), f);
    for (int y = 0; y < h; y++)
        fwrite(bmp->line[y], 4, w, f);
    fclose(f);
    always_log("debug_cmd: shot %s (%dx%d, mode %dx%d)\n", path, w, h, mon->mon_xsize, mon->mon_ysize);
}

static void
debug_cmd_run(char *line)
{
    char *arg;

    line[strcspn(line, "\r\n")] = '\0';
    arg = strchr(line, ' ');
    if (arg != NULL)
        *arg++ = '\0';
    else
        arg = line + strlen(line);

    if (!strcmp(line, "key")) {
        for (char *tok = strtok(arg, ", "); tok != NULL; tok = strtok(NULL, ", "))
            debug_cmd_queue_key((uint16_t) strtoul(tok, NULL, 16));
        always_log("debug_cmd: key %s\n", arg);
    } else if (!strcmp(line, "type")) {
        for (const char *p = arg; *p; p++) {
            uint16_t code = debug_cmd_scancode(*p);
            if (code) {
                debug_cmd_queue_key(code);
                debug_cmd_queue_key(code | 0x80);
            }
        }
        always_log("debug_cmd: type %s\n", arg);
    } else if (!strcmp(line, "shot"))
        debug_cmd_shot(arg);
    else if (!strcmp(line, "dev")) {
        if (debug_cmd_device_hook != NULL)
            debug_cmd_device_hook(arg);
        else
            always_log("debug_cmd: dev: no device hook\n");
    } else if (!strcmp(line, "log"))
        always_log("debug_cmd: %s\n", arg);
    else if (line[0])
        always_log("debug_cmd: unknown command \"%s\"\n", line);
}

void
debug_cmd_poll(void)
{
    if (debug_cmd_state == 0)
        return;
    if (debug_cmd_state < 0) {
        const char *env = getenv("BOX86NEXT_DEBUG_CMD");

        debug_cmd_state = (env != NULL) && env[0];
        if (!debug_cmd_state)
            return;
        snprintf(debug_cmd_path, sizeof(debug_cmd_path), "%s", env);
        always_log("debug_cmd: watching %s\n", debug_cmd_path);
    }

    /* Queued keys go out one every 20 polls (pc_run runs per 1 ms of guest time). */
    if (debug_cmd_key_head != debug_cmd_key_tail) {
        if (++debug_cmd_key_wait >= 20) {
            uint16_t code      = debug_cmd_keys[debug_cmd_key_head];
            debug_cmd_key_head = (debug_cmd_key_head + 1) % DEBUG_CMD_QUEUE;
            debug_cmd_key_wait = 0;
            keyboard_input(!(code & 0x80), code & 0x7f);
        }
    }

    if (++debug_cmd_ticks < 100)
        return;
    debug_cmd_ticks = 0;

    FILE *f = fopen(debug_cmd_path, "r");
    if (f == NULL)
        return;
    char line[1024];
    while (fgets(line, sizeof(line), f) != NULL)
        debug_cmd_run(line);
    fclose(f);
    remove(debug_cmd_path);
}
