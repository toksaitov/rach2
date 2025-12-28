/*
 * sb16.c - Sound Blaster 16 driver implementation
 *
 * Implements PCM audio playback using the SB16 DSP and ISA DMA.
 * Uses double-buffering for continuous playback.
 */

#include "sb16.h"
#include "dma.h"
#include "../kio.h"
#include "../serial.h"
#include "../isr.h"

/* Default SB16 configuration */
#define SB16_BASE           0x220
#define SB16_IRQ            5
#define SB16_DMA_8BIT       1
#define SB16_DMA_16BIT      5

/* IRQ vector (IRQ 5 = vector 32 + 5 = 37) */
#define SB16_IRQ_VECTOR     (32 + SB16_IRQ)

/* Current base port (set during probe) */
static uint16_t sb16_base = SB16_BASE;

/* DSP version */
static uint16_t dsp_version = 0;

/* Current audio configuration */
static uint32_t current_sample_rate = 0;
static audio_format_t current_format = AUDIO_U8;
static int current_channels = 1;
static bool is_16bit = false;

/* Playback state */
static bool playing = false;
static audio_callback_t user_callback = NULL;

/*
 * Double-buffering for DMA
 * Buffers must be in low memory (below 16MB) and not cross 64KB boundary
 * We use a single contiguous buffer split into two halves
 */
static uint8_t dma_buffer[SB16_BUFFER_SIZE * 2] __attribute__((aligned(16)));
static int current_buffer = 0;  /* Which half is currently playing */
static volatile uint32_t irq_count = 0;  /* IRQ counter for debugging */

/*
 * Ring buffer for audio buffering
 *
 * Decouples producer (Doom via AUDIO_WRITE) from consumer (DMA via IRQ).
 * This absorbs timing variations between Doom's frame rate and the fixed
 * DMA consumption rate, preventing stuttering and silence gaps.
 *
 * Size is 8 DMA buffer periods (~116ms at 11025 Hz) to absorb jitter.
 * With 160-sample DMA buffers, this holds ~4 Doom frames worth of audio.
 */
#define RING_BUFFER_SIZE (SB16_BUFFER_SIZE * 8)  /* 1280 samples = ~116ms at 11025 Hz */

static uint8_t ring_buffer[RING_BUFFER_SIZE];
static volatile uint32_t ring_read_ptr = 0;   /* Where IRQ reads from */
static volatile uint32_t ring_write_ptr = 0;  /* Where write() appends to */

/* Diagnostics */
static volatile uint32_t underrun_count = 0;  /* Times ring was empty/partial */
static volatile uint32_t underrun_samples = 0; /* Total silence samples inserted */

/*
 * Get number of samples available to read from ring buffer
 * Safe to call from both IRQ and syscall context
 */
static uint32_t ring_available(void)
{
    uint32_t w = ring_write_ptr;
    uint32_t r = ring_read_ptr;
    if (w >= r) {
        return w - r;
    }
    return RING_BUFFER_SIZE - r + w;
}

/*
 * Get number of samples that can be written to ring buffer
 * We keep one slot empty to distinguish full from empty
 */
static uint32_t ring_free(void)
{
    return RING_BUFFER_SIZE - 1 - ring_available();
}

/*
 * Write samples to ring buffer (called from syscall context)
 * Returns number of samples actually written
 */
static uint32_t ring_write(const uint8_t *data, uint32_t count)
{
    uint32_t free_space = ring_free();
    uint32_t to_write = (count < free_space) ? count : free_space;
    uint32_t i;
    uint32_t wp = ring_write_ptr;

    for (i = 0; i < to_write; i++) {
        ring_buffer[wp] = data[i];
        wp = (wp + 1) % RING_BUFFER_SIZE;
    }

    /* Update write pointer atomically after all writes complete */
    ring_write_ptr = wp;
    return to_write;
}

/*
 * Read samples from ring buffer into destination (called from IRQ context)
 * If not enough samples available, fills remainder with silence (128).
 * Always fills exactly 'count' bytes in destination.
 * Tracks underruns for diagnostics.
 */
static void ring_read_to_buffer(uint8_t *dest, uint32_t count)
{
    uint32_t avail = ring_available();
    uint32_t to_read = (count < avail) ? count : avail;
    uint32_t silence_needed = count - to_read;
    uint32_t i;
    uint32_t rp = ring_read_ptr;

    /* Read available samples from ring */
    for (i = 0; i < to_read; i++) {
        dest[i] = ring_buffer[rp];
        rp = (rp + 1) % RING_BUFFER_SIZE;
    }

    /* Update read pointer atomically after all reads complete */
    ring_read_ptr = rp;

    /* Fill remainder with silence if underrun */
    if (silence_needed > 0) {
        underrun_count++;
        underrun_samples += silence_needed;
        for (; i < count; i++) {
            dest[i] = 128;  /* 8-bit unsigned silence */
        }
    }
}

/*
 * Small delay for DSP timing
 */
static void sb16_delay(void)
{
    int i;
    for (i = 0; i < 10; i++) {
        inb(0x80);  /* Dummy port read for delay */
    }
}

/*
 * Wait for DSP to be ready to accept data
 */
static bool dsp_write_ready(void)
{
    int timeout = 10000;
    while (timeout-- > 0) {
        if (!(inb(sb16_base + SB16_WRITE_STATUS) & 0x80)) {
            return true;
        }
        sb16_delay();
    }
    return false;
}

/*
 * Wait for DSP to have data available
 */
static bool dsp_read_ready(void)
{
    int timeout = 10000;
    while (timeout-- > 0) {
        if (inb(sb16_base + SB16_READ_STATUS) & 0x80) {
            return true;
        }
        sb16_delay();
    }
    return false;
}

/*
 * Write a byte to the DSP
 */
static bool dsp_write(uint8_t value)
{
    if (!dsp_write_ready()) {
        serial_printf("SB16: DSP write timeout\n");
        return false;
    }
    outb(sb16_base + SB16_WRITE_DATA, value);
    return true;
}

/*
 * Read a byte from the DSP
 */
static uint8_t dsp_read(void)
{
    if (!dsp_read_ready()) {
        serial_printf("SB16: DSP read timeout\n");
        return 0;
    }
    return inb(sb16_base + SB16_READ_DATA);
}

/*
 * Reset the DSP
 */
static bool dsp_reset(void)
{
    int i, j;

    /* Write 1 to reset port */
    outb(sb16_base + SB16_RESET, 1);

    /* Wait at least 3 microseconds */
    for (i = 0; i < 100; i++) {
        inb(0x80);
    }

    /* Write 0 to reset port */
    outb(sb16_base + SB16_RESET, 0);

    /* Wait for data ready and check for 0xAA */
    for (i = 0; i < 100; i++) {
        if (dsp_read_ready()) {
            if (inb(sb16_base + SB16_READ_DATA) == 0xAA) {
                return true;
            }
        }
        /* Small delay */
        for (j = 0; j < 100; j++) {
            inb(0x80);
        }
    }

    return false;
}

/*
 * Write to mixer register
 */
static void mixer_write(uint8_t reg, uint8_t value)
{
    outb(sb16_base + SB16_MIXER_ADDR, reg);
    sb16_delay();
    outb(sb16_base + SB16_MIXER_DATA, value);
}

/*
 * Start auto-init DMA playback
 * Sets up DMA for the full double-buffer and starts continuous playback.
 * IRQs fire every half-buffer; DMA automatically cycles.
 */
static void sb16_start_auto_init(void)
{
    uint16_t block_size = SB16_BUFFER_SIZE - 1;  /* IRQ fires every block */
    uint32_t addr = (uint32_t)(uintptr_t)dma_buffer;

    /* Set up DMA for FULL double-buffer with auto-init flag */
    dma_start(SB16_DMA_8BIT, addr, SB16_BUFFER_SIZE * 2,
              DMA_MODE_SINGLE | DMA_MODE_READ | DMA_MODE_AUTOINIT);

    /*
     * DSP 4.xx 8-bit auto-init command (0xC6)
     * Format: command, mode, length_low, length_high
     * Mode: 0x00 = mono unsigned (what Doom uses)
     * Length: block size - 1 (when IRQs fire)
     */
    dsp_write(DSP_8BIT_OUT_AI);     /* 0xC6 */
    dsp_write(0x00);                /* Mode: mono, unsigned */
    dsp_write(block_size & 0xFF);
    dsp_write((block_size >> 8) & 0xFF);
}

/*
 * IRQ handler for SB16 (auto-init mode with ring buffer)
 *
 * In auto-init mode, the DMA controller automatically cycles through the
 * double-buffer. The IRQ fires when each half finishes. We:
 * 1. Acknowledge the interrupt
 * 2. Fill the finished DMA buffer from the ring buffer
 *
 * The ring buffer decouples Doom's write timing from DMA consumption,
 * absorbing timing jitter and preventing stuttering/silence gaps.
 */
static void sb16_irq_handler(isr_params_t *params)
{
    uint8_t *fill_buffer;
    (void)params;

    /* Acknowledge the DSP interrupt - MUST be done first */
    inb(sb16_base + SB16_INT_ACK_8BIT);

    if (!playing) {
        return;
    }

    irq_count++;

    /*
     * Determine which DMA buffer half just finished playing.
     * current_buffer indicates which half WAS playing (just finished).
     */
    fill_buffer = dma_buffer + (current_buffer * SB16_BUFFER_SIZE);

    /* Switch to track that the other buffer is now playing */
    current_buffer = 1 - current_buffer;

    /*
     * Fill the finished DMA buffer from the ring buffer.
     * ring_read_to_buffer() handles underrun by filling with silence.
     */
    ring_read_to_buffer(fill_buffer, SB16_BUFFER_SIZE);

    /* In auto-init mode, DMA continues automatically - no restart needed */
}

/*
 * Probe for SB16 at a specific base address
 */
bool sb16_probe(uint16_t base)
{
    sb16_base = base;
    if (!dsp_reset()) {
        return false;
    }

    /* Get DSP version */
    if (!dsp_write(DSP_GET_VERSION)) {
        return false;
    }

    dsp_version = (uint16_t)dsp_read() << 8;
    dsp_version |= dsp_read();

    /* SB16 has DSP version 4.xx */
    if ((dsp_version >> 8) < 4) {
        serial_printf("SB16: Found SB (version %d.%d), need SB16\n",
                      dsp_version >> 8, dsp_version & 0xFF);
        return false;
    }

    return true;
}

/*
 * Get DSP version
 */
uint16_t sb16_get_version(void)
{
    return dsp_version;
}

/*
 * Initialize SB16 driver
 */
static int sb16_init(void)
{
    /* Probe for SB16 at common base addresses */
    uint16_t bases[] = { 0x220, 0x240, 0x260, 0x280 };
    int i;
    bool found = false;

    for (i = 0; i < 4; i++) {
        if (sb16_probe(bases[i])) {
            found = true;
            break;
        }
    }

    if (!found) {
        return -1;
    }

    /* Register IRQ handler */
    register_isr_handler(SB16_IRQ_VECTOR, sb16_irq_handler);

    /* Reset mixer */
    mixer_write(MIXER_RESET, 0);

    /* Set master volume to maximum (0xFF = 100%) */
    mixer_write(MIXER_MASTER_VOL, 0xFF);
    mixer_write(MIXER_VOICE_VOL, 0xFF);

    /* Verify/set IRQ and DMA in mixer */
    /* IRQ 5 = bit 1, IRQ 7 = bit 2, IRQ 10 = bit 3 */
    mixer_write(MIXER_IRQ_SELECT, 0x02);  /* IRQ 5 */

    /* DMA 8-bit channel 1 = bit 1, DMA 16-bit channel 5 = bit 5 */
    mixer_write(MIXER_DMA_SELECT, 0x22);  /* DMA 1 and 5 */

    return 0;
}

/*
 * Shutdown SB16 driver
 */
static void sb16_shutdown(void)
{
    /* Stop any playing audio */
    if (playing) {
        sb16_driver.stop();
    }

    /* Turn off speaker */
    dsp_write(DSP_SPEAKER_OFF);

    /* Reset DSP */
    dsp_reset();
}

/*
 * Start audio playback (auto-init mode)
 */
static int sb16_start(uint32_t sample_rate, audio_format_t format, int channels)
{
    if (playing) {
        sb16_driver.stop();
    }

    current_sample_rate = sample_rate;
    current_format = format;
    current_channels = channels;
    is_16bit = (format == AUDIO_S16LE);

    /* Validate sample rate */
    if (sample_rate < 5000 || sample_rate > 44100) {
        serial_printf("SB16: Invalid sample rate %d\n", sample_rate);
        return -1;
    }

    /* Turn on speaker */
    dsp_write(DSP_SPEAKER_ON);

    /* Set sample rate using DSP 4.xx command (required before 0xC6) */
    dsp_write(DSP_SET_SAMPLE_RATE);
    dsp_write((sample_rate >> 8) & 0xFF);
    dsp_write(sample_rate & 0xFF);

    /* Fill both DMA buffer halves with silence initially */
    k_memset(dma_buffer, 128, SB16_BUFFER_SIZE * 2);

    /* Initialize ring buffer to empty */
    ring_read_ptr = 0;
    ring_write_ptr = 0;

    /* If callback is set, pre-fill both DMA halves directly */
    if (user_callback) {
        user_callback(dma_buffer, SB16_BUFFER_SIZE);
        user_callback(dma_buffer + SB16_BUFFER_SIZE, SB16_BUFFER_SIZE);
    }

    /* Reset state */
    irq_count = 0;
    current_buffer = 0;  /* Buffer 0 will play first */
    playing = true;

    /* Start auto-init playback (DMA + DSP) */
    sb16_start_auto_init();

    return 0;
}

/*
 * Stop audio playback (auto-init mode)
 *
 * For auto-init, we must:
 * 1. Send DSP_EXIT_8BIT (0xDA) to tell DSP to stop after current block
 * 2. Send DSP_STOP_8BIT (0xD0) to immediately pause
 * 3. Mask the DMA channel
 */
static void sb16_stop(void)
{
    if (!playing) {
        return;
    }

    playing = false;

    /* Exit auto-init mode: DSP will stop after current block */
    if (is_16bit) {
        dsp_write(DSP_EXIT_16BIT);
        dsp_write(DSP_STOP_16BIT);
    } else {
        dsp_write(DSP_EXIT_8BIT);   /* 0xDA - exit auto-init */
        dsp_write(DSP_STOP_8BIT);   /* 0xD0 - pause immediately */
    }

    /* Stop DMA channel */
    dma_stop(is_16bit ? SB16_DMA_16BIT : SB16_DMA_8BIT);

    /* Turn off speaker */
    dsp_write(DSP_SPEAKER_OFF);

    /* Reset diagnostics for next session */
    underrun_count = 0;
    underrun_samples = 0;
}

/*
 * Check if audio is playing
 */
static bool sb16_is_playing(void)
{
    return playing;
}

/*
 * Set audio callback
 */
static void sb16_set_callback(audio_callback_t fn)
{
    user_callback = fn;
}

/*
 * Write audio samples to the ring buffer
 *
 * Samples are queued in the ring buffer and will be consumed by the
 * IRQ handler when DMA buffers need refilling. This decouples Doom's
 * frame-based writes from the fixed DMA consumption rate.
 *
 * Returns number of samples written, or -1 on error.
 */
static int sb16_write(const uint8_t *buffer, size_t size)
{
    uint32_t written;

    if (!buffer || size == 0) {
        return -1;
    }

    if (!playing) {
        return -1;
    }

    /* Write samples to ring buffer */
    written = ring_write(buffer, (uint32_t)size);

    return (int)written;
}

/*
 * Set volume (0-100)
 */
static void sb16_set_volume(int volume)
{
    uint8_t val;

    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    /* Convert 0-100 to 0-255 for mixer */
    val = (uint8_t)((volume * 255) / 100);

    /* Set both left and right channels */
    mixer_write(MIXER_MASTER_VOL, (val & 0xF0) | ((val >> 4) & 0x0F));
    mixer_write(MIXER_VOICE_VOL, val);
}

/*
 * SB16 audio driver definition
 */
audio_driver_t sb16_driver = {
    .name = "Sound Blaster 16",
    .type = AUDIO_DRIVER_SB16,
    .init = sb16_init,
    .shutdown = sb16_shutdown,
    .start = sb16_start,
    .stop = sb16_stop,
    .is_playing = sb16_is_playing,
    .set_callback = sb16_set_callback,
    .write = sb16_write,
    .set_volume = sb16_set_volume
};
