/*
 * hda_hdmi.c: Haswell's display audio, the HD Audio controller 8086:0a0c,
 * 0c0c or 0d0c and its HDMI codec, which plays on the monitors of the Intel
 * display driver's HDMI and DisplayPort outputs, as Linux 6.12.115's
 * snd_hda_intel (sound/pci/hda/hda_intel.c) and patch_hdmi.c, bound to i915
 * through the audio component (acomp.h).
 *
 * The Linux 6.12.115 functions it follows, by file and line: the controller,
 * azx_init_pci (hda_intel.c:380) and snd_hdac_i915_set_bclk
 * (sound/hda/hdac_i915.c:34); the codec's parse, hdmi_parse_codec,
 * hdmi_add_pin and hdmi_add_cvt (patch_hdmi.c:2017, 1884, 1963), with
 * intel_hsw_common_init (3111) and intel_haswell_enable_all_pins (2760);
 * the pins, hdmi_init_pin (474) and snd_hda_set_dev_select (hda_codec.c:391);
 * the ELD, sync_eld_via_acomp (patch_hdmi.c:1827), update_eld (1509),
 * intel_pin_eld_notify (2878) and snd_hdmi_parse_eld (hda_eld.c:224); the
 * stream, hdmi_pcm_open (patch_hdmi.c:1249), hdmi_pcm_setup_pin (1438),
 * i915_pin_cvt_fixup (2974), intel_not_share_assigned_cvt (1097),
 * haswell_verify_D0 (892), generic_hdmi_playback_pcm_prepare (2096),
 * hdmi_setup_stream (956), hdmi_setup_audio_infoframe (747),
 * hdmi_infoframe_uptodate (661), hdmi_std_setup_channel_mapping and
 * hdmi_set_channel_count (sound/hda/hdmi_chmap.c:339, 204); and
 * /proc/asound, snd_card_info_read (sound/core/init.c:935), snd_card_id_read
 * (sound/core/info.c:467), the eld#C.N entry (patch_hdmi.c:530) and
 * snd_hdmi_print_eld_info (hda_eld.c:442).
 */

#include "hda_hdmi.h"
#include "acomp.h"
#include "dsp.h"
#include "hda.h"
#include "../arch/arch.h"
#include "../fs/devfs.h"
#include "../fs/file.h"
#include "../klog.h"
#include "../pci.h"
#include "../rtc.h"
#include "../timer.h"
#include "../udelay.h"
#include "../lib/kerrno.h"
#include "../lib/spinlock.h"
#include "../lib/kstring.h"
#include "../mm/paging.h"
#include "../proc/mutex.h"
#include "../proc/percpu.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/wait_queue.h"

#define HH_MMIO_SIZE 0x4000
#define HH_EM4       0x100c /* AZX_REG_HSW_EM4: the BCLK's M */
#define HH_EM5       0x1010 /* AZX_REG_HSW_EM5: its N */

/* PCI configuration: TCSEL, whose traffic class Linux clears, and the SCH
 * snoop control, whose no-snoop bit it clears to snoop
 * (hda_intel.c:384-431). */
#define HH_PCI_TCSEL    0x44
#define HH_PCI_DEVC     0x78
#define HH_DEVC_NOSNOOP (1u << 11)

/* Codec verbs (include/sound/hda_verbs.h); those of 4 bits carry 16 bits. */
#define V_SET_FORMAT    0x2
#define V_SET_AMP       0x3
#define V_GET_PARAM     0xf00
#define V_GET_CONN_SEL  0xf01
#define V_GET_CONN_LIST 0xf02
#define V_GET_POWER     0xf05
#define V_GET_DIP_DATA  0xf31
#define V_GET_DIP_XMIT  0xf32
#define V_GET_CONFIG    0xf1c
#define V_SET_CONN_SEL  0x701
#define V_SET_POWER     0x705
#define V_SET_STREAMID  0x706
#define V_SET_PIN_CTL   0x707
#define V_SET_DIGI1     0x70d
#define V_SET_DIGI2     0x70e
#define V_SET_CHAN_CNT  0x72d
#define V_SET_DIP_INDEX 0x730
#define V_SET_DIP_DATA  0x731
#define V_SET_DIP_XMIT  0x732
#define V_SET_CHAN_SLOT 0x734
#define V_SET_DEV_SEL   0x735
#define V_SET_DIGI3     0x73e
#define V_INTEL_GET     0xf81 /* INTEL_GET_VENDOR_VERB */
#define V_INTEL_SET     0x781 /* INTEL_SET_VENDOR_VERB */

#define P_VENDOR   0x00
#define P_SUBNODES 0x04
#define P_FG_TYPE  0x05
#define P_WCAPS    0x09
#define P_PCM      0x0a
#define P_PINCAP   0x0c
#define P_CONN_LEN 0x0e
#define P_DEVLIST  0x15

#define WCAP_STEREO           (1u << 0)
#define WCAP_OUT_AMP          (1u << 2)
#define WCAP_DIGITAL          (1u << 9)
#define WCAP_POWER            (1u << 10)
#define WCAP_TYPE(w)          (((w) >> 20) & 0xf)
#define WID_AUD_OUT           0x0
#define WID_PIN               0x4
#define PINCAP_HDMI           (1u << 7)
#define PINCAP_DP             (1u << 24)
#define AMP_OUT_UNMUTE        0xb000
#define PIN_OUT               0x40
#define DIPXMIT_BEST          0xc0
#define INTEL_EN_ALL_PIN_CVTS 0x01
#define INTEL_EN_DP12         0x02
#define DIG1_ENABLE           0x01

/* The Intel codec's vendor widget, and its device entries on each pin,
 * patch_i915_hsw_hdmi's intel_hsw_common_init(codec, 0x08, NULL, 0, 3). */
#define HH_VENDOR_NID 0x08
#define HH_DEV_NUM    3
#define HH_MAX_CVTS   4
#define HH_MAX_PINS   (3 * HH_DEV_NUM)
#define HH_MAX_MUX    4

/* The ELD: its fixed bytes and longest monitor name (hda_local.h), and the
 * bytes the display driver gives at most (drm.h's DRM_ELD_MAX). */
#define ELD_FIXED_BYTES 20
#define ELD_MAX_MNL     16
#define ELD_MAX_SAD     16
#define HH_ELD_MAX      128

/* The stream: its tag on the link, the DMA buffer's periods at most and in
 * bytes, and the audio it holds at least, so that the tick, which refills
 * it, has time to come round. A period is a multiple of 128 bytes, the
 * alignment of a descriptor's buffer. */
#define HH_TAG        1
#define HH_BDL_MAX    256
#define HH_PERIOD_MAX 16384u
#define HH_DMA_MAX    (3u * HH_PERIOD_MAX)
#define HH_BUFFER_MS  40u

/* The stream's rates, as OSS's /dev/dsp takes them (dsp.c's DSP_RATE_MIN
 * and DSP_RATE_MAX); the most bytes of the stream the caller's buffer keeps
 * for the driver's, dsp.c's DSP_BUFFER_MAX; and the stream's frames a
 * conversion takes into hh_stage at once, for at most HH_STAGE_FRAMES of
 * the DMA's: six for each at the largest ratio, 192 kHz to the 32 kHz
 * every sink plays, and two more for the interpolation. */
#define HH_RATE_MIN     1000u
#define HH_RATE_MAX     192000u
#define HH_STREAM_MAX   65536u
#define HH_STAGE_FRAMES 256u
#define HH_STAGE_BYTES  ((HH_STAGE_FRAMES * 6u + 2u) * 8u)

/* The longest gap between two refills whose whole laps of the ring are
 * counted; a longer one counts as this. */
#define HH_LAP_TICKS (5u * (uint32_t)TICK_HZ)

/** One short audio descriptor of an ELD, Linux's struct cea_sad. */
struct hh_sad {
    int format; /* the CEA coding type, with the extended types after 15 */
    int channels;
    int rates; /* SNDRV_PCM_RATE_* bits */
    int bits;  /* AC_SUPPCM_BITS_* bits, of LPCM */
    int max_bitrate;
    int profile;
};

/** An ELD parsed, Linux's struct parsed_hdmi_eld. */
struct hh_eld_info {
    int eld_ver, baseline_len, cea_edid_ver;
    int support_hdcp, support_ai, conn_type;
    int sad_count, aud_synch_delay, spk_alloc;
    uint64_t port_id;
    int manufacture_id, product_id;
    char monitor_name[ELD_MAX_MNL + 1];
    struct hh_sad sad[ELD_MAX_SAD];
};

/** A converter, Linux's hdmi_spec_per_cvt. */
struct hh_cvt {
    uint8_t nid;
    uint32_t pcm; /* its PCM sizes and rates parameter */
    int assigned;
};

/**
 * A device entry of a pin, Linux's hdmi_spec_per_pin: the pin's `nid`, the
 * entry `dev`, the converters it can select, the one it selects for the
 * stream, and the sink's ELD the display driver last gave for it.
 */
struct hh_pin {
    uint8_t nid;
    uint8_t dev;
    uint8_t mux[HH_MAX_MUX];
    int nmux;
    int mux_idx;
    uint8_t cvt; /* the converter it plays, 0 while it plays none */
    int setup;   /* the stream was prepared on it */
    int monitor_present;
    int eld_valid;
    int eld_size;
    uint8_t eld[HH_ELD_MAX];
    struct hh_eld_info info;
};

/*
 * The driver's state. `mutex` orders the verbs, the stream's operations
 * and the component's calls, as Linux's pcm_lock; it comes after the audio
 * component's lock and before the display's audio mutex. `slock` guards
 * what the tick's refill reads, from `running` on.
 */
static pci_device_t hh_pci;
static int hh_found;
static uintptr_t hh_base;
static struct mutex hh_mutex;
static struct spinlock hh_slock = SPINLOCK_INIT_NAMED("hdmi_audio");
static const struct acomp_ops *hh_ops;
static void *hh_dev;
static int hh_present;
static int hh_parsed;
static uint32_t hh_power;
static int hh_caddr;
static uint32_t hh_vendor;
static uint8_t hh_afg;
static uint8_t hh_nid_start;
static int hh_nids;
static int hh_out_sd;
static struct hh_cvt hh_cvts[HH_MAX_CVTS];
static int hh_ncvts;
static struct hh_pin hh_pins[HH_MAX_PINS];
static int hh_npins;
static uint16_t hh_corb_wp;
static uint16_t hh_rirb_rp;
static int hh_verb_warned;

static volatile uint32_t hh_corb[256] __attribute__((aligned(128)));
static volatile uint64_t hh_rirb[256] __attribute__((aligned(128)));
static volatile hda_bdl_entry_t hh_bdl[HH_BDL_MAX] __attribute__((aligned(128)));
static uint8_t hh_dma[HH_DMA_MAX] __attribute__((aligned(128)));

/* The stream: its owner from open to close, whether it holds the display's
 * power, which the display's going ends, the pin entry it plays on (-1 for
 * none), the converter, and the converter's rates and widths that the sink
 * allows. */
static audio_stream_t *hh_stream;
static int hh_held;
static int hh_bound;
static int hh_cvt_idx;
static uint32_t hh_rates; /* bits of the PCM parameter's rates */
static int hh_wide;       /* the sink and converter take 24 bits */

/* What `stream_setup` granted: the DMA's format word and rate, its period
 * and frame in bytes and its periods, the bytes of a frame of the stream,
 * and whether the stream's frames are the DMA's, which then copies them. */
static uint16_t hh_fmt;
static uint32_t hh_rate;
static uint32_t hh_pb;
static uint32_t hh_n;
static uint32_t hh_frame; /* 4, two 16-bit samples, or 8, two 24 in 32 */
static uint32_t hh_sframe;
static int hh_copy;

/*
 * The conversion of the stream's frames to the DMA's, as Linux's OSS
 * layer converts with its plugins (sound/core/oss/): each sample widened to
 * 24 bits, a mono one to both channels, and the rate changed by linear
 * interpolation, as its rate plugin does. The next DMA frame lies `hh_acc`
 * / hh_rate of a stream frame past `hh_prev`, between it and `hh_cur`;
 * at hh_rate or more, the next stream frame is owed first.
 */
static uint32_t hh_acc;
static int32_t hh_prev[2], hh_cur[2];
static uint8_t hh_stage[HH_STAGE_BYTES];

/*
 * Under hh_slock: whether the DMA runs; the period the refill reaches
 * next; the stream's frames taken to fill each period, those taken since
 * the start, and those of the periods played; the position last given,
 * which never goes back, and the one the last `period` call marked; and
 * the tick and the DMA's place in bytes at the last refill.
 */
static volatile int hh_running;
static uint32_t hh_next;
static uint32_t hh_in[HH_BDL_MAX];
static uint32_t hh_taken;
static uint32_t hh_played;
static uint32_t hh_pos;
static uint32_t hh_mark;
static uint64_t hh_last_tick;
static uint32_t hh_last_lpib;

/* The rates of the PCM parameter's bits 0 to 11, with their stream format
 * words, as hda.c's hda_rates. */
static const uint32_t hh_rate_hz[12] = {8000,  11025, 16000, 22050,  32000,  44100,
                                        48000, 88200, 96000, 176400, 192000, 384000};
static const uint16_t hh_rate_fmt[12] = {0x0500, 0x4300, 0x0200, 0x4100, 0x0a00, 0x4000,
                                         0x0000, 0x4800, 0x0800, 0x5800, 0x1800, 0x3800};

/* The rates of ALSA's SNDRV_PCM_RATE_* bits 0 to 13, as hda_eld.c's
 * hdmi_print_pcm_rates lists them. */
static const uint32_t alsa_rates[14] = {5512,  8000,  11025, 16000, 22050,  32000,  44100,
                                        48000, 64000, 88200, 96000, 176400, 192000, 384000};

/* SNDRV_PCM_RATE_* of the CEA sampling frequencies 1 to 7 (hda_eld.c). */
static const int cea_rates[7] = {1 << 5, 1 << 6, 1 << 7, 1 << 9, 1 << 10, 1 << 11, 1 << 12};

#ifdef RACH2_HOSTTEST
extern uint32_t hdmi_test_read(uint32_t offset, int bits);
extern void hdmi_test_write(uint32_t offset, uint32_t value, int bits);
extern uint32_t hdmi_test_verb(uint32_t cmd);
#endif

/** Read the controller's register at `offset`, `bits` wide. */
static uint32_t hh_read(uint32_t offset, int bits)
{
#ifdef RACH2_HOSTTEST
    return hdmi_test_read(offset, bits);
#else
    if (bits == 8) {
        return mmio_read8(hh_base + offset);
    }
    if (bits == 16) {
        return mmio_read16(hh_base + offset);
    }
    return mmio_read32(hh_base + offset);
#endif
}

/** Write `value` to the controller's register at `offset`, `bits` wide. */
static void hh_write(uint32_t offset, uint32_t value, int bits)
{
#ifdef RACH2_HOSTTEST
    hdmi_test_write(offset, value, bits);
#else
    if (bits == 8) {
        mmio_write8(hh_base + offset, (uint8_t)value);
    } else if (bits == 16) {
        mmio_write16(hh_base + offset, (uint16_t)value);
    } else {
        mmio_write32(hh_base + offset, value);
    }
#endif
}

/** The offset of the output stream descriptor's register `reg`. */
static uint32_t hh_sd(uint32_t reg)
{
    return 0x80u + 0x20u * (uint32_t)hh_out_sd + reg;
}

/** The high half of a physical address. */
static uint32_t hh_hi(uint64_t phys)
{
    return (uint32_t)(phys >> 32);
}

/** A condition of `hh_msleep`'s wait: the tick it waits for has come. */
static int hh_deadline(void *arg)
{
    return rtc_ticks_now() >= *(const uint64_t *)arg;
}

/** Wait `ms` milliseconds at least, Linux's msleep: a process sleeps,
 * giving up the big lock, and anything else spins. */
static void hh_msleep(uint32_t ms)
{
#ifdef RACH2_HOSTTEST
    (void)ms;
#else
    static wait_queue_t sleepers;
    static int ready;
    uint64_t due, saved;

    if (!current_proc || !sched_is_enabled()) {
        k_udelay(ms * 1000u);
        return;
    }
    if (!ready) {
        wait_queue_init(&sleepers);
        ready = 1;
    }
    due = rtc_ticks_now() + (ms * (uint32_t)TICK_HZ + 999u) / 1000u + 1u;
    saved = current_proc->wakeup_tick;
    current_proc->wakeup_tick = due;
    (void)wq_wait_event_nointr(&sleepers, hh_deadline, &due);
    current_proc->wakeup_tick = saved;
#endif
}

/* ---- the link ---- */

/**
 * Send the codec the verb `verb` of node `nid` with `parm`, through the
 * CORB, and wait for its answer in the RIRB, skipping unsolicited
 * responses, as azx_send_cmd and azx_rirb_get_response do: the response,
 * or ~0 when none came in 50 ms, which Linux's -1 is. Under hh_mutex.
 */
static uint32_t hh_verb(uint8_t nid, uint32_t verb, uint32_t parm)
{
    uint32_t cmd = ((uint32_t)hh_caddr << 28) | ((uint32_t)nid << 20) |
                   (verb <= 0xf ? (verb << 16) | (parm & 0xffff) : (verb << 8) | (parm & 0xff));
#ifdef RACH2_HOSTTEST
    return hdmi_test_verb(cmd);
#else
    uint16_t wp = (uint16_t)((hh_corb_wp + 1u) % 256u);
    int i;

    hh_corb[wp] = cmd;
    arch_dma_flush(&hh_corb[wp], sizeof(hh_corb[wp]));
    hh_corb_wp = wp;
    hh_write(HDA_CORBWP, wp, 16);
    for (i = 0; i < 5000; i++) {
        uint16_t hw = (uint16_t)(hh_read(HDA_RIRBWP, 16) & 0xff);

        while (hh_rirb_rp != hw) {
            uint64_t e;

            hh_rirb_rp = (uint16_t)((hh_rirb_rp + 1u) % 256u);
            e = hh_rirb[hh_rirb_rp];
            if (!((uint32_t)(e >> 32) & 0x10)) { /* not unsolicited */
                return (uint32_t)e;
            }
        }
        k_udelay(10);
    }
    if (!hh_verb_warned) {
        klog(KLOG_WARN, "hdmi-audio", "verb: no response: 0x%x", cmd);
        hh_verb_warned = 1;
    }
    return ~0u;
#endif
}

/** Read parameter `p` of node `nid`. */
static uint32_t hh_param(uint8_t nid, uint32_t p)
{
    return hh_verb(nid, V_GET_PARAM, p);
}

/**
 * Reset the link and start the command rings, Linux's azx_init_pci and
 * hda_intel_init_chip with snd_hdac_i915_set_bclk between them
 * (hda_intel.c:2014-2020): the traffic class cleared, snooping on, the
 * BCLK divided from the CD clock, the controller out of reset with its
 * codecs, the CORB and RIRB of 256 entries running, and interrupts off,
 * since the tick serves the stream. 0, or -EIO when the controller does not
 * leave reset, -ENODEV when no codec answers.
 */
static int hh_chip_init(void)
{
    uint8_t b = pci_read_config8(hh_pci.bus, hh_pci.device, hh_pci.function, HH_PCI_TCSEL);
    uint16_t devc = pci_read_config16(hh_pci.bus, hh_pci.device, hh_pci.function, HH_PCI_DEVC);
    int32_t cd = hh_ops->get_cdclk_freq(hh_dev);
    uint64_t corb = (uint64_t)virt_to_phys(hh_corb);
    uint64_t rirb = (uint64_t)virt_to_phys(hh_rirb);
    int i;

    pci_write_config8(hh_pci.bus, hh_pci.device, hh_pci.function, HH_PCI_TCSEL,
                      (uint8_t)(b & ~0x07));
    if (devc & HH_DEVC_NOSNOOP) {
        pci_write_config16(hh_pci.bus, hh_pci.device, hh_pci.function, HH_PCI_DEVC,
                           (uint16_t)(devc & ~HH_DEVC_NOSNOOP));
    }
    /* snd_hdac_i915_set_bclk (hdac_i915.c:34-72): 24 MHz from the CD clock. */
    hh_write(HH_EM4, cd == 337500 ? 16 : cd == 540000 ? 4 : cd == 675000 ? 8 : 4, 16);
    hh_write(HH_EM5, cd == 337500 ? 225 : cd == 540000 ? 90 : cd == 675000 ? 225 : 75, 16);

    /* snd_hdac_bus_reset_link: into reset and out, then 1 ms for codecs. */
    hh_write(HDA_GCTL, hh_read(HDA_GCTL, 32) & ~HDA_GCTL_CRST, 32);
    for (i = 0; i < 1000 && (hh_read(HDA_GCTL, 32) & HDA_GCTL_CRST); i++) {
        k_udelay(100);
    }
    k_udelay(1000);
    hh_write(HDA_GCTL, hh_read(HDA_GCTL, 32) | HDA_GCTL_CRST, 32);
    for (i = 0; i < 1000 && !(hh_read(HDA_GCTL, 32) & HDA_GCTL_CRST); i++) {
        k_udelay(100);
    }
    if (!(hh_read(HDA_GCTL, 32) & HDA_GCTL_CRST)) {
        klog(KLOG_WARN, "hdmi-audio", "reset: the controller never left it");
        return -EIO;
    }
    hh_msleep(1);
    for (i = 0; i < 100 && !(hh_read(HDA_STATESTS, 16) & 0x7fff); i++) {
        hh_msleep(1);
    }
    hh_caddr = 0;
    while (hh_caddr < 15 && !(hh_read(HDA_STATESTS, 16) & (1u << hh_caddr))) {
        hh_caddr++;
    }
    if (hh_caddr == 15) {
        klog(KLOG_WARN, "hdmi-audio", "reset: no codec answered");
        return -ENODEV;
    }
    hh_write(HDA_STATESTS, 0x7fff, 16);
    hh_out_sd = (int)((hh_read(HDA_GCAP, 16) >> 8) & 0xf);

    /* azx_init_cmd_io: the CORB, then the RIRB, of 256 entries. */
    hh_write(HDA_CORBCTL, 0, 8);
    hh_write(HDA_CORBLBASE, (uint32_t)corb, 32);
    hh_write(HDA_CORBUBASE, hh_hi(corb), 32);
    hh_write(HDA_CORBSIZE, 0x02, 8);
    hh_write(HDA_CORBWP, 0, 16);
    hh_write(HDA_CORBRP, 0x8000, 16);
    for (i = 0; i < 1000 && !(hh_read(HDA_CORBRP, 16) & 0x8000); i++) {
        k_udelay(1);
    }
    hh_write(HDA_CORBRP, 0, 16);
    for (i = 0; i < 1000 && (hh_read(HDA_CORBRP, 16) & 0x8000); i++) {
        k_udelay(1);
    }
    hh_corb_wp = 0;
    hh_write(HDA_CORBCTL, 0x02, 8);
    hh_write(HDA_RIRBCTL, 0, 8);
    hh_write(HDA_RIRBLBASE, (uint32_t)rirb, 32);
    hh_write(HDA_RIRBUBASE, hh_hi(rirb), 32);
    hh_write(HDA_RIRBSIZE, 0x02, 8);
    hh_write(HDA_RIRBWP, 0x8000, 16);
    hh_write(HDA_RINTCNT, 1, 16);
    hh_rirb_rp = 0;
    hh_write(HDA_RIRBCTL, 0x02, 8);
    hh_write(HDA_INTCTL, 0, 32);
    hh_write(HDA_GCTL, hh_read(HDA_GCTL, 32) | HDA_GCTL_UNSOL, 32);
    return 0;
}

/* ---- the codec ---- */

/** Select device entry `dev` of pin `nid`, Linux's snd_hda_set_dev_select:
 * nothing on a pin of one entry, which is every pin without a DP MST sink. */
static void hh_dev_select(uint8_t nid, int dev)
{
    uint32_t n = hh_param(nid, P_DEVLIST);

    if (n == ~0u || (n & 0x3f) == 0 || dev > (int)(n & 0x3f)) {
        return;
    }
    hh_verb(nid, V_SET_DEV_SEL, (uint32_t)dev);
}

/** Linux's intel_haswell_enable_all_pins and
 * intel_haswell_fixup_enable_dp12: the vendor bits that show every pin and
 * converter and turn on DP 1.2, which Haswell needs again before D0. */
static void hh_vendor_verbs(void)
{
    uint32_t v = hh_verb(HH_VENDOR_NID, V_INTEL_GET, 0);

    if (v != ~0u && !(v & INTEL_EN_ALL_PIN_CVTS)) {
        hh_verb(HH_VENDOR_NID, V_INTEL_SET, v | INTEL_EN_ALL_PIN_CVTS);
    }
    v = hh_verb(HH_VENDOR_NID, V_INTEL_GET, 0);
    if (v != ~0u && !(v & INTEL_EN_DP12)) {
        hh_verb(HH_VENDOR_NID, V_INTEL_SET, v | INTEL_EN_DP12);
    }
}

/**
 * Find the audio function, its converters and its HDMI or DisplayPort
 * pins, Linux's hdmi_parse_codec with hdmi_add_cvt and hdmi_add_pin: each
 * digital output converter, and HH_DEV_NUM entries of each pin that has
 * HDMI or DP and is connected to something, with the converters it can
 * select. 0, or -ENODEV for a codec without them.
 */
static int hh_parse(void)
{
    uint32_t sub = hh_param(0, P_SUBNODES);
    int i, j, d;

    hh_vendor = hh_param(0, P_VENDOR);
    hh_afg = 0;
    for (i = 0; i < (int)(sub & 0xff); i++) {
        uint8_t nid = (uint8_t)(((sub >> 16) & 0xff) + (uint32_t)i);

        if ((hh_param(nid, P_FG_TYPE) & 0xff) == 0x01) {
            hh_afg = nid;
            break;
        }
    }
    if (!hh_afg) {
        return -ENODEV;
    }
    hh_vendor_verbs();
    sub = hh_param(hh_afg, P_SUBNODES);
    hh_nid_start = (uint8_t)((sub >> 16) & 0xff);
    hh_nids = (int)(sub & 0xff);
    hh_ncvts = 0;
    hh_npins = 0;
    for (i = 0; i < hh_nids && hh_ncvts < HH_MAX_CVTS; i++) {
        uint8_t nid = (uint8_t)(hh_nid_start + i);
        uint32_t w = hh_param(nid, P_WCAPS);

        if ((w & WCAP_DIGITAL) && WCAP_TYPE(w) == WID_AUD_OUT) {
            hh_cvts[hh_ncvts].nid = nid;
            hh_cvts[hh_ncvts].pcm = hh_param(nid, P_PCM);
            hh_cvts[hh_ncvts].assigned = 0;
            hh_ncvts++;
        }
    }
    for (i = 0; i < hh_nids; i++) {
        uint8_t nid = (uint8_t)(hh_nid_start + i);
        uint32_t w = hh_param(nid, P_WCAPS);
        uint32_t len, cfg;

        if (WCAP_TYPE(w) != WID_PIN || !(w & WCAP_DIGITAL) ||
            !(hh_param(nid, P_PINCAP) & (PINCAP_HDMI | PINCAP_DP))) {
            continue;
        }
        cfg = hh_verb(nid, V_GET_CONFIG, 0);
        if (((cfg >> 30) & 3) == HDA_PORT_CONN_NONE) {
            continue;
        }
        for (d = 0; d < HH_DEV_NUM && hh_npins < HH_MAX_PINS; d++) {
            struct hh_pin *p = &hh_pins[hh_npins++];

            k_memset(p, 0, sizeof(*p));
            p->nid = nid;
            p->dev = (uint8_t)d;
            hh_dev_select(nid, d);
            /* hdmi_read_pin_conn: the short form's entries, four a word. */
            len = hh_param(nid, P_CONN_LEN) & 0x7f;
            for (j = 0; j < (int)len && j < HH_MAX_MUX; j++) {
                uint32_t r = hh_verb(nid, V_GET_CONN_LIST, (uint32_t)(j & ~3));

                p->mux[j] = (uint8_t)(r >> (8 * (j & 3)));
            }
            p->nmux = j;
        }
    }
    return hh_ncvts && hh_npins ? 0 : -ENODEV;
}

/**
 * Bring the codec to D0 and its pins up, as haswell_set_power_state and
 * generic_hdmi_init with hdmi_init_pin do: the vendor bits again, the
 * function and each widget with a power state in D0, and each pin's output
 * amplifier unmuted and its output on.
 */
static void hh_codec_up(void)
{
    int i;

    hh_vendor_verbs();
    hh_verb(hh_afg, V_SET_POWER, 0);
    for (i = 0; i < hh_nids; i++) {
        uint8_t nid = (uint8_t)(hh_nid_start + i);

        if (hh_param(nid, P_WCAPS) & WCAP_POWER) {
            hh_verb(nid, V_SET_POWER, 0);
        }
    }
    for (i = 0; i < hh_npins; i++) {
        struct hh_pin *p = &hh_pins[i];

        hh_dev_select(p->nid, p->dev);
        if (hh_param(p->nid, P_WCAPS) & WCAP_OUT_AMP) {
            hh_verb(p->nid, V_SET_AMP, AMP_OUT_UNMUTE);
        }
        hh_verb(p->nid, V_SET_PIN_CTL, PIN_OUT);
    }
}

/**
 * Hold the display's audio power and, on the first hold, bring the
 * controller and codec up from nothing, since the display may have turned
 * the well off and the controller with it, as __azx_runtime_resume does. 0,
 * or the display's or the controller's error. Under hh_mutex.
 */
static int32_t hh_power_get(void)
{
    int32_t err;

    if (hh_power++ > 0) {
        return 0;
    }
    err = hh_ops->get_power(hh_dev);
    if (err == 0) {
        err = hh_chip_init();
        if (err != 0) {
            hh_ops->put_power(hh_dev);
        }
    }
    if (err != 0) {
        hh_power = 0;
        return err;
    }
    if (!hh_parsed) {
        err = hh_parse();
        if (err != 0) {
            hh_ops->put_power(hh_dev);
            hh_power = 0;
            return err;
        }
        hh_parsed = 1;
    }
    hh_codec_up();
    return 0;
}

/** Let the display's audio power go with the last hold. Under hh_mutex. */
static void hh_power_put(void)
{
    if (hh_power > 0 && --hh_power == 0) {
        hh_ops->put_power(hh_dev);
    }
}

/* ---- the ELD ---- */

/**
 * Parse the ELD `buf` of `size` bytes into `e`, Linux's
 * snd_hdmi_parse_eld (hda_eld.c:224-283) with
 * hdmi_update_short_audio_desc: 0, or -EINVAL for one of a version it does
 * not know or whose name or descriptors run past `size`.
 */
static int hh_parse_eld(struct hh_eld_info *e, const uint8_t *buf, int size)
{
    int mnl, i;

    k_memset(e, 0, sizeof(*e));
    e->eld_ver = buf[0] >> 3;
    if (e->eld_ver != 2 && e->eld_ver != 31) {
        return -EINVAL;
    }
    e->baseline_len = buf[2];
    mnl = buf[4] & 0x1f;
    e->cea_edid_ver = buf[4] >> 5;
    e->support_hdcp = buf[5] & 1;
    e->support_ai = (buf[5] >> 1) & 1;
    e->conn_type = (buf[5] >> 2) & 3;
    e->sad_count = buf[5] >> 4;
    e->aud_synch_delay = buf[6] * 2;
    e->spk_alloc = buf[7] & 0x7f;
    for (i = 7; i >= 0; i--) {
        e->port_id = (e->port_id << 8) | buf[8 + i];
    }
    e->manufacture_id = buf[16] | (buf[17] << 8);
    e->product_id = buf[18] | (buf[19] << 8);
    if (mnl > ELD_MAX_MNL || ELD_FIXED_BYTES + mnl > size) {
        return -EINVAL;
    }
    k_memcpy(e->monitor_name, buf + ELD_FIXED_BYTES, (uint32_t)mnl);
    e->monitor_name[mnl] = '\0';
    for (i = 0; i < e->sad_count; i++) {
        const uint8_t *s = buf + ELD_FIXED_BYTES + mnl + 3 * i;
        struct hh_sad *a = &e->sad[i];
        int j;

        if (ELD_FIXED_BYTES + mnl + 3 * (i + 1) > size) {
            return -EINVAL;
        }
        for (j = 0; j < 7; j++) {
            if (s[1] & (1 << j)) {
                a->rates |= cea_rates[j];
            }
        }
        a->channels = (s[0] & 7) + 1;
        a->format = (s[0] >> 3) & 0xf;
        switch (a->format) {
        case 1: /* LPCM: 16, 20 and 24 bits as AC_SUPPCM_BITS_* */
            for (j = 0; j < 3; j++) {
                if (s[2] & (1 << j)) {
                    a->bits |= 1 << (17 + j);
                }
            }
            break;
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:
            a->max_bitrate = s[2] * 8000;
            break;
        case 14: /* WMAPro */
            a->profile = s[2] & 7;
            break;
        case 15: /* the extended types: HE-AAC, HE-AACv2, MPEG Surround */
            a->format = s[2] >> 3;
            a->format = a->format == 0 || a->format >= 4 ? 0 : a->format + 14;
            break;
        default:
            break;
        }
    }
    if (!e->spk_alloc) {
        e->spk_alloc = 0xffff;
    }
    return 0;
}

/**
 * Read the ELD of pin entry `p` from the display, Linux's sync_eld_via_acomp
 * and update_eld: valid only when the display gives one that parses with a
 * short audio descriptor, and the monitor present while the display plays
 * audio on the port. Pin 5 is port B, the display's port 1
 * (intel_pin2port). Return 1 when the entry turned valid. Under hh_mutex.
 */
static int hh_sync_eld(struct hh_pin *p)
{
    uint8_t buf[HH_ELD_MAX];
    struct hh_eld_info info;
    int enabled = 0, was = p->eld_valid, size, valid;

    k_memset(&info, 0, sizeof(info));
    size = hh_ops->get_eld(hh_dev, p->nid - 4, p->dev, &enabled, buf, (int)sizeof(buf));
    if (size > (int)sizeof(buf)) {
        size = (int)sizeof(buf);
    }
    valid = size > 0 && hh_parse_eld(&info, buf, size) == 0 && info.sad_count > 0;
    p->monitor_present = enabled;
    p->eld_valid = valid;
    p->eld_size = valid ? size : 0;
    if (valid) {
        k_memcpy(p->eld, buf, (uint32_t)size);
        p->info = info;
    }
    return valid && !was;
}

/* ---- the stream's pin ---- */

/** Linux's haswell_verify_D0: converter `cvt` and pin `pin` in D0, the
 * pin given 40 ms to get there. */
static void hh_verify_d0(uint8_t cvt, uint8_t pin)
{
    if (cvt && (hh_verb(cvt, V_GET_POWER, 0) & 0xff) != 0) {
        hh_verb(cvt, V_SET_POWER, 0);
    }
    if ((hh_verb(pin, V_GET_POWER, 0) & 0xff) != 0) {
        hh_verb(pin, V_SET_POWER, 0);
        hh_msleep(40);
    }
}

/**
 * Point every other pin entry that selects converter index `mux_idx` at an
 * unassigned converter, Linux's intel_not_share_assigned_cvt: the Intel pins
 * share one connection list, so a converter two pins select plays on both.
 * `self` is the entry to leave alone, or NULL.
 */
static void hh_not_share(const struct hh_pin *self, int mux_idx)
{
    int i, c;

    for (i = 0; i < hh_npins; i++) {
        struct hh_pin *p = &hh_pins[i];

        if (p == self || !p->eld_valid) {
            continue;
        }
        hh_dev_select(p->nid, p->dev);
        if ((int)(hh_verb(p->nid, V_GET_CONN_SEL, 0) & 0xff) != mux_idx) {
            continue;
        }
        for (c = 0; c < hh_ncvts; c++) {
            if (!hh_cvts[c].assigned) {
                hh_verb(p->nid, V_SET_CONN_SEL, (uint32_t)c);
                break;
            }
        }
    }
}

/** Linux's i915_pin_cvt_fixup for entry `p`: it and its converter in D0,
 * selecting its converter, and no other entry selecting the same. */
static void hh_pin_cvt_fixup(struct hh_pin *p)
{
    hh_verify_d0(p->cvt, p->nid);
    hh_dev_select(p->nid, p->dev);
    if ((int)(hh_verb(p->nid, V_GET_CONN_SEL, 0) & 0xff) != p->mux_idx) {
        hh_verb(p->nid, V_SET_CONN_SEL, (uint32_t)p->mux_idx);
    }
    hh_not_share(p, p->mux_idx);
}

/**
 * Send the stereo audio infoframe on entry `p`, as hdmi_setup_audio_infoframe
 * does for two channels: the pin's amplifier unmuted again, the converter
 * counting two channels, the slots of channel allocation 0, and the frame
 * HDMI or DisplayPort wants, written only when the pin is not already
 * sending it (hdmi_pin_setup_infoframe).
 */
static void hh_infoframe(struct hh_pin *p)
{
    static const uint8_t slots[8] = {0x00, 0x11, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7};
    uint8_t ai[9];
    uint8_t sum = 0;
    int i, same = 1;

    hh_dev_select(p->nid, p->dev);
    if (hh_param(p->nid, P_WCAPS) & WCAP_OUT_AMP) {
        hh_verb(p->nid, V_SET_AMP, AMP_OUT_UNMUTE);
    }
    hh_verb(p->cvt, V_SET_CHAN_CNT, 1);
    for (i = 0; i < 8; i++) {
        hh_verb(p->nid, V_SET_CHAN_SLOT, slots[i]);
    }
    k_memset(ai, 0, sizeof(ai));
    if (p->info.conn_type == 0) { /* HDMI */
        ai[0] = 0x84;
        ai[1] = 0x01;
        ai[2] = 0x0a;
        ai[4] = 1; /* CC: two channels */
        for (i = 0; i < 9; i++) {
            sum = (uint8_t)(sum + ai[i]);
        }
        ai[3] = (uint8_t)-sum;
    } else if (p->info.conn_type == 1) { /* DisplayPort */
        ai[0] = 0x84;
        ai[1] = 0x1b;
        ai[2] = 0x11 << 2;
        ai[3] = 1;
    } else {
        return;
    }
    hh_verb(p->nid, V_SET_DIP_INDEX, 0);
    if ((hh_verb(p->nid, V_GET_DIP_XMIT, 0) & 0xff) != DIPXMIT_BEST) {
        same = 0;
    }
    for (i = 0; same && i < 9; i++) {
        if ((hh_verb(p->nid, V_GET_DIP_DATA, 0) & 0xff) != ai[i]) {
            same = 0;
        }
    }
    if (same) {
        return;
    }
    hh_verb(p->nid, V_SET_DIP_INDEX, 0);
    hh_verb(p->nid, V_SET_DIP_XMIT, 0);
    hh_verb(p->nid, V_SET_DIP_INDEX, 0);
    for (i = 0; i < 9; i++) {
        hh_verb(p->nid, V_SET_DIP_DATA, ai[i]);
    }
    hh_verb(p->nid, V_SET_DIP_INDEX, 0);
    hh_verb(p->nid, V_SET_DIP_XMIT, DIPXMIT_BEST);
}

/**
 * Prepare entry `p` for the stream, as generic_hdmi_playback_pcm_prepare
 * does: the converter selected and not shared, the display's audio clocks
 * set for the rate, and the infoframe sent. Under hh_mutex.
 */
static void hh_pin_prepare(struct hh_pin *p)
{
    hh_pin_cvt_fixup(p);
    hh_ops->sync_audio_rate(hh_dev, p->nid - 4, p->dev, (int)hh_rate);
    p->setup = 1;
    hh_infoframe(p);
}

/**
 * Give the stream entry `p`, Linux's hdmi_pcm_setup_pin and hdmi_pcm_open:
 * its connection selects the stream's converter. Under hh_mutex.
 */
static void hh_pin_attach(struct hh_pin *p)
{
    int m;

    for (m = 0; m < p->nmux && p->mux[m] != hh_cvts[hh_cvt_idx].nid; m++) {
    }
    p->mux_idx = m;
    p->cvt = hh_cvts[hh_cvt_idx].nid;
    hh_bound = (int)(p - hh_pins);
    if (m < p->nmux) {
        hh_dev_select(p->nid, p->dev);
        hh_verb(p->nid, V_SET_CONN_SEL, (uint32_t)m);
    }
}

/** Set the converter's stream tag and format, Linux's
 * snd_hda_codec_setup_stream after hdmi_setup_stream's DIGI_CONVERT_3. */
static void hh_cvt_setup(void)
{
    uint8_t cvt = hh_cvts[hh_cvt_idx].nid;

    if (hh_bound >= 0) {
        hh_verify_d0(cvt, hh_pins[hh_bound].nid);
    }
    hh_verb(cvt, V_SET_DIGI3, 0);
    hh_verb(cvt, V_SET_STREAMID, HH_TAG << 4);
    hh_msleep(1);
    hh_verb(cvt, V_SET_FORMAT, hh_fmt);
}

/* ---- the stream ---- */

/**
 * Limit the stream's rates and width by the sink's ELD `e`, Linux's
 * snd_hdmi_eld_update_pcm_info: the rates every sink plays, 32, 44.1 and
 * 48 kHz, with those its descriptors add, and 24 bits when an LPCM
 * descriptor has 20 or 24.
 */
static void hh_eld_limit(const struct hh_eld_info *e)
{
    int alsa = (1 << 5) | (1 << 6) | (1 << 7), wide = 0, i, b;
    uint32_t ok = 0;

    for (i = 0; i < e->sad_count; i++) {
        alsa |= e->sad[i].rates;
        if (e->sad[i].format == 1 && (e->sad[i].bits & (3 << 18))) {
            wide = 1;
        }
    }
    for (i = 0; i < 12; i++) {
        for (b = 0; b < 14; b++) {
            if ((alsa & (1 << b)) && alsa_rates[b] == hh_rate_hz[i]) {
                ok |= 1u << i;
            }
        }
    }
    hh_rates &= ok;
    hh_wide = hh_wide && wide;
}

/**
 * Take the stream, Linux's hdmi_pcm_open: the display's power held until
 * the close, the first converter, IEC958 on, the first entry with a valid
 * ELD given the stream, and the rates limited by its sink; with no such
 * entry the stream plays on no pin, as hdmi_pcm_open_no_pin lets it.
 */
static int hh_stream_open(audio_stream_t *s)
{
    int i, err;

    if (s->dir != AUDIO_PLAYBACK) {
        return -ENODEV;
    }
    mutex_lock(&hh_mutex);
    if (!hh_present) {
        mutex_unlock(&hh_mutex);
        return -ENODEV;
    }
    if (hh_stream) {
        mutex_unlock(&hh_mutex);
        return -EBUSY;
    }
    err = hh_power_get();
    if (err != 0) {
        mutex_unlock(&hh_mutex);
        return -EIO;
    }
    hh_cvt_idx = 0;
    hh_cvts[0].assigned = 1;
    hh_bound = -1;
    hh_rates = hh_cvts[0].pcm & 0xfff;
    hh_wide = (hh_cvts[0].pcm & (1u << 19)) != 0;
    /* IEC958 on, with alsa-lib's HDMI.conf status: original, PCM coder. */
    hh_verb(hh_cvts[0].nid, V_SET_DIGI1, DIG1_ENABLE | 0x80);
    hh_verb(hh_cvts[0].nid, V_SET_DIGI2, 0x02);
    for (i = 0; i < hh_npins && hh_bound < 0; i++) {
        if (hh_pins[i].eld_valid) {
            hh_pin_attach(&hh_pins[i]);
        }
    }
    if (hh_bound >= 0) {
        hh_eld_limit(&hh_pins[hh_bound].info);
        hh_pin_cvt_fixup(&hh_pins[hh_bound]);
    } else {
        hh_not_share(NULL, 0);
    }
    if (!hh_rates) {
        if (hh_bound >= 0) {
            hh_pins[hh_bound].cvt = 0;
        }
        hh_cvts[0].assigned = 0;
        hh_power_put();
        mutex_unlock(&hh_mutex);
        return -ENODEV;
    }
    hh_stream = s;
    hh_held = 1;
    mutex_unlock(&hh_mutex);
    return 0;
}

/** The stream's frames the DMA's next `n` frames, at least 1, take. */
static uint32_t hh_need(const audio_stream_t *s, uint32_t n)
{
    return hh_copy ? n : (hh_acc + (n - 1u) * s->rate) / hh_rate;
}

/** The stream bytes the DMA's `n` periods take, in whole periods of `s`,
 * with a conversion's two frames of lookahead. The DMA's frames are at
 * most HH_DMA_MAX / 4, so their product with a rate fits in 32 bits. */
static uint32_t hh_lead(const audio_stream_t *s, uint32_t n)
{
    uint32_t spf = s->period_bytes / hh_sframe;
    uint32_t frames = n * (hh_pb / hh_frame) * s->rate / hh_rate + (hh_copy ? 0u : 2u);

    return (frames + spf - 1u) / spf * s->period_bytes;
}

/** The index in hh_rate_hz of the rate the sink allows nearest `rate`,
 * the higher on a tie, or -1 when it allows none; under hh_mutex. */
static int hh_nearest_rate(uint32_t rate)
{
    uint32_t d, bd = 0;
    int i, bi = -1;

    /* Up to 192 kHz, HDMI's highest, the 12th bit's 384 kHz left out. */
    for (i = 0; i < 11; i++) {
        if (!(hh_rates & (1u << i))) {
            continue;
        }
        d = hh_rate_hz[i] > rate ? hh_rate_hz[i] - rate : rate - hh_rate_hz[i];
        if (bi < 0 || d < bd || (d == bd && hh_rate_hz[i] > hh_rate_hz[bi])) {
            bi = i;
            bd = d;
        }
    }
    return bi;
}

/**
 * Grant `s` what it asks for, as Linux's OSS layer does through its
 * plugins: its format, one or two channels, and its rate, held between
 * HH_RATE_MIN and HH_RATE_MAX. The DMA plays two channels at the rate the
 * converter and sink allow nearest the stream's, the higher on a tie, in
 * 16 bits, or in 24 bits in 32 for a stream of more than 16 where the
 * converter and sink take them; the refill converts (hh_fill). The
 * stream's period is the one asked for, in whole frames; the DMA's covers
 * the same time, a multiple of 128 bytes up to HH_PERIOD_MAX, and the DMA
 * buffer holds enough of them for HH_BUFFER_MS, fewer when the stream
 * bytes they take would pass HH_STREAM_MAX, but two at least.
 */
static int hh_stream_setup(audio_stream_t *s)
{
    uint32_t best, pb, need, n, frames;
    int bi, wide;

    mutex_lock(&hh_mutex);
    if (s != hh_stream || s->period_bytes < 128) {
        mutex_unlock(&hh_mutex);
        return -EINVAL;
    }
    s->rate = s->rate < HH_RATE_MIN ? HH_RATE_MIN : s->rate > HH_RATE_MAX ? HH_RATE_MAX : s->rate;
    s->channels = s->channels < 2 ? 1 : 2;
    if (s->period_bytes > HH_PERIOD_MAX) {
        s->period_bytes = HH_PERIOD_MAX;
    }
    bi = hh_nearest_rate(s->rate);
    if (bi < 0) {
        mutex_unlock(&hh_mutex);
        return -EINVAL;
    }
    best = hh_rate_hz[bi];
    wide = hh_wide && AUDIO_SAMPLE_BYTES(s->format) > 2u;
    hh_rate = best;
    hh_frame = wide ? 8u : 4u;
    hh_sframe = (uint32_t)s->channels * AUDIO_SAMPLE_BYTES(s->format);
    hh_copy =
        s->rate == best && s->channels == 2 && s->format == (wide ? AUDIO_S32LE : AUDIO_S16LE);
    s->period_bytes -= s->period_bytes % hh_sframe;
    /* At most 16384 frames times 192 kHz: 32 bits hold it. */
    frames = s->period_bytes / hh_sframe * best / s->rate;
    pb = (frames * hh_frame + 64u) / 128u * 128u;
    pb = pb < 128u ? 128u : pb > HH_PERIOD_MAX ? HH_PERIOD_MAX : pb;
    need = best / 1000u * hh_frame * HH_BUFFER_MS;
    n = (need + pb - 1) / pb;
    if (n > HH_BDL_MAX) {
        n = HH_BDL_MAX;
    }
    if (n > HH_DMA_MAX / pb) {
        n = HH_DMA_MAX / pb;
    }
    hh_pb = pb;
    while (n > 2 && hh_lead(s, n) > HH_STREAM_MAX) {
        n--;
    }
    if (n < 2) {
        n = 2;
    }
    s->device_periods = hh_lead(s, n) / s->period_bytes;
    hh_n = n;
    hh_fmt = (uint16_t)(hh_rate_fmt[bi] | (wide ? HDA_FMT_BITS_24 : HDA_FMT_BITS_16) | 1u);
    mutex_unlock(&hh_mutex);
    return 0;
}

/** Sample `p` of the stream's format as 24 bits: unsigned 8-bit moved to
 * signed, and a wider one's top 24 bits. */
static int32_t hh_sample(const audio_stream_t *s, const uint8_t *p)
{
    uint32_t u;

    switch (s->format) {
    case AUDIO_U8:
        return ((int32_t)p[0] - 128) * 65536;
    case AUDIO_S16LE:
        return (int32_t)(int16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8) * 256;
    case AUDIO_S24LE:
    case AUDIO_S24_3LE:
        u = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
        break;
    case AUDIO_S32LE:
    default:
        u = (uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16;
        break;
    }
    return (int32_t)(u & 0x800000u ? u | 0xff000000u : u);
}

/** Read the stream's frame at `p` into `v` as two 24-bit samples; a mono
 * sample goes to both. */
static void hh_frame_in(const audio_stream_t *s, const uint8_t *p, int32_t *v)
{
    v[0] = hh_sample(s, p);
    v[1] = s->channels == 2 ? hh_sample(s, p + AUDIO_SAMPLE_BYTES(s->format)) : v[0];
}

/** `a` plus `w` / 8192 of the way to `b`, rounded down, both of 24 bits, in
 * 32-bit arithmetic: the difference's magnitude is split at bit 13. */
static int32_t hh_lerp(int32_t a, int32_t b, uint32_t w)
{
    uint32_t m = (uint32_t)(b > a ? b - a : a - b);
    uint32_t q = (m >> 13) * w + ((m & 8191u) * w >> 13);

    return b > a ? a + (int32_t)q : a - (int32_t)q;
}

/** Store the 24-bit sample `v` at `p` as the DMA plays it: in the top 24
 * bits of 32, or rounded to the nearest of 16, a half up, and held there.
 * The offset keeps the division's dividend positive, where C89 defines its
 * rounding. */
static void hh_store(uint8_t *p, int32_t v)
{
    uint32_t u;

    if (hh_frame == 8u) {
        u = (uint32_t)v << 8;
        p[2] = (uint8_t)(u >> 16);
        p[3] = (uint8_t)(u >> 24);
    } else {
        v = (v + 8388608 + 128) / 256 - 32768;
        u = (uint32_t)(v > 32767 ? 32767 : v);
    }
    p[0] = (uint8_t)u;
    p[1] = (uint8_t)(u >> 8);
}

/**
 * Fill period `slot` of the DMA buffer from the stream and write it back to
 * memory for the controller; the stream's frames taken for it. A stream in
 * the DMA's frames is copied; another is converted in chunks of at most
 * HH_STAGE_FRAMES of the DMA's, each `transfer` taking the frames the chunk
 * needs into hh_stage, as Linux's plugins pull them. Under hh_slock, or
 * before the DMA runs.
 */
static uint32_t hh_fill(uint32_t slot)
{
    audio_stream_t *s = hh_stream;
    uint8_t *dst = hh_dma + slot * hh_pb;
    uint32_t frames = hh_pb / hh_frame, taken = 0, chunk, need, i, k, w;
    int32_t v[2];

    if (hh_copy) {
        s->transfer(s, dst, hh_pb);
        taken = frames;
    }
    while (!hh_copy && frames > 0) {
        chunk = frames < HH_STAGE_FRAMES ? frames : HH_STAGE_FRAMES;
        need = hh_need(s, chunk);
        while (chunk > 1 && need * hh_sframe > sizeof(hh_stage)) {
            chunk /= 2;
            need = hh_need(s, chunk);
        }
        if (need > 0) {
            s->transfer(s, hh_stage, need * hh_sframe);
        }
        for (i = 0, k = 0; i < chunk; i++) {
            while (hh_acc >= hh_rate) {
                hh_prev[0] = hh_cur[0];
                hh_prev[1] = hh_cur[1];
                hh_frame_in(s, hh_stage + k++ * hh_sframe, hh_cur);
                hh_acc -= hh_rate;
            }
            w = (hh_acc << 13) / hh_rate;
            v[0] = hh_lerp(hh_prev[0], hh_cur[0], w);
            v[1] = hh_lerp(hh_prev[1], hh_cur[1], w);
            hh_store(dst + i * hh_frame, v[0]);
            hh_store(dst + i * hh_frame + hh_frame / 2u, v[1]);
            hh_acc += s->rate;
        }
        taken += need;
        frames -= chunk;
        dst += chunk * hh_frame;
    }
    arch_dma_flush(hh_dma + slot * hh_pb, hh_pb);
    return taken;
}

/** Reset the output stream descriptor through its SRST handshake. */
static void hh_sd_reset(void)
{
    int i;

    hh_write(hh_sd(HDA_SD_CTL), 0, 8);
    for (i = 0; i < 1000 && (hh_read(hh_sd(HDA_SD_CTL), 8) & HDA_SD_CTL_RUN); i++) {
        k_udelay(1);
    }
    hh_write(hh_sd(HDA_SD_CTL), HDA_SD_CTL_SRST, 8);
    for (i = 0; i < 1000 && !(hh_read(hh_sd(HDA_SD_CTL), 8) & HDA_SD_CTL_SRST); i++) {
        k_udelay(1);
    }
    hh_write(hh_sd(HDA_SD_CTL), 0, 8);
    for (i = 0; i < 1000 && (hh_read(hh_sd(HDA_SD_CTL), 8) & HDA_SD_CTL_SRST); i++) {
        k_udelay(1);
    }
}

/**
 * Lay the descriptor list over the DMA buffer, fill every period from the
 * stream, program the descriptor, prepare the codec as Linux's prepare
 * does, and run the DMA. -EIO for a stream not set up, or one whose
 * display has gone since its open.
 */
static int hh_stream_start(audio_stream_t *s)
{
    uint64_t addr = (uint64_t)virt_to_phys(hh_dma);
    uint64_t bdl = (uint64_t)virt_to_phys(hh_bdl);
    uint32_t i, flags;

    mutex_lock(&hh_mutex);
    if (s != hh_stream || !hh_present || !hh_held || !hh_pb) {
        mutex_unlock(&hh_mutex);
        return -EIO;
    }
    hh_sd_reset();
    /* The first DMA frame takes two of the stream's: the first, exactly,
     * and the next, to interpolate towards. */
    hh_acc = 2u * hh_rate;
    k_memset(hh_cur, 0, sizeof(hh_cur));
    hh_taken = 0;
    for (i = 0; i < hh_n; i++) {
        hh_bdl[i].addr_low = (uint32_t)(addr + i * hh_pb);
        hh_bdl[i].addr_high = hh_hi(addr + i * hh_pb);
        hh_bdl[i].length = hh_pb;
        hh_bdl[i].ioc = 0;
        hh_in[i] = hh_fill(i);
        hh_taken += hh_in[i];
    }
    arch_dma_flush(hh_bdl, hh_n * sizeof(hh_bdl[0]));
    hh_write(hh_sd(HDA_SD_BDPL), (uint32_t)bdl, 32);
    hh_write(hh_sd(HDA_SD_BDPU), hh_hi(bdl), 32);
    hh_write(hh_sd(HDA_SD_CBL), hh_n * hh_pb, 32);
    hh_write(hh_sd(HDA_SD_LVI), hh_n - 1, 16);
    hh_write(hh_sd(HDA_SD_FORMAT), hh_fmt, 16);
    hh_write(hh_sd(HDA_SD_CTL), (uint32_t)HH_TAG << 20, 32);
    if (hh_bound >= 0) {
        hh_pin_prepare(&hh_pins[hh_bound]);
    } else {
        hh_not_share(NULL, hh_cvt_idx);
    }
    hh_cvt_setup();
    flags = spin_lock_irqsave(&hh_slock);
    hh_next = 0;
    hh_played = 0;
    hh_pos = 0;
    hh_mark = 0;
    hh_last_tick = rtc_ticks_now();
    hh_last_lpib = 0;
    hh_running = 1;
    hh_write(hh_sd(HDA_SD_CTL), ((uint32_t)HH_TAG << 20) | HDA_SD_CTL_RUN, 32);
    spin_unlock_irqrestore(&hh_slock, flags);
    mutex_unlock(&hh_mutex);
    return 0;
}

/** Stop the DMA and the converter's stream, under hh_mutex: after the
 * spinlock is let go, the tick makes no more calls into the stream. */
static void hh_halt(void)
{
    uint32_t flags = spin_lock_irqsave(&hh_slock);
    int was = hh_running;

    hh_running = 0;
    if (was) {
        hh_write(hh_sd(HDA_SD_CTL), (uint32_t)HH_TAG << 20, 32);
    }
    spin_unlock_irqrestore(&hh_slock, flags);
    if (was) {
        hh_sd_reset();
        /* snd_hda_codec_cleanup_stream */
        hh_verb(hh_cvts[hh_cvt_idx].nid, V_SET_STREAMID, 0);
        if (hh_bound >= 0) {
            hh_pins[hh_bound].setup = 0;
        }
    }
}

/** Stop the stream at once. */
static void hh_stream_stop(audio_stream_t *s)
{
    mutex_lock(&hh_mutex);
    if (s == hh_stream && hh_present) {
        hh_halt();
    }
    mutex_unlock(&hh_mutex);
}

/** Give the device back, and the display's power with it unless the
 * display's going took it. */
static void hh_stream_close(audio_stream_t *s)
{
    mutex_lock(&hh_mutex);
    if (s == hh_stream) {
        if (hh_present) {
            hh_halt();
        }
        if (hh_held) {
            hh_power_put();
            hh_held = 0;
        }
        if (hh_bound >= 0) {
            hh_pins[hh_bound].cvt = 0;
        }
        hh_cvts[hh_cvt_idx].assigned = 0;
        hh_bound = -1;
        hh_pb = 0;
        hh_stream = NULL;
    }
    mutex_unlock(&hh_mutex);
}

/**
 * The DMA periods played since the last refill, at the DMA's place `lpib`
 * in bytes and the tick `now`: those the place moved over, and a whole
 * ring for each lap the place cannot show. LPIB gives the place only within
 * the ring, so a refill late by a lap or more would find the period it
 * left and count none; as Linux's snd_pcm_update_hw_ptr0 counts the jiffies
 * since its last update, the laps are the frames the elapsed ticks played
 * at the DMA's rate less those the place moved over, in whole rings,
 * rounded to the nearest. Under hh_slock.
 */
static uint32_t hh_passed(uint32_t lpib, uint64_t now)
{
    uint32_t cbl = hh_n * hh_pb, ring = cbl / hh_frame, laps = 0, moved, played, ticks;

    ticks = now - hh_last_tick > HH_LAP_TICKS ? HH_LAP_TICKS : (uint32_t)(now - hh_last_tick);
    /* At most 5 s at 192 kHz: 32 bits hold it. */
    played = ticks * hh_rate / (uint32_t)TICK_HZ;
    moved = (lpib + cbl - hh_last_lpib) % cbl / hh_frame;
    if (played > moved) {
        laps = (played - moved + ring / 2u) / ring;
    }
    return (lpib / hh_pb + hh_n - hh_next) % hh_n + laps * hh_n;
}

/** The stream's frames played at `lpib` with `passed` periods played since
 * the last refill: those of the periods played, the share of the one
 * playing that `lpib` has passed, and never less than the last given.
 * Under hh_slock. */
static uint32_t hh_played_at(uint32_t lpib, uint32_t passed)
{
    uint32_t pos = hh_played, slot = hh_next, off = lpib % hh_pb;

    while (passed-- > 0) {
        pos += hh_in[slot];
        slot = (slot + 1u) % hh_n;
    }
    pos += hh_in[slot] * off / hh_pb;
    return (int32_t)(pos - hh_pos) > 0 ? pos : hh_pos;
}

/** The DMA's place in the ring, in bytes. */
static uint32_t hh_lpib(void)
{
    return hh_read(hh_sd(HDA_SD_LPIB), 32) % (hh_n * hh_pb);
}

/** The frames played since the start, from the link position and the
 * clock; it refills nothing, since a wait's condition may read it. */
static uint32_t hh_stream_position(audio_stream_t *s)
{
    uint32_t flags, pos = 0, lpib;

    flags = spin_lock_irqsave(&hh_slock);
    if (s == hh_stream && hh_running) {
        lpib = hh_lpib();
        pos = hh_played_at(lpib, hh_passed(lpib, rtc_ticks_now()));
    }
    spin_unlock_irqrestore(&hh_slock, flags);
    return pos;
}

/** The frames the stream has given that have not played: those taken less
 * those played. */
static uint32_t hh_stream_delay(audio_stream_t *s)
{
    uint32_t pos, taken, flags;

    if (!s || s != hh_stream || !hh_running) {
        return 0;
    }
    pos = hh_stream_position(s);
    flags = spin_lock_irqsave(&hh_slock);
    taken = hh_taken;
    spin_unlock_irqrestore(&hh_slock, flags);
    return (int32_t)(taken - pos) > 0 ? taken - pos : 0;
}

void hdmi_audio_tick(void)
{
    audio_stream_t *s;
    uint32_t flags, lpib, passed, spf;
    uint64_t now;

    if (!hh_running) {
        return;
    }
    flags = spin_lock_irqsave(&hh_slock);
    s = hh_stream;
    if (hh_running && s) {
        now = rtc_ticks_now();
        lpib = hh_lpib();
        passed = hh_passed(lpib, now);
        hh_pos = hh_played_at(lpib, passed);
        hh_last_tick = now;
        hh_last_lpib = lpib;
        while (passed-- > 0) {
            hh_played += hh_in[hh_next];
            hh_in[hh_next] = hh_fill(hh_next);
            hh_taken += hh_in[hh_next];
            hh_next = (hh_next + 1) % hh_n;
        }
        spf = s->period_bytes / hh_sframe;
        while (hh_pos - hh_mark >= spf) {
            hh_mark += spf;
            s->period(s);
        }
    }
    spin_unlock_irqrestore(&hh_slock, flags);
}

uint32_t hdmi_audio_idle_ticks(void)
{
    return hh_running ? 1u : TICK_HZ;
}

/** The driver has only streams: no write path, volume or input. */
static int hh_none_init(void)
{
    return -1;
}

audio_driver_t hdmi_audio_driver = {"HDA Intel HDMI",
                                    AUDIO_DRIVER_HDA,
                                    hh_none_init,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    hh_stream_open,
                                    hh_stream_setup,
                                    hh_stream_start,
                                    hh_stream_stop,
                                    hh_stream_close,
                                    hh_stream_position,
                                    hh_stream_delay,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL};

/* ---- /dev/dsp1 ---- */

static const struct file_ops hh_dsp_ops = {dspdev_read,  dspdev_write, dspdev_poll, dspdev_ioctl,
                                           dspdev_close, NULL,         NULL,        0};

/** Open /dev/dsp1 on the HDMI codec's stream. */
static int32_t hh_dsp_open(struct devfs_node *node, struct fd_entry *e, int flags)
{
    (void)node;
    return dspdev_open_driver(e, flags, &hdmi_audio_driver);
}

static struct devfs_node hh_dsp_node = {"dsp1",      DSP_MAJOR,   HDMI_DSP_MINOR, 0666,
                                        &hh_dsp_ops, hh_dsp_open, NULL,           0};
static int hh_dsp_registered;

/* ---- the component ---- */

/**
 * The display is there: bring the controller and codec up once to find the
 * pins, read each entry's ELD, and offer /dev/dsp1, as azx_probe_continue
 * and patch_i915_hsw_hdmi do. Under the component's lock.
 */
static void hh_bind(void *ptr, const struct acomp_ops *ops, void *dev)
{
    int i;

    (void)ptr;
    mutex_lock(&hh_mutex);
    hh_ops = ops;
    hh_dev = dev;
    if (hh_power_get() == 0) {
        for (i = 0; i < hh_npins; i++) {
            (void)hh_sync_eld(&hh_pins[i]);
        }
        hh_power_put();
        hh_present = 1;
        klog(KLOG_INFO, "hdmi-audio", "ready: codec %x at %d, %d pins", hh_vendor, hh_caddr,
             hh_npins / HH_DEV_NUM);
    } else {
        klog(KLOG_WARN, "hdmi-audio", "absent: the controller or codec did not come up");
    }
    mutex_unlock(&hh_mutex);
    if (hh_present && devfs_node_register(&hh_dsp_node) == 0) {
        hh_dsp_registered = 1;
    }
}

/**
 * The display goes: stop the stream, let its power go, tell its owner, so
 * that a writer, a drain or a close waiting on it wakes and fails, as
 * Linux's snd_card_disconnect does, and withdraw the card. The stream
 * stays the owner's until it closes, and an open of /dev/dsp1 keeps
 * failing until then.
 */
static void hh_unbind(void *ptr)
{
    (void)ptr;
    if (hh_dsp_registered) {
        devfs_node_unregister(&hh_dsp_node);
        hh_dsp_registered = 0;
    }
    mutex_lock(&hh_mutex);
    if (hh_stream && hh_present) {
        hh_halt();
    }
    if (hh_held) {
        hh_power_put();
        hh_held = 0;
    }
    if (hh_stream && hh_stream->gone) {
        hh_stream->gone(hh_stream);
    }
    hh_present = 0;
    hh_parsed = 0;
    hh_power = 0;
    hh_ops = NULL;
    hh_dev = NULL;
    mutex_unlock(&hh_mutex);
}

/**
 * The display turned audio on or off on port `port`, Linux's
 * intel_pin_eld_notify: the BCLK set again while the controller is up, the
 * entry's ELD read, and, while a stream plays, a newly valid entry given
 * the stream when it has none, or the stream's entry prepared again, as
 * update_eld does.
 */
static void hh_notify(void *ptr, int port, int pipe)
{
    int i, dev = pipe < 0 ? 0 : pipe;

    (void)ptr;
    mutex_lock(&hh_mutex);
    for (i = 0; hh_present && i < hh_npins; i++) {
        struct hh_pin *p = &hh_pins[i];
        int turned;

        if (p->nid - 4 != port || p->dev != dev) {
            continue;
        }
        turned = hh_sync_eld(p);
        if (!hh_stream) {
            break;
        }
        if (hh_bound == i && !p->eld_valid) {
            p->cvt = 0;
            p->setup = 0;
            hh_bound = -1;
        } else if (turned && hh_bound < 0) {
            hh_pin_attach(p);
        }
        if (turned && hh_bound == i && hh_running) {
            hh_pin_prepare(p);
        }
        break;
    }
    mutex_unlock(&hh_mutex);
}

static const struct acomp_audio_ops hh_acomp_ops = {hh_bind, hh_unbind, hh_notify};

/**
 * Bring the controller to D0, as Linux's pci_enable_device does before its
 * driver touches it, since firmware may leave it in D3hot, where its
 * registers do not answer. Leaving D3hot may reset the configuration
 * header of a function without No_Soft_Reset, so the header is saved
 * before and written back after the 10 ms the change takes
 * (PCI_PM_D3HOT_WAIT), as Linux's pci_restore_bars does.
 */
static void hh_pci_d0(void)
{
    struct pci_saved_state st;
    int state = pci_power_state(&hh_pci);

    if (state <= PCI_D0) {
        return;
    }
    pci_save_state(&hh_pci, &st);
    (void)pci_set_power_state(&hh_pci, PCI_D0);
    hh_msleep(PCI_PM_D3HOT_WAIT);
    pci_restore_state(&hh_pci, &st);
    klog(KLOG_INFO, "hdmi-audio", "power: D%d to D0", state);
}

void hdmi_audio_init(void)
{
    pci_device_t d, seen, *from = NULL;
    uint64_t bar;

    mutex_init(&hh_mutex, "hdmi_audio");
    hh_bound = -1;
    while (pci_find_device_skip(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO, from, &d)) {
        if (HDMI_AUDIO_DRIVES(d.vendor_id, d.device_id)) {
            hh_pci = d;
            hh_found = 1;
            break;
        }
        seen = d;
        from = &seen;
    }
    if (!hh_found) {
        return;
    }
    hh_pci_d0();
    bar = pci_bar_address64(&hh_pci, 0);
    if (bar == 0 || (uint64_t)(uintptr_t)bar != bar) {
        hh_found = 0;
        return;
    }
#ifndef RACH2_HOSTTEST
    hh_base = map_mmio((uintptr_t)bar, HH_MMIO_SIZE);
    if (!hh_base) {
        hh_found = 0;
        return;
    }
#endif
    pci_enable_memory_space(&hh_pci);
    pci_enable_bus_master(&hh_pci);
    (void)acomp_register_audio(&hh_acomp_ops, NULL);
}

/* ---- /proc/asound ---- */

/** Text being built: `len` counts every byte, those past `cap` dropped. */
struct hh_text {
    char *buf;
    int cap;
    int len;
};

/** Append the string `s`. */
static void put_s(struct hh_text *t, const char *s)
{
    while (*s) {
        if (t->buf && t->len < t->cap) {
            t->buf[t->len] = *s;
        }
        t->len++;
        s++;
    }
}

/** Append `v` in base 16, or in base 10 its low 32 bits, at least `width`
 * digits; with no 64-bit division, which i686 lacks without libgcc. */
static void put_n(struct hh_text *t, uint64_t v, unsigned base, int width)
{
    char d[24];
    uint32_t w = (uint32_t)v;
    int n = 0;

    do {
        if (base == 16) {
            d[n++] = "0123456789abcdef"[v & 15];
            v >>= 4;
        } else {
            d[n++] = (char)('0' + w % 10u);
            w /= 10u;
            v = w;
        }
    } while (v && n < (int)sizeof(d) - 1);
    while (n < width && n < (int)sizeof(d) - 1) {
        d[n++] = '0';
    }
    while (n > 0) {
        char c[2];

        c[0] = d[--n];
        c[1] = '\0';
        put_s(t, c);
    }
}

/** Append "name<tabs>[0x%x]" for a field shown with its raw value. */
static void put_hexv(struct hh_text *t, const char *name, uint32_t v)
{
    put_s(t, name);
    put_s(t, "[0x");
    put_n(t, v, 16, 1);
    put_s(t, "]");
}

/** Append `v` in base 10 with its sign, as Linux's %d prints it. */
static void put_d(struct hh_text *t, int v)
{
    if (v < 0) {
        put_s(t, "-");
        put_n(t, 0u - (uint32_t)v, 10, 1);
    } else {
        put_n(t, (uint32_t)v, 10, 1);
    }
}

/** The name `names[i]` of a table of `n`, or "reserved" for an index past
 * it, which a written ELD may give where Linux reads past its table. */
static const char *hh_name(const char *const *names, int n, int i)
{
    return i >= 0 && i < n ? names[i] : "reserved";
}

/** Append one short audio descriptor, Linux's hdmi_print_sad_info. */
static void put_sad(struct hh_text *t, int i, const struct hh_sad *a)
{
    static const char *const names[18] = {"undefined",
                                          "LPCM",
                                          "AC-3",
                                          "MPEG1",
                                          "MP3",
                                          "MPEG2",
                                          "AAC-LC",
                                          "DTS",
                                          "ATRAC",
                                          "DSD (One Bit Audio)",
                                          "E-AC-3/DD+ (Dolby Digital Plus)",
                                          "DTS-HD",
                                          "MLP (Dolby TrueHD)",
                                          "DST",
                                          "WMAPro",
                                          "HE-AAC",
                                          "HE-AACv2",
                                          "MPEG Surround"};
    static const unsigned bits[5] = {8, 16, 20, 24, 32};
    int b;

    put_s(t, "sad");
    put_n(t, (uint32_t)i, 10, 1);
    put_hexv(t, "_coding_type\t", (uint32_t)a->format);
    put_s(t, " ");
    put_s(t, hh_name(names, 18, a->format));
    put_s(t, "\nsad");
    put_n(t, (uint32_t)i, 10, 1);
    put_s(t, "_channels\t\t");
    put_d(t, a->channels);
    put_s(t, "\nsad");
    put_n(t, (uint32_t)i, 10, 1);
    put_hexv(t, "_rates\t\t", (uint32_t)a->rates);
    for (b = 0; b < 14; b++) {
        if (a->rates & (1 << b)) {
            put_s(t, " ");
            put_n(t, alsa_rates[b], 10, 1);
        }
    }
    put_s(t, "\n");
    if (a->format == 1) {
        put_s(t, "sad");
        put_n(t, (uint32_t)i, 10, 1);
        put_hexv(t, "_bits\t\t", (uint32_t)a->bits);
        for (b = 0; b < 5; b++) {
            if (a->bits & (1 << (16 + b))) {
                put_s(t, " ");
                put_n(t, bits[b], 10, 1);
            }
        }
        put_s(t, "\n");
    }
    if (a->max_bitrate) {
        put_s(t, "sad");
        put_n(t, (uint32_t)i, 10, 1);
        put_s(t, "_max_bitrate\t");
        put_n(t, (uint32_t)a->max_bitrate, 10, 1);
        put_s(t, "\n");
    }
    if (a->profile) {
        put_s(t, "sad");
        put_n(t, (uint32_t)i, 10, 1);
        put_s(t, "_profile\t\t");
        put_n(t, (uint32_t)a->profile, 10, 1);
        put_s(t, "\n");
    }
}

/** Append entry `p`'s ELD file, Linux's snd_hdmi_print_eld_info
 * (hda_eld.c:442-492). */
static void put_eld(struct hh_text *t, const struct hh_pin *p)
{
    static const char *const conn[4] = {"HDMI", "DisplayPort", "2-reserved", "3-reserved"};
    static const char *const edid[8] = {"no CEA EDID Timing Extension block present",
                                        "CEA-861",
                                        "CEA-861-A",
                                        "CEA-861-B, C or D",
                                        "reserved",
                                        "reserved",
                                        "reserved",
                                        "reserved"};
    static const char *const spk[11] = {"FL/FR",   "LFE",     "FC",      "RL/RR", "RC", "FLC/FRC",
                                        "RLC/RRC", "FLW/FRW", "FLH/FRH", "TC",    "FCH"};
    const struct hh_eld_info *e = &p->info;
    int i;

    put_s(t, "monitor_present\t\t");
    put_d(t, p->monitor_present);
    put_s(t, "\neld_valid\t\t");
    put_d(t, p->eld_valid);
    put_s(t, "\ncodec_pin_nid\t\t0x");
    put_n(t, p->nid, 16, 1);
    put_s(t, "\ncodec_dev_id\t\t0x");
    put_n(t, p->dev, 16, 1);
    put_s(t, "\ncodec_cvt_nid\t\t0x");
    put_n(t, p->cvt, 16, 1);
    put_s(t, "\n");
    if (!p->eld_valid) {
        return;
    }
    put_s(t, "monitor_name\t\t");
    put_s(t, e->monitor_name);
    put_s(t, "\nconnection_type\t\t");
    put_s(t, hh_name(conn, 4, e->conn_type));
    put_hexv(t, "\neld_version\t\t", (uint32_t)e->eld_ver);
    put_s(t, e->eld_ver == 2 ? " CEA-861D or below" : e->eld_ver == 31 ? " partial" : " reserved");
    put_hexv(t, "\nedid_version\t\t", (uint32_t)e->cea_edid_ver);
    put_s(t, " ");
    put_s(t, hh_name(edid, 8, e->cea_edid_ver));
    put_s(t, "\nmanufacture_id\t\t0x");
    put_n(t, (uint32_t)e->manufacture_id, 16, 1);
    put_s(t, "\nproduct_id\t\t0x");
    put_n(t, (uint32_t)e->product_id, 16, 1);
    put_s(t, "\nport_id\t\t\t0x");
    put_n(t, e->port_id, 16, 1);
    put_s(t, "\nsupport_hdcp\t\t");
    put_d(t, e->support_hdcp);
    put_s(t, "\nsupport_ai\t\t");
    put_d(t, e->support_ai);
    put_s(t, "\naudio_sync_delay\t");
    put_d(t, e->aud_synch_delay);
    put_hexv(t, "\nspeakers\t\t", (uint32_t)e->spk_alloc);
    for (i = 0; i < 11; i++) {
        if (e->spk_alloc & (1 << i)) {
            put_s(t, " ");
            put_s(t, spk[i]);
        }
    }
    put_s(t, "\nsad_count\t\t");
    put_d(t, e->sad_count);
    put_s(t, "\n");
    for (i = 0; i < e->sad_count && i < ELD_MAX_SAD; i++) {
        put_sad(t, i, &e->sad[i]);
    }
}

int hdmi_audio_present(void)
{
    return hh_present;
}

int hdmi_audio_eld_count(void)
{
    return hh_present ? hh_npins : 0;
}

int hdmi_audio_codec_addr(void)
{
    return hh_caddr;
}

int hdmi_audio_proc_text(int file, int index, char *buf, int cap)
{
    struct hh_text t;

    t.buf = buf;
    t.cap = cap;
    t.len = 0;
    mutex_lock(&hh_mutex);
    if (!hh_present || file < 0 || file - 2 >= hh_npins) {
        t.len = -ENOENT;
    } else if (file == 0) {
        /* snd_card_info_read: "%2i [%-15s]: %s - %s" and the long
         * name, azx_probe_continue's "%s at 0x%lx irq %i". */
        put_s(&t, index < 10 ? " " : "");
        put_d(&t, index);
        put_s(&t, " [HDMI           ]: HDA-Intel - HDA Intel HDMI\n");
        put_s(&t, "                      HDA Intel HDMI at 0x");
        put_n(&t, pci_bar_address64(&hh_pci, 0), 16, 1);
        put_s(&t, " irq ");
        put_n(&t, hh_pci.irq, 10, 1);
        put_s(&t, "\n");
    } else if (file == 1) {
        put_s(&t, "HDMI\n");
    } else {
        put_eld(&t, &hh_pins[file - 2]);
    }
    mutex_unlock(&hh_mutex);
    return t.len;
}

/** Whether the strings `a` and `b` are equal. */
static int hh_streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Whether `c` is a space as the kernel's isspace has it. */
static int hh_space(char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

/** The value of the hex digit `c`, or -1. */
static int hh_xdigit(char c)
{
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

/**
 * Parse the line `l` as Linux's sscanf(line, "%s %llx", name, &val): a
 * word into `name`, of at most 63 bytes as the line's 64 allow, then a hex
 * number with an optional 0x, its low 64 bits kept. Returns 1 for both,
 * 0 otherwise.
 */
static int hh_eld_line(const char *l, char *name, uint64_t *val)
{
    int k = 0, d;

    while (hh_space(*l)) {
        l++;
    }
    while (*l && !hh_space(*l)) {
        name[k++] = *l++;
    }
    name[k] = '\0';
    if (k == 0) {
        return 0;
    }
    while (hh_space(*l)) {
        l++;
    }
    if (l[0] == '0' && (l[1] == 'x' || l[1] == 'X') && hh_xdigit(l[2]) >= 0) {
        l += 2;
    }
    if (hh_xdigit(*l) < 0) {
        return 0;
    }
    *val = 0;
    while ((d = hh_xdigit(*l)) >= 0) {
        *val = *val << 4 | (uint32_t)d;
        l++;
    }
    return 1;
}

/** Set the field `name` of SAD `n` of `e` to `v`, as Linux's write does,
 * which counts the SAD in sad_count whether or not the field is one. */
static void hh_eld_sad(struct hh_eld_info *e, int n, const char *name, int v)
{
    if (hh_streq(name, "_coding_type")) {
        e->sad[n].format = v;
    } else if (hh_streq(name, "_channels")) {
        e->sad[n].channels = v;
    } else if (hh_streq(name, "_rates")) {
        e->sad[n].rates = v;
    } else if (hh_streq(name, "_bits")) {
        e->sad[n].bits = v;
    } else if (hh_streq(name, "_byte2")) {
        e->sad[n].max_bitrate = v;
    } else if (hh_streq(name, "_profile")) {
        e->sad[n].profile = v;
    }
    if (n >= e->sad_count) {
        e->sad_count = n + 1;
    }
}

/** Set the field a line names in entry `p`, as Linux's
 * snd_hdmi_write_eld_info does; the monitor's name, IDs and versions are
 * not set, and a line of any other name is passed over. */
static void hh_eld_field(struct hh_pin *p, const char *name, uint64_t val)
{
    struct hh_eld_info *e = &p->info;
    int v = (int)(uint32_t)val, n;

    if (hh_streq(name, "monitor_present")) {
        p->monitor_present = v;
    } else if (hh_streq(name, "eld_valid")) {
        p->eld_valid = v;
    } else if (hh_streq(name, "connection_type")) {
        e->conn_type = v;
    } else if (hh_streq(name, "port_id")) {
        e->port_id = val;
    } else if (hh_streq(name, "support_hdcp")) {
        e->support_hdcp = v;
    } else if (hh_streq(name, "support_ai")) {
        e->support_ai = v;
    } else if (hh_streq(name, "audio_sync_delay")) {
        e->aud_synch_delay = v;
    } else if (hh_streq(name, "speakers")) {
        e->spk_alloc = v;
    } else if (hh_streq(name, "sad_count")) {
        e->sad_count = v;
    } else if (name[0] == 's' && name[1] == 'a' && name[2] == 'd') {
        n = name[3] - '0';
        name += 4;
        if (name[-1] != '\0' && name[0] >= '0' && name[0] <= '9') {
            n = 10 * n + name[0] - '0';
            name++;
        }
        if (n >= 0 && n < ELD_MAX_SAD) {
            hh_eld_sad(e, n, name, v);
        }
    }
}

int hdmi_audio_eld_write(int n, const char *text, int len)
{
    char line[64], name[64];
    uint64_t val;
    int i = 0, k;

    mutex_lock(&hh_mutex);
    if (!hh_present || n < 0 || n >= hh_npins) {
        mutex_unlock(&hh_mutex);
        return -ENOENT;
    }
    while (i < len) {
        /* snd_info_get_line: a line of at most 63 bytes, the rest of a
         * longer one passed over. */
        k = 0;
        while (i < len && text[i] != '\n') {
            if (k < (int)sizeof(line) - 1) {
                line[k++] = text[i];
            }
            i++;
        }
        i++;
        line[k] = '\0';
        if (hh_eld_line(line, name, &val)) {
            hh_eld_field(&hh_pins[n], name, val);
        }
    }
    mutex_unlock(&hh_mutex);
    return 0;
}
