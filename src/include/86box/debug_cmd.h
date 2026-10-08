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

#ifdef __cplusplus
}
#endif

#endif /*EMU_DEBUG_CMD_H*/
