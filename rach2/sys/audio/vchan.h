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

/*
 * The virtual channels, a driver with the seven stream operations and no
 * others. Each playback stream opened on it is a channel of its own, with
 * its own format, channels, rate and period; the channels that run are
 * mixed into one stream of the device's driver, `audio_hw`, which plays
 * 16-bit stereo at the rate it grants for VCHAN_RATE. The device's stream is
 * taken at the first channel's open and given back at the last one's close,
 * and it runs while a channel runs. A channel started while it runs joins
 * at the next frame of the mix not yet made, after what the device holds.
 * Capture streams are not taken: stream_open gives -ENODEV for them.
 */
extern audio_driver_t vchan_driver;

#endif /* _VCHAN_H_ */
