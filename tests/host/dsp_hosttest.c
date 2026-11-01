/**
 * /dev/dsp (audio/dsp.c) over a model driver: each ioctl's answer, blocking
 * and interrupted writes and reads, partial frames, drain against reset, the
 * trigger, a reader that falls behind, playback and capture on one open, and
 * the negotiation sequences of sox_ng's, FFmpeg's and cmus's OSS outputs and
 * of sox_ng's and FFmpeg's OSS inputs.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * The test includes dsp.c whole. The model driver's DMA buffer has two
 * periods; a tick plays the one due, appends its bytes to `m.out`, refills it
 * through the stream's `transfer` and calls `period`, as a device's interrupt
 * would. Its capture stream records the pattern in order: a tick hands the
 * next period of it over through `transfer` and calls `period`. A wait runs
 * ticks until its condition holds, or ends with a signal when a row asks for
 * one, so `m.out` is exactly what a listener would hear and `got` exactly
 * what a program read.
 * The sequences are those of sox_ng's `src/oss.c` (ossinit), FFmpeg's
 * `libavdevice/oss.c` (ff_oss_audio_open) and `oss_enc.c`, and cmus's
 * `op/oss.c` (oss_open, oss_set_sf, oss_buffer_space, oss_pause); the
 * inputs' are sox_ng's ossinit and oss_read and FFmpeg's `oss_dec.c`
 * (audio_read_header, audio_read_packet).
 */
#include "audio/dsp.c"

extern int printf(const char *format, ...);
extern unsigned long kheap_host_largest;
extern int memcmp(const void *a, const void *b, unsigned long n);

/* The requests the rows use that dsp.c does not name. */
#define DSP_SUBDIVIDE 0xC0045009u
#define TIOCGWINSZ    0x5413u
#define AFMT_S32_LE   0x1000

#define MODEL_PERIOD_MAX 16384u
#define MODEL_OUT        (1u << 20)
#define PATTERN          (1u << 18)

static int failures;
static const char *row; /* the row under way, named in a failure */

/* The model driver: its stream, its two DMA periods, the bytes it played in
 * order since the row began, and what it was asked. */
static struct {
    audio_stream_t *s;
    int running;
    uint32_t position; /* frames played since the start */
    uint32_t given;    /* frames transferred since the start */
    uint8_t dma[2][MODEL_PERIOD_MAX];
    int next;          /* the DMA period played next */
    uint32_t rate_min; /* the rates it grants */
    uint32_t rate_max;
    uint32_t period_max; /* the largest period it grants */
    int refuse_open;     /* stream_open's error, or 0 */
    int refuse_start;    /* stream_start's error, or 0 */
    int starts, stops, closes;
    unsigned ticks;
    uint8_t out[MODEL_OUT];
    uint32_t out_len;
} m;

/* The model's capture stream: what it was asked, and how far into the
 * pattern it has recorded. */
static struct {
    audio_stream_t *s;
    int running;
    uint32_t position; /* frames recorded since the start */
    uint32_t at;       /* the pattern's byte the device records next */
    int refuse_open;   /* stream_open's error, or 0 */
    int refuse_start;  /* stream_start's error, or 0 */
    int starts, stops, closes;
    unsigned ticks;
} cap;

/* The bytes the rows write and the model records, each different from its
 * neighbours. */
static uint8_t pattern[PATTERN];

/* What the rows read, in order. */
static uint8_t got[PATTERN];
static uint32_t got_len;

static fd_entry_t entry;
static struct open_file description;
static int signal_after = -1; /* ticks before a signal ends a wait; -1 none */
static unsigned waits, wakes;
static uint32_t masked; /* interrupts are off, under the open's lock */

/** Count and name a failure when `ok` is false. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("dsp-hosttest: fail: %s: %s\n", row, what);
        failures++;
    }
}

/** Turn interrupts off, for the host's spinlock; the old state. */
uint32_t kirq_save(void)
{
    uint32_t old = masked;

    masked = 1;
    return old;
}

/** Put interrupts back as `kirq_save` found them. */
void kirq_restore(uint32_t flags)
{
    masked = flags;
}

/** The bytes of one frame of the model's stream. */
static uint32_t model_frame(void)
{
    return frame_bytes(m.s);
}

/** Fill DMA period `i` through the stream's transfer. */
static void model_fill(int i)
{
    m.s->transfer(m.s, m.dma[i], m.s->period_bytes);
    m.given += m.s->period_bytes / model_frame();
}

/** Record one period of the pattern, as the device does, hand it over and
 * tell the stream. Nothing happens while the stream is stopped. */
static void cap_tick(void)
{
    uint32_t n;

    if (!cap.running) {
        return;
    }
    n = cap.s->period_bytes;
    check(cap.at + n <= PATTERN, "the pattern has a period more to record");
    if (cap.at + n > PATTERN) {
        return;
    }
    cap.s->transfer(cap.s, pattern + cap.at, n);
    cap.at += n;
    cap.position += n / frame_bytes(cap.s);
    cap.ticks++;
    cap.s->period(cap.s);
}

/** Let a period pass: record one when capture runs, and play one, then
 * refill it and tell the stream, when playback runs. */
static void model_tick(void)
{
    uint32_t n;

    cap_tick();
    if (!m.running) {
        return;
    }
    n = m.s->period_bytes;
    check(m.out_len + n <= MODEL_OUT, "the model's record has room");
    if (m.out_len + n <= MODEL_OUT) {
        k_memcpy(m.out + m.out_len, m.dma[m.next], n);
        m.out_len += n;
    }
    m.position += n / model_frame();
    model_fill(m.next);
    m.next ^= 1;
    m.ticks++;
    m.s->period(m.s);
}

static int model_open(audio_stream_t *s)
{
    if (s->dir == AUDIO_CAPTURE) {
        if (cap.refuse_open) {
            return cap.refuse_open;
        }
        if (cap.s) {
            return -EBUSY;
        }
        cap.s = s;
        return 0;
    }
    if (m.refuse_open) {
        return m.refuse_open;
    }
    if (m.s) {
        return -EBUSY;
    }
    check(s->dir == AUDIO_PLAYBACK, "a playback stream");
    m.s = s;
    return 0;
}

/** Grant the rate within the model's range, a period no larger than its
 * limit, and a DMA buffer of two periods for playback, eight for capture. */
static int model_setup(audio_stream_t *s)
{
    int in = s->dir == AUDIO_CAPTURE;

    check(s == (in ? cap.s : m.s), "setup of the open stream");
    check(!(in ? cap.running : m.running), "setup while stopped");
    check(s->format == AUDIO_U8 || s->format == AUDIO_S16LE, "a known format");
    check(s->channels == 1 || s->channels == 2, "one or two channels");
    s->rate = clamp(s->rate, m.rate_min, m.rate_max);
    if (s->period_bytes > m.period_max) {
        s->period_bytes = m.period_max;
    }
    s->device_periods = in ? 8 : 2;
    return 0;
}

static int model_start(audio_stream_t *s)
{
    if (s->dir == AUDIO_CAPTURE) {
        check(s == cap.s && !cap.running, "start of the stopped capture stream");
        check(!masked, "a capture start with interrupts on, outside the open's lock");
        if (cap.refuse_start) {
            return cap.refuse_start;
        }
        cap.running = 1;
        cap.position = 0;
        cap.starts++;
        return 0;
    }
    check(s == m.s && !m.running, "start of the stopped stream");
    check(!masked, "a start with interrupts on, outside the open's lock");
    if (m.refuse_start) {
        return m.refuse_start;
    }
    m.running = 1;
    m.position = 0;
    m.given = 0;
    m.next = 0;
    m.starts++;
    model_fill(0);
    model_fill(1);
    return 0;
}

static void model_stop(audio_stream_t *s)
{
    if (s->dir == AUDIO_CAPTURE) {
        check(s == cap.s && cap.running, "stop of the running capture stream");
        check(!masked, "a capture stop with interrupts on, outside the open's lock");
        cap.running = 0;
        cap.stops++;
        return;
    }
    check(s == m.s && m.running, "stop of the running stream");
    check(!masked, "a stop with interrupts on, outside the open's lock");
    m.running = 0;
    m.stops++;
}

static void model_close(audio_stream_t *s)
{
    if (s->dir == AUDIO_CAPTURE) {
        check(s == cap.s && !cap.running, "close of the stopped capture stream");
        cap.s = NULL;
        cap.closes++;
        return;
    }
    check(s == m.s, "close of the open stream");
    check(!m.running, "close after the stop");
    m.s = NULL;
    m.closes++;
}

static uint32_t model_position(audio_stream_t *s)
{
    if (s->dir == AUDIO_CAPTURE) {
        check(s == cap.s && cap.running, "position of the running capture stream");
        return cap.position;
    }
    check(s == m.s && m.running, "position of the running stream");
    return m.position;
}

static uint32_t model_delay(audio_stream_t *s)
{
    check(s->dir == AUDIO_PLAYBACK, "the delay of playback alone");
    check(s == m.s && m.running, "delay of the running stream");
    return m.given - m.position;
}

static audio_driver_t model = {.name = "model",
                               .type = AUDIO_DRIVER_HDA,
                               .stream_open = model_open,
                               .stream_setup = model_setup,
                               .stream_start = model_start,
                               .stream_stop = model_stop,
                               .stream_close = model_close,
                               .stream_position = model_position,
                               .delay = model_delay};

/* A driver with no streams, as HDA and the SB16 are until they get theirs. */
static audio_driver_t streamless = {.name = "streamless", .type = AUDIO_DRIVER_SB16};

audio_driver_t *audio;

/** Wait as the kernel does, with the model's ticks for the time that
 * passes: 0 once `cond` holds, or -EINTR at the signal a row asked for. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    int n = 0;

    check(wq == &dsp_owner->wq, "a wait on the open's queue");
    waits++;
    while (!cond(arg)) {
        if (signal_after >= 0 && n >= signal_after) {
            signal_after = -1;
            return -EINTR;
        }
        if ((!m.running && !cap.running) || n > 100000) {
            check(0, "a wait that nothing ends");
            return -EINTR;
        }
        model_tick();
        n++;
    }
    return 0;
}

void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    (void)kinds;
    wakes++;
}

void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
}

/** Copy from the caller; a NULL source is a bad pointer. */
int copy_from_user(void *dst, const void __user *src, size_t len)
{
    if (!src) {
        return -EFAULT;
    }
    k_memcpy(dst, src, len);
    return 0;
}

/** Copy to the caller; a NULL destination is a bad pointer. */
int copy_to_user(void __user *dst, const void *src, size_t len)
{
    if (!dst) {
        return -EFAULT;
    }
    k_memcpy(dst, src, len);
    return 0;
}

/** Begin row `name`: the model at rest, its record empty. */
static void begin(const char *name)
{
    row = name;
    check(m.s == NULL && cap.s == NULL, "no stream open between rows");
    k_memset(&m, 0, sizeof(m));
    k_memset(&cap, 0, sizeof(cap));
    got_len = 0;
    m.rate_min = 4000;
    m.rate_max = 48000;
    m.period_max = MODEL_PERIOD_MAX;
    audio = &model;
    signal_after = -1;
}

/** Open /dev/dsp on `entry` with open(2)'s `flags`. */
static int32_t open_dsp(int flags)
{
    k_memset(&entry, 0, sizeof(entry));
    k_memset(&description, 0, sizeof(description));
    entry.ofd = &description;
    description.refcount = 1;
    return dspdev_open(&entry, flags);
}

/** ioctl `request` with the int `value`; the answer is left in `value`. */
static int32_t call(uint32_t request, int *value)
{
    return dspdev_ioctl(&entry, request, (uintptr_t)value);
}

/** ioctl `request` with `value`, which must succeed; its answer. */
static int ask(uint32_t request, int value)
{
    check(call(request, &value) == 0, "the ioctl succeeds");
    return value;
}

/** Write `n` bytes of the pattern from `at`. */
static int32_t put(uint32_t at, uint32_t n)
{
    return dspdev_write(&entry, (const char __user *)(pattern + at), n);
}

/** Read `n` bytes onto the end of `got`. */
static int32_t take(uint32_t n)
{
    int32_t rc = dspdev_read(&entry, (char __user *)(got + got_len), n);

    if (rc > 0) {
        got_len += (uint32_t)rc;
    }
    return rc;
}

/** Whether `got` from `at` has `n` bytes of the pattern from `from`. */
static int recorded(uint32_t at, uint32_t from, uint32_t n)
{
    return at + n <= got_len && memcmp(got + at, pattern + from, n) == 0;
}

/** GETISPACE's answer. */
static struct dsp_buf_info ispace(void)
{
    struct dsp_buf_info info;

    check(dspdev_ioctl(&entry, DSP_GETISPACE, (uintptr_t)&info) == 0, "GETISPACE succeeds");
    return info;
}

/** The bytes queued by GETODELAY. */
static int odelay(void)
{
    return ask(DSP_GETODELAY, 0);
}

/** GETOSPACE's answer. */
static struct dsp_buf_info ospace(void)
{
    struct dsp_buf_info info;

    check(dspdev_ioctl(&entry, DSP_GETOSPACE, (uintptr_t)&info) == 0, "GETOSPACE succeeds");
    return info;
}

/** Whether `m.out` from `at` has `n` bytes of the pattern from `from`. */
static int played(uint32_t at, uint32_t from, uint32_t n)
{
    return at + n <= m.out_len && memcmp(m.out + at, pattern + from, n) == 0;
}

/** Whether `m.out` from `at` to its end is the silence of `format`. */
static int silent_from(uint32_t at, audio_format_t format)
{
    uint32_t i;

    for (i = at; i < m.out_len; i++) {
        if (m.out[i] != (uint8_t)silence(format)) {
            return 0;
        }
    }
    return 1;
}

/** Set signed 16-bit stereo at 44100 Hz and four periods of 1024 bytes,
 * 256 frames each; the stream starts at 2048 bytes. */
static void cd_quality(void)
{
    check(ask(DSP_SETFMT, AFMT_S16_LE) == AFMT_S16_LE, "S16_LE granted");
    check(ask(DSP_CHANNELS, 2) == 2, "stereo granted");
    check(ask(DSP_SPEED, 44100) == 44100, "44100 Hz granted");
    check(call(DSP_SETFRAGMENT, &(int){0x0004000A}) == 0, "SETFRAGMENT succeeds");
    check(ask(DSP_GETBLKSIZE, 0) == 1024, "a period of 1024 bytes");
}

/** Close the descriptor, as the last close of its description does. */
static void close_dsp(void)
{
    dspdev_close(&entry);
    check(entry.priv == NULL && dsp_owner == NULL, "the device is free after the close");
    check(m.s == NULL && cap.s == NULL, "the streams are closed");
}

/** OSS's defaults at open, the fixed answers, and one open at a time. */
static void row_defaults(void)
{
    fd_entry_t first;
    struct dsp_buf_info info;

    begin("defaults");
    check(dspdev_present(), "present over a driver with streams");
    check(open_dsp(1) == 0, "an open for writing succeeds");
    check(ask(DSP_SETFMT, AFMT_QUERY) == AFMT_U8, "U8 at open");
    check(ask(DSP_CHANNELS, 0) == 1, "mono at open");
    check(ask(DSP_SPEED, 0) == 8000, "8000 Hz at open");
    /* 8000 bytes a second over 43 is 186, nearest 128. */
    check(ask(DSP_GETBLKSIZE, 0) == 128, "a period of about 23 ms");
    info = ospace();
    check(info.fragments == 4 && info.fragstotal == 4 && info.fragsize == 128 && info.bytes == 512,
          "four free periods of 128 bytes");
    check(ask(DSP_GETTRIGGER, 0) == PCM_ENABLE_OUTPUT, "the trigger is on");
    check(ask(DSP_GETCAPS, 0) == (DSP_CAP_REVISION | DSP_CAP_REALTIME | DSP_CAP_TRIGGER),
          "realtime and trigger");
    check(ask(DSP_GETFMTS, 0) == (AFMT_U8 | AFMT_S16_LE), "U8 and S16_LE");
    check(ask(OSS_GETVERSION, 0) == 0x030802, "Linux's OSS version");
    check(odelay() == 0, "nothing queued");
    first = entry;
    check(open_dsp(1) == -EBUSY, "a second open is EBUSY");
    entry = first;
    close_dsp();
    check(m.starts == 0 && m.closes == 1, "a close with nothing queued plays nothing");
}

/** The opens that fail, and what they leave. */
static void row_refusals(void)
{
    begin("refusals");
    check(open_dsp(3) == -EINVAL, "an access mode of 3 is EINVAL");
    m.refuse_open = -EBUSY;
    check(open_dsp(1) == -EBUSY, "the driver's stream taken is EBUSY");
    check(dsp_owner == NULL, "a refused open leaves the device free");
    m.refuse_open = 0;
    kheap_host_largest = 1024;
    check(open_dsp(1) == -ENOMEM, "no room for the buffer is ENOMEM");
    check(open_dsp(0) == -ENOMEM, "nor for a reader's");
    check(open_dsp(2) == -ENOMEM, "nor for both");
    kheap_host_largest = 0;
    check(dsp_owner == NULL && m.s == NULL, "a failed allocation leaves the device free");
    check(open_dsp(1) == 0, "the next open succeeds");
    close_dsp();
    audio = &streamless;
    check(!dspdev_present(), "absent over a driver with no streams");
    check(open_dsp(1) == -ENODEV, "an open without streams is ENODEV");
    audio = NULL;
    check(!dspdev_present(), "absent with no driver");
    check(open_dsp(1) == -ENODEV, "an open with no driver is ENODEV");
}

/** Each setting's answer, the fragment requests and the errors. */
static void row_ioctls(void)
{
    struct dsp_buf_info info;
    int value;

    begin("ioctls");
    check(open_dsp(1) == 0, "open");
    check(ask(DSP_SETFMT, AFMT_S16_LE) == AFMT_S16_LE, "S16_LE set");
    check(ask(DSP_SETFMT, AFMT_S32_LE) == AFMT_S16_LE, "another format keeps S16_LE");
    check(ask(DSP_CHANNELS, 6) == 2, "six channels give two");
    check(ask(DSP_STEREO, 0) == 0 && ask(DSP_CHANNELS, 0) == 1, "STEREO 0 is mono");
    check(ask(DSP_STEREO, 1) == 1 && ask(DSP_CHANNELS, 0) == 2, "STEREO 1 is stereo");
    check(ask(DSP_SPEED, 44100) == 44100, "44100 Hz set");
    /* 176400 bytes a second over 43 is 4102, nearest 4096. */
    check(ask(DSP_GETBLKSIZE, 0) == 4096, "a period of 4096 bytes at 44100 Hz");
    info = ospace();
    check(info.fragments == 4 && info.fragstotal == 4 && info.fragsize == 4096 &&
              info.bytes == 16384,
          "four free periods of 4096 bytes");
    check(ask(DSP_SPEED, 96000) == 48000, "the driver's highest rate");
    check(ask(DSP_SPEED, 500) == 4000, "the driver's lowest rate");
    check(ask(DSP_SPEED, 44100) == 44100, "44100 Hz again");
    check(call(DSP_SETFRAGMENT, &(int){0x0004000A}) == 0, "four periods of 1024 bytes");
    info = ospace();
    check(ask(DSP_GETBLKSIZE, 0) == 1024 && info.fragstotal == 4 && info.bytes == 4096,
          "the buffer is four periods of 1024 bytes");
    check(call(DSP_SETFRAGMENT, &(int){0x7FFF0004}) == 0, "the most periods of 16 bytes");
    info = ospace();
    check(ask(DSP_GETBLKSIZE, 0) == (int)DSP_PERIOD_MIN &&
              info.fragstotal == (int)(DSP_BUFFER_MAX / DSP_PERIOD_MIN),
          "the smallest period, as many as the buffer holds");
    check(call(DSP_SETFRAGMENT, &(int){0x0001000C}) == 0, "one period of 4096 bytes");
    check(ospace().fragstotal == 2, "two periods at least");
    check(call(DSP_SETFRAGMENT, &(int){0x0002001F}) == 0, "periods of 2^31 bytes");
    check(ask(DSP_GETBLKSIZE, 0) == (int)DSP_PERIOD_MAX, "the largest period");
    m.period_max = 1024;
    check(call(DSP_SETFRAGMENT, &(int){0x0008000C}) == 0, "eight periods the driver halves");
    info = ospace();
    check(ask(DSP_GETBLKSIZE, 0) == 1024 && info.fragstotal == 8, "the driver's period, eight");
    m.period_max = MODEL_PERIOD_MAX;
    value = 0;
    check(call(DSP_SUBDIVIDE, &value) == -EINVAL, "another of OSS's requests is EINVAL");
    check(call(TIOCGWINSZ, &value) == -ENOTTY, "another request is ENOTTY");
    check(dspdev_ioctl(&entry, DSP_SPEED, 0) == -EFAULT, "a bad pointer is EFAULT");
    check(dspdev_ioctl(&entry, DSP_GETOSPACE, 0) == -EFAULT, "a bad GETOSPACE pointer is EFAULT");
    check(dspdev_write(&entry, NULL, 4) == -EFAULT, "a bad buffer is EFAULT");
    check(call(DSP_NONBLOCK, &value) == 0 && (description.status_flags & OFD_NONBLOCK),
          "NONBLOCK sets O_NONBLOCK");
    close_dsp();
    check(m.starts == 0, "nothing played");
}

/** Writes that block until the device has played room, and the counts
 * GETODELAY and GETOPTR give meanwhile. */
static void row_blocking(void)
{
    struct dsp_count_info optr;

    begin("blocking");
    check(open_dsp(1) == 0, "open");
    cd_quality();
    check(put(0, 3) == 3, "three bytes taken");
    check(m.starts == 0 && odelay() == 3, "three bytes queued, not started");
    check(put(3, 2045) == 2045, "two periods taken");
    check(m.starts == 1, "started at two periods, the driver's buffer");
    check(odelay() == 2048, "two periods in the driver's buffer");
    waits = 0;
    check(put(2048, 10000) == 10000, "a write of more than the buffer completes");
    check(waits > 0 && m.ticks == 6, "it waited for the six periods played");
    check(dspdev_ioctl(&entry, DSP_GETOPTR, (uintptr_t)&optr) == 0, "GETOPTR succeeds");
    check(optr.bytes == 6 * 1024 && optr.blocks == 6 && optr.ptr == 2 * 1024,
          "six periods played, the position in the third");
    check(dspdev_ioctl(&entry, DSP_GETOPTR, (uintptr_t)&optr) == 0 && optr.blocks == 0,
          "GETOPTR counts periods since the last");
    check(odelay() == 12048 - 6 * 1024, "the rest is queued");
    check(ask(DSP_SYNC, 0) == 0, "SYNC drains");
    check(!m.running && m.stops == 1, "SYNC stops the stream");
    check(played(0, 0, 12048), "everything written played, in order");
    check(silent_from(12048, AUDIO_S16LE), "then silence");
    check(odelay() == 0 && ospace().bytes == 4096, "the buffer is empty");
    check(dspdev_ioctl(&entry, DSP_GETOPTR, (uintptr_t)&optr) == 0, "GETOPTR succeeds");
    check(optr.bytes == (int)m.out_len && optr.ptr == 0, "every byte played is counted");
    close_dsp();
}

/** A partial frame waits for the write that completes it, across a period
 * the device played as silence; one left at SYNC is dropped. */
static void row_partial(void)
{
    static const uint32_t sizes[] = {1, 2, 5, 3, 7, 1000, 1033};
    uint32_t at = 0, i;

    begin("partial frames");
    check(open_dsp(1) == 0, "open");
    cd_quality();
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        check(put(at, sizes[i]) == (int32_t)sizes[i], "each piece taken");
        at += sizes[i];
    }
    check(at == 2051 && m.starts == 1, "started with three bytes over");
    check(odelay() == 2051, "a partial frame counts in the delay");
    model_tick();
    check(odelay() == 3 + 2048, "the device took silence and left the partial frame");
    check(put(2051, 1) == 1, "the byte that completes the frame");
    check(ask(DSP_SYNC, 0) == 0, "SYNC drains");
    check(played(0, 0, 2048), "the first two periods");
    check(silent_from(2048, AUDIO_S16LE) == 0, "more than silence follows");
    check(m.out_len >= 2048 + 1024 + 4, "the record reaches the late frame");
    check(played(2048 + 1024, 2048, 4), "the completed frame after the silent period");
    check(silent_from(2048 + 1024 + 4, AUDIO_S16LE), "then silence");
    check(put(0, 6) == 6, "a frame and a half more");
    check(ask(DSP_SYNC, 0) == 0 && odelay() == 0, "SYNC drops the half frame");
    close_dsp();
}

/** A signal ends a blocked write with what it took, or EINTR; O_NONBLOCK
 * gives EAGAIN; poll tells of a free period. */
static void row_interrupted(void)
{
    struct wait_queue *wq = NULL;
    uint32_t len;

    begin("interrupted");
    check(open_dsp(1) == 0, "open");
    cd_quality();
    signal_after = 0;
    check(put(0, 8192) == 6144, "a signal returns the bytes taken");
    signal_after = 0;
    check(put(6144, 100) == -EINTR, "a signal before any byte is EINTR");
    check(dspdev_poll(&entry, POLLOUT, &wq) == 0 && wq == &dsp_owner->wq,
          "no POLLOUT while full, on the open's queue");
    check(ask(DSP_NONBLOCK, 0) == 0, "NONBLOCK");
    check(put(6144, 100) == -EAGAIN, "a nonblocking write to a full buffer is EAGAIN");
    model_tick();
    check(dspdev_poll(&entry, POLLOUT | POLLIN, &wq) == POLLOUT, "POLLOUT with a period free");
    check(put(6144, 2000) == 1024, "a nonblocking write takes the room there is");
    signal_after = 1;
    check(ask(DSP_SYNC, 0) == -EINTR && m.running, "a signal ends SYNC with the stream running");
    len = m.out_len;
    check(ask(DSP_RESET, 0) == 0 && !m.running, "RESET stops the stream");
    check(odelay() == 0 && ospace().bytes == 4096, "RESET empties the buffer");
    check(played(0, 0, len) && m.out_len == len, "what played before RESET was in order");
    check(put(0, 2048) == 2048 && m.running, "a write after RESET starts again");
    len = m.out_len;
    close_dsp();
    check(m.out_len == len && m.stops == 2, "a nonblocking close stops without draining");
}

/** RESET drops what SYNC would have played; a close drains. */
static void row_drain_reset(void)
{
    uint32_t len;

    begin("drain against reset");
    check(open_dsp(1) == 0, "open");
    cd_quality();
    check(put(0, 3000) == 3000 && m.running, "started");
    check(ask(DSP_RESET, 0) == 0, "RESET");
    check(odelay() == 0 && m.out_len == 0, "nothing queued and nothing played");
    check(put(3000, 1000) == 1000 && !m.running, "less than the driver's buffer waits");
    check(ask(DSP_SYNC, 0) == 0, "SYNC starts and drains");
    check(played(0, 3000, 1000) && silent_from(1000, AUDIO_S16LE),
          "only what came after RESET played");
    check(ask(DSP_SYNC, 0) == 0 && m.starts == 2, "SYNC with nothing queued starts nothing");
    len = m.out_len;
    check(put(4000, 500) == 500, "a short write");
    close_dsp();
    check(played(len, 4000, 500) && silent_from(len + 500, AUDIO_S16LE), "the last close plays it");
}

/** SETTRIGGER holds and releases the stream; a start that fails. */
static void row_trigger(void)
{
    begin("trigger");
    check(open_dsp(1) == 0, "open");
    cd_quality();
    check(call(DSP_SETTRIGGER, &(int){0}) == 0 && ask(DSP_GETTRIGGER, 0) == 0, "trigger off");
    check(put(0, 3000) == 3000 && !m.running, "a write with the trigger off starts nothing");
    check(ask(DSP_POST, 0) == 0 && !m.running, "nor does POST");
    check(ask(DSP_SYNC, 0) == 0 && odelay() == 3000, "SYNC returns and keeps the data");
    check(call(DSP_SETTRIGGER, &(int){PCM_ENABLE_OUTPUT}) == 0 && m.running,
          "the trigger on starts the stream");
    check(odelay() == 3000, "with all of it queued");
    check(call(DSP_SETTRIGGER, &(int){0}) == 0 && !m.running, "the trigger off stops it");
    check(odelay() == 3000 - 2048, "the driver's buffer is dropped, the rest kept");
    check(ask(DSP_RESET, 0) == 0, "RESET");
    check(call(DSP_SETTRIGGER, &(int){PCM_ENABLE_OUTPUT}) == 0 && !m.running,
          "the trigger on with nothing queued starts nothing");
    m.refuse_start = -EIO;
    check(put(0, 2048) == 2048, "a write takes what it queued when the start fails");
    check(call(DSP_POST, &(int){0}) == -EIO, "POST gives the driver's error");
    check(put(2048, 2048) == 256, "the next write stops at its first failed start");
    m.refuse_start = 0;
    close_dsp();
    check(played(0, 0, 2048 + 256), "the close plays what was queued");
}

/** Play `n` bytes of the pattern in writes of `block` bytes, then close. */
static void play_blocks(uint32_t n, uint32_t block)
{
    uint32_t at;

    for (at = 0; at < n; at += block) {
        check(put(at, block) == (int32_t)block, "each block taken");
    }
    close_dsp();
    check(played(0, 0, n) && silent_from(n, AUDIO_S16LE), "the whole file played");
}

/** sox_ng's `play`: RESET, GETFMTS, SETFMT, STEREO, SPEED within 1%, SYNC,
 * then writes of its buffer. */
static void row_sox(void)
{
    begin("sox_ng");
    check(open_dsp(1) == 0, "open O_WRONLY");
    check(ask(DSP_RESET, 0) == 0, "RESET");
    check(ask(DSP_GETFMTS, 0) & AFMT_S16_LE, "GETFMTS has S16_LE");
    check(ask(DSP_SETFMT, AFMT_S16_LE) == AFMT_S16_LE, "SETFMT gives S16_LE back");
    check(ask(DSP_STEREO, 1) == 1, "STEREO gives 1 back");
    check(ask(DSP_SPEED, 44100) == 44100, "SPEED gives the rate back");
    waits = 0;
    check(ask(DSP_SYNC, 0) == 0 && m.ticks == 0, "SYNC with nothing written returns at once");
    play_blocks(8192 * 8, 8192);
}

/** FFmpeg's `-f oss` output: GETFMTS, SETFMT, STEREO, SPEED read back, then
 * writes of OSS_AUDIO_BLOCK_SIZE. */
static void row_ffmpeg(void)
{
    int formats;

    begin("FFmpeg");
    check(open_dsp(1) == 0, "the output's O_WRONLY open");
    formats = ask(DSP_GETFMTS, 0);
    check(formats & AFMT_S16_LE, "GETFMTS has S16_LE");
    check(ask(DSP_SETFMT, AFMT_S16_LE) == AFMT_S16_LE, "SETFMT");
    check(ask(DSP_STEREO, 1) == 1, "STEREO");
    check(ask(DSP_SPEED, 48000) == 48000, "SPEED read back");
    play_blocks(4096 * 20, 4096);
}

/** cmus's output: OSS_GETVERSION, RESET, CHANNELS, SAMPLESIZE, SPEED,
 * SETFRAGMENT of 32 periods of a 25th of a second's power of two, halved;
 * writes of whole frames while GETOSPACE gives (fragments - 1) periods of
 * room, waiting otherwise; POST to pause. */
static void row_cmus(void)
{
    struct dsp_buf_info info;
    uint32_t at = 0, n = 4096 * 40;
    int space, shift = 0;

    begin("cmus");
    check(open_dsp(1) == 0, "open O_WRONLY");
    check(ask(OSS_GETVERSION, 0) == 0x030802, "OSS_GETVERSION");
    check(ask(DSP_RESET, 0) == 0, "RESET");
    check(ask(DSP_CHANNELS, 2) == 2, "CHANNELS");
    check(ask(DSP_SETFMT, AFMT_S16_LE) == AFMT_S16_LE, "SAMPLESIZE, SETFMT's number");
    check(ask(DSP_SPEED, 44100) == 44100, "SPEED");
    while (1 << shift < 44100 * 4 / 25) {
        shift++;
    }
    shift--;
    check(call(DSP_SETFRAGMENT, &(int){(32 << 16) + shift}) == 0, "SETFRAGMENT");
    info = ospace();
    check(info.fragsize == 4096 && info.fragstotal == 16, "periods of 4096, as many as fit");
    while (at < n) {
        info = ospace();
        space = (info.fragments - 1) * info.fragsize;
        if (space <= 0) {
            model_tick();
            continue;
        }
        if ((uint32_t)space > n - at) {
            space = (int)(n - at);
        }
        space -= space % 4;
        check(put(at, (uint32_t)space) == space, "a write of the room there is");
        at += (uint32_t)space;
    }
    check(ask(DSP_POST, 0) == 0 && m.running, "POST to pause, still playing");
    close_dsp();
    check(played(0, 0, n) && silent_from(n, AUDIO_S16LE), "the whole file played");
}

/** An open for reading: OSS's defaults, the capture buffer's layout, and
 * the answers of the output's requests. */
static void row_capture_defaults(void)
{
    struct dsp_buf_info info;
    struct dsp_count_info optr;
    int value = 0;

    begin("capture defaults");
    check(open_dsp(0) == 0, "an open for reading succeeds");
    check(cap.s != NULL && m.s == NULL, "only the capture stream is taken");
    check(ask(DSP_SETFMT, AFMT_QUERY) == AFMT_U8, "U8 at open");
    check(ask(DSP_CHANNELS, 0) == 1, "mono at open");
    check(ask(DSP_SPEED, 0) == 8000, "8000 Hz at open");
    check(ask(DSP_GETBLKSIZE, 0) == 128, "a period of about 23 ms");
    info = ispace();
    check(info.fragments == 0 && info.fragstotal == 4 && info.fragsize == 128 && info.bytes == 0,
          "four periods, none recorded, not raised to the driver's eight");
    check(ask(DSP_GETTRIGGER, 0) == PCM_ENABLE_INPUT, "the input trigger is on");
    check(ask(DSP_GETCAPS, 0) == (DSP_CAP_REVISION | DSP_CAP_REALTIME | DSP_CAP_TRIGGER),
          "no duplex in an open for reading");
    check(call(DSP_SETDUPLEX, &value) == -EINVAL, "SETDUPLEX is EINVAL");
    check(dspdev_ioctl(&entry, DSP_GETOSPACE, (uintptr_t)&info) == -EINVAL, "GETOSPACE is EINVAL");
    check(call(DSP_GETODELAY, &value) == -EINVAL, "GETODELAY is EINVAL");
    check(dspdev_ioctl(&entry, DSP_GETOPTR, (uintptr_t)&optr) == -EINVAL, "GETOPTR is EINVAL");
    check(ask(DSP_SYNC, 0) == 0 && ask(DSP_POST, 0) == 0, "SYNC and POST do nothing");
    check(put(0, 4) == -EBADF, "a write is EBADF");
    check(cap.starts == 0, "nothing recorded before a read");
    close_dsp();
    check(cap.closes == 1 && cap.stops == 0, "a close of a stream never started");
}

/** Reads that wait for the device to record, in pieces of any size, and
 * the counts GETISPACE and GETIPTR give meanwhile. */
static void row_reading(void)
{
    struct dsp_buf_info info;
    struct dsp_count_info iptr;

    begin("reading");
    check(open_dsp(0) == 0, "open");
    cd_quality();
    waits = 0;
    check(take(3000) == 3000, "a read of almost three periods completes");
    check(cap.starts == 1 && cap.ticks == 3 && waits > 0,
          "it started the stream and waited for three periods");
    check(recorded(0, 0, 3000), "what was read is what was recorded, in order");
    info = ispace();
    check(info.fragments == 0 && info.fragstotal == 4 && info.fragsize == 1024 && info.bytes == 72,
          "the rest of the third period is ready");
    model_tick();
    model_tick();
    info = ispace();
    check(info.fragments == 2 && info.bytes == 2120, "two periods more are ready");
    check(take(2) == 2, "half a frame");
    check(take(2118) == 2118, "the rest, from within the frame");
    check(recorded(0, 0, 5120), "every byte in order");
    check(dspdev_ioctl(&entry, DSP_GETIPTR, (uintptr_t)&iptr) == 0, "GETIPTR succeeds");
    check(iptr.bytes == 5 * 1024 && iptr.blocks == 5 && iptr.ptr == 1024,
          "five periods recorded, the position in the second of the buffer");
    check(dspdev_ioctl(&entry, DSP_GETIPTR, (uintptr_t)&iptr) == 0 && iptr.blocks == 0,
          "GETIPTR counts periods since the last");
    close_dsp();
    check(cap.stops == 1, "the close stops the stream");
}

/** A reader that falls behind by the whole buffer keeps the oldest frames;
 * the device's newest are dropped in whole frames, even when a read left a
 * partial frame at the head. */
static void row_overrun(void)
{
    struct dsp_count_info iptr;
    int i;

    begin("overrun");
    check(open_dsp(0) == 0, "open");
    cd_quality();
    check(take(1024) == 1024, "the first period");
    for (i = 0; i < 6; i++) {
        model_tick();
    }
    check(ispace().bytes == 4096 && ispace().fragments == 4, "the buffer is full");
    check(take(2) == 2, "half a frame");
    model_tick();
    check(ispace().bytes == 4094, "no part of a frame is taken into the room left");
    check(take(4094) == 4094, "the rest");
    check(recorded(0, 0, 5120), "the oldest periods, in order");
    model_tick();
    check(take(1024) == 1024 && recorded(5120, 8192, 1024),
          "then the period recorded next, after the three dropped");
    check(dspdev_ioctl(&entry, DSP_GETIPTR, (uintptr_t)&iptr) == 0 && iptr.bytes == 9 * 1024,
          "GETIPTR counts the dropped periods as recorded");
    close_dsp();
}

/** A signal ends a blocked read with what it took, or EINTR; O_NONBLOCK
 * gives EAGAIN; poll for input starts recording and tells of a period. */
static void row_read_interrupted(void)
{
    struct wait_queue *wq = NULL;

    begin("read interrupted");
    check(open_dsp(0) == 0, "open");
    cd_quality();
    check(dspdev_poll(&entry, POLLIN | POLLOUT, &wq) == 0 && wq == &dsp_owner->wq,
          "nothing ready yet, on the open's queue");
    check(cap.running && cap.starts == 1, "a poll for input starts recording");
    signal_after = 0;
    check(take(100) == -EINTR, "a signal before any byte is EINTR");
    model_tick();
    check(dspdev_poll(&entry, POLLIN | POLLOUT, &wq) == POLLIN, "POLLIN with a period recorded");
    signal_after = 0;
    check(take(3000) == 1024, "a signal returns the bytes taken");
    check(ask(DSP_NONBLOCK, 0) == 0, "NONBLOCK");
    check(take(100) == -EAGAIN, "a nonblocking read of nothing is EAGAIN");
    model_tick();
    check(take(3000) == 1024, "a nonblocking read takes what there is");
    check(recorded(0, 0, 2048), "in order");
    model_tick();
    check(take(2) == 2, "half a frame");
    check(ask(DSP_RESET, 0) == 0 && !cap.running, "RESET stops recording");
    check(ispace().bytes == 0, "RESET empties the buffer");
    check(take(10) == -EAGAIN && cap.starts == 2, "a read after RESET starts again");
    close_dsp();
}

/** SETTRIGGER holds and releases recording; a start that fails. */
static void row_read_trigger(void)
{
    struct wait_queue *wq = NULL;

    begin("read trigger");
    check(open_dsp(0) == 0, "open");
    cd_quality();
    check(call(DSP_SETTRIGGER, &(int){0}) == 0 && ask(DSP_GETTRIGGER, 0) == 0, "trigger off");
    check(ask(DSP_NONBLOCK, 0) == 0, "NONBLOCK");
    check(take(10) == -EAGAIN && cap.starts == 0, "a read with input off starts nothing");
    check(dspdev_poll(&entry, POLLIN, &wq) == 0 && cap.starts == 0, "nor does a poll");
    check(call(DSP_SETTRIGGER, &(int){PCM_ENABLE_INPUT}) == 0 && cap.running,
          "the trigger on starts recording");
    check(ask(DSP_GETTRIGGER, 0) == PCM_ENABLE_INPUT, "GETTRIGGER reads it back");
    model_tick();
    check(call(DSP_SETTRIGGER, &(int){0}) == 0 && !cap.running, "the trigger off stops it");
    check(ispace().bytes == 1024, "what was recorded stays");
    check(take(4096) == 1024 && recorded(0, 0, 1024), "and is read");
    cap.refuse_start = -EIO;
    check(call(DSP_SETTRIGGER, &(int){PCM_ENABLE_INPUT}) == -EIO, "SETTRIGGER gives the error");
    check(take(10) == -EIO, "so does a read");
    cap.refuse_start = 0;
    close_dsp();
}

/** O_RDWR: both streams, set alike, each started by its own direction and
 * stopped by RESET; an open without both directions. */
static void row_duplex(void)
{
    uint32_t len;
    int value = 0;

    begin("duplex");
    check(open_dsp(2) == 0, "an open for reading and writing succeeds");
    check(m.s != NULL && cap.s != NULL, "both streams are taken");
    check(ask(DSP_GETCAPS, 0) ==
              (DSP_CAP_REVISION | DSP_CAP_REALTIME | DSP_CAP_TRIGGER | DSP_CAP_DUPLEX),
          "duplex among the caps");
    check(call(DSP_SETDUPLEX, &value) == 0, "SETDUPLEX succeeds");
    check(ask(DSP_GETTRIGGER, 0) == (PCM_ENABLE_INPUT | PCM_ENABLE_OUTPUT), "both triggers on");
    cd_quality();
    check(cap.s->format == AUDIO_S16LE && cap.s->channels == 2 && cap.s->rate == 44100 &&
              cap.s->period_bytes == 1024,
          "capture is set as playback is");
    check(ospace().fragstotal == 4 && ispace().fragstotal == 4, "four periods each way");
    check(put(0, 2048) == 2048 && m.running && !cap.running, "a write starts playback alone");
    check(take(2048) == 2048 && cap.starts == 1 && cap.ticks == 2, "a read starts capture");
    check(recorded(0, 0, 2048) && played(0, 0, 2048), "both directions moved the pattern");
    check(ask(DSP_RESET, 0) == 0 && !m.running && !cap.running, "RESET stops both");
    check(odelay() == 0 && ispace().bytes == 0, "and empties both");
    check(put(0, 1000) == 1000, "a short write");
    len = m.out_len;
    close_dsp();
    check(played(len, 0, 1000) && silent_from(len + 1000, AUDIO_S16LE), "the close plays it");
    check(m.closes == 1 && cap.closes == 1, "both streams given back");

    begin("duplex refused");
    cap.refuse_open = -ENODEV;
    check(open_dsp(2) == -ENODEV, "no capture stream is ENODEV");
    check(dsp_owner == NULL && m.s == NULL && m.closes == 1, "playback's stream is given back");
    check(open_dsp(0) == -ENODEV, "so is an open for reading");
    check(open_dsp(1) == 0, "an open for writing succeeds");
    check(call(DSP_SETDUPLEX, &value) == -EINVAL, "SETDUPLEX is EINVAL");
    check(ask(DSP_GETTRIGGER, 0) == PCM_ENABLE_OUTPUT, "the output trigger alone");
    check(dspdev_read(&entry, (char __user *)got, 4) == -EBADF, "a read is EBADF");
    close_dsp();
    cap.refuse_open = 0;
    m.refuse_open = -ENODEV;
    check(open_dsp(2) == -ENODEV && cap.s == NULL && cap.closes == 0,
          "no playback stream is ENODEV, before capture is taken");
    check(open_dsp(0) == 0, "an open for reading succeeds");
    close_dsp();
}

/** sox_ng's `rec`: RESET, GETFMTS, SETFMT, STEREO, SPEED, GETBLKSIZE for
 * its buffer, SYNC, then reads of a buffer each. */
static void row_sox_rec(void)
{
    uint32_t i;
    int block;

    begin("sox_ng rec");
    check(open_dsp(0) == 0, "open O_RDONLY");
    check(ask(DSP_RESET, 0) == 0, "RESET");
    check(ask(DSP_GETFMTS, 0) & AFMT_S16_LE, "GETFMTS has S16_LE");
    check(ask(DSP_SETFMT, AFMT_S16_LE) == AFMT_S16_LE, "SETFMT");
    check(ask(DSP_STEREO, 1) == 1, "STEREO");
    check(ask(DSP_SPEED, 44100) == 44100, "SPEED");
    block = ask(DSP_GETBLKSIZE, 0);
    check(block == 4096, "a period of 4096 bytes");
    check(ask(DSP_SYNC, 0) == 0 && cap.starts == 0, "SYNC returns and starts nothing");
    for (i = 0; i < 8; i++) {
        check(take((uint32_t)block) == block, "each buffer filled");
    }
    close_dsp();
    check(recorded(0, 0, 8 * 4096), "the whole recording, in order");
}

/** FFmpeg's `-f oss` input: O_RDONLY made nonblocking, GETFMTS, SETFMT,
 * STEREO, SPEED read back, then reads of OSS_AUDIO_BLOCK_SIZE, retried on
 * EAGAIN, each followed by GETISPACE for the packet's time. */
static void row_ffmpeg_in(void)
{
    int32_t rc;
    int tries = 0;

    begin("FFmpeg input");
    check(open_dsp(0) == 0, "open O_RDONLY");
    description.status_flags |= OFD_NONBLOCK;
    check(ask(DSP_GETFMTS, 0) & AFMT_S16_LE, "GETFMTS has S16_LE");
    check(ask(DSP_SETFMT, AFMT_S16_LE) == AFMT_S16_LE, "SETFMT");
    check(ask(DSP_STEREO, 1) == 1, "STEREO");
    check(ask(DSP_SPEED, 48000) == 48000, "SPEED read back");
    while (got_len < 4096 * 10 && tries++ < 100) {
        rc = take(4096);
        if (rc == -EAGAIN) {
            model_tick();
            continue;
        }
        check(rc == 4096, "a read of a whole block");
        check(ispace().bytes == 0, "GETISPACE after it");
    }
    check(got_len == 4096 * 10 && recorded(0, 0, got_len), "ten blocks, in order");
    close_dsp();
}

int main(void)
{
    uint32_t i;

    for (i = 0; i < PATTERN; i++) {
        pattern[i] = (uint8_t)(i * 7 + 1 + (i >> 8));
    }
    row_defaults();
    row_refusals();
    row_ioctls();
    row_blocking();
    row_partial();
    row_interrupted();
    row_drain_reset();
    row_trigger();
    row_sox();
    row_ffmpeg();
    row_cmus();
    row_capture_defaults();
    row_reading();
    row_overrun();
    row_read_interrupted();
    row_read_trigger();
    row_duplex();
    row_sox_rec();
    row_ffmpeg_in();
    if (failures) {
        printf("dsp-hosttest: %d failures\n", failures);
        return 1;
    }
    printf("dsp-hosttest: ok\n");
    return 0;
}
