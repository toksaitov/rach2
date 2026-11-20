/*
 * dsp.c: /dev/dsp (dsp.h), OSS's audio device: for each direction an open
 * has, a buffer of periods between the program and a stream, and OSS's
 * ioctls. Each open plays on a virtual channel of its own (vchan.h), so
 * that several play at once, and records on a virtual reader of the shared
 * input.
 */

#include "dsp.h"

#include "audio.h"
#include "vchan.h"

#include "../io/poll.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../lib/spinlock.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../proc/wait_queue.h"

/*
 * The requests are OSS's, with Linux's numbers (<linux/soundcard.h>):
 * _IOWR('P', n, int) for a value set and read back, _IOR for a value read,
 * and _IO for a command. SNDCTL_DSP_SAMPLESIZE is SETFMT's number. Linux's
 * header stops at AFMT_AC3; the wider formats have OSS 4's numbers, which
 * ALSA's OSS emulation takes and cmus defines on Linux.
 */
#define DSP_RESET       0x00005000u
#define DSP_SYNC        0x00005001u
#define DSP_SPEED       0xC0045002u
#define DSP_STEREO      0xC0045003u
#define DSP_GETBLKSIZE  0xC0045004u
#define DSP_SETFMT      0xC0045005u
#define DSP_CHANNELS    0xC0045006u
#define DSP_POST        0x00005008u
#define DSP_SETFRAGMENT 0xC004500Au
#define DSP_GETFMTS     0x8004500Bu
#define DSP_GETOSPACE   0x8010500Cu
#define DSP_GETISPACE   0x8010500Du
#define DSP_NONBLOCK    0x0000500Eu
#define DSP_GETCAPS     0x8004500Fu
#define DSP_GETTRIGGER  0x80045010u
#define DSP_SETTRIGGER  0x40045010u
#define DSP_GETIPTR     0x800C5011u
#define DSP_GETOPTR     0x800C5012u
#define DSP_SETDUPLEX   0x00005016u
#define DSP_GETODELAY   0x80045017u
#define OSS_GETVERSION  0x80044D76u

#define AFMT_QUERY        0x00
#define AFMT_U8           0x08
#define AFMT_S16_LE       0x10
#define AFMT_S32_LE       0x1000
#define AFMT_S24_LE       0x8000 /* 24 bits in the low three bytes of four */
#define AFMT_S24_PACKED   0x40000
#define DSP_FORMATS       (AFMT_U8 | AFMT_S16_LE | AFMT_S24_LE | AFMT_S24_PACKED | AFMT_S32_LE)
#define DSP_CAP_REVISION  1
#define DSP_CAP_DUPLEX    0x100
#define DSP_CAP_REALTIME  0x200
#define DSP_CAP_TRIGGER   0x1000
#define PCM_ENABLE_INPUT  1
#define PCM_ENABLE_OUTPUT 2
#define OSS_VERSION       0x030802 /* SOUND_VERSION, as Linux gives it */

/*
 * The buffers. Their periods are powers of two from DSP_PERIOD_MIN to
 * DSP_PERIOD_MAX bytes, and there are at least two of them, at most as
 * many as DSP_BUFFER_MAX holds. Without SETFRAGMENT a period is the power of
 * two nearest a 43rd of a second, about 23 ms, and there are DSP_PERIODS.
 * A playback buffer may take one period more, DSP_RING_MAX in all, so that
 * it holds a period beyond the start (dsp_layout), and more than that where
 * the driver's buffer holds more of the stream, up to AUDIO_DEVICE_MAX and
 * a period.
 */
#define DSP_BUFFER_MAX   65536u
#define DSP_PERIOD_MIN   128u
#define DSP_PERIOD_MAX   16384u
#define DSP_RING_MAX     (DSP_BUFFER_MAX + DSP_PERIOD_MAX)
#define DSP_PERIOD_HZ    43u
#define DSP_PERIODS      4u
#define DSP_RATE_MIN     1000u
#define DSP_RATE_MAX     192000u
#define DSP_CHUNK        256u /* bytes a read or write copies at a time */
#define DSP_CHANNELS_MAX 128  /* CHANNELS asks no more, as Linux's OSS layer */

/* OSS's `audio_buf_info`, GETOSPACE's and GETISPACE's answer. */
struct dsp_buf_info {
    int fragments;  /* whole periods free (output) or ready (input) */
    int fragstotal; /* periods in the buffer */
    int fragsize;   /* bytes of a period */
    int bytes;      /* bytes free (output) or ready (input) */
};

/* OSS's `count_info`, GETOPTR's and GETIPTR's answer. */
struct dsp_count_info {
    int bytes;  /* bytes played or recorded since the open, wrapping */
    int blocks; /* periods ended since the last call */
    int ptr;    /* the device's position's offset in the buffer */
};

/*
 * One direction of an open. `ring` holds `cap` bytes, DSP_RING_MAX for
 * playback from the open, or more once a layout needs them, and
 * DSP_BUFFER_MAX for capture, of which the first `size` are the buffer in
 * use, a circle of `used` bytes from `head` on; it is NULL in a direction
 * the open does not have. The program adds at the tail and the driver's
 * service takes from the head (playback), or the driver adds and the
 * program takes (capture), each under the open's lock.
 * While `running` the driver's stream is started. `gone` is set, under the
 * open's lock, when the driver's device went (`dsp_gone`): the direction
 * then moves no more, and every call on the open fails with ENODEV, as on a
 * Linux card that snd_card_disconnect took away.
 */
struct dsp_chan {
    audio_stream_t stream; /* the driver's view of the direction */
    audio_driver_t *drv;   /* the driver of the stream */
    uint8_t *ring;
    uint32_t cap;
    uint32_t size;
    uint32_t head;
    uint32_t used;
    int running;
    uint32_t blocks; /* periods ended since GETOPTR or GETIPTR last read them */
    uint32_t bytes;  /* bytes the stream moved before its last start */
    int gone;
};

/*
 * An open of /dev/dsp: `play` when it was opened for writing, `rec` when for
 * reading, both for O_RDWR. A write starts playback once `start` bytes are
 * queued, enough to fill the driver's own buffer, so that no silence follows
 * them; `given` counts the frames the driver has taken since the start,
 * silence among them, and `data_end` is `given` just after the last frame of
 * data, so the data has played once the stream's position reaches it.
 * Capture starts at the first read or poll for input.
 */
struct dsp {
    struct dsp_chan play;
    struct dsp_chan rec;
    struct spinlock lock;
    wait_queue_t wq;   /* readers, writers, pollers and a drain */
    int trigger;       /* PCM_ENABLE_INPUT and PCM_ENABLE_OUTPUT */
    uint32_t fragment; /* SETFRAGMENT's argument; 0 until one */
    uint32_t start;
    uint32_t given;
    uint32_t data_end;
};

int dspdev_present(void)
{
    return audio && audio->stream_open;
}

/** The bytes of one frame of `s`. */
static uint32_t frame_bytes(const audio_stream_t *s)
{
    return (uint32_t)s->channels * AUDIO_SAMPLE_BYTES(s->format);
}

/** The byte that is silence in `format`: the middle of unsigned 8-bit. */
static int silence(audio_format_t format)
{
    return format == AUDIO_U8 ? 0x80 : 0;
}

/** `v` held between `lo` and `hi`. */
static uint32_t clamp(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/** The power of two nearest `v`, which is at least 1. */
static uint32_t nearest_pow2(uint32_t v)
{
    uint32_t p = 1;

    while (p <= v / 2) {
        p *= 2;
    }
    return v - p > 2 * p - v ? 2 * p : p;
}

/** The direction whose settings the ioctls read back: playback's when the
 * open has it, else capture's. */
static struct dsp_chan *main_chan(struct dsp *d)
{
    return d->play.ring ? &d->play : &d->rec;
}

/** Add `n` bytes from `src` at the buffer's tail; the open's lock held, `n`
 * free. */
static void ring_put(struct dsp_chan *c, const uint8_t *src, uint32_t n)
{
    uint32_t tail = c->head + c->used;
    uint32_t first;

    if (tail >= c->size) {
        tail -= c->size;
    }
    first = n < c->size - tail ? n : c->size - tail;
    k_memcpy(c->ring + tail, src, first);
    k_memcpy(c->ring, src + first, n - first);
    c->used += n;
}

/** Take `n` bytes from the buffer's head into `dst`; the open's lock held,
 * `n` queued. */
static void ring_get(struct dsp_chan *c, uint8_t *dst, uint32_t n)
{
    uint32_t first = n < c->size - c->head ? n : c->size - c->head;

    k_memcpy(dst, c->ring + c->head, first);
    k_memcpy(dst + first, c->ring, n - first);
    c->head += n;
    if (c->head >= c->size) {
        c->head -= c->size;
    }
    c->used -= n;
}

/**
 * Playback's `transfer`, in the driver's service: fill `size` bytes at
 * `buf` with the whole frames queued, and the rest with silence. A partial
 * frame stays for the write that completes it.
 *
 * A take that leaves a whole period free wakes the writers and pollers, as
 * Linux's pointer update wakes them once `avail_min` frames are free
 * (snd_pcm_update_state, sound/core/pcm_lib.c), whether or not those frames
 * have played. The mix takes a channel's frames up to the device's buffer
 * ahead of play, so a writer woken only by `period` finds the buffer
 * emptied, and the mix fills the gap with silence.
 */
static void dsp_transfer(audio_stream_t *s, uint8_t *buf, size_t size)
{
    struct dsp *d = (struct dsp *)s->owner;
    struct dsp_chan *c = &d->play;
    uint32_t frame = frame_bytes(s);
    uint32_t flags, n, room;
    int wake;

    flags = spin_lock_irqsave(&d->lock);
    n = c->used - c->used % frame;
    if (n > size) {
        n = (uint32_t)size;
    }
    room = c->size - c->used;
    ring_get(c, buf, n);
    wake = room < s->period_bytes && room + n >= s->period_bytes;
    if (n > 0) {
        d->data_end = d->given + n / frame;
    }
    d->given += (uint32_t)(size / frame);
    spin_unlock_irqrestore(&d->lock, flags);
    k_memset(buf + n, silence(s->format), size - n);
    if (wake) {
        wq_wake(&d->wq, WQ_ROOM);
    }
}

/**
 * Capture's `transfer`, in the driver's service: add the `size` bytes
 * recorded at `buf` to the buffer. What does not fit is dropped in whole
 * frames, the newest first, so the program reads the oldest it missed none
 * of; the buffer's tail stays on a frame's edge, even after a read that took
 * a partial frame from its head.
 */
static void dsp_record(audio_stream_t *s, uint8_t *buf, size_t size)
{
    struct dsp *d = (struct dsp *)s->owner;
    uint32_t flags, n;

    flags = spin_lock_irqsave(&d->lock);
    n = d->rec.size - d->rec.used;
    if (n > size) {
        n = (uint32_t)size;
    }
    n -= n % frame_bytes(s);
    ring_put(&d->rec, buf, n);
    spin_unlock_irqrestore(&d->lock, flags);
}

/** The streams' `gone`, from the driver when its device goes: mark the
 * direction gone and wake every waiter, the readers, writers, pollers and
 * a drain, whose waits then end. */
static void dsp_gone(audio_stream_t *s)
{
    struct dsp *d = (struct dsp *)s->owner;
    struct dsp_chan *c = s->dir == AUDIO_CAPTURE ? &d->rec : &d->play;
    uint32_t flags;

    flags = spin_lock_irqsave(&d->lock);
    c->gone = 1;
    spin_unlock_irqrestore(&d->lock, flags);
    wq_wake(&d->wq, WQ_ALL);
}

/** Whether a direction of `d` has gone with its device. */
static int dsp_is_gone(const struct dsp *d)
{
    return d->play.gone || d->rec.gone;
}

/** The streams' `period`, in the driver's service: count the period and
 * wake the readers or writers, the pollers and a drain. */
static void dsp_period(audio_stream_t *s)
{
    struct dsp *d = (struct dsp *)s->owner;
    struct dsp_chan *c = s->dir == AUDIO_CAPTURE ? &d->rec : &d->play;
    uint32_t flags;

    flags = spin_lock_irqsave(&d->lock);
    c->blocks++;
    spin_unlock_irqrestore(&d->lock, flags);
    wq_wake(&d->wq, s->dir == AUDIO_CAPTURE ? WQ_DATA : WQ_ROOM);
}

/**
 * Lay direction `c` out for its stream's format, channels and rate and for
 * SETFRAGMENT's request, and have the driver grant it. The stream must be
 * stopped and the buffer empty. A playback buffer holds the driver's
 * `device_periods` periods, which the first start takes, and at least one
 * more, since a program may keep a period free and still wait for the start,
 * as cmus does, which writes only as much as GETOSPACE's free periods less
 * one; a capture buffer needs only two. A period holds one frame at least,
 * the smallest power of two that does where SETFRAGMENT asks for less.
 * Where the driver's periods and one more pass the playback buffer's room,
 * as for a stream of many channels at a high rate that the driver converts,
 * the buffer grows to hold them, as Linux's OSS layer sizes its buffer from
 * the hardware's in the program's frames (snd_pcm_oss_period_size). 0; or
 * the driver's error, -EIO for a grant outside the contract of
 * `stream_setup`, or -ENOMEM when the larger buffer cannot be had, with the
 * stream and the buffer as they were.
 */
static int dsp_layout(struct dsp *d, struct dsp_chan *c)
{
    audio_stream_t *s = &c->stream;
    audio_stream_t old = c->stream;
    int play = c == &d->play;
    uint32_t bytes, count, frame, size = 0, flags;
    uint8_t *ring = NULL, *was = NULL;
    int rc;

    if (d->fragment) {
        /* The low half is the period's log2, the high half the count. */
        bytes = (d->fragment & 0xFFFF) >= 15 ? DSP_PERIOD_MAX : 1u << (d->fragment & 0xFFFF);
        count = d->fragment >> 16;
    } else {
        bytes = nearest_pow2(s->rate * frame_bytes(s) / DSP_PERIOD_HZ);
        count = DSP_PERIODS;
    }
    bytes = clamp(bytes, DSP_PERIOD_MIN, DSP_PERIOD_MAX);
    /* A period holds a frame, of DSP_CHANNELS_MAX of 4 bytes at most. */
    while (bytes < frame_bytes(s)) {
        bytes <<= 1;
    }
    s->period_bytes = bytes;
    s->periods = clamp(count, 2, DSP_BUFFER_MAX / bytes);
    rc = c->drv->stream_setup(s);
    frame = frame_bytes(s);
    if (rc == 0 && (s->period_bytes == 0 || s->period_bytes > bytes ||
                    s->period_bytes % frame != 0 || s->device_periods == 0 ||
                    (play && s->device_periods > AUDIO_DEVICE_MAX / s->period_bytes))) {
        rc = -EIO;
    }
    if (rc == 0) {
        s->periods = clamp(s->periods, 2, DSP_BUFFER_MAX / s->period_bytes);
        if (play && s->periods <= s->device_periods) {
            s->periods = s->device_periods + 1;
        }
        size = s->periods * s->period_bytes;
        if (size > c->cap) {
            ring = (uint8_t *)kmalloc(size);
            rc = ring ? 0 : -ENOMEM;
        }
    }
    if (rc != 0) {
        c->stream = old;
        return rc;
    }
    flags = spin_lock_irqsave(&d->lock);
    if (ring) {
        was = c->ring;
        c->ring = ring;
        c->cap = size;
    }
    c->size = size;
    spin_unlock_irqrestore(&d->lock, flags);
    kfree(was);
    if (play) {
        d->start = s->device_periods * s->period_bytes;
    }
    return 0;
}

/** Lay out every direction the open has: 0, or the first refusal's error.
 * The caller puts the streams back and lays them out again after a refusal,
 * since a direction laid out before it has the new layout. */
static int dsp_layout_all(struct dsp *d)
{
    int rc = 0;

    if (d->play.ring) {
        rc = dsp_layout(d, &d->play);
    }
    if (rc == 0 && d->rec.ring) {
        rc = dsp_layout(d, &d->rec);
    }
    return rc;
}

/** Put back the streams `old` saved, playback's first, and lay them out
 * again, after a refused change. */
static void dsp_restore(struct dsp *d, const audio_stream_t *old)
{
    d->play.stream = old[0];
    d->rec.stream = old[1];
    (void)dsp_layout_all(d);
}

/** Start playback if it is stopped, the trigger allows it and a frame is
 * queued: 0, or the driver's error. */
static int dsp_start(struct dsp *d)
{
    struct dsp_chan *c = &d->play;
    int rc;

    if (c->gone) {
        return -ENODEV;
    }
    if (!c->ring || c->running || !(d->trigger & PCM_ENABLE_OUTPUT) ||
        c->used < frame_bytes(&c->stream)) {
        return 0;
    }
    d->given = 0;
    d->data_end = 0;
    c->running = 1;
    rc = c->drv->stream_start(&c->stream);
    if (rc != 0) {
        c->running = 0;
    }
    return rc;
}

/** Start capture if it is stopped and the trigger allows it: 0, or the
 * driver's error. */
static int dsp_record_start(struct dsp *d)
{
    struct dsp_chan *c = &d->rec;
    int rc;

    if (c->gone) {
        return -ENODEV;
    }
    if (!c->ring || c->running || !(d->trigger & PCM_ENABLE_INPUT)) {
        return 0;
    }
    c->running = 1;
    rc = c->drv->stream_start(&c->stream);
    if (rc != 0) {
        c->running = 0;
    }
    return rc;
}

/** Stop direction `c` if it runs. Playback loses what the driver took and
 * has not played, and capture what the driver has not handed over; what the
 * buffer holds stays. A direction gone with its device asks its driver
 * nothing. */
static void dsp_halt(struct dsp *d, struct dsp_chan *c)
{
    if (c->gone) {
        c->running = 0;
    }
    if (!c->running) {
        return;
    }
    c->bytes += c->drv->stream_position(&c->stream) * frame_bytes(&c->stream);
    c->drv->stream_stop(&c->stream);
    c->running = 0;
    wq_wake(&d->wq, c == &d->rec ? WQ_DATA : WQ_ROOM);
}

/** Stop direction `c` and empty its buffer. */
static void dsp_flush(struct dsp *d, struct dsp_chan *c)
{
    uint32_t flags;

    dsp_halt(d, c);
    flags = spin_lock_irqsave(&d->lock);
    c->head = 0;
    c->used = 0;
    c->blocks = 0;
    spin_unlock_irqrestore(&d->lock, flags);
}

/** Stop both directions and empty both buffers: SNDCTL_DSP_RESET. */
static void dsp_reset(struct dsp *d)
{
    dsp_flush(d, &d->play);
    dsp_flush(d, &d->rec);
}

/** The wait condition of a write: room in the buffer, or the device gone. */
static int dsp_writable(void *arg)
{
    struct dsp *d = (struct dsp *)arg;

    return d->play.used < d->play.size || d->play.gone;
}

/** The wait condition of a read: a byte in the buffer, or the device gone. */
static int dsp_readable(void *arg)
{
    struct dsp *d = (struct dsp *)arg;

    return d->rec.used > 0 || d->rec.gone;
}

/** The wait condition of a drain: playback stopped or gone, or no whole
 * frame queued and the position past the last frame of data. */
static int dsp_drained(void *arg)
{
    struct dsp *d = (struct dsp *)arg;
    struct dsp_chan *c = &d->play;

    if (!c->running || c->gone) {
        return 1;
    }
    if (c->used >= frame_bytes(&c->stream)) {
        return 0;
    }
    return (int32_t)(c->drv->stream_position(&c->stream) - d->data_end) >= 0;
}

/**
 * Play what is queued and wait until it has played, then stop playback and
 * drop a partial frame left over: SNDCTL_DSP_SYNC. With the trigger off, or
 * in an open with no playback, nothing plays and nothing is dropped. 0,
 * -EINTR when a signal ends the wait, which leaves the stream running,
 * -ENODEV when the device has gone, or the driver's error.
 */
static int32_t dsp_drain(struct dsp *d)
{
    int rc = dsp_start(d);

    if (rc != 0) {
        return rc;
    }
    if (wq_wait_event(&d->wq, dsp_drained, d) < 0) {
        return -EINTR;
    }
    if (d->play.gone) {
        d->play.running = 0;
        return -ENODEV;
    }
    if (d->play.running) {
        dsp_flush(d, &d->play);
    }
    return 0;
}

/**
 * Ask for a new format, channel count and rate in every direction. A change
 * stops the streams and empties the buffers, as OSS's does, and lays them
 * out again; when the driver grants nothing near the request the old
 * parameters and layout stay. 0, or the driver's error.
 */
static int dsp_configure(struct dsp *d, audio_format_t format, int channels, uint32_t rate)
{
    audio_stream_t old[2];
    struct dsp_chan *c[2];
    int i, rc;

    c[0] = &d->play;
    c[1] = &d->rec;
    for (i = 0; i < 2; i++) {
        if (c[i]->ring && (c[i]->stream.format != format || c[i]->stream.channels != channels ||
                           c[i]->stream.rate != rate)) {
            break;
        }
    }
    if (i == 2) {
        return 0;
    }
    dsp_reset(d);
    for (i = 0; i < 2; i++) {
        old[i] = c[i]->stream;
        c[i]->stream.format = format;
        c[i]->stream.channels = channels;
        c[i]->stream.rate = rate;
    }
    rc = dsp_layout_all(d);
    if (rc != 0) {
        dsp_restore(d, old);
    }
    return rc;
}

/** Set direction `c` of the new open `d` up as `dir`, at OSS's defaults,
 * over the buffer `ring` of `cap` bytes, on the stream operations of `drv`. */
static void dsp_chan_init(struct dsp *d, struct dsp_chan *c, audio_dir_t dir, uint8_t *ring,
                          uint32_t cap, audio_driver_t *drv)
{
    c->drv = drv;
    c->ring = ring;
    c->cap = cap;
    c->stream.dir = dir;
    c->stream.format = AUDIO_U8;
    c->stream.channels = 1;
    c->stream.rate = 8000;
    c->stream.transfer = dir == AUDIO_CAPTURE ? dsp_record : dsp_transfer;
    c->stream.period = dsp_period;
    c->stream.gone = dsp_gone;
    c->stream.owner = d;
}

/** Take the driver's stream of direction `c` and lay it out: 0, or the
 * error, with the stream not taken. */
static int dsp_take_chan(struct dsp *d, struct dsp_chan *c)
{
    int rc = c->drv->stream_open(&c->stream);

    if (rc == 0) {
        rc = dsp_layout(d, c);
        if (rc != 0) {
            c->drv->stream_close(&c->stream);
        }
    }
    return rc;
}

/**
 * Take the driver's stream of each direction `d` has and lay it out: 0, or
 * the first error, with none of them taken. An open of both directions
 * goes on without one the device lacks, whose `stream_open` is -ENODEV,
 * as Linux's snd_pcm_oss_open_file opens only the directions a PCM has:
 * its buffer is left out of `d`, which is the caller's to free, and the
 * open has the other direction alone. It fails when the device lacks both.
 */
static int dsp_take(struct dsp *d)
{
    int both = d->play.ring && d->rec.ring;
    int rc = 0;

    if (d->play.ring) {
        rc = dsp_take_chan(d, &d->play);
        if (rc == -ENODEV && both) {
            d->play.ring = NULL;
            rc = 0;
        }
        if (rc != 0) {
            return rc;
        }
    }
    if (d->rec.ring) {
        rc = dsp_take_chan(d, &d->rec);
        if (rc == -ENODEV && both && d->play.ring) {
            d->rec.ring = NULL;
            rc = 0;
        }
        if (rc != 0 && d->play.ring) {
            d->play.drv->stream_close(&d->play.stream);
        }
    }
    return rc;
}

int32_t dspdev_open(fd_entry_t *e, int flags)
{
    if (!dspdev_present()) {
        return -ENODEV;
    }
    return dspdev_open_driver(e, flags, &vchan_driver);
}

int32_t dspdev_open_driver(fd_entry_t *e, int flags, audio_driver_t *drv)
{
    int mode = flags & 3; /* O_RDONLY, O_WRONLY or O_RDWR */
    struct dsp *d;
    uint8_t *out = NULL, *in = NULL;
    int rc;

    if (mode == 3) {
        return -EINVAL;
    }
    d = (struct dsp *)kzalloc(sizeof(*d));
    if (mode != 0) {
        out = (uint8_t *)kmalloc(DSP_RING_MAX);
    }
    if (mode != 1) {
        in = (uint8_t *)kmalloc(DSP_BUFFER_MAX);
    }
    if (!d || (mode != 0 && !out) || (mode != 1 && !in)) {
        kfree(out);
        kfree(in);
        kfree(d);
        return -ENOMEM;
    }
    wait_queue_init(&d->wq);
    spin_lock_init(&d->lock, "dsp");
    d->trigger = PCM_ENABLE_INPUT | PCM_ENABLE_OUTPUT;
    if (out) {
        dsp_chan_init(d, &d->play, AUDIO_PLAYBACK, out, DSP_RING_MAX, drv);
    }
    if (in) {
        dsp_chan_init(d, &d->rec, AUDIO_CAPTURE, in, DSP_BUFFER_MAX, drv);
    }
    rc = dsp_take(d);
    if (rc != 0) {
        /* A layout may have put a larger buffer in place of `out`. */
        kfree(d->play.ring ? d->play.ring : out);
        kfree(d->rec.ring ? d->rec.ring : in);
        kfree(d);
        return rc;
    }
    if (!d->play.ring) {
        kfree(out);
    }
    if (!d->rec.ring) {
        kfree(in);
    }
    e->priv = d;
    return 0;
}

int32_t dspdev_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct dsp *d = (struct dsp *)e->priv;
    struct dsp_chan *c = &d->play;
    uint8_t chunk[DSP_CHUNK];
    uint32_t done = 0;
    uint32_t flags, n;
    int rc;

    if (!c->ring) {
        return -EBADF;
    }
    while (done < count) {
        if (c->gone) {
            return done ? (int32_t)done : -ENODEV;
        }
        if (c->used >= c->size) {
            rc = dsp_start(d);
            if (rc != 0) {
                return done ? (int32_t)done : rc;
            }
            if (e->ofd->status_flags & OFD_NONBLOCK) {
                return done ? (int32_t)done : -EAGAIN;
            }
            if (wq_wait_event(&d->wq, dsp_writable, d) < 0) {
                return done ? (int32_t)done : -EINTR;
            }
            continue;
        }
        n = count - done < DSP_CHUNK ? count - done : DSP_CHUNK;
        if (copy_from_user(chunk, buf + done, n) != 0) {
            return done ? (int32_t)done : -EFAULT;
        }
        flags = spin_lock_irqsave(&d->lock);
        if (n > c->size - c->used) {
            n = c->size - c->used;
        }
        ring_put(c, chunk, n);
        spin_unlock_irqrestore(&d->lock, flags);
        done += n;
        if (c->used >= d->start) {
            rc = dsp_start(d);
            if (rc != 0) {
                return done ? (int32_t)done : rc;
            }
        }
    }
    return (int32_t)done;
}

int32_t dspdev_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct dsp *d = (struct dsp *)e->priv;
    struct dsp_chan *c = &d->rec;
    uint8_t chunk[DSP_CHUNK];
    uint32_t done = 0;
    uint32_t flags, n;
    int rc;

    if (!c->ring) {
        return -EBADF;
    }
    while (done < count) {
        if (c->gone) {
            return done ? (int32_t)done : -ENODEV;
        }
        rc = dsp_record_start(d);
        if (rc != 0) {
            return done ? (int32_t)done : rc;
        }
        if (c->used == 0) {
            if (e->ofd->status_flags & OFD_NONBLOCK) {
                return done ? (int32_t)done : -EAGAIN;
            }
            if (wq_wait_event(&d->wq, dsp_readable, d) < 0) {
                return done ? (int32_t)done : -EINTR;
            }
            continue;
        }
        n = count - done < DSP_CHUNK ? count - done : DSP_CHUNK;
        flags = spin_lock_irqsave(&d->lock);
        if (n > c->used) {
            n = c->used;
        }
        ring_get(c, chunk, n);
        spin_unlock_irqrestore(&d->lock, flags);
        if (copy_to_user(buf + done, chunk, n) != 0) {
            return done ? (int32_t)done : -EFAULT;
        }
        done += n;
    }
    return (int32_t)done;
}

short dspdev_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct dsp *d = (struct dsp *)e->priv;
    short ready = 0;

    *wq = &d->wq;
    if (dsp_is_gone(d)) {
        return POLLERR | POLLNVAL;
    }
    if ((events & POLLIN) && d->rec.ring) {
        (void)dsp_record_start(d);
        if (d->rec.used >= d->rec.stream.period_bytes) {
            ready |= POLLIN;
        }
    }
    if ((events & POLLOUT) && d->play.ring &&
        d->play.size - d->play.used >= d->play.stream.period_bytes) {
        ready |= POLLOUT;
    }
    return ready;
}

/** Read the int at the user's `arg` into `value`: 0, or -EFAULT. */
static int32_t int_in(uintptr_t arg, int *value)
{
    return copy_from_user(value, (const void __user *)arg, sizeof(*value)) != 0 ? -EFAULT : 0;
}

/** Copy the int `value` out to the user's `arg`: 0, or -EFAULT. */
static int32_t int_out(uintptr_t arg, int value)
{
    return copy_to_user((void __user *)arg, &value, sizeof(value)) != 0 ? -EFAULT : 0;
}

/** The stream's format as OSS's AFMT_ bit. */
static int afmt(const audio_stream_t *s)
{
    switch (s->format) {
    case AUDIO_S16LE:
        return AFMT_S16_LE;
    case AUDIO_S24LE:
        return AFMT_S24_LE;
    case AUDIO_S24_3LE:
        return AFMT_S24_PACKED;
    case AUDIO_S32LE:
        return AFMT_S32_LE;
    case AUDIO_U8:
    default:
        return AFMT_U8;
    }
}

/** The format of OSS's AFMT_ bit `value`, which DSP_FORMATS has. */
static audio_format_t format_of(int value)
{
    switch (value) {
    case AFMT_S16_LE:
        return AUDIO_S16LE;
    case AFMT_S24_LE:
        return AUDIO_S24LE;
    case AFMT_S24_PACKED:
        return AUDIO_S24_3LE;
    case AFMT_S32_LE:
        return AUDIO_S32LE;
    default:
        return AUDIO_U8;
    }
}

/**
 * The ioctls that set the format, channels and rate: each asks for its
 * value in every direction and reads back what `main_chan` was granted.
 *
 * A query, SPEED or CHANNELS of 0 or less and SETFMT of AFMT_QUERY or a
 * format it lacks, which keeps the one set as FreeBSD's does, answers from
 * `main_chan` and configures nothing: on a duplex open capture's grant can
 * differ from playback's, and asking for playback's values in both
 * directions would reset the streams and drop their buffered data.
 * CHANNELS asks for at most the driver's `channels_max`, and fails with
 * -EINVAL above DSP_CHANNELS_MAX, as Linux's snd_pcm_oss_set_channels does.
 */
static int32_t dsp_set(struct dsp *d, uint32_t request, uintptr_t arg)
{
    audio_stream_t *s = &main_chan(d)->stream;
    audio_format_t format = s->format;
    int channels = s->channels;
    uint32_t rate = s->rate;
    int value, max, query = 0;
    int32_t rc;

    rc = int_in(arg, &value);
    if (rc != 0) {
        return rc;
    }
    switch (request) {
    case DSP_SPEED:
        query = value <= 0;
        rate = query ? rate : clamp((uint32_t)value, DSP_RATE_MIN, DSP_RATE_MAX);
        break;
    case DSP_STEREO:
        channels = value ? 2 : 1;
        break;
    case DSP_CHANNELS:
        if (value > DSP_CHANNELS_MAX) {
            return -EINVAL;
        }
        query = value <= 0;
        max = main_chan(d)->drv->channels_max > 2 ? main_chan(d)->drv->channels_max : 2;
        channels = query ? channels : value > max ? max : value;
        break;
    default: /* DSP_SETFMT */
        /* One bit of DSP_FORMATS sets the format; anything else is a query. */
        query = value <= 0 || (value & (value - 1)) != 0 || (value & DSP_FORMATS) == 0;
        format = query ? format : format_of(value);
        break;
    }
    rc = query ? 0 : dsp_configure(d, format, channels, rate);
    if (rc != 0) {
        return rc;
    }
    switch (request) {
    case DSP_SPEED:
        return int_out(arg, (int)s->rate);
    case DSP_STEREO:
        return int_out(arg, s->channels - 1);
    case DSP_CHANNELS:
        return int_out(arg, s->channels);
    default:
        return int_out(arg, afmt(s));
    }
}

/** SETFRAGMENT: stop the streams, empty the buffers and lay them out for
 * the request `value`, or keep the old request when the driver refuses it. */
static int32_t dsp_fragment(struct dsp *d, uint32_t value)
{
    audio_stream_t old[2];
    uint32_t was = d->fragment;
    int rc;

    dsp_reset(d);
    old[0] = d->play.stream;
    old[1] = d->rec.stream;
    d->fragment = value;
    rc = dsp_layout_all(d);
    if (rc != 0) {
        d->fragment = was;
        dsp_restore(d, old);
    }
    return rc;
}

/** GETOSPACE's and GETISPACE's answer for direction `c`: the room to write
 * or the data to read, in periods and bytes. */
static int32_t dsp_space(struct dsp_chan *c, uint32_t bytes, uintptr_t arg)
{
    struct dsp_buf_info info;

    if (!c->ring) {
        return -EINVAL;
    }
    info.fragments = (int)(bytes / c->stream.period_bytes);
    info.fragstotal = (int)c->stream.periods;
    info.fragsize = (int)c->stream.period_bytes;
    info.bytes = (int)bytes;
    return copy_to_user((void __user *)arg, &info, sizeof(info)) != 0 ? -EFAULT : 0;
}

/** GETOPTR's and GETIPTR's answer for direction `c`: the bytes played or
 * recorded since the open, the periods ended since the last call, and the
 * device's position's offset in the buffer. */
static int32_t dsp_ptr(struct dsp *d, struct dsp_chan *c, uintptr_t arg)
{
    struct dsp_count_info info;
    uint32_t frame, pos = 0;
    uint32_t flags;

    if (!c->ring) {
        return -EINVAL;
    }
    frame = frame_bytes(&c->stream);
    if (c->running) {
        pos = c->drv->stream_position(&c->stream);
    }
    flags = spin_lock_irqsave(&d->lock);
    info.blocks = (int)c->blocks;
    c->blocks = 0;
    spin_unlock_irqrestore(&d->lock, flags);
    info.bytes = (int)(c->bytes + pos * frame);
    info.ptr = (int)(pos % (c->size / frame) * frame);
    return copy_to_user((void __user *)arg, &info, sizeof(info)) != 0 ? -EFAULT : 0;
}

/** GETODELAY's answer: the bytes queued ahead of the next one written, the
 * buffer's and the driver's. */
static int32_t dsp_odelay(struct dsp *d, uintptr_t arg)
{
    struct dsp_chan *c = &d->play;
    uint32_t bytes = c->used;

    if (!c->ring) {
        return -EINVAL;
    }
    if (c->running) {
        bytes += c->drv->delay(&c->stream) * frame_bytes(&c->stream);
    }
    return int_out(arg, (int)bytes);
}

/** SETTRIGGER: start or stop each direction as `value`'s bits say. 0, or
 * the driver's error. */
static int32_t dsp_trigger(struct dsp *d, int value)
{
    int rc;

    d->trigger = value & (PCM_ENABLE_INPUT | PCM_ENABLE_OUTPUT);
    if (!(d->trigger & PCM_ENABLE_OUTPUT)) {
        dsp_halt(d, &d->play);
    }
    if (!(d->trigger & PCM_ENABLE_INPUT)) {
        dsp_halt(d, &d->rec);
    }
    rc = dsp_start(d);
    if (rc == 0) {
        rc = dsp_record_start(d);
    }
    return rc;
}

/** The trigger bits of the directions `d` has. */
static int dsp_triggers(struct dsp *d)
{
    return d->trigger &
           ((d->play.ring ? PCM_ENABLE_OUTPUT : 0) | (d->rec.ring ? PCM_ENABLE_INPUT : 0));
}

int32_t dspdev_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct dsp *d = (struct dsp *)e->priv;
    int duplex = d->play.ring && d->rec.ring;
    int value;
    int32_t rc;

    if (dsp_is_gone(d)) {
        return -ENODEV;
    }
    switch (request) {
    case DSP_SPEED:
    case DSP_STEREO:
    case DSP_CHANNELS:
    case DSP_SETFMT:
        return dsp_set(d, request, arg);
    case DSP_GETFMTS:
        return int_out(arg, DSP_FORMATS);
    case DSP_GETBLKSIZE:
        return int_out(arg, (int)main_chan(d)->stream.period_bytes);
    case DSP_SETFRAGMENT:
        rc = int_in(arg, &value);
        if (rc != 0) {
            return rc;
        }
        return dsp_fragment(d, (uint32_t)value);
    case DSP_GETOSPACE:
        return dsp_space(&d->play, d->play.size - d->play.used, arg);
    case DSP_GETISPACE:
        return dsp_space(&d->rec, d->rec.used, arg);
    case DSP_GETODELAY:
        return dsp_odelay(d, arg);
    case DSP_GETOPTR:
        return dsp_ptr(d, &d->play, arg);
    case DSP_GETIPTR:
        return dsp_ptr(d, &d->rec, arg);
    case DSP_GETCAPS:
        return int_out(arg, DSP_CAP_REVISION | DSP_CAP_REALTIME | DSP_CAP_TRIGGER |
                                (duplex ? DSP_CAP_DUPLEX : 0));
    case DSP_SETDUPLEX:
        return duplex ? 0 : -EINVAL;
    case DSP_RESET:
        dsp_reset(d);
        return 0;
    case DSP_SYNC:
        return d->play.ring ? dsp_drain(d) : 0;
    case DSP_POST:
        return dsp_start(d);
    case DSP_NONBLOCK:
        e->ofd->status_flags |= OFD_NONBLOCK;
        return 0;
    case DSP_GETTRIGGER:
        return int_out(arg, dsp_triggers(d));
    case DSP_SETTRIGGER:
        rc = int_in(arg, &value);
        if (rc != 0) {
            return rc;
        }
        return dsp_trigger(d, value);
    case OSS_GETVERSION:
        return int_out(arg, OSS_VERSION);
    default:
        break;
    }
    return ((request >> 8) & 0xFFu) == 'P' ? -EINVAL : -ENOTTY;
}

void dspdev_close(fd_entry_t *e)
{
    struct dsp *d = (struct dsp *)e->priv;

    if (!d) {
        return;
    }
    if (d->play.ring && !(e->ofd->status_flags & OFD_NONBLOCK)) {
        (void)dsp_drain(d);
    }
    dsp_halt(d, &d->play);
    dsp_halt(d, &d->rec);
    if (d->play.ring) {
        d->play.drv->stream_close(&d->play.stream);
    }
    if (d->rec.ring) {
        d->rec.drv->stream_close(&d->rec.stream);
    }
    e->priv = NULL;
    kfree(d->play.ring);
    kfree(d->rec.ring);
    kfree(d);
}
