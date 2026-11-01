/**
 * /dev/dsp (audio/dsp.c) and its virtual channels (audio/vchan.c) over a
 * model driver: each ioctl's answer, blocking and interrupted writes and
 * reads, partial frames, drain against reset, the trigger, a reader that
 * falls behind, playback and capture on one open and its queries, the
 * negotiation sequences of sox_ng's, FFmpeg's and cmus's OSS outputs and of
 * sox_ng's and FFmpeg's OSS inputs, and several opens playing at once: their
 * mix, its clipping, the conversion of each open's format and rate, and one
 * player closing while another plays.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * The test includes dsp.c and vchan.c whole. Playback reaches the model
 * through the mix, which the model plays as 16-bit stereo at 48000 Hz, so a
 * single open at that rate and format reaches it unchanged. The model
 * driver's DMA buffer has two periods, of 256 frames unless a row asks for
 * more; a tick plays the one due, appends its bytes to `m.out`, refills it
 * through the stream's `transfer` and calls `period`, as a device's
 * interrupt would. Its capture stream records the pattern in order: a tick
 * hands the next period over through `transfer` and calls `period`. A wait runs
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
#include "audio/vchan.c"

extern int printf(const char *format, ...);
extern unsigned long kheap_host_largest;
extern int memcmp(const void *a, const void *b, unsigned long n);

/* The requests the rows use that dsp.c does not name. */
#define DSP_SUBDIVIDE 0xC0045009u
#define TIOCGWINSZ    0x5413u
#define AFMT_S32_LE   0x1000

#define MODEL_PERIOD_MAX 16384u
#define MODEL_MIX_PERIOD 1024u /* bytes of the mix's period, 256 frames */
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
    uint32_t period_max; /* the largest capture period it grants */
    uint32_t play_max;   /* the largest playback period it grants */
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
 * limit for the direction, and a DMA buffer of two periods for playback,
 * eight for capture. */
static int model_setup(audio_stream_t *s)
{
    int in = s->dir == AUDIO_CAPTURE;

    check(s == (in ? cap.s : m.s), "setup of the open stream");
    check(!(in ? cap.running : m.running), "setup while stopped");
    check(s->format == AUDIO_U8 || s->format == AUDIO_S16LE, "a known format");
    check(s->channels == 1 || s->channels == 2, "one or two channels");
    s->rate = clamp(s->rate, m.rate_min, m.rate_max);
    if (s->period_bytes > (in ? m.period_max : m.play_max)) {
        s->period_bytes = in ? m.period_max : m.play_max;
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
audio_driver_t *audio_hw;
static uint32_t caller = 1;

/** The modeled process calling the audio interface. */
uint32_t audio_process_id(void)
{
    return caller;
}

/** Install the dispatch driver as at boot. */
void audio_set_dispatch(audio_driver_t *d)
{
    audio = d ? d : audio_hw;
}

/* A second open, for the rows that play two at once. */
static fd_entry_t other;
static struct open_file other_description;

/** The wait queue of the open on `e`. */
static wait_queue_t *queue_of(fd_entry_t *e)
{
    return e->priv ? &((struct dsp *)e->priv)->wq : NULL;
}

/** Wait as the kernel does, with the model's ticks for the time that
 * passes: 0 once `cond` holds, or -EINTR at the signal a row asked for. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    int n = 0;

    check(wq == queue_of(&entry) || wq == queue_of(&other), "a wait on an open's queue");
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
    caller = 1;
    vchan_set_loopback(0);
    check(m.s == NULL && cap.s == NULL, "no stream open between rows");
    k_memset(&m, 0, sizeof(m));
    k_memset(&cap, 0, sizeof(cap));
    got_len = 0;
    m.rate_min = 4000;
    m.rate_max = 48000;
    m.period_max = MODEL_PERIOD_MAX;
    m.play_max = MODEL_MIX_PERIOD;
    audio = &model;
    audio_hw = &model;
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

/** Open /dev/dsp for writing on `other`. */
static int32_t open_other(void)
{
    k_memset(&other, 0, sizeof(other));
    k_memset(&other_description, 0, sizeof(other_description));
    other.ofd = &other_description;
    other_description.refcount = 1;
    return dspdev_open(&other, 1);
}

/** Open /dev/dsp for reading on `other`. */
static int32_t open_other_reader(void)
{
    k_memset(&other, 0, sizeof(other));
    k_memset(&other_description, 0, sizeof(other_description));
    other.ofd = &other_description;
    other_description.refcount = 1;
    return dspdev_open(&other, 0);
}

/** Swap `entry` and `other`, so that the helpers reach the other open. */
static void swap_opens(void)
{
    fd_entry_t e = entry;

    entry = other;
    other = e;
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

/** Set signed 16-bit stereo at 48000 Hz, the mix's own, and four periods of
 * 1024 bytes, 256 frames each; the stream starts at 2048 bytes, the two
 * periods of the mix the model's start takes. */
static void cd_quality(void)
{
    check(ask(DSP_SETFMT, AFMT_S16_LE) == AFMT_S16_LE, "S16_LE granted");
    check(ask(DSP_CHANNELS, 2) == 2, "stereo granted");
    check(ask(DSP_SPEED, 48000) == 48000, "48000 Hz granted");
    check(call(DSP_SETFRAGMENT, &(int){0x0004000A}) == 0, "SETFRAGMENT succeeds");
    check(ask(DSP_GETBLKSIZE, 0) == 1024, "a period of 1024 bytes");
}

/** Close the descriptor, as the last close of its description does. */
static void close_dsp(void)
{
    dspdev_close(&entry);
    check(entry.priv == NULL && mix.list == NULL, "the device is free after the close");
    check(m.s == NULL && cap.s == NULL, "the streams are closed");
}

/** OSS's defaults at open, the fixed answers, and a second open with
 * settings of its own. */
static void row_defaults(void)
{
    struct dsp_buf_info info;

    begin("defaults");
    m.play_max = 4096;
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
    check(open_other() == 0, "a second open succeeds");
    swap_opens();
    check(ask(DSP_SPEED, 0) == 8000 && ask(DSP_SPEED, 22050) == 22050, "with its own rate");
    swap_opens();
    check(ask(DSP_SPEED, 0) == 8000, "the first keeps its own");
    dspdev_close(&other);
    check(other.priv == NULL && m.s != NULL, "the first keeps the device after the second's close");
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
    check(mix.list == NULL && m.s == NULL, "a refused open leaves the device free");
    m.refuse_open = 0;
    kheap_host_largest = 1024;
    check(open_dsp(1) == -ENOMEM, "no room for the buffer is ENOMEM");
    check(open_dsp(0) == -ENOMEM, "nor for a reader's");
    check(open_dsp(2) == -ENOMEM, "nor for both");
    kheap_host_largest = 0;
    check(mix.list == NULL && m.s == NULL, "a failed allocation leaves the device free");
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
    m.play_max = 4096;
    check(open_dsp(1) == 0, "open");
    check(ask(DSP_SETFMT, AFMT_S16_LE) == AFMT_S16_LE, "S16_LE set");
    check(ask(DSP_SETFMT, AFMT_S32_LE) == AFMT_S16_LE, "another format keeps S16_LE");
    check(ask(DSP_CHANNELS, 6) == 2, "six channels give two");
    check(ask(DSP_STEREO, 0) == 0 && ask(DSP_CHANNELS, 0) == 1, "STEREO 0 is mono");
    check(ask(DSP_STEREO, 1) == 1 && ask(DSP_CHANNELS, 0) == 2, "STEREO 1 is stereo");
    check(ask(DSP_SPEED, 48000) == 48000, "48000 Hz set");
    /* 192000 bytes a second over 43 is 4465, nearest 4096. */
    check(ask(DSP_GETBLKSIZE, 0) == 4096, "a period of 4096 bytes at 48000 Hz");
    info = ospace();
    check(info.fragments == 4 && info.fragstotal == 4 && info.fragsize == 4096 &&
              info.bytes == 16384,
          "four free periods of 4096 bytes");
    check(ask(DSP_SPEED, 96000) == 96000, "a rate above the device's, which the mix converts");
    check(ask(DSP_SPEED, 400000) == 192000, "the highest rate, four times the mix's");
    check(ask(DSP_SPEED, 500) == 1000, "the lowest rate");
    check(ask(DSP_SPEED, 48000) == 48000, "48000 Hz again");
    check(call(DSP_SETFRAGMENT, &(int){0x0004000A}) == 0, "four periods of 1024 bytes");
    info = ospace();
    check(ask(DSP_GETBLKSIZE, 0) == 1024 && info.fragstotal == 8 && info.bytes == 8192,
          "the buffer is raised to the eight periods the mix's start takes");
    check(call(DSP_SETFRAGMENT, &(int){0x7FFF0004}) == 0, "the most periods of 16 bytes");
    info = ospace();
    check(ask(DSP_GETBLKSIZE, 0) == (int)DSP_PERIOD_MIN &&
              info.fragstotal == (int)(DSP_BUFFER_MAX / DSP_PERIOD_MIN),
          "the smallest period, as many as the buffer holds");
    check(call(DSP_SETFRAGMENT, &(int){0x0001000C}) == 0, "one period of 4096 bytes");
    check(ospace().fragstotal == 2, "two periods at least");
    check(call(DSP_SETFRAGMENT, &(int){0x0002001F}) == 0, "periods of 2^31 bytes");
    check(ask(DSP_GETBLKSIZE, 0) == 4096, "the frames of one period of the mix");
    check(ask(DSP_SPEED, 24000) == 24000, "24000 Hz");
    check(call(DSP_SETFRAGMENT, &(int){0x0008000C}) == 0, "eight periods the mix halves");
    info = ospace();
    check(ask(DSP_GETBLKSIZE, 0) == 2048 && info.fragstotal == 8,
          "half the mix's period, at half its rate, eight");
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
    check(dspdev_poll(&entry, POLLOUT, &wq) == 0 && wq == queue_of(&entry),
          "no POLLOUT while full, on the open's queue");
    check(ask(DSP_NONBLOCK, 0) == 0, "NONBLOCK");
    check(put(6144, 100) == -EAGAIN, "a nonblocking write to a full buffer is EAGAIN");
    model_tick();
    check(dspdev_poll(&entry, POLLOUT | POLLIN, &wq) == POLLOUT, "POLLOUT with a period free");
    check(put(6144, 2000) == 1024, "a nonblocking write takes the room there is");
    signal_after = 1;
    check(call(DSP_SYNC, &(int){0}) == -EINTR && m.running,
          "a signal ends SYNC with the stream running");
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
    check(ask(DSP_SPEED, 48000) == 48000, "SPEED gives the rate back");
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
 * room, waiting otherwise; POST to pause. The rate is the mix's, so that
 * the record is what was written, over a mix whose period holds the
 * fragment. */
static void row_cmus(void)
{
    struct dsp_buf_info info;
    uint32_t at = 0, n = 4096 * 40;
    int space, shift = 0;

    begin("cmus");
    m.play_max = 4096;
    check(open_dsp(1) == 0, "open O_WRONLY");
    check(ask(OSS_GETVERSION, 0) == 0x030802, "OSS_GETVERSION");
    check(ask(DSP_RESET, 0) == 0, "RESET");
    check(ask(DSP_CHANNELS, 2) == 2, "CHANNELS");
    check(ask(DSP_SETFMT, AFMT_S16_LE) == AFMT_S16_LE, "SAMPLESIZE, SETFMT's number");
    check(ask(DSP_SPEED, 48000) == 48000, "SPEED");
    while (1 << shift < 48000 * 4 / 25) {
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
    check(dspdev_poll(&entry, POLLIN | POLLOUT, &wq) == 0 && wq == queue_of(&entry),
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
    check(cap.s->format == AUDIO_S16LE && cap.s->channels == 2 && cap.s->rate == 48000 &&
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
    check(mix.list == NULL && m.s == NULL && m.closes == 1, "playback's stream is given back");
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

/**
 * The queries on a duplex open whose capture was granted another rate than
 * playback's: SPEED and CHANNELS of 0 and SETFMT of AFMT_QUERY or a format
 * the device lacks answer playback's values and reconfigure nothing, so
 * both streams keep running and keep their buffered data.
 */
static void row_duplex_query(void)
{
    int queued, held;

    begin("duplex query");
    m.rate_max = 44100;
    check(open_dsp(2) == 0, "an open for reading and writing succeeds");
    check(ask(DSP_SETFMT, AFMT_S16_LE) == AFMT_S16_LE && ask(DSP_CHANNELS, 2) == 2 &&
              ask(DSP_SPEED, 48000) == 48000,
          "playback is granted S16_LE stereo at 48000 Hz");
    check(cap.s->rate == 44100, "capture is granted the device's 44100 Hz");
    check(call(DSP_SETFRAGMENT, &(int){0x0004000A}) == 0, "SETFRAGMENT succeeds");
    check(put(0, 4096) == 4096 && take(1024) == 1024, "a write and a read start both");
    model_tick();
    queued = odelay();
    held = ispace().bytes;
    check(queued > 0 && held > 0 && m.running && cap.running,
          "both directions run with data buffered");
    check(ask(DSP_SPEED, 0) == 48000, "SPEED 0 answers playback's rate");
    check(ask(DSP_CHANNELS, 0) == 2, "CHANNELS 0 answers its channels");
    check(ask(DSP_SETFMT, AFMT_QUERY) == AFMT_S16_LE, "AFMT_QUERY answers its format");
    check(ask(DSP_SETFMT, AFMT_S32_LE) == AFMT_S16_LE, "a format the device lacks keeps it");
    check(odelay() == queued && ispace().bytes == held && m.running && cap.running &&
              m.stops == 0 && cap.stops == 0,
          "the queries stop nothing and keep both directions' data");
    check(take(1024) == 1024 && recorded(0, 0, 1024), "capture goes on");
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

/* The 16-bit stereo the mixing rows write: two players' samples. */
#define WAVE 2048u
static int16_t wave_a[2 * WAVE];
static int16_t wave_b[2 * WAVE];

/** Write `frames` frames of 16-bit stereo `wave` from frame `at` to the
 * open on `e`: the bytes taken. */
static int32_t put_wave(fd_entry_t *e, const int16_t *wave, uint32_t at, uint32_t frames)
{
    return dspdev_write(e, (const char __user *)(wave + 2 * at), frames * 4);
}

/** The frames in `m.out`, 16-bit stereo. */
static uint32_t out_frames(void)
{
    return m.out_len / 4;
}

/** Sample `ch` of frame `f` of `m.out`. */
static int out_sample(uint32_t f, int ch)
{
    const uint8_t *p = m.out + 4 * f + 2 * (uint32_t)ch;

    return (int16_t)(uint16_t)(p[0] | p[1] << 8);
}

/** `v` held to 16 bits, as the mix clips. */
static int clip16(int v)
{
    return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

/** Sample `ch` of a player's frame `f` of `wave`, which has `n` frames and
 * entered the mix at mix frame `join`: 0 outside them. */
static int player(const int16_t *wave, uint32_t n, uint32_t join, uint32_t f, int ch)
{
    return f < join || f - join >= n ? 0 : wave[2 * (f - join) + (uint32_t)ch];
}

/** Whether every frame of `m.out` is player `a`'s plus player `b`'s,
 * clipped; `a` leaves the mix at mix frame `a_end`. */
static int mixed(const int16_t *a, uint32_t a_n, uint32_t a_end, const int16_t *b, uint32_t b_n,
                 uint32_t b_join)
{
    uint32_t f;
    int ch;

    for (f = 0; f < out_frames(); f++) {
        for (ch = 0; ch < 2; ch++) {
            int want =
                clip16((f < a_end ? player(a, a_n, 0, f, ch) : 0) + player(b, b_n, b_join, f, ch));

            if (out_sample(f, ch) != want) {
                printf("dsp-hosttest: %s: frame %u channel %d is %d, not %d\n", row, f, ch,
                       out_sample(f, ch), want);
                return 0;
            }
        }
    }
    return 1;
}

/**
 * Two players at the mix's rate: A starts the mix with 512 frames, which
 * the model's start takes; B joins at the next frame of the mix with 600,
 * and A writes 1024 more. Each has its own delay. A's SYNC drains it while B
 * plays on, B's finds its frames played, and the mix stops with B's. The
 * record is their sum, clipped.
 */
static void two_players(const char *name)
{
    begin(name);
    check(open_dsp(1) == 0 && open_other() == 0, "two opens");
    cd_quality();
    swap_opens();
    cd_quality();
    swap_opens();
    check(put_wave(&entry, wave_a, 0, 512) == 2048 && m.starts == 1, "A starts the mix");
    check(put_wave(&other, wave_b, 0, 600) == 2400, "B's frames taken");
    check(m.starts == 1 && mix.made == 512, "B joins the running mix, after A's first 512");
    check(odelay() == 2048, "A's delay: its frames in the device");
    swap_opens();
    check(odelay() == 2400 && ospace().bytes == 4096 - 2400, "B's: its frames in its buffer");
    swap_opens();
    check(put_wave(&entry, wave_a, 512, 1024) == 4096, "A fills its buffer");
    check(ask(DSP_SYNC, 0) == 0 && m.running, "A drains while B plays");
    swap_opens();
    check(ask(DSP_SYNC, 0) == 0 && !m.running, "B's frames have played, and the mix stops");
    check(odelay() == 0, "B has nothing queued");
    dspdev_close(&entry);
    swap_opens();
    close_dsp();
    check(m.starts == 1 && m.stops == 1, "one start and one stop of the device");
    check(mixed(wave_a, 1536, 1536, wave_b, 600, 512), "A and B summed");
}

/** Two players' frames summed, in range: A alone, then A and B, then
 * silence. */
static void row_mixing(void)
{
    uint32_t i;

    for (i = 0; i < WAVE; i++) {
        wave_a[2 * i] = (int16_t)((int)(i * 37 % 24000) - 12000);
        wave_a[2 * i + 1] = (int16_t)(12000 - (int)(i * 53 % 24000));
        wave_b[2 * i] = (int16_t)((int)(i * 101 % 20000) - 10000);
        wave_b[2 * i + 1] = (int16_t)((int)(i * 7 % 2000) - 1000);
    }
    two_players("mixing");
}

/** A sum past 16 bits is held at the limit: 30000 and 20000 give 32767,
 * and their negatives -32768; a sum within range is not changed. */
static void row_clipping(void)
{
    uint32_t i;

    for (i = 0; i < WAVE; i++) {
        wave_a[2 * i] = (int16_t)(i % 2 ? 30000 : -30000);
        wave_a[2 * i + 1] = (int16_t)(i % 3 ? -32768 : 32767);
        wave_b[2 * i] = (int16_t)(i % 2 ? 20000 : -20000);
        wave_b[2 * i + 1] = (int16_t)(i % 3 ? 100 : -100);
    }
    two_players("clipping");
    check(out_sample(600, 0) == -32768 && out_sample(601, 0) == 32767,
          "the left held at both ends");
    check(out_sample(600, 1) == 32767 && out_sample(601, 1) == -32668,
          "the right held at the top, and within range below it");
}

/** Frame `i` of the bytes `in`, in `format` with `channels`, as 16-bit
 * channel `ch`; silence past `n` frames. */
static int source(const uint8_t *in, uint32_t n, audio_format_t format, int channels, uint32_t i,
                  int ch)
{
    const uint8_t *p;

    if (i >= n) {
        return 0;
    }
    if (channels == 1) {
        ch = 0;
    }
    if (format == AUDIO_U8) {
        return ((int)in[i * (uint32_t)channels + (uint32_t)ch] - 128) * 256;
    }
    p = in + 2 * (i * (uint32_t)channels + (uint32_t)ch);
    return (int16_t)(uint16_t)(p[0] | p[1] << 8);
}

/**
 * One player of `n` frames of the pattern in `format`, `channels` and
 * `rate`, drained: each frame `k` of the mix is the player's frame at
 * k * rate / 48000, interpolated between the two around it by the 14-bit
 * weight of the fraction, as vchan.c's resampler makes it.
 */
static void resampled(const char *name, audio_format_t format, int channels, uint32_t rate,
                      uint32_t n)
{
    uint32_t frame = (uint32_t)channels * (format == AUDIO_S16LE ? 2u : 1u);
    uint32_t k, i, ph, w;
    unsigned long long t;
    int ch, a, b, want, bad = 0;

    begin(name);
    check(open_dsp(1) == 0, "open");
    check(ask(DSP_SETFMT, format == AUDIO_U8 ? AFMT_U8 : AFMT_S16_LE) ==
              (format == AUDIO_U8 ? AFMT_U8 : AFMT_S16_LE),
          "the format granted");
    check(ask(DSP_CHANNELS, channels) == channels, "the channels granted");
    check(ask(DSP_SPEED, (int)rate) == (int)rate, "the rate granted");
    check(put(0, n * frame) == (int32_t)(n * frame), "every frame taken");
    check(ask(DSP_SYNC, 0) == 0 && !m.running, "SYNC drains and stops the mix");
    check(odelay() == 0, "nothing queued");
    close_dsp();
    check((unsigned long long)out_frames() * rate >= (unsigned long long)n * 48000,
          "the record covers every frame");
    for (k = 0; k < out_frames() && !bad; k++) {
        t = (unsigned long long)k * rate;
        i = (uint32_t)(t / 48000);
        ph = (uint32_t)(t % 48000);
        w = (ph << 14) / 48000;
        for (ch = 0; ch < 2; ch++) {
            a = source(pattern, n, format, channels, i, ch);
            b = source(pattern, n, format, channels, i + 1, ch);
            want = ph == 0 ? a : a + (b - a) * (int)w / 16384;
            if (out_sample(k, ch) != want) {
                printf("dsp-hosttest: %s: frame %u channel %d is %d, not %d\n", row, k, ch,
                       out_sample(k, ch), want);
                bad = 1;
            }
        }
    }
    check(!bad, "each frame of the mix interpolated from the player's");
}

/** Each format, channel count and rate converted to the mix's: up from
 * 8000 Hz unsigned 8-bit mono and from 22050 and 44100 Hz, and down from
 * 96000 and 192000 Hz, four times the mix's. */
static void row_resampling(void)
{
    resampled("resampling 8000 Hz U8 mono", AUDIO_U8, 1, 8000, 3000);
    resampled("resampling 22050 Hz S16 mono", AUDIO_S16LE, 1, 22050, 3000);
    resampled("resampling 44100 Hz S16 stereo", AUDIO_S16LE, 2, 44100, 5000);
    resampled("resampling 96000 Hz U8 stereo", AUDIO_U8, 2, 96000, 9000);
    resampled("resampling 192000 Hz S16 stereo", AUDIO_S16LE, 2, 192000, 9000);
}

/**
 * A player closed while another plays: A's nonblocking close takes it out
 * of the mix at the next frame made and leaves the device running; B plays
 * on unbroken, and a new open joins it.
 */
static void row_close_while_playing(void)
{
    uint32_t a_end;

    for (a_end = 0; a_end < WAVE; a_end++) {
        wave_a[2 * a_end] = (int16_t)((int)(a_end * 37 % 24000) - 12000);
        wave_a[2 * a_end + 1] = (int16_t)(12000 - (int)(a_end * 53 % 24000));
        wave_b[2 * a_end] = (int16_t)((int)(a_end * 101 % 20000) - 10000);
        wave_b[2 * a_end + 1] = (int16_t)((int)(a_end * 7 % 2000) - 1000);
    }
    begin("close while playing");
    check(open_dsp(1) == 0 && open_other() == 0, "two opens");
    cd_quality();
    swap_opens();
    cd_quality();
    swap_opens();
    check(put_wave(&entry, wave_a, 0, 512) == 2048, "A starts the mix");
    check(put_wave(&other, wave_b, 0, 1024) == 4096, "B joins it");
    check(put_wave(&entry, wave_a, 512, 1024) == 4096, "A fills its buffer");
    model_tick();
    model_tick();
    a_end = mix.made;
    check(a_end == 1024, "A and B mixed for two periods");
    check(ask(DSP_NONBLOCK, 0) == 0, "A nonblocking");
    dspdev_close(&entry);
    check(entry.priv == NULL && m.running && m.stops == 0, "A's close leaves the device running");
    check(open_dsp(1) == 0, "a new open while B plays");
    check(m.starts == 1 && m.closes == 0, "on the same stream of the device");
    dspdev_close(&entry);
    swap_opens();
    check(ask(DSP_SYNC, 0) == 0 && !m.running, "B drains, and the mix stops");
    close_dsp();
    check(m.starts == 1 && m.stops == 1 && m.closes == 1,
          "one start, stop and close of the device");
    check(mixed(wave_a, 1536, a_end, wave_b, 1024, 512), "A until its close, B throughout");
}

/** Two readers receive the same periods. One falls behind and resets;
 * neither event stops or discards the other reader's frames. */
static void row_two_readers(void)
{
    uint8_t second[1024];
    struct wait_queue *wq;
    uint32_t i;

    begin("two readers");
    check(open_dsp(0) == 0, "first reader");
    cd_quality();
    check(open_other() == 0, "temporary playback open");
    dspdev_close(&other);
    check(dspdev_open(&other, 0) == 0, "second reader shares the input");
    swap_opens();
    cd_quality();
    check(dspdev_poll(&entry, POLLIN, &wq) == 0, "second reader started");
    swap_opens();
    check(dspdev_poll(&entry, POLLIN, &wq) == 0, "first reader started");
    check(cap.starts == 1, "one hardware start for both readers");
    check(take(1024) == 1024, "first reader's first period");
    check(dspdev_read(&other, (char __user *)second, sizeof(second)) == sizeof(second) &&
              memcmp(got, second, sizeof(second)) == 0,
          "identical frames for both readers");
    for (i = 0; i < 8; i++) {
        check(take(1024) == 1024, "first reader keeps up while the second overruns");
    }
    swap_opens();
    check(ask(DSP_RESET, 0) == 0 && cap.running && cap.stops == 0,
          "reset of the second leaves the first running");
    dspdev_close(&entry);
    check(entry.priv == NULL && other.priv != NULL && cap.running && cap.stops == 0,
          "the second reader closes without stopping the first");
    swap_opens();
    check(take(1024) == 1024 && recorded(0, 0, got_len), "first reader has no gap");
    close_dsp();
    check(cap.stops == 1 && cap.closes == 1, "last reader releases the input");
}

/**
 * A reader that joins after an overrun skips only what the device recorded
 * before it joined and had not handed over: the frames the overrun lost lie
 * behind it on the device's timeline, so it reads the next period recorded.
 * One that joins while a period is in flight, which a later overrun loses,
 * reads the period after it, which it was not to skip.
 */
static void row_reader_after_overrun(void)
{
    uint8_t second[1024];
    struct wait_queue *wq;
    uint32_t n, pf, fresh;

    begin("reader after overrun");
    check(open_dsp(0) == 0, "first reader");
    cd_quality();
    check(take(1024) == 1024, "the first reader's first period");
    n = cap.s->period_bytes;
    pf = n / frame_bytes(cap.s);
    check(n == sizeof(second), "the input's period is a reader's");
    cap.at += 3 * n; /* an overrun loses three periods */
    cap.position += 3 * pf;
    cap.s->lost += 3 * pf;
    cap.position += pf; /* and the device records one it has not handed over */
    check(open_other_reader() == 0, "second reader");
    swap_opens();
    cd_quality();
    check(dspdev_poll(&entry, POLLIN, &wq) == 0, "the second reader starts");
    swap_opens();
    cap.position -= pf;
    cap_tick();
    fresh = cap.at;
    cap_tick();
    check(dspdev_read(&other, (char __user *)second, n) == (int32_t)n &&
              memcmp(second, pattern + fresh, n) == 0,
          "it skips the period in flight and reads the next one recorded");
    dspdev_close(&other);

    cap.position += pf; /* a period in flight */
    check(open_other_reader() == 0, "a third reader");
    swap_opens();
    cd_quality();
    check(dspdev_poll(&entry, POLLIN, &wq) == 0, "the third reader starts");
    swap_opens();
    cap.at += n; /* which an overrun loses */
    cap.s->lost += pf;
    fresh = cap.at;
    cap_tick();
    check(dspdev_read(&other, (char __user *)second, n) == (int32_t)n &&
              memcmp(second, pattern + fresh, n) == 0,
          "the lost period counts against its skip, and it reads the next");
    dspdev_close(&other);
    close_dsp();
}

/** A second reader converts stereo input to mono at half the rate without
 * changing the first reader's setup or samples. */
static void row_reader_conversion(void)
{
    struct wait_queue *wq;
    uint8_t mono[256];
    uint32_t i;

    begin("reader conversion");
    check(open_dsp(0) == 0, "first reader");
    cd_quality();
    k_memset(&other, 0, sizeof(other));
    k_memset(&other_description, 0, sizeof(other_description));
    other.ofd = &other_description;
    other_description.refcount = 1;
    check(dspdev_open(&other, 0) == 0, "second reader");
    swap_opens();
    check(ask(DSP_SETFMT, AFMT_S16_LE) == AFMT_S16_LE, "second reader signed 16 bit");
    check(ask(DSP_SPEED, 24000) == 24000, "second reader half rate");
    check(dspdev_poll(&entry, POLLIN, &wq) == 0, "start second reader");
    swap_opens();
    check(take(1024) == 1024 && recorded(0, 0, 1024), "first reader unchanged");
    check(dspdev_read(&other, (char __user *)mono, sizeof(mono)) == sizeof(mono),
          "second reader's converted period");
    for (i = 0; i < 128; i++) {
        uint32_t at = (2 * i + 1) * 4;
        int left = (int16_t)(uint16_t)(pattern[at] | pattern[at + 1] << 8);
        int right = (int16_t)(uint16_t)(pattern[at + 2] | pattern[at + 3] << 8);
        int value = (int16_t)(uint16_t)(mono[2 * i] | mono[2 * i + 1] << 8);

        check(value == (left + right) / 2, "mono from both channels at half rate");
    }
    dspdev_close(&other);
    close_dsp();
}

/** Per-process PCM scales only its opener's samples. A new process writing
 * an inherited open cannot take ownership of it by setting its own PCM. */
static void row_process_pcm(void)
{
    uint32_t i, join;

    begin("process PCM");
    check(open_dsp(1) == 0, "process one's channel");
    cd_quality();
    check(vchan_pcm(25) == 25, "first process level");
    caller = 2;
    check(open_other() == 0, "process two's channel");
    swap_opens();
    cd_quality();
    swap_opens();
    check(vchan_pcm(-1) == 100, "new process starts at full PCM");
    check(vchan_pcm(50) == 50, "second process level");
    caller = 1;
    check(vchan_pcm(-1) == 25, "first process unchanged");
    for (i = 0; i < 2 * WAVE; i++) {
        wave_a[i] = 8000;
        wave_b[i] = 12000;
    }
    check(put_wave(&entry, wave_a, 0, 1024) == 4096, "first player starts");
    join = mix.made;
    caller = 3;
    check(vchan_pcm(0) == 100, "inheritor has no channel of its own");
    check(put_wave(&other, wave_b, 0, 1024) == 4096, "inherited write keeps opener's PCM");
    check(ask(DSP_SYNC, 0) == 0, "first player drains");
    swap_opens();
    check(ask(DSP_SYNC, 0) == 0, "second player drains");
    dspdev_close(&entry);
    check(entry.priv == NULL && other.priv != NULL && m.closes == 0,
          "the second player closes without releasing the first's stream");
    swap_opens();
    close_dsp();
    for (i = 0; i < out_frames(); i++) {
        int want = (i < 1024 ? 2000 : 0) + (i >= join && i < join + 1024 ? 6000 : 0);

        check(out_sample(i, 0) == want && out_sample(i, 1) == want, "scaled sum");
    }
}

/** The console starts and stops a channel while a DSP player runs; neither
 * action restarts or stops the shared hardware. */
static void row_console_client(void)
{
    begin("console client");
    check(open_dsp(1) == 0, "DSP open");
    cd_quality();
    check(put(0, 2048) == 2048, "DSP starts");
    check(vchan_driver.start(48000, AUDIO_S16LE, 2) == 0, "console joins");
    check(vchan_driver.write(pattern, 1024) == 1024, "console write");
    check(vchan_driver.delay(NULL) >= 256, "console delay includes its ring");
    check(m.starts == 1, "no restart when the console joins");
    model_tick();
    vchan_driver.stop();
    check(m.running && m.stops == 0, "console stop leaves DSP playing");
    check(ask(DSP_SYNC, 0) == 0, "DSP drains");
    close_dsp();
}

/** The clipped mix feeds both loopback readers, including on a device
 * with no capture stream; the playback clock lives until its last user. */
static void row_loopback(void)
{
    struct wait_queue *wq;
    uint32_t i;

    begin("loopback");
    vchan_set_loopback(1);
    cap.refuse_open = -ENODEV;
    check(open_dsp(2) == 0, "duplex on playback-only hardware");
    cd_quality();
    check(dspdev_poll(&entry, POLLIN, &wq) >= 0, "loopback clock starts");
    check(cap.s == NULL && m.running, "playback alone clocks loopback");
    for (i = 0; i < 2 * WAVE; i++)
        wave_a[i] = 12345;
    check(put_wave(&entry, wave_a, 0, 1024) == 4096, "known samples queued");
    check(take(4096) == 4096, "initial clock periods read");
    check(take(4096) == 4096, "played samples read");
    for (i = 2048; i < 6144; i += 2)
        check((int16_t)(uint16_t)(got[i] | got[i + 1] << 8) == 12345,
              "loopback is the mixed output");
    check(ask(DSP_NONBLOCK, 0) == 0, "nonblocking close");
    close_dsp();
    check(!m.running && m.s == NULL, "last loopback user releases the clock");
    vchan_set_loopback(0);
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
    row_duplex_query();
    row_sox_rec();
    row_ffmpeg_in();
    row_mixing();
    row_clipping();
    row_resampling();
    row_close_while_playing();
    row_two_readers();
    row_reader_after_overrun();
    row_reader_conversion();
    row_process_pcm();
    row_console_client();
    row_loopback();
    if (failures) {
        printf("dsp-hosttest: %d failures\n", failures);
        return 1;
    }
    printf("dsp-hosttest: ok\n");
    return 0;
}
