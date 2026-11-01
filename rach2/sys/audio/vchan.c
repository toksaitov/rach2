/*
 * vchan.c: virtual playback channels (vchan.h): every playback open mixed
 * into the device's one stream, each converted to the mix's rate by an
 * integer linear resampler, and the sum clipped to 16 bits.
 */

#include "vchan.h"

#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../lib/spinlock.h"
#include "../mm/kheap.h"

#define VCHAN_RATE_MIN 1000u   /* the lowest rate a channel is granted */
#define VCHAN_RATE_MAX 192000u /* the highest, and at most VCHAN_RATIO times the mix's */
#define VCHAN_RATIO    4u
#define VCHAN_PERIOD   4096u /* bytes of the period the mix asks of the device */
#define VCHAN_BLOCK    256u  /* frames of the mix made at a time */
#define VCHAN_CHUNK    4096u /* frames of the mix a count advances by at a time */
#define VCHAN_WEIGHT   14    /* bits of the resampler's weight between two frames */

/* The most frames a channel gives for a block: VCHAN_RATIO for each frame of
 * the mix, one more for the fraction, and the frame after the last. */
#define VCHAN_STAGE (VCHAN_BLOCK * VCHAN_RATIO + 2u)

/* The most bytes a channel asks its caller to queue before its start: the
 * largest buffer dsp.c lays out. */
#define VCHAN_START_MAX 65536u

/*
 * A channel: the playback stream `s`, which belongs to the caller that
 * opened it. While `running` it is in the mix, which takes its first frame
 * at the first block made after the start, mix frame `at` then.
 *
 * The resampler steps through the channel's frames by its rate for each
 * frame of the mix, R being the mix's rate: `phase` is the distance from
 * `cur` toward the frame after it in 1/R of a frame, and a frame of the mix
 * is `cur` and that frame weighed by the distance. From the end of one block
 * to the start of the next, `phase` may be R or more: the steps past `cur`
 * wait for the frames the next block takes. `nxt` holds the frame after
 * `cur` once a block has taken it.
 *
 * `played` and `frac` count the channel's frames the device has played by
 * mix frame `at`: the whole frames, and the fraction of the next in 1/R. The
 * mix's `period` moves them on.
 */
struct vchan {
    audio_stream_t *s;
    struct vchan *next;
    int running;
    int joined;       /* the mix has taken its first frame since the start */
    uint32_t frame;   /* bytes of one of its frames */
    uint32_t pframes; /* frames of its period */
    uint32_t phase;
    int has_nxt; /* `nxt` holds the frame after `cur` */
    int16_t cur[2];
    int16_t nxt[2];
    uint32_t taken; /* its frames taken by `transfer` since the start */
    uint32_t at;
    uint32_t played;
    uint32_t frac;
    uint32_t mark; /* `played` at the end of its last period */
};

/*
 * The mix: the device's stream, 16-bit stereo at `rate`, between the first
 * channel's open and the last one's close, and the channels open. `made`
 * counts the frames of the mix made since the stream's start, and `ended`
 * those in the periods the device has ended since. `lock` keeps the list,
 * the channels' state and the counts between process context and the
 * device's interrupt.
 */
static struct {
    audio_driver_t *hw; /* the device's driver; NULL while no channel is open */
    audio_stream_t stream;
    uint32_t rate;   /* R */
    uint32_t period; /* frames of the stream's period */
    struct vchan *list;
    int running;
    uint32_t made;
    uint32_t ended;
    struct spinlock lock;
    int32_t sum[VCHAN_BLOCK * 2];    /* a block's sum, left and right */
    uint8_t stage[VCHAN_STAGE * 4u]; /* a channel's frames for a block */
} mix;

/** The bytes of one frame of `s`. */
static uint32_t frame_bytes(const audio_stream_t *s)
{
    return (uint32_t)s->channels * (s->format == AUDIO_S16LE ? 2u : 1u);
}

/** The channel of stream `s`, or NULL. */
static struct vchan *vchan_find(const audio_stream_t *s)
{
    struct vchan *c;

    for (c = mix.list; c; c = c->next) {
        if (c->s == s) {
            return c;
        }
    }
    return NULL;
}

/** Put frame `p` of `s`'s format in `out` as 16-bit stereo: unsigned 8-bit
 * is moved to signed and widened, and a mono sample goes to both. */
static void vchan_frame(const audio_stream_t *s, const uint8_t *p, int16_t *out)
{
    if (s->format == AUDIO_S16LE) {
        out[0] = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        out[1] = s->channels == 2 ? (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8)) : out[0];
    } else {
        out[0] = (int16_t)(((int)p[0] - 128) * 256);
        out[1] = s->channels == 2 ? (int16_t)(((int)p[1] - 128) * 256) : out[0];
    }
}

/**
 * Advance the count of a channel at `rate` by `frames` frames of the mix:
 * `*whole` frames and `*frac` in 1/R of one. The steps of VCHAN_CHUNK keep
 * the product in 32 bits, the kernel having no 64-bit division.
 */
static void vchan_advance(uint32_t *whole, uint32_t *frac, uint32_t frames, uint32_t rate)
{
    uint32_t n;

    while (frames > 0) {
        n = frames < VCHAN_CHUNK ? frames : VCHAN_CHUNK;
        *frac += n * rate;
        *whole += *frac / mix.rate;
        *frac %= mix.rate;
        frames -= n;
    }
}

/** The frames of `c` played by mix frame `h`, which the count kept at
 * `c->at` reaches by `vchan_advance`; with `commit` the count moves to `h`.
 * The lock is held. */
static uint32_t vchan_played(struct vchan *c, uint32_t h, int commit)
{
    uint32_t whole = c->played, frac = c->frac;

    if ((int32_t)(h - c->at) <= 0) {
        return whole;
    }
    vchan_advance(&whole, &frac, h - c->at, c->s->rate);
    if (commit) {
        c->at = h;
        c->played = whole;
        c->frac = frac;
    }
    return whole;
}

/**
 * Add `n` frames of channel `c`, at most VCHAN_BLOCK, to the block's sum.
 * The frames it reads are known before it makes any, as `cur` and the
 * highest a frame of the block interpolates toward, so that one `transfer`
 * takes them all; the caller's silence for frames it lacks is mixed like
 * data. A channel not yet joined takes its first frame and joins at the
 * block. The lock is held.
 */
static void vchan_mix(struct vchan *c, uint32_t n)
{
    audio_stream_t *s = c->s;
    uint32_t rate = s->rate, top, need, k = 0, i, w;
    int32_t *sum = mix.sum;
    int32_t left, right;

    if (!c->joined) {
        c->joined = 1;
        c->at = mix.made;
        c->played = 0;
        c->frac = 0;
        c->mark = 0;
        c->phase = 0;
        c->has_nxt = 0;
        s->transfer(s, mix.stage, c->frame);
        vchan_frame(s, mix.stage, c->cur);
        c->taken = 1;
    }
    /* The block's last frame lies `top` frames of the channel past `cur`,
       rounded up; `cur` and `nxt` are held already. */
    top = (c->phase + (n - 1) * rate + mix.rate - 1) / mix.rate;
    need = top > (uint32_t)c->has_nxt ? top - (uint32_t)c->has_nxt : 0;
    if (need > 0) {
        s->transfer(s, mix.stage, need * c->frame);
        c->taken += need;
    }
    for (i = 0; i < n; i++) {
        while (c->phase >= mix.rate) {
            c->phase -= mix.rate;
            if (c->has_nxt) {
                c->cur[0] = c->nxt[0];
                c->cur[1] = c->nxt[1];
                c->has_nxt = 0;
            } else if (k < need) {
                vchan_frame(s, mix.stage + k++ * c->frame, c->cur);
            }
        }
        left = c->cur[0];
        right = c->cur[1];
        if (c->phase != 0) {
            if (!c->has_nxt && k < need) {
                vchan_frame(s, mix.stage + k++ * c->frame, c->nxt);
                c->has_nxt = 1;
            }
            if (c->has_nxt) {
                w = (c->phase << VCHAN_WEIGHT) / mix.rate;
                left += (c->nxt[0] - left) * (int32_t)w / (1 << VCHAN_WEIGHT);
                right += (c->nxt[1] - right) * (int32_t)w / (1 << VCHAN_WEIGHT);
            }
        }
        sum[2 * i] += left;
        sum[2 * i + 1] += right;
        c->phase += rate;
    }
}

/**
 * The device stream's `transfer`, in the device's interrupt or its start:
 * make `size` bytes of the mix at `buf`, block by block, each the sum of
 * the channels running, clipped to 16 bits.
 */
static void vchan_render(audio_stream_t *s, uint8_t *buf, size_t size)
{
    uint32_t frames = (uint32_t)(size / 4u);
    uint32_t flags, n, i;
    struct vchan *c;
    int32_t v;

    (void)s;
    flags = spin_lock_irqsave(&mix.lock);
    while (frames > 0) {
        n = frames < VCHAN_BLOCK ? frames : VCHAN_BLOCK;
        k_memset(mix.sum, 0, n * 2u * sizeof(mix.sum[0]));
        for (c = mix.list; c; c = c->next) {
            if (c->running) {
                vchan_mix(c, n);
            }
        }
        for (i = 0; i < 2 * n; i++) {
            v = mix.sum[i] < -32768 ? -32768 : mix.sum[i] > 32767 ? 32767 : mix.sum[i];
            buf[0] = (uint8_t)((uint16_t)v & 0xFFu);
            buf[1] = (uint8_t)((uint16_t)v >> 8);
            buf += 2;
        }
        mix.made += n;
        frames -= n;
    }
    spin_unlock_irqrestore(&mix.lock, flags);
}

/**
 * The device stream's `period`, in the device's interrupt: count the
 * period's frames as played, move each channel's count on to them, and call
 * a channel's `period` once for each of its periods played since its last.
 */
static void vchan_period(audio_stream_t *s)
{
    uint32_t flags;
    struct vchan *c;

    (void)s;
    flags = spin_lock_irqsave(&mix.lock);
    mix.ended += mix.period;
    for (c = mix.list; c; c = c->next) {
        if (!c->running || !c->joined) {
            continue;
        }
        (void)vchan_played(c, mix.ended, 1);
        while (c->played - c->mark >= c->pframes) {
            c->mark += c->pframes;
            c->s->period(c->s);
        }
    }
    spin_unlock_irqrestore(&mix.lock, flags);
}

/**
 * Take the device's playback stream from `hw` for the mix and have it grant
 * 16-bit stereo at VCHAN_RATE, or the rate nearest it. 0; the driver's
 * error, as -EBUSY while the write path or another stream has the device;
 * or -EIO for a grant the mix cannot use. The stream is given back on an
 * error.
 */
static int vchan_take(audio_driver_t *hw)
{
    audio_stream_t *m = &mix.stream;
    int rc;

    k_memset(m, 0, sizeof(*m));
    m->dir = AUDIO_PLAYBACK;
    m->rate = VCHAN_RATE;
    m->format = AUDIO_S16LE;
    m->channels = 2;
    m->period_bytes = VCHAN_PERIOD;
    m->periods = 2;
    m->transfer = vchan_render;
    m->period = vchan_period;
    rc = hw->stream_open(m);
    if (rc != 0) {
        return rc;
    }
    rc = hw->stream_setup(m);
    if (rc == 0 && (m->format != AUDIO_S16LE || m->channels != 2 || m->period_bytes < 4 ||
                    m->period_bytes % 4 != 0 || m->rate < VCHAN_RATE_MIN || m->rate > VCHAN_RATE ||
                    m->device_periods == 0)) {
        rc = -EIO;
    }
    if (rc != 0) {
        hw->stream_close(m);
        return rc;
    }
    mix.hw = hw;
    mix.rate = m->rate;
    mix.period = m->period_bytes / 4u;
    mix.running = 0;
    return 0;
}

/** Open a channel for playback stream `s`; the first takes the device's
 * stream (vchan_take). 0, -ENODEV for capture or a device with no streams,
 * -ENOMEM, or vchan_take's error. */
static int vchan_open(audio_stream_t *s)
{
    audio_driver_t *hw = audio_hw;
    struct vchan *c;
    uint32_t flags;
    int rc;

    if (s->dir != AUDIO_PLAYBACK || !hw || !hw->stream_open) {
        return -ENODEV;
    }
    if (vchan_find(s)) {
        return -EBUSY;
    }
    c = (struct vchan *)kzalloc(sizeof(*c));
    if (!c) {
        return -ENOMEM;
    }
    if (!mix.list) {
        rc = vchan_take(hw);
        if (rc != 0) {
            kfree(c);
            return rc;
        }
    }
    c->s = s;
    c->frame = frame_bytes(s);
    flags = spin_lock_irqsave(&mix.lock);
    c->next = mix.list;
    mix.list = c;
    spin_unlock_irqrestore(&mix.lock, flags);
    return 0;
}

/**
 * Grant a stopped channel its format and channels, and its rate held from
 * VCHAN_RATE_MIN to VCHAN_RATE_MAX and VCHAN_RATIO times the mix's. The
 * period is the one asked for, cut to the frames of the channel one period
 * of the mix plays, so that a `period` call is due at each of the mix's.
 * `device_periods` covers the frames the device's start takes from the
 * channel that starts the mix, up to VCHAN_START_MAX bytes. 0, or -EINVAL
 * for a stream not open or running, a format other than unsigned 8-bit or
 * signed 16-bit, a channel count other than 1 or 2, or no rate.
 */
static int vchan_setup(audio_stream_t *s)
{
    struct vchan *c = vchan_find(s);
    uint32_t top, rate, frame, max, want, first = 1, frac = 0, dev, cap;

    if (!c || c->running || s->rate == 0 || (s->format != AUDIO_U8 && s->format != AUDIO_S16LE) ||
        s->channels < 1 || s->channels > 2) {
        return -EINVAL;
    }
    top = VCHAN_RATIO * mix.rate < VCHAN_RATE_MAX ? VCHAN_RATIO * mix.rate : VCHAN_RATE_MAX;
    rate = s->rate < VCHAN_RATE_MIN ? VCHAN_RATE_MIN : s->rate > top ? top : s->rate;
    frame = frame_bytes(s);
    max = mix.period * rate / mix.rate;
    want = s->period_bytes / frame;
    if (want > max) {
        want = max;
    }
    if (want == 0) {
        return -EINVAL;
    }
    /* The device's start makes device_periods periods of the mix; the
       channel gives its first frame and those its last frame reaches. */
    vchan_advance(&first, &frac, mix.stream.device_periods * mix.period - 1, rate);
    first += frac != 0;
    dev = (first + want - 1) / want;
    cap = VCHAN_START_MAX / (want * frame);
    if (dev > cap) {
        dev = cap > 0 ? cap : 1;
    }
    s->rate = rate;
    s->period_bytes = want * frame;
    s->device_periods = dev;
    if (s->periods < dev) {
        s->periods = dev;
    }
    c->frame = frame;
    c->pframes = want;
    return 0;
}

/** Put channel `s` in the mix, starting the device's stream when no other
 * channel runs: 0, or -EIO when it is not open or the device fails to
 * start. */
static int vchan_start(audio_stream_t *s)
{
    struct vchan *c = vchan_find(s);
    uint32_t flags;

    if (!c) {
        return -EIO;
    }
    if (c->running) {
        return 0;
    }
    flags = spin_lock_irqsave(&mix.lock);
    c->running = 1;
    c->joined = 0;
    c->taken = 0;
    c->played = 0;
    spin_unlock_irqrestore(&mix.lock, flags);
    if (!mix.running) {
        mix.made = 0;
        mix.ended = 0;
        if (mix.hw->stream_start(&mix.stream) != 0) {
            flags = spin_lock_irqsave(&mix.lock);
            c->running = 0;
            spin_unlock_irqrestore(&mix.lock, flags);
            return -EIO;
        }
        mix.running = 1;
    }
    return 0;
}

/**
 * Take channel `s` out of the mix at once; what the mix took from it and
 * has not played still plays, since it is summed with the others. The
 * device's stream stops with the last channel that runs.
 */
static void vchan_stop(audio_stream_t *s)
{
    struct vchan *c = vchan_find(s);
    struct vchan *o;
    uint32_t flags;
    int others = 0;

    if (!c || !c->running) {
        return;
    }
    flags = spin_lock_irqsave(&mix.lock);
    c->running = 0;
    for (o = mix.list; o; o = o->next) {
        others |= o->running;
    }
    spin_unlock_irqrestore(&mix.lock, flags);
    if (!others && mix.running) {
        mix.hw->stream_stop(&mix.stream);
        mix.running = 0;
    }
}

/** Close channel `s`, stopping it first; the last close gives the device's
 * stream back. */
static void vchan_close(audio_stream_t *s)
{
    struct vchan *c = vchan_find(s);
    struct vchan **p;
    uint32_t flags;

    if (!c) {
        return;
    }
    vchan_stop(s);
    flags = spin_lock_irqsave(&mix.lock);
    for (p = &mix.list; *p != c; p = &(*p)->next) {
    }
    *p = c->next;
    spin_unlock_irqrestore(&mix.lock, flags);
    kfree(c);
    if (!mix.list) {
        mix.hw->stream_close(&mix.stream);
        mix.hw = NULL;
    }
}

/**
 * The frames of channel `c` played, from the device's position, and those
 * taken, into `*played` and `*taken`. Played is never more than taken: a
 * channel at a higher rate than the mix's is ahead of the last frame it gave
 * by the fraction of a frame of the mix.
 */
static void vchan_counts(struct vchan *c, uint32_t *played, uint32_t *taken)
{
    uint32_t h = 0, flags;

    if (c->running && mix.running) {
        h = mix.hw->stream_position(&mix.stream);
    }
    flags = spin_lock_irqsave(&mix.lock);
    *taken = c->taken;
    *played = c->running && c->joined ? vchan_played(c, h, 0) : 0;
    if ((int32_t)(*played - *taken) > 0) {
        *played = *taken;
    }
    spin_unlock_irqrestore(&mix.lock, flags);
}

/** The frames of channel `s` played since its start; 0 when it is not
 * running. Safe with interrupts off. */
static uint32_t vchan_position(audio_stream_t *s)
{
    struct vchan *c = vchan_find(s);
    uint32_t played = 0, taken;

    if (c) {
        vchan_counts(c, &played, &taken);
    }
    return played;
}

/** The frames channel `s` has given and the device has not played, in its
 * rate; 0 when it is not running, and for `s` NULL, there being no write
 * path. */
static uint32_t vchan_delay(audio_stream_t *s)
{
    struct vchan *c = s ? vchan_find(s) : NULL;
    uint32_t played, taken;

    if (!c || !c->running) {
        return 0;
    }
    vchan_counts(c, &played, &taken);
    return taken - played;
}

/* The audio_driver_t fields, in the order audio.h declares them: name,
   type, init, shutdown, start, stop, is_playing, set_callback, write,
   set_volume, get_volume, then the seven stream operations; the channels
   have the stream operations alone. */
audio_driver_t vchan_driver = {"Virtual channels",
                               AUDIO_DRIVER_NONE,
                               NULL,
                               NULL,
                               NULL,
                               NULL,
                               NULL,
                               NULL,
                               NULL,
                               NULL,
                               NULL,
                               vchan_open,
                               vchan_setup,
                               vchan_start,
                               vchan_stop,
                               vchan_close,
                               vchan_position,
                               vchan_delay};
