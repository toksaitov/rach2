/*
 * hdmi_audio_hosttest.c: Haswell's display audio (audio/hda_hdmi.c) against
 * a model of the controller and of the HDMI codec 8086:2807, bound through
 * the audio component to a display side that gives IGT's ELD on port B.
 *
 * host-deps: rach2/sys/lib/kstring.c tests/host/compat/mutex_none.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/kirq_none.c
 * host-deps: rach2/sys/lib/kctypes.c
 *
 * The rows: the bind, which resets the link, finds the codec's three
 * converters and three HDMI pins of three device entries each, sets the
 * vendor bits and the BCLK, and reads each entry's ELD; /proc/asound's
 * cards, id and eld#0.N in Linux's text, which IGT's kms_hdmi_inject
 * inject-audio reads for monitor_present, eld_valid and monitor_name; a
 * stream opened, granted, started and served from the tick, with the pin's
 * converter selected, the display's clocks set for the rate, the stereo
 * infoframe and channel slots, IEC958 on and the stream's tag and format;
 * 24 bits granted where the sink takes them; a monitor that comes while a
 * stream plays given the stream; and the close, which gives the display's
 * power back.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "audio/hda_hdmi.c"
#include "audio/acomp.c"

extern int printf(const char *format, ...);
extern int memcmp(const void *a, const void *b, unsigned long n);
extern char *strstr(const char *text, const char *needle);

static int failures;

/** Count a failed row and name it. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("hdmi-audio-test: FAIL: %s\n", what);
        failures++;
    }
}

/* ---- the controller ---- */

static uint8_t mmio[HH_MMIO_SIZE];

/** The register at `off`, `bits` wide, from the model's bytes. */
static uint32_t mm_get(uint32_t off, int bits)
{
    uint32_t v = 0;
    int i;

    for (i = 0; i < bits / 8; i++) {
        v |= (uint32_t)mmio[off + (uint32_t)i] << (8 * i);
    }
    return v;
}

/** Store `v` in the register at `off`, `bits` wide. */
static void mm_put(uint32_t off, uint32_t v, int bits)
{
    int i;

    for (i = 0; i < bits / 8; i++) {
        mmio[off + (uint32_t)i] = (uint8_t)(v >> (8 * i));
    }
}

/** The driver's register read, from the model. */
uint32_t hdmi_test_read(uint32_t offset, int bits)
{
    return mm_get(offset, bits);
}

/** The driver's register write: STATESTS clears the bits written, leaving
 * reset finds the codec, and the ring pointers' resets read back. */
void hdmi_test_write(uint32_t offset, uint32_t value, int bits)
{
    if (offset == HDA_STATESTS) {
        mm_put(offset, mm_get(offset, 16) & ~value, 16);
        return;
    }
    if (offset == HDA_GCTL && (value & HDA_GCTL_CRST) && !(mm_get(HDA_GCTL, 32) & 1)) {
        mm_put(HDA_STATESTS, 1, 16); /* the codec at address 0 answers */
    }
    if (offset == HDA_RIRBWP || offset == HDA_CORBRP) {
        /* The read pointer's reset reads back while it is asserted. */
        mm_put(offset, offset == HDA_CORBRP ? value : 0, 16);
        return;
    }
    mm_put(offset, value, bits);
}

/* ---- the codec ---- */

#define NODES 9

static uint32_t vendor_bits;
static uint8_t power[NODES];
static uint8_t conn_sel[NODES];
static uint8_t pin_ctl[NODES];
static uint16_t amp[NODES];
static uint8_t streamid[NODES];
static uint16_t format[NODES];
static uint8_t chan_cnt[NODES];
static uint8_t digi1[NODES], digi2[NODES];
static uint8_t slots[NODES][8];
static int nslots[NODES];
static uint8_t dip[NODES][16];
static int dip_at[NODES];
static uint8_t dip_xmit[NODES];
static int verbs;

/** The codec's parameter `p` of node `nid`: the root, the audio function
 * at 1, converters 2 to 4, HDMI pins 5 to 7, and the vendor widget 8. */
static uint32_t model_param(int nid, uint32_t p)
{
    if (nid == 0) {
        return p == P_VENDOR ? 0x80862807u : p == P_SUBNODES ? 0x00010001u : 0;
    }
    if (nid == 1) {
        return p == P_FG_TYPE ? 1 : p == P_SUBNODES ? 0x00020007u : 0;
    }
    if (nid >= 2 && nid <= 4) {
        return p == P_WCAPS ? 0x00000601u : p == P_PCM ? 0x000e07f0u : 0;
    }
    if (nid >= 5 && nid <= 7) {
        return p == P_WCAPS ? 0x00400705u : p == P_PINCAP ? 0x01000090u : p == P_CONN_LEN ? 3u : 0;
    }
    return p == P_WCAPS ? 0x00f00000u : 0;
}

/** The codec's answer to the command word `cmd`, keeping what it sets. */
uint32_t hdmi_test_verb(uint32_t cmd)
{
    int nid = (int)((cmd >> 20) & 0xff);
    uint32_t verb = (cmd >> 8) & 0xfff, parm = cmd & 0xff;

    verbs++;
    if (nid >= NODES || (cmd >> 28) != 0) {
        return ~0u;
    }
    if ((cmd & 0xf0000) == 0x20000) { /* Set Converter Format */
        format[nid] = (uint16_t)(cmd & 0xffff);
        return 0;
    }
    if ((cmd & 0xf0000) == 0x30000) { /* Set Amplifier Gain/Mute */
        amp[nid] = (uint16_t)(cmd & 0xffff);
        return 0;
    }
    switch (verb) {
    case V_GET_PARAM:
        return model_param(nid, parm);
    case V_GET_CONN_LIST:
        return 0x00040302u;
    case V_GET_CONN_SEL:
        return conn_sel[nid];
    case V_GET_CONFIG:
        return 0x18560010u;
    case V_GET_POWER:
        return (uint32_t)power[nid] << 4 | power[nid];
    case V_SET_POWER:
        power[nid] = (uint8_t)parm;
        return 0;
    case V_SET_CONN_SEL:
        conn_sel[nid] = (uint8_t)parm;
        return 0;
    case V_SET_PIN_CTL:
        pin_ctl[nid] = (uint8_t)parm;
        return 0;
    case V_SET_STREAMID:
        streamid[nid] = (uint8_t)parm;
        return 0;
    case V_SET_CHAN_CNT:
        chan_cnt[nid] = (uint8_t)parm;
        return 0;
    case V_SET_DIGI1:
        digi1[nid] = (uint8_t)parm;
        return 0;
    case V_SET_DIGI2:
        digi2[nid] = (uint8_t)parm;
        return 0;
    case V_SET_CHAN_SLOT:
        if (nslots[nid] < 8) {
            slots[nid][nslots[nid]++] = (uint8_t)parm;
        }
        return 0;
    case V_SET_DIP_INDEX:
        dip_at[nid] = 0;
        return 0;
    case V_SET_DIP_DATA:
        dip[nid][dip_at[nid]++ & 15] = (uint8_t)parm;
        return 0;
    case V_GET_DIP_DATA:
        return dip[nid][dip_at[nid]++ & 15];
    case V_SET_DIP_XMIT:
        dip_xmit[nid] = (uint8_t)parm;
        return 0;
    case V_GET_DIP_XMIT:
        return dip_xmit[nid];
    case V_INTEL_GET:
        return vendor_bits;
    case V_INTEL_SET:
        vendor_bits = parm;
        return vendor_bits;
    default:
        return 0;
    }
}

/* ---- the display ---- */

/* IGT's ELD as drm_edid_to_eld makes it from igt_audio_edid.h: version 2,
 * six words of baseline, the name IGT, CEA-861-B, one LPCM SAD of two
 * channels at 32 to 48 kHz in 16 to 24 bits, and FLC/FRC. */
static const uint8_t igt_eld[28] = {0x10, 0x00, 0x06, 0x00, 0x63, 0x10, 0x00, 0x20, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0xf4, 0x00, 0x00,
                                    'I',  'G',  'T',  0x09, 0x07, 0x07, 0x00, 0x00};

static int display_power, display_powers;
static int sink_on = 1; /* the display plays audio on port B */
static int rate_port, rate_set;

/** The display's get_power. */
static int32_t d_get_power(void *dev)
{
    (void)dev;
    display_power++;
    display_powers++;
    return 0;
}

/** The display's put_power. */
static void d_put_power(void *dev)
{
    (void)dev;
    display_power--;
}

/** The display's CD clock. */
static int32_t d_cdclk(void *dev)
{
    (void)dev;
    return 450000;
}

/** The display's sync_audio_rate: kept. */
static int32_t d_rate(void *dev, int port, int pipe, int rate)
{
    (void)dev;
    (void)pipe;
    rate_port = port;
    rate_set = rate;
    return 0;
}

/** The display's get_eld: IGT's on port B, entry 0, while it plays audio. */
static int32_t d_eld(void *dev, int port, int pipe, int *enabled, uint8_t *buf, int max)
{
    (void)dev;
    *enabled = 0;
    if (port != 1 || pipe > 0 || !sink_on) {
        return -EINVAL;
    }
    *enabled = 1;
    k_memcpy(buf, igt_eld, (uint32_t)(max < 28 ? max : 28));
    return 28;
}

static const struct acomp_ops disp_ops = {d_get_power, d_put_power, d_cdclk, d_rate, d_eld};
static int disp_dev;

/* ---- the kernel ---- */

static int transfers, periods;
static uint32_t last_size;

/** The stream's transfer: fill with a byte the count names. */
static void s_transfer(audio_stream_t *s, uint8_t *buf, size_t size)
{
    (void)s;
    k_memset(buf, (uint8_t)transfers, size);
    transfers++;
    last_size = (uint32_t)size;
}

/** The stream's period. */
static void s_period(audio_stream_t *s)
{
    (void)s;
    periods++;
}

/** A stream asking for `rate`, `fmt` and a period of `pb` bytes. */
static void stream_init(audio_stream_t *s, uint32_t rate, audio_format_t fmt, uint32_t pb)
{
    k_memset(s, 0, sizeof(*s));
    s->dir = AUDIO_PLAYBACK;
    s->rate = rate;
    s->format = fmt;
    s->channels = 2;
    s->period_bytes = pb;
    s->periods = 4;
    s->transfer = s_transfer;
    s->period = s_period;
}

/** The text of /proc/asound's file `file`. */
static const char *proc(int file)
{
    static char buf[2048];
    int n = hdmi_audio_proc_text(file, buf, (int)sizeof(buf) - 1);

    buf[n < 0 ? 0 : n] = '\0';
    return buf;
}

/* ---- the rows ---- */

/** The bind: the link reset and the codec found, its vendor bits, the
 * BCLK of a 450 MHz CD clock, the pins up and the ELDs read, and the
 * display's power back. */
static void row_bind(void)
{
    mm_put(HDA_GCAP, 0x4401, 16); /* four output streams after four input */
    hdmi_audio_init();
    check(hh_found && !hdmi_audio_present(), "bind: the controller found, nothing up yet");
    check(acomp_register_display(&disp_ops, &disp_dev) == 0 && hdmi_audio_present(),
          "bind: the display's registration brings the card up");
    check(hh_ncvts == 3 && hh_npins == 9 && hdmi_audio_eld_count() == 9 && hh_out_sd == 4 &&
              hh_cvts[0].nid == 2 && hh_pins[0].nid == 5 && hh_pins[8].nid == 7 &&
              hh_pins[8].dev == 2 && hh_pins[3].nmux == 3 && hh_pins[3].mux[2] == 4,
          "bind: three converters, three pins of three entries, each selecting the three");
    check(vendor_bits == (INTEL_EN_ALL_PIN_CVTS | INTEL_EN_DP12),
          "bind: all pins and converters shown, DP 1.2 on");
    check(mm_get(HH_EM4, 16) == 4 && mm_get(HH_EM5, 16) == 75,
          "bind: the BCLK divided 4/75 from 450 MHz");
    check(pin_ctl[5] == PIN_OUT && pin_ctl[7] == PIN_OUT && amp[6] == AMP_OUT_UNMUTE &&
              power[1] == 0,
          "bind: the pins' outputs on and unmuted");
    check(hh_pins[0].eld_valid && hh_pins[0].monitor_present && !hh_pins[1].eld_valid &&
              !hh_pins[3].eld_valid && display_power == 0 && display_powers == 1,
          "bind: port B's entry 0 valid, the others not, the display's power given back");
}

/** /proc/asound in Linux's text. */
static void row_proc(void)
{
    static const char eld0[] = "monitor_present\t\t1\n"
                               "eld_valid\t\t1\n"
                               "codec_pin_nid\t\t0x5\n"
                               "codec_dev_id\t\t0x0\n"
                               "codec_cvt_nid\t\t0x0\n"
                               "monitor_name\t\tIGT\n"
                               "connection_type\t\tHDMI\n"
                               "eld_version\t\t[0x2] CEA-861D or below\n"
                               "edid_version\t\t[0x3] CEA-861-B, C or D\n"
                               "manufacture_id\t\t0xf424\n"
                               "product_id\t\t0x0\n"
                               "port_id\t\t\t0x0\n"
                               "support_hdcp\t\t0\n"
                               "support_ai\t\t0\n"
                               "audio_sync_delay\t0\n"
                               "speakers\t\t[0x20] FLC/FRC\n"
                               "sad_count\t\t1\n"
                               "sad0_coding_type\t[0x1] LPCM\n"
                               "sad0_channels\t\t2\n"
                               "sad0_rates\t\t[0xe0] 32000 44100 48000\n"
                               "sad0_bits\t\t[0xe0000] 16 20 24\n";
    static const char eld4[] = "monitor_present\t\t0\n"
                               "eld_valid\t\t0\n"
                               "codec_pin_nid\t\t0x6\n"
                               "codec_dev_id\t\t0x1\n"
                               "codec_cvt_nid\t\t0x0\n";

    check(strstr(proc(0), " 0 [HDMI           ]: HDA-Intel - HDA Intel HDMI\n"
                          "                      HDA Intel HDMI at 0xf7e34000 irq 11\n") == proc(0),
          "proc: cards names the card as Linux does");
    check(memcmp(proc(1), "HDMI\n", 6) == 0, "proc: the card's id");
    check(memcmp(proc(2), eld0, sizeof(eld0)) == 0, "proc: eld#0.0, IGT's monitor, as Linux's");
    check(memcmp(proc(6), eld4, sizeof(eld4)) == 0, "proc: eld#0.4, pin 6's entry 1, empty");
    check(hdmi_audio_proc_text(11, NULL, 0) == -ENOENT &&
              hdmi_audio_proc_text(2, NULL, 0) == (int)sizeof(eld0) - 1,
          "proc: no eld#0.9; a length measured without a buffer");
}

static audio_stream_t played; /* the stream of row_stream and row_play */

/** A stream at 44.1 kHz in 16 bits: granted as asked, port B's converter
 * selected, its clocks set, the infoframe and slots sent, IEC958 on, and
 * the stream's tag and format. */
static void row_stream(void)
{
    static const uint8_t ai[9] = {0x84, 0x01, 0x0a, 0x70, 0x01, 0, 0, 0, 0};
    static const uint8_t slot[8] = {0x00, 0x11, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7};
    audio_stream_t *sp = &played;
    uint32_t ctl;

    stream_init(sp, 44100, AUDIO_S16LE, 4096);
    check(hh_stream_open(sp) == 0 && display_power == 1 && hh_bound == 0 && hh_pins[0].cvt == 2 &&
              conn_sel[5] == 0,
          "stream: open holds the display's power and gives port B the first converter");
    check(digi1[2] == 0x81 && digi2[2] == 0x02, "stream: IEC958 on, original PCM");
    check(hh_stream_setup(sp) == 0 && sp->rate == 44100 && sp->format == AUDIO_S16LE &&
              sp->channels == 2 && sp->period_bytes == 4096 && sp->device_periods == 2 &&
              hh_fmt == 0x4011,
          "stream: 44.1 kHz, 16 bits, stereo, two periods of 4096 bytes");
    transfers = 0;
    check(hh_stream_start(sp) == 0 && transfers == 2 && last_size == 4096,
          "stream: start fills both periods");
    ctl = mm_get(hh_sd(HDA_SD_CTL), 32);
    check((ctl & HDA_SD_CTL_RUN) && (ctl >> 20) == HH_TAG &&
              mm_get(hh_sd(HDA_SD_FORMAT), 16) == 0x4011 && mm_get(hh_sd(HDA_SD_CBL), 32) == 8192 &&
              mm_get(hh_sd(HDA_SD_LVI), 16) == 1 && hh_bdl[1].length == 4096,
          "stream: the descriptor runs with tag 1, the format and two periods");
    check(rate_port == 1 && rate_set == 44100, "stream: the display's clocks set for 44.1 kHz");
    check(chan_cnt[2] == 1 && nslots[5] == 8 && memcmp(slots[5], slot, 8) == 0,
          "stream: two channels counted, channel allocation 0's slots");
    check(memcmp(dip[5], ai, 9) == 0 && dip_xmit[5] == DIPXMIT_BEST,
          "stream: HDMI's stereo audio infoframe, sent at best effort");
    check(streamid[2] == HH_TAG << 4 && format[2] == 0x4011,
          "stream: the converter has the tag and the format");
    check(strstr(proc(2), "codec_cvt_nid\t\t0x2\n") != NULL,
          "stream: eld#0.0 names the converter while it plays");
}

/** row_stream's stream served from the tick round its buffer, stopped and
 * closed. */
static void row_play(void)
{
    audio_stream_t *sp = &played;
    uint32_t pf = 4096 / 4;

    periods = 0;
    mm_put(hh_sd(HDA_SD_LPIB), 4096 + 2048, 32);
    hdmi_audio_tick();
    check(periods == 1 && transfers == 3 && hh_stream_position(sp) == pf + pf / 2 &&
              hh_stream_delay(sp) == pf + pf / 2 && hdmi_audio_idle_ticks() == 1,
          "stream: the tick refills the period played and counts it");
    mm_put(hh_sd(HDA_SD_LPIB), 1024, 32);
    hdmi_audio_tick();
    check(periods == 2 && hh_stream_position(sp) == 2 * pf + 256,
          "stream: the position goes on round the buffer");
    hh_stream_stop(sp);
    check(!(mm_get(hh_sd(HDA_SD_CTL), 8) & HDA_SD_CTL_RUN) && streamid[2] == 0 &&
              hdmi_audio_idle_ticks() == TICK_HZ,
          "stream: stop halts the descriptor and clears the converter's tag");
    hh_stream_close(sp);
    check(display_power == 0 && !hh_stream && !hh_cvts[0].assigned,
          "stream: close gives the converter and the display's power back");
}

/** A wider format asked for: 24 bits in 32, at 48 kHz, since the sink's
 * LPCM takes 24. */
static void row_wide(void)
{
    audio_stream_t s;

    stream_init(&s, 48000, AUDIO_S24LE, 16384);
    check(hh_stream_open(&s) == 0 && hh_stream_setup(&s) == 0 && s.format == AUDIO_S32LE &&
              s.rate == 48000 && hh_fmt == 0x0031 && s.period_bytes == 16384 &&
              s.device_periods == 2,
          "wide: S32LE at 48 kHz, two periods of 16384 bytes");
    hh_stream_close(&s);
    stream_init(&s, 22050, AUDIO_U8, 128);
    check(hh_stream_open(&s) == 0 && hh_stream_setup(&s) == 0 && s.format == AUDIO_S16LE &&
              s.rate == 32000 && s.period_bytes == 128 && s.device_periods == 40,
          "wide: 22050 Hz becomes the sink's nearest, 32 kHz, in 16 bits");
    hh_stream_close(&s);
}

/** A monitor that comes while a stream plays on no pin is given it: the
 * converter selected, the clocks and infoframe sent, as update_eld does. */
static void row_notify(void)
{
    audio_stream_t s;

    sink_on = 0;
    acomp_eld_notify(1, -1);
    check(!hh_pins[0].eld_valid && strstr(proc(2), "eld_valid\t\t0\n") != NULL,
          "notify: the display's disable empties port B's ELD");
    stream_init(&s, 48000, AUDIO_S16LE, 4096);
    check(hh_stream_open(&s) == 0 && hh_bound < 0 && hh_stream_setup(&s) == 0 &&
              hh_stream_start(&s) == 0,
          "notify: a stream plays on no pin");
    k_memset(dip[5], 0, sizeof(dip[5]));
    rate_set = 0;
    conn_sel[5] = 1;
    sink_on = 1;
    acomp_eld_notify(1, -1);
    check(hh_bound == 0 && hh_pins[0].eld_valid && conn_sel[5] == 0 && rate_set == 48000 &&
              dip[5][0] == 0x84 && hh_pins[0].setup,
          "notify: the monitor's coming gives it the stream");
    hh_stream_close(&s);
    check(display_power == 0 && hh_pins[0].cvt == 0, "notify: the close leaves nothing held");
}

/** The display going unbinds the card. */
static void row_unbind(void)
{
    acomp_unregister_display(&disp_dev);
    check(!hdmi_audio_present() && hdmi_audio_eld_count() == 0 &&
              hdmi_audio_proc_text(1, NULL, 0) == -ENOENT &&
              strstr(proc(0), "no soundcards") != NULL,
          "unbind: the card goes with the display");
}

int main(void)
{
    row_bind();
    row_proc();
    row_stream();
    row_play();
    row_wide();
    row_notify();
    row_unbind();
    if (failures == 0) {
        printf("hdmi-audio-test: ok\n");
    }
    return failures != 0;
}

/* ---- the kernel's functions the driver calls ---- */

void klog(int level, const char *component, const char *fmt, ...)
{
    (void)level;
    (void)component;
    (void)fmt;
}

void k_udelay(uint32_t us)
{
    (void)us;
}

void arch_dma_flush(const volatile void *p, size_t len)
{
    (void)p;
    (void)len;
}

uint64_t rtc_ticks_now(void)
{
    return 0;
}

uint8_t pci_read_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    (void)bus;
    (void)device;
    (void)function;
    return offset == HH_PCI_TCSEL ? 0x07 : 0;
}

uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    (void)bus;
    (void)device;
    (void)function;
    return offset == HH_PCI_DEVC ? HH_DEVC_NOSNOOP : 0;
}

static uint8_t tcsel = 0xff;
static uint16_t devc = 0xffff;

void pci_write_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t value)
{
    (void)bus;
    (void)device;
    (void)function;
    if (offset == HH_PCI_TCSEL) {
        tcsel = value;
    }
}

void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint16_t value)
{
    (void)bus;
    (void)device;
    (void)function;
    if (offset == HH_PCI_DEVC) {
        devc = value;
    }
}

bool pci_find_device_skip(uint8_t class_code, uint8_t subclass, pci_device_t *skip_dev,
                          pci_device_t *out)
{
    (void)class_code;
    (void)subclass;
    if (skip_dev) {
        return false;
    }
    k_memset(out, 0, sizeof(*out));
    out->vendor_id = 0x8086;
    out->device_id = 0x0c0c;
    out->device = 3;
    out->irq = 11;
    return true;
}

uint64_t pci_bar_address64(const pci_device_t *dev, int i)
{
    (void)dev;
    (void)i;
    return 0xf7e34000u;
}

/* The function's power state, D0 unless a row sets the firmware's D3hot;
 * whether its header was saved before a change of state and written back
 * after; and the decodes enabled while it was not in D0. */
static int pci_d_state;
static int pci_saved, pci_restored, pci_enabled_asleep;

int pci_power_state(const pci_device_t *dev)
{
    (void)dev;
    return pci_d_state;
}

int pci_set_power_state(const pci_device_t *dev, int state)
{
    (void)dev;
    pci_d_state = state;
    return 0;
}

void pci_save_state(const pci_device_t *dev, struct pci_saved_state *st)
{
    (void)dev;
    (void)st;
    pci_saved = pci_d_state != PCI_D0;
}

void pci_restore_state(const pci_device_t *dev, const struct pci_saved_state *st)
{
    (void)dev;
    (void)st;
    pci_restored = pci_saved && pci_d_state == PCI_D0;
}

void pci_enable_memory_space(pci_device_t *dev)
{
    (void)dev;
    pci_enabled_asleep += pci_d_state != PCI_D0;
}

void pci_enable_bus_master(pci_device_t *dev)
{
    (void)dev;
    pci_enabled_asleep += pci_d_state != PCI_D0;
}

int32_t devfs_node_register(struct devfs_node *node)
{
    (void)node;
    return 0;
}

void devfs_node_unregister(struct devfs_node *node)
{
    (void)node;
}

int32_t dspdev_open_driver(fd_entry_t *e, int flags, struct audio_driver *drv)
{
    (void)e;
    (void)flags;
    (void)drv;
    return 0;
}

int32_t dspdev_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    (void)count;
    return 0;
}

int32_t dspdev_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    (void)count;
    return 0;
}

short dspdev_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    (void)e;
    (void)events;
    (void)wq;
    return 0;
}

int32_t dspdev_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    (void)e;
    (void)request;
    (void)arg;
    return 0;
}

void dspdev_close(fd_entry_t *e)
{
    (void)e;
}
