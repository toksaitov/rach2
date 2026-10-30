/*
 * mixer.h: /dev/mixer, OSS's mixer device, which reads and sets the master
 * volume of the audio driver in use.
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
 * The ioctls of /dev/mixer, OSS's with Linux's numbers:
 * SOUND_MIXER_READ_VOLUME and SOUND_MIXER_WRITE_VOLUME, whose level is the
 * left channel's percentage in the low byte and the right's in the next,
 * the device and record masks, SOUND_MIXER_INFO and OSS_GETVERSION. The
 * driver's volume is one channel, so a write sets the louder of the two
 * sides and a read gives the level on both; the driver keeps the level set
 * and applies it again when a stream starts. -EINVAL for another channel,
 * -ENOTTY for another request, -EFAULT for a bad pointer, -ENODEV when no
 * driver with a volume is in use.
 */
int32_t mixer_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg);

#endif /* _MIXER_H_ */
