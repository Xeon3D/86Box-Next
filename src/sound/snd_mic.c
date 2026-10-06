/*
 * 86Box-Next: the host's microphone, for a voice modem's handset
 * (char_modem.c): OpenAL capture, 8000 Hz mono 16-bit, opened while a
 * handset is off hook.  Builds without OpenAL have no microphone, and the
 * handset only listens.
 *
 * Only the emulation thread calls these.
 *
 * Released under the GNU General Public License version 2 or later.  See
 * COPYING for more information.
 */
#include <stddef.h>
#include <stdint.h>
#include <86box/86box.h>
#include <86box/snd_mic.h>

#ifdef USE_OPENAL_MIC
/* As openal.c: OpenAL is linked in, not imported. */
#    undef AL_API
#    undef ALC_API
#    define AL_LIBTYPE_STATIC
#    define ALC_LIBTYPE_STATIC
#    include "AL/al.h"
#    include "AL/alc.h"

#    define MIC_RATE 8000
#    define MIC_KEEP 2400 /* more than 300 ms captured and not read: catch up */

static ALCdevice *mic;
static int        users;

int
snd_mic_open(void)
{
    if (users++ == 0) {
        /* The input device chosen in Settings > Sound, if any (it is a
           host device, whether or not the machine's sound card records). */
        if (sound_input_dev_name[0] != '\0')
            mic = alcCaptureOpenDevice(sound_input_dev_name, MIC_RATE, AL_FORMAT_MONO16, MIC_RATE);
        if (mic == NULL)
            mic = alcCaptureOpenDevice(NULL, MIC_RATE, AL_FORMAT_MONO16, MIC_RATE);
        if (mic != NULL)
            alcCaptureStart(mic);
    }
    return (mic != NULL) ? 0 : -1;
}

void
snd_mic_close(void)
{
    if ((users > 0) && (--users == 0) && (mic != NULL)) {
        alcCaptureStop(mic);
        alcCaptureCloseDevice(mic);
        mic = NULL;
    }
}

size_t
snd_mic_read(int16_t *buf, size_t n)
{
    ALCint avail = 0;

    if (mic == NULL)
        return 0;
    alcGetIntegerv(mic, ALC_CAPTURE_SAMPLES, 1, &avail);
    /* The capture's clock and the emulation's are not the same clock: drop
       what has piled up rather than talk ever further behind. */
    while (avail > (ALCint) (MIC_KEEP + n)) {
        int16_t      junk[400];
        const ALCint k = ((avail - (ALCint) n - MIC_KEEP / 2) > 400) ? 400 : (avail - (ALCint) n - MIC_KEEP / 2);

        alcCaptureSamples(mic, junk, k);
        avail -= k;
    }
    if (avail <= 0)
        return 0;
    if ((size_t) avail < n)
        n = (size_t) avail;
    alcCaptureSamples(mic, buf, (ALCsizei) n);
    return n;
}

#else

int
snd_mic_open(void)
{
    return -1;
}

void
snd_mic_close(void)
{
}

size_t
snd_mic_read(int16_t *buf, size_t n)
{
    (void) buf;
    (void) n;
    return 0;
}

#endif
