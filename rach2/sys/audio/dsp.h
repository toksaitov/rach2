/*
 * dsp.h: /dev/dsp, OSS's audio device, which plays what a program writes,
 * mixed with what other opens write, and records what it reads through the
 * stream operations of the audio driver in use.
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
 * take a virtual channel of the mix (vchan.h) for O_WRONLY, a virtual reader of the shared
 * capture stream for O_RDONLY, and both for O_RDWR, and set OSS's defaults,
 * unsigned 8-bit mono at 8000 Hz, each open its own. Any number of opens
 * play at once, and any number record. 0; -EBUSY when another kernel
 * caller has taken the hardware stream directly; -ENODEV when the driver has no streams or
 * lacks a direction asked for; -EINVAL for an access mode of 3; -ENOMEM.
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
 * to start, -EBADF in an open not for writing.
 */
int32_t dspdev_write(fd_entry_t *e, const char __user *buf, uint32_t count);

/**
 * Read `count` bytes of samples recorded, in the format, channels and rate
 * set. The first read starts the capture stream, unless SETTRIGGER has
 * turned input off, and a read waits until it has all `count` bytes: then it
 * returns the bytes taken so far when a signal comes, or -EINTR when it took
 * none, and with O_NONBLOCK the bytes there were or -EAGAIN. A read may end
 * within a frame; the next goes on from there. When the program falls
 * behind by the whole buffer, the device's newest frames are dropped until
 * there is room. -EFAULT for a bad buffer, -EIO when the device fails to
 * start, -EBADF in an open not for reading.
 */
int32_t dspdev_read(fd_entry_t *e, char __user *buf, uint32_t count);

/** POLLIN while a whole period has been recorded, and POLLOUT while a whole
 * period of the playback buffer is free. A poll for POLLIN starts the
 * capture stream as a read does. */
short dspdev_poll(fd_entry_t *e, short events, struct wait_queue **wq);

/**
 * OSS's ioctls of /dev/dsp, with Linux's numbers; dsp(4) lists them and
 * their answers. -EINVAL for another of OSS's 'P' requests or a value out of
 * range, -ENOTTY for another request, -EFAULT for a bad pointer, -EINTR
 * when a signal ends SNDCTL_DSP_SYNC's wait.
 */
int32_t dspdev_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg);

/** The last close: wait for what is queued to play, unless the descriptor
 * is nonblocking or a signal comes, then stop the streams, drop what was
 * recorded and not read, and give the device back. May sleep. */
void dspdev_close(fd_entry_t *e);

#endif /* _DSP_H_ */
