/*
 * Sound Blaster 16 driver. PCM playback through the SB16 DSP and ISA DMA,
 * double-buffered for continuous output.
 */

#include "../arch/arch.h"
#include "../arch/x86/portio.h"
#include "../klog.h"
#include "sb16.h"
#include "dma.h"
#include "../kio.h"
#include "../serial.h"
#include "../irq.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"

#define SB16_BASE      0x220
#define SB16_IRQ       5
#define SB16_DMA_8BIT  1
#define SB16_DMA_16BIT 5

/* PIC remaps the ISA IRQs to vectors 32..47, so IRQ 5 is vector 37. */
#define SB16_IRQ_VECTOR (32 + SB16_IRQ)

static uint16_t sb16_base = SB16_BASE;

static uint16_t dsp_version = 0;

static uint32_t current_sample_rate = 0;
static audio_format_t current_format = AUDIO_U8;
static int current_channels = 1;
static bool is_16bit = false;

static bool playing = false;
static audio_callback_t user_callback = NULL;

static audio_stream_t *play_stream = NULL; /* the playback direction's owner */
static bool stream_configured = false;
static uint32_t stream_frames = 0; /* frames played since stream_start, wrapping */
static uint32_t stream_last = 0;   /* last DMA position, in frames */
static uint32_t stream_given = 0;  /* position after the last frame in DMA */

static void sb16_halt(void);

/*
 * DMA double-buffer: one contiguous region split into two halves of
 * period_bytes each. ISA DMA requires the buffer to lie below 16MB and not
 * cross the page its channel counts in: 64KB for the 8-bit channel, 128KB for
 * the 16-bit one. The region is one aligned page, so it crosses neither. The
 * kernel image, whose .bss this is, is loaded from 1 MiB, and the controller
 * is given the physical address, `virt_to_phys` of it.
 */
static uint8_t dma_buffer[SB16_PERIOD_MAX * 2] __attribute__((aligned(4096)));
static uint32_t period_bytes = SB16_PERIOD_FRAMES; /* set by sb16_start */
static volatile uint32_t irq_count = 0;

/*
 * Ring buffer that decouples the producer (a writer of the console's
 * AUDIO_WRITE) from the consumer (DMA via IRQ), absorbing timing jitter
 * between the writer's pace and the fixed DMA consumption rate. It holds
 * ring_size bytes, eight DMA periods (93 ms at 11025 Hz, about three tics of
 * Doom's), of an array large enough for eight of the largest.
 */
#define RING_BUFFER_SIZE (SB16_PERIOD_MAX * 8)

static uint8_t ring_buffer[RING_BUFFER_SIZE];
static uint32_t ring_size = RING_BUFFER_SIZE; /* set by sb16_start */
static volatile uint32_t ring_read_ptr = 0;   /* IRQ reads here */
static volatile uint32_t ring_write_ptr = 0;  /* write() appends here */

static volatile uint32_t underrun_count = 0;
static volatile uint32_t underrun_samples = 0;

/**
 * Return the number of bytes queued in the ring buffer.
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
    return ring_size - r + w;
}

/**
 * Return the number of bytes that can be appended to the ring buffer.
 *
 * One slot is always left empty so a full buffer is distinguishable from an
 * empty one.
 */
static uint32_t ring_free(void)
{
    return ring_size - 1 - ring_available();
}

/**
 * Append up to `count` bytes to the ring buffer, dropping any overflow.
 *
 * Called from syscall context (the sole writer). The write pointer is
 * published only after every byte lands, so the IRQ-side reader never reads a
 * byte this call has not written. The ring counts bytes, so a short write
 * can leave an incomplete frame. The reader leaves that frame queued until
 * a later write completes it.
 *
 * @return the number of bytes actually stored.
 */
static uint32_t ring_write(const uint8_t *data, uint32_t count)
{
    uint32_t free_space = ring_free();
    uint32_t to_write = (count < free_space) ? count : free_space;
    uint32_t i;
    uint32_t wp = ring_write_ptr;

    for (i = 0; i < to_write; i++) {
        ring_buffer[wp] = data[i];
        wp = (wp + 1) % ring_size;
    }

    ring_write_ptr = wp;
    return to_write;
}

/**
 * Drain `count` bytes from the ring buffer into `dest`, padding with silence.
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
    uint32_t frame_bytes = (is_16bit ? 2u : 1u) * (current_channels == 2 ? 2u : 1u);
    uint32_t silence_needed;
    uint32_t i;
    uint32_t rp = ring_read_ptr;

    to_read -= to_read % frame_bytes;
    silence_needed = count - to_read;

    for (i = 0; i < to_read; i++) {
        dest[i] = ring_buffer[rp];
        rp = (rp + 1) % ring_size;
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
 * register has the count minus one. Stereo interleaves L/R in consecutive
 * transfer units; halving the count for stereo would interrupt twice per
 * DMA half and refill samples that the DAC is still reading.
 */
static bool sb16_start_auto_init(void)
{
    uint32_t addr = (uint32_t)virt_to_phys(dma_buffer);
    uint16_t block_size;
    uint8_t mode;
    int dma_channel;
    uint8_t dsp_cmd;

    if (is_16bit) {
        dma_channel = SB16_DMA_16BIT;
        dsp_cmd = DSP_16BIT_OUT_AI;
        mode = 0x10;                                   /* signed */
        block_size = (uint16_t)(period_bytes / 2 - 1); /* one half, in 16-bit words */
    } else {
        dma_channel = SB16_DMA_8BIT;
        dsp_cmd = DSP_8BIT_OUT_AI;
        mode = 0x00;                               /* unsigned */
        block_size = (uint16_t)(period_bytes - 1); /* one half, in bytes */
    }
    if (current_channels == 2) {
        mode |= 0x20; /* stereo */
    }

    if (dma_start(dma_channel, addr, period_bytes * 2,
                  DMA_MODE_SINGLE | DMA_MODE_READ | DMA_MODE_AUTOINIT) < 0) {
        return false;
    }

    /* DSP 4.xx auto-init: command, mode, length_low, length_high. */
    return dsp_write(dsp_cmd) && dsp_write(mode) && dsp_write(block_size & 0xFF) &&
           dsp_write((block_size >> 8) & 0xFF);
}

/**
 * The half of the double buffer the DMA controller is reading, 0 or 1, from
 * the transfers it has left before the end of the buffer. A count outside the
 * buffer, which no running transfer gives, is taken as its start.
 */
static int sb16_playing_half(void)
{
    uint32_t unit = is_16bit ? 2u : 1u;
    uint32_t total = period_bytes * 2u / unit;
    int left = dma_remaining(is_16bit ? SB16_DMA_16BIT : SB16_DMA_8BIT);

    if (left < 1 || (uint32_t)left > total) {
        return 0;
    }
    return (total - (uint32_t)left) * unit >= period_bytes ? 1 : 0;
}

/**
 * Read and accumulate the stream's DMA position in frames; interrupts off.
 * The count cannot reveal a whole revolution between readings.
 */
static uint32_t sb16_stream_progress(void)
{
    uint32_t frame = (is_16bit ? 2u : 1u) * (current_channels == 2 ? 2u : 1u);
    uint32_t unit = is_16bit ? 2u : 1u;
    uint32_t total = period_bytes * 2u / frame;
    uint32_t pos;
    int left = dma_remaining(is_16bit ? SB16_DMA_16BIT : SB16_DMA_8BIT);

    if (left < 1 || (uint32_t)left > period_bytes * 2u / unit)
        return stream_frames;
    pos = (period_bytes * 2u - (uint32_t)left * unit) / frame;
    stream_frames += pos >= stream_last ? pos - stream_last : total - stream_last + pos;
    stream_last = pos;
    return stream_frames;
}

/**
 * Fill one DMA half from its owner; callbacks run with interrupts off.
 * A stream's caller fills every byte, using silence when it has no data.
 */
static void sb16_fill(uint8_t *buffer)
{
    uint32_t flags = kirq_save();

    if (play_stream) {
        play_stream->transfer(play_stream, buffer, period_bytes);
        stream_given += period_bytes / ((is_16bit ? 2u : 1u) * (current_channels == 2 ? 2u : 1u));
    } else if (user_callback) {
        user_callback(buffer, period_bytes);
    } else {
        ring_read_to_buffer(buffer, period_bytes);
    }
    kirq_restore(flags);
}

/**
 * Service the SB16 half-buffer interrupt: refill the half the DMA is not
 * reading.
 *
 * In auto-init mode the DMA controller cycles the double-buffer on its own and
 * the DSP raises this IRQ as each block completes, so the handler only
 * acknowledges the interrupt and refills a half from the callback or the ring
 * buffer; no DMA restart is needed. ring_read_to_buffer() covers any underrun
 * with silence. The 8-bit and 16-bit transfers acknowledge their interrupts
 * through different ports.
 *
 * The half is found from the controller's position, not by counting
 * interrupts: two blocks that complete before the first interrupt is serviced
 * raise it once, and a count would from then on refill the half being read.
 * An interrupt serviced that late finds the DMA reading a half it has played
 * already, so that half is heard twice, and the refills stay in step. An
 * interrupt that finds the DMA still in the half of the last refill refills
 * the other half again, and the frames of the last refill are not played.
 */
static void sb16_irq_handler(isr_params_t *params)
{
    uint8_t *fill_buffer;
    (void)params;

    if (is_16bit) {
        inb(sb16_base + SB16_INT_ACK_16BIT);
    } else {
        inb(sb16_base + SB16_INT_ACK_8BIT);
    }

    if (!playing) {
        return;
    }

    irq_count++;

    if (play_stream) {
        uint32_t frames = period_bytes / ((is_16bit ? 2u : 1u) * (current_channels == 2 ? 2u : 1u));

        sb16_stream_progress();
        fill_buffer = dma_buffer + (1u - stream_last / frames) * period_bytes;
    } else {
        fill_buffer = dma_buffer + (uint32_t)(1 - sb16_playing_half()) * period_bytes;
    }
    sb16_fill(fill_buffer);
    if (play_stream) {
        uint32_t frames = period_bytes / ((is_16bit ? 2u : 1u) * (current_channels == 2 ? 2u : 1u));

        stream_given = stream_frames + 2u * frames - stream_last % frames;
        if (play_stream->period)
            play_stream->period(play_stream);
    }
}

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
        klog(KLOG_INFO, "sb16", "found version %d.%d, which is not an SB16", dsp_version >> 8,
             dsp_version & 0xFF);
        return false;
    }

    return true;
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
    uint16_t bases[] = {0x220, 0x240, 0x260, 0x280};
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
        sb16_halt();
    }

    dsp_write(DSP_SPEAKER_OFF);

    dsp_reset();
}

/**
 * Begin auto-init playback at the given rate, format, and channel count.
 *
 * Stops any current playback, sizes the period and the ring for the format and
 * empties the ring, primes both DMA halves with silence (and the callback's
 * data if one is set), then arms the DSP.
 * The DSP 4.xx sample-rate command must precede the transfer command, so the
 * rate is programmed before sb16_start_auto_init() issues 0xC6/0xB6.
 * `requested_period` is a stream's granted bytes, or 0 for console playback.
 *
 * @return 0 on success, -EINVAL for a rate outside 5000 through 44100, or
 * -EIO when the DSP or DMA setup fails.
 */
static int sb16_begin(uint32_t sample_rate, audio_format_t format, int channels,
                      uint32_t requested_period)
{
    if (playing) {
        sb16_halt();
    }

    current_sample_rate = sample_rate;
    current_format = format;
    current_channels = channels;
    is_16bit = (format == AUDIO_S16LE);

    if (sample_rate < 5000 || sample_rate > 44100) {
        klog(KLOG_WARN, "sb16", "refused: sample rate %d", sample_rate);
        return -EINVAL;
    }

    period_bytes = SB16_PERIOD_FRAMES * ((sample_rate + 11024) / 11025) * (is_16bit ? 2u : 1u) *
                   (channels == 2 ? 2u : 1u);
    if (requested_period)
        period_bytes = requested_period;
    ring_size = period_bytes * 8;
    ring_read_ptr = 0;
    ring_write_ptr = 0;

    /* DSP 4.xx sample-rate command must precede the transfer command. */
    if (!dsp_write(DSP_SPEAKER_ON) || !dsp_write(DSP_SET_SAMPLE_RATE) ||
        !dsp_write((sample_rate >> 8) & 0xFF) || !dsp_write(sample_rate & 0xFF)) {
        goto fail;
    }

    /* Silence both halves: 0 for 16-bit signed, 128 for 8-bit unsigned. */
    k_memset(dma_buffer, is_16bit ? 0 : 128, period_bytes * 2);

    stream_frames = stream_last = stream_given = 0;
    if (play_stream || user_callback) {
        sb16_fill(dma_buffer);
        sb16_fill(dma_buffer + period_bytes);
    }

    irq_count = 0;
    playing = true;

    if (!sb16_start_auto_init()) {
        goto fail;
    }

    return 0;

fail:
    playing = false;
    dma_stop(is_16bit ? SB16_DMA_16BIT : SB16_DMA_8BIT);
    dsp_reset();
    return -EIO;
}

/**
 * Halt auto-init playback and silence the card.
 *
 * The DSP needs both an exit-auto-init command (0xDA/0xD9) so it stops after
 * the current block and an immediate-pause command (0xD0/0xD5); the DMA
 * channel is then masked and the speaker turned off.
 */
static void sb16_halt(void)
{
    uint32_t flags = kirq_save();

    if (!playing) {
        kirq_restore(flags);
        return;
    }

    if (play_stream)
        sb16_stream_progress();
    playing = false;
    kirq_restore(flags);

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

/** Start console playback, or -EBUSY while a stream owns the device. */
static int sb16_start(uint32_t rate, audio_format_t format, int channels)
{
    return play_stream ? -EBUSY : sb16_begin(rate, format, channels, 0);
}

/** Stop console playback; a stream keeps its direction until close. */
static void sb16_stop(void)
{
    if (!play_stream)
        sb16_halt();
}

uint32_t sb16_delay_frames(void)
{
    uint32_t frame = (is_16bit ? 2u : 1u) * (current_channels == 2 ? 2u : 1u);
    uint32_t unit = is_16bit ? 2u : 1u;
    uint32_t total = period_bytes * 2u / unit;
    uint32_t flags, ahead = 0;
    int left;

    flags = kirq_save();
    if (playing) {
        left = dma_remaining(is_16bit ? SB16_DMA_16BIT : SB16_DMA_8BIT);
        if (left >= 1 && (uint32_t)left <= total) {
            ahead = (uint32_t)left * unit;
            if (ahead <= period_bytes)
                ahead += period_bytes;
        }
        ahead += user_callback ? 0 : ring_available();
    }
    kirq_restore(flags);
    return ahead / frame;
}

/** Whether playback is running. */
static bool sb16_is_playing(void)
{
    return playing;
}

/** Set the pull callback the interrupt handler and sb16_start call; NULL
 * plays from the ring buffer. */
static void sb16_set_callback(audio_callback_t fn)
{
    if (!play_stream)
        user_callback = fn;
}

/**
 * Queue audio samples for playback.
 *
 * Samples are appended to the ring buffer and later drained by the IRQ handler
 * as DMA halves need refilling, decoupling the writer's pace from the fixed
 * DMA rate. Overflow beyond the ring's free space is dropped.
 *
 * @return the number of bytes queued, or -EINVAL if not playing or given no
 * data.
 */
static int sb16_write(const uint8_t *buffer, size_t size)
{
    uint32_t written;

    if (play_stream)
        return -EBUSY;
    if (!buffer || size == 0 || !playing) {
        return -EINVAL;
    }

    written = ring_write(buffer, (uint32_t)size);

    return (int)written;
}

/* The level last set, 0 to 100; init opens the mixer fully. */
static int sb16_volume = 100;

/**
 * Set output volume from a 0..100 percentage.
 *
 * The percentage is clamped and scaled to the mixer's 0..255 range. The master
 * and voice registers each pack left in the high nibble and right in the low
 * one, so the scaled value is folded into both nibbles, for both registers,
 * and the two channels come out level.
 */
static void sb16_set_volume(int volume)
{
    uint8_t val;

    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;

    sb16_volume = volume;
    val = (uint8_t)((volume * 255) / 100);

    val = (uint8_t)((val & 0xF0) | ((val >> 4) & 0x0F));
    mixer_write(MIXER_MASTER_VOL, val);
    mixer_write(MIXER_VOICE_VOL, val);
}

/** The level `sb16_set_volume` last set, which the mixer keeps across a
 * stream's start; 100 until one is. */
static int sb16_get_volume(void)
{
    return sb16_volume;
}

/**
 * Reserve playback for `s`, which remains the caller's until close.
 * Return -ENODEV for capture, -EBUSY while playback is owned or running.
 */
static int sb16_stream_open(audio_stream_t *s)
{
    if (s->dir != AUDIO_PLAYBACK)
        return -ENODEV;
    if (play_stream || playing)
        return -EBUSY;
    play_stream = s;
    stream_configured = false;
    stream_frames = stream_last = stream_given = 0;
    return 0;
}

/**
 * Grant a stopped stream U8 or S16, mono or stereo, at 5000..44100 Hz; a
 * wider format is granted S16, its period keeping its frames. Clamp the
 * period to the static DMA half and round down to whole frames. Return
 * -EINVAL without changing the setup for invalid parameters.
 */
static int sb16_stream_setup(audio_stream_t *s)
{
    audio_format_t format = s->format;
    uint32_t frame, bytes = s->period_bytes;

    if (s != play_stream || playing || !s->transfer ||
        (format != AUDIO_U8 && format != AUDIO_S16LE && format != AUDIO_S24LE &&
         format != AUDIO_S24_3LE && format != AUDIO_S32LE) ||
        (s->channels != 1 && s->channels != 2))
        return -EINVAL;
    if (AUDIO_SAMPLE_BYTES(format) > 2u) {
        bytes = bytes / AUDIO_SAMPLE_BYTES(format) * 2u;
        format = AUDIO_S16LE;
    }
    frame = (format == AUDIO_S16LE ? 2u : 1u) * (uint32_t)s->channels;
    bytes = bytes < SB16_PERIOD_MAX ? bytes : SB16_PERIOD_MAX;
    bytes -= bytes % frame;
    if (bytes == 0)
        return -EINVAL;
    s->format = format;
    s->rate = s->rate < 5000 ? 5000 : s->rate > 44100 ? 44100 : s->rate;
    s->period_bytes = bytes;
    if (s->periods < 2)
        s->periods = 2;
    s->device_periods = 2;
    stream_configured = true;
    return 0;
}

/** Start the configured stream, priming both halves, or return -EIO. */
static int sb16_stream_start(audio_stream_t *s)
{
    if (s != play_stream || !stream_configured)
        return -EIO;
    if (playing)
        return 0;
    return sb16_begin(s->rate, s->format, s->channels, s->period_bytes);
}

/** Stop `s` and keep its final position; no callback follows this return. */
static void sb16_stream_stop(audio_stream_t *s)
{
    if (s == play_stream)
        sb16_halt();
}

/** Stop `s` and give playback back; a different caller changes nothing. */
static void sb16_stream_close(audio_stream_t *s)
{
    if (s != play_stream)
        return;
    sb16_stream_stop(s);
    play_stream = NULL;
    stream_configured = false;
}

/** Read the owner's frames played, moving within a period; safe in a wait. */
static uint32_t sb16_stream_position(audio_stream_t *s)
{
    uint32_t flags = kirq_save();
    uint32_t frames = 0;

    if (s && s == play_stream)
        frames = playing ? sb16_stream_progress() : stream_frames;
    kirq_restore(flags);
    return frames;
}

/**
 * Read the frames transferred but not played, or the console queue for NULL.
 * Stopped streams and streams belonging to another caller have no delay.
 */
static uint32_t sb16_stream_delay(audio_stream_t *s)
{
    uint32_t flags, frames = 0, pos;

    if (!s)
        return sb16_delay_frames();
    flags = kirq_save();
    if (s == play_stream && playing) {
        pos = sb16_stream_progress();
        if ((int32_t)(stream_given - pos) > 0)
            frames = stream_given - pos;
    }
    kirq_restore(flags);
    return frames;
}

/* The audio_driver_t fields, in the order audio.h declares them: name,
   type, init, shutdown, start, stop, is_playing, set_callback, write,
   set_volume, get_volume, the seven stream operations, then the three
   input controls and the two of the beep generator, which this driver
   lacks. */
audio_driver_t sb16_driver = {"Sound Blaster 16",
                              AUDIO_DRIVER_SB16,
                              sb16_init,
                              sb16_shutdown,
                              sb16_start,
                              sb16_stop,
                              sb16_is_playing,
                              sb16_set_callback,
                              sb16_write,
                              sb16_set_volume,
                              sb16_get_volume,
                              sb16_stream_open,
                              sb16_stream_setup,
                              sb16_stream_start,
                              sb16_stream_stop,
                              sb16_stream_close,
                              sb16_stream_position,
                              sb16_stream_delay,
                              NULL,
                              NULL,
                              NULL,
                              NULL,
                              NULL};
