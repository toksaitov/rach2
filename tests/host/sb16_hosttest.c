/**
 * SB16 PCM frames, failed setup, and callback playback against port stubs.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 */
#include "audio/sb16.c"
#include "audio/audio_debug.h"
#include "audio/hda.h"

#include "audio/audio_debug.c"

extern int printf(const char *format, ...);
static int failures;
static int busy_after = -1;
static int writes;
static int resets;
static int dma_failure;
static int dma_active;
static int dma_starts;
static int stopped_channel;
static int callback_count;
static int immediate_irq;
static uint8_t commands[32];
static int scale_done;
static int scale_failed;
static unsigned int scale_notes;
volatile uint32_t kernel_ticks;
audio_driver_t *audio = &sb16_driver;

#define CHECK(c, s)                             \
    do {                                        \
        if (!(c)) {                             \
            printf("sb16-test: fail: %s\n", s); \
            failures++;                         \
        }                                       \
    } while (0)

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
        if (immediate_irq && writes == 8)
            sb16_irq_handler(NULL);
    }
}

/** Supply DMA success or failure without using the host address as ISA DMA. */
int dma_start(uint8_t channel, uint32_t address, uint32_t length, uint8_t mode)
{
    (void)address;
    dma_starts++;
    CHECK(channel == (is_16bit ? 5 : 1), "dma channel matches format");
    CHECK(length == SB16_BUFFER_SIZE * 2, "dma covers both halves");
    CHECK(mode == (DMA_MODE_SINGLE | DMA_MODE_READ | DMA_MODE_AUTOINIT), "dma mode");
    dma_active = !dma_failure;
    return dma_failure ? -1 : 0;
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
    user_callback = NULL;
    busy_after = -1;
    writes = resets = dma_failure = dma_active = callback_count = immediate_irq = 0;
    stopped_channel = -1;
    dma_starts = 0;
    ring_read_ptr = ring_write_ptr = 0;
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
            CHECK(sb16_start(11025, bits ? AUDIO_S16LE : AUDIO_U8, 1) == -1,
                  "failed setup returns failure");
            CHECK(!sb16_is_playing() && !dma_active, "failed setup leaves playback stopped");
            CHECK(dma_starts == (fail < 4 ? 0 : 1), "failed setup does not start dma early");
            CHECK(stopped_channel == (bits ? 5 : 1), "failed setup masks dma channel");
            CHECK(resets == 1, "failed setup resets partial dsp command");
            CHECK(sb16_write(&byte, 1) == -1, "failed setup refuses samples");
            busy_after = -1;
            dma_failure = 0;
            writes = 0;
            CHECK(sb16_start(11025, bits ? AUDIO_S16LE : AUDIO_U8, 1) == 0,
                  "setup recovers after failure");
            CHECK(playing && dma_active && writes == 8, "successful setup starts hardware");
        }
    }
}

/** Stamp each callback fill so repeated or silent halves can be detected. */
static void fill_callback(void *buffer, size_t size)
{
    callback_count++;
    CHECK(size == SB16_BUFFER_SIZE, "callback fills one half");
    k_memset(buffer, callback_count, size);
}

/** Callback data must continue beyond the two primed halves. */
static void row_callback(void)
{
    int n;
    uint8_t queued = 0x55;

    reset_model();
    user_callback = fill_callback;
    immediate_irq = 1;
    CHECK(sb16_start(44100, AUDIO_S16LE, 2) == 0, "callback start");
    CHECK(callback_count == 3 && irq_count == 1, "immediate irq sees initialized playback");
    CHECK(sb16_write(&queued, 1) == 1, "callback row queues sentinel");
    for (n = 0; n < 6; n++) {
        int half = current_buffer;
        sb16_irq_handler(NULL);
        CHECK(dma_buffer[half * SB16_BUFFER_SIZE] == callback_count,
              "callback refills completed half");
    }
    CHECK(callback_count == 9, "callback continues after priming");
    CHECK(ring_available() == 1, "callback leaves ring untouched");
    sb16_stop();
    sb16_irq_handler(NULL);
    CHECK(callback_count == 9, "stopped playback ignores refill");
    reset_model();
    CHECK(sb16_start(11025, AUDIO_U8, 1) == 0, "ring start");
    sb16_write(&queued, 1);
    sb16_irq_handler(NULL);
    CHECK(dma_buffer[0] == queued && dma_buffer[1] == 128, "ring refill remains available");
}

/** Replace the halt with one timer tick and twelve completed DMA blocks. */
void arch_halt(void)
{
    static const uint32_t half_periods[] = {84, 75, 66, 63, 56};
    unsigned int block, note = kernel_ticks / 40;

    CHECK(note < 5, "scale finishes within its tick count");
    if (note < 5) {
        CHECK(_tone_half_period == half_periods[note], "scale retunes all five notes");
        scale_notes |= 1u << note;
    }
    for (block = 0; block < 12; block++) {
        int half = current_buffer;
        int16_t *samples = (int16_t *)(dma_buffer + half * SB16_BUFFER_SIZE);
        sb16_irq_handler(NULL);
        CHECK(samples[0] == 12000 || samples[0] == -12000, "scale refills contain sound");
        CHECK(samples[0] == samples[1], "scale channels agree");
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

/** Ignore presentation text; the scale rows check state and samples. */
void console_printf(const char *format, ...)
{
    (void)format;
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

/** Run the scale command through setup, refills, and shutdown. */
static void row_scale(void)
{
    reset_model();
    kernel_ticks = scale_notes = 0;
    scale_done = scale_failed = 0;
    CHECK(audio_debug_handle_command("audio test scale 2000"), "scale command recognized");
    CHECK(current_sample_rate == 44100, "sb16 scale selects 44100 hz");
    CHECK(scale_done == 1 && scale_failed == 0, "sb16 scale completes");
    CHECK(scale_notes == 31 && irq_count == 2400, "scale continues through all notes");
    CHECK(!playing && !dma_active && !user_callback, "scale stops and detaches callback");
}

/** Select the frame, setup, or complete set of playback rows. */
int main(int argc, char **argv)
{
    row_frames();
    row_short_write();
    if (argc == 1 || argv[1][0] == 's') {
        row_setup();
    }
    if (argc == 1) {
        row_callback();
        row_scale();
    }
    printf("sb16-test: result: %s\n", failures ? "fail" : "pass");
    return failures ? 1 : 0;
}
