/*
 * Sound Blaster 16 driver. PCM playback through the SB16 DSP and ISA DMA,
 * double-buffered for continuous output.
 */

#include "../klog.h"
#include "sb16.h"
#include "dma.h"
#include "../kio.h"
#include "../serial.h"
#include "../isr.h"

#define SB16_BASE           0x220
#define SB16_IRQ            5
#define SB16_DMA_8BIT       1
#define SB16_DMA_16BIT      5

/* PIC remaps the ISA IRQs to vectors 32..47, so IRQ 5 is vector 37. */
#define SB16_IRQ_VECTOR     (32 + SB16_IRQ)

static uint16_t sb16_base = SB16_BASE;

static uint16_t dsp_version = 0;

static uint32_t current_sample_rate = 0;
static audio_format_t current_format = AUDIO_U8;
static int current_channels = 1;
static bool is_16bit = false;

static bool playing = false;
static audio_callback_t user_callback = NULL;

/*
 * DMA double-buffer: one contiguous region split into two halves. ISA DMA
 * requires the buffer to lie below 16MB and not cross a 64KB boundary.
 */
static uint8_t dma_buffer[SB16_BUFFER_SIZE * 2] __attribute__((aligned(16)));
static int current_buffer = 0;  /* half the DMA is currently playing */
static volatile uint32_t irq_count = 0;

/*
 * Ring buffer that decouples the producer (Doom via AUDIO_WRITE) from the
 * consumer (DMA via IRQ), absorbing timing jitter between Doom's frame rate
 * and the fixed DMA consumption rate. Eight DMA periods deep (~116ms at
 * 11025 Hz, about four Doom frames with 160-sample DMA buffers).
 */
#define RING_BUFFER_SIZE (SB16_BUFFER_SIZE * 8)

static uint8_t ring_buffer[RING_BUFFER_SIZE];
static volatile uint32_t ring_read_ptr = 0;   /* IRQ reads here */
static volatile uint32_t ring_write_ptr = 0;  /* write() appends here */

static volatile uint32_t underrun_count = 0;
static volatile uint32_t underrun_samples = 0;

/**
 * Return the number of samples queued in the ring buffer.
 *
 * Safe to call from both IRQ and syscall context: it samples the volatile
 * pointers once and reads no shared state that the other side may tear.
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

/**
 * Return the number of samples that can be appended to the ring buffer.
 *
 * One slot is always left empty so a full buffer is distinguishable from an
 * empty one.
 */
static uint32_t ring_free(void)
{
    return RING_BUFFER_SIZE - 1 - ring_available();
}

/**
 * Append up to `count` samples to the ring buffer, dropping any overflow.
 *
 * Called from syscall context (the sole writer). The write pointer is
 * published only after every byte lands, so the IRQ-side reader never sees a
 * half-written sample.
 *
 * @return the number of samples actually stored.
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

    ring_write_ptr = wp;
    return to_write;
}

/**
 * Drain `count` samples from the ring buffer into `dest`, padding with silence.
 *
 * Called from IRQ context (the sole reader). It always fills exactly `count`
 * bytes: a short ring is an underrun, counted in the diagnostics and made up
 * with the format's zero level. The read pointer is published only after the
 * copy completes. Silence is 0 for 16-bit signed and 128 for 8-bit unsigned,
 * each the midpoint of its range.
 */
static void ring_read_to_buffer(uint8_t *dest, uint32_t count)
{
    uint32_t avail = ring_available();
    uint32_t to_read = (count < avail) ? count : avail;
    uint32_t silence_needed = count - to_read;
    uint32_t i;
    uint32_t rp = ring_read_ptr;

    for (i = 0; i < to_read; i++) {
        dest[i] = ring_buffer[rp];
        rp = (rp + 1) % RING_BUFFER_SIZE;
    }

    ring_read_ptr = rp;

    if (silence_needed > 0) {
        underrun_count++;
        underrun_samples += silence_needed;
        if (is_16bit) {
            for (; i < count; i++) {
                dest[i] = 0;
            }
        } else {
            for (; i < count; i++) {
                dest[i] = 128;
            }
        }
    }
}

/**
 * Spin for a few microseconds to let the DSP settle between accesses.
 *
 * Reads of port 0x80 (the POST diagnostic port) are the classic ISA-bus way
 * to burn a short, bus-paced delay.
 */
static void sb16_delay(void)
{
    int i;
    for (i = 0; i < 10; i++) {
        inb(0x80);
    }
}

/**
 * Poll the write-status port until the DSP can accept a byte, or time out.
 *
 * Bit 7 of the write-status port is set while the DSP buffer is full.
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

/**
 * Poll the read-status port until the DSP has a byte ready, or time out.
 *
 * Bit 7 of the read-status port is set when output data is available.
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

/**
 * Send one command or data byte to the DSP after waiting for readiness.
 *
 * @return false if the DSP never drained, leaving the byte unsent.
 */
static bool dsp_write(uint8_t value)
{
    if (!dsp_write_ready()) {
        klog(KLOG_WARN, "sb16", "dsp: write timed out");
        return false;
    }
    outb(sb16_base + SB16_WRITE_DATA, value);
    return true;
}

/**
 * Read one byte from the DSP, returning 0 if none arrives in time.
 */
static uint8_t dsp_read(void)
{
    if (!dsp_read_ready()) {
        klog(KLOG_WARN, "sb16", "dsp: read timed out");
        return 0;
    }
    return inb(sb16_base + SB16_READ_DATA);
}

/**
 * Reset the DSP and wait for its 0xAA ready acknowledgement.
 *
 * The reset protocol is: write 1 to the reset port, hold it for at least 3us,
 * write 0, then poll the read port for the 0xAA byte the DSP emits once it has
 * come back up.
 *
 * @return true if 0xAA arrived, false on timeout (no SB16 at this base).
 */
static bool dsp_reset(void)
{
    int i, j;

    outb(sb16_base + SB16_RESET, 1);

    for (i = 0; i < 100; i++) {
        inb(0x80);
    }

    outb(sb16_base + SB16_RESET, 0);

    for (i = 0; i < 100; i++) {
        if (dsp_read_ready()) {
            if (inb(sb16_base + SB16_READ_DATA) == 0xAA) {
                return true;
            }
        }
        for (j = 0; j < 100; j++) {
            inb(0x80);
        }
    }

    return false;
}

/**
 * Write `value` to mixer register `reg`.
 *
 * The mixer is a register file behind an address/data port pair: latch the
 * register index, settle, then write the datum.
 */
static void mixer_write(uint8_t reg, uint8_t value)
{
    outb(sb16_base + SB16_MIXER_ADDR, reg);
    sb16_delay();
    outb(sb16_base + SB16_MIXER_DATA, value);
}

/**
 * Program DMA and the DSP for auto-init playback of the double-buffer.
 *
 * DMA covers the full double-buffer; the DSP block size is one half, so an
 * IRQ fires each time a half completes while the DMA controller cycles the
 * buffer automatically. The DSP transfer command, the mode byte, and the
 * block-size unit all depend on the active format:
 *
 *   8-bit  uses DMA channel 1, command 0xC6, block size counted in bytes.
 *   16-bit uses DMA channel 5, command 0xB6, block size counted in words.
 *
 * Mode byte: bit 4 = signed sample encoding, bit 5 = stereo. The block-size
 * register has the count minus one.
 */
static void sb16_start_auto_init(void)
{
    uint32_t addr = (uint32_t)(uintptr_t)dma_buffer;
    uint16_t block_size;
    uint8_t mode;
    int dma_channel;
    uint8_t dsp_cmd;

    /*
     * The DSP block-size register counts transfer units per IRQ (one byte
     * for 8-bit DMA, one 16-bit word for 16-bit DMA), set to one DMA half.
     * Stereo only interleaves L/R into consecutive units (signalled by the
     * mode bit), so it does NOT change the unit count: halving block_size
     * again for stereo would fire the DSP IRQ twice per half, out of phase
     * with the DMA, and the handler would refill the half the DAC is still
     * reading.
     */
    if (is_16bit) {
        dma_channel = SB16_DMA_16BIT;
        dsp_cmd = DSP_16BIT_OUT_AI;
        mode = 0x10;  /* signed */
        block_size = (SB16_BUFFER_SIZE / 2) - 1;  /* one half, in 16-bit words */
    } else {
        dma_channel = SB16_DMA_8BIT;
        dsp_cmd = DSP_8BIT_OUT_AI;
        mode = 0x00;  /* unsigned */
        block_size = SB16_BUFFER_SIZE - 1;  /* one half, in bytes */
    }
    if (current_channels == 2) {
        mode |= 0x20;  /* stereo */
    }

    dma_start(dma_channel, addr, SB16_BUFFER_SIZE * 2,
              DMA_MODE_SINGLE | DMA_MODE_READ | DMA_MODE_AUTOINIT);

    /* DSP 4.xx auto-init: command, mode, length_low, length_high. */
    dsp_write(dsp_cmd);
    dsp_write(mode);
    dsp_write(block_size & 0xFF);
    dsp_write((block_size >> 8) & 0xFF);
}

/**
 * Service the SB16 half-buffer interrupt: refill the half that just drained.
 *
 * In auto-init mode the DMA controller cycles the double-buffer on its own and
 * raises this IRQ as each half completes, so the handler only acknowledges the
 * interrupt and refills the finished half from the ring buffer; no DMA restart
 * is needed. ring_read_to_buffer() covers any underrun with silence.
 */
static void sb16_irq_handler(isr_params_t *params)
{
    uint8_t *fill_buffer;
    (void)params;

    /*
     * Acknowledge the DSP interrupt first. The 8-bit and 16-bit transfers
     * latch their interrupt on different ports.
     */
    if (is_16bit) {
        inb(sb16_base + SB16_INT_ACK_16BIT);
    } else {
        inb(sb16_base + SB16_INT_ACK_8BIT);
    }

    if (!playing) {
        return;
    }

    irq_count++;

    /* current_buffer names the half that was playing and just finished. */
    fill_buffer = dma_buffer + (current_buffer * SB16_BUFFER_SIZE);

    current_buffer = 1 - current_buffer;

    ring_read_to_buffer(fill_buffer, SB16_BUFFER_SIZE);
}

/**
 * Probe for an SB16 at `base` and record its DSP version.
 *
 * Resets the DSP and reads the version word; an SB16 reports DSP version 4.xx,
 * so anything below 4 is an older card and is rejected.
 *
 * @return true only if a DSP 4.xx answered at this base.
 */
bool sb16_probe(uint16_t base)
{
    sb16_base = base;
    if (!dsp_reset()) {
        return false;
    }

    if (!dsp_write(DSP_GET_VERSION)) {
        return false;
    }

    dsp_version = (uint16_t)dsp_read() << 8;
    dsp_version |= dsp_read();

    if ((dsp_version >> 8) < 4) {
        klog(KLOG_INFO, "sb16", "found version %d.%d, which is not an SB16",
                      dsp_version >> 8, dsp_version & 0xFF);
        return false;
    }

    return true;
}

uint16_t sb16_get_version(void)
{
    return dsp_version;
}

/**
 * Locate the SB16, install its IRQ handler, and program the mixer.
 *
 * Probes the four standard ISA base ports in turn. Once found, registers the
 * interrupt, resets the mixer, opens the master and voice volumes, and selects
 * IRQ 5 and DMA channels 1 (8-bit) and 5 (16-bit).
 *
 * @return 0 on success, -1 if no card responded.
 */
static int sb16_init(void)
{
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

    register_isr_handler(SB16_IRQ_VECTOR, sb16_irq_handler, "sb16");

    mixer_write(MIXER_RESET, 0);

    mixer_write(MIXER_MASTER_VOL, 0xFF);
    mixer_write(MIXER_VOICE_VOL, 0xFF);

    /* IRQ select: bit 1 = IRQ 5, bit 2 = IRQ 7, bit 3 = IRQ 10. */
    mixer_write(MIXER_IRQ_SELECT, 0x02);

    /* DMA select: bit 1 = 8-bit channel 1, bit 5 = 16-bit channel 5. */
    mixer_write(MIXER_DMA_SELECT, 0x22);

    return 0;
}

/**
 * Stop playback, silence the speaker, and reset the DSP.
 */
static void sb16_shutdown(void)
{
    if (playing) {
        sb16_driver.stop();
    }

    dsp_write(DSP_SPEAKER_OFF);

    dsp_reset();
}

/**
 * Begin auto-init playback at the given rate, format, and channel count.
 *
 * Stops any current playback, primes both DMA halves with silence (and the
 * callback's data if one is set), resets the ring buffer, then arms the DSP.
 * The DSP 4.xx sample-rate command must precede the transfer command, so the
 * rate is programmed before sb16_start_auto_init() issues 0xC6/0xB6.
 *
 * @return 0 on success, -1 if the rate is outside 5000..44100 Hz.
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

    if (sample_rate < 5000 || sample_rate > 44100) {
        klog(KLOG_WARN, "sb16", "refused: sample rate %d", sample_rate);
        return -1;
    }

    dsp_write(DSP_SPEAKER_ON);

    /* DSP 4.xx sample-rate command must precede the transfer command. */
    dsp_write(DSP_SET_SAMPLE_RATE);
    dsp_write((sample_rate >> 8) & 0xFF);
    dsp_write(sample_rate & 0xFF);

    /* Silence both halves: 0 for 16-bit signed, 128 for 8-bit unsigned. */
    k_memset(dma_buffer, is_16bit ? 0 : 128, SB16_BUFFER_SIZE * 2);

    ring_read_ptr = 0;
    ring_write_ptr = 0;

    if (user_callback) {
        user_callback(dma_buffer, SB16_BUFFER_SIZE);
        user_callback(dma_buffer + SB16_BUFFER_SIZE, SB16_BUFFER_SIZE);
    }

    irq_count = 0;
    current_buffer = 0;
    playing = true;

    sb16_start_auto_init();

    return 0;
}

/**
 * Halt auto-init playback and silence the card.
 *
 * The DSP needs both an exit-auto-init command (0xDA/0xD9) so it stops after
 * the current block and an immediate-pause command (0xD0/0xD5); the DMA
 * channel is then masked and the speaker turned off.
 */
static void sb16_stop(void)
{
    if (!playing) {
        return;
    }

    playing = false;

    if (is_16bit) {
        dsp_write(DSP_EXIT_16BIT);
        dsp_write(DSP_STOP_16BIT);
    } else {
        dsp_write(DSP_EXIT_8BIT);
        dsp_write(DSP_STOP_8BIT);
    }

    dma_stop(is_16bit ? SB16_DMA_16BIT : SB16_DMA_8BIT);

    dsp_write(DSP_SPEAKER_OFF);

    underrun_count = 0;
    underrun_samples = 0;
}

static bool sb16_is_playing(void)
{
    return playing;
}

static void sb16_set_callback(audio_callback_t fn)
{
    user_callback = fn;
}

/**
 * Queue audio samples for playback.
 *
 * Samples are appended to the ring buffer and later drained by the IRQ handler
 * as DMA halves need refilling, decoupling Doom's frame-paced writes from the
 * fixed DMA rate. Overflow beyond the ring's free space is dropped.
 *
 * @return the number of samples queued, or -1 if not playing or given no data.
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

    written = ring_write(buffer, (uint32_t)size);

    return (int)written;
}

/**
 * Set output volume from a 0..100 percentage.
 *
 * The percentage is clamped and scaled to the mixer's 0..255 range. The master
 * register packs left in the high nibble and right in the low nibble, so the
 * scaled value is folded into both nibbles for an equal stereo level.
 */
static void sb16_set_volume(int volume)
{
    uint8_t val;

    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    val = (uint8_t)((volume * 255) / 100);

    mixer_write(MIXER_MASTER_VOL, (val & 0xF0) | ((val >> 4) & 0x0F));
    mixer_write(MIXER_VOICE_VOL, val);
}

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
