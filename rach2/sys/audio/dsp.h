/*
 * dsp.h: /dev/dsp, OSS's audio device, which plays what a program writes
 * through the stream operations of the audio driver in use.
 */
#ifndef _DSP_H_
#define _DSP_H_

#include "../lib/kint.h"
#include "../fs/file.h"

/* The numbers of /dev/dsp's node, as on Linux. */
#define DSP_MAJOR 14
#define DSP_MINOR 3

/** Whether /dev/dsp is there: 1 while the driver in use has the stream
 * operations, 0 otherwise. devfs.c lists and opens the node only then. */
int dspdev_present(void);

/**
 * Open /dev/dsp for the new descriptor `e`, whose open(2) flags are `flags`:
 * take the playback stream and set OSS's defaults, unsigned 8-bit mono at
 * 8000 Hz. 0; -EBUSY while another open has the device or the driver's
 * stream is taken; -ENXIO for an open for reading, since the device does
 * not record; -ENODEV when the driver has no streams; -ENOMEM.
 */
int32_t dspdev_open(fd_entry_t *e, int flags);

/**
 * Queue `count` bytes of samples to play, in the format, channels and rate
 * set. It starts the stream once enough is queued to fill the driver's
 * buffer, `device_periods` periods (audio.h), and waits for room while the
 * buffer is full: then it returns the bytes taken so far when a signal
 * comes, or -EINTR when it took none, as Linux's does, and with O_NONBLOCK
 * the bytes taken or -EAGAIN. A partial frame stays queued until a later
 * write completes it. -EFAULT for a bad buffer, -EIO when the device fails
 * to start.
 */
int32_t dspdev_write(fd_entry_t *e, const char __user *buf, uint32_t count);

/** POLLOUT while a whole period of the buffer is free. */
short dspdev_poll(fd_entry_t *e, short events, struct wait_queue **wq);

/**
 * OSS's ioctls of /dev/dsp, with Linux's numbers; dsp(4) lists them and
 * their answers. -EINVAL for another of OSS's 'P' requests or a value out of
 * range, -ENOTTY for another request, -EFAULT for a bad pointer, -EINTR
 * when a signal ends SNDCTL_DSP_SYNC's wait.
 */
int32_t dspdev_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg);

/** The last close: wait for what is queued to play, unless the descriptor
 * is nonblocking or a signal comes, then stop the stream and give the
 * device back. May sleep. */
void dspdev_close(fd_entry_t *e);

#endif /* _DSP_H_ */
