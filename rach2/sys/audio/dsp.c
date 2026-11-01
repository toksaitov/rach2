/*
 * dsp.c: /dev/dsp (dsp.h), OSS's audio device: a buffer of periods that
 * write fills and the driver's playback stream empties, and OSS's ioctls.
 */

#include "dsp.h"

#include "audio.h"

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
 * and _IO for a command. SNDCTL_DSP_SAMPLESIZE is SETFMT's number.
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
#define DSP_NONBLOCK    0x0000500Eu
#define DSP_GETCAPS     0x8004500Fu
#define DSP_GETTRIGGER  0x80045010u
#define DSP_SETTRIGGER  0x40045010u
#define DSP_GETOPTR     0x800C5012u
#define DSP_GETODELAY   0x80045017u
#define OSS_GETVERSION  0x80044D76u

#define AFMT_QUERY        0x00
#define AFMT_U8           0x08
#define AFMT_S16_LE       0x10
#define DSP_CAP_REVISION  1
#define DSP_CAP_REALTIME  0x200
#define DSP_CAP_TRIGGER   0x1000
#define PCM_ENABLE_OUTPUT 2
#define OSS_VERSION       0x030802 /* SOUND_VERSION, as Linux gives it */

/*
 * The buffer. Its periods are powers of two from DSP_PERIOD_MIN to
 * DSP_PERIOD_MAX bytes, and there are at least two of them, at most as
 * many as DSP_BUFFER_MAX holds. Without SETFRAGMENT a period is the power of
 * two nearest a 43rd of a second, about 23 ms, and there are DSP_PERIODS.
 */
#define DSP_BUFFER_MAX 65536u
#define DSP_PERIOD_MIN 128u
#define DSP_PERIOD_MAX 16384u
#define DSP_PERIOD_HZ  43u
#define DSP_PERIODS    4u
#define DSP_RATE_MIN   1000u
#define DSP_RATE_MAX   192000u
#define DSP_CHUNK      256u /* bytes a write copies from the caller at a time */

/* OSS's `audio_buf_info`, GETOSPACE's answer. */
struct dsp_buf_info {
    int fragments;  /* whole periods free */
    int fragstotal; /* periods in the buffer */
    int fragsize;   /* bytes of a period */
    int bytes;      /* bytes free */
};

/* OSS's `count_info`, GETOPTR's answer. */
struct dsp_count_info {
    int bytes;  /* bytes played since the open, wrapping */
    int blocks; /* periods ended since the last GETOPTR */
    int ptr;    /* the play position's offset in the buffer */
};

/*
 * An open of /dev/dsp. `ring` holds DSP_BUFFER_MAX bytes, of which the first
 * `size` are the buffer in use, a circle of `used` bytes from `head` on. The
 * writer adds at the tail and `dsp_transfer`, in the driver's interrupt,
 * takes from the head, each under `lock`. A write starts the driver's stream
 * once `start` bytes are queued, enough to fill the driver's own buffer, so
 * that no silence follows them. While `running` the stream is started;
 * `given` counts the frames the driver has taken since, silence among them,
 * and `data_end` is `given` just after the last frame of data, so the data
 * has played once the stream's position reaches it.
 */
struct dsp {
    audio_stream_t stream; /* the driver's view of the open */
    audio_driver_t *drv;   /* the driver it was opened on */
    uint8_t *ring;
    uint32_t size;
    uint32_t start;
    uint32_t head;
    uint32_t used;
    struct spinlock lock;
    wait_queue_t wq; /* writers, pollers and a drain; woken at each period */
    int running;
    int trigger;       /* PCM_ENABLE_OUTPUT: the stream may start */
    uint32_t fragment; /* SETFRAGMENT's argument; 0 until one */
    uint32_t given;
    uint32_t data_end;
    uint32_t blocks; /* periods ended since GETOPTR last read them */
    uint32_t played; /* bytes the stream played before its last start */
};

/* The open that has the device, or NULL. There is one open at a time. */
static struct dsp *dsp_owner;

int dspdev_present(void)
{
    return audio && audio->stream_open;
}

/** The bytes of one frame of `s`. */
static uint32_t frame_bytes(const audio_stream_t *s)
{
    return (uint32_t)s->channels * (s->format == AUDIO_S16LE ? 2u : 1u);
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

/** Add `n` bytes from `src` at the buffer's tail; `lock` held, `n` free. */
static void ring_put(struct dsp *d, const uint8_t *src, uint32_t n)
{
    uint32_t tail = d->head + d->used;
    uint32_t first;

    if (tail >= d->size) {
        tail -= d->size;
    }
    first = n < d->size - tail ? n : d->size - tail;
    k_memcpy(d->ring + tail, src, first);
    k_memcpy(d->ring, src + first, n - first);
    d->used += n;
}

/** Take `n` bytes from the buffer's head into `dst`; `lock` held, `n`
 * queued. */
static void ring_get(struct dsp *d, uint8_t *dst, uint32_t n)
{
    uint32_t first = n < d->size - d->head ? n : d->size - d->head;

    k_memcpy(dst, d->ring + d->head, first);
    k_memcpy(dst + first, d->ring, n - first);
    d->head += n;
    if (d->head >= d->size) {
        d->head -= d->size;
    }
    d->used -= n;
}

/**
 * The stream's `transfer`, in the driver's interrupt: fill `size` bytes at
 * `buf` with the whole frames queued, and the rest with silence. A partial
 * frame stays for the write that completes it.
 */
static void dsp_transfer(audio_stream_t *s, uint8_t *buf, size_t size)
{
    struct dsp *d = (struct dsp *)s->owner;
    uint32_t frame = frame_bytes(s);
    uint32_t flags, n;

    flags = spin_lock_irqsave(&d->lock);
    n = d->used - d->used % frame;
    if (n > size) {
        n = (uint32_t)size;
    }
    ring_get(d, buf, n);
    if (n > 0) {
        d->data_end = d->given + n / frame;
    }
    d->given += (uint32_t)(size / frame);
    spin_unlock_irqrestore(&d->lock, flags);
    k_memset(buf + n, silence(s->format), size - n);
}

/** The stream's `period`, in the driver's interrupt: count the period and
 * wake the writers, the pollers and a drain. */
static void dsp_period(audio_stream_t *s)
{
    struct dsp *d = (struct dsp *)s->owner;
    uint32_t flags;

    flags = spin_lock_irqsave(&d->lock);
    d->blocks++;
    spin_unlock_irqrestore(&d->lock, flags);
    wq_wake(&d->wq, WQ_ROOM);
}

/**
 * Lay the buffer out for the stream's format, channels and rate and for
 * SETFRAGMENT's request, and have the driver grant it. The stream must be
 * stopped and the buffer empty. 0; or the driver's error, or -EIO for a
 * grant outside the contract of `stream_setup`, with the stream as it was.
 */
static int dsp_layout(struct dsp *d)
{
    audio_stream_t *s = &d->stream;
    audio_stream_t old = d->stream;
    uint32_t bytes, count, frame;
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
    s->period_bytes = bytes;
    s->periods = clamp(count, 2, DSP_BUFFER_MAX / bytes);
    rc = d->drv->stream_setup(s);
    frame = frame_bytes(s);
    if (rc == 0 &&
        (s->period_bytes == 0 || s->period_bytes > bytes || s->period_bytes % frame != 0 ||
         s->device_periods == 0 || s->device_periods > DSP_BUFFER_MAX / s->period_bytes)) {
        rc = -EIO;
    }
    if (rc != 0) {
        d->stream = old;
        return rc;
    }
    if (s->periods < s->device_periods) {
        s->periods = s->device_periods;
    }
    s->periods = clamp(s->periods, 2, DSP_BUFFER_MAX / s->period_bytes);
    d->size = s->periods * s->period_bytes;
    d->start = s->device_periods * s->period_bytes;
    return 0;
}

/** Start the stream if it is stopped, the trigger allows it and a frame is
 * queued: 0, or the driver's error. */
static int dsp_start(struct dsp *d)
{
    int rc;

    if (d->running || !d->trigger || d->used < frame_bytes(&d->stream)) {
        return 0;
    }
    d->given = 0;
    d->data_end = 0;
    d->running = 1;
    rc = d->drv->stream_start(&d->stream);
    if (rc != 0) {
        d->running = 0;
    }
    return rc;
}

/** Stop the stream if it runs; what the driver took and has not played is
 * lost, and what the buffer holds stays. */
static void dsp_halt(struct dsp *d)
{
    if (!d->running) {
        return;
    }
    d->played += d->drv->stream_position(&d->stream) * frame_bytes(&d->stream);
    d->drv->stream_stop(&d->stream);
    d->running = 0;
    wq_wake(&d->wq, WQ_ROOM);
}

/** Stop the stream and empty the buffer: SNDCTL_DSP_RESET. */
static void dsp_reset(struct dsp *d)
{
    uint32_t flags;

    dsp_halt(d);
    flags = spin_lock_irqsave(&d->lock);
    d->head = 0;
    d->used = 0;
    d->blocks = 0;
    spin_unlock_irqrestore(&d->lock, flags);
}

/** The wait condition of a write: room in the buffer. */
static int dsp_writable(void *arg)
{
    struct dsp *d = (struct dsp *)arg;

    return d->used < d->size;
}

/** The wait condition of a drain: the stream stopped, or no whole frame
 * queued and the position past the last frame of data. */
static int dsp_drained(void *arg)
{
    struct dsp *d = (struct dsp *)arg;

    if (!d->running) {
        return 1;
    }
    if (d->used >= frame_bytes(&d->stream)) {
        return 0;
    }
    return (int32_t)(d->drv->stream_position(&d->stream) - d->data_end) >= 0;
}

/**
 * Play what is queued and wait until it has played, then stop the stream
 * and drop a partial frame left over: SNDCTL_DSP_SYNC. With the trigger
 * off nothing plays and nothing is dropped. 0, -EINTR when a signal ends
 * the wait, which leaves the stream running, or the driver's error.
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
    if (d->running) {
        dsp_reset(d);
    }
    return 0;
}

/**
 * Ask for a new format, channel count and rate. A change stops the stream
 * and empties the buffer, as OSS's does, and lays the buffer out again; when
 * the driver grants nothing near the request the old parameters and layout
 * stay. 0, or the driver's error.
 */
static int dsp_configure(struct dsp *d, audio_format_t format, int channels, uint32_t rate)
{
    audio_stream_t old = d->stream;
    int rc;

    if (format == old.format && channels == old.channels && rate == old.rate) {
        return 0;
    }
    dsp_reset(d);
    d->stream.format = format;
    d->stream.channels = channels;
    d->stream.rate = rate;
    rc = dsp_layout(d);
    if (rc != 0) {
        d->stream = old;
    }
    return rc;
}

int32_t dspdev_open(fd_entry_t *e, int flags)
{
    audio_driver_t *drv = audio;
    struct dsp *d;
    uint8_t *ring;
    int rc;

    if (!dspdev_present()) {
        return -ENODEV;
    }
    if ((flags & 3) != 1) {
        return -ENXIO; /* not O_WRONLY: the device does not record */
    }
    if (dsp_owner) {
        return -EBUSY;
    }
    d = (struct dsp *)kzalloc(sizeof(*d));
    ring = (uint8_t *)kmalloc(DSP_BUFFER_MAX);
    if (!d || !ring || dsp_owner) {
        kfree(d);
        kfree(ring);
        return d && ring ? -EBUSY : -ENOMEM;
    }
    dsp_owner = d;
    d->drv = drv;
    d->ring = ring;
    wait_queue_init(&d->wq);
    d->trigger = 1;
    d->stream.dir = AUDIO_PLAYBACK;
    d->stream.format = AUDIO_U8;
    d->stream.channels = 1;
    d->stream.rate = 8000;
    d->stream.transfer = dsp_transfer;
    d->stream.period = dsp_period;
    d->stream.owner = d;
    rc = drv->stream_open(&d->stream);
    if (rc == 0) {
        rc = dsp_layout(d);
        if (rc != 0) {
            drv->stream_close(&d->stream);
        }
    }
    if (rc != 0) {
        dsp_owner = NULL;
        kfree(ring);
        kfree(d);
        return rc;
    }
    e->priv = d;
    return 0;
}

int32_t dspdev_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct dsp *d = (struct dsp *)e->priv;
    uint8_t chunk[DSP_CHUNK];
    uint32_t done = 0;
    uint32_t flags, n;
    int rc;

    while (done < count) {
        if (d->used >= d->size) {
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
        if (n > d->size - d->used) {
            n = d->size - d->used;
        }
        ring_put(d, chunk, n);
        spin_unlock_irqrestore(&d->lock, flags);
        done += n;
        if (d->used >= d->start) {
            rc = dsp_start(d);
            if (rc != 0) {
                return done ? (int32_t)done : rc;
            }
        }
    }
    return (int32_t)done;
}

short dspdev_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct dsp *d = (struct dsp *)e->priv;

    *wq = &d->wq;
    return (short)(events & (d->size - d->used >= d->stream.period_bytes ? POLLOUT : 0));
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
    return s->format == AUDIO_S16LE ? AFMT_S16_LE : AFMT_U8;
}

/** The ioctls that set the format, channels and rate: each asks for its
 * value and reads back what was granted. */
static int32_t dsp_set(struct dsp *d, uint32_t request, uintptr_t arg)
{
    audio_stream_t *s = &d->stream;
    audio_format_t format = s->format;
    int channels = s->channels;
    uint32_t rate = s->rate;
    int value;
    int32_t rc;

    rc = int_in(arg, &value);
    if (rc != 0) {
        return rc;
    }
    switch (request) {
    case DSP_SPEED:
        if (value > 0) {
            rate = clamp((uint32_t)value, DSP_RATE_MIN, DSP_RATE_MAX);
        }
        break;
    case DSP_STEREO:
        channels = value ? 2 : 1;
        break;
    case DSP_CHANNELS:
        if (value > 0) {
            channels = value > 1 ? 2 : 1;
        }
        break;
    default: /* DSP_SETFMT; another format keeps the one set, as FreeBSD's */
        if (value == AFMT_U8) {
            format = AUDIO_U8;
        } else if (value == AFMT_S16_LE) {
            format = AUDIO_S16LE;
        }
        break;
    }
    rc = dsp_configure(d, format, channels, rate);
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

/** SETFRAGMENT: stop the stream, empty the buffer and lay it out for the
 * request `value`, or keep the old request when the driver refuses it. */
static int32_t dsp_fragment(struct dsp *d, uint32_t value)
{
    uint32_t old = d->fragment;
    int rc;

    dsp_reset(d);
    d->fragment = value;
    rc = dsp_layout(d);
    if (rc != 0) {
        d->fragment = old;
    }
    return rc;
}

/** GETOSPACE's answer: the free space in periods and bytes. */
static int32_t dsp_ospace(struct dsp *d, uintptr_t arg)
{
    struct dsp_buf_info info;
    uint32_t free = d->size - d->used;

    info.fragments = (int)(free / d->stream.period_bytes);
    info.fragstotal = (int)d->stream.periods;
    info.fragsize = (int)d->stream.period_bytes;
    info.bytes = (int)free;
    return copy_to_user((void __user *)arg, &info, sizeof(info)) != 0 ? -EFAULT : 0;
}

/** GETOPTR's answer: the bytes played since the open, the periods ended
 * since the last GETOPTR, and the play position's offset in the buffer. */
static int32_t dsp_optr(struct dsp *d, uintptr_t arg)
{
    struct dsp_count_info info;
    uint32_t frame = frame_bytes(&d->stream);
    uint32_t pos = 0;
    uint32_t flags;

    if (d->running) {
        pos = d->drv->stream_position(&d->stream);
    }
    flags = spin_lock_irqsave(&d->lock);
    info.blocks = (int)d->blocks;
    d->blocks = 0;
    spin_unlock_irqrestore(&d->lock, flags);
    info.bytes = (int)(d->played + pos * frame);
    info.ptr = (int)(pos % (d->size / frame) * frame);
    return copy_to_user((void __user *)arg, &info, sizeof(info)) != 0 ? -EFAULT : 0;
}

/** GETODELAY's answer: the bytes queued ahead of the next one written, the
 * buffer's and the driver's. */
static int32_t dsp_odelay(struct dsp *d, uintptr_t arg)
{
    uint32_t bytes = d->used;

    if (d->running) {
        bytes += d->drv->delay(&d->stream) * frame_bytes(&d->stream);
    }
    return int_out(arg, (int)bytes);
}

int32_t dspdev_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct dsp *d = (struct dsp *)e->priv;
    int value;
    int32_t rc;

    switch (request) {
    case DSP_SPEED:
    case DSP_STEREO:
    case DSP_CHANNELS:
    case DSP_SETFMT:
        return dsp_set(d, request, arg);
    case DSP_GETFMTS:
        return int_out(arg, AFMT_U8 | AFMT_S16_LE);
    case DSP_GETBLKSIZE:
        return int_out(arg, (int)d->stream.period_bytes);
    case DSP_SETFRAGMENT:
        rc = int_in(arg, &value);
        if (rc != 0) {
            return rc;
        }
        return dsp_fragment(d, (uint32_t)value);
    case DSP_GETOSPACE:
        return dsp_ospace(d, arg);
    case DSP_GETODELAY:
        return dsp_odelay(d, arg);
    case DSP_GETOPTR:
        return dsp_optr(d, arg);
    case DSP_GETCAPS:
        return int_out(arg, DSP_CAP_REVISION | DSP_CAP_REALTIME | DSP_CAP_TRIGGER);
    case DSP_RESET:
        dsp_reset(d);
        return 0;
    case DSP_SYNC:
        return dsp_drain(d);
    case DSP_POST:
        return dsp_start(d);
    case DSP_NONBLOCK:
        e->ofd->status_flags |= OFD_NONBLOCK;
        return 0;
    case DSP_GETTRIGGER:
        return int_out(arg, d->trigger ? PCM_ENABLE_OUTPUT : 0);
    case DSP_SETTRIGGER:
        rc = int_in(arg, &value);
        if (rc != 0) {
            return rc;
        }
        d->trigger = (value & PCM_ENABLE_OUTPUT) != 0;
        if (!d->trigger) {
            dsp_halt(d);
            return 0;
        }
        return dsp_start(d);
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
    if (!(e->ofd->status_flags & OFD_NONBLOCK)) {
        (void)dsp_drain(d);
    }
    dsp_halt(d);
    d->drv->stream_close(&d->stream);
    e->priv = NULL;
    dsp_owner = NULL;
    kfree(d->ring);
    kfree(d);
}
