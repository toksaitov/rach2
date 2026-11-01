/**
 * /dev/dsp (audio/dsp.c) over a model driver: each ioctl's answer, blocking
 * and interrupted writes, partial frames, drain against reset, the trigger,
 * and the negotiation sequences of sox_ng's, FFmpeg's and cmus's OSS outputs.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * The test includes dsp.c whole. The model driver's DMA buffer has two
 * periods; a tick plays the one due, appends its bytes to `m.out`, refills it
 * through the stream's `transfer` and calls `period`, as a device's interrupt
 * would. A wait runs ticks until its condition holds, or ends with a signal
 * when a row asks for one, so `m.out` is exactly what a listener would hear.
 * The sequences are those of sox_ng's `src/oss.c` (ossinit), FFmpeg's
 * `libavdevice/oss.c` (ff_oss_audio_open) and `oss_enc.c`, and cmus's
 * `op/oss.c` (oss_open, oss_set_sf, oss_buffer_space, oss_pause).
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

/* The bytes the rows write, each different from its neighbours. */
static uint8_t pattern[PATTERN];

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

/** Play one period, as the device does, then refill it and tell the
 * stream. Nothing happens while the stream is stopped. */
static void model_tick(void)
{
    uint32_t n;

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
 * limit, and a DMA buffer of two periods. */
static int model_setup(audio_stream_t *s)
{
    check(s == m.s, "setup of the open stream");
    check(!m.running, "setup while stopped");
    check(s->format == AUDIO_U8 || s->format == AUDIO_S16LE, "a known format");
    check(s->channels == 1 || s->channels == 2, "one or two channels");
    s->rate = clamp(s->rate, m.rate_min, m.rate_max);
    if (s->period_bytes > m.period_max) {
        s->period_bytes = m.period_max;
    }
    s->device_periods = 2;
    return 0;
}

static int model_start(audio_stream_t *s)
{
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
    check(s == m.s && m.running, "stop of the running stream");
    check(!masked, "a stop with interrupts on, outside the open's lock");
    m.running = 0;
    m.stops++;
}

static void model_close(audio_stream_t *s)
{
    check(s == m.s, "close of the open stream");
    check(!m.running, "close after the stop");
    m.s = NULL;
    m.closes++;
}

static uint32_t model_position(audio_stream_t *s)
{
    check(s == m.s && m.running, "position of the running stream");
    return m.position;
}

static uint32_t model_delay(audio_stream_t *s)
{
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
        if (!m.running || n > 100000) {
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
    check(m.s == NULL, "no stream open between rows");
    k_memset(&m, 0, sizeof(m));
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
    check(m.s == NULL, "the stream is closed");
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
    check(open_dsp(0) == -ENXIO, "an open for reading is ENXIO");
    check(open_dsp(2) == -ENXIO, "an open for reading and writing is ENXIO");
    m.refuse_open = -EBUSY;
    check(open_dsp(1) == -EBUSY, "the driver's stream taken is EBUSY");
    check(dsp_owner == NULL, "a refused open leaves the device free");
    m.refuse_open = 0;
    kheap_host_largest = 1024;
    check(open_dsp(1) == -ENOMEM, "no room for the buffer is ENOMEM");
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
 * writes of OSS_AUDIO_BLOCK_SIZE; its input opens for reading. */
static void row_ffmpeg(void)
{
    int formats;

    begin("FFmpeg");
    check(open_dsp(0) == -ENXIO, "the input's O_RDONLY open is ENXIO");
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
    if (failures) {
        printf("dsp-hosttest: %d failures\n", failures);
        return 1;
    }
    printf("dsp-hosttest: ok\n");
    return 0;
}
