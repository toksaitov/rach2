/*
 * Intel HD Audio (HDA) playback driver.
 *
 * Drives audio output on an Intel HDA controller: codec verbs go over the
 * Immediate Command Interface (CORB/RIRB are set up but enumeration uses ICI),
 * and PCM frames stream out of a cyclic BDL via the controller's DMA engine.
 * The frames come from a callback, the write path's ring, or a playback
 * stream of audio.h, such as /dev/dsp's.
 */

#include "../arch/arch.h"
#include "../klog.h"
#include "hda.h"
#include "resample.h"
#include "../kio.h"
#include "../serial.h"
#include "../timer.h"
#include "../irq.h"
#include "../pci.h"
#include "../udelay.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/spinlock.h"
#include "../lib/kstd.h"
#include "../lib/kstring.h"
#include "../mm/paging.h"

/* HDA MMIO region size (typically 16KB) */
#define HDA_MMIO_SIZE 0x4000

/* PCI configuration offsets and capability IDs, for power management and the
 * dump */
#define PCI_SUBSYSTEM_VENDOR_ID 0x2C
#define PCI_SUBSYSTEM_ID        0x2E

#define PCI_CAP_ID_PM  0x01
#define PCI_CAP_ID_MSI 0x05

/* PCI device info */
static pci_device_t hda_pci;

/* The vector the controller's messages arrive on, given by `isr_alloc_vector`
 * when they are enabled; -1 before. */
static int msi_vector = -1;
static uintptr_t hda_base;

/* Controller capabilities */
static uint16_t num_input_streams = 0;
static uint16_t num_output_streams = 0;

/* The most streams the controller's register layout addresses, a hardware
 * limit. */
#define HDA_MAX_STREAMS 32
/* DMA position buffer, two words a stream with the position first (optional;
 * more reliable than LPIB on some controllers). */
static volatile uint32_t dma_posbuf[HDA_MAX_STREAMS * 2] __attribute__((aligned(128)));
static volatile bool dma_posbuf_enabled = false;
static volatile bool dma_posbuf_use = false;
/* The check of the first stream started after the controller was set up:
 * LPIB, the stream status and the position buffer's entry, read 20 ms apart
 * ([0] before, [1] after). `audio hda stats` prints it. */
static bool dma_checked = false;
static uint32_t dma_check_lpib[2], dma_check_pos[2];
static uint8_t dma_check_sts[2];

/* Codec info */
static uint8_t codec_addr = 0;
static uint32_t codec_vendor_id = 0;
static uint8_t audio_fg_nid = 0;    /* Audio function group node ID */
static uint8_t dac_nid = 0;         /* DAC widget node ID */
static uint8_t pin_nid = 0;         /* Output pin widget node ID */
static uint8_t mixer_nid = 0;       /* Mixer widget in output path (if any) */
static uint8_t hp_pin_nid = 0;      /* Headphone pin (for fallback) */
static uint8_t hp_dac_nid = 0;      /* Headphone DAC */
static uint8_t speaker_pin_nid = 0; /* Speaker pin (for EAPD/BTL heuristics) */

/* Output routing override (kernel debug). */
#define HDA_OUTMODE_AUTO    0
#define HDA_OUTMODE_PRIMARY 1
#define HDA_OUTMODE_HP      2
#define HDA_OUTMODE_BOTH    3
static volatile int hda_output_mode = HDA_OUTMODE_AUTO;

/* Cached AFG widget range (for scanning pins later) */
static uint8_t afg_widget_start = 0;
static uint8_t afg_widget_count = 0;

/* The connection indices chosen on the output pin, the headphone pin and the
 * mixer */
static uint8_t pin_conn_sel_idx = 0;
static uint8_t hp_conn_sel_idx = 0;
static uint8_t mixer_conn_sel_idx = 0;
static hda_report_t hda_report;

/* Stream info */
static uint16_t output_stream_index = 0; /* Set during init based on GCAP */
static uint16_t input_stream_index = 0;  /* the descriptor capture uses */
static uint32_t stream_tag = 1;

/* CORB/RIRB buffers (must be 128-byte aligned, volatile for DMA). These and
 * the other DMA buffers here are in the image's .bss, and the controller is
 * given `virt_to_phys` of each, its lower and upper halves in the
 * registers and the descriptors that have both. */
static volatile uint32_t corb_buffer[HDA_CORB_SIZE] __attribute__((aligned(128)));
static volatile uint64_t rirb_buffer[HDA_RIRB_SIZE] __attribute__((aligned(128)));
static volatile uint16_t corb_wp = 0;
static volatile uint16_t rirb_rp = 0;

/*
 * The DMA buffer: HDA_PERIODS periods of period_bytes each, one descriptor
 * list entry apiece, contiguous in audio_buffer. A period is the frame count
 * nearest HDA_PERIOD_MS at the stream's rate, rounded to the 128 bytes a
 * descriptor's address must be aligned to, and at most HDA_PERIOD_MAX:
 * 4096 bytes at 44.1 kHz 16-bit stereo, 23.2 ms. Each period's end raises the
 * stream's interrupt, which refills the periods played; the timer tick does
 * the same where the interrupt does not arrive (MSI or IOAPIC routing), and
 * two periods ahead outlast any tick. The descriptor list is volatile, since
 * the controller reads it.
 */
#define HDA_PERIOD_MS  23
#define HDA_PERIODS    3
#define HDA_PERIOD_MAX 16384
static volatile hda_bdl_entry_t bdl[HDA_PERIODS] __attribute__((aligned(1024)));
static int16_t audio_buffer[HDA_PERIOD_MAX * HDA_PERIODS / 2] __attribute__((aligned(128)));
static uint32_t period_bytes = HDA_PERIOD_MAX; /* set by hda_start */

/*
 * The write path's ring, which `hda_start` sizes to HDA_RING_PERIODS periods
 * of the caller's frames, so that a write waits at most that long in it and
 * HDA_PERIODS more in the DMA buffer: 139 ms at 44.1 kHz. Three periods take
 * a writer's burst of up to one period, such as a tic of Doom's, 28.6 ms,
 * beside the period the service takes at once. ring_size is one more than the
 * bytes it holds, since one slot stays empty, and at most HDA_RING_MAX, which
 * caps the ring under three periods only above 96 kHz stereo.
 */
#define HDA_RING_PERIODS 3
#define HDA_RING_MAX     32768
static uint8_t hda_ring[HDA_RING_MAX];
static uint32_t ring_size = HDA_RING_MAX; /* set by hda_start */
static volatile uint32_t ring_read_ptr = 0;
static volatile uint32_t ring_write_ptr = 0;

/* Diagnostics */
static volatile uint32_t irq_count = 0;
static volatile uint32_t underrun_count = 0;
static volatile uint32_t underrun_samples = 0;
static volatile uint32_t session_bytes_written = 0;
static volatile uint32_t session_bytes_dropped = 0;
static volatile uint32_t ring_high_water = 0;
static volatile uint32_t walclk_start = 0;
static volatile uint32_t walclk_end = 0;
static volatile int16_t out_min_s16 = 0;
static volatile int16_t out_max_s16 = 0;
static char hda_boot_status[48] = "Intel HD Audio";

static void hda_build_boot_status(void);

/** The bytes queued in the ring; the IRQ side and the write side may both
 * call it, since it reads each volatile pointer once. */
static uint32_t ring_available(void)
{
    uint32_t w = ring_write_ptr;
    uint32_t r = ring_read_ptr;
    if (w >= r) {
        return w - r;
    }
    return ring_size - r + w;
}

/** The bytes the ring can take; one slot stays empty, so a full ring and an
 * empty one differ. */
static uint32_t ring_free(void)
{
    return ring_size - 1 - ring_available();
}

/**
 * Copy up to `count` bytes into the audio ring buffer.
 *
 * Writes only what fits in the free space; any tail that does not fit is
 * counted as dropped, not blocked. Returns the number of bytes accepted.
 */
static uint32_t ring_write(const uint8_t *data, uint32_t count)
{
    uint32_t free_space = ring_free();
    uint32_t to_write = (count < free_space) ? count : free_space;
    uint32_t i;
    uint32_t wp = ring_write_ptr;

    for (i = 0; i < to_write; i++) {
        hda_ring[wp] = data[i];
        wp = (wp + 1) % ring_size;
    }

    ring_write_ptr = wp;
    if (count > to_write) {
        session_bytes_dropped += (count - to_write);
    }
    {
        uint32_t avail = ring_available();
        if (avail > ring_high_water) {
            ring_high_water = avail;
        }
    }
    return to_write;
}

/* Playback state */
static bool playing = false;
static bool stream_started = false; /* True once hardware stream is running */
static audio_callback_t user_callback = NULL;

/* Requested format/rate from AUDIO_START (userspace). */
static uint32_t current_sample_rate = 0;

/* The master volume, 0 to 100, that `hda_set_volume` last set; a stream's
 * start applies it again. */
static int hda_volume = 100;
static audio_format_t current_format = AUDIO_S16LE;
static int current_channels = 2;

/* Actual hardware stream configuration (may differ from requested). */
static uint32_t current_hw_sample_rate = 0;
static audio_format_t current_hw_format = AUDIO_S16LE;
static int current_hw_channels = 2;

/* The rate conversion of the write path or the stream, whichever plays:
 * hda_rs converts the caller's frames to the rate programmed, and
 * resample_step_fp is the ratio of the two rates, 16.16, by which the
 * driver counts frames of one rate in the other. */
static bool resample_enabled = false;
static uint32_t resample_step_fp = 0; /* input_rate / hw_rate */
static resampler_t hda_rs;

/* Persistent diagnostics. */
static volatile uint32_t total_bytes_written = 0;

/*
 * The playback stream (audio.h) that has the output stream, from
 * `stream_open` to `stream_close`, or NULL. While it is set, `start`,
 * `stop` and `write` are refused and the callback is not called.
 *
 * A stream plays as stereo at hda_play_rate's rate, through hda_rs: one in
 * 32-bit samples as 24 bits in 32 (hda_play_setup grants it only where
 * every DAC has 24 bits), and one in 8 or 16-bit samples as 16 bits. Its
 * frames are taken by `transfer` into hda_stage, at most HDA_STAGE_FRAMES
 * frames of the DMA's at a time, and the stream's rate is at most
 * HDA_RATE_RATIO times the programmed one, so the frames of one chunk, at
 * most HDA_RATE_RATIO for each frame of the DMA's and one more, fit the
 * stage.
 *
 * stream_taken counts the frames taken since `stream_start`, wrapping, and
 * stream_in[] those each DMA period was filled with, from which
 * hda_play_delay gives the frames not yet played. stream_pos is the last
 * position given out, which never goes back, and stream_mark the position at
 * which the stream's last `period` call was due; one is due each
 * period_bytes of the stream's frames played.
 */
#define HDA_STAGE_FRAMES 256u
#define HDA_RATE_RATIO   4u
static audio_stream_t *play_stream = NULL;
static uint8_t hda_stage[(HDA_STAGE_FRAMES * HDA_RATE_RATIO + 1u) * 8u];
static volatile uint32_t stream_taken = 0;
static volatile uint32_t stream_in[HDA_PERIODS];
static volatile uint32_t stream_pos = 0;
static volatile uint32_t stream_mark = 0;

/*
 * The service's state as it last left the stream, which `hda_start` sets for
 * the start of the buffer: the position read, in bytes into the buffer, the
 * wall clock read with it, and the period then playing. The periods from the
 * one after it round to it hold samples not yet played, since the service
 * refills every period played. lpib_wraps counts the revolutions of the
 * buffer since the start, those the position showed and those the wall clock
 * did.
 */
static volatile uint32_t lpib_last = 0;
static volatile uint32_t walclk_last = 0;
static volatile uint32_t hw_period_last = 0;
static volatile uint32_t lpib_wraps = 0;

/* Diagnostics: the DMA periods refilled since the stream started. */
static volatile uint32_t hw_fill_count = 0;

/* Whole revolutions of the DMA buffer that passed between two services,
 * which the position alone cannot show; the wall clock does. */
static volatile uint32_t missed_revolutions = 0;

/* Forward declarations for MMIO helpers (needed for C89 order). */
static uint32_t hda_read32(uint32_t offset);
static void hda_write32(uint32_t offset, uint32_t value);

/** The upper half of a physical address, which the controller takes apart
 * from its lower half. */
static uint32_t hda_hi(uint64_t phys)
{
    return (uint32_t)(phys >> 32);
}

/**
 * Push a CPU-written range out to memory so the DMA engine sees it.
 *
 * The architecture writes back each cache line spanning [addr, addr+size)
 * and orders the writes (arch_dma_flush), or the whole cache where it has no
 * per-line write-back.
 */
static void hda_dma_sync_range(const volatile void *addr, uint32_t size)
{
    arch_dma_flush(addr, size);
}

/**
 * Program the DMA position buffer and record whether it is usable.
 *
 * The position buffer reports each stream's playback pointer more reliably
 * than LPIB on some controllers. DPLBASE bit 0 enables it; the controller may
 * ignore it, so the result is read back and `dma_posbuf_enabled` is set only
 * if the base and enable bit stuck.
 */
static void hda_setup_dma_posbuf(void)
{
    uint32_t addr;
    uint32_t dpl;
    uint32_t i;

    /* DPLBASE must not be written while any DMA engine is running. */
    if (!hda_base) {
        return;
    }

    for (i = 0; i < HDA_MAX_STREAMS * 2; i++) {
        dma_posbuf[i] = 0;
    }

    addr = (uint32_t)virt_to_phys(dma_posbuf);
    addr &= ~0x7Fu; /* 128-byte aligned base */

    /* Bit0 enables the position buffer (if implemented by the controller). */
    hda_write32(HDA_DPLBASE, addr | 0x1u);
    hda_write32(HDA_DPUBASE, hda_hi(virt_to_phys(dma_posbuf)));

    dpl = hda_read32(HDA_DPLBASE);
    dma_posbuf_enabled = ((dpl & 0x1u) != 0) && ((dpl & ~0x7Fu) == addr);
    dma_posbuf_use = dma_posbuf_enabled;
    dma_checked = false;
}

int hda_get_output_mode(void)
{
    return (int)hda_output_mode;
}

void hda_set_output_mode(int mode)
{
    if (mode < HDA_OUTMODE_AUTO || mode > HDA_OUTMODE_BOTH) {
        return;
    }
    hda_output_mode = mode;
}

/**
 * Compose the boot banner string from the detected codec vendor ID.
 *
 * Renders "Intel HD Audio, codec 0x<vid>" into `hda_boot_status`, truncated
 * to the buffer. Falls back to the plain name when no codec was found.
 */
static void hda_build_boot_status(void)
{
    static const char prefix[] = "Intel HD Audio, codec 0x";
    char hex[16];
    size_t i;
    size_t pos = 0;

    if (codec_vendor_id == 0) {
        k_strncpy(hda_boot_status, "Intel HD Audio", sizeof(hda_boot_status) - 1);
        hda_boot_status[sizeof(hda_boot_status) - 1] = '\0';
        return;
    }

    k_itoa((int)codec_vendor_id, hex, sizeof(hex), 'x', false);

    for (i = 0; prefix[i] != '\0' && pos < sizeof(hda_boot_status) - 1; ++i) {
        hda_boot_status[pos++] = prefix[i];
    }
    for (i = 0; hex[i] != '\0' && pos < sizeof(hda_boot_status) - 1; ++i) {
        hda_boot_status[pos++] = hex[i];
    }
    hda_boot_status[pos] = '\0';
}

const char *hda_get_boot_status(void)
{
    return hda_boot_status;
}

static uint32_t hda_in_frame(void);

/**
 * Put frame `v`, its two samples as the DMA buffer holds them in
 * current_hw_format, in hda_rs: unchanged at equal rates, where the
 * resampler copies, and otherwise as the 24 bits its filter takes, the top
 * 24 of a 32-bit sample or a 16-bit one widened.
 */
static void hda_rs_put(int32_t *v)
{
    uint32_t u;
    int ch;

    if (hda_rs.span != 0) {
        for (ch = 0; ch < 2; ch++) {
            u = (uint32_t)v[ch] >> 8;
            v[ch] = current_hw_format == AUDIO_S32LE
                        ? (int32_t)(u & 0x800000u ? u | 0xFF000000u : u)
                        : v[ch] * 256;
        }
    }
    resample_put(&hda_rs, v);
}

/**
 * Make the next frame of hda_rs in `v` as the DMA buffer holds it: from 24
 * bits, a 32-bit sample over a zero byte, or a 16-bit one rounded to the
 * nearest, a half up, and held to 16 bits. The offset keeps the division's
 * dividend positive, where C89 defines its rounding.
 */
static void hda_rs_get(int32_t *v)
{
    int ch;

    resample_get(&hda_rs, v);
    if (hda_rs.span == 0)
        return;
    for (ch = 0; ch < 2; ch++) {
        if (current_hw_format == AUDIO_S32LE) {
            v[ch] *= 256;
        } else {
            v[ch] = (v[ch] + 8388608 + 128) / 256 - 32768;
            v[ch] = v[ch] > 32767 ? 32767 : v[ch];
        }
    }
}

/** Read the ring's frame at `rp`, in the caller's format of unsigned 8-bit
 * or signed 16-bit, into `v` as signed 16-bit stereo; a mono sample goes to
 * both. */
static void ring_frame(uint32_t rp, int32_t *v)
{
    uint32_t r;

    if (current_format == AUDIO_S16LE) {
        v[0] = (int16_t)((uint16_t)hda_ring[rp] | ((uint16_t)hda_ring[(rp + 1) % ring_size] << 8));
        r = (rp + 2) % ring_size;
        v[1] =
            current_channels >= 2
                ? (int16_t)((uint16_t)hda_ring[r] | ((uint16_t)hda_ring[(r + 1) % ring_size] << 8))
                : v[0];
    } else {
        v[0] = ((int)hda_ring[rp] - 128) * 256;
        v[1] = current_channels >= 2 ? ((int)hda_ring[(rp + 1) % ring_size] - 128) * 256 : v[0];
    }
}

/**
 * Render `out_frames` frames of ring audio into one DMA period at `dest`.
 *
 * Converts from the format, rate and channel count AUDIO_START requested
 * (11025 Hz U8 mono, say) to the stream's, signed 16-bit stereo at the rate
 * hda_select_hw_rate chose, through hda_rs: each output frame puts the
 * input frames the resampler owes and takes its next frame. Stereo input
 * keeps its two channels, and mono input goes to both. A ring short of the
 * frames owed counts as an underrun and plays silence; the resampler is
 * held meanwhile, so silence cannot consume input that arrives later.
 */
static void ring_render_to_hw_buffer(int16_t *dest, uint32_t out_frames)
{
    uint32_t rp = ring_read_ptr;
    uint32_t avail_bytes = ring_available();
    uint32_t in_bytes_per_frame = hda_in_frame();
    uint32_t out_ch = current_hw_channels <= 0 ? 1u : (uint32_t)current_hw_channels;
    uint32_t f;
    bool had_underrun = false;
    int16_t local_min = out_min_s16;
    int16_t local_max = out_max_s16;
    int32_t v[2];

    for (f = 0; f < out_frames; f++) {
        int16_t left = 0, right = 0;
        uint32_t oi = f * out_ch;

        while (hda_rs.owed > 0 && avail_bytes >= in_bytes_per_frame) {
            ring_frame(rp, v);
            hda_rs_put(v);
            rp = (rp + in_bytes_per_frame) % ring_size;
            avail_bytes -= in_bytes_per_frame;
        }
        if (hda_rs.owed > 0) {
            had_underrun = true;
            underrun_samples += out_ch;
        } else {
            hda_rs_get(v);
            left = (int16_t)v[0];
            right = (int16_t)v[1];
        }

        if (out_ch == 2) {
            dest[oi] = left;
            dest[oi + 1] = right;
        } else {
            dest[oi] = (int16_t)(((int32_t)left + (int32_t)right) / 2);
        }

        if (left < local_min || right < local_min) {
            local_min = left < right ? left : right;
        }
        if (left > local_max || right > local_max) {
            local_max = left > right ? left : right;
        }
    }

    ring_read_ptr = rp;
    out_min_s16 = local_min;
    out_max_s16 = local_max;

    if (had_underrun) {
        underrun_count++;
    }
}

/* Forward declarations needed for C89 (no implicit function declarations). */
static void hda_delay(int count);
static void hda_delay_walclk_us(uint32_t us);
static uint32_t hda_sd_read32(int stream, uint32_t reg);
static uint32_t hda_send_verb(uint8_t caddr, uint8_t nid, uint32_t verb);
static uint32_t hda_get_param(uint8_t nid, uint8_t param);
static int hda_get_connections(uint8_t nid, uint16_t *conn_list, int max_conn);
static void hda_pci_force_d0(pci_device_t *dev);
static uint32_t hda_select_hw_rate(uint32_t req_rate);
static uint32_t hda_stream_position(void);
static void hda_halt(void);
static uint8_t hda_amp_steps(uint32_t amp_caps);
static uint8_t hda_get_out_amp_max(uint8_t nid);
static uint8_t hda_get_in_amp_max(uint8_t nid);
static uint8_t hda_clamp_gain(uint8_t gain, uint8_t max);
static void hda_set_volume(int volume);
static void fill_silence(volatile uint8_t *buffer, size_t size, bool is_signed);

/** The bytes of one frame of the stream as the controller plays it. */
static uint32_t hda_out_frame(void)
{
    return AUDIO_SAMPLE_BYTES(current_hw_format) *
           (current_hw_channels > 0 ? (uint32_t)current_hw_channels : 1u);
}

/** The bytes of one frame as the caller writes it. */
static uint32_t hda_in_frame(void)
{
    return AUDIO_SAMPLE_BYTES(current_format) *
           (current_channels > 0 ? (uint32_t)current_channels : 1u);
}

/**
 * The bytes of a DMA period at `rate` frames per second of `frame` bytes:
 * the frame count nearest HDA_PERIOD_MS, rounded to the nearest multiple of
 * the 128 bytes a descriptor's address is aligned to, from 128 to
 * HDA_PERIOD_MAX. A frame is 1, 2, 4 or 8 bytes, which divide 128, so a
 * period holds whole frames.
 */
static uint32_t hda_period_bytes(uint32_t rate, uint32_t frame)
{
    uint32_t bytes = (rate * HDA_PERIOD_MS + 500u) / 1000u * frame;

    bytes = (bytes + 64u) / 128u * 128u;
    if (bytes < 128u)
        return 128u;
    return bytes > HDA_PERIOD_MAX ? HDA_PERIOD_MAX : bytes;
}

/**
 * The time `bytes` of the stream take to play, in microseconds, at the
 * programmed rate. `bytes` is at most the DMA buffer, so no product here
 * passes 32 bits.
 */
static uint32_t hda_bytes_us(uint32_t bytes)
{
    uint32_t rate = current_hw_sample_rate ? current_hw_sample_rate : 48000u;
    uint32_t milli = bytes / hda_out_frame() * 1000u; /* frames times 1000 */

    return milli / rate * 1000u + milli % rate * 1000u / rate;
}

/**
 * Convert `frames` of the stream, at most 65536, to frames at the caller's
 * rate, by the resampler's 16.16 step; rounded up when `up` is true, down
 * otherwise. The product is split at the step's binary point so that it
 * stays in 32 bits.
 */
static uint32_t hda_frames_in(uint32_t frames, bool up)
{
    uint32_t step = resample_step_fp ? resample_step_fp : 1u << 16;

    return frames * (step >> 16) + (frames * (step & 0xFFFFu) + (up ? 0xFFFFu : 0u)) / 65536u;
}

/** The sample at `p` in the stream's format as the DMA buffer holds it:
 * a 32-bit sample unchanged, a 16-bit one as it is, and unsigned 8-bit moved
 * to signed and widened to 16 bits. */
static int32_t hda_play_sample(const uint8_t *p)
{
    if (current_format == AUDIO_S32LE)
        return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                         (uint32_t)p[3] << 24);
    if (current_format == AUDIO_S16LE)
        return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    return ((int)p[0] - 128) * 256;
}

/** Put frame `p` of the stream's format in `out` as the DMA buffer's
 * stereo samples (hda_play_sample); a mono sample goes to both. */
static void hda_play_frame(const uint8_t *p, int32_t *out)
{
    out[0] = hda_play_sample(p);
    out[1] =
        current_channels == 2 ? hda_play_sample(p + AUDIO_SAMPLE_BYTES(current_format)) : out[0];
}

/** Store sample `v` at `p` in the DMA buffer's format, 32 or 16 bits. */
static void hda_play_store(uint8_t *p, int32_t v)
{
    uint32_t u = (uint32_t)v;

    p[0] = (uint8_t)u;
    p[1] = (uint8_t)(u >> 8);
    if (current_hw_format == AUDIO_S32LE) {
        p[2] = (uint8_t)(u >> 16);
        p[3] = (uint8_t)(u >> 24);
    }
}

/**
 * Fill `frames` stereo frames of the DMA buffer's format at `dest` from the
 * stream, and give the frames of the stream taken for them.
 *
 * The frames a chunk of HDA_STAGE_FRAMES takes are known before it is made,
 * from hda_rs, so one `transfer` gives them all; the caller's silence for
 * frames it lacks is played like data. Runs with interrupts off, from the
 * service or `stream_start`.
 */
static uint32_t hda_play_render(uint8_t *dest, uint32_t frames)
{
    audio_stream_t *s = play_stream;
    uint32_t frame = hda_in_frame();
    uint32_t size = AUDIO_SAMPLE_BYTES(current_hw_format);
    uint32_t taken = 0;
    int32_t v[2];

    while (frames > 0) {
        uint32_t chunk = frames < HDA_STAGE_FRAMES ? frames : HDA_STAGE_FRAMES;
        uint32_t need = resample_need(&hda_rs, chunk);
        uint32_t i, k = 0;

        if (need > 0)
            s->transfer(s, hda_stage, need * frame);
        for (i = 0; i < chunk; i++) {
            while (hda_rs.owed > 0) {
                hda_play_frame(hda_stage + k++ * frame, v);
                hda_rs_put(v);
            }
            hda_rs_get(v);
            hda_play_store(dest, v[0]);
            hda_play_store(dest + size, v[1]);
            dest += 2 * size;
        }
        taken += need;
        frames -= chunk;
    }
    stream_taken += taken;
    return taken;
}

/** Fill period `index` of the DMA buffer from the stream, the callback or the
 * ring, and write it back to memory for the controller. */
static void hda_fill_period(uint32_t index)
{
    int16_t *period = audio_buffer + index * period_bytes / 2u;

    if (play_stream)
        stream_in[index] = hda_play_render((uint8_t *)period, period_bytes / hda_out_frame());
    else if (user_callback)
        user_callback(period, period_bytes);
    else
        ring_render_to_hw_buffer(period, period_bytes / hda_out_frame());
    hda_dma_sync_range(period, period_bytes);
    hw_fill_count++;
}

/**
 * The frames of the stream taken and not yet played at byte `pos` of the DMA
 * buffer: those of each period filled ahead of the one playing, the part of
 * the one playing past `pos`, in proportion, and those hda_rs holds past
 * the time of its next frame, which play after them. The periods filled are
 * those from the one playing round to the one playing at the last service,
 * as hda_delay_at says. Interrupts are off, so that the service does not
 * move them between the reads.
 */
static uint32_t hda_play_delay(uint32_t pos)
{
    uint32_t cbl = period_bytes * HDA_PERIODS;
    uint32_t cur, rest, ahead, frames, i;

    pos %= cbl;
    cur = pos / period_bytes;
    rest = (cur + 1) * period_bytes - pos;
    frames = stream_in[cur] * (rest / hda_out_frame()) / (period_bytes / hda_out_frame());
    ahead = (hw_period_last + 2 * HDA_PERIODS - cur - 1) % HDA_PERIODS;
    for (i = 1; i <= ahead; i++)
        frames += stream_in[(cur + i) % HDA_PERIODS];
    return frames + resample_held(&hda_rs);
}

/** The stream's frames played since its start, silence among them: those
 * taken less those not played, and never less than the last given out.
 * Interrupts are off and the stream runs. */
static uint32_t hda_play_played(void)
{
    uint32_t pos = stream_taken - hda_play_delay(hda_stream_position());

    if ((int32_t)(pos - stream_pos) > 0)
        stream_pos = pos;
    return stream_pos;
}

/** Call the stream's `period` once for each of its periods played since the
 * last call, from the service after its transfers. */
static void hda_play_signal(void)
{
    audio_stream_t *s = play_stream;
    uint32_t frames = s->period_bytes / hda_in_frame();
    uint32_t pos = hda_play_played();

    while (pos - stream_mark >= frames) {
        stream_mark += frames;
        s->period(s);
    }
}

/**
 * Refill every DMA period played since the last call.
 *
 * Called from the stream's interrupt and from every timer tick (hda_tick),
 * both with interrupts off. The play position, from the position buffer or
 * LPIB, names the period playing; the periods from the one playing at the
 * last call up to this one have been played, and are filled in that order, so
 * that they play after the periods filled before them.
 *
 * The position cannot show a whole revolution of the buffer between two
 * calls, after which every period but the one playing has been played again.
 * The wall clock, which counts 24 MHz whatever the stream does, can: when the
 * time since the last call exceeds the bytes the position moved by a
 * revolution, less half a period of tolerance, the revolutions are counted in
 * missed_revolutions, the rest of the period playing is silenced, the ring is
 * emptied, since what it holds was due while the stream went round, and
 * every other period is filled afresh, so that nothing stale is played. The
 * ring's bytes count as dropped. A stream's frames are the caller's, and are
 * played late instead; the period playing then holds none of them.
 *
 * A stream's `period` is called after the fills, once for each of its
 * periods the position has passed.
 */
static void hda_service_stream(void)
{
    uint32_t cbl, pos, now, period, elapsed, moved, rev, half, first, count, i;

    if (!playing || !stream_started || !hda_base) {
        return;
    }

    cbl = period_bytes * HDA_PERIODS;
    pos = hda_stream_position() % cbl;
    now = hda_read32(HDA_WALCLK);
    period = pos / period_bytes;

    elapsed = (now - walclk_last) / 24u;
    moved = (pos + cbl - lpib_last) % cbl;
    if (pos < lpib_last)
        lpib_wraps++;
    rev = hda_bytes_us(cbl);
    half = hda_bytes_us(period_bytes) / 2u;
    moved = hda_bytes_us(moved);
    if (elapsed + half >= moved + rev) {
        uint32_t missed = (elapsed + half - moved) / rev;
        uint32_t from = pos - pos % hda_out_frame();
        uint8_t *rest = (uint8_t *)audio_buffer + from;
        uint32_t size = (period + 1) * period_bytes - from;

        missed_revolutions += missed;
        lpib_wraps += missed;
        fill_silence(rest, size, current_hw_format != AUDIO_U8);
        hda_dma_sync_range(rest, size);
        if (play_stream) {
            stream_in[period] = 0;
        } else if (!user_callback) {
            session_bytes_dropped += ring_available();
            ring_read_ptr = ring_write_ptr;
            resample_init(&hda_rs, current_sample_rate, current_hw_sample_rate);
        }
        first = period + 1;
        count = HDA_PERIODS - 1;
    } else {
        first = hw_period_last;
        count = (period + HDA_PERIODS - hw_period_last) % HDA_PERIODS;
    }
    lpib_last = pos;
    walclk_last = now;
    hw_period_last = period;

    for (i = 0; i < count; i++)
        hda_fill_period((first + i) % HDA_PERIODS);
    if (play_stream)
        hda_play_signal();
}

/**
 * Turn EAPD on or off on every EAPD-capable pin of the function group, by
 * the pin caps discovery read.
 *
 * Many laptops power the speaker amplifier through the EAPD of a pin that is
 * not the one chosen as primary output, so a start sets EAPD on all of them:
 * the speaker pin EAPD and BTL (0x03), the others EAPD alone (0x02). A stop
 * clears it on all of them, so that the amplifiers are off between streams.
 */
static void hda_set_eapd(bool on)
{
    int i;

    for (i = 0; i < hda_report.widget_count; i++) {
        const hda_widget_report_t *w = &hda_report.widgets[i];

        if (w->type != HDA_WIDGET_PIN || !(w->pin_caps & 0x10000)) /* EAPD capable */
            continue;
        hda_send_verb(codec_addr, w->nid,
                      HDA_VERB_SET_EAPD | (!on                         ? 0x00
                                           : w->nid == speaker_pin_nid ? 0x03
                                                                       : 0x02));
    }
}

#ifdef RACH2_HOSTTEST
extern uint32_t hda_test_read(uint32_t offset, int bits);
extern void hda_test_write(uint32_t offset, uint32_t value, int bits);
#endif

/** Read the controller's 8-bit register at `offset`; the host test supplies
 * the register model. */
static uint8_t hda_read8(uint32_t offset)
{
#ifdef RACH2_HOSTTEST
    return (uint8_t)hda_test_read(offset, 8);
#else
    return mmio_read8(hda_base + offset);
#endif
}

/** Read the controller's 16-bit register at `offset`. */
static uint16_t hda_read16(uint32_t offset)
{
#ifdef RACH2_HOSTTEST
    return (uint16_t)hda_test_read(offset, 16);
#else
    return mmio_read16(hda_base + offset);
#endif
}

/** Read the controller's 32-bit register at `offset`. */
static uint32_t hda_read32(uint32_t offset)
{
#ifdef RACH2_HOSTTEST
    return (uint32_t)hda_test_read(offset, 32);
#else
    return mmio_read32(hda_base + offset);
#endif
}

/** Write the controller's 8-bit register at `offset`. */
static void hda_write8(uint32_t offset, uint8_t value)
{
#ifdef RACH2_HOSTTEST
    hda_test_write(offset, value, 8);
#else
    mmio_write8(hda_base + offset, value);
#endif
}

/** Write the controller's 16-bit register at `offset`. */
static void hda_write16(uint32_t offset, uint16_t value)
{
#ifdef RACH2_HOSTTEST
    hda_test_write(offset, value, 16);
#else
    mmio_write16(hda_base + offset, value);
#endif
}

/** Write the controller's 32-bit register at `offset`. */
static void hda_write32(uint32_t offset, uint32_t value)
{
#ifdef RACH2_HOSTTEST
    hda_test_write(offset, value, 32);
#else
    mmio_write32(hda_base + offset, value);
#endif
}

/** The MMIO offset of stream descriptor `stream`: 0x80 + stream * 0x20. */
static uint32_t hda_sd_offset(int stream)
{
    return 0x80 + (stream * 0x20);
}

/** Read register `reg` of stream descriptor `stream`. */
static uint32_t hda_sd_read32(int stream, uint32_t reg)
{
    return hda_read32(hda_sd_offset(stream) + reg);
}

/** Write the 32-bit register `reg` of stream descriptor `stream`. */
static void hda_sd_write32(int stream, uint32_t reg, uint32_t value)
{
    hda_write32(hda_sd_offset(stream) + reg, value);
}

/** Write the 16-bit register `reg` of stream descriptor `stream`. */
static void hda_sd_write16(int stream, uint32_t reg, uint16_t value)
{
    hda_write16(hda_sd_offset(stream) + reg, value);
}

/** Write the 8-bit register `reg` of stream descriptor `stream`. */
static void hda_sd_write8(int stream, uint32_t reg, uint8_t value)
{
    hda_write8(hda_sd_offset(stream) + reg, value);
}

/**
 * Reset a stream descriptor through the SRST handshake.
 *
 * Stops the stream, asserts SRST and polls until the controller reflects it,
 * then deasserts and polls until it clears. Real hardware needs the poll on
 * each edge, not a fixed delay. Returns false on either poll timeout.
 */
static bool hda_stream_reset(int stream)
{
    int timeout;
    uint32_t ctl_off = hda_sd_offset(stream) + HDA_SD_CTL;

    hda_write8(ctl_off, 0);
    hda_delay(100);

    hda_write8(ctl_off, HDA_SD_CTL_SRST);
    timeout = 100000;
    while (!(hda_read8(ctl_off) & HDA_SD_CTL_SRST) && timeout-- > 0) {
        hda_delay(1);
    }
    if (timeout <= 0) {
        klog(KLOG_WARN, "hda", "stream reset: the set never took");
        return false;
    }

    hda_write8(ctl_off, 0);
    timeout = 100000;
    while ((hda_read8(ctl_off) & HDA_SD_CTL_SRST) && timeout-- > 0) {
        hda_delay(1);
    }
    if (timeout <= 0) {
        klog(KLOG_WARN, "hda", "stream reset: the clear never took");
        return false;
    }

    return true;
}

/** Busy-wait `count` microseconds (k_udelay). */
static void hda_delay(int count)
{
    k_udelay((uint32_t)count);
}

/* The identical WALCLK reads in a row after which the wall clock counts as
 * stopped. A running 24MHz clock changes between two reads, since one MMIO
 * read takes longer than its 41ns tick. */
#define HDA_WALCLK_STUCK_READS 100000u

/**
 * Busy-wait `us` microseconds against the 24MHz HDA wall clock.
 *
 * Unlike a timer-driven sleep this works with interrupts disabled (for
 * example inside a syscall), since it only reads the WALCLK register. A
 * controller whose wall clock does not advance (one failed or not fully
 * powered) would hold the caller here for ever, so after
 * HDA_WALCLK_STUCK_READS identical reads the wait is made with hda_delay
 * instead.
 */
static void hda_delay_walclk_us(uint32_t us)
{
    uint32_t start, now, prev;
    uint32_t delta_ticks;
    uint32_t stalled = 0;

    if (!hda_base || us == 0) {
        return;
    }

    /* 24 ticks per microsecond. */
    delta_ticks = us * 24u;
    start = hda_read32(HDA_WALCLK);
    prev = start;
    for (;;) {
        now = hda_read32(HDA_WALCLK);
        if ((uint32_t)(now - start) >= delta_ticks) {
            return;
        }
        if (now == prev) {
            if (++stalled >= HDA_WALCLK_STUCK_READS) {
                hda_delay((int)us);
                return;
            }
        } else {
            stalled = 0;
            prev = now;
        }
    }
}

/**
 * Force the controller into PCI power state D0.
 *
 * Some machines leave the HDA controller in D3hot at boot, where its MMIO and
 * streams do not respond. Clears the PMCSR power-state bits and waits 200 ms
 * for the device to wake. A no-op when there is no PM capability or it is
 * already D0.
 */
static void hda_pci_force_d0(pci_device_t *dev)
{
    uint8_t pmcap;
    uint16_t pmcsr;
    uint8_t state;

    if (!dev) {
        return;
    }

    pmcap = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_PM);
    if (pmcap == 0) {
        return;
    }

    pmcsr = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(pmcap + 4));
    state = (uint8_t)(pmcsr & 0x3);

    if (state == 0) {
        return;
    }

    pmcsr &= (uint16_t)~0x3;
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(pmcap + 4), pmcsr);

    hda_delay(200000);
}

/**
 * Reset the HDA controller and wait for the codecs to come up.
 *
 * Drives GCTL.CRST low then high, polling the bit on each edge, then, since
 * some codecs take 100 to 500 ms to initialize, polls STATESTS every 10 ms
 * for up to 500 ms until a codec responds, and lets it settle for 50 ms
 * more. Returns false when the controller does not enter or leave reset; a
 * codec that never responds is left for hda_find_output to find.
 */
static bool hda_reset(void)
{
    int timeout;

    hda_write32(HDA_GCTL, 0);
    hda_delay(100);

    timeout = 10000;
    while ((hda_read32(HDA_GCTL) & HDA_GCTL_CRST) && timeout-- > 0) {
        hda_delay(10);
    }
    if (timeout <= 0) {
        klog(KLOG_WARN, "hda", "reset: the controller never entered it");
        return false;
    }

    hda_write32(HDA_GCTL, HDA_GCTL_CRST);
    hda_delay(100);

    timeout = 10000;
    while (!(hda_read32(HDA_GCTL) & HDA_GCTL_CRST) && timeout-- > 0) {
        hda_delay(10);
    }
    if (timeout <= 0) {
        klog(KLOG_WARN, "hda", "reset: the controller never left it");
        return false;
    }

    {
        int wait_count;
        uint16_t sts;

        for (wait_count = 0; wait_count < 50; wait_count++) {
            hda_delay(10000);
            sts = hda_read16(HDA_STATESTS);
            if (sts != 0) {
                break;
            }
        }
        hda_delay(50000);
    }

    return true;
}

/**
 * Initialize the CORB (Command Outbound Ring Buffer) and start it running.
 *
 * Stops the engine, programs the base and a 256-entry size, resets the read
 * pointer through its set/clear handshake, syncs the write pointer to it, then
 * sets CORBCTL.RUN. The driver sends every verb over ICI, so the CORB runs
 * with nothing written to it.
 */
static void hda_setup_corb(void)
{
    uint32_t addr;
    int timeout;

    hda_write8(HDA_CORBCTL, 0);
    hda_delay(100);

    addr = (uint32_t)virt_to_phys(corb_buffer);
    hda_write32(HDA_CORBLBASE, addr);
    hda_write32(HDA_CORBUBASE, hda_hi(virt_to_phys(corb_buffer)));

    hda_write8(HDA_CORBSIZE, 0x02); /* 256 entries */

    /* Reset the read pointer: set bit 15 and wait for it to read back set. */
    hda_write16(HDA_CORBRP, 0x8000);
    timeout = 1000;
    while (!(hda_read16(HDA_CORBRP) & 0x8000) && timeout-- > 0) {
        hda_delay(10);
    }

    /* Clear reset bit */
    hda_write16(HDA_CORBRP, 0x0000);
    timeout = 1000;
    while ((hda_read16(HDA_CORBRP) & 0x8000) && timeout-- > 0) {
        hda_delay(10);
    }

    corb_wp = hda_read16(HDA_CORBRP) & 0xFF;
    hda_write16(HDA_CORBWP, corb_wp);

    hda_write8(HDA_CORBCTL, 0x02); /* CORBCTL.RUN */
}

/**
 * Initialize the RIRB (Response Inbound Ring Buffer) and start it running.
 *
 * Mirrors CORB setup: stop, program base and 256-entry size, reset the write
 * pointer, sync the software read pointer to it, set the response interrupt
 * count, then set RIRBCTL.RUN. The write pointer's reset bit (15) is
 * write-only and reads back as zero, so there is nothing to poll: a 100 us
 * delay lets the controller act on it before the pointer is read.
 */
static void hda_setup_rirb(void)
{
    uint32_t addr;

    hda_write8(HDA_RIRBCTL, 0);
    hda_delay(100);

    addr = (uint32_t)virt_to_phys(rirb_buffer);
    hda_write32(HDA_RIRBLBASE, addr);
    hda_write32(HDA_RIRBUBASE, hda_hi(virt_to_phys(rirb_buffer)));

    hda_write8(HDA_RIRBSIZE, 0x02); /* 256 entries */

    hda_write16(HDA_RIRBWP, 0x8000);
    hda_delay(100);

    rirb_rp = hda_read16(HDA_RIRBWP) & 0xFF;

    hda_write16(HDA_RINTCNT, 1); /* interrupt after 1 response */

    hda_write8(HDA_RIRBCTL, 0x02); /* RIRBCTL.RUN */
}

/* The failures of the first three verbs are logged; counts down to 0. */
static int verb_debug_count = 3;

/* One verb at a time on the immediate command interface: its holder owns
 * ICW, IRR and ICS from the wait for an idle interface to the response. */
static struct spinlock verb_lock = SPINLOCK_INIT;

/* The longest a verb waits on the interface, in ticks of the 24 MHz wall
 * clock: 1 ms. A codec answers in the next link frame, 21 us at 48 kHz. */
#define HDA_VERB_TICKS 24000u

/* The polls after which a verb gives up when the wall clock does not run;
 * each waits a microsecond, so they last at least as long as the clock's
 * limit. */
#define HDA_VERB_POLLS 1000

/* A verb's wait on the interface: when it began, the polls made, and the
 * interrupt flags of the caller, which are put back between polls. */
struct hda_verb_wait {
    uint32_t start;
    int polls;
    uint32_t flags;
};

/**
 * Wait one poll's interval of a verb, with interrupts as the caller had
 * them. Return false, without waiting, once the verb has waited 1 ms by the
 * wall clock or HDA_VERB_POLLS polls. The first call reads the clock to
 * start the count, so a verb that never waits does not read it.
 */
static bool hda_verb_wait(struct hda_verb_wait *w)
{
    uint32_t now = hda_read32(HDA_WALCLK);

    if (w->polls++ == 0)
        w->start = now;
    else if ((uint32_t)(now - w->start) >= HDA_VERB_TICKS || w->polls > HDA_VERB_POLLS)
        return false;
    kirq_restore(w->flags);
    hda_delay(1);
    w->flags = kirq_save();
    return true;
}

/* hda_verb's `mode`: wait for the interface, or give up at once when it is
 * busy, as a caller in the tick must. */
#define HDA_VERB_WAIT 0
#define HDA_VERB_TRY  1

/**
 * Send a codec verb over the Immediate Command Interface and store its
 * response in `*response`, which may be NULL.
 *
 * ICI is simpler than CORB/RIRB, and with one verb at a time it is as fast.
 * The command packs the codec address, node ID and verb. The verb lock is
 * held from the wait for an idle interface to the response's read, with
 * interrupts off only while a register is read or written: the waits between
 * polls run with the caller's interrupts, so a verb sent from a system call
 * does not hold off the tick. The tick may stop such a verb half-way, so a
 * caller in the tick or another interrupt passes HDA_VERB_TRY, which gives up
 * when the lock is held or the interface is still busy with a command before
 * it; waiting there would never end, or end too late for the tick.
 *
 * Return 0 with the response; -EBUSY in HDA_VERB_TRY mode when the interface
 * is in use, having sent nothing; -EIO when the controller is not mapped or
 * the codec completed the command with no response; -ETIMEDOUT when the
 * interface stayed busy, or no response came, within 1 ms. On an error
 * `*response` is 0, and the caller treats what it asked as unknown.
 */
static int hda_verb(uint8_t caddr, uint8_t nid, uint32_t verb, int mode, uint32_t *response)
{
    struct hda_verb_wait w;
    uint32_t cmd = ((uint32_t)caddr << 28) | ((uint32_t)nid << 20) | (verb & 0xFFFFF);
    uint16_t ics;
    int err = -ETIMEDOUT;
    bool debug = (verb_debug_count > 0);

    if (response)
        *response = 0;
    if (!hda_base)
        return -EIO;
    w.polls = 0;
    w.flags = kirq_save();
    if (mode == HDA_VERB_TRY) {
        if (!spin_trylock(&verb_lock)) {
            kirq_restore(w.flags);
            return -EBUSY;
        }
        if (hda_read16(HDA_ICS) & HDA_ICS_ICB) {
            spin_unlock(&verb_lock);
            kirq_restore(w.flags);
            return -EBUSY;
        }
    } else {
        spin_lock(&verb_lock);
    }

    while (hda_read16(HDA_ICS) & HDA_ICS_ICB) {
        if (!hda_verb_wait(&w)) {
            if (debug) {
                klog(KLOG_DEBUG, "hda", "verb: the command interface is busy");
                verb_debug_count--;
            }
            goto out;
        }
    }

    hda_write16(HDA_ICS, HDA_ICS_IRV); /* IRV is write-1-to-clear */
    hda_write32(HDA_ICW, cmd);
    hda_write16(HDA_ICS, HDA_ICS_ICB); /* ICB starts the command */

    for (;;) {
        ics = hda_read16(HDA_ICS);
        if (ics & HDA_ICS_IRV) {
            uint32_t r = hda_read32(HDA_IRR);
            hda_write16(HDA_ICS, HDA_ICS_IRV);
            if (response)
                *response = r;
            err = 0;
            break;
        }
        if (!(ics & HDA_ICS_ICB)) {
            /* ICB cleared without IRV: command done, no response. */
            if (debug) {
                klog(KLOG_DEBUG, "hda", "verb: no response valid, ics 0x%x", ics);
                verb_debug_count--;
            }
            err = -EIO;
            break;
        }
        if (!hda_verb_wait(&w)) {
            if (debug) {
                klog(KLOG_WARN, "hda", "verb: timed out, ics 0x%x", ics);
                verb_debug_count--;
            }
            break;
        }
    }
out:
    spin_unlock(&verb_lock);
    kirq_restore(w.flags);
    return err;
}

/**
 * Send a codec verb, waiting for the interface (hda_verb), and return the
 * response, or 0 when the verb failed. Not for the tick or an interrupt
 * handler, which use hda_verb with HDA_VERB_TRY.
 */
static uint32_t hda_send_verb(uint8_t caddr, uint8_t nid, uint32_t verb)
{
    uint32_t response;

    hda_verb(caddr, nid, verb, HDA_VERB_WAIT, &response);
    return response;
}

/** Read parameter `param` of node `nid` of the chosen codec. */
static uint32_t hda_get_param(uint8_t nid, uint8_t param)
{
    return hda_send_verb(codec_addr, nid, HDA_VERB_GET_PARAM | param);
}

/**
 * Read node `nid`'s connection list into `conn_list`, at most `max_conn`
 * nodes, expanding ranges in selector order; return the count, or -1.
 *
 * The entries are packed four short or two long ones to a response, and each
 * response is read at the offset of its first entry. Long entries retain all
 * fifteen NID bits. A range must follow a smaller NID. Malformed lists and
 * lists exceeding the caller's capacity return -1; no caller may route
 * through their partial output. Direct verbs address only seven-bit NIDs,
 * checked separately before querying a connection (hda_connection_caps).
 * With `report`, the raw entries and the count are recorded there too.
 */
static int hda_read_connections(uint8_t nid, uint16_t *conn_list, int max_conn,
                                hda_connection_report_t *report)
{
    uint32_t param = hda_get_param(nid, HDA_PARAM_CONN_LIST);
    int length = param & 0x7F;
    int bits = (param & 0x80) ? 16 : 8;
    int per_word = 32 / bits;
    uint32_t range_bit = 1u << (bits - 1);
    uint32_t word = 0;
    uint16_t previous = 0;
    int count = 0;
    int i;

    if (report) {
        report->nid = nid;
        report->long_form = bits == 16;
        report->raw_count = 0;
        report->count = -1;
    }
    for (i = 0; i < length; i++) {
        uint16_t entry;
        uint16_t node;
        uint32_t first, n;

        if (i % per_word == 0)
            word = hda_send_verb(codec_addr, nid, HDA_VERB_GET_CONN_LIST | i);
        entry = (uint16_t)((word >> ((i % per_word) * bits)) & (range_bit * 2 - 1));
        if (report) {
            report->raw[i] = entry;
            report->raw_count = i + 1;
        }
        node = entry & (range_bit - 1);
        first = node;
        if (entry & range_bit) {
            if (i == 0 || node <= previous)
                return -1;
            first = (uint32_t)previous + 1;
        }
        if (node == 0 || node - first + 1 > (uint32_t)(max_conn - count))
            return -1;
        for (n = first; n <= node; n++)
            conn_list[count++] = (uint16_t)n;
        previous = node;
    }
    if (report)
        report->count = count;
    return count;
}

/** Read a complete connection list without retaining a diagnostic copy. */
static int hda_get_connections(uint8_t nid, uint16_t *conn_list, int max_conn)
{
    return hda_read_connections(nid, conn_list, max_conn, NULL);
}

/** The audio widget capabilities of connection `nid`, or 0xF00000 (a widget
 * type no path takes) for a NID a direct verb cannot address (above 127) or
 * one outside the function group. */
static uint32_t hda_connection_caps(uint16_t nid)
{
    if (nid > 127 || nid < afg_widget_start || nid >= (unsigned)afg_widget_start + afg_widget_count)
        return 0xF00000;
    return hda_get_param((uint8_t)nid, HDA_PARAM_AUDIO_CAPS);
}

/** Fill `*caps` with DAC `nid`'s format parameters, read from the DAC under
 * Format Override and from the function group otherwise; all zero for `nid`
 * 0. */
static void hda_read_caps(uint8_t nid, hda_caps_report_t *caps)
{
    uint32_t widgets;

    k_memset(caps, 0, sizeof(*caps));
    if (!nid)
        return;
    widgets = hda_get_param(nid, HDA_PARAM_AUDIO_CAPS);
    caps->nid = nid;
    caps->source = (widgets & (1u << 4)) ? nid : audio_fg_nid;
    caps->pcm = hda_get_param(caps->source, 0x0A);
    caps->formats = hda_get_param(caps->source, 0x0B);
}

/**
 * Read every widget of the function group into the report: its type, its
 * capabilities and amplifiers', a pin's capabilities and configuration
 * default, and its connection list, expanded. The pins' sense words are
 * read at each start (hda_sense_pins).
 */
static void hda_capture_widgets(void)
{
    uint32_t afg_in = hda_get_param(audio_fg_nid, HDA_PARAM_INPUT_AMP);
    uint32_t afg_out = hda_get_param(audio_fg_nid, HDA_PARAM_OUTPUT_AMP);
    uint16_t conns[HDA_MAX_CONNECTIONS];
    int i, j, n;

    hda_report.gpio = hda_get_param(audio_fg_nid, HDA_PARAM_GPIO_COUNT);
    hda_report.widget_count = afg_widget_count;
    for (i = 0; i < afg_widget_count; i++) {
        hda_widget_report_t *w = &hda_report.widgets[i];
        uint8_t nid = (uint8_t)(afg_widget_start + i);
        bool override;

        k_memset(w, 0, sizeof(*w));
        w->nid = nid;
        w->caps = hda_get_param(nid, HDA_PARAM_AUDIO_CAPS);
        w->type = (uint8_t)((w->caps >> 20) & 0xF);
        override = (w->caps & (1u << 3)) != 0; /* Amp Param Override */
        if (w->caps & (1u << 1))               /* In Amp Present */
            w->amp_in = override ? hda_get_param(nid, HDA_PARAM_INPUT_AMP) : afg_in;
        if (w->caps & (1u << 2)) /* Out Amp Present */
            w->amp_out = override ? hda_get_param(nid, HDA_PARAM_OUTPUT_AMP) : afg_out;
        if (w->type == HDA_WIDGET_PIN) {
            w->pin_caps = hda_get_param(nid, HDA_PARAM_PIN_CAPS);
            w->config = hda_send_verb(codec_addr, nid, HDA_VERB_GET_CONFIG_DEF);
        }
        n = hda_get_connections(nid, conns, HDA_MAX_CONNECTIONS);
        if (n > HDA_WIDGET_REPORT_CONNS)
            n = -1;
        w->conn_count = n;
        for (j = 0; j < n; j++)
            w->conns[j] = conns[j];
    }
}

/**
 * Read Get Pin Sense into the report for every pin with presence detect,
 * first executing the sense on a pin that needs a trigger. hda_jacks_sense
 * calls it once the path is powered; the word of a pin without presence
 * detect, or whose verb failed, stays 0, and `sensed` false.
 */
static void hda_sense_pins(void)
{
    int i;

    for (i = 0; i < hda_report.widget_count; i++) {
        hda_widget_report_t *w = &hda_report.widgets[i];

        w->sensed = false;
        w->sense = 0;
        if (w->type != HDA_WIDGET_PIN || !(w->pin_caps & (1u << 2))) /* Presence Detect */
            continue;
        if (w->pin_caps & (1u << 1)) /* Trigger Required */
            hda_send_verb(codec_addr, w->nid, HDA_VERB_SET_PIN_SENSE);
        w->sensed =
            hda_verb(codec_addr, w->nid, HDA_VERB_GET_PIN_SENSE, HDA_VERB_WAIT, &w->sense) == 0;
    }
}

/** Cache the selected paths, their codec lists and every widget of the
 * function group for reads through procfs. */
static void hda_capture_report(void)
{
    int i;
    uint8_t nodes[3];

    hda_report.vendor = codec_vendor_id;
    hda_report.codec = codec_addr;
    hda_report.afg = audio_fg_nid;
    hda_report.pin = pin_nid;
    hda_report.dac = dac_nid;
    hda_report.mixer = mixer_nid;
    hda_report.hp_pin = hp_pin_nid;
    hda_report.hp_dac = hp_dac_nid;
    hda_report.pin_index = pin_conn_sel_idx;
    hda_report.mixer_index = mixer_conn_sel_idx;
    hda_report.hp_index = hp_conn_sel_idx;
    hda_read_caps(dac_nid, &hda_report.caps[0]);
    hda_read_caps(hp_dac_nid, &hda_report.caps[1]);
    nodes[0] = pin_nid;
    nodes[1] = mixer_nid;
    nodes[2] = hp_pin_nid;
    for (i = 0; i < 3; i++) {
        hda_connection_report_t *c = &hda_report.connections[i];
        k_memset(c, 0, sizeof(*c));
        if (nodes[i])
            hda_read_connections(nodes[i], c->nodes, HDA_MAX_CONNECTIONS, c);
    }
    hda_capture_widgets();
}

const hda_report_t *hda_get_report(void)
{
    return &hda_report;
}

/*
 * The input path: the ADC, the pin it records from and a mixer or selector
 * between them, which hda_find_input chooses from the report's widgets. The
 * input stream itself is further on, by the stream operations.
 */
static uint8_t adc_nid = 0;      /* ADC the input stream records from */
static uint8_t in_pin_nid = 0;   /* the pin it records */
static uint8_t in_conn_idx = 0;  /* the ADC's connection toward the pin */
static uint8_t in_mid_nid = 0;   /* a mixer or selector between, or 0 */
static uint8_t in_mid_idx = 0;   /* its connection to the pin */
static uint8_t int_mic_nid = 0;  /* the internal microphone's pin */
static uint8_t jack_mic_nid = 0; /* the microphone jack's pin */
static uint8_t line_in_nid = 0;  /* the first line in's pin */
static int in_src = -1;          /* the audio_source_t recorded, or -1 */

/* Each source's amplifier words, by audio_source_t, which the mixer sets:
 * the ADC's input gain on the source's connection, and the gain of the
 * source's pin. The low seven bits are the step and 0x80 is mute, as in a
 * Set Amplifier Gain verb's payload. */
static uint8_t in_gain[AUDIO_SOURCES];
static uint8_t in_boost[AUDIO_SOURCES];

/*
 * The input stream. Its DMA buffer is `cap_periods` periods of `cap_period`
 * bytes, which the ADC fills with 16-bit samples in its own channels at a
 * rate it has; the service converts each finished period to the reader's
 * format and channels, hands it over with `transfer` and calls `period`.
 * No rate is converted: stream_setup grants the ADC's rate nearest the one
 * asked for. The service runs from the interrupt and the tick under
 * `cap_lock`, with interrupts off; the stream operations take the lock to
 * change what it reads.
 */
#define HDA_CAPTURE_TAG       2      /* the input stream's tag; playback's is 1 */
#define HDA_CAPTURE_BUFFER    65536u /* bytes of the DMA buffer at most */
#define HDA_CAPTURE_PERIOD    16384u /* bytes of one DMA period at most */
#define HDA_CAPTURE_ENTRIES   32u    /* periods in the DMA buffer at most */
/* Microseconds a capture's start waits after formatting the converter, and
 * again after unmuting the ADC's amplifier, before the DMA records. */
#define HDA_CAPTURE_SETTLE_US 10000u
/* The internal mic's boost at discovery, in dB: Linux under PipeWire sets
 * the whole path to its top, +6 and +36 dB on the G50-70, where speech at 0
 * dB peaks near -34 dBFS and would clip; 24 dB brings it near -10 dBFS. */
#define HDA_INT_MIC_BOOST_DB  24u

static volatile hda_bdl_entry_t cap_bdl[HDA_CAPTURE_ENTRIES] __attribute__((aligned(128)));
static uint8_t cap_buffer[HDA_CAPTURE_BUFFER] __attribute__((aligned(128)));
static uint8_t cap_out[HDA_CAPTURE_PERIOD]; /* a period in the reader's format */
static struct spinlock cap_lock = SPINLOCK_INIT;
static audio_stream_t *cap_stream = NULL; /* the open input stream, or NULL */
static volatile bool cap_running = false;
static uint16_t cap_format = 0;   /* the stream format word stream_setup chose */
static uint32_t cap_asked = 0;    /* the rate stream_setup was asked for */
static uint32_t cap_period = 0;   /* bytes of one DMA period; 0 before a setup */
static uint32_t cap_periods = 0;  /* periods in the DMA buffer */
static uint32_t cap_frame = 4;    /* bytes of one frame in the DMA buffer */
static uint32_t cap_next = 0;     /* the period the service hands over next */
static uint32_t cap_pos = 0;      /* the position at the last service, bytes */
static uint32_t cap_clock = 0;    /* the wall clock at the last service */
static uint32_t cap_recorded = 0; /* frames recorded up to `cap_pos` */
static uint32_t cap_handed = 0;   /* frames handed to the reader */

static void hda_service_capture(void);
static void hda_capture_stop(audio_stream_t *s);
static bool hda_input_ready(void);
static void hda_find_jacks(void);
static void hda_find_beep(void);
static void hda_beep_tick(uint32_t passed);

/* The PCI subsystem, vendor in the high half, which names a machine's
 * quirks; hda_init reads it. */
static uint32_t hda_subsystem = 0;

/* Machines whose internal microphone is a stereo pair of digital mics with
 * the right one inverted, so that the two channels cancel when mixed:
 * Linux's CXT_FIXUP_STEREO_DMIC. */
static const uint32_t hda_inv_dmic_machines[] = {
    0x17AA3978, /* Lenovo G50-70 */
};

/** The report's entry for widget `nid`, or NULL for a node outside the
 * function group. */
static hda_widget_report_t *hda_widget(uint16_t nid)
{
    if (nid < afg_widget_start || nid >= (unsigned)afg_widget_start + afg_widget_count ||
        (int)(nid - afg_widget_start) >= hda_report.widget_count)
        return NULL;
    return &hda_report.widgets[nid - afg_widget_start];
}

/** The index of `nid` in widget `w`'s connection list, or -1. */
static int hda_conn_index(const hda_widget_report_t *w, uint16_t nid)
{
    int i;

    for (i = 0; w && i < w->conn_count; i++)
        if (w->conns[i] == nid)
            return i;
    return -1;
}

/**
 * Find how ADC `adc` reaches pin `pin`: directly, or through one mixer or
 * selector among its connections. Return true and set `*index` to the ADC's
 * connection, `*mid` to the mixer or selector (0 when direct) and
 * `*mid_index` to its connection to the pin; false when there is no such
 * route. Reads only the report, so it sends no verb.
 */
static bool hda_input_route(uint8_t adc, uint8_t pin, uint8_t *index, uint8_t *mid,
                            uint8_t *mid_index)
{
    const hda_widget_report_t *a = hda_widget(adc);
    int i, j;

    if (!a || !pin)
        return false;
    if ((i = hda_conn_index(a, pin)) >= 0) {
        *index = (uint8_t)i;
        *mid = 0;
        *mid_index = 0;
        return true;
    }
    for (i = 0; i < a->conn_count; i++) {
        const hda_widget_report_t *m = hda_widget(a->conns[i]);

        if (!m || (m->type != HDA_WIDGET_AUDIO_MIX && m->type != HDA_WIDGET_AUDIO_SEL))
            continue;
        if ((j = hda_conn_index(m, pin)) >= 0) {
            *index = (uint8_t)i;
            *mid = m->nid;
            *mid_index = (uint8_t)j;
            return true;
        }
    }
    return false;
}

/** The pin of source `src`, an audio_source_t, or 0 when there is none. */
static uint8_t hda_source_pin(int src)
{
    return src == AUDIO_SRC_MIC       ? jack_mic_nid
           : src == AUDIO_SRC_INT_MIC ? int_mic_nid
           : src == AUDIO_SRC_LINE    ? line_in_nid
                                      : 0;
}

/**
 * Find the input pins in the report: a pin is an input when its pin caps say
 * it can take input (bit 5) and its configuration default says something is
 * connected. The internal mic is the first fixed microphone pin, the jack
 * mic the first microphone in a jack, the line in the first line in.
 */
static void hda_find_input_pins(void)
{
    int i;

    int_mic_nid = jack_mic_nid = line_in_nid = 0;
    for (i = 0; i < hda_report.widget_count; i++) {
        const hda_widget_report_t *w = &hda_report.widgets[i];
        uint8_t port = (uint8_t)HDA_PIN_DEF_PORT(w->config);
        uint8_t dev = (uint8_t)HDA_PIN_DEF_DEV(w->config);

        if (w->type != HDA_WIDGET_PIN || !(w->pin_caps & (1u << 5)) || /* Input Capable */
            port == HDA_PORT_CONN_NONE)
            continue;
        if (dev == HDA_DEF_DEV_MIC_IN && port == HDA_PORT_CONN_FIXED && !int_mic_nid)
            int_mic_nid = w->nid;
        else if (dev == HDA_DEF_DEV_MIC_IN && port != HDA_PORT_CONN_FIXED && !jack_mic_nid)
            jack_mic_nid = w->nid;
        else if (dev == HDA_DEF_DEV_LINE_IN && !line_in_nid)
            line_in_nid = w->nid;
    }
}

/**
 * The ADC that records pin `pin`: the first, in node order, that reaches
 * it, and of those the first that reaches pin `other` too, when `other` is
 * not 0. 0 when no ADC reaches `pin`.
 */
static uint8_t hda_choose_adc(uint8_t pin, uint8_t other)
{
    uint8_t adc = 0;
    int i, best = -1;

    for (i = 0; pin && i < hda_report.widget_count; i++) {
        const hda_widget_report_t *w = &hda_report.widgets[i];
        uint8_t index, mid, mid_index;
        int score;

        if (w->type != HDA_WIDGET_AUDIO_IN ||
            !hda_input_route(w->nid, pin, &index, &mid, &mid_index))
            continue;
        score = other && hda_input_route(w->nid, other, &index, &mid, &mid_index) ? 1 : 0;
        if (score > best) {
            best = score;
            adc = w->nid;
        }
    }
    return adc;
}

/** The step of an amplifier with capabilities `amp` nearest `db` above its
 * 0 dB step, the offset, and within its steps. Steps are in quarters of a dB
 * less one, in bits 22:16. */
static uint8_t hda_amp_db(uint32_t amp, uint32_t db)
{
    uint32_t quarters = ((amp >> 16) & 0x7F) + 1;
    uint32_t step = (amp & 0x7F) + (db * 4 + quarters / 2) / quarters;

    return (uint8_t)(step < hda_amp_steps(amp) ? step : hda_amp_steps(amp));
}

/**
 * Choose the input path from the widgets in the report: the pins, the ADC
 * and the capture level. hda_find_output calls it once the report has every
 * widget.
 *
 * The pin recorded is the internal mic, else the jack mic, else the line in
 * (hda_find_input_pins). The ADC is the first, in node order, that reaches
 * that pin, and of those it prefers one that reaches the other microphone
 * too, so that a later switch between the two needs only its connection
 * select. Each source's gain starts at the ADC's 0 dB step, the offset of
 * its input amplifier, and its boost at its pin's 0 dB step, but the
 * internal mic's at HDA_INT_MIC_BOOST_DB. On a machine of
 * hda_inv_dmic_machines the internal mic is marked inverted.
 */
static void hda_find_input(void)
{
    hda_input_report_t *in = &hda_report.input;
    uint8_t pin, other;
    int i;

    adc_nid = in_pin_nid = in_conn_idx = in_mid_nid = in_mid_idx = 0;
    in_src = -1;
    hda_find_input_pins();
    pin = int_mic_nid ? int_mic_nid : jack_mic_nid ? jack_mic_nid : line_in_nid;
    other = pin == int_mic_nid ? jack_mic_nid : int_mic_nid;
    adc_nid = hda_choose_adc(pin, other);
    if (adc_nid) {
        in_pin_nid = pin;
        hda_input_route(adc_nid, pin, &in_conn_idx, &in_mid_nid, &in_mid_idx);
        in_src = pin == int_mic_nid    ? AUDIO_SRC_INT_MIC
                 : pin == jack_mic_nid ? AUDIO_SRC_MIC
                                       : AUDIO_SRC_LINE;
        for (i = 0; i < AUDIO_SOURCES; i++) {
            const hda_widget_report_t *p = hda_source_pin(i) ? hda_widget(hda_source_pin(i)) : NULL;

            in_gain[i] = (uint8_t)(hda_widget(adc_nid)->amp_in & 0x7F); /* Offset: 0 dB */
            in_boost[i] = !p                       ? 0
                          : i == AUDIO_SRC_INT_MIC ? hda_amp_db(p->amp_in, HDA_INT_MIC_BOOST_DB)
                                                   : (uint8_t)(p->amp_in & 0x7F);
        }
    }

    in->adc = adc_nid;
    in->pin = in_pin_nid;
    in->index = in_conn_idx;
    in->mid = in_mid_nid;
    in->mid_index = in_mid_idx;
    in->int_mic = int_mic_nid;
    in->jack_mic = jack_mic_nid;
    in->level = adc_nid ? (uint8_t)(in_gain[in_src] & 0x7F) : 0;
    in->inv_dmic = false;
    for (i = 0; int_mic_nid && i < (int)(sizeof(hda_inv_dmic_machines) / sizeof(uint32_t)); i++)
        if (hda_subsystem == hda_inv_dmic_machines[i])
            in->inv_dmic = true;
    hda_read_caps(adc_nid, &in->caps);
}

/**
 * Drive the codec's GPIO pins high to enable external amplifiers.
 *
 * Many laptops switch the speaker amplifier with a codec GPIO, and which one
 * varies by board, so every GPIO the function group has, up to eight, is made
 * an output and driven high. Does nothing when the function group reports no
 * GPIOs.
 */
static void hda_enable_gpio(void)
{
    uint32_t gpio_param;
    int num_gpio;
    uint8_t mask;

    gpio_param = hda_get_param(audio_fg_nid, HDA_PARAM_GPIO_COUNT);
    num_gpio = gpio_param & 0xFF;

    if (num_gpio == 0) {
        return;
    }

    if (num_gpio >= 8) {
        mask = 0xFF;
    } else {
        mask = (uint8_t)((1u << num_gpio) - 1u);
    }

    hda_send_verb(codec_addr, audio_fg_nid, HDA_VERB_SET_GPIO_MASK | mask);
    hda_send_verb(codec_addr, audio_fg_nid, HDA_VERB_SET_GPIO_DIR | mask);
    hda_send_verb(codec_addr, audio_fg_nid, HDA_VERB_SET_GPIO_DATA | mask);
}

/**
 * Report whether a codec vendor/device ID is an HDMI/DisplayPort codec.
 *
 * An HDMI codec has no analog speaker path, so the enumeration below passes
 * over one while any other codec answers, and takes it only when none does.
 * Matches the Intel HDMI codecs, device IDs 0x2800 to 0x28ff and 0x29fb
 * (Crestline), and treats all Nvidia and AMD/ATI codecs as HDMI.
 */
static bool is_hdmi_codec(uint32_t vendor_id)
{
    uint16_t vendor = (vendor_id >> 16) & 0xFFFF;
    uint16_t device = vendor_id & 0xFFFF;

    if (vendor == 0x8086) {
        if ((device >= 0x2800 && device < 0x2900) || device == 0x29fb) {
            return true;
        }
    }

    /* Nvidia HDMI: vendor 0x10de */
    if (vendor == 0x10de) {
        return true;
    }

    /* AMD/ATI HDMI: vendor 0x1002 */
    if (vendor == 0x1002) {
        return true;
    }

    return false;
}

/**
 * Discover the codec and the widget path to drive for output.
 *
 * Takes the first codec in STATESTS that responds and is not an HDMI codec,
 * reading each one's vendor ID up to three times, 50 ms apart; when none is,
 * the first codec that responds at all. A Conexant codec (vendor 0x14f1)
 * starts in the primary output mode, since Conexant laptop codecs route their
 * jacks unreliably, and any other in the automatic mode. The audio function
 * group is powered up and its GPIOs driven (hda_enable_gpio).
 *
 * The first output-capable pin is the fallback, and every one whose port is
 * connected is classified by its default device. The output pin is the first
 * speaker, else line out, else headphone, else the fallback. Its connection
 * list is traced to a DAC: the connection the pin has selected, as the
 * firmware left it, first, then the others in order. A DAC is taken; a mixer
 * or selector is followed one level, its selected input first and then the
 * first DAC among its inputs. The headphone pin, when it is another pin, gets
 * its DAC the same way without the mixer level: its selected connection when
 * that is a DAC, else the first DAC in its list.
 *
 * The chosen pin, DAC, optional mixer, and headphone pin/DAC are left in the
 * file-scope routing globals and the report, and hda_find_input then
 * chooses the input path from the report's widgets. Returns false if no usable
 * codec, function group, pin or DAC is found, or the codec's node counts are
 * out of range.
 */
static bool hda_find_output(void)
{
    uint16_t statests;
    uint32_t vendor, node_count, subnode_count;
    int i, j;
    uint8_t start_nid, num_nodes;
    uint8_t fg_start, fg_count;
    uint8_t speaker_pin = 0, hp_pin = 0, line_out_pin = 0;
    uint8_t fallback_pin = 0;
    uint16_t conn_list[HDA_MAX_CONNECTIONS];
    int num_conn;
    uint8_t analog_codec_addr = 0xFF;
    uint32_t analog_vendor = 0;

    audio_fg_nid = dac_nid = pin_nid = mixer_nid = hp_pin_nid = hp_dac_nid = 0;
    pin_conn_sel_idx = hp_conn_sel_idx = mixer_conn_sel_idx = 0;
    k_memset(&hda_report, 0, sizeof(hda_report));
    statests = hda_read16(HDA_STATESTS);
    if (statests == 0) {
        return false;
    }

    for (i = 0; i < 15; i++) {
        if (statests & (1 << i)) {
            uint32_t vid;
            int retry;

            codec_addr = i;

            vid = 0;
            for (retry = 0; retry < 3 && vid == 0; retry++) {
                if (retry > 0) {
                    hda_delay(50000);
                }
                vid = hda_send_verb(i, 0, HDA_VERB_GET_PARAM | HDA_PARAM_VENDOR_ID);
            }

            if (vid == 0) {
                continue;
            }

            if (is_hdmi_codec(vid)) {
                continue;
            }

            if (analog_codec_addr == 0xFF) {
                analog_codec_addr = i;
                analog_vendor = vid;
            }
        }
    }

    if (analog_codec_addr == 0xFF) {
        for (i = 0; i < 15; i++) {
            if (statests & (1 << i)) {
                uint32_t vid;
                codec_addr = i;
                hda_delay(100000);
                vid = hda_send_verb(i, 0, HDA_VERB_GET_PARAM | HDA_PARAM_VENDOR_ID);
                if (vid != 0) {
                    analog_codec_addr = i;
                    analog_vendor = vid;
                    break;
                }
            }
        }
    }

    if (analog_codec_addr == 0xFF) {
        return false;
    }

    codec_addr = analog_codec_addr;
    vendor = analog_vendor;
    codec_vendor_id = vendor;

    hda_output_mode = HDA_OUTMODE_AUTO;

    node_count = hda_get_param(0, HDA_PARAM_NODE_COUNT);
    start_nid = (node_count >> 16) & 0xFF;
    num_nodes = node_count & 0xFF;
    if (start_nid == 0 || (unsigned)start_nid + num_nodes > 128)
        return false;

    for (i = 0; i < num_nodes; i++) {
        uint32_t func_type = hda_get_param(start_nid + i, HDA_PARAM_FUNC_GROUP);
        if ((func_type & 0xFF) == 0x01) { /* function group type 1 = audio */
            audio_fg_nid = start_nid + i;
            break;
        }
    }

    if (audio_fg_nid == 0) {
        return false;
    }

    hda_send_verb(codec_addr, audio_fg_nid, HDA_VERB_SET_POWER | 0x00);
    hda_delay(100);

    hda_enable_gpio();

    subnode_count = hda_get_param(audio_fg_nid, HDA_PARAM_NODE_COUNT);
    fg_start = (subnode_count >> 16) & 0xFF;
    fg_count = subnode_count & 0xFF;
    if (fg_start == 0 || (unsigned)fg_start + fg_count > 128)
        return false;
    afg_widget_start = fg_start;
    afg_widget_count = fg_count;

    for (j = 0; j < fg_count; j++) {
        uint8_t nid = fg_start + j;
        uint32_t caps = hda_get_param(nid, HDA_PARAM_AUDIO_CAPS);
        uint8_t widget_type = (caps >> 20) & 0xF;

        if (widget_type == HDA_WIDGET_PIN) {
            uint32_t pin_caps = hda_get_param(nid, HDA_PARAM_PIN_CAPS);

            if (pin_caps & 0x10) { /* output capable */
                uint32_t pin_cfg = hda_send_verb(codec_addr, nid, HDA_VERB_GET_CONFIG_DEF);
                uint8_t def_dev = HDA_PIN_DEF_DEV(pin_cfg);
                uint8_t port_conn = HDA_PIN_DEF_PORT(pin_cfg);

                if (fallback_pin == 0) {
                    fallback_pin = nid;
                }

                if (port_conn == HDA_PORT_CONN_NONE) {
                    continue;
                }

                if (def_dev == HDA_DEF_DEV_SPEAKER && speaker_pin == 0) {
                    speaker_pin = nid;
                } else if (def_dev == HDA_DEF_DEV_HP_OUT && hp_pin == 0) {
                    hp_pin = nid;
                } else if (def_dev == HDA_DEF_DEV_LINE_OUT && line_out_pin == 0) {
                    line_out_pin = nid;
                }
            }
        }
    }

    if (speaker_pin != 0) {
        pin_nid = speaker_pin;
    } else if (line_out_pin != 0) {
        pin_nid = line_out_pin;
    } else if (hp_pin != 0) {
        pin_nid = hp_pin;
    } else if (fallback_pin != 0) {
        pin_nid = fallback_pin;
    }

    hp_pin_nid = hp_pin;
    speaker_pin_nid = speaker_pin;

    if (pin_nid == 0) {
        return false;
    }

    num_conn = hda_get_connections(pin_nid, conn_list, HDA_MAX_CONNECTIONS);

    if (num_conn > 0) {
        uint8_t pin_sel;
        int pass;

        pin_sel = (uint8_t)(hda_send_verb(codec_addr, pin_nid, HDA_VERB_GET_CONN_SEL) & 0xFF);
        if (pin_sel >= num_conn) {
            pin_sel = 0;
        }

        for (pass = 0; pass < num_conn; pass++) {
            int idx;

            if (pass == 0) {
                idx = (int)pin_sel;
            } else {
                idx = pass - 1;
                if (idx >= (int)pin_sel) {
                    idx++;
                }
                if (idx >= num_conn) {
                    idx = (int)pin_sel;
                }
            }

            {
                uint16_t conn_nid = conn_list[idx];
                uint32_t caps = hda_connection_caps(conn_nid);
                uint8_t wtype = (caps >> 20) & 0xF;

                if (wtype == HDA_WIDGET_AUDIO_OUT) {
                    dac_nid = (uint8_t)conn_nid;
                    mixer_nid = 0;
                    pin_conn_sel_idx = (uint8_t)idx;
                    break;
                } else if (wtype == HDA_WIDGET_AUDIO_MIX || wtype == HDA_WIDGET_AUDIO_SEL) {
                    uint16_t mix_conn[HDA_MAX_CONNECTIONS];
                    int mix_num;
                    uint8_t mix_sel;

                    mixer_nid = (uint8_t)conn_nid;
                    pin_conn_sel_idx = (uint8_t)idx;

                    mix_num = hda_get_connections(mixer_nid, mix_conn, HDA_MAX_CONNECTIONS);

                    mix_sel =
                        (uint8_t)(hda_send_verb(codec_addr, mixer_nid, HDA_VERB_GET_CONN_SEL) &
                                  0xFF);
                    if (mix_sel < mix_num) {
                        uint32_t mcaps0 = hda_connection_caps(mix_conn[mix_sel]);
                        if (((mcaps0 >> 20) & 0xF) == HDA_WIDGET_AUDIO_OUT) {
                            dac_nid = (uint8_t)mix_conn[mix_sel];
                            mixer_conn_sel_idx = mix_sel;
                            break;
                        }
                    }

                    for (j = 0; j < mix_num; j++) {
                        uint32_t mcaps = hda_connection_caps(mix_conn[j]);
                        if (((mcaps >> 20) & 0xF) == HDA_WIDGET_AUDIO_OUT) {
                            dac_nid = (uint8_t)mix_conn[j];
                            mixer_conn_sel_idx = (uint8_t)j;
                            break;
                        }
                    }
                    if (dac_nid != 0)
                        break;
                    mixer_nid = 0;
                }
            }
        }
    }

    if (dac_nid == 0) {
        return false;
    }

    if (hp_pin_nid != 0 && hp_pin_nid != pin_nid) {
        uint8_t hp_sel;
        uint16_t hp_conn[HDA_MAX_CONNECTIONS];
        int hp_num;

        hp_num = hda_get_connections(hp_pin_nid, hp_conn, HDA_MAX_CONNECTIONS);
        hp_sel = (uint8_t)(hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_GET_CONN_SEL) & 0xFF);
        if (hp_sel >= hp_num) {
            hp_sel = 0;
        }

        if (hp_num > 0) {
            uint32_t caps0 = hda_connection_caps(hp_conn[hp_sel]);
            uint8_t wtype0 = (caps0 >> 20) & 0xF;
            if (wtype0 == HDA_WIDGET_AUDIO_OUT) {
                hp_dac_nid = hp_conn[hp_sel];
            }
            hp_conn_sel_idx = hp_sel;
        }

        if (hp_dac_nid == 0) {
            for (i = 0; i < hp_num; i++) {
                uint32_t caps = hda_connection_caps(hp_conn[i]);
                if (((caps >> 20) & 0xF) == HDA_WIDGET_AUDIO_OUT) {
                    hp_dac_nid = hp_conn[i];
                    hp_conn_sel_idx = (uint8_t)i;
                    break;
                }
            }
        }
    }

    hda_capture_report();
    hda_find_input();
    hda_find_jacks();
    hda_find_beep();
    hda_report.ready = true;
    return true;
}

typedef struct {
    uint32_t rate;
    uint16_t format;
} hda_rate_t;

/* The PCM rates in the order of their bits in the PCM sizes and rates
 * parameter, each with its stream format word (base, multiplier, divisor). */
static const hda_rate_t hda_rates[] = {{8000, 0x0500},   {11025, 0x4300},  {16000, 0x0200},
                                       {22050, 0x4100},  {32000, 0x0A00},  {44100, 0x4000},
                                       {48000, 0x0000},  {88200, 0x4800},  {96000, 0x0800},
                                       {176400, 0x5800}, {192000, 0x1800}, {384000, 0x3800}};

/** Intersect the format capabilities of every DAC the driver programs: 0
 * when either has no PCM. */
static uint32_t hda_common_pcm(void)
{
    uint32_t pcm = hda_report.caps[0].pcm;

    if (!(hda_report.caps[0].formats & 1))
        return 0;
    if (hp_dac_nid && hp_dac_nid != dac_nid) {
        if (!(hda_report.caps[1].formats & 1))
            return 0;
        pcm &= hda_report.caps[1].pcm;
    }
    return pcm;
}

/**
 * Encode an exact codec-supported PCM format, or return 0xFFFF.
 *
 * The table index is the PCM rate bit. AUDIO_S32LE is 24 bits in a 32-bit
 * container, the top 24 bits played, as Linux plays S32_LE to a codec of
 * 24 bits; it needs the 24-bit size bit, 19, of the PCM sizes and rates
 * word, and U8 and S16LE bits 16 and 17. Both the stream descriptor and
 * every programmed converter receive this word; an unsupported rate is never
 * rounded here. Callback playback must match exactly because it has no
 * resampler. Write playback chooses a supported rate before encoding.
 */
static uint16_t hda_calc_format(uint32_t sample_rate, audio_format_t format, int channels)
{
    unsigned i;
    uint32_t pcm = hda_common_pcm();
    uint32_t size_bit = format == AUDIO_S32LE   ? (1u << 19)
                        : format == AUDIO_S16LE ? (1u << 17)
                                                : (1u << 16);
    uint16_t bits = format == AUDIO_S32LE   ? HDA_FMT_BITS_24
                    : format == AUDIO_S16LE ? HDA_FMT_BITS_16
                                            : HDA_FMT_BITS_8;

    if ((format != AUDIO_U8 && format != AUDIO_S16LE && format != AUDIO_S32LE) || channels < 1 ||
        channels > 2 || !(pcm & size_bit))
        return 0xFFFF;
    for (i = 0; i < sizeof(hda_rates) / sizeof(hda_rates[0]); i++) {
        if (hda_rates[i].rate == sample_rate && (pcm & (1u << i)))
            return hda_rates[i].format | bits | (uint16_t)(channels - 1);
    }
    return 0xFFFF;
}

/** INTCTL's value: the global interrupt enable, the output stream's bit,
 * and the input stream's when the controller has one. */
static uint32_t hda_intctl(void)
{
    uint32_t v = 0x80000000u | (1u << output_stream_index);

    if (num_input_streams > 0)
        v |= 1u << input_stream_index;
    return v;
}

/**
 * HDA interrupt handler: acknowledge each stream's interrupt and serve it.
 *
 * Returns immediately after shutdown. For each stream INTSTS flags, the
 * input stream and then the output stream, it clears the stream's status
 * register (write-1-to-clear) and services the stream: the input stream
 * hands its finished periods to the reader, the output stream refills the
 * next DMA period.
 */
static void hda_irq_handler(isr_params_t *params)
{
    uint32_t intsts;
    uint8_t sd_sts;
    (void)params;

    if (!hda_base) {
        return;
    }

    intsts = hda_read32(HDA_INTSTS);
    if (num_input_streams > 0 && (intsts & (1u << input_stream_index))) {
        sd_sts = hda_read8(hda_sd_offset(input_stream_index) + HDA_SD_STS);
        hda_write8(hda_sd_offset(input_stream_index) + HDA_SD_STS, sd_sts);
        hda_service_capture();
    }
    if (!(intsts & (1 << output_stream_index))) {
        return;
    }

    sd_sts = hda_read8(hda_sd_offset(output_stream_index) + HDA_SD_STS);
    hda_write8(hda_sd_offset(output_stream_index) + HDA_SD_STS, sd_sts);

    irq_count++;

    if (!playing || !stream_started) {
        return;
    }

    hda_service_stream();
}

/**
 * Report whether a PCI vendor/device ID is an HDMI audio controller.
 *
 * Init prefers the PCH analog controller and skips HDMI controllers when both
 * are present. Matches the Intel controllers that serve only the display,
 * 0x0a0c, 0x0c0c and 0x0d0c (Haswell) and 0x160c (Broadwell), and treats all
 * Nvidia (0x10de) and AMD (0x1002) GPU audio functions as HDMI. The ICH8
 * (0x284b) and ICH9 (0x293e) controllers, and the Skylake and later platform
 * controllers such as Gemini Lake's 0x3198, drive the analog codec, so none of
 * them is listed.
 */
static bool is_hdmi_controller(uint16_t vendor_id, uint16_t device_id)
{
    if (vendor_id == 0x8086) { /* Intel */
        if (device_id == 0x0a0c || device_id == 0x0c0c || device_id == 0x0d0c ||
            device_id == 0x160c) {
            return true;
        }
    }
    if (vendor_id == 0x10de || vendor_id == 0x1002) {
        return true;
    }
    return false;
}

/**
 * Register the handler on a message vector of the controller's own and
 * enable the output stream's interrupt there, when the machine has an APIC
 * and the controller a capability for it. A controller whose messages will
 * not enable gives the vector back, handler and all, and `msi_vector` stays
 * -1.
 */
static void hda_msi_start(void)
{
    if (!isr_msi_ready() || (msi_vector = isr_alloc_vector()) < 0) {
        return;
    }
    register_isr_handler((uint8_t)msi_vector, hda_irq_handler, "hda");
    if (pci_msi_enable(&hda_pci, (uint8_t)msi_vector) != 0) {
        isr_free_vector(msi_vector);
        msi_vector = -1;
        return;
    }
    hda_write32(HDA_INTCTL, hda_intctl());
    klog(KLOG_INFO, "audio", "interrupts: messages on vector %d", msi_vector);
}

/**
 * Choose the controller to drive into hda_pci: the first PCI audio function
 * that is not an HDMI controller, else the first audio function. Log every
 * other audio function as not driven, since this kernel drives one, and a
 * person looking at a machine with two needs to see that the other was
 * found and passed over rather than missed. Return false when the machine
 * has no audio function.
 */
static bool hda_find_controller(void)
{
    pci_device_t *skip_dev = NULL;
    pci_device_t temp_pci;
    bool found_controller = false;
    pci_device_t other;
    pci_device_t seen;
    pci_device_t *from = 0;

    while (pci_find_device_skip(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO, skip_dev, &temp_pci)) {
        if (is_hdmi_controller(temp_pci.vendor_id, temp_pci.device_id)) {
            hda_pci = temp_pci;
            skip_dev = &hda_pci;
            continue;
        }

        hda_pci = temp_pci;
        found_controller = true;
        break;
    }

    if (!found_controller) {
        if (!pci_find_device(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO, &hda_pci)) {
            return false;
        }
    }

    while (pci_find_device_skip(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO, from, &other)) {
        if (other.bus != hda_pci.bus || other.device != hda_pci.device ||
            other.function != hda_pci.function) {
            klog(KLOG_INFO, "audio", "not driven: %x:%x at %02x:%02x.%d", (int)other.vendor_id,
                 (int)other.device_id, (int)other.bus, (int)other.device, (int)other.function);
        }
        seen = other;
        from = &seen;
    }
    return true;
}

/**
 * Find, power on, reset, and enumerate the HDA controller.
 *
 * Chooses the controller (hda_find_controller), maps its BAR0 MMIO,
 * enables bus mastering and forces D0, resets the controller, reads the
 * stream counts from GCAP, brings up CORB/RIRB and the DMA position buffer,
 * discovers the output path, and registers the IRQ handler. Returns 0 on
 * success, -1 on any failure.
 *
 * The handler is registered on the controller's legacy IRQ line, when it is
 * below 16, and also on a message vector where the machine has an APIC and
 * the controller a capability for it (`hda_msi_start`). The timer tick
 * services the stream as well, so a machine whose messages do not arrive
 * still plays, and the handler first reads INTSTS for the stream's bit, so
 * being called twice for one event costs a register read.
 */
static int hda_init(void)
{
    uint16_t gcap;
    uint64_t bar0;
    uintptr_t mapped;

    k_strncpy(hda_boot_status, "Intel HD Audio", sizeof(hda_boot_status) - 1);
    hda_boot_status[sizeof(hda_boot_status) - 1] = '\0';

    if (!hda_find_controller()) {
        return -1;
    }

    bar0 = pci_bar_address64(&hda_pci, 0);
    if (bar0 == 0 || (uint64_t)(uintptr_t)bar0 != bar0) {
        return -1;
    }

    mapped = map_mmio((uintptr_t)bar0, HDA_MMIO_SIZE);
    if (mapped == 0) {
        return -1;
    }

    hda_base = mapped;
    hda_subsystem =
        ((uint32_t)pci_read_config16(hda_pci.bus, hda_pci.device, hda_pci.function,
                                     PCI_SUBSYSTEM_VENDOR_ID)
         << 16) |
        pci_read_config16(hda_pci.bus, hda_pci.device, hda_pci.function, PCI_SUBSYSTEM_ID);

    pci_enable_bus_master(&hda_pci);
    pci_enable_memory_space(&hda_pci);
    pci_enable_interrupts(&hda_pci);

    /* Real hardware sometimes boots the controller in D3hot. */
    hda_pci_force_d0(&hda_pci);

    if (!hda_reset()) {
        return -1;
    }

    gcap = hda_read16(HDA_GCAP);
    num_output_streams = (gcap >> 12) & 0xF;
    num_input_streams = (gcap >> 8) & 0xF;

    /* Output streams come after input streams in the descriptor array; the
     * input stream is the first of them. */
    output_stream_index = num_input_streams;
    input_stream_index = 0;

    if (num_output_streams == 0) {
        return -1;
    }

    hda_setup_corb();
    hda_setup_rirb();
    hda_setup_dma_posbuf();

    /* Let the codec settle before issuing commands. */
    hda_delay(50000);

    if (!hda_find_output()) {
        return -1;
    }
    if (adc_nid)
        hda_input_ready();

    if (hda_pci.irq > 0 && hda_pci.irq < 16) {
        register_isr_handler((uint8_t)IRQ_VECTOR(hda_pci.irq), hda_irq_handler, "hda");

        hda_write32(HDA_INTCTL, hda_intctl());
    }

    hda_msi_start();
    hda_build_boot_status();
    return 0;
}

/**
 * Stop playback, disable interrupts, and reset the controller.
 * A queued interrupt after shutdown does not access the controller.
 */
static void hda_shutdown(void)
{
    if (!hda_base) {
        return;
    }

    hda_halt();
    if (cap_stream) {
        hda_capture_stop(cap_stream);
    }

    hda_write32(HDA_INTCTL, 0); /* disable all interrupts */
    hda_write32(HDA_GCTL, 0);   /* assert controller reset */

    hda_base = 0;
}

/** Print on the console, the screen and the serial port, and not to the
 * kernel log: the dumps a person asks for with the `audio` shell command. */
static void hda_log(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    k_vprintf(console_putc, format, ap);
    va_end(ap);
}

/** The name of widget type `type` in the dumps. */
static const char *hda_widget_type_name(uint8_t type)
{
    switch (type) {
    case HDA_WIDGET_AUDIO_OUT:
        return "DAC";
    case HDA_WIDGET_AUDIO_IN:
        return "ADC";
    case HDA_WIDGET_AUDIO_MIX:
        return "Mixer";
    case HDA_WIDGET_AUDIO_SEL:
        return "Selector";
    case HDA_WIDGET_PIN:
        return "Pin";
    case HDA_WIDGET_POWER:
        return "Power";
    case HDA_WIDGET_VOLUME:
        return "Volume";
    case HDA_WIDGET_BEEP:
        return "Beep";
    default:
        return "Unknown";
    }
}

/** The name of a pin's default device `dev` in the dumps. */
static const char *hda_pin_def_dev_name(uint8_t dev)
{
    switch (dev) {
    case HDA_DEF_DEV_LINE_OUT:
        return "LineOut";
    case HDA_DEF_DEV_SPEAKER:
        return "Speaker";
    case HDA_DEF_DEV_HP_OUT:
        return "HP";
    case HDA_DEF_DEV_SPDIF_OUT:
        return "SPDIF";
    case HDA_DEF_DEV_DIG_OUT:
        return "DigOut";
    case HDA_DEF_DEV_LINE_IN:
        return "LineIn";
    case HDA_DEF_DEV_MIC_IN:
        return "Mic";
    default:
        return "Other";
    }
}

/** Print node `nid`'s PCM rates, sample sizes and stream formats. */
static void hda_dump_pcm_caps(uint8_t nid)
{
    uint32_t pcm = hda_get_param(nid, 0x0A);
    uint32_t fmt = hda_get_param(nid, 0x0B);

    hda_log("    PCM caps: 0x%x\n", pcm);
    hda_log("      rates:");
    if (pcm & (1u << 0))
        hda_log(" 8k");
    if (pcm & (1u << 1))
        hda_log(" 11k");
    if (pcm & (1u << 2))
        hda_log(" 16k");
    if (pcm & (1u << 3))
        hda_log(" 22k");
    if (pcm & (1u << 4))
        hda_log(" 32k");
    if (pcm & (1u << 5))
        hda_log(" 44k");
    if (pcm & (1u << 6))
        hda_log(" 48k");
    if (pcm & (1u << 7))
        hda_log(" 88k");
    if (pcm & (1u << 8))
        hda_log(" 96k");
    if (pcm & (1u << 9))
        hda_log(" 176k");
    if (pcm & (1u << 10))
        hda_log(" 192k");
    if (pcm & (1u << 11))
        hda_log(" 384k");
    hda_log("\n");
    hda_log("      bits:");
    if (pcm & (1u << 16))
        hda_log(" 8");
    if (pcm & (1u << 17))
        hda_log(" 16");
    if (pcm & (1u << 18))
        hda_log(" 20");
    if (pcm & (1u << 19))
        hda_log(" 24");
    if (pcm & (1u << 20))
        hda_log(" 32");
    hda_log("\n");
    hda_log("    Stream formats: 0x%x", fmt);
    if (fmt & 0x1)
        hda_log(" PCM");
    if (fmt & 0x2)
        hda_log(" FLOAT32");
    if (fmt & 0x4)
        hda_log(" AC3");
    hda_log("\n");
}

/** The gain steps of an amplifier with capabilities `amp_caps`. */
static uint8_t hda_amp_steps(uint32_t amp_caps)
{
    /* Amp caps: bits 14:8 = num steps (0..0x7f). */
    return (uint8_t)((amp_caps >> 8) & 0x7F);
}

/** The gain steps of node `nid`'s output amplifier. */
static uint8_t hda_get_out_amp_max(uint8_t nid)
{
    return hda_amp_steps(hda_get_param(nid, HDA_PARAM_OUTPUT_AMP));
}

/** The gain steps of node `nid`'s input amplifier. */
static uint8_t hda_get_in_amp_max(uint8_t nid)
{
    return hda_amp_steps(hda_get_param(nid, 0x0D)); /* Input amp caps */
}

/** `gain` limited to `max`, or `gain` itself when `max` is 0. */
static uint8_t hda_clamp_gain(uint8_t gain, uint8_t max)
{
    if (max != 0 && gain > max) {
        return max;
    }
    return gain;
}

/** Choose the closest supported rate at or below the request, or the lowest. */
static uint32_t hda_select_hw_rate(uint32_t req_rate)
{
    uint32_t pcm = hda_common_pcm();
    uint32_t selected = 0;
    unsigned i;

    for (i = 0; i < sizeof(hda_rates) / sizeof(hda_rates[0]); i++) {
        if (pcm & (1u << i)) {
            if (!selected || hda_rates[i].rate <= req_rate)
                selected = hda_rates[i].rate;
        }
    }
    return selected;
}

/** Compute a 16.16 ratio without overflowing a 32-bit shift or using libgcc. */
static uint32_t hda_rate_step(uint32_t input, uint32_t output)
{
    uint32_t result = input / output;
    uint32_t remainder = input % output;
    int i;

    for (i = 0; i < 16; i++) {
        remainder *= 2;
        result *= 2;
        if (remainder >= output) {
            result++;
            remainder -= output;
        }
    }
    return result ? result : 1;
}

void hda_debug_dump(void)
{
    uint16_t gcap;
    uint32_t gctl;
    uint16_t statests;
    uint32_t intctl, intsts;
    int stream;

    hda_log("\nHDA: dump\n");

    if (hda_pci.vendor_id != 0) {
        uint16_t cmd, status;
        uint16_t subven, subdev;
        uint8_t int_line, int_pin;
        uint8_t cap_ptr;

        cmd = pci_read_config16(hda_pci.bus, hda_pci.device, hda_pci.function, PCI_COMMAND);
        status = pci_read_config16(hda_pci.bus, hda_pci.device, hda_pci.function, PCI_STATUS);
        subven = pci_read_config16(hda_pci.bus, hda_pci.device, hda_pci.function,
                                   PCI_SUBSYSTEM_VENDOR_ID);
        subdev = pci_read_config16(hda_pci.bus, hda_pci.device, hda_pci.function, PCI_SUBSYSTEM_ID);
        int_line =
            pci_read_config8(hda_pci.bus, hda_pci.device, hda_pci.function, PCI_INTERRUPT_LINE);
        int_pin =
            pci_read_config8(hda_pci.bus, hda_pci.device, hda_pci.function, PCI_INTERRUPT_PIN);

        hda_log("  PCI: %x:%x.%d vid:did=%x:%x class=%x/%x prog=%x rev=%x\n", hda_pci.bus,
                hda_pci.device, hda_pci.function, hda_pci.vendor_id, hda_pci.device_id,
                hda_pci.class_code, hda_pci.subclass, hda_pci.prog_if, hda_pci.revision);
        hda_log("  PCI: irq line=%d pin=%d cmd=0x%x (io=%d mem=%d bm=%d intdis=%d) sts=0x%x\n",
                int_line, int_pin, cmd, (cmd & PCI_CMD_IO_SPACE) ? 1 : 0,
                (cmd & PCI_CMD_MEMORY_SPACE) ? 1 : 0, (cmd & PCI_CMD_BUS_MASTER) ? 1 : 0,
                (cmd & PCI_CMD_INTERRUPT_DIS) ? 1 : 0, status);
        hda_log("  PCI: ssid=%x:%x\n", subven, subdev);
        hda_log("  PCI: BAR0=0x%x BAR1=0x%x BAR2=0x%x BAR3=0x%x BAR4=0x%x BAR5=0x%x\n",
                hda_pci.bar[0], hda_pci.bar[1], hda_pci.bar[2], hda_pci.bar[3], hda_pci.bar[4],
                hda_pci.bar[5]);

        if (status & PCI_STATUS_CAP_LIST) {
            cap_ptr = pci_read_config8(hda_pci.bus, hda_pci.device, hda_pci.function, PCI_CAP_PTR);
            hda_log("  PCI: cap ptr=0x%x\n", cap_ptr);

            {
                uint8_t off;

                off = pci_find_capability(hda_pci.bus, hda_pci.device, hda_pci.function,
                                          PCI_CAP_ID_PM);
                if (off) {
                    uint16_t pmcsr = pci_read_config16(hda_pci.bus, hda_pci.device,
                                                       hda_pci.function, (uint8_t)(off + 4));
                    hda_log("    PM:  cap@0x%x PMCSR=0x%x state=D%d\n", off, pmcsr,
                            (int)(pmcsr & 0x3));
                }

                off = pci_find_capability(hda_pci.bus, hda_pci.device, hda_pci.function,
                                          PCI_CAP_ID_MSI);
                if (off) {
                    uint16_t msi_ctl = pci_read_config16(hda_pci.bus, hda_pci.device,
                                                         hda_pci.function, (uint8_t)(off + 2));
                    hda_log("    MSI: cap@0x%x ctl=0x%x enabled=%d 64=%d\n", off, msi_ctl,
                            (msi_ctl & 0x1) ? 1 : 0, (msi_ctl & 0x80) ? 1 : 0);
                }

                off = pci_find_capability(hda_pci.bus, hda_pci.device, hda_pci.function,
                                          PCI_CAP_ID_PCIE);
                if (off) {
                    uint16_t pcie_flags = pci_read_config16(hda_pci.bus, hda_pci.device,
                                                            hda_pci.function, (uint8_t)(off + 2));
                    hda_log("    PCIe: cap@0x%x flags=0x%x\n", off, pcie_flags);
                }
            }
        } else {
            hda_log("  PCI: no capability list\n");
        }
    }

    if (!hda_base) {
        hda_log("  (not initialized)\n");
        return;
    }

    gcap = hda_read16(HDA_GCAP);
    gctl = hda_read32(HDA_GCTL);
    statests = hda_read16(HDA_STATESTS);
    intctl = hda_read32(HDA_INTCTL);
    intsts = hda_read32(HDA_INTSTS);

    hda_log("  MMIO base: 0x%x\n", (uint32_t)hda_base);
    hda_log("  GCAP: 0x%x\n", gcap);
    hda_log("  GCTL: 0x%x\n", gctl);
    hda_log("  STATESTS: 0x%x\n", statests);
    hda_log("  INTCTL: 0x%x\n", intctl);
    hda_log("  INTSTS: 0x%x\n", intsts);
    hda_log("  CORB: ctl=0x%x size=0x%x wp=0x%x rp=0x%x base=0x%x\n", hda_read8(HDA_CORBCTL),
            hda_read8(HDA_CORBSIZE), hda_read16(HDA_CORBWP), hda_read16(HDA_CORBRP),
            hda_read32(HDA_CORBLBASE));
    hda_log("  RIRB: ctl=0x%x size=0x%x wp=0x%x base=0x%x\n", hda_read8(HDA_RIRBCTL),
            hda_read8(HDA_RIRBSIZE), hda_read16(HDA_RIRBWP), hda_read32(HDA_RIRBLBASE));

    hda_log("  Codec: addr=%d vendor=0x%x afg=0x%x\n", codec_addr, codec_vendor_id, audio_fg_nid);
    hda_log("  Path: pin=0x%x (idx=%d) dac=0x%x mixer=0x%x hp_pin=0x%x (idx=%d) hp_dac=0x%x\n",
            pin_nid, pin_conn_sel_idx, dac_nid, mixer_nid, hp_pin_nid, hp_conn_sel_idx, hp_dac_nid);
    hda_log("  Out mode: %d\n", (int)hda_output_mode);
    if (pin_nid != 0) {
        uint32_t sense = hda_send_verb(codec_addr, pin_nid, HDA_VERB_GET_PIN_SENSE);
        uint8_t ctl = (uint8_t)(hda_send_verb(codec_addr, pin_nid, 0xF0700) &
                                0xFF); /* GET_PIN_WIDGET_CONTROL */
        uint8_t eapd =
            (uint8_t)(hda_send_verb(codec_addr, pin_nid, 0xF0C00) & 0xFF); /* GET_EAPD_BTLENABLE */
        hda_log("  Out pin 0x%x: ctl=0x%x eapd=0x%x sense=0x%x\n", pin_nid, ctl, eapd, sense);
    }
    if (hp_pin_nid != 0 && hp_pin_nid != pin_nid) {
        uint32_t sense = hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_GET_PIN_SENSE);
        uint8_t ctl = (uint8_t)(hda_send_verb(codec_addr, hp_pin_nid, 0xF0700) &
                                0xFF); /* GET_PIN_WIDGET_CONTROL */
        uint8_t eapd = (uint8_t)(hda_send_verb(codec_addr, hp_pin_nid, 0xF0C00) &
                                 0xFF); /* GET_EAPD_BTLENABLE */
        hda_log("  HP  pin 0x%x: ctl=0x%x eapd=0x%x sense=0x%x\n", hp_pin_nid, ctl, eapd, sense);
    }
    hda_log("  State: playing=%d stream_started=%d irq=%d underruns=%d (%d samples)\n",
            playing ? 1 : 0, stream_started ? 1 : 0, irq_count, underrun_count, underrun_samples);
    hda_log("  DMA: fills=%d wraps=%d lpib_last=%d\n", hw_fill_count, lpib_wraps, lpib_last);
    hda_log("  Audio: req=%dHz fmt=%d ch=%d hw=%dHz fmt=%d ch=%d resample=%d step=0x%x phase=%d "
            "written=%d\n",
            current_sample_rate, (int)current_format, current_channels, current_hw_sample_rate,
            (int)current_hw_format, current_hw_channels, resample_enabled ? 1 : 0, resample_step_fp,
            hda_rs.phase, total_bytes_written);
    {
        uint32_t wal_now = hda_read32(HDA_WALCLK);
        uint32_t end = playing ? wal_now : walclk_end;
        uint32_t delta = (end >= walclk_start) ? (end - walclk_start)
                                               : (end + (0xFFFFFFFFu - walclk_start) + 1u);
        uint32_t ms = delta / 24000u; /* HDA wall clock is 24MHz */
        uint32_t bps = 0;
        if (ms != 0) {
            bps = (session_bytes_written / ms) * 1000;
        }
        hda_log("  Session: %dms written=%d drop=%d ring_hi=%d bps=%d\n", ms, session_bytes_written,
                session_bytes_dropped, ring_high_water, bps);
    }
    hda_log("  Render: out_s16 min=%d max=%d\n", (int)out_min_s16, (int)out_max_s16);
    hda_log("  Ring: avail=%d free=%d\n", ring_available(), ring_free());

    stream = (int)output_stream_index;
    hda_log("  Stream index: %d (tag=%d)\n", stream, stream_tag);
    hda_log("    CTL=0x%x STS=0x%x FMT=0x%x\n", hda_sd_read32(stream, HDA_SD_CTL),
            (uint32_t)hda_read8(hda_sd_offset(stream) + HDA_SD_STS),
            (uint32_t)hda_read16(hda_sd_offset(stream) + HDA_SD_FORMAT));
    hda_log("    CBL=%d LVI=%d LPIB=%d\n", hda_sd_read32(stream, HDA_SD_CBL),
            (uint32_t)hda_read16(hda_sd_offset(stream) + HDA_SD_LVI),
            hda_sd_read32(stream, HDA_SD_LPIB));
    hda_log("    BDL: BDPL=0x%x BDPU=0x%x\n", hda_sd_read32(stream, HDA_SD_BDPL),
            hda_sd_read32(stream, HDA_SD_BDPU));
}

void hda_debug_dump_codec(void)
{
    uint8_t j;

    hda_log("\nHDA: codec dump\n");

    if (!hda_base || !audio_fg_nid) {
        hda_log("  (not ready)\n");
        return;
    }

    hda_log("  Codec %d vendor=0x%x\n", codec_addr, codec_vendor_id);
    hda_log("  AFG 0x%x widgets [%x..%x] (%d)\n", audio_fg_nid, afg_widget_start,
            (uint8_t)(afg_widget_start + (afg_widget_count ? (afg_widget_count - 1) : 0)),
            afg_widget_count);

    if (dac_nid) {
        uint32_t wcap = hda_get_param(dac_nid, HDA_PARAM_AUDIO_CAPS);
        hda_log("  DAC 0x%x wcap=0x%x\n", dac_nid, wcap);
        hda_dump_pcm_caps(dac_nid);
        hda_log("    AMP out caps: 0x%x\n", hda_get_param(dac_nid, HDA_PARAM_OUTPUT_AMP));
    }
    if (mixer_nid) {
        uint32_t wcap = hda_get_param(mixer_nid, HDA_PARAM_AUDIO_CAPS);
        uint8_t sel = (uint8_t)(hda_send_verb(codec_addr, mixer_nid, HDA_VERB_GET_CONN_SEL) & 0xFF);
        hda_log("  MIX 0x%x wcap=0x%x type=%s sel=%d\n", mixer_nid, wcap,
                hda_widget_type_name((wcap >> 20) & 0xF), sel);
        hda_log("    AMP in caps:  0x%x\n", hda_get_param(mixer_nid, 0x0D));
        hda_log("    AMP out caps: 0x%x\n", hda_get_param(mixer_nid, HDA_PARAM_OUTPUT_AMP));
    }

    for (j = 0; j < afg_widget_count; j++) {
        uint8_t nid = (uint8_t)(afg_widget_start + j);
        uint32_t wcap = hda_get_param(nid, HDA_PARAM_AUDIO_CAPS);
        uint8_t type = (wcap >> 20) & 0xF;

        if (type != HDA_WIDGET_PIN) {
            continue;
        }

        {
            uint32_t pin_caps = hda_get_param(nid, HDA_PARAM_PIN_CAPS);
            uint32_t cfg = hda_send_verb(codec_addr, nid, HDA_VERB_GET_CONFIG_DEF);
            uint8_t dev = HDA_PIN_DEF_DEV(cfg);
            uint8_t port = HDA_PIN_DEF_PORT(cfg);
            uint8_t conn_sel =
                (uint8_t)(hda_send_verb(codec_addr, nid, HDA_VERB_GET_CONN_SEL) & 0xFF);
            uint32_t conn_param = hda_get_param(nid, HDA_PARAM_CONN_LIST);
            uint8_t pin_ctl = (uint8_t)(hda_send_verb(codec_addr, nid, 0xF0700) &
                                        0xFF); /* GET_PIN_WIDGET_CONTROL */
            uint8_t eapd =
                (uint8_t)(hda_send_verb(codec_addr, nid, 0xF0C00) & 0xFF); /* GET_EAPD_BTLENABLE */
            uint16_t conn_list[HDA_MAX_CONNECTIONS];
            int conn_num;
            int i;

            hda_log("  Pin 0x%x wcap=0x%x caps=0x%x cfg=0x%x dev=%s port=%d ctl=0x%x eapd=0x%x\n",
                    nid, wcap, pin_caps, cfg, hda_pin_def_dev_name(dev), port, pin_ctl, eapd);

            hda_log("    Conn: len=%d long=%d sel=%d\n", (int)(conn_param & 0x7F),
                    (int)((conn_param >> 7) & 0x1), (int)conn_sel);

            conn_num = hda_get_connections(nid, conn_list, HDA_MAX_CONNECTIONS);
            if (conn_num > 0) {
                hda_log("    List:");
                for (i = 0; i < conn_num; i++) {
                    if ((uint8_t)i == conn_sel) {
                        hda_log(" [%x]", conn_list[i]);
                    } else {
                        hda_log(" %x", conn_list[i]);
                    }
                }
                hda_log("\n");
            }
        }
    }
}

void hda_debug_dump_pins(void)
{
    uint32_t out_sense = 0;
    uint32_t hp_sense = 0;
    uint8_t out_ctl = 0;
    uint8_t out_eapd = 0;
    uint8_t hp_ctl = 0;
    uint8_t hp_eapd = 0;
    uint8_t out_sel = 0;
    uint8_t hp_sel = 0;
    uint16_t conn_list[HDA_MAX_CONNECTIONS];
    int conn_num;

    hda_log("\nHDA: pins\n");
    if (!hda_base || !audio_fg_nid) {
        hda_log("  (not ready)\n");
        return;
    }

    hda_log("  mode=%d (0=auto 1=primary 2=hp 3=both)\n", (int)hda_output_mode);
    hda_log("  dma: fills=%d wraps=%d lpib_last=%d\n", hw_fill_count, lpib_wraps, lpib_last);
    hda_log("  path: out_pin=0x%x dac=0x%x hp_pin=0x%x hp_dac=0x%x\n", pin_nid, dac_nid, hp_pin_nid,
            hp_dac_nid);

    if (pin_nid != 0) {
        out_sense = hda_send_verb(codec_addr, pin_nid, HDA_VERB_GET_PIN_SENSE);
        out_ctl = (uint8_t)(hda_send_verb(codec_addr, pin_nid, 0xF0700) &
                            0xFF); /* GET_PIN_WIDGET_CONTROL */
        out_eapd =
            (uint8_t)(hda_send_verb(codec_addr, pin_nid, 0xF0C00) & 0xFF); /* GET_EAPD_BTLENABLE */
        out_sel = (uint8_t)(hda_send_verb(codec_addr, pin_nid, HDA_VERB_GET_CONN_SEL) & 0xFF);
        conn_num = hda_get_connections(pin_nid, conn_list, HDA_MAX_CONNECTIONS);
        hda_log("  out 0x%x: sel=%d ctl=0x%x eapd=0x%x sense=0x%x present=%d\n", pin_nid,
                (int)out_sel, (uint32_t)out_ctl, (uint32_t)out_eapd, out_sense,
                (out_sense & 0x80000000u) ? 1 : 0);
        if (conn_num > 0 && out_sel < (uint8_t)conn_num) {
            hda_log("    sel_nid=0x%x (of %d)\n", conn_list[out_sel], conn_num);
        }
    }

    if (hp_pin_nid != 0 && hp_pin_nid != pin_nid) {
        hp_sense = hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_GET_PIN_SENSE);
        hp_ctl = (uint8_t)(hda_send_verb(codec_addr, hp_pin_nid, 0xF0700) &
                           0xFF); /* GET_PIN_WIDGET_CONTROL */
        hp_eapd = (uint8_t)(hda_send_verb(codec_addr, hp_pin_nid, 0xF0C00) &
                            0xFF); /* GET_EAPD_BTLENABLE */
        hp_sel = (uint8_t)(hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_GET_CONN_SEL) & 0xFF);
        conn_num = hda_get_connections(hp_pin_nid, conn_list, HDA_MAX_CONNECTIONS);
        hda_log("  hp  0x%x: sel=%d ctl=0x%x eapd=0x%x sense=0x%x present=%d\n", hp_pin_nid,
                (int)hp_sel, (uint32_t)hp_ctl, (uint32_t)hp_eapd, hp_sense,
                (hp_sense & 0x80000000u) ? 1 : 0);
        if (conn_num > 0 && hp_sel < (uint8_t)conn_num) {
            hda_log("    sel_nid=0x%x (of %d)\n", conn_list[hp_sel], conn_num);
        }
    }
}

void hda_debug_dump_stats(void)
{
    uint32_t wal_now;
    uint32_t end;
    uint32_t delta;
    uint32_t ms;
    uint32_t bps;
    uint32_t lpib;
    uint32_t pos = 0;
    uint32_t cbl;
    uint32_t ctl;
    uint8_t sts;
    uint16_t fmt;
    uint32_t idx;

    hda_log("\nHDA: stats\n");
    if (!hda_base) {
        hda_log("  (not initialized)\n");
        return;
    }

    idx = (uint32_t)output_stream_index * 2u;
    if (dma_posbuf_enabled && idx < (uint32_t)(HDA_MAX_STREAMS * 2)) {
        pos = dma_posbuf[idx];
    }

    lpib = hda_sd_read32(output_stream_index, HDA_SD_LPIB);
    cbl = hda_sd_read32(output_stream_index, HDA_SD_CBL);
    ctl = hda_sd_read32(output_stream_index, HDA_SD_CTL);
    sts = hda_read8(hda_sd_offset(output_stream_index) + HDA_SD_STS);
    fmt = hda_read16(hda_sd_offset(output_stream_index) + HDA_SD_FORMAT);

    wal_now = hda_read32(HDA_WALCLK);
    end = playing ? wal_now : walclk_end;
    delta =
        (end >= walclk_start) ? (end - walclk_start) : (end + (0xFFFFFFFFu - walclk_start) + 1u);
    ms = delta / 24000u; /* HDA wall clock is 24MHz */
    bps = 0;
    if (ms != 0) {
        bps = (session_bytes_written / ms) * 1000u;
    }

    hda_log("  posbuf: enabled=%d use=%d pos=%d lpib=%d cbl=%d\n", dma_posbuf_enabled ? 1 : 0,
            dma_posbuf_use ? 1 : 0, pos, lpib, cbl);
    if (dma_checked) {
        hda_log("  check: lpib %d->%d sts 0x%x->0x%x pos %d->%d%s\n", dma_check_lpib[0],
                dma_check_lpib[1], (uint32_t)dma_check_sts[0], (uint32_t)dma_check_sts[1],
                dma_check_pos[0], dma_check_pos[1],
                dma_check_lpib[1] == dma_check_lpib[0] ? " (lpib did not move)" : "");
    } else {
        hda_log("  check: none since setup\n");
    }
    hda_log("  state: playing=%d stream_started=%d irq=%d underruns=%d (%d samples)\n",
            playing ? 1 : 0, stream_started ? 1 : 0, irq_count, underrun_count, underrun_samples);
    hda_log("  dma: fills=%d wraps=%d lpib_last=%d\n", hw_fill_count, lpib_wraps, lpib_last);
    hda_log("  session: %dms written=%d drop=%d ring_hi=%d bps=%d\n", ms, session_bytes_written,
            session_bytes_dropped, ring_high_water, bps);
    hda_log("  render: out_s16 min=%d max=%d\n", (int)out_min_s16, (int)out_max_s16);
    hda_log("  ring: avail=%d free=%d\n", ring_available(), ring_free());
    hda_log("  stream: ctl=0x%x sts=0x%x fmt=0x%x\n", ctl, (uint32_t)sts, (uint32_t)fmt);
}

bool hda_debug_get_stream_snapshot(hda_stream_debug_snapshot_t *out)
{
    if (!hda_base || !out) {
        return false;
    }

    out->lpib = hda_sd_read32(output_stream_index, HDA_SD_LPIB);
    out->cbl = hda_sd_read32(output_stream_index, HDA_SD_CBL);
    out->ctl = hda_sd_read32(output_stream_index, HDA_SD_CTL);
    out->sts = hda_read8(hda_sd_offset(output_stream_index) + HDA_SD_STS);
    out->fmt = hda_read16(hda_sd_offset(output_stream_index) + HDA_SD_FORMAT);
    return true;
}

/** Fill a buffer with silence: 0 for signed samples, 128 for unsigned U8. */
static void fill_silence(volatile uint8_t *buffer, size_t size, bool is_signed)
{
    size_t i;
    if (is_signed) {
        for (i = 0; i < size; i++) {
            buffer[i] = 0;
        }
    } else {
        for (i = 0; i < size; i++) {
            buffer[i] = 128;
        }
    }
}

/**
 * Read the running stream's position twice, 20 ms apart, and choose its source.
 *
 * When LPIB moved and the DMA position buffer's entry did not, the buffer is
 * not used for the stream's position; otherwise it is, if the controller took
 * it. The readings are kept for `audio hda stats` and `dma_checked` is set.
 * The wait reads the wall clock, so it works with interrupts off.
 */
static void hda_check_dma(int stream)
{
    uint32_t idx = (uint32_t)stream * 2u;
    bool have_pos = dma_posbuf_enabled && idx < (uint32_t)(HDA_MAX_STREAMS * 2);
    int i;

    for (i = 0; i < 2; i++) {
        if (i == 1) {
            hda_delay_walclk_us(20000);
        }
        dma_check_lpib[i] = hda_sd_read32(stream, HDA_SD_LPIB);
        dma_check_sts[i] = hda_read8(hda_sd_offset(stream) + HDA_SD_STS);
        dma_check_pos[i] = 0;
        if (have_pos) {
            hda_dma_sync_range(&dma_posbuf[idx], (uint32_t)sizeof(uint32_t));
            dma_check_pos[i] = dma_posbuf[idx];
        }
    }

    dma_posbuf_use = have_pos && !(dma_check_lpib[1] != dma_check_lpib[0] &&
                                   dma_check_pos[1] == dma_check_pos[0]);
    dma_checked = true;
}

/*
 * Jack detection. The headphone pin and the microphone jack's pin, each
 * where its caps have presence detect and its configuration does not deny
 * it, are polled from the tick every quarter second (hda_poll_jacks), with
 * verbs that give up when the interface is in use (HDA_VERB_TRY). A reading
 * that differs from a jack's state changes it only when the next reading
 * agrees, so a plug half-way in does not move the sound back and forth; a
 * reading whose verb fails changes nothing. A start reads every pin's sense
 * (hda_sense_pins) and takes the jacks' states from it at once.
 *
 * In the automatic output mode the headphones play, and the primary pin is
 * off, while the headphone jack has a plug (hda_route_output); a change
 * while a stream plays moves the output at the poll that sees it. A change
 * of the microphone jack moves capture between the internal mic and the jack
 * mic (hda_follow_mic), at once when the input stream runs.
 *
 * Code that programs the pins outside the tick holds the jacks for that time
 * (hda_jack_hold), and the tick, which takes jack_lock with a try, polls
 * only when nothing holds them, so that the two never set a pin in turn.
 */
#define HDA_JACK_TICKS (TICK_HZ / 4 > 0 ? TICK_HZ / 4 : 1)

static struct spinlock jack_lock = SPINLOCK_INIT;
static int jack_holds = 0;          /* regions outside the tick that program the pins */
static uint32_t jack_ticks = 0;     /* ticks since the last poll */
static int8_t jack_seen[HDA_JACKS]; /* a reading that differs from the state and
                                       waits for a second, or HDA_JACK_UNKNOWN */
static int8_t route_hp = -1;        /* the headphone pin's output as the last routing
                                       set it: 1 on, 0 off, -1 unset since a stop */
static bool mic_follow = false;     /* the microphone jack changed and capture has
                                       yet to follow it */
static bool mic_program = false;    /* capture moved while its stream runs, and the
                                       codec's route is yet to be programmed */
static uint8_t mic_left = 0;        /* the pin capture moved from, whose input the
                                       route's programming turns off */

/*
 * The beep generator (hda_beep), found at enumeration. With no stream
 * playing the output pins are off, so a beep turns them on as a start does,
 * and the tick turns them off again once the generator has been silent for
 * HDA_BEEP_IDLE_TICKS, so that a run of bells does not switch the pins
 * between bells. The pins and beep_opened change only with the jacks held
 * or under jack_lock.
 */
#define HDA_BEEP_IDLE_TICKS (TICK_HZ > 0 ? TICK_HZ : 1) /* one second */

static uint8_t beep_nid = 0;              /* the beep widget, or 0 */
static uint8_t beep_word = 0;             /* its output amplifier's step and
                                             mute bit, for beep_level */
static int beep_level = 0;                /* the mixer's level, 0 to 100; 0
                                             leaves the generator silent */
static volatile uint8_t beep_div = 0;     /* the divider sounding; 0 silent */
static volatile bool beep_opened = false; /* a beep turned the output pins on
                                             with no stream playing */
static uint32_t beep_quiet = 0;           /* ticks silent since, while opened */

static bool hda_input_program(int vmode, bool live);
static uint8_t hda_source_route(int source, bool take);

/** Count a region outside the tick that programs the codec's pins; the tick
 * polls no jack until hda_jack_release ends it. */
static void hda_jack_hold(void)
{
    uint32_t flags = kirq_save();

    spin_lock(&jack_lock);
    jack_holds++;
    spin_unlock(&jack_lock);
    kirq_restore(flags);
}

/** End a region hda_jack_hold began. */
static void hda_jack_release(void)
{
    uint32_t flags = kirq_save();

    spin_lock(&jack_lock);
    jack_holds--;
    spin_unlock(&jack_lock);
    kirq_restore(flags);
}

/**
 * Send `verb` to `nid` in `vmode` (hda_verb) as one of a sequence, setting
 * `*ok` false when it fails. In HDA_VERB_TRY mode, the tick's, a sequence
 * stops at its first failure, since the interface is in use or the codec
 * does not answer, and the caller sends it again at its next poll; in
 * HDA_VERB_WAIT mode the rest is still sent.
 */
static void hda_put(uint8_t nid, uint32_t verb, int vmode, bool *ok)
{
    if (!*ok && vmode == HDA_VERB_TRY)
        return;
    if (hda_verb(codec_addr, nid, verb, vmode, NULL) != 0)
        *ok = false;
}

/** Whether the pin of report entry `w` can tell a plug: its caps have
 * presence detect (bit 2), its port is a jack, and its configuration's
 * Jack Detect Override (bit 8) is clear. */
static bool hda_jack_capable(const hda_widget_report_t *w)
{
    return w && w->type == HDA_WIDGET_PIN && (w->pin_caps & (1u << 2)) &&
           HDA_PIN_DEF_PORT(w->config) == HDA_PORT_CONN_JACK && !(w->config & (1u << 8));
}

/**
 * Choose the jacks to watch, once hda_find_output and hda_find_input have
 * chosen the pins: the headphone pin when it is not the primary pin and has
 * a DAC, and the jack mic, each when it can tell a plug. Their states start
 * unknown.
 */
static void hda_find_jacks(void)
{
    uint8_t pins[HDA_JACKS];
    int j;

    pins[HDA_JACK_HP] = hp_pin_nid != pin_nid && hp_dac_nid ? hp_pin_nid : 0;
    pins[HDA_JACK_MIC] = jack_mic_nid;
    for (j = 0; j < HDA_JACKS; j++) {
        hda_jack_report_t *k = &hda_report.jacks[j];

        k_memset(k, 0, sizeof(*k));
        k->pin = hda_jack_capable(hda_widget(pins[j])) ? pins[j] : 0;
        k->state = HDA_JACK_UNKNOWN;
        jack_seen[j] = HDA_JACK_UNKNOWN;
    }
    jack_ticks = 0;
    route_hp = -1;
    mic_follow = mic_program = false;
    mic_left = 0;
}

/** Make `state` jack `j`'s, counting a change; a change of the microphone
 * jack is for capture to follow (hda_follow_mic). */
static void hda_jack_set(int j, int8_t state)
{
    hda_jack_report_t *k = &hda_report.jacks[j];

    jack_seen[j] = HDA_JACK_UNKNOWN;
    if (k->state == state)
        return;
    k->state = state;
    k->changes++;
    if (j == HDA_JACK_MIC)
        mic_follow = true;
}

/**
 * Take reading `state` of jack `j`, HDA_JACK_UNKNOWN for one that failed:
 * a failed reading changes nothing, one that agrees with the state clears a
 * reading that waits, and one that differs changes the state when the
 * reading before it, waiting, was the same.
 */
static void hda_jack_reading(int j, int8_t state)
{
    hda_jack_report_t *k = &hda_report.jacks[j];

    k->polls++;
    if (state == HDA_JACK_UNKNOWN)
        k->failed++;
    else if (state == k->state)
        jack_seen[j] = HDA_JACK_UNKNOWN;
    else if (jack_seen[j] == state)
        hda_jack_set(j, state);
    else
        jack_seen[j] = state;
}

/**
 * Turn the output pins on and off for the output mode: the primary pin but
 * in `hp`, and the headphone pin, when there is a headphone path, in `hp`
 * and `both`; in `auto`, `hp` while the headphone jack has a plug and
 * `primary` otherwise. The primary pin's widget control is Out Enable, the
 * headphone pin's Out Enable and HP Enable, and 0 turns a pin off. Each verb
 * in `vmode`; false when one failed, in which case the report and
 * `route_hp` keep what they had.
 */
static bool hda_route_output(int vmode)
{
    bool have_hp = hp_pin_nid != 0 && hp_pin_nid != pin_nid && hp_dac_nid != 0;
    int mode = (int)hda_output_mode;
    bool primary, hp, ok = true;

    if (mode == HDA_OUTMODE_AUTO)
        mode = have_hp && hda_report.jacks[HDA_JACK_HP].state == HDA_JACK_IN ? HDA_OUTMODE_HP
                                                                             : HDA_OUTMODE_PRIMARY;
    primary = mode != HDA_OUTMODE_HP;
    hp = have_hp && (mode == HDA_OUTMODE_HP || mode == HDA_OUTMODE_BOTH);
    if (pin_nid)
        hda_put(pin_nid, HDA_VERB_SET_PIN_CTRL | (primary ? 0x40 : 0x00), vmode, &ok);
    if (have_hp)
        hda_put(hp_pin_nid, HDA_VERB_SET_PIN_CTRL | (hp ? 0xC0 : 0x00), vmode, &ok);
    if (!ok)
        return false;
    hda_report.primary_enabled = primary;
    hda_report.hp_enabled = hp;
    route_hp = hp ? 1 : 0;
    return true;
}

/**
 * Move capture to the microphone the microphone jack calls for after it
 * changed, the jack mic while it has a plug and the internal mic otherwise,
 * as the mixer's record source does (hda_source_route): the input path's
 * fields at once, and, while the input stream runs, the codec's route
 * (hda_input_program) in verbs of `vmode`, with the input of the pin left
 * turned off. Capture follows only from one microphone to the other, and
 * only to one the ADC reaches; a source the mixer chose stays until the
 * jack next changes. Return false when a verb failed; the route is
 * programmed again at the next call.
 */
static bool hda_follow_mic(int vmode)
{
    if (mic_follow) {
        int want =
            hda_report.jacks[HDA_JACK_MIC].state == HDA_JACK_IN ? AUDIO_SRC_MIC : AUDIO_SRC_INT_MIC;
        uint8_t from = in_pin_nid;

        mic_follow = false;
        if (want != in_src && (in_src == AUDIO_SRC_MIC || in_src == AUDIO_SRC_INT_MIC) &&
            hda_source_route(want, true)) {
            mic_left = from;
            mic_program = true;
        }
    }
    if (!mic_program)
        return true;
    if (!hda_input_program(vmode, cap_running))
        return false;
    mic_program = false;
    mic_left = 0;
    return true;
}

/**
 * Read the sense of jack `j` in a try, triggering it first on a pin whose
 * caps require that (bit 1): HDA_JACK_IN or HDA_JACK_OUT by presence (bit
 * 31), or HDA_JACK_UNKNOWN when a verb failed or found the interface busy.
 */
static int8_t hda_jack_sense(int j)
{
    const hda_widget_report_t *w = hda_widget(hda_report.jacks[j].pin);
    uint32_t sense;

    if ((w->pin_caps & (1u << 1)) &&
        hda_verb(codec_addr, w->nid, HDA_VERB_SET_PIN_SENSE, HDA_VERB_TRY, NULL) != 0)
        return HDA_JACK_UNKNOWN;
    if (hda_verb(codec_addr, w->nid, HDA_VERB_GET_PIN_SENSE, HDA_VERB_TRY, &sense) != 0)
        return HDA_JACK_UNKNOWN;
    return (sense & 0x80000000u) ? HDA_JACK_IN : HDA_JACK_OUT;
}

/**
 * Poll the jacks, from the tick with interrupts off, every HDA_JACK_TICKS
 * ticks: read each one's sense, and then move the output while a stream
 * plays in the automatic mode and the headphones' routing differs from the
 * jack, and capture after a change of the microphone jack. Nothing is done
 * while code outside the tick holds the jacks, and what a failed verb left
 * undone is done again at the next poll. `passed` is the ticks since the
 * last call (hda_tick).
 */
static void hda_poll_jacks(uint32_t passed)
{
    uint32_t flags;
    int j;

    if (!hda_base || !hda_report.ready)
        return;
    jack_ticks += passed;
    if (jack_ticks < HDA_JACK_TICKS)
        return;
    jack_ticks = 0;
    flags = kirq_save();
    if (!spin_trylock(&jack_lock)) {
        kirq_restore(flags);
        return;
    }
    if (jack_holds == 0) {
        for (j = 0; j < HDA_JACKS; j++)
            if (hda_report.jacks[j].pin)
                hda_jack_reading(j, hda_jack_sense(j));
        if ((playing || beep_opened) && hda_output_mode == HDA_OUTMODE_AUTO &&
            hda_report.jacks[HDA_JACK_HP].pin &&
            route_hp != (hda_report.jacks[HDA_JACK_HP].state == HDA_JACK_IN))
            hda_route_output(HDA_VERB_TRY);
        hda_follow_mic(HDA_VERB_TRY);
    }
    spin_unlock(&jack_lock);
    kirq_restore(flags);
}

/** Start the output session: zero its counters and read the wall clock.
 * total_bytes_written goes on. */
static void hda_session_reset(void)
{
    irq_count = 0;
    underrun_count = 0;
    underrun_samples = 0;
    hw_fill_count = 0;
    missed_revolutions = 0;
    session_bytes_written = 0;
    session_bytes_dropped = 0;
    ring_high_water = 0;
    walclk_start = hda_read32(HDA_WALCLK);
    walclk_end = walclk_start;
    out_min_s16 = 32767;
    out_max_s16 = (int16_t)-32768;
}

/**
 * Size the period and the ring for the programmed rate, lay the descriptor
 * list over the DMA buffer with an interrupt at each period, fill the
 * buffer, and program output stream `stream` for format word `fmt` short of
 * running it.
 *
 * The first fill takes from the stream of audio.h when one has the output,
 * else from the callback; the write path's starts silent.
 */
static void hda_out_buffers(int stream, uint16_t fmt)
{
    uint64_t buf_addr;
    int i;

    period_bytes = hda_period_bytes(current_hw_sample_rate, hda_out_frame());
    ring_size = HDA_RING_MAX;
    if (!user_callback && !play_stream) {
        uint32_t frames = hda_frames_in(period_bytes / hda_out_frame(), true);

        if (frames * HDA_RING_PERIODS * hda_in_frame() < HDA_RING_MAX)
            ring_size = frames * HDA_RING_PERIODS * hda_in_frame() + 1u;
    }
    ring_read_ptr = 0;
    ring_write_ptr = 0;

    buf_addr = (uint64_t)virt_to_phys(audio_buffer);
    for (i = 0; i < HDA_PERIODS; i++) {
        bdl[i].addr_low = (uint32_t)(buf_addr + (uint64_t)i * period_bytes);
        bdl[i].addr_high = hda_hi(buf_addr + (uint64_t)i * period_bytes);
        bdl[i].length = period_bytes;
        bdl[i].ioc = 1; /* interrupt on completion */
    }

    if (play_stream) {
        stream_taken = 0;
        for (i = 0; i < HDA_PERIODS; i++) {
            stream_in[i] = hda_play_render((uint8_t *)audio_buffer + i * period_bytes,
                                           period_bytes / hda_out_frame());
        }
    } else if (user_callback) {
        for (i = 0; i < HDA_PERIODS; i++) {
            user_callback((uint8_t *)audio_buffer + i * period_bytes, period_bytes);
        }
    } else {
        /* Write path silence is signed 16-bit zeros. */
        fill_silence((uint8_t *)audio_buffer, period_bytes * HDA_PERIODS, true);
    }

    /* Push initial samples to RAM for DMA (matters on some laptops). */
    hda_dma_sync_range(audio_buffer, period_bytes * HDA_PERIODS);

    hda_sd_write32(stream, HDA_SD_BDPL, (uint32_t)virt_to_phys(bdl));
    hda_sd_write32(stream, HDA_SD_BDPU, hda_hi(virt_to_phys(bdl)));
    hda_sd_write32(stream, HDA_SD_CBL, period_bytes * HDA_PERIODS);
    hda_sd_write16(stream, HDA_SD_LVI, HDA_PERIODS - 1);
    hda_sd_write16(stream, HDA_SD_FORMAT, fmt);

    hda_sd_write32(stream, HDA_SD_CTL, (stream_tag << 20) | HDA_SD_CTL_IOCE);
}

/** The step that sets full gain on an amplifier whose top step is `max`:
 * `max`, or 0x7F when the amplifier reports none. */
static uint8_t hda_full_gain(uint8_t max)
{
    return max != 0 ? max : 0x7F;
}

/**
 * Power DAC `nid`, wait `settle` (hda_delay's count), give it format word
 * `fmt` and the output stream's tag, and set its output amplifier, where it
 * has one, unmuted at full gain.
 */
static void hda_dac_program(uint8_t nid, uint16_t fmt, int settle)
{
    hda_send_verb(codec_addr, nid, HDA_VERB_SET_POWER | 0x00);
    hda_delay(settle);
    hda_send_verb(codec_addr, nid, HDA_VERB_SET_CONV_FMT | fmt);
    hda_send_verb(codec_addr, nid, HDA_VERB_SET_CHANNEL | (stream_tag << 4));
    /* 0xB000: set the output amplifier, both channels. */
    if (hda_get_param(nid, HDA_PARAM_AUDIO_CAPS) & 0x04) /* AC_WCAP_OUT_AMP */
        hda_send_verb(codec_addr, nid,
                      HDA_VERB_SET_AMP_GAIN | 0xB000 | hda_full_gain(hda_get_out_amp_max(nid)));
}

/**
 * Power the output path's mixer or selector and pass the DAC through it at
 * full gain: a mixer has the amplifier of every input unmuted, a selector
 * selects its connection and unmutes that one's, and the output amplifier is
 * unmuted too. Does nothing when the path has none.
 */
static void hda_out_mixer_program(void)
{
    uint32_t mix_wcap;
    uint8_t mix_type;
    uint8_t mix_conn_sel = mixer_conn_sel_idx;
    int i;

    if (mixer_nid == 0)
        return;
    hda_send_verb(codec_addr, mixer_nid, HDA_VERB_SET_POWER | 0x00);
    hda_delay(20);

    mix_wcap = hda_get_param(mixer_nid, HDA_PARAM_AUDIO_CAPS);
    mix_type = (mix_wcap >> 20) & 0xF;

    if (mix_type == HDA_WIDGET_AUDIO_SEL)
        hda_send_verb(codec_addr, mixer_nid, HDA_VERB_SET_CONN_SEL | mix_conn_sel);

    if (mix_wcap & 0x02) { /* AC_WCAP_IN_AMP */
        uint16_t mix_conn[HDA_MAX_CONNECTIONS];
        int mix_num = hda_get_connections(mixer_nid, mix_conn, HDA_MAX_CONNECTIONS);
        int start = (mix_type == HDA_WIDGET_AUDIO_MIX) ? 0 : (int)mix_conn_sel;
        int end = (mix_type == HDA_WIDGET_AUDIO_MIX) ? mix_num : (start + 1);
        uint8_t gain_in = hda_full_gain(hda_get_in_amp_max(mixer_nid));

        /* Amplifier input indices are four bits. */
        for (i = start; i < end && i < 16; i++) {
            hda_send_verb(codec_addr, mixer_nid,
                          HDA_VERB_SET_AMP_GAIN | 0x7000 | ((uint32_t)i << 8) | gain_in);
        }
    }
    if (mix_wcap & 0x04) /* AC_WCAP_OUT_AMP */
        hda_send_verb(codec_addr, mixer_nid,
                      HDA_VERB_SET_AMP_GAIN | 0xB000 |
                          hda_full_gain(hda_get_out_amp_max(mixer_nid)));
}

/**
 * Set the amplifiers of output pin `nid`, whose audio caps are `wcap` and
 * whose connection is `conn_sel`, unmuted at full gain: its input amplifier
 * on that connection, since some codecs mute the output through it, and its
 * output amplifier.
 */
static void hda_out_pin_amps(uint8_t nid, uint32_t wcap, uint8_t conn_sel)
{
    if (wcap & 0x02) /* AC_WCAP_IN_AMP */
        hda_send_verb(codec_addr, nid,
                      HDA_VERB_SET_AMP_GAIN | 0x7000 | ((uint32_t)conn_sel << 8) |
                          hda_full_gain(hda_get_in_amp_max(nid)));
    if (wcap & 0x04) { /* AC_WCAP_OUT_AMP */
        uint32_t amp = 0xB000 | hda_full_gain(hda_get_out_amp_max(nid));

        /* A Conexant codec takes an index on the pin's output amplifier. */
        if (((codec_vendor_id >> 16) & 0xFFFF) == 0x14f1)
            amp |= ((uint32_t)conn_sel << 8);
        hda_send_verb(codec_addr, nid, HDA_VERB_SET_AMP_GAIN | amp);
    }
}

/**
 * Power the output pin and the headphone pin, select their connections, set
 * their amplifiers (hda_out_pin_amps), and turn on their external
 * amplifiers: the speaker pin gets EAPD and BTL, another pin EAPD where it
 * has it, and the headphone pin EAPD. The headphone pin is set up only when
 * it is another pin with a DAC of its own path.
 */
static void hda_out_pins_program(void)
{
    if (pin_nid) {
        uint32_t pin_caps = hda_get_param(pin_nid, HDA_PARAM_PIN_CAPS);

        hda_send_verb(codec_addr, pin_nid, HDA_VERB_SET_POWER | 0x00);
        hda_delay(20);
        hda_send_verb(codec_addr, pin_nid, HDA_VERB_SET_CONN_SEL | pin_conn_sel_idx);
        hda_out_pin_amps(pin_nid, hda_get_param(pin_nid, HDA_PARAM_AUDIO_CAPS), pin_conn_sel_idx);
        if (pin_nid == speaker_pin_nid)
            hda_send_verb(codec_addr, pin_nid, HDA_VERB_SET_EAPD | 0x03);
        else if (pin_caps & 0x10000)
            hda_send_verb(codec_addr, pin_nid, HDA_VERB_SET_EAPD | 0x02);
    }

    if (hp_pin_nid != 0 && hp_pin_nid != pin_nid && hp_dac_nid != 0) {
        uint32_t hp_wcap = hda_get_param(hp_pin_nid, HDA_PARAM_AUDIO_CAPS);

        /* Some codecs sense a jack only on a powered pin. */
        hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_SET_POWER | 0x00);
        hda_delay(5);
        hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_SET_CONN_SEL | hp_conn_sel_idx);
        hda_out_pin_amps(hp_pin_nid, hp_wcap, hp_conn_sel_idx);
        hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_SET_EAPD | 0x02);
    }
}

/** Read every pin's sense (hda_sense_pins) and give each jack its state
 * from it, counting a poll for each jack and a failure for each unsensed. */
static void hda_jacks_sense(void)
{
    int i;

    hda_sense_pins();
    for (i = 0; i < HDA_JACKS; i++) {
        const hda_widget_report_t *w = hda_widget(hda_report.jacks[i].pin);

        if (!w)
            continue;
        hda_report.jacks[i].polls++;
        if (w->sensed)
            hda_jack_set(i, (w->sense & 0x80000000u) ? HDA_JACK_IN : HDA_JACK_OUT);
        else
            hda_report.jacks[i].failed++;
    }
}

/**
 * Program the discovered codec path for format word `fmt`, in order: the
 * function group's power, the DAC, the headphone DAC, the mixer, the pin,
 * the headphone pin, then EAPD. The headphone DAC gets the same format and
 * stream even when the headphones stay off, since on many laptops the
 * firmware or codec moves a speaker pin between DACs with the jack's state.
 *
 * Every pin's sense is then read, the jacks take their states from it
 * (hda_jacks_sense), and the output pins are turned on or off for the
 * output mode (hda_route_output): with the headphones alone the primary
 * pin's output is off; with them off, the headphone pin's output is off, so
 * the codec neither mutes the speaker for it nor routes sound to it. A
 * change of the microphone jack moves capture (hda_follow_mic). The caller
 * holds the jacks (hda_jack_hold).
 */
static void hda_out_codec(uint16_t fmt)
{
    hda_send_verb(codec_addr, audio_fg_nid, HDA_VERB_SET_POWER | 0x00);
    hda_delay(50);
    hda_dac_program(dac_nid, fmt, 50);
    if (hp_dac_nid != 0 && hp_dac_nid != dac_nid)
        hda_dac_program(hp_dac_nid, fmt, 20);
    hda_out_mixer_program();
    hda_out_pins_program();

    hda_jacks_sense();
    hda_route_output(HDA_VERB_WAIT);
    hda_follow_mic(HDA_VERB_WAIT);

    /* The amplifiers above are at full gain; the level set is applied over
     * them. */
    if (hda_volume != 100) {
        hda_set_volume(hda_volume);
    }

    /* Re-enable GPIO after all configuration (some codecs reset GPIO state). */
    hda_enable_gpio();
    hda_set_eapd(true);
}

/**
 * Set up the output stream for the format already chosen and begin playback.
 *
 * `fmt` is the stream format word of the programmed rate, format and
 * channels, which the caller has stored with the requested ones and the
 * resampler's step. Reset the stream, fill its DMA buffers
 * (hda_out_buffers), program the discovered codec path (hda_out_codec) and
 * start DMA. Cache the requested and programmed rates with controller and
 * converter format readbacks for procfs. Return 0, or -EIO when the stream
 * does not reset.
 *
 * The session's counters start again (hda_session_reset). The tick polls no
 * jack while the path is programmed.
 *
 * The first start after the controller was set up then waits 20 ms in
 * hda_check_dma to choose where the stream's position is read; later starts
 * keep that choice and do not wait.
 */
static int hda_launch(uint16_t fmt)
{
    int stream = output_stream_index;
    uint32_t ctl;

    hda_session_reset();
    if (!hda_stream_reset(stream)) {
        return -EIO;
    }
    hda_out_buffers(stream, fmt);

    hda_jack_hold();
    hda_out_codec(fmt);

    /* Flush caches so the controller sees BDL/buffers on real hardware */
    arch_dma_flush_all();

    /* The position buffer keeps the last stream's position until the DMA
     * writes this one's. */
    if (dma_posbuf_enabled && (uint32_t)stream * 2u < (uint32_t)(HDA_MAX_STREAMS * 2)) {
        dma_posbuf[stream * 2] = 0;
        hda_dma_sync_range(&dma_posbuf[stream * 2], (uint32_t)sizeof(uint32_t));
    }
    lpib_last = 0;
    lpib_wraps = 0;
    hw_period_last = 0;
    playing = true;

    ctl = hda_sd_read32(stream, HDA_SD_CTL) | HDA_SD_CTL_RUN;
    walclk_last = hda_read32(HDA_WALCLK);
    hda_sd_write32(stream, HDA_SD_CTL, ctl);
    stream_started = true;
    hda_report.playing = true;
    hda_report.requested_rate = current_sample_rate;
    hda_report.programmed_rate = current_hw_sample_rate;
    hda_report.stream_format = hda_read16(hda_sd_offset(stream) + HDA_SD_FORMAT);
    hda_report.dac_format = (uint16_t)hda_send_verb(codec_addr, dac_nid, 0xA0000);
    hda_report.hp_format =
        hp_dac_nid ? (uint16_t)hda_send_verb(codec_addr, hp_dac_nid, 0xA0000) : 0;
    hda_jack_release();

    if (!dma_checked) {
        hda_check_dma(stream);
    }

    return 0;
}

/**
 * Configure the codec and stream and begin playback on the write path or
 * the callback.
 *
 * Validate the requested PCM format before changing the current stream.
 * Callbacks require an exact supported rate; queued writes resample to a
 * supported rate. Return 0, -EBUSY while a stream of audio.h has the output,
 * -EINVAL for a format the codec cannot play, or -EIO when the codec was
 * never set up or the stream does not reset (hda_launch).
 */
static int hda_start(uint32_t sample_rate, audio_format_t format, int channels)
{
    uint16_t fmt;
    audio_format_t hw_format;
    uint32_t hw_rate;
    int hw_channels;

    if (play_stream)
        return -EBUSY;
    if (!hda_report.ready)
        return -EIO;
    if (sample_rate == 0 || sample_rate > 384000 || (format != AUDIO_U8 && format != AUDIO_S16LE) ||
        channels < 1 || channels > 2)
        return -EINVAL;
    hw_format = user_callback ? format : AUDIO_S16LE;
    hw_rate = user_callback ? sample_rate : hda_select_hw_rate(sample_rate);
    hw_channels = user_callback ? channels : 2;
    fmt = hda_calc_format(hw_rate, hw_format, hw_channels);
    if (fmt == 0xFFFF)
        return -EINVAL;

    if (playing) {
        hda_driver.stop();
    }

    current_sample_rate = sample_rate;
    current_format = format;
    current_channels = channels;

    resample_step_fp = hda_rate_step(sample_rate, hw_rate);
    resample_init(&hda_rs, sample_rate, hw_rate);
    resample_enabled = !user_callback && hw_rate != sample_rate;

    current_hw_sample_rate = hw_rate;
    current_hw_format = hw_format;
    current_hw_channels = hw_channels;
    return hda_launch(fmt);
}

/**
 * Halt the output stream, mute the DAC, turn the output pins and EAPD off,
 * and reset the ring buffer. The service is turned off first, with
 * interrupts off, so that no fill, and no `transfer` or `period` of a
 * stream, is made once the run bit is cleared. The next start turns the
 * pins and EAPD on again.
 */
static void hda_halt(void)
{
    int stream = output_stream_index;
    uint32_t flags;

    if (!playing) {
        return;
    }

    flags = kirq_save();
    playing = false;
    stream_started = false;
    kirq_restore(flags);
    walclk_end = hda_read32(HDA_WALCLK);

    hda_sd_write8(stream, HDA_SD_CTL, 0);
    hda_delay(100);

    /* 0xB080: output amp, mute bit set. */
    hda_send_verb(codec_addr, dac_nid, HDA_VERB_SET_AMP_GAIN | 0xB080);
    hda_jack_hold();
    if (beep_div) {
        /* A beep sounds through the pins; the tick turns them off after. */
        beep_opened = true;
        beep_quiet = 0;
        hda_report.beep_path = 1;
    } else {
        if (pin_nid)
            hda_send_verb(codec_addr, pin_nid, HDA_VERB_SET_PIN_CTRL | 0x00);
        if (hp_pin_nid && hp_pin_nid != pin_nid)
            hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_SET_PIN_CTRL | 0x00);
        hda_set_eapd(false);
        hda_report.primary_enabled = hda_report.hp_enabled = 0;
        route_hp = -1;
    }
    hda_jack_release();

    hda_report.playing = false;
    lpib_last = 0;
    hw_period_last = 0;

    ring_read_ptr = 0;
    ring_write_ptr = 0;
}

/** Stop playback on the write path or the callback; a stream of audio.h
 * that has the output is left playing, since only its owner stops it. */
static void hda_stop(void)
{
    if (!play_stream)
        hda_halt();
}

/** The output stream's position in its DMA buffer, in bytes, from the
 * position buffer when hda_check_dma chose it and from LPIB otherwise. */
static uint32_t hda_stream_position(void)
{
    uint32_t idx = (uint32_t)output_stream_index * 2u;

    if (dma_posbuf_use && idx < (uint32_t)(HDA_MAX_STREAMS * 2)) {
        /* DMA writes -> invalidate cached line before reading. */
        hda_dma_sync_range(&dma_posbuf[idx], (uint32_t)sizeof(uint32_t));
        return dma_posbuf[idx];
    }
    return hda_sd_read32(output_stream_index, HDA_SD_LPIB);
}

/**
 * The frames queued at position `pos`, in frames at the caller's rate: the
 * ring's, those hda_rs holds past the time of its next frame, and the DMA
 * buffer's from `pos` to the end of the samples the
 * service has put there, converted from the programmed rate. That end is the
 * start of the period playing at the last service, since every period after
 * it has been filled; a distance of 0 is the whole buffer. For a stream of
 * audio.h, hda_play_delay's count. 0 when no stream plays. The caller keeps
 * interrupts off, so that the service does not move the end between the
 * reads.
 */
static uint32_t hda_delay_at(uint32_t pos)
{
    uint32_t cbl = period_bytes * HDA_PERIODS;
    uint32_t dma;

    if (!playing || !current_sample_rate || !current_hw_sample_rate)
        return 0;
    if (play_stream)
        return hda_play_delay(pos);
    dma = (hw_period_last * period_bytes + cbl - pos % cbl) % cbl;
    if (dma == 0)
        dma = cbl;
    dma /= hda_out_frame();
    if (user_callback)
        return dma;
    return ring_available() / hda_in_frame() + resample_held(&hda_rs) + hda_frames_in(dma, false);
}

uint32_t hda_delay_frames(void)
{
    uint32_t flags, frames = 0;

    flags = kirq_save();
    if (hda_base && playing && stream_started)
        frames = hda_delay_at(hda_stream_position());
    kirq_restore(flags);
    return frames;
}

void hda_read_queue(void)
{
    hda_queue_report_t *q = &hda_report.queue;
    uint32_t flags = kirq_save();

    q->lpib = hda_base && hda_report.ready ? hda_stream_position() : 0;
    q->fill = ring_available();
    q->high = ring_high_water;
    q->delay_ms = 0;
    if (hda_base && hda_report.ready && current_sample_rate)
        q->delay_ms = hda_delay_at(q->lpib) * 1000u / current_sample_rate;
    q->written = session_bytes_written;
    q->dropped = session_bytes_dropped;
    q->periods = hw_fill_count;
    q->underruns = underrun_count;
    q->missed = missed_revolutions;
    kirq_restore(flags);
}

void hda_tick(void)
{
    static uint32_t seen; /* `kernel_ticks` at the last call */
    uint32_t now = (uint32_t)kernel_ticks, passed = now - seen;

    seen = now;
    hda_service_stream();
    hda_service_capture();
    hda_poll_jacks(passed);
    hda_beep_tick(passed);
}

/** Whether playback is running. */
static bool hda_is_playing(void)
{
    return playing;
}

/** Set the pull callback, NULL for the write path. hda_start chooses the
 * stream's format by whether one is set, and each refill calls it if set. */
static void hda_set_callback(audio_callback_t fn)
{
    user_callback = fn;
}

/**
 * Queue PCM samples for playback on the write path.
 *
 * Copies into the ring buffer, dropping any tail that does not fit (see
 * `ring_write`), and updates the byte counters. Returns the number of bytes
 * accepted, -EBUSY while a stream of audio.h has the output, or -EINVAL if
 * not playing or given an empty buffer.
 */
static int hda_write(const uint8_t *buffer, size_t size)
{
    uint32_t written;

    if (play_stream) {
        return -EBUSY;
    }
    if (!buffer || size == 0 || !playing) {
        return -EINVAL;
    }

    written = ring_write(buffer, (uint32_t)size);
    total_bytes_written += written;
    session_bytes_written += written;

    return (int)written;
}

/**
 * Set output volume as a 0-100 percentage across the whole codec path.
 *
 * Scales the percentage to each widget's amp step range and writes it to the
 * DAC, any mixer, the output pin, and the headphone DAC/pin. A volume of 0
 * sets the amp mute bit (0x80). The level is kept in `hda_volume`, which
 * `hda_start` applies again after it sets the amplifiers afresh.
 */
static void hda_set_volume(int volume)
{
    uint32_t amp;
    uint8_t mute;

    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;
    hda_volume = volume;

    mute = (volume == 0) ? 0x80 : 0x00;

    if (dac_nid != 0) {
        uint32_t wcap = hda_get_param(dac_nid, HDA_PARAM_AUDIO_CAPS);
        if (wcap & 0x04) { /* AC_WCAP_OUT_AMP */
            uint8_t max_gain = hda_get_out_amp_max(dac_nid);
            uint8_t gain = (max_gain != 0) ? (uint8_t)((volume * (int)max_gain) / 100)
                                           : (uint8_t)((volume * 127) / 100);
            gain = hda_clamp_gain(gain, max_gain);
            hda_send_verb(codec_addr, dac_nid,
                          HDA_VERB_SET_AMP_GAIN | 0xB000 | (uint32_t)(mute | gain));
        }
    }

    if (mixer_nid != 0) {
        uint32_t wcap = hda_get_param(mixer_nid, HDA_PARAM_AUDIO_CAPS);
        if (wcap & 0x02) { /* AC_WCAP_IN_AMP */
            uint8_t max_gain = hda_get_in_amp_max(mixer_nid);
            uint8_t gain = (max_gain != 0) ? (uint8_t)((volume * (int)max_gain) / 100)
                                           : (uint8_t)((volume * 127) / 100);
            gain = hda_clamp_gain(gain, max_gain);
            hda_send_verb(codec_addr, mixer_nid,
                          HDA_VERB_SET_AMP_GAIN | 0x7000 | (uint32_t)(mute | gain));
        }
        if (wcap & 0x04) { /* AC_WCAP_OUT_AMP */
            uint8_t max_gain = hda_get_out_amp_max(mixer_nid);
            uint8_t gain = (max_gain != 0) ? (uint8_t)((volume * (int)max_gain) / 100)
                                           : (uint8_t)((volume * 127) / 100);
            gain = hda_clamp_gain(gain, max_gain);
            hda_send_verb(codec_addr, mixer_nid,
                          HDA_VERB_SET_AMP_GAIN | 0xB000 | (uint32_t)(mute | gain));
        }
    }

    if (pin_nid != 0) {
        uint32_t wcap = hda_get_param(pin_nid, HDA_PARAM_AUDIO_CAPS);
        if (wcap & 0x04) { /* AC_WCAP_OUT_AMP */
            uint8_t max_gain = hda_get_out_amp_max(pin_nid);
            uint8_t gain = (max_gain != 0) ? (uint8_t)((volume * (int)max_gain) / 100)
                                           : (uint8_t)((volume * 127) / 100);
            gain = hda_clamp_gain(gain, max_gain);
            amp = 0xB000 | (uint32_t)(mute | gain);
            if (((codec_vendor_id >> 16) & 0xFFFF) == 0x14f1) {
                amp |= ((uint32_t)pin_conn_sel_idx << 8);
            }
            hda_send_verb(codec_addr, pin_nid, HDA_VERB_SET_AMP_GAIN | amp);
        }
    }

    if (hp_dac_nid != 0) {
        uint32_t wcap = hda_get_param(hp_dac_nid, HDA_PARAM_AUDIO_CAPS);
        if (wcap & 0x04) { /* AC_WCAP_OUT_AMP */
            uint8_t max_gain = hda_get_out_amp_max(hp_dac_nid);
            uint8_t gain = (max_gain != 0) ? (uint8_t)((volume * (int)max_gain) / 100)
                                           : (uint8_t)((volume * 127) / 100);
            gain = hda_clamp_gain(gain, max_gain);
            hda_send_verb(codec_addr, hp_dac_nid,
                          HDA_VERB_SET_AMP_GAIN | 0xB000 | (uint32_t)(mute | gain));
        }
    }
    if (hp_pin_nid != 0 && hp_pin_nid != pin_nid) {
        uint32_t wcap = hda_get_param(hp_pin_nid, HDA_PARAM_AUDIO_CAPS);
        if (wcap & 0x04) { /* AC_WCAP_OUT_AMP */
            uint8_t max_gain = hda_get_out_amp_max(hp_pin_nid);
            uint8_t gain = (max_gain != 0) ? (uint8_t)((volume * (int)max_gain) / 100)
                                           : (uint8_t)((volume * 127) / 100);
            gain = hda_clamp_gain(gain, max_gain);
            amp = 0xB000 | (uint32_t)(mute | gain);
            if (((codec_vendor_id >> 16) & 0xFFFF) == 0x14f1) {
                amp |= ((uint32_t)hp_conn_sel_idx << 8);
            }
            hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_SET_AMP_GAIN | amp);
        }
    }
}

/** The level `hda_set_volume` last set; 100 until one is. */
static int hda_get_volume(void)
{
    return hda_volume;
}

/** The input stream's position in its DMA buffer, in bytes, read as the
 * output stream's is (hda_stream_position). */
static uint32_t hda_input_position(void)
{
    uint32_t idx = (uint32_t)input_stream_index * 2u;

    if (dma_posbuf_use && idx < (uint32_t)(HDA_MAX_STREAMS * 2)) {
        hda_dma_sync_range(&dma_posbuf[idx], (uint32_t)sizeof(uint32_t));
        return dma_posbuf[idx];
    }
    return hda_sd_read32(input_stream_index, HDA_SD_LPIB);
}

/**
 * The rate nearest `rate`, the higher of two as near, among those the ADC
 * has with 16-bit PCM, with its index in hda_rates in `*index`; 0 when the
 * ADC has none.
 */
static uint32_t hda_input_rate(uint32_t rate, unsigned *index)
{
    const hda_caps_report_t *c = &hda_report.input.caps;
    uint32_t best = 0, best_d = 0;
    unsigned i;

    if (!(c->formats & 1) || !(c->pcm & (1u << 17)))
        return 0;
    for (i = 0; i < sizeof(hda_rates) / sizeof(hda_rates[0]); i++) {
        uint32_t r = hda_rates[i].rate;
        uint32_t d = r > rate ? r - rate : rate - r;

        if ((c->pcm & (1u << i)) && (!best || d <= best_d)) {
            best = r;
            best_d = d;
            *index = i;
        }
    }
    return best;
}

/**
 * Whole revolutions of the DMA buffer, of `buf` bytes, that the device made
 * in `ticks` of the wall clock beyond the `moved` bytes the position shows:
 * the elapsed time in frames less those moved, to the nearest buffer. Counted
 * in milliseconds, so that no product passes 32 bits; a buffer is longer than
 * two of them at any rate.
 */
static uint32_t hda_capture_laps(uint32_t ticks, uint32_t moved, uint32_t buf)
{
    uint32_t ms = ticks / 24000u;
    uint32_t rate = hda_report.input.rate;
    uint32_t frames = ms * (rate / 1000u) + ms * (rate % 1000u) / 1000u;
    uint32_t buf_frames = buf / cap_frame;
    uint32_t moved_frames = moved / cap_frame;

    if (frames <= moved_frames)
        return 0;
    return (frames - moved_frames + buf_frames / 2) / buf_frames;
}

/**
 * Convert the DMA period at `src` into cap_out in the format and channels of
 * `s`; return the bytes. Two channels go to one as their mean. With the
 * inverted internal mic recorded, the right channel is a copy of the left.
 */
static uint32_t hda_capture_convert(const audio_stream_t *s, const uint8_t *src)
{
    uint32_t frames = cap_period / cap_frame;
    bool left_only = hda_report.input.inv_dmic && in_pin_nid == int_mic_nid;
    uint32_t f, o = 0;
    int c;

    for (f = 0; f < frames; f++, src += cap_frame) {
        int16_t v[2];

        v[0] = (int16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
        v[1] = cap_frame == 4 && !left_only ? (int16_t)((uint16_t)src[2] | ((uint16_t)src[3] << 8))
                                            : v[0];
        if (s->channels == 1)
            v[0] = (int16_t)(((int32_t)v[0] + v[1]) / 2);
        for (c = 0; c < s->channels; c++) {
            uint16_t u = (uint16_t)v[c];

            if (s->format == AUDIO_U8) {
                cap_out[o++] = (uint8_t)((u >> 8) ^ 0x80);
            } else {
                cap_out[o++] = (uint8_t)u;
                cap_out[o++] = (uint8_t)(u >> 8);
            }
        }
    }
    return o;
}

/**
 * Hand the input stream's finished periods to its reader, from the
 * interrupt and the tick with interrupts off.
 *
 * The periods finished are those from `cap_next` to the one the position is
 * in. The device overruns when it comes round to `cap_next` again before a
 * service, which the position alone cannot show when it made a whole
 * revolution, so the wall clock counts those. On an overrun the period the
 * device writes is lost and the rest, from the one after it, are handed
 * over: the newest whole revolution, in order. A service already under way
 * elsewhere is left to finish alone.
 */
static void hda_service_capture(void)
{
    audio_stream_t *s;
    uint32_t flags, buf, pos, now, moved, laps, cur;

    if (!cap_running)
        return;
    flags = kirq_save();
    if (!spin_trylock(&cap_lock)) {
        kirq_restore(flags);
        return;
    }
    if (!cap_running || !hda_base)
        goto out;
    s = cap_stream;
    buf = cap_period * cap_periods;
    pos = hda_input_position() % buf;
    now = hda_read32(HDA_WALCLK);
    moved = (pos + buf - cap_pos) % buf;
    laps = hda_capture_laps(now - cap_clock, moved, buf);
    cur = pos / cap_period;
    if (laps > 0 || moved >= buf - cap_pos % cap_period) {
        hda_report.input.overruns++;
        cap_next = (cur + 1) % cap_periods;
    }
    cap_recorded += (moved + laps * buf) / cap_frame;
    cap_pos = pos;
    cap_clock = now;
    /* The frames up to period `cur` that are neither handed nor to be. */
    s->lost = cap_recorded - pos % cap_period / cap_frame - cap_handed -
              (cur + cap_periods - cap_next) % cap_periods * (cap_period / cap_frame);
    while (cap_next != cur) {
        const uint8_t *src = cap_buffer + cap_next * cap_period;

        hda_dma_sync_range(src, cap_period);
        s->transfer(s, cap_out, hda_capture_convert(s, src));
        s->period(s);
        cap_handed += cap_period / cap_frame;
        cap_next = (cap_next + 1) % cap_periods;
    }
    hda_report.input.frames = cap_handed;
    hda_report.input.lpib = pos;
out:
    spin_unlock(&cap_lock);
    kirq_restore(flags);
}

/** The Set Amplifier Gain payload of input amplifier `index`: `which` is
 * 0x7000 for both channels, 0x6000 for the left and 0x5000 for the right;
 * `mute` is 0x80 or 0. */
static uint32_t hda_in_amp_verb(uint32_t which, uint8_t index, uint8_t mute, uint8_t gain)
{
    return HDA_VERB_SET_AMP_GAIN | which | ((uint32_t)index << 8) | mute | (gain & 0x7F);
}

/**
 * Set the ADC's input amplifier on the connection of the source recorded,
 * in verbs of `vmode` (hda_put): to the source's gain when `live`, else
 * muted where it can mute, at the source's gain still. With the inverted
 * internal mic recorded, the right channel stays muted either way.
 */
static void hda_input_amp(int vmode, bool live, bool *ok)
{
    const hda_widget_report_t *a = hda_widget(adc_nid);
    uint8_t mute_cap = (a->amp_in & 0x80000000u) ? 0x80 : 0;
    uint8_t gain = in_gain[in_src] & 0x7F;
    uint8_t muted = live ? in_gain[in_src] & 0x80 : mute_cap;

    if (!(a->caps & (1u << 1))) /* In Amp Present */
        return;
    if (hda_report.input.inv_dmic && in_pin_nid == int_mic_nid) {
        hda_put(adc_nid, hda_in_amp_verb(0x6000, in_conn_idx, muted, gain), vmode, ok);
        hda_put(adc_nid, hda_in_amp_verb(0x5000, in_conn_idx, mute_cap, gain), vmode, ok);
    } else {
        hda_put(adc_nid, hda_in_amp_verb(0x7000, in_conn_idx, muted, gain), vmode, ok);
    }
}

/**
 * Program the route from the source recorded to the ADC, in verbs of
 * `vmode` (hda_put): turn off the input of the pin capture left (mic_left),
 * select the pin, and enable its input; then set the ADC's amplifier on the
 * pin's connection (hda_input_amp), to the source's gain when `live` and
 * muted otherwise. Return false when a verb failed.
 *
 * The ADC's other connections are muted. A mixer or selector between passes
 * the pin at 0 dB. A microphone pin gets its bias, VREF 80% where its caps
 * have it, else 50%, else 100%; a line in gets none. The pin's own input
 * amplifier, a boost on most codecs, is the source's boost. The ADC's
 * amplifier comes last, so that whatever the pin's start sends arrives
 * muted.
 */
static bool hda_input_program(int vmode, bool live)
{
    const hda_widget_report_t *a = hda_widget(adc_nid);
    const hda_widget_report_t *p = hda_widget(in_pin_nid);
    const hda_widget_report_t *m = in_mid_nid ? hda_widget(in_mid_nid) : NULL;
    uint32_t vref = (p->pin_caps >> 8) & 0xFF; /* VRef Control: Hi-Z, 50%, ground, -, 80%, 100% */
    uint16_t pin_ctl = 0x20;                   /* In Enable */
    bool ok = true;
    int i;

    if (mic_left && mic_left != in_pin_nid)
        hda_put(mic_left, HDA_VERB_SET_PIN_CTRL | 0x00, vmode, &ok);
    if (a->conn_count > 1)
        hda_put(adc_nid, HDA_VERB_SET_CONN_SEL | in_conn_idx, vmode, &ok);
    /* Amplifier input indices are four bits. */
    for (i = 0; (a->caps & (1u << 1)) && (a->amp_in & 0x80000000u) && i < a->conn_count && i < 16;
         i++)
        if (i != in_conn_idx)
            hda_put(adc_nid, hda_in_amp_verb(0x7000, (uint8_t)i, 0x80, 0), vmode, &ok);
    if (m) {
        hda_put(m->nid, HDA_VERB_SET_POWER | 0x00, vmode, &ok);
        if (m->type == HDA_WIDGET_AUDIO_SEL)
            hda_put(m->nid, HDA_VERB_SET_CONN_SEL | in_mid_idx, vmode, &ok);
        if (m->caps & (1u << 1))
            hda_put(m->nid, hda_in_amp_verb(0x7000, in_mid_idx, 0, (uint8_t)(m->amp_in & 0x7F)),
                    vmode, &ok);
        if (m->caps & (1u << 2))
            hda_put(m->nid, HDA_VERB_SET_AMP_GAIN | 0xB000 | (m->amp_out & 0x7F), vmode, &ok);
    }
    hda_put(in_pin_nid, HDA_VERB_SET_POWER | 0x00, vmode, &ok);
    if (HDA_PIN_DEF_DEV(p->config) == HDA_DEF_DEV_MIC_IN) {
        if (vref & 0x10)
            pin_ctl |= 4;
        else if (vref & 0x02)
            pin_ctl |= 1;
        else if (vref & 0x20)
            pin_ctl |= 5;
    }
    hda_put(in_pin_nid, HDA_VERB_SET_PIN_CTRL | pin_ctl, vmode, &ok);
    if (p->caps & (1u << 1))
        hda_put(in_pin_nid,
                hda_in_amp_verb(0x7000, 0, in_boost[in_src] & 0x80, in_boost[in_src] & 0x7F), vmode,
                &ok);
    hda_input_amp(vmode, live, &ok);
    hda_report.input.pin_ctl = pin_ctl;
    hda_report.input.level = in_gain[in_src] & 0x7F;
    return ok;
}

/**
 * Program the input path for format word `fmt` in Linux's order for a
 * capture's start, with the ADC's amplifier unmuted only once the converter
 * runs: power the function group and the ADC, program the route with the
 * amplifier muted (hda_input_program), give the ADC the input stream's tag
 * and, 1 ms later, the format, as snd_hda_codec_setup_stream does; wait
 * HDA_CAPTURE_SETTLE_US for the converter to settle, unmute the amplifier,
 * and wait HDA_CAPTURE_SETTLE_US again, so that what the unmute sets off
 * has passed before the DMA records. Return 0, or -EIO when a verb failed;
 * every verb is sent even so. The route's retry flags, mic_program and
 * mic_left, are cleared only on success, so that a pin capture moved from
 * is turned off at the next try.
 */
static int hda_capture_path(uint16_t fmt)
{
    bool ok = true;

    hda_put(audio_fg_nid, HDA_VERB_SET_POWER | 0x00, HDA_VERB_WAIT, &ok);
    hda_put(adc_nid, HDA_VERB_SET_POWER | 0x00, HDA_VERB_WAIT, &ok);
    if (!hda_input_program(HDA_VERB_WAIT, false))
        ok = false;
    hda_put(adc_nid, HDA_VERB_SET_CHANNEL | (HDA_CAPTURE_TAG << 4), HDA_VERB_WAIT, &ok);
    hda_delay(1000);
    hda_put(adc_nid, HDA_VERB_SET_CONV_FMT | fmt, HDA_VERB_WAIT, &ok);
    hda_delay_walclk_us(HDA_CAPTURE_SETTLE_US);
    hda_input_amp(HDA_VERB_WAIT, true, &ok);
    hda_delay_walclk_us(HDA_CAPTURE_SETTLE_US);
    if (!ok)
        return -EIO;
    mic_program = false;
    mic_left = 0;
    return 0;
}

/**
 * Make the input path ready for a capture that has not started: power the
 * function group and the ADC and program the route with the ADC's
 * amplifier muted (hda_input_program). Linux's codec init leaves its input
 * pins on in the same way, so that a digital mic is clocked and a jack
 * mic's bias steady long before a capture's start. Return false when a verb
 * failed.
 */
static bool hda_input_ready(void)
{
    bool ok = true;

    hda_put(audio_fg_nid, HDA_VERB_SET_POWER | 0x00, HDA_VERB_WAIT, &ok);
    hda_put(adc_nid, HDA_VERB_SET_POWER | 0x00, HDA_VERB_WAIT, &ok);
    return hda_input_program(HDA_VERB_WAIT, false) && ok;
}

/** Take the input stream for `s`: 0, -EBUSY while it is taken, or -ENODEV
 * when the controller has no input stream or the codec no input path. */
static int hda_capture_open(audio_stream_t *s)
{
    if (!hda_report.ready || !adc_nid || num_input_streams == 0)
        return -ENODEV;
    if (cap_stream)
        return -EBUSY;
    cap_stream = s;
    cap_period = 0;
    return 0;
}

/**
 * Grant `s` the ADC's rate nearest its own, its format and channels, and a
 * period of the frames it asks for or fewer: a whole number of the DMA
 * descriptors' 128 bytes in the ADC's frames, at most HDA_CAPTURE_PERIOD of
 * them. The DMA buffer is as many periods as fit in HDA_CAPTURE_BUFFER, at
 * most HDA_CAPTURE_ENTRIES. 0, or -EINVAL for a format, channel count or
 * period it cannot grant, or while the stream runs.
 */
static int hda_capture_setup(audio_stream_t *s)
{
    const hda_widget_report_t *a = hda_widget(adc_nid);
    uint32_t frame, hw_frame, frames, align, rate, n;
    unsigned index = 0;

    if (s != cap_stream || cap_running || (s->format != AUDIO_U8 && s->format != AUDIO_S16LE) ||
        s->channels < 1 || s->channels > 2)
        return -EINVAL;
    rate = hda_input_rate(s->rate, &index);
    hw_frame = (a->caps & 1) ? 4u : 2u; /* Stereo */
    frame = (uint32_t)s->channels * (s->format == AUDIO_S16LE ? 2u : 1u);
    align = 128u / hw_frame;
    frames = s->period_bytes / frame;
    if (frames > HDA_CAPTURE_PERIOD / hw_frame)
        frames = HDA_CAPTURE_PERIOD / hw_frame;
    frames -= frames % align;
    if (!rate || frames == 0)
        return -EINVAL;
    n = HDA_CAPTURE_BUFFER / (frames * hw_frame);
    if (n > HDA_CAPTURE_ENTRIES)
        n = HDA_CAPTURE_ENTRIES;
    cap_asked = s->rate;
    s->rate = rate;
    s->period_bytes = frames * frame;
    s->device_periods = n;
    if (s->periods < 2)
        s->periods = 2;
    cap_format = (uint16_t)(hda_rates[index].format | HDA_FMT_BITS_16 | (hw_frame / 2 - 1));
    cap_frame = hw_frame;
    cap_period = frames * hw_frame;
    cap_periods = n;
    return 0;
}

/**
 * Start the input stream as stream_setup left it: reset the descriptor, lay
 * the descriptor list over the DMA buffer with an interrupt at each period,
 * program the path, and run. The first start after the controller was set
 * up chooses where positions are read (hda_check_dma), as playback's does.
 * 0, or -EIO when the stream is not set up, already runs, or will not reset,
 * or the codec does not take the path (hda_capture_path); after an error
 * capture stays stopped, the descriptor halted and the ADC untagged.
 */
static int hda_capture_start(audio_stream_t *s)
{
    int stream = input_stream_index;
    uint64_t addr = (uint64_t)virt_to_phys(cap_buffer);
    uint32_t flags, i;
    int err;

    if (s != cap_stream || !cap_period || cap_running || !hda_base)
        return -EIO;
    if (!hda_stream_reset(stream))
        return -EIO;
    for (i = 0; i < cap_periods; i++) {
        cap_bdl[i].addr_low = (uint32_t)(addr + (uint64_t)i * cap_period);
        cap_bdl[i].addr_high = hda_hi(addr + (uint64_t)i * cap_period);
        cap_bdl[i].length = cap_period;
        cap_bdl[i].ioc = 1;
    }
    k_memset(cap_buffer, 0, cap_period * cap_periods);
    hda_dma_sync_range(cap_buffer, cap_period * cap_periods);
    hda_dma_sync_range(cap_bdl, (uint32_t)sizeof(cap_bdl));
    if ((uint32_t)stream * 2u < (uint32_t)(HDA_MAX_STREAMS * 2))
        dma_posbuf[stream * 2] = 0;
    hda_sd_write32(stream, HDA_SD_BDPL, (uint32_t)virt_to_phys(cap_bdl));
    hda_sd_write32(stream, HDA_SD_BDPU, hda_hi(virt_to_phys(cap_bdl)));
    hda_sd_write32(stream, HDA_SD_CBL, cap_period * cap_periods);
    hda_sd_write16(stream, HDA_SD_LVI, (uint16_t)(cap_periods - 1));
    hda_sd_write16(stream, HDA_SD_FORMAT, cap_format);
    hda_sd_write32(stream, HDA_SD_CTL, ((uint32_t)HDA_CAPTURE_TAG << 20) | HDA_SD_CTL_IOCE);
    hda_jack_hold();
    err = hda_capture_path(cap_format);
    if (err) {
        hda_sd_write8(stream, HDA_SD_CTL, 0);
        hda_send_verb(codec_addr, adc_nid, HDA_VERB_SET_CHANNEL | 0);
        hda_send_verb(codec_addr, in_pin_nid, HDA_VERB_SET_PIN_CTRL | 0x00);
        hda_jack_release();
        return err;
    }
    hda_jack_release();
    arch_dma_flush_all();

    hda_report.input.requested_rate = cap_asked;
    hda_report.input.rate = s->rate;
    hda_report.input.format = hda_read16(hda_sd_offset(stream) + HDA_SD_FORMAT);
    hda_report.input.period = cap_period;
    hda_report.input.periods = cap_periods;
    hda_report.input.frames = 0;
    hda_report.input.overruns = 0;
    hda_report.input.lpib = 0;

    flags = kirq_save();
    spin_lock(&cap_lock);
    cap_next = 0;
    cap_pos = 0;
    cap_recorded = 0;
    cap_handed = 0;
    s->lost = 0;
    cap_clock = hda_read32(HDA_WALCLK);
    cap_running = true;
    hda_report.input.running = true;
    hda_sd_write32(stream, HDA_SD_CTL, hda_sd_read32(stream, HDA_SD_CTL) | HDA_SD_CTL_RUN);
    spin_unlock(&cap_lock);
    kirq_restore(flags);

    if (!dma_checked)
        hda_check_dma(stream);
    return 0;
}

/**
 * Stop the input stream at once and clear its status; then take the input
 * stream's tag from the ADC, so that it sends nothing, and turn the pin's
 * input and its bias off. After this returns no service hands anything over.
 */
static void hda_capture_stop(audio_stream_t *s)
{
    int stream = input_stream_index;
    uint32_t flags;
    bool was;

    if (s != cap_stream)
        return;
    flags = kirq_save();
    spin_lock(&cap_lock);
    was = cap_running;
    cap_running = false;
    hda_report.input.running = false;
    if (was && hda_base) {
        hda_sd_write8(stream, HDA_SD_CTL, 0);
        hda_write8(hda_sd_offset(stream) + HDA_SD_STS, 0x1C); /* BCIS, FIFOE, DESE */
    }
    spin_unlock(&cap_lock);
    kirq_restore(flags);
    if (was && hda_base) {
        bool ok = true;

        hda_jack_hold();
        hda_input_amp(HDA_VERB_WAIT, false, &ok);
        hda_send_verb(codec_addr, adc_nid, HDA_VERB_SET_CHANNEL | 0);
        hda_jack_release();
    }
}

/** Give the input stream back, stopping it first. */
static void hda_capture_close(audio_stream_t *s)
{
    if (s != cap_stream)
        return;
    hda_capture_stop(s);
    cap_stream = NULL;
    cap_period = 0;
}

/** The frames the input stream has recorded since its start: those up to
 * the last service and those since, read from the position. After a stop it
 * stays at the frames recorded up to the last service. */
static uint32_t hda_capture_position(void)
{
    uint32_t flags, n;

    flags = kirq_save();
    spin_lock(&cap_lock);
    n = cap_recorded;
    if (cap_running && hda_base) {
        uint32_t buf = cap_period * cap_periods;

        n += (hda_input_position() % buf + buf - cap_pos) % buf / cap_frame;
    }
    spin_unlock(&cap_lock);
    kirq_restore(flags);
    return n;
}

/**
 * The rate the codec plays a stream of `rate` at: `rate` itself when every
 * DAC has it, else the lowest rate they have above it, else the highest
 * below; 0 when they have no 16-bit PCM. The rates are those of every
 * sample size the DACs have, so a stream plays at the same rate in 16 bits
 * or 24, and 44.1 and 48 kHz play exactly on any codec that has them.
 */
static uint32_t hda_play_rate(uint32_t rate)
{
    uint32_t pcm = hda_common_pcm();
    uint32_t above = 0, below = 0;
    unsigned i;

    if (!(pcm & (1u << 17)))
        return 0;
    for (i = 0; i < sizeof(hda_rates) / sizeof(hda_rates[0]); i++) {
        if (!(pcm & (1u << i)))
            continue;
        if (hda_rates[i].rate >= rate) {
            if (!above)
                above = hda_rates[i].rate;
        } else {
            below = hda_rates[i].rate;
        }
    }
    return above ? above : below;
}

/** Give the output to playback stream `s`: 0, -ENODEV for capture or a codec
 * with no path, or -EBUSY while another stream or the write path has it. */
static int hda_play_open(audio_stream_t *s)
{
    if (s->dir != AUDIO_PLAYBACK || !hda_report.ready)
        return -ENODEV;
    if (play_stream || playing)
        return -EBUSY;
    play_stream = s;
    return 0;
}

/**
 * Grant `s` its channels and the rate it asks for up to HDA_RATE_RATIO
 * times the rate it plays at (hda_play_rate), which is the resampler's
 * limit. Unsigned 8-bit and signed 16-bit are granted as they are, and play
 * as 16 bits; a wider format is granted AUDIO_S32LE, played as 24 bits in
 * 32, when every DAC has 24 bits, and signed 16-bit otherwise, the period
 * asked for keeping its frames. The DMA's period is hda_period_bytes's at
 * the rate played, about 23 ms; the stream's period is the one asked for,
 * cut to the frames of the stream a DMA period plays, so that a `period`
 * call is due at least at every DMA period's end. `device_periods` covers
 * the frames `stream_start` takes for the three DMA periods. 0, or -EINVAL
 * for a stream that is not the open one or runs, a format not in
 * audio_format_t, a channel count other than 1 or 2, or a codec with no
 * 16-bit PCM.
 */
static int hda_play_setup(audio_stream_t *s)
{
    uint32_t hw, rate, step, dma, per, frame, want, first;
    audio_format_t format = s->format;

    if (s != play_stream || playing || s->rate == 0 || s->channels < 1 || s->channels > 2 ||
        (format != AUDIO_U8 && format != AUDIO_S16LE && format != AUDIO_S24LE &&
         format != AUDIO_S24_3LE && format != AUDIO_S32LE))
        return -EINVAL;
    hw = hda_play_rate(s->rate);
    if (!hw)
        return -EINVAL;
    if (AUDIO_SAMPLE_BYTES(format) > 2u)
        format = (hda_common_pcm() & (1u << 19)) ? AUDIO_S32LE : AUDIO_S16LE;
    rate = s->rate < HDA_RATE_RATIO * hw ? s->rate : HDA_RATE_RATIO * hw;
    step = hda_rate_step(rate, hw);
    frame = 2u * (format == AUDIO_S32LE ? 4u : 2u);
    dma = hda_period_bytes(hw, frame) / frame;
    per = (dma * step) >> 16;
    want = s->period_bytes / ((uint32_t)s->channels * AUDIO_SAMPLE_BYTES(s->format));
    frame = (uint32_t)s->channels * AUDIO_SAMPLE_BYTES(format);
    if (want > per)
        want = per;
    if (want == 0)
        return -EINVAL;
    first = 1u + (((HDA_PERIODS * dma - 1u) * step) >> 16);
    s->rate = rate;
    s->format = format;
    s->period_bytes = want * frame;
    s->device_periods = (first + want - 1) / want;
    if (s->periods < s->device_periods)
        s->periods = s->device_periods;
    return 0;
}

/** Start playback stream `s` with the parameters `stream_setup` granted, a
 * stream of 32-bit samples as 24 bits in 32 and any other as 16 bits: 0, or
 * -EIO when it is not the open stream or the device fails to start. */
static int hda_play_start(audio_stream_t *s)
{
    audio_format_t format;
    uint32_t hw;
    uint16_t fmt;

    if (s != play_stream)
        return -EIO;
    hda_halt();
    hw = hda_play_rate(s->rate);
    format = s->format == AUDIO_S32LE ? AUDIO_S32LE : AUDIO_S16LE;
    fmt = hw ? hda_calc_format(hw, format, 2) : 0xFFFF;
    if (fmt == 0xFFFF)
        return -EIO;
    current_sample_rate = s->rate;
    current_format = s->format;
    current_channels = s->channels;
    current_hw_sample_rate = hw;
    current_hw_format = format;
    current_hw_channels = 2;
    resample_step_fp = hda_rate_step(s->rate, hw);
    resample_init(&hda_rs, s->rate, hw);
    resample_enabled = hw != s->rate;
    stream_pos = 0;
    stream_mark = 0;
    return hda_launch(fmt) == 0 ? 0 : -EIO;
}

/** Stop playback stream `s` at once. */
static void hda_play_stop(audio_stream_t *s)
{
    if (s == play_stream)
        hda_halt();
}

/** Stop playback stream `s` and give the output back. */
static void hda_play_close(audio_stream_t *s)
{
    if (s != play_stream)
        return;
    hda_halt();
    play_stream = NULL;
}

/** The frames of `s` played since its start (hda_play_played), or the
 * last count given while it is stopped. Safe with interrupts off. */
static uint32_t hda_play_frames(audio_stream_t *s)
{
    uint32_t flags = kirq_save();
    uint32_t pos = stream_pos;

    if (s == play_stream && hda_base && playing && stream_started)
        pos = hda_play_played();
    kirq_restore(flags);
    return pos;
}

/*
 * The input controls, which /dev/mixer reaches through the driver's
 * input_caps, input_select and input_level. A level is in percent of an
 * amplifier's steps, from step 0 at 0% to its last step at 100%, rounded
 * to the nearest; 0% mutes an amplifier that can mute.
 */

/** Whether a widget with capabilities `caps` and input amplifier
 * capabilities `amp` has an input amplifier that can change: one with
 * steps or a mute. */
static bool hda_in_amp_usable(uint32_t caps, uint32_t amp)
{
    return (caps & (1u << 1)) && (hda_amp_steps(amp) > 0 || (amp & 0x80000000u));
}

/** The amplifier word, step and mute bit, of `level` percent on an
 * amplifier with capabilities `amp`. */
static uint8_t hda_level_word(uint32_t amp, int level)
{
    uint32_t steps = hda_amp_steps(amp);

    if (level > 100)
        level = 100;
    if (level == 0 && (amp & 0x80000000u))
        return 0x80;
    return (uint8_t)(((uint32_t)level * steps + 50) / 100);
}

/** The level in percent of amplifier word `word` on an amplifier with
 * capabilities `amp`. */
static int hda_word_level(uint32_t amp, uint8_t word)
{
    uint32_t steps = hda_amp_steps(amp);

    if (word & 0x80)
        return 0;
    return steps ? (int)(((word & 0x7Fu) * 100 + steps / 2) / steps) : 100;
}

/** Find the route from the ADC to `source`'s pin into the input path's
 * fields when `take` is true: its pin, or 0 when the ADC cannot reach it. */
static uint8_t hda_source_route(int source, bool take)
{
    uint8_t pin, index, mid, mid_index;

    if (!adc_nid || source < 0 || source >= AUDIO_SOURCES || !(pin = hda_source_pin(source)) ||
        !hda_input_route(adc_nid, pin, &index, &mid, &mid_index))
        return 0;
    if (take) {
        in_src = source;
        in_pin_nid = hda_report.input.pin = pin;
        in_conn_idx = hda_report.input.index = index;
        in_mid_nid = hda_report.input.mid = mid;
        in_mid_idx = hda_report.input.mid_index = mid_index;
        hda_report.input.level = in_gain[source] & 0x7F;
    }
    return pin;
}

/** The sources the ADC reaches, and in `*controls` the gain when the ADC
 * has an input amplifier and the boost when a source's pin has one. */
static unsigned hda_input_caps(unsigned *controls)
{
    const hda_widget_report_t *a = hda_widget(adc_nid);
    unsigned sources = 0, ctl = 0;
    int i;

    for (i = 0; a && i < AUDIO_SOURCES; i++) {
        uint8_t pin = hda_source_route(i, false);
        const hda_widget_report_t *p = pin ? hda_widget(pin) : NULL;

        if (!p)
            continue;
        sources |= 1u << i;
        if (hda_in_amp_usable(p->caps, p->amp_in))
            ctl |= 1u << AUDIO_IN_BOOST;
        if (hda_in_amp_usable(a->caps, a->amp_in))
            ctl |= 1u << AUDIO_IN_GAIN;
    }
    *controls = ctl;
    return sources;
}

/** Record `source`, reprogramming the route at once while the input stream
 * runs, with the input of the pin left turned off; the source recorded
 * after, or -ENODEV when the codec has no input path. The tick polls no jack
 * meanwhile. */
static int hda_input_select(int source)
{
    uint8_t from = in_pin_nid;

    if (!adc_nid)
        return -ENODEV;
    hda_jack_hold();
    if (source != in_src && hda_source_route(source, true)) {
        mic_left = from;
        mic_program = !hda_input_program(HDA_VERB_WAIT, cap_running);
        if (!mic_program)
            mic_left = 0;
    }
    hda_jack_release();
    return in_src;
}

/** Set or read control `ctl` of `source`, reprogramming the route at once
 * when it is the source recorded and the input stream runs. */
static int hda_input_level(int ctl, int source, int level)
{
    const hda_widget_report_t *w;
    uint8_t pin = hda_source_route(source, false);
    uint8_t *word;

    if (!pin)
        return -EINVAL;
    if (ctl == AUDIO_IN_GAIN) {
        w = hda_widget(adc_nid);
        word = &in_gain[source];
    } else if (ctl == AUDIO_IN_BOOST) {
        w = hda_widget(pin);
        word = &in_boost[source];
    } else {
        return -EINVAL;
    }
    if (!w || !hda_in_amp_usable(w->caps, w->amp_in))
        return -EINVAL;
    if (level >= 0) {
        *word = hda_level_word(w->amp_in, level);
        if (source == in_src) {
            hda_report.input.level = in_gain[source] & 0x7F;
            if (cap_running) {
                hda_jack_hold();
                hda_input_program(HDA_VERB_WAIT, true);
                hda_jack_release();
            }
        }
    }
    return hda_word_level(w->amp_in, *word);
}

/*
 * The beep generator: the codec's beep widget, which sounds a square wave of
 * 48000 / (4 x divider) Hz for a divider of 1 to 255 sent with Set Beep
 * Generation, and stops at 0. Linux's hda_beep.c drives it the same way for
 * the console's tones. On the Lenovo G50-70's Conexant codec it is node 18,
 * which no widget lists as a connection: the codec mixes it into the output
 * pins itself, so the pin playing has to be on.
 */

/**
 * Find the beep widget, the first of type 7, and set it up as Linux's
 * generic parser does: power it where it has power states, set its output
 * amplifier to its 0 dB step, the amplifier's offset, and unmute the input
 * from it of every mixer that lists it, at that input's 0 dB step. The
 * generator starts silent. Nothing is sent on a codec that has none.
 */
static void hda_find_beep(void)
{
    const hda_widget_report_t *b = NULL;
    int i, j;

    beep_nid = 0;
    beep_div = 0;
    beep_opened = false;
    beep_quiet = 0;
    hda_report.beep = hda_report.beep_divider = hda_report.beep_path = 0;
    hda_report.beeps = 0;
    for (i = 0; i < hda_report.widget_count && !b; i++)
        if (hda_report.widgets[i].type == HDA_WIDGET_BEEP)
            b = &hda_report.widgets[i];
    if (!b)
        return;
    beep_nid = hda_report.beep = b->nid;
    if (b->caps & (1u << 10)) /* Power Control */
        hda_send_verb(codec_addr, beep_nid, HDA_VERB_SET_POWER | 0x00);
    beep_level = 100;
    if (b->caps & (1u << 2)) { /* Out Amp Present */
        beep_word = hda_clamp_gain((uint8_t)(b->amp_out & 0x7F), hda_amp_steps(b->amp_out));
        beep_level = hda_word_level(b->amp_out, beep_word);
        if (beep_level == 0)
            beep_level = 1;
        hda_send_verb(codec_addr, beep_nid, HDA_VERB_SET_AMP_GAIN | 0xB000 | beep_word);
    }
    for (i = 0; i < hda_report.widget_count; i++) {
        const hda_widget_report_t *m = &hda_report.widgets[i];

        if (m->type != HDA_WIDGET_AUDIO_MIX || !(m->caps & (1u << 1))) /* In Amp Present */
            continue;
        /* Amplifier input indices are four bits. */
        for (j = 0; j < m->conn_count && j < 16; j++)
            if (m->conns[j] == beep_nid)
                hda_send_verb(
                    codec_addr, m->nid,
                    HDA_VERB_SET_AMP_GAIN | 0x7000 | ((uint32_t)j << 8) |
                        hda_clamp_gain((uint8_t)(m->amp_in & 0x7F), hda_amp_steps(m->amp_in)));
    }
    hda_send_verb(codec_addr, beep_nid, HDA_VERB_SET_BEEP | 0);
}

/**
 * Sound `hz` on the beep generator until the next call, or stop it with 0,
 * as audio.h's `beep` says. The divider is Linux's, 12000 / `hz` held
 * between 1 and 255, so that 750 Hz is 16; a level of 0 sends 0. With no
 * stream playing, a tone first turns the output pins on for the output mode
 * and EAPD, as a start does (hda_route_output), and hda_beep_tick turns them
 * off a second after the generator stops. A stop from the tick sends only
 * the stop, in a try. The divider sent, or -ENODEV, or -EBUSY from the tick.
 */
static int hda_beep(uint32_t hz, int tick)
{
    uint32_t div = 0;

    if (!beep_nid)
        return -ENODEV;
    if (hz && beep_level && !tick) {
        div = 12000u / hz;
        if (div > 255)
            div = 255;
        if (div < 1)
            div = 1;
    }
    if (tick) {
        if (hda_verb(codec_addr, beep_nid, HDA_VERB_SET_BEEP | 0, HDA_VERB_TRY, NULL) != 0)
            return -EBUSY;
        beep_div = 0;
        hda_report.beep_divider = 0;
        return 0;
    }
    hda_jack_hold();
    if (div && !playing && !beep_opened) {
        hda_route_output(HDA_VERB_WAIT);
        hda_set_eapd(true);
        beep_opened = true;
        hda_report.beep_path = 1;
    }
    beep_quiet = 0;
    hda_send_verb(codec_addr, beep_nid, HDA_VERB_SET_BEEP | div);
    beep_div = (uint8_t)div;
    hda_report.beep_divider = (uint8_t)div;
    if (div)
        hda_report.beeps++;
    hda_jack_release();
    return (int)div;
}

/**
 * From the tick: once a stream plays, leave the pins a beep turned on to it;
 * with none, turn them and EAPD off when the generator has been silent for
 * HDA_BEEP_IDLE_TICKS, in tries, and try again at the next tick when a verb
 * fails or code outside the tick holds the jacks. `passed` is the ticks
 * since the last call (hda_tick).
 */
static void hda_beep_tick(uint32_t passed)
{
    uint32_t flags;
    bool ok = true;
    int i;

    if (!beep_opened || beep_div)
        return;
    if (playing) {
        beep_opened = false;
        hda_report.beep_path = 0;
        return;
    }
    beep_quiet += passed;
    if (beep_quiet < HDA_BEEP_IDLE_TICKS)
        return;
    flags = kirq_save();
    if (!spin_trylock(&jack_lock)) {
        kirq_restore(flags);
        return;
    }
    if (jack_holds == 0 && beep_opened && !beep_div && !playing) {
        if (pin_nid)
            hda_put(pin_nid, HDA_VERB_SET_PIN_CTRL | 0x00, HDA_VERB_TRY, &ok);
        if (hp_pin_nid && hp_pin_nid != pin_nid)
            hda_put(hp_pin_nid, HDA_VERB_SET_PIN_CTRL | 0x00, HDA_VERB_TRY, &ok);
        for (i = 0; i < hda_report.widget_count; i++) {
            const hda_widget_report_t *w = &hda_report.widgets[i];

            if (w->type == HDA_WIDGET_PIN && (w->pin_caps & 0x10000)) /* EAPD capable */
                hda_put(w->nid, HDA_VERB_SET_EAPD | 0x00, HDA_VERB_TRY, &ok);
        }
        if (ok) {
            beep_opened = false;
            hda_report.beep_path = 0;
            hda_report.primary_enabled = hda_report.hp_enabled = 0;
            route_hp = -1;
        }
    }
    spin_unlock(&jack_lock);
    kirq_restore(flags);
}

/**
 * Set or read the beep generator's level, as audio.h's `beep_level` says:
 * the output amplifier's step nearest `level`, muted at 0 where it can
 * mute. A level other than 0 reads as its step's, and at least 1, since
 * only 0 leaves the generator silent; without an amplifier any other level
 * is full. Each tone takes the level when it begins.
 */
static int hda_beep_level(int level)
{
    const hda_widget_report_t *b = hda_widget(beep_nid);

    if (!beep_nid || !b)
        return -ENODEV;
    if (level < 0)
        return beep_level;
    if (level > 100)
        level = 100;
    beep_level = level;
    if (b->caps & (1u << 2)) { /* Out Amp Present */
        beep_word = hda_level_word(b->amp_out, level);
        hda_send_verb(codec_addr, beep_nid, HDA_VERB_SET_AMP_GAIN | 0xB000 | beep_word);
        if (level) {
            beep_level = hda_word_level(b->amp_out, beep_word);
            if (beep_level == 0)
                beep_level = 1;
        }
    }
    return beep_level;
}

/*
 * The stream operations: capture is the input stream, and playback the
 * output stream, which a stream has from hda_play_open to hda_play_close.
 */

/** Open `s`'s direction. */
static int hda_stream_open(audio_stream_t *s)
{
    return s->dir == AUDIO_CAPTURE ? hda_capture_open(s) : hda_play_open(s);
}

/** Grant `s` its parameters. */
static int hda_stream_setup(audio_stream_t *s)
{
    return s->dir == AUDIO_CAPTURE ? hda_capture_setup(s) : hda_play_setup(s);
}

/** Start `s`. */
static int hda_stream_start(audio_stream_t *s)
{
    return s->dir == AUDIO_CAPTURE ? hda_capture_start(s) : hda_play_start(s);
}

/** Stop `s`. */
static void hda_stream_stop(audio_stream_t *s)
{
    if (s->dir == AUDIO_CAPTURE)
        hda_capture_stop(s);
    else
        hda_play_stop(s);
}

/** Give `s`'s direction back. */
static void hda_stream_close(audio_stream_t *s)
{
    if (s->dir == AUDIO_CAPTURE)
        hda_capture_close(s);
    else
        hda_play_close(s);
}

/** The frames `s` has played or recorded since its start. */
static uint32_t hda_stream_frames(audio_stream_t *s)
{
    if (s->dir == AUDIO_CAPTURE)
        return s == cap_stream ? hda_capture_position() : 0;
    return hda_play_frames(s);
}

/** The frames given and not yet played, at the caller's rate
 * (hda_delay_frames): the playback stream's for `s` when it has the output,
 * or the write path's for `s` NULL while no stream has it; 0 otherwise, and
 * for capture, which plays nothing. */
static uint32_t hda_stream_delay(audio_stream_t *s)
{
    if (s ? s != play_stream : play_stream != NULL)
        return 0;
    return hda_delay_frames();
}

/* The audio_driver_t fields, in the order audio.h declares them: name,
   type, init, shutdown, start, stop, is_playing, set_callback, write,
   set_volume, get_volume, the seven stream operations, the three input
   controls, then the two of the beep generator. */
audio_driver_t hda_driver = {"Intel HD Audio",  AUDIO_DRIVER_HDA, hda_init,        hda_shutdown,
                             hda_start,         hda_stop,         hda_is_playing,  hda_set_callback,
                             hda_write,         hda_set_volume,   hda_get_volume,  hda_stream_open,
                             hda_stream_setup,  hda_stream_start, hda_stream_stop, hda_stream_close,
                             hda_stream_frames, hda_stream_delay, hda_input_caps,  hda_input_select,
                             hda_input_level,   hda_beep,         hda_beep_level};
