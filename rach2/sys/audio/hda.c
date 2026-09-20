/**
 * Intel HD Audio (HDA) playback driver.
 *
 * Drives audio output on an Intel HDA controller: codec verbs go over the
 * Immediate Command Interface (CORB/RIRB are set up but enumeration uses ICI),
 * and PCM frames stream out of a cyclic BDL via the controller's DMA engine.
 */

#include "../klog.h"
#include "hda.h"
#include "../kio.h"
#include "../serial.h"
#include "../isr.h"
#include "../lapic.h"
#include "../lib/kcpu.h"
#include "../pci.h"
#include "../lib/kstd.h"
#include "../lib/kstring.h"
#include "../mm/paging.h"

/* HDA MMIO region size (typically 16KB) */
#define HDA_MMIO_SIZE 0x4000

/* PCI capability IDs and offsets (for power/MSI diagnostics) */
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
static uint16_t num_bidir_streams = 0;

/* Physical: the streams the controller's register layout addresses. */
#define HDA_MAX_STREAMS 32
/* DMA position buffer (optional; more reliable than LPIB on some
 * controllers). */
static volatile uint32_t dma_posbuf[HDA_MAX_STREAMS * 2] __attribute__((aligned(128)));
static volatile bool dma_posbuf_enabled = false;
static volatile bool dma_posbuf_use = false;

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

/* Remember selected connection indices for pins */
static uint8_t pin_conn_sel_idx = 0;
static uint8_t hp_conn_sel_idx = 0;

/* Stream info */
static uint16_t output_stream_index = 0; /* Set during init based on GCAP */
static uint32_t stream_tag = 1;

/* CORB/RIRB buffers (must be 128-byte aligned, volatile for DMA) */
static volatile uint32_t corb_buffer[HDA_CORB_SIZE] __attribute__((aligned(128)));
static volatile uint64_t rirb_buffer[HDA_RIRB_SIZE] __attribute__((aligned(128)));
static volatile uint16_t corb_wp = 0;
static volatile uint16_t rirb_rp = 0;

/*
 * DMA period and buffer size. BDL entry buffer addresses must be 128-byte
 * aligned, so each period is a multiple of 128 bytes in this contiguous
 * multi-buffer. On real hardware legacy IRQs may not arrive (MSI/IOAPIC
 * routing), so the stream is also serviced from the 100Hz PIT tick; the
 * period is large enough to tolerate occasional missed ticks. 16384 bytes
 * is about 85ms at 48kHz 16-bit stereo (4096 frames). Volatile for DMA.
 */
#define HDA_BUFFER_SIZE 16384
#define HDA_NUM_BUFFERS 2 /* Double-buffered like SB16 */
static volatile hda_bdl_entry_t bdl[HDA_NUM_BUFFERS] __attribute__((aligned(1024)));
static int16_t audio_buffer[HDA_BUFFER_SIZE * HDA_NUM_BUFFERS / 2] __attribute__((aligned(128)));

/* Ring buffer for the write() audio path (Doom). */
#define HDA_RING_SIZE 8192 /* sized to tolerate bursty writes */
static uint8_t hda_ring[HDA_RING_SIZE];
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
static bool hda_probe_log_enabled = false;

static void hda_probe_log(const char *format, ...);
static void hda_build_boot_status(void);

static uint32_t ring_available(void)
{
    uint32_t w = ring_write_ptr;
    uint32_t r = ring_read_ptr;
    if (w >= r) {
        return w - r;
    }
    return HDA_RING_SIZE - r + w;
}

static uint32_t ring_free(void)
{
    return HDA_RING_SIZE - 1 - ring_available();
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
        wp = (wp + 1) % HDA_RING_SIZE;
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
static audio_format_t current_format = AUDIO_S16LE;
static int current_channels = 2;

/* Actual hardware stream configuration (may differ from requested). */
static uint32_t current_hw_sample_rate = 0;
static audio_format_t current_hw_format = AUDIO_S16LE;
static int current_hw_channels = 2;

/* Write-path resampler state (fixed-point 16.16). */
static bool resample_enabled = false;
static uint32_t resample_step_fp = 0; /* input_rate / hw_rate */
static uint32_t resample_frac_fp = 0;

/* Persistent diagnostics. */
static volatile uint32_t total_bytes_written = 0;

/*
 * DMA progress tracked as an absolute period number (LPIB plus wrap count),
 * not just the 0/1 buffer index. After a full CBL wrap the buffer index
 * returns to its old value, and an absolute period number tells that apart
 * from no progress. The wrap is counted when a reading comes out lower than
 * the one before it, so this holds as long as the handler reads LPIB more
 * often than the buffer turns over; a whole revolution between two readings
 * still looks like none.
 */
static volatile bool lpib_tracking_valid = false;
static volatile uint32_t lpib_last = 0;
static volatile uint32_t lpib_wraps = 0;
static volatile uint32_t hw_period_last = 0; /* counts HDA_BUFFER_SIZE periods */

/* Diagnostics: number of DMA periods we refilled. */
static volatile uint32_t hw_fill_count = 0;

/* Forward declarations for MMIO helpers (needed for C89 order). */
static uint32_t hda_read32(uint32_t offset);
static void hda_write32(uint32_t offset, uint32_t value);

/* Cache/DMA coherency helpers (real hardware). */
static bool hda_have_clflush = false;
static uint32_t hda_clflush_size = 64;

/**
 * Detect CLFLUSH support and cache line size for DMA coherency.
 *
 * Records whether CLFLUSH is available and its line size so DMA buffers can be
 * flushed a line at a time on real hardware. Emulators report no CLFLUSH and
 * fall back to a heavier sync.
 */
static void hda_cpu_detect_cache(void)
{
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    uint32_t sz;

    /* CPUID leaf 1: feature flags + CLFLUSH line size. */
    kcpu_id(1, &eax, &ebx, &ecx, &edx);

    hda_have_clflush = ((edx & (1u << 19)) != 0);
    sz = ((ebx >> 8) & 0xFFu) * 8u;
    if (sz == 0) {
        sz = 64;
    }
    hda_clflush_size = sz;
}

/**
 * Push a CPU-written range out to memory so the DMA engine sees it.
 *
 * CLFLUSHes each cache line spanning [addr, addr+size) when CLFLUSH is
 * available, then issues a full barrier; otherwise falls back to WBINVD.
 * A no-op in practice on emulators, which keep DMA coherent.
 */
static void hda_dma_sync_range(const volatile void *addr, uint32_t size)
{
    uintptr_t p;
    uintptr_t end;
    uintptr_t line;

    if (!addr || size == 0) {
        return;
    }

    if (hda_have_clflush && hda_clflush_size != 0) {
        line = (uintptr_t)hda_clflush_size;
        p = (uintptr_t)addr & ~(line - 1u);
        end = (uintptr_t)addr + (uintptr_t)size;
        while (p < end) {
            /* clflush (%eax) as bytes 0f ae 38: tcc has no mnemonic for it. */
            __asm__ __volatile__(".byte 0x0f, 0xae, 0x38" ::"a"(p) : "memory");
            p += line;
        }
        /* Full barrier (avoid SSE mfence). */
        __asm__ __volatile__("lock; addl $0, 0(%%esp)" ::: "memory");
    } else {
        /* Heavy fallback, but safe on all x86 that support HDA. */
        __asm__ __volatile__("wbinvd" ::: "memory");
    }
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

    addr = (uint32_t)(uintptr_t)dma_posbuf;
    addr &= ~0x7Fu; /* 128-byte aligned base */

    /* Bit0 enables the position buffer (if implemented by the controller). */
    hda_write32(HDA_DPLBASE, addr | 0x1u);
    hda_write32(HDA_DPUBASE, 0);

    dpl = hda_read32(HDA_DPLBASE);
    dma_posbuf_enabled = ((dpl & 0x1u) != 0) && ((dpl & ~0x7Fu) == addr);
    dma_posbuf_use = dma_posbuf_enabled;
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

/*
 * The bring-up trace, which composes its lines from fragments: a step prints
 * its name, then the reason, then "OK" or "FAIL" and the newline. A record is
 * a whole line, so the fragments are gathered here and one record is emitted
 * when the line ends. A line longer than the buffer is emitted at the length
 * that fits, which is the same thing the record builder does.
 */
static char hda_trace_line[192];
static unsigned hda_trace_len;

static void hda_trace_putc(char c)
{
    if (c == '\n') {
        const char *text = hda_trace_line;

        hda_trace_line[hda_trace_len] = '\0';
        /* Some fragments name the subsystem themselves, from when this went
           to the serial port unprefixed; the record already says it. */
        if (hda_trace_len >= 5 && text[0] == 'H' && text[1] == 'D' && text[2] == 'A' &&
            text[3] == ':' && text[4] == ' ') {
            text += 5;
        }
        if (hda_trace_len > 0) {
            klog(KLOG_DEBUG, "hda", "%s", text);
        }
        hda_trace_len = 0;
        return;
    }
    if (hda_trace_len + 1 < sizeof(hda_trace_line)) {
        hda_trace_line[hda_trace_len++] = c;
    }
}

static void hda_probe_log(const char *format, ...)
{
    va_list ap;

    if (!hda_probe_log_enabled) {
        return;
    }

    va_start(ap, format);
    k_vprintf(hda_trace_putc, format, ap);
    va_end(ap);
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

/**
 * Render `out_frames` of ring-buffer audio into one hardware DMA period.
 *
 * Converts from the userspace-requested format, rate, and channel count
 * (typically Doom: 11025Hz U8 mono) to the hardware format (typically 48kHz
 * S16 stereo) using a fixed-point 16.16 sample-hold resampler. Mono input is
 * duplicated to both output channels; stereo input is mixed down before
 * resampling. A short ring counts as an underrun and emits silence.
 */
static void ring_render_to_hw_buffer(int16_t *dest, uint32_t out_frames)
{
    uint32_t rp = ring_read_ptr;
    uint32_t avail_bytes = ring_available();
    uint32_t frac = resample_frac_fp;
    uint32_t step = resample_step_fp;
    uint32_t in_bytes_per_sample;
    uint32_t in_bytes_per_frame;
    uint32_t out_ch;
    uint32_t f;
    bool had_underrun = false;
    int16_t local_min = out_min_s16;
    int16_t local_max = out_max_s16;

    if (current_format == AUDIO_S16LE) {
        in_bytes_per_sample = 2;
    } else {
        in_bytes_per_sample = 1; /* AUDIO_U8 */
    }

    if (current_channels <= 0) {
        in_bytes_per_frame = in_bytes_per_sample;
    } else {
        in_bytes_per_frame = in_bytes_per_sample * (uint32_t)current_channels;
        if (in_bytes_per_frame == 0) {
            in_bytes_per_frame = in_bytes_per_sample;
        }
    }

    if (current_hw_channels <= 0) {
        out_ch = 1;
    } else {
        out_ch = (uint32_t)current_hw_channels;
    }

    if (step == 0) {
        /* Shouldn't happen, but avoid lock-up. */
        step = 1u << 16;
    }

    for (f = 0; f < out_frames; f++) {
        int16_t sample16 = 0;
        uint32_t oi = f * out_ch;

        /* Read current input frame (sample-hold). */
        if (avail_bytes < in_bytes_per_frame) {
            had_underrun = true;
            sample16 = 0;
            underrun_samples += out_ch;
        } else if (current_format == AUDIO_S16LE) {
            int16_t left = (int16_t)((uint16_t)hda_ring[rp] |
                                     ((uint16_t)hda_ring[(rp + 1) % HDA_RING_SIZE] << 8));
            if (current_channels >= 2 && avail_bytes >= 4) {
                uint32_t rp_r = (rp + 2) % HDA_RING_SIZE;
                int16_t right = (int16_t)((uint16_t)hda_ring[rp_r] |
                                          ((uint16_t)hda_ring[(rp_r + 1) % HDA_RING_SIZE] << 8));
                sample16 = (int16_t)(((int32_t)left + (int32_t)right) / 2);
            } else {
                sample16 = left;
            }
        } else {
            uint8_t left8 = hda_ring[rp];
            if (current_channels >= 2 && avail_bytes >= 2) {
                uint8_t right8 = hda_ring[(rp + 1) % HDA_RING_SIZE];
                int16_t mixed = (int16_t)(((int32_t)left8 + (int32_t)right8) / 2);
                sample16 = (int16_t)(mixed - 128) * 256;
            } else {
                sample16 = ((int16_t)left8 - 128) * 256;
            }
        }

        /* Mono output duplicates the sample to both channels. */
        if (out_ch == 2) {
            dest[oi] = sample16;
            dest[oi + 1] = sample16;
        } else {
            dest[oi] = sample16;
        }

        if (sample16 < local_min) {
            local_min = sample16;
        }
        if (sample16 > local_max) {
            local_max = sample16;
        }

        /* Advance resampler: consume input frames as frac crosses 1.0. */
        frac += step;
        while (frac >= (1u << 16)) {
            frac -= (1u << 16);
            if (avail_bytes >= in_bytes_per_frame) {
                rp = (rp + in_bytes_per_frame) % HDA_RING_SIZE;
                avail_bytes -= in_bytes_per_frame;
            } else {
                break;
            }
        }
    }

    ring_read_ptr = rp;
    resample_frac_fp = frac;
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
static int hda_get_connections(uint8_t nid, uint8_t *conn_list, int max_conn);
static void hda_pci_force_d0(pci_device_t *dev);
static bool hda_pcm_rate_supported(uint32_t pcm_caps, uint32_t rate);
static uint32_t hda_select_hw_rate(uint32_t req_rate);
static uint8_t hda_amp_steps(uint32_t amp_caps);
static uint8_t hda_get_out_amp_max(uint8_t nid);
static uint8_t hda_get_in_amp_max(uint8_t nid);
static uint8_t hda_clamp_gain(uint8_t gain, uint8_t max);

/**
 * Refill the DMA period the engine is about to play, if it has advanced.
 *
 * Called from the HDA stream IRQ or, when legacy IRQs are not delivered (the
 * controller is MSI-only), from the PIT tick. Reads the play pointer from the
 * position buffer or LPIB, converts it to an absolute period number, and
 * returns without work if no new period has started. Otherwise it fills the
 * buffer the engine is not currently reading, from the user callback or the
 * ring resampler, and flushes it so the controller sees fresh samples.
 */
static void hda_service_stream(void)
{
    uint32_t lpib;
    uint32_t cbl;
    uint32_t lpib_mod;
    uint32_t hw_period;
    int current_buf, next_buf;
    int16_t *next_buffer;

    if (!playing || !stream_started || !hda_base) {
        return;
    }

    cbl = HDA_BUFFER_SIZE * HDA_NUM_BUFFERS;
    if (dma_posbuf_use) {
        uint32_t idx = (uint32_t)output_stream_index * 2u;
        if (idx < (uint32_t)(HDA_MAX_STREAMS * 2)) {
            /* DMA writes -> invalidate cached line before reading. */
            hda_dma_sync_range(&dma_posbuf[idx], (uint32_t)sizeof(uint32_t));
            lpib = dma_posbuf[idx];
        } else {
            lpib = hda_sd_read32(output_stream_index, HDA_SD_LPIB);
        }
    } else {
        lpib = hda_sd_read32(output_stream_index, HDA_SD_LPIB);
    }
    lpib_mod = (cbl != 0) ? (lpib % cbl) : lpib;

    if (!lpib_tracking_valid) {
        lpib_tracking_valid = true;
        lpib_last = lpib_mod;
        lpib_wraps = 0;
        hw_period_last = (HDA_BUFFER_SIZE != 0) ? (lpib_mod / HDA_BUFFER_SIZE) : 0;
    } else {
        /* Detect CBL wrap (LPIB goes backwards). */
        if (lpib_mod < lpib_last) {
            lpib_wraps++;
        }
        lpib_last = lpib_mod;

        hw_period = (HDA_BUFFER_SIZE != 0) ? ((lpib_wraps * cbl + lpib_mod) / HDA_BUFFER_SIZE) : 0;
        if (hw_period == hw_period_last) {
            return;
        }
        hw_period_last = hw_period;
    }

    current_buf =
        (HDA_BUFFER_SIZE != 0) ? ((int)(lpib_mod / HDA_BUFFER_SIZE) % HDA_NUM_BUFFERS) : 0;
    next_buf = (current_buf + 1) % HDA_NUM_BUFFERS;
    next_buffer = audio_buffer + (next_buf * HDA_BUFFER_SIZE) / 2;

    if (user_callback) {
        user_callback(next_buffer, HDA_BUFFER_SIZE);
    } else {
        uint32_t out_ch = (current_hw_channels > 0) ? (uint32_t)current_hw_channels : 1;
        uint32_t frames = HDA_BUFFER_SIZE / (2u * out_ch);
        ring_render_to_hw_buffer(next_buffer, frames);
    }

    /* Ensure the controller sees freshly written samples on real hardware. */
    hda_dma_sync_range(next_buffer, HDA_BUFFER_SIZE);

    hw_fill_count++;
}

/**
 * Enable EAPD on every EAPD-capable pin in the audio function group.
 *
 * Many laptops gate the speaker amplifier behind EAPD on a pin that is not the
 * one chosen as primary output, so EAPD is set on all of them. The speaker pin
 * gets EAPD plus BTL (0x03); other pins get EAPD alone (0x02).
 */
static void hda_enable_eapd_all(void)
{
    uint8_t j;

    if (!audio_fg_nid || afg_widget_count == 0) {
        return;
    }

    for (j = 0; j < afg_widget_count; j++) {
        uint8_t nid = (uint8_t)(afg_widget_start + j);
        uint32_t caps = hda_get_param(nid, HDA_PARAM_AUDIO_CAPS);
        uint8_t wtype = (caps >> 20) & 0xF;

        if (wtype == HDA_WIDGET_PIN) {
            uint32_t pin_caps = hda_get_param(nid, HDA_PARAM_PIN_CAPS);
            if (pin_caps & 0x10000) { /* EAPD capable */
                uint8_t eapd = (nid == speaker_pin_nid) ? 0x03 : 0x02;
                hda_send_verb(codec_addr, nid, HDA_VERB_SET_EAPD | eapd);
            }
        }
    }
}

static uint8_t hda_read8(uint32_t offset)
{
    return *(volatile uint8_t *)(hda_base + offset);
}

static uint16_t hda_read16(uint32_t offset)
{
    return *(volatile uint16_t *)(hda_base + offset);
}

static uint32_t hda_read32(uint32_t offset)
{
    return *(volatile uint32_t *)(hda_base + offset);
}

static void hda_write8(uint32_t offset, uint8_t value)
{
    *(volatile uint8_t *)(hda_base + offset) = value;
}

static void hda_write16(uint32_t offset, uint16_t value)
{
    *(volatile uint16_t *)(hda_base + offset) = value;
}

static void hda_write32(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(hda_base + offset) = value;
}

/* Stream descriptor N lives at MMIO 0x80 + N*0x20. */
static uint32_t hda_sd_offset(int stream)
{
    return 0x80 + (stream * 0x20);
}

static uint32_t hda_sd_read32(int stream, uint32_t reg)
{
    return hda_read32(hda_sd_offset(stream) + reg);
}

static void hda_sd_write32(int stream, uint32_t reg, uint32_t value)
{
    hda_write32(hda_sd_offset(stream) + reg, value);
}

static void hda_sd_write16(int stream, uint32_t reg, uint16_t value)
{
    hda_write16(hda_sd_offset(stream) + reg, value);
}

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

/* Busy-wait roughly `count` microseconds via reads of POST port 0x80. */
static void hda_delay(int count)
{
    int i;
    for (i = 0; i < count; i++) {
        inb(0x80);
    }
}

/* Consecutive non-advancing WALCLK reads after which it is judged dead. A live
 * 24MHz clock changes between reads (one MMIO read is slower than a 41ns tick),
 * so this many identical reads means the wall clock is stuck. */
#define HDA_WALCLK_STUCK_READS 100000u

/**
 * Busy-wait `us` microseconds against the 24MHz HDA wall clock.
 *
 * Unlike a timer-driven sleep this works with interrupts disabled (for
 * example inside a syscall), since it only reads the WALCLK register. A
 * controller whose wall clock never advances (a dead or partially powered
 * HBA) would otherwise spin here forever; if it stops ticking, fall back to
 * the POST-port busy-wait so a syscall caller cannot hang.
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
                hda_delay((int)us); /* wall clock is dead: approximate the delay */
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
 * streams do not respond. Clears the PMCSR power-state bits and waits for the
 * device to wake. A no-op when there is no PM capability or it is already D0.
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

    hda_probe_log("HDA: PCI PMCSR=0x%x (D%d) -> D0\n", pmcsr, state);

    pmcsr &= (uint16_t)~0x3;
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(pmcap + 4), pmcsr);

    /* Give the device time to wake up */
    hda_delay(200000); /* ~200ms worst-case */

    pmcsr = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(pmcap + 4));
    hda_probe_log("HDA: PCI PMCSR now 0x%x (D%d)\n", pmcsr, (int)(pmcsr & 0x3));
}

/**
 * Reset the HDA controller and wait for the codecs to come up.
 *
 * Drives GCTL.CRST low then high, polling the bit on each edge, then waits up
 * to about half a second for STATESTS to report a responding codec. Returns
 * false on any handshake timeout.
 */
static bool hda_reset(void)
{
    int timeout;

    /* Enter reset */
    hda_write32(HDA_GCTL, 0);
    hda_delay(100);

    /* Wait for reset to complete */
    timeout = 10000;
    while ((hda_read32(HDA_GCTL) & HDA_GCTL_CRST) && timeout-- > 0) {
        hda_delay(10);
    }
    if (timeout <= 0) {
        klog(KLOG_WARN, "hda", "reset: the controller never entered it");
        return false;
    }

    /* Exit reset */
    hda_write32(HDA_GCTL, HDA_GCTL_CRST);
    hda_delay(100);

    /* Wait for controller to be ready */
    timeout = 10000;
    while (!(hda_read32(HDA_GCTL) & HDA_GCTL_CRST) && timeout-- > 0) {
        hda_delay(10);
    }
    if (timeout <= 0) {
        klog(KLOG_WARN, "hda", "reset: the controller never left it");
        return false;
    }

    /* Some codecs take 100-500ms to initialize; poll STATESTS until one
     * answers, up to 500ms, then let it settle. */
    {
        int wait_count;
        uint16_t sts;

        for (wait_count = 0; wait_count < 50; wait_count++) {
            hda_delay(10000); /* ~10ms per iteration, 500ms total max */
            sts = hda_read16(HDA_STATESTS);
            if (sts != 0) {
                break;
            }
        }
        hda_delay(50000); /* ~50ms settle */
    }

    return true;
}

/**
 * Initialize the CORB (Command Outbound Ring Buffer) and start it running.
 *
 * Stops the engine, programs the base and a 256-entry size, resets the read
 * pointer through its set/clear handshake, syncs the write pointer to it, then
 * sets CORBCTL.RUN. The codec is normally driven over ICI; this prepares CORB
 * for the controller's benefit during init.
 */
static void hda_setup_corb(void)
{
    uint32_t addr;
    int timeout;

    hda_write8(HDA_CORBCTL, 0);
    hda_delay(100);

    addr = (uint32_t)(uintptr_t)corb_buffer;
    hda_write32(HDA_CORBLBASE, addr);
    hda_write32(HDA_CORBUBASE, 0);

    hda_write8(HDA_CORBSIZE, 0x02); /* 256 entries */

    /* Reset read pointer: set bit 15, wait for the controller to
     * acknowledge. */
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
 * count, then set RIRBCTL.RUN.
 */
static void hda_setup_rirb(void)
{
    uint32_t addr;

    hda_write8(HDA_RIRBCTL, 0);
    hda_delay(100);

    addr = (uint32_t)(uintptr_t)rirb_buffer;
    hda_write32(HDA_RIRBLBASE, addr);
    hda_write32(HDA_RIRBUBASE, 0);

    hda_write8(HDA_RIRBSIZE, 0x02); /* 256 entries */

    /* Reset the write pointer. Bit 15 is write-only and reads back as zero,
     * so there is nothing to wait on; the delay is for the controller to act
     * on it before the pointer is read. */
    hda_write16(HDA_RIRBWP, 0x8000);
    hda_delay(100);

    rirb_rp = hda_read16(HDA_RIRBWP) & 0xFF;

    hda_write16(HDA_RINTCNT, 1); /* interrupt after 1 response */

    hda_write8(HDA_RIRBCTL, 0x02); /* RIRBCTL.RUN */
}

/* Trace the first few verbs to the serial log; counts down to 0. */
static int verb_debug_count = 3;

/**
 * Send a codec verb over the Immediate Command Interface and return the
 * response.
 *
 * ICI is simpler and more reliable than CORB/RIRB for enumeration. The command
 * packs the codec address, node ID, and verb; the routine waits for the
 * interface to go idle, issues it, then polls ICS for IRV (response valid) or a
 * cleared ICB. Returns 0 on a busy, no-response, or timeout condition.
 */
static uint32_t hda_send_verb(uint8_t caddr, uint8_t nid, uint32_t verb)
{
    uint32_t cmd;
    uint16_t ics;
    uint32_t response;
    int timeout;
    bool debug = (verb_debug_count > 0);

    cmd = ((uint32_t)caddr << 28) | ((uint32_t)nid << 20) | (verb & 0xFFFFF);

    timeout = 10000;
    while ((hda_read16(HDA_ICS) & HDA_ICS_ICB) && timeout-- > 0) {
        hda_delay(1);
    }
    if (timeout <= 0) {
        if (debug) {
            klog(KLOG_DEBUG, "hda", "verb: the command interface is busy");
        }
        return 0;
    }

    hda_write16(HDA_ICS, HDA_ICS_IRV); /* IRV is write-1-to-clear */

    hda_write32(HDA_ICW, cmd);

    hda_write16(HDA_ICS, HDA_ICS_ICB); /* ICB starts the command */

    timeout = 100000;
    while (timeout-- > 0) {
        ics = hda_read16(HDA_ICS);
        if (ics & HDA_ICS_IRV) {
            response = hda_read32(HDA_IRR);
            hda_write16(HDA_ICS, HDA_ICS_IRV);
            if (debug) {
                verb_debug_count--;
            }
            return response;
        }
        if (!(ics & HDA_ICS_ICB)) {
            /* ICB cleared without IRV: command done, no response. */
            if (debug) {
                klog(KLOG_DEBUG, "hda", "verb: no response valid, ics 0x%x", ics);
                verb_debug_count--;
            }
            return 0;
        }
        hda_delay(1);
    }

    if (debug) {
        klog(KLOG_WARN, "hda", "verb: timed out, ics 0x%x", hda_read16(HDA_ICS));
        verb_debug_count--;
    }
    return 0;
}

static uint32_t hda_get_param(uint8_t nid, uint8_t param)
{
    return hda_send_verb(codec_addr, nid, HDA_VERB_GET_PARAM | param);
}

/**
 * Read a widget's connection list into `conn_list`, returning the count.
 *
 * Fills up to `max_conn` entries. Handles both short-form (four 8-bit entries
 * per response word) and long-form (two 16-bit entries) connection lists, as
 * indicated by bit 7 of the connection-list-length parameter.
 */
static int hda_get_connections(uint8_t nid, uint8_t *conn_list, int max_conn)
{
    uint32_t conn_param = hda_get_param(nid, HDA_PARAM_CONN_LIST);
    int conn_len = conn_param & 0x7F;
    int is_long = (conn_param >> 7) & 0x1;
    int count = 0;
    int i;

    if (conn_len == 0) {
        return 0;
    }

    for (i = 0; i < conn_len && count < max_conn; i++) {
        uint32_t entry;
        int entries_per_word = is_long ? 2 : 4;
        int word_index = i / entries_per_word;
        int byte_index = i % entries_per_word;

        entry = hda_send_verb(codec_addr, nid, HDA_VERB_GET_CONN_LIST | word_index);

        if (is_long) {
            conn_list[count++] = (entry >> (byte_index * 16)) & 0xFF;
        } else {
            conn_list[count++] = (entry >> (byte_index * 8)) & 0xFF;
        }
    }

    return count;
}

/**
 * Drive the codec's GPIO pins high to enable external amplifiers.
 *
 * Many laptops gate the speaker amplifier behind a codec GPIO, but which bit
 * varies by board. For bring-up this sets every available GPIO as a high
 * output. Does nothing when the function group reports no GPIOs.
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
 * Recognizes Intel HDMI device ID ranges and treats all Nvidia and AMD/ATI
 * codecs as HDMI.
 */
static bool is_hdmi_codec(uint32_t vendor_id)
{
    uint16_t vendor = (vendor_id >> 16) & 0xFFFF;
    uint16_t device = vendor_id & 0xFFFF;

    /* Intel HDMI codecs: vendor 0x8086, device 0x28xx or 0x29xx */
    if (vendor == 0x8086) {
        if ((device >= 0x2800 && device < 0x2a00) || (device >= 0x2900 && device < 0x2b00)) {
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
 * Scans codec addresses for a responding analog codec, and takes a responding
 * HDMI codec only when no analog one answers. Finds its audio function group
 * and classifies its output pins by default-device type (speaker, line out,
 * headphone). It selects an output pin
 * (preferring speaker, then line out, then headphone, then any output-capable
 * pin) and traces the pin's connection list back through any mixer or selector
 * to a DAC. The chosen pin, DAC, optional mixer, and headphone pin/DAC are left
 * in the file-scope routing globals. Returns false if no usable codec, pin, or
 * DAC is found.
 */
static bool hda_find_output(void)
{
    uint16_t statests;
    uint32_t vendor, node_count, subnode_count;
    int i, j;
    uint8_t start_nid, num_nodes;
    uint8_t fg_start, fg_count;
    uint8_t speaker_pin = 0, hp_pin = 0, line_out_pin = 0;
    uint8_t fallback_pin = 0; /* Any output-capable pin as fallback */
    uint8_t first_dac = 0;
    uint8_t conn_list[16];
    int num_conn;
    uint8_t analog_codec_addr = 0xFF; /* Track preferred analog codec */
    uint32_t analog_vendor = 0;

    statests = hda_read16(HDA_STATESTS);
    hda_probe_log("HDA: STATESTS=0x%x\n", statests);
    if (statests == 0) {
        hda_probe_log("HDA: no codec\n");
        return false;
    }

    for (i = 0; i < 15; i++) {
        if (statests & (1 << i)) {
            uint32_t vid;
            int retry;

            /* Temporarily set codec_addr to query this codec */
            codec_addr = i;

            /* Try a few times - codec might need more time */
            vid = 0;
            for (retry = 0; retry < 3 && vid == 0; retry++) {
                if (retry > 0) {
                    hda_delay(50000); /* 50ms between retries */
                }
                vid = hda_send_verb(i, 0, HDA_VERB_GET_PARAM | HDA_PARAM_VENDOR_ID);
            }

            hda_probe_log("HDA: Codec %d vendor=0x%x", i, vid);

            if (vid == 0) {
                hda_probe_log(" (no response, skip)\n");
                continue;
            }

            if (is_hdmi_codec(vid)) {
                hda_probe_log(" (HDMI, skip)\n");
                continue;
            }

            hda_probe_log(" (analog)\n");

            if (analog_codec_addr == 0xFF) {
                analog_codec_addr = i;
                analog_vendor = vid;
            }
        }
    }

    /* No analog codec: accept any responding codec instead. */
    if (analog_codec_addr == 0xFF) {
        for (i = 0; i < 15; i++) {
            if (statests & (1 << i)) {
                uint32_t vid;
                codec_addr = i;
                hda_delay(100000); /* Extra long delay */
                vid = hda_send_verb(i, 0, HDA_VERB_GET_PARAM | HDA_PARAM_VENDOR_ID);
                if (vid != 0) {
                    analog_codec_addr = i;
                    analog_vendor = vid;
                    hda_probe_log("HDA: Fallback to codec %d: 0x%x\n", i, vid);
                    break;
                }
            }
        }
    }

    if (analog_codec_addr == 0xFF) {
        hda_probe_log("HDA: no usable codec\n");
        return false;
    }

    codec_addr = analog_codec_addr;
    vendor = analog_vendor;
    codec_vendor_id = vendor;
    hda_probe_log("HDA: Using codec %d: 0x%x\n", codec_addr, vendor);

    /* Conexant laptop codecs are often picky about jack routing; default to
     * primary output. */
    if (((codec_vendor_id >> 16) & 0xFFFF) == 0x14f1) {
        hda_output_mode = HDA_OUTMODE_PRIMARY;
    } else {
        hda_output_mode = HDA_OUTMODE_AUTO;
    }

    node_count = hda_get_param(0, HDA_PARAM_NODE_COUNT);
    start_nid = (node_count >> 16) & 0xFF;
    num_nodes = node_count & 0xFF;
    hda_probe_log("HDA: Root has %d subnodes starting at 0x%x\n", num_nodes, start_nid);

    for (i = 0; i < num_nodes; i++) {
        uint32_t func_type = hda_get_param(start_nid + i, HDA_PARAM_FUNC_GROUP);
        if ((func_type & 0xFF) == 0x01) { /* function group type 1 = audio */
            audio_fg_nid = start_nid + i;
            break;
        }
    }

    if (audio_fg_nid == 0) {
        hda_probe_log("HDA: No audio function group found\n");
        return false;
    }
    hda_probe_log("HDA: Audio function group at 0x%x\n", audio_fg_nid);

    hda_send_verb(codec_addr, audio_fg_nid, HDA_VERB_SET_POWER | 0x00);
    hda_delay(100);

    hda_enable_gpio();

    subnode_count = hda_get_param(audio_fg_nid, HDA_PARAM_NODE_COUNT);
    fg_start = (subnode_count >> 16) & 0xFF;
    fg_count = subnode_count & 0xFF;
    hda_probe_log("HDA: AFG has %d widgets starting at 0x%x\n", fg_count, fg_start);
    afg_widget_start = fg_start;
    afg_widget_count = fg_count;

    /* First pass: classify every output-capable pin by default device. */
    for (j = 0; j < fg_count; j++) {
        uint8_t nid = fg_start + j;
        uint32_t caps = hda_get_param(nid, HDA_PARAM_AUDIO_CAPS);
        uint8_t widget_type = (caps >> 20) & 0xF;

        if (widget_type == HDA_WIDGET_AUDIO_OUT && first_dac == 0) {
            first_dac = nid;
            hda_probe_log("HDA: Found DAC at 0x%x\n", nid);
        }

        if (widget_type == HDA_WIDGET_PIN) {
            uint32_t pin_caps = hda_get_param(nid, HDA_PARAM_PIN_CAPS);

            if (pin_caps & 0x10) { /* output capable */
                uint32_t pin_cfg = hda_send_verb(codec_addr, nid, HDA_VERB_GET_CONFIG_DEF);
                uint8_t def_dev = HDA_PIN_DEF_DEV(pin_cfg);
                uint8_t port_conn = HDA_PIN_DEF_PORT(pin_cfg);

                hda_probe_log("HDA: Pin 0x%x cfg=0x%x dev=%d port=%d\n", nid, pin_cfg, def_dev,
                              port_conn);

                if (fallback_pin == 0) {
                    fallback_pin = nid;
                }

                /* An unconnected pin can still serve as fallback, but is not
                 * classified by device type. */
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

    /* Select output pin: prefer speaker > line out > headphone > any
     * fallback */
    if (speaker_pin != 0) {
        pin_nid = speaker_pin;
        hda_probe_log("HDA: Selected speaker pin 0x%x\n", pin_nid);
    } else if (line_out_pin != 0) {
        pin_nid = line_out_pin;
        hda_probe_log("HDA: Selected line out pin 0x%x\n", pin_nid);
    } else if (hp_pin != 0) {
        pin_nid = hp_pin;
        hda_probe_log("HDA: Selected HP pin 0x%x\n", pin_nid);
    } else if (fallback_pin != 0) {
        pin_nid = fallback_pin;
        hda_probe_log("HDA: Using fallback pin 0x%x\n", pin_nid);
    }

    /* Also save headphone pin for potential dual output */
    hp_pin_nid = hp_pin;
    speaker_pin_nid = speaker_pin;

    if (pin_nid == 0) {
        hda_probe_log("HDA: no pin\n");
        return false;
    }

    hda_probe_log("HDA: pin=0x%x ", pin_nid);

    /* Trace the pin's connection list back to its DAC. */
    num_conn = hda_get_connections(pin_nid, conn_list, 16);

    if (num_conn > 0) {
        uint8_t pin_sel;
        int pass;

        /* Prefer BIOS/default connection selection first (important on real
         * hardware) */
        pin_sel = (uint8_t)(hda_send_verb(codec_addr, pin_nid, HDA_VERB_GET_CONN_SEL) & 0xFF);
        if (pin_sel >= (uint8_t)num_conn) {
            pin_sel = 0;
        }

        /* Check each connection, but start with the selected one */
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
                uint8_t conn_nid = conn_list[idx];
                uint32_t caps = hda_get_param(conn_nid, HDA_PARAM_AUDIO_CAPS);
                uint8_t wtype = (caps >> 20) & 0xF;

                if (wtype == HDA_WIDGET_AUDIO_OUT) {
                    dac_nid = conn_nid;
                    pin_conn_sel_idx = (uint8_t)idx;
                    hda_probe_log("HDA: Pin connects directly to DAC 0x%x\n", dac_nid);
                    break;
                } else if (wtype == HDA_WIDGET_AUDIO_MIX || wtype == HDA_WIDGET_AUDIO_SEL) {
                    uint8_t mix_conn[8];
                    int mix_num;
                    uint8_t mix_sel;

                    mixer_nid = conn_nid;
                    pin_conn_sel_idx = (uint8_t)idx;
                    hda_probe_log("HDA: Pin connects to mixer/sel 0x%x\n", mixer_nid);

                    mix_num = hda_get_connections(mixer_nid, mix_conn, 8);

                    /* Prefer currently selected source on selector widgets */
                    mix_sel =
                        (uint8_t)(hda_send_verb(codec_addr, mixer_nid, HDA_VERB_GET_CONN_SEL) &
                                  0xFF);
                    if (mix_sel < (uint8_t)mix_num) {
                        uint32_t mcaps0 = hda_get_param(mix_conn[mix_sel], HDA_PARAM_AUDIO_CAPS);
                        if (((mcaps0 >> 20) & 0xF) == HDA_WIDGET_AUDIO_OUT) {
                            dac_nid = mix_conn[mix_sel];
                            hda_probe_log("HDA: Mixer selected DAC 0x%x\n", dac_nid);
                            break;
                        }
                    }

                    for (j = 0; j < mix_num; j++) {
                        uint32_t mcaps = hda_get_param(mix_conn[j], HDA_PARAM_AUDIO_CAPS);
                        if (((mcaps >> 20) & 0xF) == HDA_WIDGET_AUDIO_OUT) {
                            dac_nid = mix_conn[j];
                            hda_probe_log("HDA: Mixer connects to DAC 0x%x\n", dac_nid);
                            break;
                        }
                    }
                    if (dac_nid != 0)
                        break;
                }
            }
        }
    }

    /* Fallback: if no DAC found through tracing, use first DAC */
    if (dac_nid == 0 && first_dac != 0) {
        dac_nid = first_dac;
        hda_probe_log("HDA: Fallback to first DAC 0x%x\n", dac_nid);
    }

    if (dac_nid == 0) {
        hda_probe_log("HDA: no DAC\n");
        return false;
    }

    hda_probe_log("dac=0x%x", dac_nid);
    if (mixer_nid != 0) {
        hda_probe_log(" mix=0x%x", mixer_nid);
    }
    hda_probe_log("\n");

    /* If we have a headphone pin, find its DAC too */
    if (hp_pin_nid != 0 && hp_pin_nid != pin_nid) {
        uint8_t hp_sel;
        uint8_t hp_conn[16];
        int hp_num;

        hp_num = hda_get_connections(hp_pin_nid, hp_conn, 16);
        hp_sel = (uint8_t)(hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_GET_CONN_SEL) & 0xFF);
        if (hp_sel >= (uint8_t)hp_num) {
            hp_sel = 0;
        }

        /* Prefer selected connection first */
        if (hp_num > 0) {
            uint32_t caps0 = hda_get_param(hp_conn[hp_sel], HDA_PARAM_AUDIO_CAPS);
            uint8_t wtype0 = (caps0 >> 20) & 0xF;
            if (wtype0 == HDA_WIDGET_AUDIO_OUT) {
                hp_dac_nid = hp_conn[hp_sel];
            }
            /* A mixer, a selector or anything else keeps the selected
             * upstream widget; the DAC is chosen below when none was
             * found here. */
            hp_conn_sel_idx = hp_sel;
        }

        /* If we didn't get a DAC, pick the first DAC in the list */
        if (hp_dac_nid == 0) {
            for (i = 0; i < hp_num; i++) {
                uint32_t caps = hda_get_param(hp_conn[i], HDA_PARAM_AUDIO_CAPS);
                if (((caps >> 20) & 0xF) == HDA_WIDGET_AUDIO_OUT) {
                    hp_dac_nid = hp_conn[i];
                    hp_conn_sel_idx = (uint8_t)i;
                    break;
                }
            }
        }
    }

    return true;
}

/**
 * Encode a stream format word from sample rate, format, and channel count.
 *
 * Produces the 16-bit value the HDA stream-format register and the codec
 * SET_CONV_FMT verb expect: the channel count less one in the low nibble, a
 * bits-per-sample code, and a base with a divisor.
 *
 * The rates are the six this driver programs: 8000, 11025, 16000, 22050,
 * 44100 and 48000. A request between two of them takes the next one up, and
 * anything above 44100 is played at 48000. The codec is not asked which rates
 * it supports. dev-log/50-shortcomings/plan.md entry 126 has the rest.
 */
static uint16_t hda_calc_format(uint32_t sample_rate, audio_format_t format, int channels)
{
    uint16_t fmt = 0;

    fmt |= (channels - 1) & 0xF;

    if (format == AUDIO_S16LE) {
        fmt |= HDA_FMT_BITS_16;
    } else {
        fmt |= HDA_FMT_BITS_8;
    }

    /* The next programmed rate at or above the request. */
    if (sample_rate <= 8000) {
        fmt |= HDA_FMT_BASE_48K | (0x05 << 8); /* 48000 / 6 = 8000 */
    } else if (sample_rate <= 11025) {
        fmt |= HDA_FMT_BASE_44K | (0x03 << 8); /* 44100 / 4 = 11025 */
    } else if (sample_rate <= 16000) {
        fmt |= HDA_FMT_BASE_48K | (0x02 << 8); /* 48000 / 3 = 16000 */
    } else if (sample_rate <= 22050) {
        fmt |= HDA_FMT_BASE_44K | (0x01 << 8); /* 44100 / 2 = 22050 */
    } else if (sample_rate <= 44100) {
        fmt |= HDA_FMT_BASE_44K; /* 44100 */
    } else {
        fmt |= HDA_FMT_BASE_48K; /* 48000 */
    }

    return fmt;
}

/**
 * HDA interrupt handler: acknowledge the output stream IRQ and refill.
 *
 * Returns immediately if INTSTS does not flag our output stream. Otherwise it
 * clears the stream's status register (write-1-to-clear) and services the
 * stream to refill the next DMA period.
 */
static void hda_irq_handler(isr_params_t *params)
{
    uint32_t intsts;
    uint8_t sd_sts;
    (void)params;

    intsts = hda_read32(HDA_INTSTS);
    if (!(intsts & (1 << output_stream_index))) {
        return; /* Not our interrupt */
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
 * are present. Matches known Intel HDMI device IDs and treats all Nvidia and
 * AMD GPU audio functions as HDMI.
 */
static bool is_hdmi_controller(uint16_t vendor_id, uint16_t device_id)
{
    if (vendor_id == 0x8086) { /* Intel */
        /* Intel HDMI audio device IDs:
         * 0x0a0c, 0x0c0c, 0x0d0c - Haswell
         * 0x160c - Broadwell
         * 0x3198 - Gemini Lake
         * 0x280x, 0x293x - various HDMI */
        if (device_id == 0x0a0c || device_id == 0x0c0c || device_id == 0x0d0c ||
            device_id == 0x160c || device_id == 0x3198 ||
            (device_id >= 0x2800 && device_id < 0x2900) ||
            (device_id >= 0x2930 && device_id < 0x2940)) {
            return true;
        }
    }
    /* Nvidia and AMD GPUs also have HDMI audio, but they have different
     * vendor IDs */
    if (vendor_id == 0x10de || vendor_id == 0x1002) {
        return true; /* Nvidia or AMD GPU audio */
    }
    return false;
}

/**
 * Find, power on, reset, and enumerate the HDA controller.
 *
 * Locates a non-HDMI PCI audio controller (falling back to the first audio
 * function if all are HDMI), maps its BAR0 MMIO, enables bus mastering and
 * forces D0, resets the controller, reads the stream counts from GCAP, brings
 * up CORB/RIRB and the DMA position buffer, discovers the output path, and
 * registers the IRQ handler. Returns 0 on success, -1 on any failure.
 *
 * Any other audio controller the machine has is named in the log and not
 * driven. This kernel takes one, and a person looking at a machine with two
 * needs to see that the other was found and passed over rather than missed.
 */
static int hda_init(void)
{
    uint16_t gcap;
    uint32_t bar0;
    uintptr_t mapped;
    pci_device_t *skip_dev = NULL;
    pci_device_t temp_pci;
    bool found_controller = false;

    k_strncpy(hda_boot_status, "Intel HD Audio", sizeof(hda_boot_status) - 1);
    hda_boot_status[sizeof(hda_boot_status) - 1] = '\0';

    hda_probe_log("\nHDA: init\n");

    while (pci_find_device_skip(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO, skip_dev, &temp_pci)) {
        hda_probe_log("HDA: Found PCI %x:%x", temp_pci.vendor_id, temp_pci.device_id);

        if (is_hdmi_controller(temp_pci.vendor_id, temp_pci.device_id)) {
            hda_probe_log(" (HDMI, skip)\n");
            hda_pci = temp_pci; /* Save for next skip */
            skip_dev = &hda_pci;
            continue;
        }

        hda_probe_log(" (use)\n");
        hda_pci = temp_pci;
        found_controller = true;
        break;
    }

    /* If all controllers were HDMI, try using the first one anyway */
    if (!found_controller) {
        if (!pci_find_device(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO, &hda_pci)) {
            hda_probe_log("HDA: No PCI audio device\n");
            return -1;
        }
        hda_probe_log("HDA: Fallback to %x:%x\n", hda_pci.vendor_id, hda_pci.device_id);
    }

    {
        pci_device_t other;
        pci_device_t seen;
        pci_device_t *from = 0;

        while (pci_find_device_skip(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO, from, &other)) {
            if (other.bus != hda_pci.bus || other.device != hda_pci.device ||
                other.function != hda_pci.function) {
                klog(KLOG_INFO, "audio", "not driven: %x:%x at %02x:%02x.%d", (int)other.vendor_id,
                     (int)other.device_id, (int)other.bus, (int)other.device, (int)other.function);
            }
            seen = other;
            from = &seen;
        }
    }

    hda_probe_log("HDA: Using %x:%x BAR0=", hda_pci.vendor_id, hda_pci.device_id);

    bar0 = pci_bar_address(hda_pci.bar[0]);
    hda_probe_log("0x%x\n", bar0);
    if (bar0 == 0) {
        hda_probe_log("HDA: BAR0 invalid\n");
        return -1;
    }

    hda_probe_log("HDA: Mapping MMIO...");
    mapped = map_mmio(bar0, HDA_MMIO_SIZE);
    if (mapped == 0) {
        hda_probe_log("FAIL\n");
        return -1;
    }
    hda_probe_log("OK\n");

    hda_base = mapped;

    /* Detect CLFLUSH support for DMA coherency (no-ops in emulators). */
    hda_cpu_detect_cache();

    pci_enable_bus_master(&hda_pci);
    pci_enable_memory_space(&hda_pci);
    pci_enable_interrupts(&hda_pci);

    /* Real hardware sometimes boots the controller in D3hot. */
    hda_pci_force_d0(&hda_pci);

    hda_probe_log("HDA: Reset...");
    if (!hda_reset()) {
        hda_probe_log("FAIL\n");
        return -1;
    }
    hda_probe_log("OK\n");

    gcap = hda_read16(HDA_GCAP);
    num_output_streams = (gcap >> 12) & 0xF;
    num_input_streams = (gcap >> 8) & 0xF;
    num_bidir_streams = (gcap >> 3) & 0x1F;
    hda_probe_log("HDA: GCAP=0x%x out=%d\n", gcap, num_output_streams);

    /* Output streams come after input streams in the descriptor array */
    output_stream_index = num_input_streams;

    if (num_output_streams == 0) {
        hda_probe_log("HDA: No output streams\n");
        return -1;
    }

    hda_setup_corb();
    hda_setup_rirb();
    hda_setup_dma_posbuf();

    /* Let the codec settle before issuing commands. */
    hda_delay(50000);

    hda_probe_log("HDA: Find output...");
    if (!hda_find_output()) {
        hda_probe_log("FAIL\n");
        return -1;
    }
    hda_probe_log("OK\n");

    if (hda_pci.irq > 0 && hda_pci.irq < 16) {
        register_isr_handler(32 + hda_pci.irq, hda_irq_handler, "hda");

        /* INTCTL: global interrupt enable plus our output stream's bit. */
        hda_write32(HDA_INTCTL, 0x80000000 | (1 << output_stream_index));
    }

    /*
     * And the same handler on a message vector, where the machine has an
     * APIC and the controller a capability for it. The legacy registration
     * above and the timer's servicing stay exactly as they were: a machine
     * whose messages do not arrive is no worse than it was, and the handler
     * begins by asking the controller whether the interrupt is its own, so
     * being called twice for one event costs a register read.
     */
    if (lapic_ready() && (msi_vector = isr_alloc_vector()) >= 0 &&
        pci_msi_enable(&hda_pci, (uint8_t)msi_vector) == 0) {
        register_isr_handler((uint8_t)msi_vector, hda_irq_handler, "hda");
        hda_write32(HDA_INTCTL, 0x80000000 | (1 << output_stream_index));
        klog(KLOG_INFO, "audio", "interrupts: messages on vector %d", msi_vector);
    }

    hda_build_boot_status();
    hda_probe_log("HDA: Initialization complete\n");
    return 0;
}

/**
 * Stop playback, disable interrupts, and reset the controller.
 */
static void hda_shutdown(void)
{
    if (playing) {
        hda_driver.stop();
    }

    hda_write32(HDA_INTCTL, 0); /* disable all interrupts */
    hda_write32(HDA_GCTL, 0);   /* assert controller reset */

    hda_base = 0;
}

/* The codec dump, which the `audio` shell command prints for a person who
   asked for it: the screen and the port, and neither the log nor the wire. */
static void hda_log(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    k_vprintf(console_putc, format, ap);
    va_end(ap);
}

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

static uint8_t hda_amp_steps(uint32_t amp_caps)
{
    /* Amp caps: bits 14:8 = num steps (0..0x7f). */
    return (uint8_t)((amp_caps >> 8) & 0x7F);
}

static uint8_t hda_get_out_amp_max(uint8_t nid)
{
    return hda_amp_steps(hda_get_param(nid, HDA_PARAM_OUTPUT_AMP));
}

static uint8_t hda_get_in_amp_max(uint8_t nid)
{
    return hda_amp_steps(hda_get_param(nid, 0x0D)); /* Input amp caps */
}

static uint8_t hda_clamp_gain(uint8_t gain, uint8_t max)
{
    if (max != 0 && gain > max) {
        return max;
    }
    return gain;
}

/* Test whether a sample rate's bit is set in a node's PCM capabilities. */
static bool hda_pcm_rate_supported(uint32_t pcm_caps, uint32_t rate)
{
    uint32_t bit = 0;

    switch (rate) {
    case 8000:
        bit = (1u << 0);
        break;
    case 11025:
        bit = (1u << 1);
        break;
    case 16000:
        bit = (1u << 2);
        break;
    case 22050:
        bit = (1u << 3);
        break;
    case 32000:
        bit = (1u << 4);
        break;
    case 44100:
        bit = (1u << 5);
        break;
    case 48000:
        bit = (1u << 6);
        break;
    case 88200:
        bit = (1u << 7);
        break;
    case 96000:
        bit = (1u << 8);
        break;
    case 176400:
        bit = (1u << 9);
        break;
    case 192000:
        bit = (1u << 10);
        break;
    case 384000:
        bit = (1u << 11);
        break;
    default:
        bit = 0;
        break;
    }

    if (bit == 0) {
        return false;
    }
    return (pcm_caps & bit) != 0;
}

/**
 * Choose a hardware sample rate the active DACs support.
 *
 * Intersects the PCM-rate capabilities of the primary and headphone DACs and
 * returns the requested rate if supported. Otherwise it prefers 48000, then
 * 44100, then the highest of 192000, 96000, 32000, 22050, 16000, 11025 and
 * 8000 the codec supports; a rate the codec reports but this list omits,
 * 88200 among them, is not chosen. With nothing supported it returns the
 * request unchanged, which the format encoder then rounds. Many real codecs
 * (including the Conexant part this driver was developed against) do not
 * support 11025Hz even though QEMU does, which is why the write path
 * resamples.
 */
static uint32_t hda_select_hw_rate(uint32_t req_rate)
{
    uint32_t pcm_caps = 0;

    if (!dac_nid) {
        return req_rate ? req_rate : 48000;
    }

    pcm_caps = hda_get_param(dac_nid, 0x0A);
    if (hp_dac_nid) {
        pcm_caps &= hda_get_param(hp_dac_nid, 0x0A);
    }

    if (req_rate != 0 && hda_pcm_rate_supported(pcm_caps, req_rate)) {
        return req_rate;
    }

    /* Prefer 48k when available (common and robust). */
    if (hda_pcm_rate_supported(pcm_caps, 48000)) {
        return 48000;
    }
    if (hda_pcm_rate_supported(pcm_caps, 44100)) {
        return 44100;
    }

    /* The highest of the rates below that the codec supports. */
    if (hda_pcm_rate_supported(pcm_caps, 192000))
        return 192000;
    if (hda_pcm_rate_supported(pcm_caps, 96000))
        return 96000;
    if (hda_pcm_rate_supported(pcm_caps, 32000))
        return 32000;
    if (hda_pcm_rate_supported(pcm_caps, 22050))
        return 22050;
    if (hda_pcm_rate_supported(pcm_caps, 16000))
        return 16000;
    if (hda_pcm_rate_supported(pcm_caps, 11025))
        return 11025;
    if (hda_pcm_rate_supported(pcm_caps, 8000))
        return 8000;

    return req_rate ? req_rate : 48000;
}

/**
 * Print the controller's PCI config, MMIO registers, codec path, and stream
 * state to the kernel and serial log for debugging.
 */
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

            /* PM / MSI / PCIe quick decode */
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
    hda_log("  Audio: req=%dHz fmt=%d ch=%d hw=%dHz fmt=%d ch=%d resample=%d step=0x%x frac=0x%x "
            "written=%d\n",
            current_sample_rate, (int)current_format, current_channels, current_hw_sample_rate,
            (int)current_hw_format, current_hw_channels, resample_enabled ? 1 : 0, resample_step_fp,
            resample_frac_fp, total_bytes_written);
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

/**
 * Print the codec's DAC, mixer, and every pin widget with its capabilities,
 * configuration, and connection list, for debugging.
 */
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
            uint8_t conn_list[16];
            int conn_num;
            int i;

            hda_log("  Pin 0x%x wcap=0x%x caps=0x%x cfg=0x%x dev=%s port=%d ctl=0x%x eapd=0x%x\n",
                    nid, wcap, pin_caps, cfg, hda_pin_def_dev_name(dev), port, pin_ctl, eapd);

            hda_log("    Conn: len=%d long=%d sel=%d\n", (int)(conn_param & 0x7F),
                    (int)((conn_param >> 7) & 0x1), (int)conn_sel);

            conn_num = hda_get_connections(nid, conn_list, 16);
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

/**
 * Print the output and headphone pins' control, EAPD, jack-sense, and selected
 * connection, for debugging routing.
 */
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
    uint8_t conn_list[16];
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
        conn_num = hda_get_connections(pin_nid, conn_list, 16);
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
        conn_num = hda_get_connections(hp_pin_nid, conn_list, 16);
        hda_log("  hp  0x%x: sel=%d ctl=0x%x eapd=0x%x sense=0x%x present=%d\n", hp_pin_nid,
                (int)hp_sel, (uint32_t)hp_ctl, (uint32_t)hp_eapd, hp_sense,
                (hp_sense & 0x80000000u) ? 1 : 0);
        if (conn_num > 0 && hp_sel < (uint8_t)conn_num) {
            hda_log("    sel_nid=0x%x (of %d)\n", conn_list[hp_sel], conn_num);
        }
    }
}

/**
 * Print DMA position, playback counters, underruns, and throughput for the
 * current session, for debugging.
 */
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

/* Fill a buffer with silence: 0 for signed samples, 128 for unsigned U8. */
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
 * Configure the codec and stream and begin playback.
 *
 * Records the requested format and, for the write path, chooses a
 * codec-supported hardware rate and sets up the resampler; the callback path
 * asks the hardware for the requested format. Either way the format encoder
 * has the last word, and it programs one of six rates, so a stream recorded
 * at 96000 runs at 48000 on the wire. Resets the stream, fills the BDL
 * and buffers, programs the stream descriptor, walks the codec path
 * (AFG, DAC, optional mixer, pin, EAPD/GPIO) to route and unmute output,
 * optionally enables the headphone path, then sets the stream's RUN bit and
 * checks that LPIB advances. Returns 0 on success, -1 if the stream reset
 * fails.
 */
static int hda_start(uint32_t sample_rate, audio_format_t format, int channels)
{
    uint16_t fmt;
    uint32_t buf_addr;
    int i;
    int stream = output_stream_index;
    uint32_t wcap;
    audio_format_t hw_format;
    uint32_t hw_rate;
    int hw_channels;
    bool is_conexant = (((codec_vendor_id >> 16) & 0xFFFF) == 0x14f1);
    bool hp_present = false;

    if (playing) {
        hda_driver.stop();
    }

    current_sample_rate = sample_rate;
    current_format = format;
    current_channels = channels;

    /*
     * Decide hardware format.
     *
     * * Callback mode: ask for the requested format, rate and channels.
     * * Write mode (Doom): run hardware at a codec-supported rate (usually 48k)
     *   and convert/resample in the driver. Many real codecs do NOT support
     *   11025Hz even though QEMU does.
     */
    if (user_callback) {
        hw_format = format;
        hw_rate = sample_rate;
        hw_channels = channels;
        resample_enabled = false;
        resample_step_fp = 1u << 16;
        resample_frac_fp = 0;
    } else {
        hw_format = AUDIO_S16LE;
        hw_rate = hda_select_hw_rate(sample_rate);
        /* Upmix to stereo for headphone friendliness. */
        hw_channels = 2;

        /* Fixed-point step: input_rate / hw_rate (frames). */
        {
            uint32_t in_rate = sample_rate ? sample_rate : hw_rate;
            uint32_t hw = hw_rate ? hw_rate : 48000;
            uint32_t whole = (in_rate / hw) << 16;
            uint32_t rem = in_rate % hw;
            uint32_t frac = (rem << 16) / hw;
            resample_step_fp = whole + frac;
            if (resample_step_fp == 0) {
                resample_step_fp = 1;
            }
            resample_frac_fp = 0;
            resample_enabled = (hw_rate != sample_rate);
        }
    }

    current_hw_sample_rate = hw_rate;
    current_hw_format = hw_format;
    current_hw_channels = hw_channels;

    /* Reset per-session counters (keep total_bytes_written monotonic). */
    irq_count = 0;
    underrun_count = 0;
    underrun_samples = 0;
    hw_fill_count = 0;
    session_bytes_written = 0;
    session_bytes_dropped = 0;
    ring_high_water = 0;
    walclk_start = hda_read32(HDA_WALCLK);
    walclk_end = walclk_start;
    out_min_s16 = 32767;
    out_max_s16 = (int16_t)-32768;

    fmt = hda_calc_format(hw_rate ? hw_rate : sample_rate, hw_format, hw_channels);

    if (!hda_stream_reset(stream)) {
        return -1;
    }

    buf_addr = (uint32_t)(uintptr_t)audio_buffer;
    for (i = 0; i < HDA_NUM_BUFFERS; i++) {
        bdl[i].addr_low = buf_addr + (i * HDA_BUFFER_SIZE);
        bdl[i].addr_high = 0;
        bdl[i].length = HDA_BUFFER_SIZE;
        bdl[i].ioc = 1; /* interrupt on completion */
    }

    if (user_callback) {
        for (i = 0; i < HDA_NUM_BUFFERS; i++) {
            user_callback((uint8_t *)audio_buffer + (i * HDA_BUFFER_SIZE), HDA_BUFFER_SIZE);
        }
    } else {
        /* Write path silence is signed 16-bit zeros. */
        fill_silence((uint8_t *)audio_buffer, HDA_BUFFER_SIZE * HDA_NUM_BUFFERS, true);
    }

    /* Push initial samples to RAM for DMA (matters on some laptops). */
    hda_dma_sync_range(audio_buffer, HDA_BUFFER_SIZE * HDA_NUM_BUFFERS);

    hda_sd_write32(stream, HDA_SD_BDPL, (uint32_t)(uintptr_t)bdl);
    hda_sd_write32(stream, HDA_SD_BDPU, 0);
    hda_sd_write32(stream, HDA_SD_CBL, HDA_BUFFER_SIZE * HDA_NUM_BUFFERS);
    hda_sd_write16(stream, HDA_SD_LVI, HDA_NUM_BUFFERS - 1);
    hda_sd_write16(stream, HDA_SD_FORMAT, fmt);

    hda_sd_write32(stream, HDA_SD_CTL, (stream_tag << 20) | HDA_SD_CTL_IOCE);

    /* Configure the codec path in order: AFG power, DAC, [mixer], pin, EAPD. */

    hda_send_verb(codec_addr, audio_fg_nid, HDA_VERB_SET_POWER | 0x00);
    hda_delay(50);

    hda_send_verb(codec_addr, dac_nid, HDA_VERB_SET_POWER | 0x00);
    hda_delay(50);

    hda_send_verb(codec_addr, dac_nid, HDA_VERB_SET_CONV_FMT | fmt);

    hda_send_verb(codec_addr, dac_nid, HDA_VERB_SET_CHANNEL | (stream_tag << 4));

    /* Unmute DAC output amp: 0xB000 = set, output, both L/R; plus gain. */
    wcap = hda_get_param(dac_nid, HDA_PARAM_AUDIO_CAPS);
    if (wcap & 0x04) { /* AC_WCAP_OUT_AMP */
        uint8_t max_gain = hda_get_out_amp_max(dac_nid);
        uint8_t gain = (max_gain != 0) ? max_gain : 0x7F;
        hda_send_verb(codec_addr, dac_nid, HDA_VERB_SET_AMP_GAIN | 0xB000 | gain);
    }

    /*
     * On many laptops, the BIOS/codec may switch a speaker pin between multiple
     * DACs (e.g., internal speaker vs HP) depending on jack state. Configure
     * the secondary DAC too so we still get sound even if routing changes.
     */
    if (hp_dac_nid != 0 && hp_dac_nid != dac_nid) {
        uint32_t hp_wcap;

        hda_send_verb(codec_addr, hp_dac_nid, HDA_VERB_SET_POWER | 0x00);
        hda_delay(20);
        hda_send_verb(codec_addr, hp_dac_nid, HDA_VERB_SET_CONV_FMT | fmt);
        hda_send_verb(codec_addr, hp_dac_nid, HDA_VERB_SET_CHANNEL | (stream_tag << 4));
        hp_wcap = hda_get_param(hp_dac_nid, HDA_PARAM_AUDIO_CAPS);
        if (hp_wcap & 0x04) { /* AC_WCAP_OUT_AMP */
            uint8_t max_gain_out = hda_get_out_amp_max(hp_dac_nid);
            uint8_t gain_out = (max_gain_out != 0) ? max_gain_out : 0x7F;
            hda_send_verb(codec_addr, hp_dac_nid, HDA_VERB_SET_AMP_GAIN | 0xB000 | gain_out);
        }
    }

    if (mixer_nid != 0) {
        uint32_t mix_wcap;
        uint8_t mix_type;
        uint8_t mix_conn_sel = 0;

        hda_send_verb(codec_addr, mixer_nid, HDA_VERB_SET_POWER | 0x00);
        hda_delay(20);

        mix_wcap = hda_get_param(mixer_nid, HDA_PARAM_AUDIO_CAPS);
        mix_type = (mix_wcap >> 20) & 0xF;

        /* If this is a selector widget, ensure it selects the DAC we
         * discovered */
        if (mix_type == HDA_WIDGET_AUDIO_SEL) {
            uint8_t mix_conn[16];
            int mix_num = hda_get_connections(mixer_nid, mix_conn, 16);
            for (i = 0; i < mix_num; i++) {
                if (mix_conn[i] == dac_nid) {
                    mix_conn_sel = (uint8_t)i;
                    break;
                }
            }
            hda_send_verb(codec_addr, mixer_nid, HDA_VERB_SET_CONN_SEL | mix_conn_sel);
        }

        /* Unmute amps on the widget if present */
        if (mix_wcap & 0x02) { /* AC_WCAP_IN_AMP */
            /* If it's a mixer, unmute all input indices; if it's a
             * selector, unmute selected index */
            uint8_t mix_conn[16];
            int mix_num = hda_get_connections(mixer_nid, mix_conn, 16);
            int start = (mix_type == HDA_WIDGET_AUDIO_MIX) ? 0 : (int)mix_conn_sel;
            int end = (mix_type == HDA_WIDGET_AUDIO_MIX) ? mix_num : (start + 1);
            uint8_t max_gain_in = hda_get_in_amp_max(mixer_nid);
            uint8_t gain_in = (max_gain_in != 0) ? max_gain_in : 0x7F;
            for (i = start; i < end; i++) {
                hda_send_verb(codec_addr, mixer_nid,
                              HDA_VERB_SET_AMP_GAIN | 0x7000 | ((uint32_t)i << 8) | gain_in);
            }
        }
        if (mix_wcap & 0x04) { /* AC_WCAP_OUT_AMP */
            uint8_t max_gain_out = hda_get_out_amp_max(mixer_nid);
            uint8_t gain_out = (max_gain_out != 0) ? max_gain_out : 0x7F;
            hda_send_verb(codec_addr, mixer_nid, HDA_VERB_SET_AMP_GAIN | 0xB000 | gain_out);
        }
    }

    if (pin_nid) {
        uint32_t pin_caps = hda_get_param(pin_nid, HDA_PARAM_PIN_CAPS);
        uint8_t pin_conn_sel = pin_conn_sel_idx;
        uint32_t pin_wcap;

        hda_send_verb(codec_addr, pin_nid, HDA_VERB_SET_POWER | 0x00);
        hda_delay(20);

        /* Read current BIOS-selected connection and keep it for Conexant. */
        pin_conn_sel = (uint8_t)(hda_send_verb(codec_addr, pin_nid, HDA_VERB_GET_CONN_SEL) & 0xFF);

        /* Select the discovered source (DAC or mixer) in the pin's
         * connection list */
        {
            uint8_t pin_conn[16];
            int pin_num;
            uint8_t target;

            pin_num = hda_get_connections(pin_nid, pin_conn, 16);
            target = (mixer_nid != 0) ? mixer_nid : dac_nid;

            if (pin_num > 0) {
                if (pin_conn_sel >= (uint8_t)pin_num) {
                    pin_conn_sel = 0;
                }

                /* For Conexant, don't fight BIOS routing; just use current
                 * selection. */
                if (!is_conexant) {
                    if (pin_conn[pin_conn_sel] != target) {
                        pin_conn_sel = 0;
                        for (i = 0; i < pin_num; i++) {
                            if (pin_conn[i] == target) {
                                pin_conn_sel = (uint8_t)i;
                                break;
                            }
                        }
                    }
                }
            } else {
                pin_conn_sel = 0;
            }
        }
        if (!is_conexant) {
            hda_send_verb(codec_addr, pin_nid, HDA_VERB_SET_CONN_SEL | pin_conn_sel);
        }

        /* Enable output with proper pin control
         * 0x40 = OUT_EN, 0xC0 = OUT_EN | HP_EN (for headphones) */
        hda_send_verb(codec_addr, pin_nid, HDA_VERB_SET_PIN_CTRL | 0x40);

        /* Unmute pin amps (some real codecs gate output via AMP-in on the
         * pin) */
        pin_wcap = hda_get_param(pin_nid, HDA_PARAM_AUDIO_CAPS);
        if (pin_wcap & 0x02) { /* AC_WCAP_IN_AMP */
            uint8_t max_gain_in = hda_get_in_amp_max(pin_nid);
            uint8_t gain_in = (max_gain_in != 0) ? max_gain_in : 0x7F;
            hda_send_verb(codec_addr, pin_nid,
                          HDA_VERB_SET_AMP_GAIN | 0x7000 | ((uint32_t)pin_conn_sel << 8) | gain_in);
        }
        if (pin_wcap & 0x04) { /* AC_WCAP_OUT_AMP */
            uint8_t max_gain_out = hda_get_out_amp_max(pin_nid);
            uint8_t gain_out = (max_gain_out != 0) ? max_gain_out : 0x7F;
            uint32_t amp = 0xB000 | gain_out;
            /* Conexant codecs expect the pin OUT amp to be indexed */
            if (((codec_vendor_id >> 16) & 0xFFFF) == 0x14f1) {
                amp |= ((uint32_t)pin_conn_sel << 8);
            }
            hda_send_verb(codec_addr, pin_nid, HDA_VERB_SET_AMP_GAIN | amp);
        }

        /* Many laptops gate internal speaker amp behind EAPD/BTL; enable
         * aggressively. */
        if (pin_nid == speaker_pin_nid) {
            hda_send_verb(codec_addr, pin_nid, HDA_VERB_SET_EAPD | 0x03);
        } else {
            if (pin_caps & 0x10000) {
                hda_send_verb(codec_addr, pin_nid, HDA_VERB_SET_EAPD | 0x02);
            }
        }
    }

    /* Optional headphone output (debug/override). */
    {
        bool have_hp = (hp_pin_nid != 0 && hp_pin_nid != pin_nid && hp_dac_nid != 0);
        int mode = (int)hda_output_mode;
        uint32_t hp_sense = 0;
        bool enable_hp = false;

        hp_present = false;
        if (have_hp) {
            /* Some codecs need the pin powered before pin sense is
             * meaningful. */
            hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_SET_POWER | 0x00);
            hda_delay(5);
            hp_sense = hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_GET_PIN_SENSE);
            hp_present = (hp_sense & 0x80000000u) != 0;
        }

        if (mode == HDA_OUTMODE_AUTO) {
            mode = (have_hp && hp_present) ? HDA_OUTMODE_HP : HDA_OUTMODE_PRIMARY;
        }

        enable_hp = (mode == HDA_OUTMODE_HP || mode == HDA_OUTMODE_BOTH) && have_hp;

        if (mode == HDA_OUTMODE_HP && pin_nid != 0) {
            /* Force primary off when routing to headphones. */
            hda_send_verb(codec_addr, pin_nid, HDA_VERB_SET_PIN_CTRL | 0x00);
        }

        if (enable_hp) {
            uint32_t hp_wcap;
            uint8_t hp_conn_sel = hp_conn_sel_idx;

            hda_send_verb(codec_addr, hp_dac_nid, HDA_VERB_SET_POWER | 0x00);
            hda_send_verb(codec_addr, hp_dac_nid, HDA_VERB_SET_CONV_FMT | fmt);
            hda_send_verb(codec_addr, hp_dac_nid, HDA_VERB_SET_CHANNEL | (stream_tag << 4));
            hp_wcap = hda_get_param(hp_dac_nid, HDA_PARAM_AUDIO_CAPS);
            if (hp_wcap & 0x04) { /* AC_WCAP_OUT_AMP */
                uint8_t max_gain_out = hda_get_out_amp_max(hp_dac_nid);
                uint8_t gain_out = (max_gain_out != 0) ? max_gain_out : 0x7F;
                hda_send_verb(codec_addr, hp_dac_nid, HDA_VERB_SET_AMP_GAIN | 0xB000 | gain_out);
            }

            hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_SET_POWER | 0x00);
            /* Read current BIOS-selected connection and keep it for
             * Conexant. */
            hp_conn_sel =
                (uint8_t)(hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_GET_CONN_SEL) & 0xFF);
            {
                uint8_t hp_conn[16];
                int hp_num;

                hp_num = hda_get_connections(hp_pin_nid, hp_conn, 16);
                if (hp_num > 0) {
                    if (hp_conn_sel >= (uint8_t)hp_num) {
                        hp_conn_sel = 0;
                    }

                    if (!is_conexant) {
                        if (hp_conn[hp_conn_sel] != hp_dac_nid) {
                            hp_conn_sel = 0;
                            for (i = 0; i < hp_num; i++) {
                                if (hp_conn[i] == hp_dac_nid) {
                                    hp_conn_sel = (uint8_t)i;
                                    break;
                                }
                            }
                        }
                    }
                } else {
                    hp_conn_sel = 0;
                }
            }
            if (!is_conexant) {
                hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_SET_CONN_SEL | hp_conn_sel);
            }
            hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_SET_PIN_CTRL | 0xC0); /* OUT+HP */
            hp_wcap = hda_get_param(hp_pin_nid, HDA_PARAM_AUDIO_CAPS);
            if (hp_wcap & 0x02) { /* AC_WCAP_IN_AMP */
                uint8_t max_gain_in = hda_get_in_amp_max(hp_pin_nid);
                uint8_t gain_in = (max_gain_in != 0) ? max_gain_in : 0x7F;
                hda_send_verb(codec_addr, hp_pin_nid,
                              HDA_VERB_SET_AMP_GAIN | 0x7000 | ((uint32_t)hp_conn_sel << 8) |
                                  gain_in);
            }
            if (hp_wcap & 0x04) { /* AC_WCAP_OUT_AMP */
                uint8_t max_gain_out = hda_get_out_amp_max(hp_pin_nid);
                uint8_t gain_out = (max_gain_out != 0) ? max_gain_out : 0x7F;
                uint32_t amp = 0xB000 | gain_out;
                if (((codec_vendor_id >> 16) & 0xFFFF) == 0x14f1) {
                    amp |= ((uint32_t)hp_conn_sel << 8);
                }
                hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_SET_AMP_GAIN | amp);
            }
            /* Headphone outputs typically only need EAPD */
            hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_SET_EAPD | 0x02);
        } else if (have_hp) {
            /* Disable HP pin output to avoid speaker auto-mute / phantom
             * routing. */
            hda_send_verb(codec_addr, hp_pin_nid, HDA_VERB_SET_PIN_CTRL | 0x00);
        }
    }

    /* Re-enable GPIO after all configuration (some codecs reset GPIO state). */
    hda_enable_gpio();
    hda_enable_eapd_all();

    /* Flush caches so the controller sees BDL/buffers on real hardware */
    __asm__ __volatile__("wbinvd" ::: "memory");

    playing = true;
    lpib_tracking_valid = false;
    lpib_last = 0;
    lpib_wraps = 0;
    hw_period_last = 0;

    {
        uint32_t ctl = hda_sd_read32(stream, HDA_SD_CTL);
        ctl |= HDA_SD_CTL_RUN;
        hda_sd_write32(stream, HDA_SD_CTL, ctl);
    }
    stream_started = true;

    /* Quick DMA sanity check: does LPIB advance shortly after RUN? */
    {
        uint32_t lpib0, lpib1;
        uint32_t pos0 = 0, pos1 = 0;
        uint8_t sts0, sts1;

        lpib0 = hda_sd_read32(stream, HDA_SD_LPIB);
        sts0 = hda_read8(hda_sd_offset(stream) + HDA_SD_STS);
        if (dma_posbuf_enabled) {
            uint32_t idx = (uint32_t)stream * 2u;
            if (idx < (uint32_t)(HDA_MAX_STREAMS * 2)) {
                hda_dma_sync_range(&dma_posbuf[idx], (uint32_t)sizeof(uint32_t));
                pos0 = dma_posbuf[idx];
            }
        }
        hda_delay_walclk_us(20000);
        lpib1 = hda_sd_read32(stream, HDA_SD_LPIB);
        sts1 = hda_read8(hda_sd_offset(stream) + HDA_SD_STS);
        if (dma_posbuf_enabled) {
            uint32_t idx = (uint32_t)stream * 2u;
            if (idx < (uint32_t)(HDA_MAX_STREAMS * 2)) {
                hda_dma_sync_range(&dma_posbuf[idx], (uint32_t)sizeof(uint32_t));
                pos1 = dma_posbuf[idx];
            }
        }

        hda_log("  DMA: LPIB %d->%d STS 0x%x->0x%x\n", lpib0, lpib1, (uint32_t)sts0,
                (uint32_t)sts1);
        if (lpib1 == lpib0) {
            hda_log("  DMA: WARNING: LPIB did not move (PCI BM/D0/IOMMU/BDL?)\n");
        }

        /* Decide whether to use the DMA position buffer for pointer
         * reporting. */
        if (dma_posbuf_enabled) {
            if (lpib1 != lpib0 && pos1 == pos0) {
                dma_posbuf_use = false;
                hda_log("  DMA: posbuf %d->%d looks dead; using LPIB\n", pos0, pos1);
            } else {
                dma_posbuf_use = true;
                hda_log("  DMA: posbuf %d->%d OK\n", pos0, pos1);
            }
        } else {
            dma_posbuf_use = false;
        }
    }

    return 0;
}

/**
 * Stop playback: halt the stream, mute the DAC, and reset the ring buffer.
 */
static void hda_stop(void)
{
    int stream = output_stream_index;

    if (!playing) {
        return;
    }

    walclk_end = hda_read32(HDA_WALCLK);

    hda_sd_write8(stream, HDA_SD_CTL, 0);
    hda_delay(100);

    /* 0xB080: output amp, mute bit set. */
    hda_send_verb(codec_addr, dac_nid, HDA_VERB_SET_AMP_GAIN | 0xB080);

    playing = false;
    stream_started = false;
    lpib_tracking_valid = false;
    lpib_last = 0;
    lpib_wraps = 0;
    hw_period_last = 0;

    /* Reset ring buffer for next session */
    ring_read_ptr = 0;
    ring_write_ptr = 0;
}

/**
 * Periodic service hook for systems where HDA stream IRQs are not delivered
 * through the legacy PIC (MSI-only setups). Call from the timer tick.
 */
void hda_tick(void)
{
    hda_service_stream();
}

static bool hda_is_playing(void)
{
    return playing;
}

static void hda_set_callback(audio_callback_t fn)
{
    user_callback = fn;
}

/**
 * Queue PCM samples for playback on the write path.
 *
 * Copies into the ring buffer, dropping any tail that does not fit (see
 * `ring_write`), and updates the byte counters. Returns the number of bytes
 * accepted, or -1 if not playing or given an empty buffer.
 */
static int hda_write(const uint8_t *buffer, size_t size)
{
    uint32_t written;

    if (!buffer || size == 0) {
        return -1;
    }

    if (!playing) {
        return -1;
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
 * sets the amp mute bit (0x80).
 */
static void hda_set_volume(int volume)
{
    uint32_t amp;
    uint8_t mute;

    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;

    mute = (volume == 0) ? 0x80 : 0x00;

    /* DAC output amp */
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

    /* Mixer if present */
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

    /* Pin output amp */
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

    /* HP DAC and pin if present */
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

/* Dispatch table registered with the audio subsystem. */
/* The audio_driver_t fields, in the order audio.h declares them: name,
   type, init, shutdown, start, stop, is_playing, set_callback, write,
   set_volume. */
audio_driver_t hda_driver = {"Intel HD Audio", AUDIO_DRIVER_HDA, hda_init,       hda_shutdown,
                             hda_start,        hda_stop,         hda_is_playing, hda_set_callback,
                             hda_write,        hda_set_volume};
