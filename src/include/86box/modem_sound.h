/*
 * 86Box-Next: the modem's speaker (src/char/modem_sound.c).
 */
#ifndef EMU_MODEM_SOUND_H
#define EMU_MODEM_SOUND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct modem_sound_t modem_sound_t;

enum {
    MODEM_SOUND_HANGUP = 0,
    MODEM_SOUND_DIAL,     /* number; arg = S8 seconds | (pulse << 8) */
    MODEM_SOUND_ANSWER,   /* the far end picked up: the handshake     */
    MODEM_SOUND_CONNECT,  /* carrier: M1 goes quiet                   */
    MODEM_SOUND_BUSY
};

extern modem_sound_t *modem_sound_init(int enabled);
extern void           modem_sound_close(modem_sound_t *s);
extern void           modem_sound_event(modem_sound_t *s, int type, const char *number, int arg);
extern void           modem_sound_speaker(modem_sound_t *s, int mode, int level); /* ATM, ATL */
extern void           modem_sound_country(modem_sound_t *s, int uk, int v90);

/* How long each part of a call takes -- the modem waits these out whether
   or not the speaker is heard, so a call takes as long as a real one. */
extern uint32_t modem_sound_dial_ms(const char *number, int s8, int pulse);
extern uint32_t modem_sound_ring_ms(void);
extern uint32_t modem_sound_handshake_ms(int v90);

#ifdef __cplusplus
}
#endif

#endif
