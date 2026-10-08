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

#ifdef __cplusplus
}
#endif

#endif /*EMU_DEBUG_CMD_H*/
