/*
 * vchan.h: virtual playback channels, which mix every playback open of
 * /dev/dsp into the one playback stream of the device.
 */
#ifndef _VCHAN_H_
#define _VCHAN_H_

#include "audio.h"

/* The mix asks the device for this rate, which HD Audio codecs have and the
 * HDA driver converts on one that lacks it; the SB16 grants 44100 Hz in its
 * place. */
#define VCHAN_RATE 48000u

/** Playback channels mixed into one device stream, capture channels fed
 * from one input, and the console's private playback channel. Hardware
 * identity and controls pass through to audio_hw. */
extern audio_driver_t vchan_driver;

/** Install the dispatch driver after audio_hw is chosen, if it has streams. */
void vchan_dispatch(void);

/** Select capture of the clipped mix at boot, before any stream is open. */
void vchan_set_loopback(int enabled);

/** Read the calling process's PCM level, or set all the playback opens it
 * owns to `level` (0 to 100). Negative only reads; no open reads as 100.
 * A new open takes the level of that process's existing opens, else 100. */
int vchan_pcm(int level);

#endif /* _VCHAN_H_ */
