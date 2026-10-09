/*
 * 86Box-Next    Debug command file, for driving a guest in automated tests
 *               without touching the host's keyboard focus.
 *
 *               Off unless BOX86NEXT_DEBUG_CMD names a file. The emulation
 *               thread looks for that file about ten times a second; when it
 *               exists, its lines are run and the file is deleted:
 *
 *                 key 1c,9c        scancodes (hex; 9c = release), one every 20 ms
 *                 type text        printable US-keyboard characters (pressed and released)
 *                 shot path.bmp    the emulated screen as a 32-bit BMP
 *                 log text         a line in the 86Box log
 *                 dev args         passed to the device that registered a hook
 *                 cpu              CS:EIP, registers and the code bytes there
 *                 peek addr        the physical address and 16 bytes at a linear address
 *                 bp addr [n]      log registers and stack the next n (default 8) times the
 *                                  CPU executes linear addr; "bp clear" drops them all
 *                 wp addr          log each instruction that changes the dword at linear addr
 *                                  (its physical page as mapped now); "wp clear" drops it
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
#include <86box/mem.h>
#include "cpu.h"

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
    /* US layout: set 1 make codes for 0x02-0x35 and 0x39, unshifted and shifted;
       0x100 in the result = hold shift */
    static const char plain[]   = "\0\0" "1234567890-=\0\0" "qwertyuiop[]\0\0" "asdfghjkl;'`\0\\" "zxcvbnm,./";
    static const char shifted[] = "\0\0" "!@#$%^&*()_+\0\0" "QWERTYUIOP{}\0\0" "ASDFGHJKL:\"~\0|" "ZXCVBNM<>?";

    if (c == ' ')
        return 0x39;
    for (uint16_t code = 2; code < sizeof(plain) - 1; code++) {
        if (plain[code] && (plain[code] == c))
            return code;
        if (shifted[code] && (shifted[code] == c))
            return code | 0x100;
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

/* Where the guest is: registers, and the code at CS:EIP (paging-translated
   without faulting; "--" for an unmapped byte). */
static void
debug_cmd_cpu(void)
{
    char     code[3 * 32 + 1];
    uint32_t lin = cs + cpu_state.pc;

    for (int i = 0; i < 32; i++) {
        uint64_t phys = (cr0 >> 31) ? mmutranslate_noabrt(lin + i, 0) : (uint64_t) (lin + i);

        if (phys == 0xffffffffffffffffULL)
            snprintf(code + 3 * i, 4, " --");
        else
            snprintf(code + 3 * i, 4, " %02x", mem_readb_phys((uint32_t) phys));
    }
    always_log("debug_cmd: cpu %04x:%08x (lin %08x) flags %04x eax %08x ebx %08x ecx %08x edx %08x esi %08x edi %08x ebp %08x esp %08x ds %04x es %04x\n",
               CS, cpu_state.pc, lin, cpu_state.flags, EAX, EBX, ECX, EDX, ESI, EDI, EBP, ESP, DS, ES);
    always_log("debug_cmd: code%s\n", code);
}

#define DEBUG_CMD_BPS 16
int             debug_cmd_bp_count;
static uint32_t debug_cmd_bp_addr[DEBUG_CMD_BPS];
static uint32_t debug_cmd_bp_left[DEBUG_CMD_BPS];
extern int      cpu_force_interpreter;

static uint32_t
debug_cmd_lin_read32(uint32_t lin)
{
    uint64_t phys = (cr0 >> 31) ? mmutranslate_noabrt(lin, 0) : (uint64_t) lin;

    return (phys == 0xffffffffffffffffULL) ? 0xdeadbeef : mem_readl_phys((uint32_t) phys);
}

static uint32_t debug_cmd_wp_phys;
static uint32_t debug_cmd_wp_val;
static uint32_t debug_cmd_wp_left;
static uint32_t debug_cmd_wp_lastpc;

/* Called by the interpreter before each instruction while breakpoints are set. */
void
debug_cmd_bp_check(uint32_t lin)
{
    if (debug_cmd_wp_left) {
        uint32_t v = mem_readl_phys(debug_cmd_wp_phys);
        if (v != debug_cmd_wp_val) {
            debug_cmd_wp_left--;
            always_log("debug_cmd: wp %08x: %08x -> %08x by the instruction at %08x (now at %08x, esp %08x, [esp] %08x %08x %08x)\n",
                       debug_cmd_wp_phys, debug_cmd_wp_val, v, debug_cmd_wp_lastpc, lin, ESP,
                       debug_cmd_lin_read32(ss + ESP), debug_cmd_lin_read32(ss + ESP + 4), debug_cmd_lin_read32(ss + ESP + 8));
            debug_cmd_wp_val = v;
        }
        debug_cmd_wp_lastpc = lin;
    }
    for (int i = 0; i < debug_cmd_bp_count; i++) {
        if ((debug_cmd_bp_addr[i] != lin) || !debug_cmd_bp_left[i])
            continue;
        debug_cmd_bp_left[i]--;
        uint32_t sp = ss + ESP;
        always_log("debug_cmd: bp %08x eax %08x ebx %08x ecx %08x edx %08x esi %08x edi %08x ebp %08x esp %08x | "
                   "ds %08x es %08x | stack %08x %08x %08x %08x %08x %08x %08x %08x\n",
                   lin, EAX, EBX, ECX, EDX, ESI, EDI, EBP, ESP, ds, es, debug_cmd_lin_read32(sp), debug_cmd_lin_read32(sp + 4),
                   debug_cmd_lin_read32(sp + 8), debug_cmd_lin_read32(sp + 12), debug_cmd_lin_read32(sp + 16),
                   debug_cmd_lin_read32(sp + 20), debug_cmd_lin_read32(sp + 24), debug_cmd_lin_read32(sp + 28));
    }
}

static void
debug_cmd_wp(char *arg)
{
    if (!strncmp(arg, "clear", 5)) {
        debug_cmd_wp_left = 0;
        if (debug_cmd_bp_count == 1)
            debug_cmd_bp_count = 0;
        return;
    }
    uint32_t lin  = strtoul(arg, NULL, 16);
    uint64_t phys = (cr0 >> 31) ? mmutranslate_noabrt(lin, 0) : (uint64_t) lin;

    if (phys == 0xffffffffffffffffULL) {
        always_log("debug_cmd: wp %08x: not mapped\n", lin);
        return;
    }
    debug_cmd_wp_phys = (uint32_t) phys;
    debug_cmd_wp_val  = mem_readl_phys(debug_cmd_wp_phys);
    debug_cmd_wp_left = 64;
    /* the check runs from the breakpoint hook: keep it called */
    if (debug_cmd_bp_count == 0) {
        debug_cmd_bp_addr[0] = 0xffffffff;
        debug_cmd_bp_left[0] = 0;
        debug_cmd_bp_count   = 1;
    }
    cpu_force_interpreter = 1;
    always_log("debug_cmd: wp %08x (phys %08x) = %08x\n", lin, debug_cmd_wp_phys, debug_cmd_wp_val);
}

static void
debug_cmd_bp(char *arg)
{
    if (!strncmp(arg, "clear", 5)) {
        debug_cmd_bp_count    = 0;
        cpu_force_interpreter = 0;
        always_log("debug_cmd: bp cleared\n");
        return;
    }
    if (debug_cmd_bp_count >= DEBUG_CMD_BPS)
        return;
    char    *end;
    uint32_t addr = strtoul(arg, &end, 16);
    uint32_t n    = strtoul(end, NULL, 0);

    debug_cmd_bp_addr[debug_cmd_bp_count] = addr;
    debug_cmd_bp_left[debug_cmd_bp_count] = n ? n : 8;
    debug_cmd_bp_count++;
    cpu_force_interpreter = 1;
    always_log("debug_cmd: bp %08x x%u\n", addr, n ? n : 8);
}

/* A linear address (paging-translated without faulting) and the 16 bytes there. */
static void
debug_cmd_peek(const char *arg)
{
    char     bytes[3 * 16 + 1];
    uint32_t lin  = strtoul(arg, NULL, 16);
    uint64_t phys = (cr0 >> 31) ? mmutranslate_noabrt(lin, 0) : (uint64_t) lin;

    if (phys == 0xffffffffffffffffULL) {
        always_log("debug_cmd: peek %08x: not mapped\n", lin);
        return;
    }
    for (int i = 0; i < 16; i++)
        snprintf(bytes + 3 * i, 4, " %02x", mem_readb_phys((uint32_t) phys + i));
    always_log("debug_cmd: peek %08x (phys %08x):%s\n", lin, (uint32_t) phys, bytes);
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
            if (code & 0x100)
                debug_cmd_queue_key(0x2a);
            if (code & 0xff) {
                debug_cmd_queue_key(code & 0xff);
                debug_cmd_queue_key((code & 0xff) | 0x80);
            }
            if (code & 0x100)
                debug_cmd_queue_key(0xaa);
        }
        always_log("debug_cmd: type %s\n", arg);
    } else if (!strcmp(line, "shot"))
        debug_cmd_shot(arg);
    else if (!strcmp(line, "dev")) {
        if (debug_cmd_device_hook != NULL)
            debug_cmd_device_hook(arg);
        else
            always_log("debug_cmd: dev: no device hook\n");
    } else if (!strcmp(line, "cpu"))
        debug_cmd_cpu();
    else if (!strcmp(line, "wp"))
        debug_cmd_wp(arg);
    else if (!strcmp(line, "bp"))
        debug_cmd_bp(arg);
    else if (!strcmp(line, "peek"))
        debug_cmd_peek(arg);
    else if (!strcmp(line, "interp")) {
        /* interp 1|0: run every block through the interpreter (to tell dynarec bugs apart) */
        cpu_force_interpreter = atoi(arg) != 0;
        always_log("debug_cmd: interpreter %s\n", cpu_force_interpreter ? "forced" : "off");
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
