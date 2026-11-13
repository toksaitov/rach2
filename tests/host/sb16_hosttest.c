/**
 * SB16 PCM frames, stream negotiation and progress, failed setup, callback
 * playback and the shell's scale against port stubs and a DMA position model.
 *
 * host-deps: rach2/sys/tick.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 */
#include "audio/sb16.c"
#include "audio/audio_debug.h"
#include "audio/hda.h"

#include "audio/audio_debug.c"

extern int printf(const char *format, ...);
static int failures;
static int busy_after = -1; /* the DSP is busy after this many writes; -1 never */
static int writes;          /* bytes written to the DSP */
static int resets;
static int dma_failure;
static int dma_active;
static int dma_starts;
static int stopped_channel;
static int callback_count;
static int immediate_irq;   /* the 8th DSP byte completes a block at once */
static uint32_t dma_offset; /* bytes already played in the current half */
static int interrupts_enabled = 1;
static int stream_periods;
static int dma_half; /* the half of the buffer the model's DMA reads */
static uint8_t commands[32];
static int scale_done;
static int scale_failed;
static int scale_stalled;
static int device_stopped;           /* the model's device takes no more frames */
static int scale_captured;           /* the primed halves are in scale_stream */
static int16_t scale_stream[120000]; /* the scale's left channel, in DMA order */
static long scale_length;
volatile uint64_t kernel_ticks;
audio_driver_t *audio = &sb16_driver;

#define CHECK(c, s)                             \
    do {                                        \
        if (!(c)) {                             \
            printf("sb16-test: fail: %s\n", s); \
            failures++;                         \
        }                                       \
    } while (0)

/** Save and mask the model's interrupts, including nested callback calls. */
uint32_t kirq_save(void)
{
    uint32_t flags = (uint32_t)interrupts_enabled;

    interrupts_enabled = 0;
    return flags;
}

/** Restore the model's interrupt flag. */
void kirq_restore(uint32_t flags)
{
    interrupts_enabled = (int)flags;
}

/** Deliver an interrupt with the processor's interrupt flag masked. */
static void model_irq(void)
{
    uint32_t flags = kirq_save();

    sb16_irq_handler(NULL);
    kirq_restore(flags);
}

/** Model a writable DSP, or one stuck after a chosen command byte. */
uint8_t inb(uint16_t port)
{
    if (port == sb16_base + SB16_WRITE_STATUS)
        return busy_after >= 0 && writes >= busy_after ? 0x80 : 0;
    if (port == sb16_base + SB16_READ_STATUS)
        return 0x80;
    if (port == sb16_base + SB16_READ_DATA)
        return 0xAA;
    return 0;
}

/** Record DSP setup and optionally complete its first block immediately. */
void outb(uint16_t port, uint8_t value)
{
    if (port == sb16_base + SB16_RESET && value == 1)
        resets++;
    if (port == sb16_base + SB16_WRITE_DATA) {
        if (writes < (int)sizeof(commands))
            commands[writes] = value;
        writes++;
        if (immediate_irq && writes == 8) {
            dma_half = 1;
            model_irq();
        }
    }
}

/** Supply DMA success or failure without using the host address as ISA DMA. */
int dma_start(uint8_t channel, uint32_t address, uint32_t length, uint8_t mode)
{
    (void)address;
    dma_starts++;
    CHECK(channel == (is_16bit ? 5 : 1), "dma channel matches format");
    CHECK(length == period_bytes * 2, "dma covers both halves");
    CHECK(mode == (DMA_MODE_SINGLE | DMA_MODE_READ | DMA_MODE_AUTOINIT), "dma mode");
    dma_active = !dma_failure;
    return dma_failure ? -1 : 0;
}

/** Give the count left when the model's DMA begins the half `dma_half`. */
int dma_remaining(uint8_t channel)
{
    uint32_t unit = is_16bit ? 2 : 1;

    CHECK(channel == (is_16bit ? 5 : 1), "position is read on the format's channel");
    return (int)(((uint32_t)(2 - dma_half) * period_bytes - dma_offset) / unit);
}

/** Record masking of the selected channel. */
void dma_stop(uint8_t channel)
{
    dma_active = 0;
    stopped_channel = channel;
}

/** These rows call the handler directly. */
void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}

/** Ignore driver diagnostics; assertions read state and command bytes. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Start each row with an idle model and no callback. */
static void reset_model(void)
{
    playing = false;
    play_stream = NULL;
    stream_configured = false;
    stream_frames = stream_last = stream_given = 0;
    dma_offset = 0;
    interrupts_enabled = 1;
    stream_periods = 0;
    user_callback = NULL;
    busy_after = -1;
    writes = resets = dma_failure = dma_active = callback_count = immediate_irq = 0;
    stopped_channel = -1;
    dma_starts = dma_half = 0;
    ring_read_ptr = ring_write_ptr = 0;
    ring_size = RING_BUFFER_SIZE;
}

/** Compare bytes without importing host definitions of kernel types. */
static int bytes_differ(const uint8_t *a, const uint8_t *b, size_t size)
{
    size_t i;
    for (i = 0; i < size; i++)
        if (a[i] != b[i])
            return 1;
    return 0;
}

/** Split every PCM frame at every byte, including a ring wrap. */
static void row_frames(void)
{
    uint8_t data[] = {0x34, 0x12, 0x78, 0x56, 0xBC, 0x9A, 0xF0, 0xDE};
    uint8_t out[8];
    unsigned int bits, channels, split, frame, i, wrap;

    for (bits = 0; bits < 2; bits++) {
        for (channels = 1; channels <= 2; channels++) {
            frame = (bits ? 2 : 1) * channels;
            for (wrap = 0; wrap < 2; wrap++) {
                for (split = 0; split < frame; split++) {
                    reset_model();
                    is_16bit = bits;
                    current_channels = channels;
                    ring_read_ptr = ring_write_ptr = wrap ? RING_BUFFER_SIZE - 1 : 0;
                    CHECK(ring_write(data, split) == split, "partial write accepted");
                    ring_read_to_buffer(out, sizeof(out));
                    CHECK(ring_available() == split, "partial frame stays queued");
                    for (i = 0; i < sizeof(out); i++)
                        CHECK(out[i] == (bits ? 0 : 128), "partial frame emits silence");
                    CHECK(ring_write(data + split, sizeof(data) - split) == sizeof(data) - split,
                          "frame completion accepted");
                    ring_read_to_buffer(out, sizeof(out));
                    CHECK(bytes_differ(data, out, sizeof(out)) == 0,
                          "completed frames keep byte order");
                    CHECK(ring_available() == 0, "completed frames consumed");
                }
            }
        }
    }
}

/** A capacity-limited write retains the final partial stereo frame. */
static void row_short_write(void)
{
    uint8_t data[RING_BUFFER_SIZE + 4];
    uint8_t out[RING_BUFFER_SIZE];
    unsigned int i;

    reset_model();
    is_16bit = true;
    current_channels = 2;
    for (i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)i;
    CHECK(ring_write(data, sizeof(data)) == RING_BUFFER_SIZE - 1, "capacity short write");
    ring_read_to_buffer(out, sizeof(out));
    CHECK(ring_available() == 3, "short write retains three bytes");
    CHECK(bytes_differ(data, out, RING_BUFFER_SIZE - 4) == 0, "short write complete frames");
    CHECK(ring_write(data + RING_BUFFER_SIZE - 1, 1) == 1, "short write completion");
    ring_read_to_buffer(out, 4);
    CHECK(bytes_differ(data + RING_BUFFER_SIZE - 4, out, 4) == 0, "short write final frame");
}

/** Every setup byte and DMA setup may fail; each failure must stop playback. */
static void row_setup(void)
{
    int bits, fail;
    uint8_t byte = 1;

    for (bits = 0; bits < 2; bits++) {
        for (fail = 0; fail <= 8; fail++) {
            reset_model();
            busy_after = fail < 8 ? fail : -1;
            dma_failure = fail == 8;
            CHECK(sb16_start(11025, bits ? AUDIO_S16LE : AUDIO_U8, 1) == -EIO,
                  "failed setup returns failure");
            CHECK(!sb16_is_playing() && !dma_active, "failed setup leaves playback stopped");
            CHECK(dma_starts == (fail < 4 ? 0 : 1), "failed setup does not start dma early");
            CHECK(stopped_channel == (bits ? 5 : 1), "failed setup masks dma channel");
            CHECK(resets == 1, "failed setup resets partial dsp command");
            CHECK(sb16_write(&byte, 1) == -EINVAL, "failed setup refuses samples");
            busy_after = -1;
            dma_failure = 0;
            writes = 0;
            CHECK(sb16_start(11025, bits ? AUDIO_S16LE : AUDIO_U8, 1) == 0,
                  "setup recovers after failure");
            CHECK(playing && dma_active && writes == 8, "successful setup starts hardware");
        }
    }
    reset_model();
    CHECK(sb16_start(4999, AUDIO_U8, 1) == -EINVAL && !sb16_is_playing(),
          "rate below 5000 refused");
}

/**
 * A period is 128 frames for each 11025 Hz of the rate or part of it, the
 * DSP's block is one period and the ring is eight.
 */
static void row_period(void)
{
    static const struct {
        uint32_t rate;
        audio_format_t format;
        int channels;
        uint32_t bytes;
    } rows[] = {{5000, AUDIO_U8, 1, 128},     {11025, AUDIO_U8, 1, 128},
                {11026, AUDIO_U8, 1, 256},    {22050, AUDIO_U8, 2, 512},
                {32000, AUDIO_S16LE, 1, 768}, {44100, AUDIO_S16LE, 2, 2048},
                {44100, AUDIO_U8, 3, 512}};
    unsigned int i;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        uint32_t unit = rows[i].format == AUDIO_S16LE ? 2 : 1;

        reset_model();
        CHECK(sb16_start(rows[i].rate, rows[i].format, rows[i].channels) == 0, "period start");
        CHECK(period_bytes == rows[i].bytes, "period follows rate and format");
        CHECK((uint32_t)(commands[6] | commands[7] << 8) == rows[i].bytes / unit - 1,
              "dsp block is one period");
        CHECK(ring_size == rows[i].bytes * 8, "ring holds eight periods");
        CHECK(period_bytes * 2 <= sizeof(dma_buffer), "both periods fit the buffer");
    }
}

/** Stamp each callback fill so repeated or silent halves can be detected. */
static void fill_callback(void *buffer, size_t size)
{
    callback_count++;
    CHECK(size == period_bytes, "callback fills one half");
    k_memset(buffer, callback_count, size);
}

/**
 * Callback data must continue beyond the two primed halves into the half the
 * DMA is not reading, wherever the interrupts find it: twice in one half, as
 * an interrupt does that is serviced after two blocks have completed, and
 * alternating.
 */
static void row_callback(void)
{
    static const int reading[] = {0, 1, 1, 0, 0, 0, 1, 0};
    unsigned int n;
    uint8_t queued = 0x55;

    reset_model();
    user_callback = fill_callback;
    immediate_irq = 1;
    CHECK(sb16_start(44100, AUDIO_S16LE, 2) == 0, "callback start");
    CHECK(callback_count == 3 && irq_count == 1, "immediate irq sees initialized playback");
    CHECK(dma_buffer[0] == 3 && dma_buffer[period_bytes] == 2,
          "immediate irq refills the half after the first block");
    CHECK(sb16_write(&queued, 1) == 1, "callback row queues sentinel");
    for (n = 0; n < sizeof(reading) / sizeof(reading[0]); n++) {
        uint8_t kept;

        dma_half = reading[n];
        kept = dma_buffer[(uint32_t)dma_half * period_bytes];
        model_irq();
        CHECK(dma_buffer[(uint32_t)(1 - dma_half) * period_bytes] == callback_count,
              "callback refills the half the dma is not reading");
        CHECK(dma_buffer[(uint32_t)dma_half * period_bytes] == kept,
              "refill leaves the half the dma reads");
    }
    CHECK(callback_count == 11, "callback continues after priming");
    CHECK(ring_available() == 1, "callback leaves ring untouched");
    sb16_stop();
    model_irq();
    CHECK(callback_count == 11, "stopped playback ignores refill");
    reset_model();
    CHECK(sb16_start(11025, AUDIO_U8, 1) == 0, "ring start");
    sb16_write(&queued, 1);
    model_irq();
    CHECK(dma_buffer[period_bytes] == queued && dma_buffer[period_bytes + 1] == 128,
          "ring refill remains available");
}

/** Stamp a stream half and require the driver's promised callback context. */
static void stream_transfer(audio_stream_t *s, uint8_t *buffer, size_t size)
{
    CHECK(s == play_stream, "transfer has the stream owner");
    CHECK(!interrupts_enabled, "transfer has interrupts masked");
    CHECK(size == s->period_bytes, "transfer is the granted period");
    fill_callback(buffer, size);
}

/** The period notification follows its refill, and never follows priming. */
static void stream_period(audio_stream_t *s)
{
    CHECK(s == play_stream && !interrupts_enabled, "period has owner and irq context");
    stream_periods++;
    CHECK(callback_count == stream_periods + 2, "period follows its transfer");
}

/** Give a row an ordinary playback request, with no device running yet. */
static void stream_request(audio_stream_t *s)
{
    k_memset(s, 0, sizeof(*s));
    s->dir = AUDIO_PLAYBACK;
    s->rate = 8000;
    s->format = AUDIO_U8;
    s->channels = 1;
    s->period_bytes = 128;
    s->periods = 4;
    s->transfer = stream_transfer;
    s->period = stream_period;
}

/** Check every format's grant, whole frames, and the static DMA limit. */
static void row_stream_setup(void)
{
    static const uint32_t requests[] = {1, 127, 128, 2047, 2048, 16384};
    audio_stream_t s, other;
    unsigned int bits, channels, n;

    reset_model();
    stream_request(&s);
    other = s;
    other.dir = AUDIO_CAPTURE;
    CHECK(sb16_driver.stream_open(&other) == -ENODEV, "capture is refused");
    CHECK(sb16_driver.stream_open(&s) == 0, "stream reserves playback");
    CHECK(sb16_driver.stream_open(&s) == -EBUSY, "stream cannot open twice");
    other.dir = AUDIO_PLAYBACK;
    CHECK(sb16_driver.stream_open(&other) == -EBUSY, "second stream is refused");
    CHECK(sb16_driver.stream_start(&s) == -EIO, "start needs a setup");
    for (bits = 0; bits < 2; bits++) {
        for (channels = 1; channels <= 2; channels++) {
            uint32_t frame = (bits ? 2u : 1u) * channels;

            for (n = 0; n < sizeof(requests) / sizeof(requests[0]); n++) {
                uint32_t bytes = requests[n] > SB16_PERIOD_MAX ? SB16_PERIOD_MAX : requests[n];
                int rc;

                s.format = bits ? AUDIO_S16LE : AUDIO_U8;
                s.channels = (int)channels;
                s.rate = bits ? 48000 : 1000;
                s.period_bytes = requests[n];
                s.periods = 1;
                bytes -= bytes % frame;
                rc = sb16_driver.stream_setup(&s);
                CHECK(rc == (bytes ? 0 : -EINVAL), "grant needs a complete frame");
                if (!bytes)
                    continue;
                CHECK(s.rate == (bits ? 44100u : 5000u), "rate clamps to device range");
                CHECK(s.period_bytes == bytes && s.periods == 2 && s.device_periods == 2,
                      "period clamps and rounds, both buffers have two periods");
                writes = 0;
                CHECK(sb16_driver.stream_start(&s) == 0, "granted stream starts");
                CHECK(period_bytes == bytes && period_bytes * 2 <= sizeof(dma_buffer),
                      "dma uses granted period within its static buffer");
                CHECK((uint32_t)(commands[6] | commands[7] << 8) == bytes / (bits ? 2u : 1u) - 1,
                      "dsp block matches granted period");
                CHECK(commands[0] == DSP_SPEAKER_ON && commands[1] == DSP_SET_SAMPLE_RATE &&
                          (uint32_t)(commands[2] << 8 | commands[3]) == s.rate &&
                          commands[4] == (bits ? DSP_16BIT_OUT_AI : DSP_8BIT_OUT_AI) &&
                          commands[5] == ((bits ? 0x10 : 0) | (channels == 2 ? 0x20 : 0)),
                      "dsp programs granted rate, signedness and channels");
                CHECK(sb16_driver.stream_setup(&s) == -EINVAL, "running setup is refused");
                sb16_driver.stream_stop(&s);
                dma_half = 0;
                callback_count = stream_periods = 0;
            }
        }
    }
    s.channels = 3;
    CHECK(sb16_driver.stream_setup(&s) == -EINVAL, "invalid channels refused");
    s.channels = 1;
    s.format = (audio_format_t)99;
    CHECK(sb16_driver.stream_setup(&s) == -EINVAL, "invalid format refused");
    s.format = AUDIO_S32LE;
    s.channels = 2;
    s.period_bytes = 1024;
    CHECK(sb16_driver.stream_setup(&s) == 0 && s.format == AUDIO_S16LE && s.period_bytes == 512,
          "a wider format is granted 16 bits, its period keeping its frames");
    s.channels = 1;
    s.format = AUDIO_U8;
    s.transfer = NULL;
    CHECK(sb16_driver.stream_setup(&s) == -EINVAL, "missing transfer refused");
    CHECK(sb16_driver.stream_setup(&other) == -EINVAL, "foreign setup refused");
    sb16_driver.stream_close(&other);
    CHECK(play_stream == &s, "foreign close leaves owner");
    sb16_driver.stream_close(&s);
    CHECK(sb16_start(11025, AUDIO_U8, 1) == 0, "close releases console playback");
    CHECK(sb16_driver.stream_open(&s) == -EBUSY, "console playback excludes streams");
    sb16_stop();
}

/** Position moves within periods and across wrap; stop excludes callbacks. */
static void row_stream_playback(void)
{
    audio_stream_t s, other;
    uint8_t byte = 7;
    uint32_t half;

    reset_model();
    stream_request(&s);
    s.rate = 44100;
    s.format = AUDIO_S16LE;
    s.channels = 2;
    s.period_bytes = 1024;
    other = s;
    CHECK(sb16_driver.stream_open(&s) == 0 && sb16_driver.stream_setup(&s) == 0,
          "playback open and setup");
    CHECK(sb16_start(11025, AUDIO_U8, 1) == -EBUSY, "stream excludes console start");
    sb16_set_callback(fill_callback);
    CHECK(!user_callback, "console cannot change stream callback");
    CHECK(sb16_driver.stream_start(&s) == 0, "stream starts");
    half = s.period_bytes / 4;
    CHECK(callback_count == 2 && !stream_periods && interrupts_enabled,
          "start primes twice without notification and restores interrupts");
    CHECK(dma_buffer[0] == 1 && dma_buffer[period_bytes] == 2, "primed halves are ordered");
    CHECK(sb16_driver.stream_position(&s) == 0 && sb16_driver.delay(&s) == 2 * half,
          "initial position and delay are in frames");
    dma_offset = 100;
    CHECK(sb16_driver.stream_position(&s) == 25 && sb16_driver.delay(&s) == 2 * half - 25,
          "position and delay move inside a period");
    dma_half = 1;
    dma_offset = 0;
    CHECK(sb16_driver.stream_position(&s) == half && sb16_driver.delay(&s) == half,
          "position sees the boundary before the interrupt");
    model_irq();
    CHECK(dma_buffer[0] == 3 && dma_buffer[period_bytes] == 2 && stream_periods == 1,
          "irq refills completed half then notifies");
    CHECK(sb16_driver.delay(&s) == 2 * half, "refill restores two periods ahead");
    dma_half = 0;
    dma_offset = 40;
    model_irq();
    CHECK(sb16_driver.stream_position(&s) == 2 * half + 10 &&
              sb16_driver.delay(&s) == 2 * half - 10,
          "wrapped position and late refill retain fractional progress");
    CHECK(dma_buffer[0] == 3 && dma_buffer[period_bytes] == 4, "wrapped irq keeps reading half");
    model_irq();
    CHECK(sb16_driver.delay(&s) == 2 * half - 10, "repeated refill replaces queued half");
    sb16_stop();
    CHECK(playing && sb16_write(&byte, 1) == -EBUSY, "console cannot stop or feed stream");
    sb16_driver.stream_stop(&other);
    CHECK(playing && sb16_driver.stream_position(&other) == 0 && !sb16_driver.delay(&other),
          "foreign stream cannot stop or read owner");
    sb16_driver.stream_stop(&s);
    CHECK(!playing && !dma_active && sb16_driver.stream_position(&s) == 2 * half + 10 &&
              sb16_driver.delay(&s) == 0,
          "stop masks dma and keeps final position");
    model_irq();
    CHECK(callback_count == 5 && stream_periods == 3, "stopped interrupt calls nobody");
    dma_half = 0;
    dma_offset = 0;
    callback_count = stream_periods = 0;
    CHECK(sb16_driver.stream_start(&s) == 0 && sb16_driver.stream_position(&s) == 0,
          "restart resets position and primes again");
    stream_frames = 0xFFFFFFF0u;
    dma_offset = 128;
    CHECK(sb16_driver.stream_position(&s) == 16,
          "frame counter wraps at 32 bits while dma advances");
    sb16_driver.stream_close(&s);
    model_irq();
    CHECK(!play_stream && !playing && callback_count == 2, "close stops and detaches owner");
    CHECK(sb16_driver.delay(NULL) == 0, "console delay operation is zero while stopped");
    CHECK(sb16_start(11025, AUDIO_U8, 1) == 0, "console starts after close");
    CHECK(sb16_driver.delay(NULL) == sb16_delay_frames(), "null stream reads console delay");
    sb16_stop();
}

/** Every stream setup failure releases the hardware and permits a retry. */
static void row_stream_failure(void)
{
    audio_stream_t s;
    int fail;

    for (fail = 0; fail <= 8; fail++) {
        reset_model();
        stream_request(&s);
        CHECK(sb16_driver.stream_open(&s) == 0 && sb16_driver.stream_setup(&s) == 0,
              "failure row reserves stream");
        busy_after = fail < 8 ? fail : -1;
        dma_failure = fail == 8;
        CHECK(sb16_driver.stream_start(&s) == -EIO && !playing && !dma_active,
              "failed stream start leaves hardware stopped");
        busy_after = -1;
        dma_failure = 0;
        writes = callback_count = stream_periods = 0;
        immediate_irq = 1;
        CHECK(sb16_driver.stream_start(&s) == 0 && stream_periods == 1 && callback_count == 3,
              "stream retries and handles an immediate irq");
        sb16_driver.stream_close(&s);
    }
}

/** Append the half `half` of the DMA buffer to the scale's stream. */
static void capture_half(int half)
{
    const int16_t *samples = (const int16_t *)(dma_buffer + (uint32_t)half * period_bytes);
    uint32_t i;

    for (i = 0; i < period_bytes / 4 && scale_length < (long)(sizeof(scale_stream) / 2); i++) {
        CHECK(samples[2 * i] == samples[2 * i + 1], "scale channels agree");
        scale_stream[scale_length++] = samples[2 * i];
    }
}

/**
 * Replace the idle wait with one timer tick, during which the model's DMA
 * completes the half it reads and moves to the other, unless the device has
 * stopped. Each half the interrupt refills joins the stream after the two
 * primed halves, so the stream is the scale as the device plays it.
 */
void idle_wait_irq_on(void)
{
    if (!scale_captured) {
        capture_half(0);
        capture_half(1);
        scale_captured = 1;
    }
    if (!device_stopped) {
        dma_half = 1 - dma_half;
        model_irq();
        capture_half(1 - dma_half);
    }
    kernel_ticks++;
}

/** Record the shell command's success and failure reports. */
void console_puts(const char *text)
{
    if (audio_str_equal(text, "Done."))
        scale_done++;
    if (audio_str_equal(text, "AUDIO_START failed."))
        scale_failed++;
}

/** Record a stall report; the rest is presentation text. */
void console_printf(const char *format, ...)
{
    if (audio_str_starts_with(format, "audio: stalled:"))
        scale_stalled++;
}

/** Name the driver selected by this model. */
const char *audio_get_driver_name(void)
{
    return audio->name;
}

/** The model always selects a PCM driver. */
bool audio_has_real_output(void)
{
    return true;
}

/** HDA diagnostics are outside the SB16 command rows. */
void hda_debug_dump(void)
{}
/** HDA diagnostics are outside the SB16 command rows. */
void hda_debug_dump_codec(void)
{}
/** HDA diagnostics are outside the SB16 command rows. */
void hda_debug_dump_pins(void)
{}
/** HDA diagnostics are outside the SB16 command rows. */
void hda_debug_dump_stats(void)
{}
/** HDA routing is outside the SB16 command rows. */
void hda_set_output_mode(int mode)
{
    (void)mode;
}
/** HDA routing is outside the SB16 command rows. */
int hda_get_output_mode(void)
{
    return 0;
}
/** No HDA stream exists in this model. */
bool hda_debug_get_stream_snapshot(hda_stream_debug_snapshot_t *out)
{
    (void)out;
    return false;
}

/** Start the scale command on an idle model whose device runs or stops. */
static void run_scale(int stopped)
{
    reset_model();
    kernel_ticks = 0;
    scale_done = scale_failed = scale_stalled = scale_captured = 0;
    scale_length = 0;
    device_stopped = stopped;
    CHECK(audio_debug_handle_command("audio test scale 2000"), "scale command recognized");
    CHECK(!playing && !dma_active && !user_callback, "scale stops and detaches callback");
}

/**
 * Run the scale command through setup, refills, and shutdown. At 44100 Hz
 * each note is 17640 frames of a square wave whose runs are its half period,
 * starting high, and a rest of three 512-frame periods follows it; the
 * command ends once the device has the last rest.
 */
static void row_scale(void)
{
    static const long half_periods[] = {84, 75, 66, 63, 56};
    long at = 0, note;

    run_scale(0);
    CHECK(current_sample_rate == 44100, "sb16 scale selects 44100 hz");
    CHECK(scale_done == 1 && scale_failed == 0 && scale_stalled == 0, "sb16 scale completes");
    CHECK(_scale_frames >= 5 * (17640 + 1536) && _scale_frames < 5 * (17640 + 1536) + 512,
          "scale generates its notes and rests and stops");
    for (note = 0; note < 5; note++) {
        long i;

        for (i = 0; i < 17640 && at + i < scale_length; i++) {
            int16_t want = (i / half_periods[note]) % 2 == 0 ? 12000 : -12000;

            if (scale_stream[at + i] != want)
                break;
        }
        CHECK(i == 17640, "scale plays each note's whole square wave");
        at += 17640;
        for (i = 0; i < 1536 && at + i < scale_length; i++) {
            if (scale_stream[at + i] != 0)
                break;
        }
        CHECK(i == 1536, "scale rests three periods after each note");
        at += 1536;
    }
    CHECK(scale_length >= at, "device plays the whole scale");
    run_scale(1);
    CHECK(scale_done == 0 && scale_stalled == 1, "stopped device makes scale report a stall");
    CHECK(kernel_ticks >= (4u * 5u * (17640u + 1536u) * TICK_HZ) / 44100u + TICK_HZ,
          "stall is reported after four lengths and a second");
}

/** Run every row. Returns 0 when every check passes, 1 otherwise. */
int main(void)
{
    row_frames();
    row_short_write();
    row_setup();
    row_period();
    row_callback();
    row_scale();
    row_stream_setup();
    row_stream_playback();
    row_stream_failure();
    printf("sb16-test: result: %s\n", failures ? "fail" : "pass");
    return failures ? 1 : 0;
}
