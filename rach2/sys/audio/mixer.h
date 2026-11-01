/*
 * mixer.h: /dev/mixer, OSS's mixer device, which reads and sets the master
 * volume of the audio driver in use and its recording source and levels.
 */
#ifndef _MIXER_H_
#define _MIXER_H_

#include "../lib/kint.h"
#include "../fs/file.h"

/* The numbers of /dev/mixer's node, as on Linux. */
#define MIXER_MAJOR 14
#define MIXER_MINOR 0

/** Whether /dev/mixer is there: 1 while an audio driver that has a volume,
 * HDA or the Sound Blaster, is in use, 0 otherwise. devfs.c lists and opens
 * the node only then. */
int mixer_present(void);

/**
 * The ioctls of /dev/mixer, OSS's with Linux's numbers: SOUND_MIXER_READ
 * and SOUND_MIXER_WRITE of each channel the device mask has, whose level is
 * the left channel's percentage in the low byte and the right's in the
 * next; the device, record and stereo masks, the capabilities and the
 * record source, read and written; SOUND_MIXER_INFO and OSS_GETVERSION.
 *
 * The channels are VOLUME, the hardware master, and PCM, the calling
 * process's playback channels before they are summed, and, on a driver with input controls
 * (audio.h), MIC, MONITOR and LINE, the levels of the microphone jack, the internal microphone and
 * the line in, RECLEV, by which each source's level is scaled for the converter's gain, and IGAIN,
 * the boost of each source's pin. Each channel is one level, so a write sets the louder of the two
 * sides and a read gives the level on both. The record source is one source at a time
 * (SOUND_CAP_EXCL_INPUT); a write of a mask of several takes one that is
 * not recorded now. -EINVAL for a channel the mask lacks, -ENOTTY for
 * another request, -EFAULT for a bad pointer, -ENODEV when no driver with a
 * volume is in use.
 */
int32_t mixer_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg);

#endif /* _MIXER_H_ */
