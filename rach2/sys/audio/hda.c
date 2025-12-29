/*
 * hda.c - Intel HD Audio driver implementation
 *
 * Implements audio playback using Intel HD Audio controllers.
 * Uses CORB/RIRB for codec communication and DMA for streaming.
 */

#include "hda.h"
#include "../kio.h"
#include "../serial.h"
#include "../isr.h"
#include "../lib/kstring.h"
#include "../mm/paging.h"

/* HDA MMIO region size (typically 16KB) */
#define HDA_MMIO_SIZE 0x4000

/* PCI device info */
static pci_device_t hda_pci;
static volatile uint8_t *hda_base = NULL;

/* Controller capabilities */
static uint16_t num_input_streams = 0;
static uint16_t num_output_streams = 0;
static uint16_t num_bidir_streams = 0;

/* Codec info */
static uint8_t codec_addr = 0;
static uint8_t audio_fg_nid = 0;        /* Audio function group node ID */
static uint8_t dac_nid = 0;             /* DAC widget node ID */
static uint8_t pin_nid = 0;             /* Output pin widget node ID */

/* Stream info */
static uint16_t output_stream_index = 0;  /* Set during init based on GCAP */
static uint32_t stream_tag = 1;

/* CORB/RIRB buffers (must be 128-byte aligned, volatile for DMA) */
static volatile uint32_t corb_buffer[HDA_CORB_SIZE] __attribute__((aligned(128)));
static volatile uint64_t rirb_buffer[HDA_RIRB_SIZE] __attribute__((aligned(128)));
static volatile uint16_t corb_wp = 0;
static volatile uint16_t rirb_rp = 0;

/* BDL and audio buffers (volatile for DMA access) */
/* Match SB16's ~15ms per buffer: 160 8-bit samples = 320 bytes at 16-bit */
#define HDA_BUFFER_SIZE     320     /* ~15ms per buffer at 11025 Hz 16-bit mono */
#define HDA_NUM_BUFFERS     2       /* Double-buffered like SB16 */
static volatile hda_bdl_entry_t bdl[HDA_NUM_BUFFERS] __attribute__((aligned(128)));
static volatile uint8_t audio_buffer[HDA_BUFFER_SIZE * HDA_NUM_BUFFERS] __attribute__((aligned(128)));

/*
 * Ring buffer for write-based audio (like Doom)
 * Match SB16's 1280 byte ring buffer (~116ms at 11025 Hz)
 */
#define HDA_RING_SIZE 1280  /* Same as SB16 */
static uint8_t hda_ring[HDA_RING_SIZE];
static volatile uint32_t ring_read_ptr = 0;
static volatile uint32_t ring_write_ptr = 0;

/* Diagnostics */
static volatile uint32_t irq_count = 0;
static volatile uint32_t underrun_count = 0;
static volatile uint32_t underrun_samples = 0;

/*
 * Ring buffer helpers
 */
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
    return to_write;
}

/*
 * Read from ring buffer and convert 8-bit unsigned to 16-bit signed
 * HDA codecs typically work better with 16-bit signed samples
 */
static void ring_read_to_buffer_16bit(int16_t *dest, uint32_t sample_count)
{
    uint32_t avail = ring_available();
    uint32_t to_read = (sample_count < avail) ? sample_count : avail;
    uint32_t silence_needed = sample_count - to_read;
    uint32_t i;
    uint32_t rp = ring_read_ptr;

    for (i = 0; i < to_read; i++) {
        /* Convert 8-bit unsigned (0-255, center=128) to 16-bit signed */
        int16_t sample = ((int16_t)hda_ring[rp] - 128) * 256;
        dest[i] = sample;
        rp = (rp + 1) % HDA_RING_SIZE;
    }

    ring_read_ptr = rp;

    if (silence_needed > 0) {
        underrun_count++;
        underrun_samples += silence_needed;
        for (; i < sample_count; i++) {
            dest[i] = 0;  /* 16-bit signed silence */
        }
    }
}

/* Playback state */
static bool playing = false;
static bool stream_started = false;  /* True once hardware stream is running */
static audio_callback_t user_callback = NULL;
static uint32_t current_sample_rate = 0;
static audio_format_t current_format = AUDIO_S16LE;
static int current_channels = 2;

/*
 * Register access helpers
 */
static uint8_t hda_read8(uint32_t offset)
{
    return hda_base[offset];
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
    hda_base[offset] = value;
}

static void hda_write16(uint32_t offset, uint16_t value)
{
    *(volatile uint16_t *)(hda_base + offset) = value;
}

static void hda_write32(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(hda_base + offset) = value;
}

/*
 * Stream descriptor register access
 */
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

/*
 * Small delay
 */
static void hda_delay(int count)
{
    int i;
    for (i = 0; i < count; i++) {
        inb(0x80);
    }
}

/*
 * Reset the controller
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
        serial_printf("HDA: Reset timeout (enter)\n");
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
        serial_printf("HDA: Reset timeout (exit)\n");
        return false;
    }

    /* Wait for codecs to initialize */
    hda_delay(1000);

    return true;
}

/*
 * Set up CORB (Command Outbound Ring Buffer)
 */
static void hda_setup_corb(void)
{
    uint32_t addr;
    int timeout;

    /* Stop CORB */
    hda_write8(HDA_CORBCTL, 0);
    hda_delay(100);

    /* Set CORB base address */
    addr = (uint32_t)(uintptr_t)corb_buffer;
    hda_write32(HDA_CORBLBASE, addr);
    hda_write32(HDA_CORBUBASE, 0);

    /* Set size (256 entries) */
    hda_write8(HDA_CORBSIZE, 0x02);

    /* Reset read pointer - set bit 15, wait for hardware to acknowledge */
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

    /* Sync write pointer with read pointer */
    corb_wp = hda_read16(HDA_CORBRP) & 0xFF;
    hda_write16(HDA_CORBWP, corb_wp);

    /* Start CORB */
    hda_write8(HDA_CORBCTL, 0x02);  /* Run bit */
}

/*
 * Set up RIRB (Response Inbound Ring Buffer)
 */
static void hda_setup_rirb(void)
{
    uint32_t addr;
    int timeout;

    /* Stop RIRB */
    hda_write8(HDA_RIRBCTL, 0);
    hda_delay(100);

    /* Set RIRB base address */
    addr = (uint32_t)(uintptr_t)rirb_buffer;
    hda_write32(HDA_RIRBLBASE, addr);
    hda_write32(HDA_RIRBUBASE, 0);

    /* Set size (256 entries) */
    hda_write8(HDA_RIRBSIZE, 0x02);

    /* Reset write pointer - set bit 15, wait for hardware to acknowledge */
    hda_write16(HDA_RIRBWP, 0x8000);
    timeout = 1000;
    while ((hda_read16(HDA_RIRBWP) & 0x8000) && timeout-- > 0) {
        hda_delay(10);
    }

    /* Sync software read pointer with hardware write pointer */
    rirb_rp = hda_read16(HDA_RIRBWP) & 0xFF;

    /* Set interrupt count (1 response) */
    hda_write16(HDA_RINTCNT, 1);

    /* Start RIRB */
    hda_write8(HDA_RIRBCTL, 0x02);  /* Run bit */
}

/*
 * Send a verb using Immediate Command Interface (ICI)
 * This is simpler and more reliable than CORB/RIRB for codec enumeration
 */
static uint32_t hda_send_verb(uint8_t caddr, uint8_t nid, uint32_t verb)
{
    uint32_t cmd;
    uint16_t ics;
    int timeout;

    /* Build command */
    cmd = ((uint32_t)caddr << 28) | ((uint32_t)nid << 20) | (verb & 0xFFFFF);

    /* Wait for not busy */
    timeout = 10000;
    while ((hda_read16(HDA_ICS) & HDA_ICS_ICB) && timeout-- > 0) {
        hda_delay(1);
    }
    if (timeout <= 0) {
        serial_printf("HDA: ICI busy timeout\n");
        return 0;
    }

    /* Clear IRV bit by writing 1 to it */
    hda_write16(HDA_ICS, HDA_ICS_IRV);

    /* Write command */
    hda_write32(HDA_ICW, cmd);

    /* Set ICB to start command */
    hda_write16(HDA_ICS, HDA_ICS_ICB);

    /* Wait for response (IRV set or ICB cleared) */
    timeout = 100000;
    while (timeout-- > 0) {
        ics = hda_read16(HDA_ICS);
        if (ics & HDA_ICS_IRV) {
            /* Response is valid, clear IRV and return response */
            hda_write16(HDA_ICS, HDA_ICS_IRV);
            return hda_read32(HDA_IRR);
        }
        if (!(ics & HDA_ICS_ICB)) {
            /* Command completed but no response - might be okay for some verbs */
            break;
        }
        hda_delay(1);
    }

    serial_printf("HDA: Verb timeout (cmd=0x%x, ics=0x%x)\n", cmd, hda_read16(HDA_ICS));
    return 0;
}

/*
 * Get parameter from a codec node
 */
static uint32_t hda_get_param(uint8_t nid, uint8_t param)
{
    return hda_send_verb(codec_addr, nid, HDA_VERB_GET_PARAM | param);
}

/*
 * Find audio codec and output widgets
 */
static bool hda_find_output(void)
{
    uint16_t statests;
    uint32_t vendor, node_count, subnode_count;
    int i, j;
    uint8_t start_nid, num_nodes;
    uint8_t fg_start, fg_count;

    /* Check for attached codecs */
    statests = hda_read16(HDA_STATESTS);
    if (statests == 0) {
        serial_printf("HDA: No codecs detected\n");
        return false;
    }

    /* Find first codec */
    for (i = 0; i < 15; i++) {
        if (statests & (1 << i)) {
            codec_addr = i;
            break;
        }
    }

    /* Get vendor ID */
    vendor = hda_get_param(0, HDA_PARAM_VENDOR_ID);
    (void)vendor;  /* Used for codec identification */

    /* Get subordinate node count from root */
    node_count = hda_get_param(0, HDA_PARAM_NODE_COUNT);
    start_nid = (node_count >> 16) & 0xFF;
    num_nodes = node_count & 0xFF;

    /* Find audio function group */
    for (i = 0; i < num_nodes; i++) {
        uint32_t func_type = hda_get_param(start_nid + i, HDA_PARAM_FUNC_GROUP);
        if ((func_type & 0xFF) == 0x01) {  /* Audio function group */
            audio_fg_nid = start_nid + i;
            break;
        }
    }

    if (audio_fg_nid == 0) {
        serial_printf("HDA: No audio function group found\n");
        return false;
    }

    /* Get widgets in audio function group */
    subnode_count = hda_get_param(audio_fg_nid, HDA_PARAM_NODE_COUNT);
    fg_start = (subnode_count >> 16) & 0xFF;
    fg_count = subnode_count & 0xFF;

    /* Find DAC and output pin */
    for (j = 0; j < fg_count; j++) {
        uint8_t nid = fg_start + j;
        uint32_t caps = hda_get_param(nid, HDA_PARAM_AUDIO_CAPS);
        uint8_t widget_type = (caps >> 20) & 0xF;

        if (widget_type == HDA_WIDGET_AUDIO_OUT && dac_nid == 0) {
            dac_nid = nid;
        }

        if (widget_type == HDA_WIDGET_PIN) {
            uint32_t pin_caps = hda_get_param(nid, HDA_PARAM_PIN_CAPS);
            if (pin_caps & 0x10) {  /* Output capable */
                pin_nid = nid;
            }
        }
    }

    if (dac_nid == 0) {
        serial_printf("HDA: No DAC found\n");
        return false;
    }

    return true;
}

/*
 * Configure stream format based on sample rate/format/channels
 */
static uint16_t hda_calc_format(uint32_t sample_rate, audio_format_t format, int channels)
{
    uint16_t fmt = 0;

    /* Channels */
    fmt |= (channels - 1) & 0xF;

    /* Sample format */
    if (format == AUDIO_S16LE) {
        fmt |= HDA_FMT_BITS_16;
    } else {
        fmt |= HDA_FMT_BITS_8;
    }

    /* Sample rate - find closest base/multiplier/divisor */
    /* Common rates: 8000, 11025, 16000, 22050, 44100, 48000 */
    if (sample_rate <= 8000) {
        fmt |= HDA_FMT_BASE_48K | (0x05 << 8);  /* 48000 / 6 = 8000 */
    } else if (sample_rate <= 11025) {
        fmt |= HDA_FMT_BASE_44K | (0x03 << 8);  /* 44100 / 4 = 11025 */
    } else if (sample_rate <= 16000) {
        fmt |= HDA_FMT_BASE_48K | (0x02 << 8);  /* 48000 / 3 = 16000 */
    } else if (sample_rate <= 22050) {
        fmt |= HDA_FMT_BASE_44K | (0x01 << 8);  /* 44100 / 2 = 22050 */
    } else if (sample_rate <= 44100) {
        fmt |= HDA_FMT_BASE_44K;                 /* 44100 */
    } else {
        fmt |= HDA_FMT_BASE_48K;                 /* 48000 */
    }

    return fmt;
}

/*
 * IRQ handler for HDA
 */
static void hda_irq_handler(isr_params_t *params)
{
    uint32_t intsts;
    uint8_t sd_sts;
    volatile uint8_t *next_buffer;
    uint32_t lpib;
    int current_buf, next_buf;
    (void)params;

    intsts = hda_read32(HDA_INTSTS);
    if (!(intsts & (1 << output_stream_index))) {
        return;  /* Not our interrupt */
    }

    /* Clear stream status */
    sd_sts = hda_read8(hda_sd_offset(output_stream_index) + HDA_SD_STS);
    hda_write8(hda_sd_offset(output_stream_index) + HDA_SD_STS, sd_sts);

    irq_count++;

    if (!playing || !stream_started) {
        return;
    }

    /* Determine which buffer to fill next */
    lpib = hda_sd_read32(output_stream_index, HDA_SD_LPIB);
    current_buf = (lpib / HDA_BUFFER_SIZE) % HDA_NUM_BUFFERS;
    next_buf = (current_buf + 1) % HDA_NUM_BUFFERS;
    next_buffer = audio_buffer + (next_buf * HDA_BUFFER_SIZE);

    /* Fill buffer with new data */
    if (user_callback) {
        user_callback((uint8_t *)next_buffer, HDA_BUFFER_SIZE);
    } else {
        /* Use ring buffer - convert 8-bit to 16-bit */
        /* HDA_BUFFER_SIZE bytes of 16-bit = HDA_BUFFER_SIZE/2 samples */
        ring_read_to_buffer_16bit((int16_t *)next_buffer, HDA_BUFFER_SIZE / 2);
    }
}

/*
 * Initialize HDA driver
 */
static int hda_init(void)
{
    uint16_t gcap;
    uint32_t bar0;

    /* Find HDA controller on PCI */
    if (!pci_find_device(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO, &hda_pci)) {
        return -1;
    }

    /* Get MMIO base from BAR0 */
    bar0 = pci_bar_address(hda_pci.bar[0]);
    if (bar0 == 0) {
        serial_printf("HDA: Invalid BAR0 address\n");
        return -1;
    }

    /* Map MMIO region if above identity-mapped memory */
    if (bar0 >= HARD_MEMORY_LIMIT) {
        if (map_mmio(bar0, HDA_MMIO_SIZE) == 0) {
            serial_printf("HDA: Failed to map MMIO at 0x%x\n", bar0);
            return -1;
        }
    }

    hda_base = (volatile uint8_t *)(uintptr_t)bar0;

    /* Enable PCI bus mastering, memory space, and interrupts */
    pci_enable_bus_master(&hda_pci);
    pci_enable_memory_space(&hda_pci);
    pci_enable_interrupts(&hda_pci);

    /* Reset controller */
    if (!hda_reset()) {
        return -1;
    }

    /* Read capabilities */
    gcap = hda_read16(HDA_GCAP);
    num_output_streams = (gcap >> 12) & 0xF;
    num_input_streams = (gcap >> 8) & 0xF;
    num_bidir_streams = (gcap >> 3) & 0x1F;

    /* Output streams come after input streams in the descriptor array */
    output_stream_index = num_input_streams;

    if (num_output_streams == 0) {
        serial_printf("HDA: No output streams available\n");
        return -1;
    }

    /* Set up CORB/RIRB */
    hda_setup_corb();
    hda_setup_rirb();

    /* Find audio output path */
    if (!hda_find_output()) {
        return -1;
    }

    /* Register IRQ handler */
    if (hda_pci.irq > 0 && hda_pci.irq < 16) {
        register_isr_handler(32 + hda_pci.irq, hda_irq_handler);

        /* Enable interrupts */
        hda_write32(HDA_INTCTL, 0x80000000 | (1 << output_stream_index));
    }

    return 0;
}

/*
 * Shutdown HDA driver
 */
static void hda_shutdown(void)
{
    if (playing) {
        hda_driver.stop();
    }

    /* Disable interrupts */
    hda_write32(HDA_INTCTL, 0);

    /* Reset controller */
    hda_write32(HDA_GCTL, 0);

    hda_base = NULL;
}

/*
 * Fill buffer with silence
 */
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

/*
 * Start audio playback
 */
static int hda_start(uint32_t sample_rate, audio_format_t format, int channels)
{
    uint16_t fmt;
    uint32_t buf_addr;
    int i;
    int stream = output_stream_index;
    audio_format_t hw_format;

    if (playing) {
        hda_driver.stop();
    }

    current_sample_rate = sample_rate;
    current_format = format;
    current_channels = channels;

    /* For write-based audio (no callback), always use 16-bit signed
     * because HDA codecs work better with 16-bit samples.
     * The driver will convert 8-bit to 16-bit in the IRQ handler. */
    hw_format = (user_callback == NULL) ? AUDIO_S16LE : format;

    /* Calculate stream format */
    fmt = hda_calc_format(sample_rate, hw_format, channels);

    /* Reset stream */
    hda_sd_write8(stream, HDA_SD_CTL, HDA_SD_CTL_SRST);
    hda_delay(100);
    hda_sd_write8(stream, HDA_SD_CTL, 0);
    hda_delay(100);

    /* Set up BDL */
    buf_addr = (uint32_t)(uintptr_t)audio_buffer;
    for (i = 0; i < HDA_NUM_BUFFERS; i++) {
        bdl[i].addr_low = buf_addr + (i * HDA_BUFFER_SIZE);
        bdl[i].addr_high = 0;
        bdl[i].length = HDA_BUFFER_SIZE;
        bdl[i].ioc = 1;  /* Interrupt on completion */
    }

    /* Fill buffers with initial data or silence */
    if (user_callback) {
        for (i = 0; i < HDA_NUM_BUFFERS; i++) {
            user_callback((uint8_t *)(audio_buffer + (i * HDA_BUFFER_SIZE)), HDA_BUFFER_SIZE);
        }
    } else {
        /* For write-based audio, use 16-bit signed silence (zeros) */
        fill_silence(audio_buffer, HDA_BUFFER_SIZE * HDA_NUM_BUFFERS, true);
    }

    /* Configure stream */
    hda_sd_write32(stream, HDA_SD_BDPL, (uint32_t)(uintptr_t)bdl);
    hda_sd_write32(stream, HDA_SD_BDPU, 0);
    hda_sd_write32(stream, HDA_SD_CBL, HDA_BUFFER_SIZE * HDA_NUM_BUFFERS);
    hda_sd_write16(stream, HDA_SD_LVI, HDA_NUM_BUFFERS - 1);
    hda_sd_write16(stream, HDA_SD_FORMAT, fmt);

    /* Set stream tag */
    hda_sd_write32(stream, HDA_SD_CTL,
                   (stream_tag << 20) | HDA_SD_CTL_IOCE);

    /* Configure codec */
    /* Power on DAC */
    hda_send_verb(codec_addr, dac_nid, HDA_VERB_SET_POWER | 0x00);
    hda_delay(100);

    /* Set stream format on DAC */
    hda_send_verb(codec_addr, dac_nid, HDA_VERB_SET_CONV_FMT | fmt);

    /* Set stream/channel on DAC */
    hda_send_verb(codec_addr, dac_nid, HDA_VERB_SET_CHANNEL | (stream_tag << 4));

    /* Enable output pin if found */
    if (pin_nid) {
        hda_send_verb(codec_addr, pin_nid, HDA_VERB_SET_PIN_CTRL | 0x40);
        /* Set EAPD if supported */
        hda_send_verb(codec_addr, pin_nid, HDA_VERB_SET_EAPD | 0x02);
    }

    /* Unmute DAC */
    hda_send_verb(codec_addr, dac_nid, HDA_VERB_SET_AMP_GAIN | 0xB000 | 0x7F);

    playing = true;

    /* Start stream immediately (like SB16) */
    hda_sd_write8(stream, HDA_SD_CTL + 0,
                  hda_read8(hda_sd_offset(stream) + HDA_SD_CTL) | HDA_SD_CTL_RUN);
    stream_started = true;

    return 0;
}

/*
 * Stop audio playback
 */
static void hda_stop(void)
{
    int stream = output_stream_index;

    if (!playing) {
        return;
    }

    /* Stop stream */
    hda_sd_write8(stream, HDA_SD_CTL, 0);
    hda_delay(100);

    /* Mute DAC */
    hda_send_verb(codec_addr, dac_nid, HDA_VERB_SET_AMP_GAIN | 0xB080);

    playing = false;
    stream_started = false;

    /* Reset ring buffer and counters for next session */
    ring_read_ptr = 0;
    ring_write_ptr = 0;
    irq_count = 0;
    underrun_count = 0;
    underrun_samples = 0;
}

/*
 * Check if playing
 */
static bool hda_is_playing(void)
{
    return playing;
}

/*
 * Set callback
 */
static void hda_set_callback(audio_callback_t fn)
{
    user_callback = fn;
}

/*
 * Write samples
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

    /* Write samples to ring buffer */
    written = ring_write(buffer, (uint32_t)size);

    return (int)written;
}

/*
 * Set volume
 */
static void hda_set_volume(int volume)
{
    uint8_t gain;

    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    /* Convert to 7-bit gain (0-127) */
    gain = (uint8_t)((volume * 127) / 100);

    /* Set amplifier gain on DAC (output, left+right) */
    hda_send_verb(codec_addr, dac_nid, HDA_VERB_SET_AMP_GAIN | 0xB000 | gain);
}

/*
 * HDA audio driver definition
 */
audio_driver_t hda_driver = {
    .name = "Intel HD Audio",
    .type = AUDIO_DRIVER_HDA,
    .init = hda_init,
    .shutdown = hda_shutdown,
    .start = hda_start,
    .stop = hda_stop,
    .is_playing = hda_is_playing,
    .set_callback = hda_set_callback,
    .write = hda_write,
    .set_volume = hda_set_volume
};
