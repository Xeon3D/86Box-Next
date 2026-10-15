/*
 * 86Box-Next    Debug command file (BOX86NEXT_DEBUG_CMD), see debug_cmd.c.
 */
#ifndef EMU_DEBUG_CMD_H
#define EMU_DEBUG_CMD_H

#ifdef __cplusplus
extern "C" {
#endif

extern void debug_cmd_poll(void);

/* A device can answer "dev <args>" (one at a time; NULL to drop it). */
extern void debug_cmd_set_device_hook(void (*hook)(const char *args));

/* "bp" breakpoints: linear addresses logged (registers and stack) when executed.
   While any is set, the CPU interprets (no dynarec blocks skip the check). */
extern int  debug_cmd_bp_count;
extern void debug_cmd_bp_check(uint32_t lin);

/* "speedlog 1|0": the speed shown in the title bar, logged with where the
   guest is, once a second. */
extern int  debug_cmd_speed_log;
/* set once a "mouse" line has run: the PS/2 mouse reports without capture */
extern int  debug_cmd_mouse;
extern void debug_cmd_log_speed(int speed_percent);

#ifdef __cplusplus
}
#endif

#endif /*EMU_DEBUG_CMD_H*/
