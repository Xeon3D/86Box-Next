/*
 * 86Box-Next: the host's microphone, for a voice modem's handset
 * (char_modem.c).  8000 Hz mono, 16-bit; OpenAL capture where the build has
 * OpenAL, nothing elsewhere.
 *
 * Released under the GNU General Public License version 2 or later.  See
 * COPYING for more information.
 */
#ifndef EMU_SND_MIC_H
#define EMU_SND_MIC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Counted: the device opens with the first user and closes with the last.
   0 if there is a microphone to hear. */
int  snd_mic_open(void);
void snd_mic_close(void);
/* Up to n samples captured since the last read; the rest of buf is left
   alone.  Returns how many. */
size_t snd_mic_read(int16_t *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /*EMU_SND_MIC_H*/
