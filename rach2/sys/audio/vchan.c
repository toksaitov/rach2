/*
 * vchan.c: virtual playback channels (vchan.h): every playback open mixed
 * into the device's one stream, each converted to 24 bits and to the mix's
 * rate by an integer linear resampler, and the sum clipped to the device's
 * width, 24 bits in 32 or 16 bits.
 */

#include "vchan.h"

#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../lib/spinlock.h"
#include "../mm/kheap.h"

#define VCHAN_RATE_MIN 1000u   /* the lowest rate a channel is granted */
#define VCHAN_RATE_MAX 192000u /* the highest, and at most VCHAN_RATIO times the mix's */
#define VCHAN_RATIO    4u
#define VCHAN_PERIOD   1024u   /* frames of the period the mix asks of the device */
#define VCHAN_BLOCK    256u    /* frames of the mix made at a time */
#define VCHAN_CHUNK    4096u   /* frames of the mix a count advances by at a time */
#define VCHAN_WEIGHT   14      /* bits of the resampler's weight between two frames */
#define VCHAN_FRAME    8u      /* bytes of the widest frame, 32-bit stereo */
#define VCHAN_MAX      8388607 /* the largest sample of the mix, 2^23 - 1 */
#define VCHAN_SUM_MAX                                         \
    1073741824 /* 2^30, within which the sum is held, so that \
                  a channel's sample added cannot overflow it */

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
    int has_nxt;    /* `nxt` holds the frame after `cur` */
    int32_t cur[2]; /* samples of 24 bits (vchan_sample) */
    int32_t nxt[2];
    uint32_t taken; /* its frames taken by `transfer` since the start */
    uint32_t at;
    uint32_t played;
    uint32_t frac;
    uint32_t process; /* the process that opened it, unchanged across fork */
    int volume;       /* its process PCM level, 0 to 100 */
    uint32_t mark;    /* `played` at the end of its last period */
};

/*
 * The mix: the device's stream, stereo at `rate`, between the first
 * channel's open and the last one's close, and the channels open. Its
 * samples are AUDIO_S32LE, of which the device plays the top 24 bits, or
 * AUDIO_S16LE, as the device granted; the sum is of samples of 24 bits,
 * and is clipped to 24 bits and narrowed to the device's width. `made`
 * counts the frames of the mix made since the stream's start, and `ended`
 * those in the periods the device has ended since. `lock` keeps the list,
 * the channels' state and the counts between process context and the
 * device's interrupt.
 */
static void vchan_capture_feed(audio_stream_t *s, uint8_t *buf, size_t size);

static struct {
    audio_driver_t *hw; /* the device's driver; NULL while no channel is open */
    audio_stream_t stream;
    uint32_t rate;   /* R */
    uint32_t period; /* frames of the stream's period */
    uint32_t frame;  /* bytes of one of its frames, 8 or 4 */
    struct vchan *list;
    int running;
    uint32_t made;
    uint32_t ended;
    struct spinlock lock;
    int32_t sum[VCHAN_BLOCK * 2];             /* a block's sum, left and right */
    uint8_t stage[VCHAN_STAGE * VCHAN_FRAME]; /* a channel's frames for a block */
} mix;

/** The bytes of one frame of `s`. */
static uint32_t vchan_frame_bytes(const audio_stream_t *s)
{
    return (uint32_t)s->channels * AUDIO_SAMPLE_BYTES(s->format);
}

/** Whether a channel or reader may have `format`: any audio_format_t. */
static int vchan_format_ok(audio_format_t format)
{
    return format == AUDIO_U8 || format == AUDIO_S16LE || format == AUDIO_S24LE ||
           format == AUDIO_S24_3LE || format == AUDIO_S32LE;
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

/** The signed 24-bit sample whose three bytes, low first, are at `p`. */
static int32_t vchan_s24(const uint8_t *p)
{
    uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;

    return (int32_t)(v & 0x800000u ? v | 0xFF000000u : v);
}

/**
 * The sample at `p` in `format` as 24 bits: unsigned 8-bit is moved to
 * signed and widened, 8 and 16 bits exactly, and a 32-bit sample keeps its
 * top 24 bits, as a codec of 24 bits plays it. The top byte of 24 bits in
 * four is ignored.
 */
static int32_t vchan_sample(audio_format_t format, const uint8_t *p)
{
    switch (format) {
    case AUDIO_U8:
        return ((int32_t)p[0] - 128) * 65536;
    case AUDIO_S16LE:
        return (int32_t)(int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)) * 256;
    case AUDIO_S32LE:
        return vchan_s24(p + 1);
    case AUDIO_S24LE:
    case AUDIO_S24_3LE:
    default:
        return vchan_s24(p);
    }
}

/** Put frame `p` of `s`'s format in `out` as 24-bit stereo; a mono sample
 * goes to both. */
static void vchan_frame(const audio_stream_t *s, const uint8_t *p, int32_t *out)
{
    out[0] = vchan_sample(s->format, p);
    out[1] = s->channels == 2 ? vchan_sample(s->format, p + AUDIO_SAMPLE_BYTES(s->format)) : out[0];
}

/** `v` clipped to 24 bits. */
static int32_t vchan_clip(int32_t v)
{
    return v < -VCHAN_MAX - 1 ? -VCHAN_MAX - 1 : v > VCHAN_MAX ? VCHAN_MAX : v;
}

/**
 * Sample `v` of 24 bits narrowed to `bits` of 8 or 16 bits, signed: rounded
 * to the nearest, a half up, and held to the range. The offset keeps the
 * division's dividend positive, where C89 defines its rounding.
 */
static int32_t vchan_narrow(int32_t v, int bits)
{
    int32_t unit = (int32_t)1 << (24 - bits), top = (int32_t)1 << (bits - 1);
    int32_t q = (vchan_clip(v) + VCHAN_MAX + 1 + unit / 2) / unit - top;

    return q > top - 1 ? top - 1 : q;
}

/**
 * `d` times `w` over 2^VCHAN_WEIGHT, rounded toward zero, for a difference
 * `d` of two samples of 24 bits and a weight `w` below 2^VCHAN_WEIGHT. The
 * product passes 32 bits, so the magnitude of `d` is split at bit 10: the
 * top part's product is under 2^28, and its low four bits, carried down, and
 * the low part's product sum to under 2^25.
 */
static int32_t vchan_weigh(int32_t d, uint32_t w)
{
    uint32_t a = (uint32_t)(d < 0 ? -d : d);
    uint32_t p = (a >> 10) * w;
    uint32_t r = (p >> 4) + (((p & 15u) << 10) + (a & 1023u) * w) / (1u << VCHAN_WEIGHT);

    return d < 0 ? -(int32_t)r : (int32_t)r;
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
    int32_t left, right, v;
    int ch;

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
                left += vchan_weigh(c->nxt[0] - left, w);
                right += vchan_weigh(c->nxt[1] - right, w);
            }
        }
        for (ch = 0; ch < 2; ch++) {
            v = sum[2 * i + ch] + (ch ? right : left) * c->volume / 100;
            sum[2 * i + ch] = v > VCHAN_SUM_MAX    ? VCHAN_SUM_MAX
                              : v < -VCHAN_SUM_MAX ? -VCHAN_SUM_MAX
                                                   : v;
        }
        c->phase += rate;
    }
}

/**
 * The device stream's `transfer`, in the device's interrupt or its start:
 * make `size` bytes of the mix at `buf`, block by block, each the sum of
 * the channels running, clipped to 24 bits and stored as the top 24 bits of
 * 32 or narrowed to 16 (vchan_narrow).
 */
static void vchan_render(audio_stream_t *s, uint8_t *buf, size_t size)
{
    uint32_t frames = (uint32_t)(size / mix.frame);
    uint32_t flags, n, i, u;
    uint8_t *block;
    struct vchan *c;

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
        block = buf;
        for (i = 0; i < 2 * n; i++) {
            if (mix.frame == 8u) {
                u = (uint32_t)vchan_clip(mix.sum[i]);
                buf[0] = 0;
                buf[1] = (uint8_t)u;
                buf[2] = (uint8_t)(u >> 8);
                buf[3] = (uint8_t)(u >> 16);
                buf += 4;
            } else {
                u = (uint32_t)vchan_narrow(mix.sum[i], 16);
                buf[0] = (uint8_t)u;
                buf[1] = (uint8_t)(u >> 8);
                buf += 2;
            }
        }
        vchan_capture_feed(s, block, n * mix.frame);
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
 * 32-bit stereo at VCHAN_RATE, or 16-bit, and the rate nearest it. 0; the
 * driver's error, as -EBUSY while the write path or another stream has the
 * device; or -EIO for a grant the mix cannot use. The stream is given back
 * on an error.
 */
static int vchan_take(audio_driver_t *hw)
{
    audio_stream_t *m = &mix.stream;
    int rc;

    k_memset(m, 0, sizeof(*m));
    m->dir = AUDIO_PLAYBACK;
    m->rate = VCHAN_RATE;
    m->format = AUDIO_S32LE;
    m->channels = 2;
    m->period_bytes = VCHAN_PERIOD * 8u;
    m->periods = 2;
    m->transfer = vchan_render;
    m->period = vchan_period;
    rc = hw->stream_open(m);
    if (rc != 0) {
        return rc;
    }
    rc = hw->stream_setup(m);
    if (rc == 0 &&
        ((m->format != AUDIO_S32LE && m->format != AUDIO_S16LE) || m->channels != 2 ||
         m->period_bytes < vchan_frame_bytes(m) || m->period_bytes % vchan_frame_bytes(m) != 0 ||
         m->rate < VCHAN_RATE_MIN || m->rate > VCHAN_RATE || m->device_periods == 0)) {
        rc = -EIO;
    }
    if (rc != 0) {
        hw->stream_close(m);
        return rc;
    }
    mix.hw = hw;
    mix.rate = m->rate;
    mix.frame = vchan_frame_bytes(m);
    mix.period = m->period_bytes / mix.frame;
    mix.running = 0;
    return 0;
}

static int vchan_capture_open(audio_stream_t *s);
static int vchan_capture_setup(audio_stream_t *s);
static int vchan_capture_start(audio_stream_t *s);
static void vchan_capture_stop(audio_stream_t *s);
static void vchan_capture_close(audio_stream_t *s);
static uint32_t vchan_capture_position(audio_stream_t *s);

/** Open a channel for playback stream `s`; the first takes the device's
 * stream (vchan_take). 0, -ENODEV for capture or a device with no streams,
 * -ENOMEM, or vchan_take's error. */
static int vchan_open(audio_stream_t *s)
{
    audio_driver_t *hw = audio_hw;
    struct vchan *c;
    uint32_t flags;
    int rc;

    if (s->dir == AUDIO_CAPTURE) {
        return vchan_capture_open(s);
    }
    if (!hw || !hw->stream_open) {
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
    c->process = audio_process_id();
    c->volume = vchan_pcm(-1);
    c->frame = vchan_frame_bytes(s);
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
 * for a stream not open or running, a format not in audio_format_t, a
 * channel count other than 1 or 2, or no rate.
 */
static int vchan_setup(audio_stream_t *s)
{
    struct vchan *c = vchan_find(s);
    uint32_t top, rate, frame, max, want, first = 1, frac = 0, dev, cap;

    if (s->dir == AUDIO_CAPTURE) {
        return vchan_capture_setup(s);
    }
    if (!c || c->running || s->rate == 0 || !vchan_format_ok(s->format) || s->channels < 1 ||
        s->channels > 2) {
        return -EINVAL;
    }
    top = VCHAN_RATIO * mix.rate < VCHAN_RATE_MAX ? VCHAN_RATIO * mix.rate : VCHAN_RATE_MAX;
    rate = s->rate < VCHAN_RATE_MIN ? VCHAN_RATE_MIN : s->rate > top ? top : s->rate;
    frame = vchan_frame_bytes(s);
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

    if (s->dir == AUDIO_CAPTURE) {
        return vchan_capture_start(s);
    }
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

    if (s->dir == AUDIO_CAPTURE) {
        vchan_capture_stop(s);
        return;
    }
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

    if (s->dir == AUDIO_CAPTURE) {
        vchan_capture_close(s);
        return;
    }
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

    if (s->dir == AUDIO_CAPTURE) {
        return vchan_capture_position(s);
    }
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

/* Each reader owns its conversion phase and staging period. The input
 * stream stays at its first reader's settings once another reader opens. */
struct vchan_reader {
    audio_stream_t *s;
    struct vchan_reader *next;
    int running;
    uint32_t origin; /* conversion phase when the reader joined */
    uint32_t phase;  /* output rate accumulated against the input rate */
    uint32_t skip;   /* frames in flight before this reader joined */
    uint32_t at;     /* input position when this reader started */
    uint32_t frames; /* frames handed to this reader since its start */
    uint32_t used;   /* bytes staged, less than its period */
    uint8_t *buffer; /* one period, at most VCHAN_START_MAX bytes */
};

static struct {
    struct vchan_reader *list;
    audio_stream_t stream;
    audio_stream_t clock; /* silent playback channel clocks loopback */
    audio_driver_t *hw;
    int running;
    int loopback;
    uint32_t frames; /* hardware input frames already handed to the readers */
    uint32_t lost;   /* the input's `lost` the feed has taken into account;
                        `frames` plus `lost` is where on the device's
                        timeline the frames handed over end */
    struct spinlock lock;
} input;

/** The reader of `s`, or NULL; calls in process context are serialized by
 * the kernel lock, and the interrupt's walks hold input.lock. */
static struct vchan_reader *vchan_reader_find(audio_stream_t *s)
{
    struct vchan_reader *r;

    for (r = input.list; r; r = r->next)
        if (r->s == s)
            return r;
    return NULL;
}

/** Fill the loopback clock's playback with silence; it owns no samples. */
static void vchan_silence(audio_stream_t *s, uint8_t *buf, size_t size)
{
    (void)s;
    k_memset(buf, 0, size);
}

/** The clock has no caller to wake at a period. */
static void vchan_clock_period(audio_stream_t *s)
{
    (void)s;
}

/** Stage the input frame `sample` of 24 bits, left and right, in reader
 * `r`'s format, and hand its period over and announce it when the frame
 * completes it. To 8 or 16 bits it is narrowed (vchan_narrow); a 32-bit
 * sample has the 24 bits on top of zeros. The caller holds input.lock. */
static void vchan_capture_put(struct vchan_reader *r, const int32_t *sample)
{
    audio_format_t format = r->s->format;
    uint8_t *p;
    uint32_t value;
    int ch;

    for (ch = 0; ch < r->s->channels; ch++) {
        value = (uint32_t)(r->s->channels == 1 ? (sample[0] + sample[1]) / 2 : sample[ch]);
        p = r->buffer + r->used;
        r->used += AUDIO_SAMPLE_BYTES(format);
        if (format == AUDIO_U8) {
            p[0] = (uint8_t)(vchan_narrow((int32_t)value, 8) + 128);
            continue;
        }
        if (format == AUDIO_S16LE) {
            value = (uint32_t)vchan_narrow((int32_t)value, 16);
            p[0] = (uint8_t)value;
            p[1] = (uint8_t)(value >> 8);
            continue;
        }
        if (format == AUDIO_S32LE) {
            *p++ = 0;
        }
        p[0] = (uint8_t)value;
        p[1] = (uint8_t)(value >> 8);
        p[2] = (uint8_t)(value >> 16);
        if (format == AUDIO_S24LE) {
            p[3] = (uint8_t)(value >> 24); /* the sign of 24 bits in four */
        }
    }
    r->frames++;
    if (r->used == r->s->period_bytes) {
        r->s->transfer(r->s, r->buffer, r->used);
        r->used = 0;
        r->s->period(r->s);
    }
}

/**
 * Convert the input frames at `buf` into each running reader's format and
 * rate. Each input frame is repeated or omitted by the reader's rational
 * phase, and each complete reader period is handed over and announced.
 * Frames the device lost since the last call were among those a reader
 * that joined meanwhile was to skip, so they count against its skip.
 * Loopback feeds the clipped playback mix; a real input calls this from
 * its interrupt. No caller may call the driver back from either callback.
 */
static void vchan_capture_feed(audio_stream_t *s, uint8_t *buf, size_t size)
{
    struct vchan_reader *r;
    uint32_t flags, i, n, frame, lost;
    int32_t sample[2];

    if (!input.running || (input.loopback ? s != &mix.stream : s != &input.stream))
        return;
    flags = spin_lock_irqsave(&input.lock);
    frame = vchan_frame_bytes(s);
    n = (uint32_t)size / frame;
    lost = input.loopback ? 0 : s->lost - input.lost;
    input.lost += lost;
    for (r = input.list; r; r = r->next) {
        if (!r->running)
            continue;
        r->skip -= r->skip < lost ? r->skip : lost;
        i = r->skip < n ? r->skip : n;
        r->skip -= i;
        for (; i < n; i++) {
            vchan_frame(s, buf + i * frame, sample);
            r->phase += r->s->rate;
            while (r->phase >= s->rate) {
                r->phase -= s->rate;
                vchan_capture_put(r, sample);
            }
        }
    }
    input.frames += n;
    spin_unlock_irqrestore(&input.lock, flags);
}

/** Open a reader, taking the device's input at the first open; in loopback
 * a silent playback channel takes its place. 0, -ENOMEM, or the device's
 * error. A failed open gives back everything it took. */
static int vchan_capture_open(audio_stream_t *s)
{
    struct vchan_reader *r;
    audio_stream_t *m = &input.stream;
    uint32_t flags;
    int rc;

    if (vchan_reader_find(s))
        return -EBUSY;
    r = (struct vchan_reader *)kzalloc(sizeof(*r));
    if (!r)
        return -ENOMEM;
    r->buffer = (uint8_t *)kmalloc(VCHAN_START_MAX);
    if (!r->buffer) {
        kfree(r);
        return -ENOMEM;
    }
    if (!input.list) {
        if (input.loopback) {
            k_memset(&input.clock, 0, sizeof(input.clock));
            input.clock.dir = AUDIO_PLAYBACK;
            input.clock.rate = VCHAN_RATE;
            input.clock.format = AUDIO_S16LE;
            input.clock.channels = 2;
            input.clock.transfer = vchan_silence;
            input.clock.period = vchan_clock_period;
            rc = vchan_open(&input.clock);
            if (rc == 0) {
                input.clock.period_bytes = mix.period * 4u;
                input.clock.periods = 2;
                rc = vchan_setup(&input.clock);
                if (rc != 0)
                    vchan_close(&input.clock);
            }
            if (rc == 0)
                *m = mix.stream;
        } else {
            *m = *s;
            m->transfer = vchan_capture_feed;
            m->period = vchan_clock_period;
            input.hw = audio_hw;
            rc = input.hw->stream_open(m);
        }
        if (rc != 0) {
            kfree(r->buffer);
            kfree(r);
            return rc;
        }
    }
    r->s = s;
    flags = spin_lock_irqsave(&input.lock);
    r->next = input.list;
    input.list = r;
    spin_unlock_irqrestore(&input.lock, flags);
    return 0;
}

/** Grant a stopped reader its format and rate. The only reader may set a
 * stopped device's parameters, and asks it for 16 bits for a wider format;
 * several readers convert independently from the fixed input. 0, -EINVAL,
 * or the device's setup error. */
static int vchan_capture_setup(audio_stream_t *s)
{
    struct vchan_reader *r = vchan_reader_find(s);
    audio_stream_t old = input.stream;
    audio_stream_t *m = &input.stream;
    uint32_t frame, max;
    int rc, wide;

    if (!r || r->running || !s->rate || s->rate > VCHAN_RATE_MAX || !vchan_format_ok(s->format) ||
        s->channels < 1 || s->channels > 2 || s->period_bytes > VCHAN_START_MAX)
        return -EINVAL;
    if (!input.loopback && !input.running && input.list == r && !r->next) {
        wide = AUDIO_SAMPLE_BYTES(s->format) > 2;
        *m = *s;
        if (wide) {
            m->format = AUDIO_S16LE;
            m->period_bytes = s->period_bytes / vchan_frame_bytes(s) * vchan_frame_bytes(m);
        }
        m->transfer = vchan_capture_feed;
        m->period = vchan_clock_period;
        rc = input.hw->stream_setup(m);
        if (rc != 0) {
            *m = old;
            return rc;
        }
        s->rate = m->rate;
        if (!wide)
            s->format = m->format;
        s->channels = m->channels;
        s->period_bytes = m->period_bytes / vchan_frame_bytes(m) * vchan_frame_bytes(s);
    }
    if (!m->rate || m->rate > VCHAN_RATE_MAX)
        return -EIO;
    frame = vchan_frame_bytes(s);
    max = (m->period_bytes / vchan_frame_bytes(m)) * s->rate / m->rate;
    if (s->period_bytes / frame > max)
        s->period_bytes = max * frame;
    s->period_bytes -= s->period_bytes % frame;
    if (!s->period_bytes)
        return -EINVAL;
    s->device_periods = m->device_periods;
    r->used = 0;
    return 0;
}

/**
 * Start reader `s`, starting the shared input only for its first running
 * reader. 0, -EIO, or the device's error.
 *
 * A reader that joins a running input skips the frames the device recorded
 * before it joined and has not handed over yet: those from where the frames
 * handed over end on the device's timeline, past the ones an overrun lost,
 * to the device's position.
 */
static int vchan_capture_start(audio_stream_t *s)
{
    struct vchan_reader *r = vchan_reader_find(s);
    uint32_t flags, frames, n, handed;
    int rc = 0;

    if (!r)
        return -EIO;
    r->at = input.running && !input.loopback ? input.hw->stream_position(&input.stream) : 0;
    flags = spin_lock_irqsave(&input.lock);
    handed = input.frames + input.lost;
    if (input.running && !input.loopback && (int32_t)(handed - r->at) > 0)
        r->at = handed;
    r->skip = input.running && !input.loopback ? r->at - handed : 0;
    r->running = 1;
    r->phase = r->frames = r->used = 0;
    frames = input.running ? (input.loopback ? input.frames : r->at) % input.stream.rate : 0;
    while (frames) {
        n = frames < VCHAN_CHUNK ? frames : VCHAN_CHUNK;
        r->phase = (r->phase + n * s->rate) % input.stream.rate;
        frames -= n;
    }
    r->origin = r->phase;
    spin_unlock_irqrestore(&input.lock, flags);
    if (!input.running) {
        input.frames = 0;
        input.lost = 0;
        input.stream.lost = 0;
        input.running = 1;
        rc = input.loopback ? vchan_start(&input.clock) : input.hw->stream_start(&input.stream);
        if (rc != 0) {
            input.running = 0;
            r->running = 0;
        }
    }
    return rc;
}

/** Stop reader `s`; the last running reader stops the shared input without
 * touching another reader's buffer or any playback channel. */
static void vchan_capture_stop(audio_stream_t *s)
{
    struct vchan_reader *r = vchan_reader_find(s), *other;
    uint32_t flags;
    int running = 0;

    if (!r || !r->running)
        return;
    flags = spin_lock_irqsave(&input.lock);
    r->running = 0;
    for (other = input.list; other; other = other->next)
        running |= other->running;
    spin_unlock_irqrestore(&input.lock, flags);
    if (!running && input.running) {
        input.running = 0;
        if (input.loopback)
            vchan_stop(&input.clock);
        else
            input.hw->stream_stop(&input.stream);
    }
}

/** Close reader `s`; the last close releases the shared input. */
static void vchan_capture_close(audio_stream_t *s)
{
    struct vchan_reader *r = vchan_reader_find(s), **p;
    uint32_t flags;

    if (!r)
        return;
    vchan_capture_stop(s);
    flags = spin_lock_irqsave(&input.lock);
    for (p = &input.list; *p != r; p = &(*p)->next) {
    }
    *p = r->next;
    spin_unlock_irqrestore(&input.lock, flags);
    kfree(r->buffer);
    kfree(r);
    if (!input.list) {
        if (input.loopback)
            vchan_close(&input.clock);
        else
            input.hw->stream_close(&input.stream);
        input.hw = NULL;
    }
}

/** The reader's frames captured since its start, including frames its
 * caller dropped and those staged for its next period. */
static uint32_t vchan_capture_position(audio_stream_t *s)
{
    struct vchan_reader *r = vchan_reader_find(s);
    uint32_t flags, frames = 0, delta, n, frac, position = 0;

    if (r && r->running && !input.loopback)
        position = input.hw->stream_position(&input.stream);
    flags = spin_lock_irqsave(&input.lock);
    if (r && r->running) {
        frames = r->frames;
        if (!input.loopback) {
            delta = position - r->at;
            frac = r->origin;
            frames = 0;
            while (delta) {
                n = delta < VCHAN_CHUNK ? delta : VCHAN_CHUNK;
                frac += n * s->rate;
                frames += frac / input.stream.rate;
                frac %= input.stream.rate;
                delta -= n;
            }
        }
    }
    spin_unlock_irqrestore(&input.lock, flags);
    return frames;
}

/** Select loopback at boot, before any stream opens. */
void vchan_set_loopback(int enabled)
{
    input.loopback = enabled;
}

/** Read the calling process's PCM level, or set every playback channel it
 * opened when `level` is nonnegative. With no channels it reads 100. The
 * opener's process identity stays with a channel across fork and exec. */
int vchan_pcm(int level)
{
    struct vchan *c;
    uint32_t process = audio_process_id(), flags;
    int value = 100;

    flags = spin_lock_irqsave(&mix.lock);
    for (c = mix.list; c; c = c->next) {
        if (c->process != process || c->s == &input.clock)
            continue;
        if (level >= 0)
            c->volume = level > 100 ? 100 : level;
        value = c->volume;
    }
    spin_unlock_irqrestore(&mix.lock, flags);
    return value;
}

/* The console's AUDIO_* interface has one channel of its own. Its write
 * ring and callback are independent of every /dev/dsp open. */
static struct {
    audio_stream_t stream;
    audio_callback_t callback;
    uint8_t *ring;
    uint32_t head, used;
    uint32_t capacity; /* three periods of the console caller's frames */
    int opened;
    struct spinlock lock;
} console_audio;

/** Fill the console channel from its callback or write ring; missing whole
 * frames become silence. Called with the mix locked and interrupts off. */
static void vchan_console_transfer(audio_stream_t *s, uint8_t *buf, size_t size)
{
    uint32_t flags, n, first, frame = vchan_frame_bytes(s);

    if (console_audio.callback) {
        console_audio.callback(buf, size);
        return;
    }
    flags = spin_lock_irqsave(&console_audio.lock);
    n = console_audio.used - console_audio.used % frame;
    if (n > size)
        n = (uint32_t)size;
    first = n < console_audio.capacity - console_audio.head
                ? n
                : console_audio.capacity - console_audio.head;
    k_memcpy(buf, console_audio.ring + console_audio.head, first);
    k_memcpy(buf + first, console_audio.ring, n - first);
    console_audio.head = (console_audio.head + n) % console_audio.capacity;
    console_audio.used -= n;
    spin_unlock_irqrestore(&console_audio.lock, flags);
    k_memset(buf + n, s->format == AUDIO_U8 ? 128 : 0, size - n);
}

/** Stop and release only the console's playback channel. */
static void vchan_console_stop(void)
{
    if (!console_audio.opened)
        return;
    vchan_close(&console_audio.stream);
    console_audio.opened = 0;
    kfree(console_audio.ring);
    console_audio.ring = NULL;
    console_audio.head = console_audio.used = 0;
}

/** Start the console's own channel at the requested parameters; 0, or the
 * stream's error. A refused format leaves any running channel alone. */
static int vchan_console_start(uint32_t rate, audio_format_t format, int channels)
{
    audio_stream_t *s = &console_audio.stream;
    int rc;

    if (!rate || rate > VCHAN_RATE_MAX || (format != AUDIO_U8 && format != AUDIO_S16LE) ||
        channels < 1 || channels > 2)
        return -EINVAL;
    vchan_console_stop();
    console_audio.ring = (uint8_t *)kmalloc(VCHAN_START_MAX);
    if (!console_audio.ring)
        return -ENOMEM;
    k_memset(s, 0, sizeof(*s));
    s->rate = rate;
    s->format = format;
    s->channels = channels;
    s->period_bytes = VCHAN_PERIOD;
    s->periods = 2;
    s->transfer = vchan_console_transfer;
    s->period = vchan_clock_period;
    rc = vchan_open(s);
    if (rc == 0) {
        console_audio.opened = 1;
        rc = vchan_setup(s);
        if (rc == 0) {
            console_audio.capacity = 3u * s->period_bytes;
            if (console_audio.capacity > VCHAN_START_MAX)
                console_audio.capacity = VCHAN_START_MAX;
            rc = vchan_start(s);
        }
    }
    if (rc != 0) {
        if (console_audio.opened)
            vchan_console_stop();
        else {
            kfree(console_audio.ring);
            console_audio.ring = NULL;
        }
    }
    return rc;
}

/** Whether the console's channel is running. */
static bool vchan_console_playing(void)
{
    return console_audio.opened != 0;
}

/** Select the console channel's callback, or NULL for its write ring. */
static void vchan_console_callback(audio_callback_t fn)
{
    console_audio.callback = fn;
}

/** Queue at most the room in the console channel's ring; the bytes taken,
 * or -EINVAL while stopped or for an empty write. */
static int vchan_console_write(const uint8_t *buf, size_t size)
{
    uint32_t flags, n, tail, first;

    if (!console_audio.opened || !buf || !size)
        return -EINVAL;
    flags = spin_lock_irqsave(&console_audio.lock);
    n = console_audio.capacity - console_audio.used;
    if (n > size)
        n = (uint32_t)size;
    tail = (console_audio.head + console_audio.used) % console_audio.capacity;
    first = n < console_audio.capacity - tail ? n : console_audio.capacity - tail;
    k_memcpy(console_audio.ring + tail, buf, first);
    k_memcpy(console_audio.ring, buf + first, n - first);
    console_audio.used += n;
    spin_unlock_irqrestore(&console_audio.lock, flags);
    return (int)n;
}

/** Set the hardware's master level; PCM is each process's channel level. */
static void vchan_volume(int level)
{
    audio_hw->set_volume(level);
}

/** The hardware's master level. */
static int vchan_get_volume(void)
{
    return audio_hw->get_volume();
}

/** The hardware's input capabilities, or none. */
static unsigned vchan_input_caps(unsigned *controls)
{
    *controls = 0;
    return audio_hw->input_caps ? audio_hw->input_caps(controls) : 0;
}

/** Select or read the hardware's input source; -1 when it has none. */
static int vchan_input_select(int source)
{
    return audio_hw->input_select ? audio_hw->input_select(source) : -1;
}

/** Read or set the hardware's input amplifier; -EINVAL when absent. */
static int vchan_input_level(int ctl, int source, int level)
{
    return audio_hw->input_level ? audio_hw->input_level(ctl, source, level) : -EINVAL;
}

/** The channel's delay, or the console's queue and channel for NULL. */
static uint32_t vchan_dispatch_delay(audio_stream_t *s)
{
    if (s)
        return vchan_delay(s);
    if (!console_audio.opened)
        return 0;
    return console_audio.used / vchan_frame_bytes(&console_audio.stream) +
           vchan_delay(&console_audio.stream);
}

/** Shut down the console channel and then the hardware at machine exit. */
static void vchan_shutdown(void)
{
    vchan_console_stop();
    if (audio_hw->shutdown)
        audio_hw->shutdown();
}

/* Hardware identity, and the beep generator, which needs no mix, are
 * filled by vchan_dispatch at boot. */
audio_driver_t vchan_driver = {"Virtual channels",
                               AUDIO_DRIVER_NONE,
                               NULL,
                               vchan_shutdown,
                               vchan_console_start,
                               vchan_console_stop,
                               vchan_console_playing,
                               vchan_console_callback,
                               vchan_console_write,
                               vchan_volume,
                               vchan_get_volume,
                               vchan_open,
                               vchan_setup,
                               vchan_start,
                               vchan_stop,
                               vchan_close,
                               vchan_position,
                               vchan_dispatch_delay,
                               vchan_input_caps,
                               vchan_input_select,
                               vchan_input_level,
                               NULL,
                               NULL};

/** Install the mix over hardware that has streams, passing the beep
 * generator's operations through; the speaker stays directly accessible,
 * since it cannot play PCM samples. */
void vchan_dispatch(void)
{
    if (!audio_hw || !audio_hw->stream_open)
        return;
    vchan_driver.name = audio_hw->name;
    vchan_driver.type = audio_hw->type;
    vchan_driver.beep = audio_hw->beep;
    vchan_driver.beep_level = audio_hw->beep_level;
    audio_set_dispatch(&vchan_driver);
}
