/**
 * HDA enumeration and stream programming against a model codec and MMIO,
 * the mix (vchan.c) played through the driver's stream at 24 bits on a codec
 * that has them and at 16 on one that has not, the driver's report as
 * /proc/audio's text (audio_proc.c) gives it, and the console's tones
 * through the PC speaker (arch/x86/speaker.c) and the codec's beep
 * generator.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/audio/resample.c
 */
#include "audio/hda.c"
#include "audio/audio_proc.c"
#include "audio/vchan.c"
#include "arch/x86/speaker.c"

extern int printf(const char *format, ...);
extern char *strstr(const char *text, const char *needle);
extern int memcmp(const void *a, const void *b, unsigned long n);
static int failures;
static const char *row; /* the row under way, named in a failure */
/* The model: the controller's registers, and for each node its parameters,
 * connection list, selected input, converter format, pin configuration and
 * the payload of the last verb that set its output amplifier; the
 * connection-list reads, the verbs, and the list reads at an offset that is
 * not the start of a response. */
static uint8_t mmio[HDA_MMIO_SIZE];
static uint32_t params[128][32];
static uint16_t connections[128][127];
static uint8_t selected[128];
static uint16_t converter_format[128];
static uint32_t configurations[128];
static uint16_t out_amp[128];
static uint32_t sense[128]; /* each pin's Get Pin Sense word */
/* Each node's input amplifiers as the last verbs set them, by index and
 * channel (0 left, 1 right): the payload's low byte, mute and gain, with
 * 0x100 once one is set; each pin's widget control and each converter's
 * stream and channel. */
static uint16_t in_amp[128][16][2];
static uint8_t pin_ctl[128];
static uint8_t channel[128];
static uint16_t eapd[128]; /* each pin's EAPD/BTL payload, with 0x100 once set */
/* Each node's Set Beep Generation and Set Power State payloads, with 0x100
 * once set, and the beep verbs sent. */
static uint16_t beep_ctl[128], power[128];
static unsigned beep_verbs;
/* The PC speaker's ports: port 0x61, and the PIT channel 2's count with the
 * bytes written to it. */
static uint8_t port61;
static uint16_t pit_count;
static unsigned pit_bytes;
volatile uint64_t kernel_ticks; /* the tick count, which `hda_tick` counts by */
static unsigned conn_reads, verbs, bad_offsets, sense_triggers;
static int codec_silent; /* the codec takes commands and never answers */
/* The command the codec refuses, ending it with no response, when its node
 * and verb bits under fail_mask are fail_command; none while fail_mask is 0. */
static uint32_t fail_command, fail_mask;
static int clock_stopped; /* the wall clock does not advance */
/* The interrupt flag kirq_save and kirq_restore keep; the delays made and
 * those made with interrupts off; the reads of ICS with interrupts on. */
static int irqs_on = 1;
static unsigned delays, delays_irqs_off, ics_reads_irqs_on;
static int refuse_alloc; /* kmalloc fails */
extern void *malloc(unsigned long size);
extern void free(void *ptr);
static uint32_t clock_value; /* the wall clock, 24000 ticks on at each read */
/* The model of DMA progress: with dma_model set, the wall clock moves only
 * with model_play, which advances it and the stream's position together and
 * keeps the left sample of each frame the DMA passes in `played`. */
static int dma_model;
static unsigned long long model_us, model_frames;
static uint32_t model_pos; /* bytes into the DMA buffer */
static int16_t played[1 << 17];
static unsigned played_count;
static unsigned clock_reads, console_writes;
/* The verbs the codec took in order, each with the wall clock when it came,
 * and how many it had taken, and the clock, when the input stream's
 * descriptor was set running; run_at is ~0 until then. */
#define VERB_LOG 512
static uint32_t log_command[VERB_LOG], log_clock[VERB_LOG];
static unsigned log_count, run_at;
static uint32_t run_clock;
static unsigned mmio_reads, mmio_writes;

/** Report the row and assertion that failed. */
static void check(int ok, const char *message)
{
    if (!ok) {
        printf("hda-test: fail: %s: %s\n", row, message);
        failures++;
    }
}

/** Answer Get Connection List Entry at OFFSET from node NID's packed list. */
static uint32_t model_conn_list(unsigned nid, unsigned offset)
{
    unsigned width = params[nid][HDA_PARAM_CONN_LIST] & 128 ? 16 : 8;
    unsigned count = 32 / width;
    uint32_t result = 0;
    unsigned i;

    conn_reads++;
    if (offset % count != 0)
        bad_offsets++;
    for (i = 0; i < count && offset + i < (params[nid][HDA_PARAM_CONN_LIST] & 127); i++)
        result |= (uint32_t)connections[nid][offset + i] << (width * i);
    return result;
}

/** Record a set verb's payload, OFFSET its low byte, in node NID's state. */
static void model_set(unsigned nid, unsigned verb, unsigned offset)
{
    if (verb == HDA_VERB_SET_PIN_SENSE)
        sense_triggers++;
    if ((verb & 0xFFF00) == HDA_VERB_SET_CONN_SEL)
        selected[nid] = offset;
    if ((verb & 0xF0000) == HDA_VERB_SET_CONV_FMT)
        converter_format[nid] = (uint16_t)verb;
    if ((verb & 0xF0000) == HDA_VERB_SET_AMP_GAIN && (verb & 0x8000))
        out_amp[nid] = (uint16_t)verb;
    if ((verb & 0xF0000) == HDA_VERB_SET_AMP_GAIN && (verb & 0x4000)) {
        if (verb & 0x2000)
            in_amp[nid][(verb >> 8) & 15][0] = (uint16_t)(0x100 | (verb & 0xFF));
        if (verb & 0x1000)
            in_amp[nid][(verb >> 8) & 15][1] = (uint16_t)(0x100 | (verb & 0xFF));
    }
    if ((verb & 0xFFF00) == HDA_VERB_SET_PIN_CTRL)
        pin_ctl[nid] = (uint8_t)offset;
    if ((verb & 0xFFF00) == HDA_VERB_SET_CHANNEL)
        channel[nid] = (uint8_t)offset;
    if ((verb & 0xFFF00) == HDA_VERB_SET_EAPD)
        eapd[nid] = (uint16_t)(0x100 | offset);
    if ((verb & 0xFFF00) == HDA_VERB_SET_BEEP) {
        beep_ctl[nid] = (uint16_t)(0x100 | offset);
        beep_verbs++;
    }
    if ((verb & 0xFFF00) == HDA_VERB_SET_POWER)
        power[nid] = (uint16_t)(0x100 | offset);
}

/** Answer verbs from an independently packed codec connection list. */
static uint32_t model_verb(uint32_t command)
{
    unsigned nid = (command >> 20) & 0x7F;
    unsigned verb = command & 0xFFFFF;
    unsigned offset = verb & 255;

    verbs++;
    if (log_count < VERB_LOG) {
        log_command[log_count] = command & 0xFFFFFFF;
        log_clock[log_count++] = clock_value;
    }
    check(!(command & (1u << 27)), "no indirect or truncated node verb");
    if ((verb & 0xFFF00) == HDA_VERB_GET_PARAM)
        return params[nid][offset & 31];
    if ((verb & 0xFFF00) == HDA_VERB_GET_CONN_LIST)
        return model_conn_list(nid, offset);
    if (verb == HDA_VERB_GET_CONFIG_DEF)
        return configurations[nid];
    if (verb == HDA_VERB_GET_CONN_SEL)
        return selected[nid];
    if (verb == HDA_VERB_GET_PIN_SENSE)
        return sense[nid];
    model_set(nid, verb, offset);
    if (verb == 0xA0000)
        return converter_format[nid];
    return 0;
}

/** Supply byte-addressed MMIO and an advancing controller clock. */
uint32_t hda_test_read(uint32_t offset, int bits)
{
    uint32_t value = 0;
    int i;

    mmio_reads++;
    if (offset == HDA_WALCLK) {
        clock_reads++;
        return clock_stopped || dma_model ? clock_value : (clock_value += 24000);
    }
    if (offset == HDA_ICS && irqs_on)
        ics_reads_irqs_on++;
    for (i = 0; i < bits / 8; i++)
        value |= (uint32_t)mmio[offset + i] << (i * 8);
    return value;
}

/** Complete immediate commands when the driver sets ICB. */
void hda_test_write(uint32_t offset, uint32_t value, int bits)
{
    int i;

    mmio_writes++;
    if (offset == HDA_ICS) {
        if (value & HDA_ICS_IRV)
            mmio[HDA_ICS] &= ~HDA_ICS_IRV;
        if (value & HDA_ICS_ICB) {
            uint32_t command = hda_test_read(HDA_ICW, 32);

            if (fail_mask && (command & fail_mask) == fail_command) {
                mmio[HDA_ICS] = 0;
            } else if (codec_silent) {
                model_verb(command);
                mmio[HDA_ICS] |= HDA_ICS_ICB;
            } else {
                hda_test_write(HDA_IRR, model_verb(command), 32);
                mmio[HDA_ICS] = HDA_ICS_IRV;
            }
        }
        return;
    }
    /* A reset of the input stream's descriptor zeroes its position, and its
     * run bit set is logged. */
    if (offset == 0x80 + HDA_SD_CTL && (value & HDA_SD_CTL_SRST))
        k_memset(&mmio[0x80 + HDA_SD_LPIB], 0, 4);
    if (offset == 0x80 + HDA_SD_CTL && (value & HDA_SD_CTL_RUN) && run_at == ~0u) {
        run_at = log_count;
        run_clock = clock_value;
    }
    for (i = 0; i < bits / 8; i++)
        mmio[offset + i] = (uint8_t)(value >> (i * 8));
}

/** Set up an AFG, one speaker pin and one stereo DAC. */
static void reset_model(void)
{
    k_memset(mmio, 0, sizeof(mmio));
    k_memset(params, 0, sizeof(params));
    k_memset(connections, 0, sizeof(connections));
    k_memset(selected, 0, sizeof(selected));
    k_memset(converter_format, 0, sizeof(converter_format));
    k_memset(configurations, 0, sizeof(configurations));
    k_memset(out_amp, 0, sizeof(out_amp));
    k_memset(sense, 0, sizeof(sense));
    k_memset(in_amp, 0, sizeof(in_amp));
    k_memset(pin_ctl, 0, sizeof(pin_ctl));
    k_memset(channel, 0, sizeof(channel));
    k_memset(eapd, 0, sizeof(eapd));
    k_memset(beep_ctl, 0, sizeof(beep_ctl));
    k_memset(power, 0, sizeof(power));
    beep_verbs = 0;
    conn_reads = verbs = bad_offsets = sense_triggers = 0;
    log_count = 0;
    run_at = ~0u;
    cap_stream = NULL;
    cap_running = false;
    hda_subsystem = 0;
    num_input_streams = 0;
    input_stream_index = 0;
    codec_silent = clock_stopped = dma_model = 0;
    fail_command = fail_mask = 0;
    playing = stream_started = false;
    user_callback = NULL;
    hda_base = 1;
    dma_posbuf_enabled = false;
    dma_checked = false;
    output_stream_index = 0;
    mmio[HDA_STATESTS] = 1;
    params[0][HDA_PARAM_VENDOR_ID] = 0x14F1506E;
    params[0][HDA_PARAM_NODE_COUNT] = 0x10001;
    params[1][HDA_PARAM_FUNC_GROUP] = 1;
    params[1][HDA_PARAM_NODE_COUNT] = 0x2007E;
    params[1][0x0A] = 0x30000 | 0xFFF;
    params[1][0x0B] = 1;
    params[2][HDA_PARAM_AUDIO_CAPS] = 1;
    params[10][HDA_PARAM_AUDIO_CAPS] = HDA_WIDGET_PIN << 20;
    params[10][HDA_PARAM_PIN_CAPS] = 0x10;
    params[10][HDA_PARAM_CONN_LIST] = 1;
    connections[10][0] = 2;
    configurations[10] = HDA_DEF_DEV_SPEAKER << 20;
}

/** Decode the controller word using its base, multiplier and divisor fields. */
static uint32_t decoded_rate(uint16_t word)
{
    return ((word & 0x4000) ? 44100u : 48000u) * (((word >> 11) & 7) + 1) / (((word >> 8) & 7) + 1);
}

/** Supply samples for exact-rate callback setup. */
static void callback(void *buffer, size_t size)
{
    k_memset(buffer, 0, size);
}

/** Every capability bit must produce that rate in both stream and converter. */
static void row_rates(void)
{
    static const uint32_t rates[] = {8000,  11025, 16000, 22050,  32000,  44100,
                                     48000, 88200, 96000, 176400, 192000, 384000};
    unsigned i, mode;

    row = "rates";
    for (i = 0; i < 12; i++) {
        for (mode = 0; mode < 2; mode++) {
            reset_model();
            params[1][0x0A] = 0x30000 | (1u << i);
            check(hda_find_output(), "enumeration succeeds");
            user_callback = mode ? callback : NULL;
            check(hda_start(rates[i], AUDIO_S16LE, 2) == 0, "supported start");
            check(decoded_rate((uint16_t)hda_test_read(0x92, 16)) == rates[i],
                  "stream rate equals request");
            check(decoded_rate(converter_format[2]) == rates[i], "converter rate equals request");
            check(current_hw_sample_rate == rates[i], "recorded rate equals programmed rate");
            check(hda_get_report()->programmed_rate == rates[i], "report rate");
            hda_stop();
            check(!hda_get_report()->playing && hda_get_report()->programmed_rate == rates[i],
                  "report survives stop");
            user_callback = NULL;
            check(hda_start(12345, AUDIO_U8, 1) == 0, "single advertised fallback rate");
            check(current_hw_sample_rate == rates[i], "fallback includes every rate bit");
        }
    }
    check(hda_rate_step(176400, 192000) == 60211, "high-rate ratio does not overflow");
}

/*
 * The write path's conversion as row_underrun expects it: a resampler of
 * the row's own over what was written, which puts the frames it owes while
 * any remain and plays silence and holds while they do not, each frame
 * rounded from 24 bits to 16, a half up.
 */
static struct {
    resampler_t rs;
    int16_t in[4096]; /* stereo frames written, one sample each side */
    unsigned put;     /* frames put */
    unsigned written; /* frames written */
} ref;

/** Render `frames` stereo frames of the model write path into `out`. */
static void ref_render(int16_t *out, unsigned frames)
{
    int32_t v[2];
    unsigned i;

    for (i = 0; i < frames; i++) {
        while (ref.rs.owed > 0 && ref.put < ref.written) {
            v[0] = v[1] = ref.in[ref.put++ % 4096] * 256;
            resample_put(&ref.rs, v);
        }
        if (ref.rs.owed > 0) {
            out[2 * i] = out[2 * i + 1] = 0;
            continue;
        }
        resample_get(&ref.rs, v);
        out[2 * i] = (int16_t)((v[0] + 8388608 + 128) / 256 - 32768);
        out[2 * i + 1] = (int16_t)((v[1] + 8388608 + 128) / 256 - 32768);
    }
}

/**
 * An empty period plays silence and holds the resampler, so the frames
 * written after it are each converted in turn: the driver's output and the
 * frames it leaves in the ring are the model's, through a period that runs
 * dry and a refill after it, twice. 44100 Hz input plays at 32000 Hz.
 */
static void row_underrun(void)
{
    static int16_t output[8192], expect[8192];
    uint8_t input[4096];
    unsigned i, pass;
    int same;

    row = "underrun";
    reset_model();
    ring_read_ptr = ring_write_ptr = 0;
    params[1][0x0A] = 0x20000 | (1u << 4) | (1u << 6);
    check(hda_find_output(), "enumeration succeeds");
    check(hda_start(44100, AUDIO_S16LE, 2) == 0 && current_hw_sample_rate == 32000,
          "44100 input selects 32000 output");
    resample_init(&ref.rs, 44100, 32000);
    ref.put = ref.written = 0;
    for (pass = 0; pass < 2; pass++) {
        ring_render_to_hw_buffer(output, 4096);
        ref_render(expect, 4096);
        check(ring_available() == 0, "period exhausts input");
        same = 1;
        for (i = 0; i < 8192; i++)
            same &= output[i] == expect[i];
        check(same, "the period is the model's");
        check(output[8190] == 0 && output[8191] == 0, "empty tail renders silence");
        for (i = 0; i < sizeof(input) / 4; i++) {
            unsigned sample = 1000 + 7 * (ref.written % 1024) + 16 * pass;

            ref.in[ref.written++ % 4096] = (int16_t)sample;
            input[4 * i] = input[4 * i + 2] = (uint8_t)sample;
            input[4 * i + 1] = input[4 * i + 3] = (uint8_t)(sample >> 8);
        }
        check(hda_write(input, sizeof(input)) == (int)sizeof(input), "refill accepted");
        ring_render_to_hw_buffer(output, 16);
        ref_render(expect, 16);
        same = 1;
        for (i = 0; i < 32; i++)
            same &= output[i] == expect[i];
        check(same, "the refill's frames are the model's");
        check(ring_available() == (ref.written - ref.put) * 4,
              "silence does not consume refill frames");
    }
    hda_stop();
}

/** Inherited, overridden, absent and disjoint capabilities govern setup. */
static void row_caps(void)
{
    unsigned before;

    row = "capabilities";
    reset_model();
    params[1][0x0A] = 0x30040;
    check(hda_find_output(), "inherit enumeration");
    check(hda_start(32000, AUDIO_S16LE, 2) == 0 && current_hw_sample_rate == 48000,
          "write resamples unsupported rate");
    hda_stop();
    user_callback = callback;
    before = verbs;
    check(hda_start(32000, AUDIO_S16LE, 2) == -EINVAL && !playing && verbs == before,
          "unsupported callback refused before programming");
    reset_model();
    params[2][HDA_PARAM_AUDIO_CAPS] |= 1u << 4;
    params[2][0x0A] = 0x20010;
    params[2][0x0B] = 1;
    check(hda_find_output(), "override enumeration");
    check(hda_start(32000, AUDIO_S16LE, 2) == 0 && current_hw_sample_rate == 32000,
          "widget overrides afg");
    hda_stop();
    user_callback = callback;
    check(hda_start(32000, AUDIO_U8, 1) == -EINVAL, "unsupported width refused");
    params[2][0x0A] = 0;
    hda_capture_report();
    user_callback = NULL;
    check(hda_start(48000, AUDIO_S16LE, 2) == -EINVAL, "empty mask refused");
    params[2][0x0A] = 0x30040;
    params[2][0x0B] = 0;
    hda_capture_report();
    check(hda_start(48000, AUDIO_S16LE, 2) == -EINVAL, "non-pcm refused");
    params[2][0x0B] = 1;
    hp_dac_nid = 3;
    params[3][HDA_PARAM_AUDIO_CAPS] = 0x11;
    params[3][0x0A] = 0x30100;
    params[3][0x0B] = 1;
    hda_capture_report();
    check(hda_start(48000, AUDIO_S16LE, 2) == -EINVAL, "disjoint dacs refused");
    params[3][0x0A] |= 0x40;
    hda_capture_report();
    check(hda_start(96000, AUDIO_S16LE, 2) == 0 && current_hw_sample_rate == 48000,
          "both dacs use intersection");
    check(converter_format[2] == converter_format[3], "both converters match");
}

/** Read second words, full long NIDs and ranges across response boundaries. */
static void row_connections(void)
{
    uint16_t out[HDA_MAX_CONNECTIONS];
    int i, n;

    row = "short-offset";
    reset_model();
    params[10][HDA_PARAM_CONN_LIST] = 9;
    for (i = 0; i < 9; i++)
        connections[10][i] = (uint16_t)(20 + i);
    n = hda_get_connections(10, out, HDA_MAX_CONNECTIONS);
    check(n == 9 && out[4] == 24 && out[8] == 28 && conn_reads == 3 && !bad_offsets,
          "entry offset advances by four");
    row = "long-offset";
    params[10][HDA_PARAM_CONN_LIST] = 0x85;
    conn_reads = bad_offsets = 0;
    n = hda_get_connections(10, out, HDA_MAX_CONNECTIONS);
    check(n == 5 && out[2] == 22 && out[4] == 24 && conn_reads == 3 && !bad_offsets,
          "entry offset advances by two");
    row = "long-width";
    params[10][HDA_PARAM_CONN_LIST] = 0x82;
    connections[10][0] = 0x102;
    connections[10][1] = 0x1234;
    n = hda_get_connections(10, out, HDA_MAX_CONNECTIONS);
    check(n == 2 && out[0] == 0x102 && out[1] == 0x1234, "fifteen-bit nids retained");
    check(!hda_find_output(), "indirect nid cannot alias the first dac");
    row = "short-range";
    params[10][HDA_PARAM_CONN_LIST] = 5;
    connections[10][0] = 2;
    connections[10][1] = 0x85;
    connections[10][2] = 8;
    connections[10][3] = 9;
    connections[10][4] = 0x8C;
    n = hda_get_connections(10, out, HDA_MAX_CONNECTIONS);
    check(n == 9 && out[3] == 5 && out[6] == 10 && out[8] == 12, "expanded selector order");
    row = "long-range";
    params[10][HDA_PARAM_CONN_LIST] = 0x83;
    connections[10][0] = 0x101;
    connections[10][1] = 0x8104;
    connections[10][2] = 0x1234;
    n = hda_get_connections(10, out, HDA_MAX_CONNECTIONS);
    check(n == 5 && out[3] == 0x104 && out[4] == 0x1234, "long range bit and endpoint");
    row = "invalid-list";
    check(hda_get_connections(10, out, 3) == -1, "capacity rejects a partial list");
    connections[10][0] = 0x8101;
    check(hda_get_connections(10, out, HDA_MAX_CONNECTIONS) == -1, "leading range refused");
    connections[10][0] = 0x105;
    check(hda_get_connections(10, out, HDA_MAX_CONNECTIONS) == -1, "descending range refused");
}

/** Discovery and setup use the expanded index, including on Conexant codecs. */
static void row_route(void)
{
    row = "route";
    reset_model();
    params[10][HDA_PARAM_CONN_LIST] = 0x83;
    connections[10][0] = 20;
    connections[10][1] = 0x8018;
    connections[10][2] = 25;
    params[24][HDA_PARAM_AUDIO_CAPS] = (HDA_WIDGET_AUDIO_SEL << 20);
    params[24][HDA_PARAM_CONN_LIST] = 2;
    connections[24][0] = 3;
    connections[24][1] = 0x85;
    params[3][HDA_PARAM_AUDIO_CAPS] = 0xF00000;
    params[4][HDA_PARAM_AUDIO_CAPS] = 1;
    params[5][HDA_PARAM_AUDIO_CAPS] = 0xF00000;
    params[20][HDA_PARAM_AUDIO_CAPS] = params[21][HDA_PARAM_AUDIO_CAPS] = 0xF00000;
    params[22][HDA_PARAM_AUDIO_CAPS] = params[23][HDA_PARAM_AUDIO_CAPS] = 0xF00000;
    selected[24] = 1;
    check(hda_find_output(), "path found through range");
    check(pin_conn_sel_idx == 4 && mixer_nid == 24 && dac_nid == 4, "expanded route selected");
    check(hda_start(96000, AUDIO_S16LE, 2) == 0, "routed start");
    check(selected[10] == 4 && selected[24] == 1, "expanded indices programmed");
    check(hda_report.connections[0].raw[1] == 0x8018 && hda_report.connections[0].nodes[4] == 24,
          "raw and expanded path retained");
    reset_model();
    params[10][HDA_PARAM_CONN_LIST] = 1;
    connections[10][0] = 0x82;
    check(!hda_find_output(), "invalid list cannot fall back to an unrelated dac");
}

/**
 * A start sets the DAC's and the pin's output amplifiers at full gain and
 * then applies the level set before it again: the gain scaled to each
 * node's steps, and the mute bit at 0. The DAC has 87 steps and the pin 31.
 */
static void row_volume(void)
{
    row = "volume";
    reset_model();
    params[2][HDA_PARAM_AUDIO_CAPS] = 1 | 0x04;
    params[2][HDA_PARAM_OUTPUT_AMP] = 87 << 8;
    params[10][HDA_PARAM_AUDIO_CAPS] |= 0x04;
    params[10][HDA_PARAM_OUTPUT_AMP] = 31 << 8;
    check(hda_find_output(), "enumeration succeeds");
    check(dac_nid == 2 && pin_nid == 10, "the amplified DAC and pin are the path");
    check(hda_start(48000, AUDIO_S16LE, 2) == 0, "start at the full level");
    check((out_amp[2] & 0xF0FF) == (0xB000 | 87), "the DAC starts at full gain");
    check((out_amp[10] & 0xF0FF) == (0xB000 | 31), "the pin starts at full gain");
    hda_stop();
    hda_set_volume(40);
    out_amp[2] = out_amp[10] = 0;
    check(hda_start(48000, AUDIO_S16LE, 2) == 0, "start at a level of 40");
    check((out_amp[2] & 0xF0FF) == (0xB000 | 34), "the DAC's gain is 40% of 87 after a start");
    check((out_amp[10] & 0xF0FF) == (0xB000 | 12), "the pin's gain is 40% of 31 after a start");
    check(hda_get_volume() == 40, "the level is kept across the start");
    hda_stop();
    hda_set_volume(0);
    out_amp[2] = out_amp[10] = 0;
    check(hda_start(48000, AUDIO_S16LE, 2) == 0, "start at a level of 0");
    check((out_amp[2] & 0xF0FF) == (0xB000 | 0x80), "the DAC is muted after a start");
    check((out_amp[10] & 0xF0FF) == (0xB000 | 0x80), "the pin is muted after a start");
    hda_stop();
    hda_set_volume(100);
}

/** Tell display-only controllers and codecs from those that drive speakers. */
static void row_hdmi(void)
{
    row = "hdmi";
    check(is_hdmi_controller(0x8086, 0x0a0c) && is_hdmi_controller(0x8086, 0x160c),
          "haswell and broadwell display controllers are hdmi");
    check(is_hdmi_controller(0x10de, 0x0e0f) && is_hdmi_controller(0x1002, 0xaa38),
          "nvidia and amd functions are hdmi");
    check(!is_hdmi_controller(0x8086, 0x284b), "ich8 controller is analog");
    check(!is_hdmi_controller(0x8086, 0x293e), "ich9 controller is analog");
    check(!is_hdmi_controller(0x8086, 0x3198), "gemini lake controller is analog");
    check(!is_hdmi_controller(0x8086, 0x2668), "ich6 controller is analog");
    check(is_hdmi_codec(0x80862807) && is_hdmi_codec(0x808629fb), "intel hdmi codecs");
    check(is_hdmi_codec(0x10de0040) && is_hdmi_codec(0x1002aa01), "nvidia and amd codecs");
    check(!is_hdmi_codec(0x80862a00) && !is_hdmi_codec(0x80862900), "other intel ids are analog");
    check(!is_hdmi_codec(0x10ec0269) && !is_hdmi_codec(0x1af40022), "realtek and qemu codecs");
}

/**
 * Give the model codec an ADC and a microphone jack, as /proc/audio's
 * widget rows show them: nodes 2 to 10 in the function group, the DAC 2,
 * the ADC 3 reading the microphone pin 4, vendor widgets 5 to 9 and the
 * speaker pin 10. The ADC's input amplifier is the function group's, and
 * the pin's its own; the pin has presence detect that needs a trigger, and
 * senses a plug.
 */
static void model_adc(void)
{
    int nid;

    params[1][HDA_PARAM_NODE_COUNT] = 0x20009;
    params[1][HDA_PARAM_INPUT_AMP] = 0x00270300;
    params[1][HDA_PARAM_GPIO_COUNT] = 0x40000002;
    params[3][HDA_PARAM_AUDIO_CAPS] = (HDA_WIDGET_AUDIO_IN << 20) | (1u << 8) | (1u << 1) | 1;
    params[3][HDA_PARAM_CONN_LIST] = 1;
    connections[3][0] = 4;
    params[4][HDA_PARAM_AUDIO_CAPS] = (HDA_WIDGET_PIN << 20) | (1u << 3) | (1u << 1);
    params[4][HDA_PARAM_INPUT_AMP] = 0x00030003;
    params[4][HDA_PARAM_PIN_CAPS] = 0x00001726;
    configurations[4] = 0x02A11030;
    sense[4] = 0x80000000;
    for (nid = 5; nid < 10; nid++)
        params[nid][HDA_PARAM_AUDIO_CAPS] = 0xF00000;
}

/** Snapshot the real driver report through /proc/audio's text. */
static void row_report(void)
{
    static const uint8_t ring_bytes[3200];
    char text[4096], small[8];
    unsigned before;
    int len;

    row = "proc-audio";
    reset_model();
    model_adc();
    check(hda_find_output(), "report enumeration");
    check(hda_start(32000, AUDIO_S16LE, 2) == 0, "report stream start");
    check(sense_triggers == 1, "the start triggers the sense of the pin that needs it");
    check(hda_write(ring_bytes, sizeof(ring_bytes)) == (int)sizeof(ring_bytes), "ring write");
    hda_test_write(0x80 + HDA_SD_LPIB, 4096, 32);
    before = verbs;
    len = audio_proc_text(NULL, 0);
    check(audio_proc_text(text, (int)sizeof(text) - 1) == len, "a length and its text agree");
    check(len > 0 && len < (int)sizeof(text) && verbs == before, "report sends no verbs");
    text[len < (int)sizeof(text) ? len : (int)sizeof(text) - 1] = 0;
    printf("hda-test: proc-audio:\n%s", text);
    check(strstr(text, "stream requested 32000 programmed 32000 format 0x0A11") != NULL,
          "report exposes programmed rate and register");
    check(strstr(text, "caps dac 2 source 1 pcm 0x00030FFF formats 0x00000001") != NULL,
          "report exposes inherited capabilities");
    check(strstr(text, "connections nid 10 long 0 raw 0x02\nexpanded 2\n") != NULL,
          "report exposes codec list");
    /* 800 frames in the ring at 32 kHz are 25 ms; the 4736 bytes of the
     * three 2944-byte periods ahead of position 4096, 1184 frames, are 37 ms;
     * together 1984 frames, 62 ms. */
    check(strstr(text, "queue fill 3200 high 3200 delay-ms 62 written 3200 dropped 0 periods 0 "
                       "underruns 0 missed 0 lpib 4096\n") != NULL,
          "report exposes the queue");
    check(strstr(text, "gpio count 2 caps 0x40000002\n") != NULL, "report exposes the gpios");
    check(strstr(text, "widget 3 type in caps 0x00100103 amp-in 0x00270300 amp-out 0x00000000 "
                       "conn 4\n") != NULL,
          "the adc's amplifier is the function group's");
    check(strstr(text, "widget 4 type pin caps 0x0040000A amp-in 0x00030003 amp-out 0x00000000 "
                       "pin-caps 0x00001726 config 0x02A11030 sense 0x80000000 conn\n") != NULL,
          "the microphone pin has its own amplifier and its sense");
    check(strstr(text, "widget 5 type vendor ") != NULL, "a vendor widget is named");
    check(strstr(text, "widget 10 type pin caps 0x00400000 amp-in 0x00000000 amp-out 0x00000000 "
                       "pin-caps 0x00000010 config 0x00100000 conn 2\n") != NULL,
          "a pin without presence detect has no sense");
    check(strstr(text, "\nadcs 3\n") != NULL, "report lists the adcs");
    check(strstr(text, "enabled 0 mode auto\njack headphone pin 0 state unknown polls 0 failed 0 "
                       "changes 0\njack mic pin 4 state in polls 1 failed 0 changes 1\n") != NULL,
          "report exposes the mode and the jacks, the mic as the start read it");
    check(strstr(text, "input adc 3 index 0 mid 0 index 0 pin 4 int-mic 0 jack-mic 4 inv-dmic 0 "
                       "level 0 pin-ctl 0x00\n") != NULL,
          "report exposes the input path");
    check(strstr(text, "caps adc 3 source 1 pcm 0x00030FFF formats 0x00000001\n") != NULL,
          "report exposes the adc's capabilities");
    check(strstr(text, "capture running 0 requested 0 rate 0 format 0x0000 period 0 periods 0 "
                       "frames 0 overruns 0 lpib 0\n") != NULL,
          "report exposes the idle input stream");
    hda_stop();
    check(strstr(text, "playing 1") != NULL, "open snapshot survives stop");
    check(audio_proc_text(small, (int)sizeof(small)) > (int)sizeof(small) &&
              !memcmp(small, "driver ", 7),
          "a short buffer keeps the text's start and learns its length");
    hda_read_queue();
    check(hda_get_report()->queue.fill == 0 && hda_get_report()->queue.delay_ms == 0,
          "a stopped stream queues nothing");
}

/**
 * A /proc/audio read whose capture counter wraps between its two passes:
 * the second renders the first's snapshot, so the length it measured is
 * the length it writes, and the text has the counter as the first pass read
 * it.
 */
static void row_report_wrap(void)
{
    char text[4096];
    int len;

    row = "proc-audio-wrap";
    reset_model();
    model_adc();
    check(hda_find_output(), "wrap enumeration");
    hda_report.input.frames = 0xFFFFFFFFu;
    len = audio_proc_text(NULL, 0);
    hda_report.input.frames = 3;
    check(len < (int)sizeof(text) && audio_proc_text(text, len) == len,
          "a pass after the counter wrapped writes the length measured before");
    text[len] = 0;
    check(strstr(text, " frames 4294967295 overruns ") != NULL,
          "the text has the counter the snapshot took");
    len = audio_proc_text(NULL, 0);
    check(audio_proc_text(text, len) == len, "a new snapshot measures its own length");
    text[len] = 0;
    check(strstr(text, " frames 3 overruns ") != NULL, "and has the counter as it is now");
    hda_report.input.frames = 0;
}

/**
 * A verb from the tick gives up at once when another holds the interface,
 * and one that the codec never answers gives up after 1 ms, polling with the
 * caller's interrupts between reads; the lock is free after either.
 */
static void row_verbs(void)
{
    uint32_t response = 1;
    unsigned reads, writes, n, clocks;

    row = "verb-busy";
    reset_model();
    params[0][HDA_PARAM_VENDOR_ID] = 0x10EC0269;
    check(hda_verb(0, 0, HDA_VERB_GET_PARAM | HDA_PARAM_VENDOR_ID, HDA_VERB_TRY, &response) == 0 &&
              response == 0x10EC0269,
          "a free interface answers a try");
    verb_lock.locked = 1;
    reads = mmio_reads;
    writes = mmio_writes;
    n = verbs;
    check(hda_verb(0, 0, HDA_VERB_GET_PARAM, HDA_VERB_TRY, &response) == -EBUSY && response == 0,
          "a held lock makes a try busy");
    check(mmio_reads == reads && mmio_writes == writes && verbs == n,
          "a busy try touches no register");
    check(irqs_on, "a busy try puts interrupts back");
    verb_lock.locked = 0;
    mmio[HDA_ICS] = HDA_ICS_ICB;
    writes = mmio_writes;
    check(hda_verb(0, 0, HDA_VERB_GET_PARAM, HDA_VERB_TRY, &response) == -EBUSY,
          "a command under way makes a try busy");
    check(mmio_writes == writes && verbs == n && !verb_lock.locked && irqs_on,
          "a try sends nothing behind a command under way");
    mmio[HDA_ICS] = 0;

    row = "verb-timeout";
    codec_silent = 1;
    clocks = clock_reads;
    delays = delays_irqs_off = ics_reads_irqs_on = 0;
    check(hda_verb(0, 0, HDA_VERB_GET_PARAM, HDA_VERB_WAIT, &response) == -ETIMEDOUT &&
              response == 0,
          "an unanswered verb times out");
    check(verbs == n + 1, "the verb was sent once");
    check(clock_reads - clocks <= 3 && delays >= 1, "the wall clock ends the wait at 1 ms");
    check(delays_irqs_off == 0 && ics_reads_irqs_on == 0,
          "the waits run with interrupts on, the reads with them off");
    check(!verb_lock.locked && irqs_on, "a timed-out verb frees the lock");
    check(hda_send_verb(0, 0, HDA_VERB_GET_PARAM) == 0, "the command still under way times out");
    clock_stopped = 1;
    delays = 0;
    check(hda_verb(0, 0, HDA_VERB_GET_PARAM, HDA_VERB_WAIT, &response) == -ETIMEDOUT,
          "a verb times out when the clock stops");
    check(delays == HDA_VERB_POLLS, "a stopped clock ends the wait after its polls");
    mmio[HDA_ICS] = 0;
    codec_silent = clock_stopped = 0;
    check(hda_verb(0, 0, HDA_VERB_GET_PARAM | HDA_PARAM_VENDOR_ID, HDA_VERB_WAIT, &response) == 0 &&
              response == 0x10EC0269,
          "the interface answers again once the codec does");
    irqs_on = 0;
    delays = delays_irqs_off = 0;
    codec_silent = 1;
    check(hda_verb(0, 0, HDA_VERB_GET_PARAM, HDA_VERB_TRY, &response) == -ETIMEDOUT,
          "a try from the tick times out too");
    check(delays_irqs_off == delays && !irqs_on, "a tick's waits keep its interrupts off");
    irqs_on = 1;
    mmio[HDA_ICS] = 0;
    codec_silent = 0;
}

/**
 * Advance the model's DMA by `us` microseconds at the programmed rate, 16-bit
 * stereo: each frame it passes is read from the driver's buffer into
 * `played`, the position wraps at the buffer's end, and LPIB and the wall
 * clock, 24 ticks to a microsecond, follow.
 */
static void model_play(uint32_t us)
{
    unsigned long long target;
    uint32_t cbl = period_bytes * HDA_PERIODS;

    model_us += us;
    target = model_us * current_hw_sample_rate / 1000000u;
    while (model_frames < target) {
        if (played_count < sizeof(played) / sizeof(played[0]))
            played[played_count++] = audio_buffer[model_pos / 2];
        model_pos = (model_pos + 4) % cbl;
        model_frames++;
    }
    clock_value = (uint32_t)(model_us * 24u);
    hda_test_write(hda_sd_offset(output_stream_index) + HDA_SD_LPIB, model_pos, 32);
}

/**
 * Start the model's DMA on a codec of the rates in `rates`, a mask of the
 * rate bits, at `rate`, in `format` and `channels`, through the write path;
 * the position check is taken as made, since the model's clock moves only
 * when a row moves it.
 */
static void model_start(uint32_t rates, uint32_t rate, audio_format_t format, int channels)
{
    reset_model();
    params[1][0x0A] = 0x20000 | rates;
    check(hda_find_output(), "enumeration succeeds");
    dma_checked = true;
    dma_model = 1;
    model_us = model_frames = 0;
    model_pos = 0;
    played_count = 0;
    clock_value = 0;
    check(hda_start(rate, format, channels) == 0, "the model's stream starts");
}

/* A numbered block: 480 frames, 10 ms at 48 kHz, of 16-bit stereo, each
 * sample the block's number. */
#define BLOCK_FRAMES 480
static int16_t block[BLOCK_FRAMES * 2];

/** Write whole blocks, numbered from `*next` on, while the ring has room for
 * one, and leave `*next` the number of the next. */
static void write_blocks(unsigned *next)
{
    unsigned i;

    while (ring_free() >= sizeof(block)) {
        for (i = 0; i < BLOCK_FRAMES * 2; i++)
            block[i] = (int16_t)*next;
        check(hda_write((const uint8_t *)block, sizeof(block)) == (int)sizeof(block),
              "a block fits in the ring's room");
        ++*next;
    }
}

/** Count one tick and run the driver's tick, as the boot processor's tick
 *  does. */
static void tick(void)
{
    kernel_ticks++;
    hda_tick();
}

/**
 * Run `ticks` timer ticks 10 ms apart on the model, writing blocks after each
 * when `next` is not NULL, and check after each that the delay stays within
 * six periods. Gives the largest delay seen, in frames.
 */
static uint32_t run_ticks(unsigned ticks, unsigned *next)
{
    uint32_t most = 0, delay;

    while (ticks--) {
        model_play(10000);
        tick();
        if (next)
            write_blocks(next);
        delay = hda_delay_frames();
        if (delay > most)
            most = delay;
    }
    check(most <= 6 * period_bytes / 4, "the delay stays within six periods");
    return most;
}

/**
 * Walk `played` from frame `from`: every frame is silence or a block, the
 * blocks come in order from `first`, each whole, 480 frames in one run, but
 * the last, which the log may cut. Gives the last block played, or
 * `first` - 1 when none was.
 */
static unsigned check_blocks(unsigned from, unsigned first)
{
    unsigned i, expect = first, run = 0;
    int ok = 1;

    for (i = from; i < played_count; i++) {
        if (played[i] == 0) {
            if (run != 0 && run != BLOCK_FRAMES)
                ok = 0;
            if (run != 0)
                expect++;
            run = 0;
            continue;
        }
        if (run == BLOCK_FRAMES) {
            expect++;
            run = 0;
        }
        if ((unsigned)played[i] != expect)
            ok = 0;
        run++;
    }
    check(ok, "the blocks play in order, each once and whole");
    return run ? expect : expect - 1;
}

/**
 * A period is the frame count nearest 23 ms at the programmed rate, in
 * bytes of 16-bit stereo rounded to the nearest 128 and at most 16384, in
 * three descriptors; the ring holds three periods of the caller's frames.
 */
static void row_periods(void)
{
    static const uint32_t rates[] = {8000,  11025, 16000, 22050,  32000, 44100,
                                     48000, 88200, 96000, 176400, 192000};
    static const uint32_t bytes[] = {768,  1024, 1536, 2048,  2944, 4096,
                                     4480, 8064, 8832, 16256, 16384};
    unsigned i, sd = hda_sd_offset(0);

    row = "period-by-rate";
    for (i = 0; i < 11; i++) {
        reset_model();
        check(hda_find_output(), "enumeration succeeds");
        check(hda_start(rates[i], AUDIO_S16LE, 2) == 0 && current_hw_sample_rate == rates[i],
              "the rate is programmed as asked");
        check(period_bytes == bytes[i], "the period is the frames nearest 23 ms");
        check(hda_test_read(sd + HDA_SD_CBL, 32) == 3 * bytes[i], "the buffer is three periods");
        check(hda_test_read(sd + HDA_SD_LVI, 16) == 2, "the last descriptor is the third");
        check(bdl[0].length == bytes[i] && bdl[2].length == bytes[i] &&
                  bdl[2].addr_low - bdl[0].addr_low == 2 * bytes[i],
              "each descriptor is one period, one after another");
        if (rates[i] <= 96000)
            check(ring_size == 3 * bytes[i] + 1, "the ring holds three periods");
        else
            check(ring_size == HDA_RING_MAX, "the ring stops at its array");
        hda_stop();
    }
    row = "ring-in-time";
    reset_model();
    params[1][0x0A] = 0x20000 | (1u << 5);
    check(hda_find_output(), "enumeration succeeds");
    check(hda_start(11025, AUDIO_U8, 1) == 0 && current_hw_sample_rate == 44100,
          "Doom's 11025 Hz plays at 44100 Hz");
    check(period_bytes == 4096, "the period is 1024 frames, 23.2 ms");
    check(ring_size == 3 * 256 + 1, "the ring holds three periods of 256 Doom frames");
    hda_stop();
}

/**
 * The delay is the ring's frames, the 58 the resampler holds past the time
 * of its next frame, and the DMA's ahead of the position, at the caller's
 * rate. Doom's 11025 Hz U8 mono plays at 44100 Hz, so a frame of the DMA is
 * a quarter of one of the caller's, rounded down, and the filter reaches 59
 * of the caller's frames each side.
 */
static void row_delay(void)
{
    static const uint8_t doom[300];

    row = "delay";
    model_start(1u << 5, 11025, AUDIO_U8, 1);
    check(hda_delay_frames() == 768 + 58,
          "a start queues three periods of silence, and 58 frames of the filter's");
    check(hda_write(doom, sizeof(doom)) == (int)sizeof(doom), "300 frames written");
    check(hda_delay_frames() == 768 + 58 + 300, "the ring's frames add to the delay");
    model_play(10000);
    check(model_pos == 1764, "10 ms are 441 frames");
    check(hda_delay_frames() == 300 + 58 + (12288 - 1764) / 4 / 4, "the DMA's frames ahead count");
    tick();
    check(hw_fill_count == 0, "no period has ended");
    model_play(15000);
    check(model_pos == 4408, "25 ms are 1102 frames, into the second period");
    check(hda_delay_frames() == 300 + 58 + (12288 - 4408) / 4 / 4, "before the refill");
    tick();
    check(hw_fill_count == 1 && ring_available() == 44, "the first period takes 256 frames");
    check(resample_held(&hda_rs) == 58, "the filter holds 58 past the next frame's time");
    check(hda_delay_frames() == 44 + 58 + (4096 + 12288 - 4408) / 4 / 4,
          "after it, the DMA's samples end a revolution after the second period's start");
    hda_read_queue();
    check(hda_get_report()->queue.delay_ms == (44 + 58 + 2994 / 4) * 1000 / 11025,
          "/proc/audio gives the delay in milliseconds");
    hda_stop();
}

/**
 * A service that comes late fills every period played since the last, in
 * order, and a writer that keeps the ring full is heard without a gap: the
 * start's three periods of silence, then each block once, in order.
 */
static void row_fill(void)
{
    unsigned next = 1, i;
    int ok = 1;

    row = "period-fill";
    model_start(1u << 6, 48000, AUDIO_S16LE, 2);
    check(period_bytes == 4480 && ring_size == 13441, "48 kHz periods are 1120 frames");
    write_blocks(&next);
    check(next == 8, "the ring takes three periods, seven blocks");
    model_play(50000);
    tick();
    check(hw_fill_count == 2 && hw_period_last == 2, "a late service fills both periods played");
    check(ring_available() == 1120 * 4, "the two periods take 2240 frames");
    run_ticks(30, &next);
    for (i = 0; i < played_count; i++)
        if (played[i] != (i < 3360 ? 0 : (int16_t)(1 + (i - 3360) / BLOCK_FRAMES)))
            ok = 0;
    check(ok, "silence for three periods, then every block in order with no gap");
    check(played_count == 16800 && underrun_count == 0 && missed_revolutions == 0,
          "350 ms played without an underrun or a missed revolution");
    hda_stop();
}

/**
 * A writer that stops for a second and goes on: the ring empties and the
 * stream plays silence, the delay stays within six periods throughout, and
 * the blocks after the stall follow the last one played before it, none
 * played twice.
 */
static void row_writer_stall(void)
{
    unsigned next = 1, before, last;

    row = "writer-stall";
    model_start(1u << 6, 48000, AUDIO_S16LE, 2);
    write_blocks(&next);
    run_ticks(20, &next);
    before = next;
    run_ticks(100, NULL);
    check(ring_available() == 0 && underrun_count > 0, "the stall empties the ring");
    run_ticks(20, &next);
    last = check_blocks(3360, 1);
    check(last >= before && last < next, "the blocks after the stall are played");
    check(missed_revolutions == 0, "the tick went on, so no revolution was missed");
    hda_stop();
}

/**
 * The machine stops for a second with the ring and the DMA full: no service
 * runs while the DMA goes round 14 times and 960 frames. The next service
 * counts the 14 revolutions by the wall clock, silences the rest of the
 * period playing and empties the ring, so that none of the blocks queued at
 * the stall is played after it, and the blocks written after it follow.
 */
static void row_missed(void)
{
    unsigned next = 1, queued, from;

    row = "missed-revolution";
    model_start(1u << 6, 48000, AUDIO_S16LE, 2);
    write_blocks(&next);
    run_ticks(20, &next);
    queued = next - 1;
    model_play(1000000);
    check(model_pos == 480 * 4, "the DMA stopped 480 frames into the first period");
    from = played_count;
    /* Only the kernel build records a request; the miss path must clear it. */
    hda_report.service.asked_us = 12345;
    hda_report.service.asked_valid = 1;
    tick();
    check(missed_revolutions == 14 && lpib_wraps >= 14, "14 revolutions missed");
    check(hda_report.service.missed == 14 && hda_report.service.revolution_us != 0 &&
              hda_report.service.asked_valid == 0 && hda_report.service.asked_us == 0,
          "the report counts the 14 misses and the buffer's duration, no request");
    check(ring_available() == 0 && session_bytes_dropped > 0, "the ring's stale blocks dropped");
    check(hw_period_last == 0, "the service is in step with the period playing");
    run_ticks(30, &next);
    check(played[from] == 0, "the rest of the period playing is silent");
    check(check_blocks(from, queued + 1) > queued, "blocks after the stall follow, none before");
    hda_stop();
}

/**
 * Once the output stream's interrupt has come, the tick leaves its periods
 * to the interrupt and fills nothing, until more than a period and a half
 * has passed since that interrupt (`hda_irq_late`); then it fills the
 * period played itself.
 */
static void row_play_gate(void)
{
    unsigned next = 1;

    row = "play-irq-gate";
    model_start(1u << 6, 48000, AUDIO_S16LE, 2);
    write_blocks(&next);
    model_play(25000);
    hda_test_write(HDA_INTSTS, 1u << output_stream_index, 32);
    hda_irq_handler(NULL);
    check(play_irq_seen && hw_fill_count == 1,
          "the output stream's interrupt fills the period played");
    model_play(24000);
    tick();
    check(hw_fill_count == 1, "the tick leaves a period played to the interrupt that is due");
    kernel_ticks += 36; /* a period and a half of 23.3 ms, and a tick more */
    tick();
    check(hw_fill_count == 2, "the tick fills the period itself once the interrupt is late");
    hda_stop();
}

/**
 * An output interrupt that comes while the position still reads the period
 * before fills nothing and leaves the stream pending (`play_irq_pending`),
 * as Linux's azx_position_check marks one; the tick then asks for the
 * service at each call, and the first after the position has passed into
 * the next period fills the period played.
 */
static void row_play_early(void)
{
    unsigned next = 1;

    row = "play-irq-early";
    model_start(1u << 6, 48000, AUDIO_S16LE, 2);
    write_blocks(&next);
    model_play(20000);
    hda_test_write(HDA_INTSTS, 1u << output_stream_index, 32);
    hda_irq_handler(NULL);
    check(play_irq_pending && hw_fill_count == 0,
          "an interrupt before the period's end fills nothing and stays pending");
    tick();
    check(play_irq_pending && hw_fill_count == 0,
          "the tick fills nothing while the position reads the first period");
    model_play(5000);
    tick();
    check(!play_irq_pending && hw_fill_count == 1,
          "the next tick fills the period once the position has passed it");
    hda_stop();
}

/**
 * An output interrupt for a period the tick already refilled, before the
 * stream's first interrupt, finds the position in the first half of the
 * next period: its service fills nothing and clears the pending flag, as
 * Linux's azx_position_ok passes such a position, so the tick does not ask
 * for the stream again until the next interrupt is late.
 */
static void row_play_refilled(void)
{
    unsigned next = 1;

    row = "play-irq-refilled";
    model_start(1u << 6, 48000, AUDIO_S16LE, 2);
    write_blocks(&next);
    model_play(25000);
    tick();
    check(hw_fill_count == 1, "the tick fills the period played before the first interrupt");
    hda_test_write(HDA_INTSTS, 1u << output_stream_index, 32);
    hda_irq_handler(NULL);
    hda_test_write(HDA_INTSTS, 0, 32);
    check(play_irq_seen && !play_irq_pending && hw_fill_count == 1,
          "its interrupt fills nothing and leaves the stream not pending");
    model_play(1000);
    tick();
    check(hw_fill_count == 1 && !play_irq_pending, "the tick then leaves the stream alone");
    hda_stop();
}

/** Allocate a procfs snapshot, or inject allocation failure. */
void *kmalloc(size_t size)
{
    return refuse_alloc ? NULL : malloc(size);
}
/** Release a procfs snapshot. */
void kfree(void *ptr)
{
    free(ptr);
}
/** Name the model's selected driver. */
const char *audio_get_driver_name(void)
{
    return "Intel HD Audio";
}

/** Allocate a channel of the mix, zeroed. */
void *kzalloc(size_t size)
{
    void *p = kmalloc(size);

    if (p)
        k_memset(p, 0, size);
    return p;
}

/* The driver the mix plays through: HDA's, as init_audio chooses it. */
audio_driver_t *audio_hw = &hda_driver;
audio_driver_t *audio;

/** The process calling the audio interface: one, for the mix's rows. */
uint32_t audio_process_id(void)
{
    return 1;
}

/** Install the dispatch driver as at boot. */
void audio_set_dispatch(audio_driver_t *d)
{
    audio = d ? d : audio_hw;
}

/* The player the mix rows open on the mix: one stereo frame of S24_LE,
 * repeated, whose top byte is no sign, which the mix does not read. */
static const uint8_t mix_frame[8] = {0x56, 0x34, 0x12, 0xA5, 0xDF, 0xBC, 0x9A, 0x5A};

/** The player's `transfer`: fill `size` bytes with its frame. */
static void mix_transfer(audio_stream_t *s, uint8_t *buf, size_t size)
{
    size_t i;

    (void)s;
    for (i = 0; i < size; i++)
        buf[i] = mix_frame[i % 8];
}

/** The player's `period`: nothing to wake. */
static void mix_period(audio_stream_t *s)
{
    (void)s;
}

/** Little-endian `n`-byte word at byte `at` of the DMA buffer. */
static uint32_t dma_word(uint32_t at, int n)
{
    const uint8_t *p = (const uint8_t *)audio_buffer + at;
    uint32_t v = 0;
    int i;

    for (i = n - 1; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

/**
 * One player of S24_LE stereo at 48000 Hz on the mix over the driver, on a
 * codec whose DACs have the sizes in `pcm_sizes`: with 24 bits the mix's
 * stream is granted 32 bits, the stream format is 24 bits in 32 (0x0031),
 * and each sample of the DMA buffer is the player's on top of a zero byte,
 * exactly; with 16 alone it is granted 16 bits (0x0011), and each sample is
 * the player's rounded to 16 bits. /proc/audio's stream line gives the bits.
 */
static void mix_through(uint32_t pcm_sizes)
{
    int wide = (pcm_sizes & (1u << 19)) != 0;
    static char text[16384];
    audio_stream_t s;
    uint32_t i, frames;
    int len, bad = 0;

    reset_model();
    params[1][0x0A] = pcm_sizes | 0x560; /* 44.1, 48, 96 and 192 kHz */
    check(hda_find_output(), "enumeration succeeds");
    dma_checked = true;
    vchan_dispatch();
    check(audio == &vchan_driver, "the mix is in front of the driver");
    k_memset(&s, 0, sizeof(s));
    s.dir = AUDIO_PLAYBACK;
    s.rate = 48000;
    s.format = AUDIO_S24LE;
    s.channels = 2;
    s.period_bytes = 4096;
    s.periods = 4;
    s.transfer = mix_transfer;
    s.period = mix_period;
    check(audio->stream_open(&s) == 0 && audio->stream_setup(&s) == 0, "the player is set up");
    check(s.format == AUDIO_S24LE && s.rate == 48000, "the player keeps its format and rate");
    check(play_stream == &mix.stream && mix.stream.format == (wide ? AUDIO_S32LE : AUDIO_S16LE),
          wide ? "the mix's stream is granted 32 bits" : "the mix's stream is granted 16 bits");
    check(audio->stream_start(&s) == 0 && playing, "the player starts the device");
    check(hda_get_report()->stream_format == (wide ? 0x0031 : 0x0011) &&
              converter_format[2] == (wide ? 0x0031 : 0x0011),
          wide ? "the stream and the DAC play 24 bits in 32"
               : "the stream and the DAC play 16 bits");
    frames = period_bytes * HDA_PERIODS / (wide ? 8u : 4u);
    check(frames > 1000, "the DMA buffer holds the periods");
    for (i = 0; i < frames && !bad; i++) {
        if (wide)
            bad = dma_word(8 * i, 4) != 0x12345600u || dma_word(8 * i + 4, 4) != 0x9ABCDF00u;
        else
            bad = dma_word(4 * i, 2) != 0x1234u || dma_word(4 * i + 2, 2) != 0x9ABDu;
    }
    check(!bad, wide ? "each sample the player's 24 bits on a zero byte"
                     : "each sample the player's rounded to 16 bits");
    len = audio_proc_text(NULL, 0);
    check(len > 0 && len < (int)sizeof(text) && audio_proc_text(text, len) == len, "report text");
    text[len > 0 && len < (int)sizeof(text) ? len : 0] = 0;
    check(strstr(text, wide ? " bits 24\n" : " bits 16\n") != NULL,
          "the stream line gives the bits played");
    audio->stream_close(&s);
    check(!playing && play_stream == NULL && mix.list == NULL, "the close gives the device back");
    audio_set_dispatch(NULL);
}

/** The mix on a codec of 16 and 24 bits, the Lenovo's word, and on one of
 * 16 bits alone. */
static void row_mix_width(void)
{
    row = "mix at 24 bits";
    mix_through(0xA0000);
    row = "mix at 16 bits";
    mix_through(0x20000);
}

/** Only the first start after setup waits for the DMA check, which reads the
 * wall clock at each poll; a later start reads it twice, for the session's
 * start and the service's last reading. No start prints. */
static void row_dma_check(void)
{
    unsigned reads;

    row = "dma-check";
    reset_model();
    check(hda_find_output(), "enumeration succeeds");
    console_writes = 0;
    reads = clock_reads;
    check(hda_start(48000, AUDIO_S16LE, 2) == 0, "first start succeeds");
    check(dma_checked && clock_reads - reads > 2, "first start waits on the wall clock");
    hda_stop();
    reads = clock_reads;
    check(hda_start(48000, AUDIO_S16LE, 2) == 0, "second start succeeds");
    check(clock_reads - reads <= 2, "second start does not wait");
    check(console_writes == 0, "starts print nothing on the console");
    hda_stop();
}

/** Shutdown stops DMA; later interrupts and repeated shutdown do no I/O. */
static void row_shutdown(void)
{
    unsigned reads, writes;

    row = "shutdown";
    reset_model();
    check(hda_find_output(), "enumeration succeeds");
    check(hda_start(48000, AUDIO_S16LE, 2) == 0, "start succeeds");
    hda_test_write(HDA_GCTL, 1, 32);
    hda_test_write(HDA_INTCTL, 0x80000001, 32);
    hda_shutdown();
    check(!playing && !stream_started && !hda_base, "shutdown clears playback and mapping");
    check(hda_test_read(HDA_INTCTL, 32) == 0, "shutdown disables interrupts");
    check(hda_test_read(HDA_GCTL, 32) == 0, "shutdown asserts reset");
    check(!(hda_test_read(hda_sd_offset(output_stream_index) + HDA_SD_CTL, 8) & HDA_SD_CTL_RUN),
          "shutdown stops stream DMA");
    hda_test_write(HDA_INTSTS, 1, 32);
    reads = mmio_reads;
    writes = mmio_writes;
    hda_irq_handler(NULL);
    check(mmio_reads == reads && mmio_writes == writes, "late interrupt does no MMIO");
    tick();
    check(mmio_reads == reads && mmio_writes == writes, "tick after shutdown does no MMIO");
    hda_shutdown();
    check(mmio_reads == reads && mmio_writes == writes, "repeated shutdown does no MMIO");
}

/**
 * Give the model the Lenovo G50-70's codec, as dev-log/68-sound/lenovo.md
 * records it: the Conexant CX20751/2 with widgets 16 to 28, its two DACs,
 * the ADCs 19 and 20, the output mixer 21, the headphone pin 22, the speaker
 * 23, the unconnected pin 24, the jack mic 25 and the internal mic 26.
 */
static void model_lenovo(void)
{
    static const uint8_t pins[] = {22, 23, 24, 25, 26};
    static const uint32_t pin_caps[] = {0x1C, 0x10, 0x1124, 0x1124, 0x20};
    static const uint32_t configs[] = {0x04211040, 0x94170110, 0x40F001F0, 0x04A11030, 0x94A70120};
    static const uint32_t wcaps[] = {0x00400581, 0x00400501, 0x0040048B, 0x0040048B, 0x0040048B};
    int i;

    params[0][HDA_PARAM_VENDOR_ID] = 0x14F1510F;
    params[1][HDA_PARAM_NODE_COUNT] = (16u << 16) | 13;
    params[1][HDA_PARAM_GPIO_COUNT] = 0x40000003;
    for (i = 16; i <= 17; i++) {
        params[i][HDA_PARAM_AUDIO_CAPS] = 0x00000C1D;
        params[i][HDA_PARAM_OUTPUT_AMP] = 0x80034A4A;
        params[i][0x0A] = 0x20060;
        params[i][0x0B] = 1;
    }
    params[18][HDA_PARAM_AUDIO_CAPS] = 0x0070000C;
    params[18][HDA_PARAM_OUTPUT_AMP] = 0x000F0707;
    for (i = 19; i <= 20; i++) {
        params[i][HDA_PARAM_AUDIO_CAPS] = 0x00100D1B;
        params[i][HDA_PARAM_INPUT_AMP] = 0x8003504A;
        params[i][HDA_PARAM_CONN_LIST] = 3;
        params[i][0x0A] = 0x20060;
        params[i][0x0B] = 1;
    }
    connections[19][0] = 24;
    connections[19][1] = 26;
    connections[19][2] = 25;
    connections[20][0] = 25;
    connections[20][1] = 26;
    connections[20][2] = 21;
    params[21][HDA_PARAM_AUDIO_CAPS] = 0x0020050B;
    params[21][HDA_PARAM_INPUT_AMP] = 0x80034A4A;
    params[21][HDA_PARAM_CONN_LIST] = 2;
    connections[21][0] = 16;
    connections[21][1] = 17;
    for (i = 0; i < 5; i++) {
        params[pins[i]][HDA_PARAM_AUDIO_CAPS] = wcaps[i];
        params[pins[i]][HDA_PARAM_PIN_CAPS] = pin_caps[i];
        configurations[pins[i]] = configs[i];
        if (pins[i] >= 24)
            params[pins[i]][HDA_PARAM_INPUT_AMP] = 0x002F0300;
        if (pins[i] <= 24) {
            params[pins[i]][HDA_PARAM_CONN_LIST] = 2;
            connections[pins[i]][0] = 16;
            connections[pins[i]][1] = 17;
        }
    }
    params[27][HDA_PARAM_AUDIO_CAPS] = params[28][HDA_PARAM_AUDIO_CAPS] = 0x00F00000;
}

/* What the model's reader was handed: the transfers, their bytes, the
 * period calls, and the first bytes of the last transfer. */
static unsigned got_transfers, got_periods;
static uint32_t got_bytes;
static uint8_t got_first[8];

/** The model reader's `transfer`: count it and keep its first bytes. */
static void got_transfer(audio_stream_t *s, uint8_t *buf, size_t size)
{
    (void)s;
    got_transfers++;
    got_bytes += (uint32_t)size;
    k_memcpy(got_first, buf, size < sizeof(got_first) ? size : sizeof(got_first));
}

/** The model reader's `period`: count it. */
static void got_period(audio_stream_t *s)
{
    (void)s;
    got_periods++;
}

/** Open and set up `s` for capture at `rate`, `format` and `channels`, with
 * a period of `bytes`; the model has one input stream before its output. */
static int open_capture(audio_stream_t *s, uint32_t rate, audio_format_t format, int channels,
                        uint32_t bytes)
{
    int rc;

    k_memset(s, 0, sizeof(*s));
    s->dir = AUDIO_CAPTURE;
    s->rate = rate;
    s->format = format;
    s->channels = channels;
    s->period_bytes = bytes;
    s->periods = 4;
    s->transfer = got_transfer;
    s->period = got_period;
    got_transfers = got_periods = 0;
    got_bytes = 0;
    num_input_streams = 1;
    input_stream_index = 0;
    output_stream_index = 1;
    dma_checked = true;
    dma_posbuf_use = false;
    rc = hda_driver.stream_open(s);
    return rc != 0 ? rc : hda_driver.stream_setup(s);
}

/** Put the input stream's position at `pos` bytes of its DMA buffer. */
static void move_input(uint32_t pos)
{
    hda_test_write(0x80 + HDA_SD_LPIB, pos, 32);
}

/**
 * On the G50-70's codec discovery records ADC 19 reading the internal mic 26
 * through its connection 1, which reaches the jack mic 25 too, and a start
 * selects it, gives the ADC the input stream's tag and format, unmutes its
 * amplifier on that connection at 0 dB, step 74, mutes its others, and
 * enables the pin's input with no VREF, which the pin lacks. With the
 * machine's subsystem the inverted right channel is muted, and the left is
 * recorded in both.
 */
static void row_capture_path(void)
{
    audio_stream_t s;
    const hda_input_report_t *in = &hda_get_report()->input;

    row = "capture-path";
    reset_model();
    model_lenovo();
    check(hda_find_output(), "enumeration succeeds");
    check(in->adc == 19 && in->index == 1 && in->pin == 26 && in->mid == 0,
          "adc 19 reads the internal mic on connection 1");
    check(in->int_mic == 26 && in->jack_mic == 25, "the internal and jack mics are found");
    check(in->level == 74 && !in->inv_dmic, "0 dB is the level; no quirk without the subsystem");
    check(in->caps.nid == 19 && in->caps.source == 19 && in->caps.pcm == 0x20060,
          "the adc's own capabilities");
    check(open_capture(&s, 48000, AUDIO_S16LE, 2, 4096) == 0, "capture set up");
    check(hda_driver.stream_start(&s) == 0, "capture starts");
    check(selected[19] == 1 && channel[19] == (HDA_CAPTURE_TAG << 4),
          "the adc selects the mic and takes the input stream's tag");
    check(converter_format[19] == 0x0011 && hda_test_read(0x80 + HDA_SD_FORMAT, 16) == 0x0011,
          "the adc and the stream record 48 kHz 16-bit stereo");
    check(in_amp[19][1][0] == (0x100 | 74) && in_amp[19][1][1] == (0x100 | 74),
          "the mic's connection is unmuted at 0 dB");
    check(in_amp[19][0][0] == 0x180 && in_amp[19][2][1] == 0x180,
          "the other connections are muted");
    check(pin_ctl[26] == 0x20 && in->pin_ctl == 0x20,
          "the internal mic's input is on, with no vref");
    check(in_amp[26][0][0] == (0x100 | 2), "the internal mic's boost is at 24 dB, step 2");
    hda_driver.stream_close(&s);
    check(channel[19] == 0, "a close takes the stream's tag from the adc");

    row = "capture-inverted-mic";
    reset_model();
    model_lenovo();
    hda_subsystem = 0x17AA3978;
    check(hda_find_output() && in->inv_dmic, "the g50-70's internal mic is inverted");
    check(open_capture(&s, 44100, AUDIO_S16LE, 2, 4096) == 0, "capture set up");
    check(hda_driver.stream_start(&s) == 0, "capture starts");
    check(in_amp[19][1][0] == (0x100 | 74) && in_amp[19][1][1] == (0x100 | 0x80 | 74),
          "the right channel is muted and the left is not");
    {
        static const uint8_t frame[] = {0x34, 0x12, 0xCC, 0xED};

        k_memcpy(cap_buffer, frame, sizeof(frame));
        hda_capture_convert(&s, cap_buffer);
        check(cap_out[0] == 0x34 && cap_out[1] == 0x12 && cap_out[2] == 0x34 && cap_out[3] == 0x12,
              "the left is recorded in both channels");
    }
    hda_driver.stream_close(&s);
}

/** The index in the verb log of the first verb from `from` on that is
 * `verb` to `nid` under `mask`, or log_count when there is none. */
static unsigned log_find(unsigned from, unsigned nid, uint32_t verb, uint32_t mask)
{
    unsigned i;

    for (i = from; i < log_count; i++)
        if ((log_command[i] >> 20) == nid && (log_command[i] & mask) == verb)
            break;
    return i;
}

/**
 * A capture's start on the G50-70 in Linux's order. The path made ready at
 * init leaves the internal mic's input on, its boost at 24 dB and ADC 19's
 * amplifier muted. A start then powers the ADC, programs the route with the
 * amplifier still muted, tags the ADC and formats it, as Linux's
 * snd_hda_codec_setup_stream does, and unmutes the left channel only
 * HDA_CAPTURE_SETTLE_US after the format; the descriptor runs
 * HDA_CAPTURE_SETTLE_US after the unmute. A start after a capture that
 * stopped part way resets the position, and the first period handed over
 * is the first one recorded.
 */
static void row_capture_start(void)
{
    audio_stream_t s;
    unsigned power, tag, format, unmute, i;
    int muted = 1;

    row = "capture-start";
    reset_model();
    model_lenovo();
    hda_subsystem = 0x17AA3978;
    check(hda_find_output(), "enumeration succeeds");
    check(hda_input_ready() && pin_ctl[26] == 0x20 && in_amp[26][0][0] == (0x100 | 2) &&
              in_amp[19][1][0] == (0x100 | 0x80 | 74) && in_amp[19][1][1] == (0x100 | 0x80 | 74) &&
              selected[19] == 1 && channel[19] == 0,
          "init leaves the mic on at 24 dB and the adc muted and untagged");
    check(open_capture(&s, 48000, AUDIO_S16LE, 2, 4096) == 0, "capture set up");
    log_count = 0;
    check(hda_driver.stream_start(&s) == 0, "capture starts");
    power = log_find(0, 19, HDA_VERB_SET_POWER, 0xFFFFF);
    tag = log_find(0, 19, HDA_VERB_SET_CHANNEL | (HDA_CAPTURE_TAG << 4), 0xFFFFF);
    format = log_find(0, 19, HDA_VERB_SET_CONV_FMT | 0x0011, 0xFFFFF);
    unmute = log_find(0, 19, HDA_VERB_SET_AMP_GAIN | 0x6000 | (1u << 8) | 74, 0xFFFFF);
    check(power < tag && tag < format && format < unmute && unmute < log_count,
          "power, tag and format, then the unmute");
    check(log_find(0, 26, HDA_VERB_SET_PIN_CTRL | 0x20, 0xFFFFF) < tag,
          "the pin's input is on before the tag");
    for (i = 0; i < unmute; i++)
        if ((log_command[i] >> 20) == 19 && (log_command[i] & 0xF0000) == HDA_VERB_SET_AMP_GAIN &&
            !(log_command[i] & 0x80))
            muted = 0;
    check(muted, "every amplifier of the adc is muted until the unmute");
    check(log_clock[unmute] - log_clock[format] >= HDA_CAPTURE_SETTLE_US * 24u,
          "the unmute waits for the converter to settle");
    check(run_at == log_count && run_clock - log_clock[unmute] >= HDA_CAPTURE_SETTLE_US * 24u,
          "the descriptor runs after the unmute has settled, with no verb after it");
    check(in_amp[19][1][0] == (0x100 | 74) && in_amp[19][1][1] == (0x100 | 0x80 | 74),
          "the left at 0 dB, the right muted");
    move_input(4096 * 2 + 64);
    clock_value += 24000 * 43;
    tick();
    hda_driver.stream_stop(&s);
    clock_stopped = 1;
    k_memset(cap_buffer, 0x7F, sizeof(cap_buffer));
    run_at = ~0u;
    check(hda_driver.stream_start(&s) == 0 && hda_test_read(0x80 + HDA_SD_LPIB, 32) == 0,
          "a second start resets the position the first left");
    got_transfers = 0;
    tick();
    check(got_transfers == 0, "nothing is handed over before the first period is recorded");
    cap_buffer[0] = 0x21;
    cap_buffer[1] = 0x43;
    move_input(4096 + 64);
    clock_value += 24000 * 22;
    tick();
    check(got_transfers == 1 && got_first[0] == 0x21 && got_first[1] == 0x43,
          "the first period handed over is the first recorded");
    hda_driver.stream_close(&s);
    clock_stopped = 0;
}

/**
 * Capture on the model ADC: a jack mic alone is recorded and biased at
 * VREF 80%; an ADC that reaches the mic through a selector selects both
 * connections; and a codec with no ADC has no capture.
 */
static void row_capture_routes(void)
{
    audio_stream_t s;
    const hda_input_report_t *in = &hda_get_report()->input;

    row = "capture-jack-mic";
    reset_model();
    model_adc();
    check(hda_find_output(), "enumeration succeeds");
    check(in->adc == 3 && in->pin == 4 && in->jack_mic == 4 && !in->int_mic, "the jack mic alone");
    check(open_capture(&s, 48000, AUDIO_S16LE, 2, 4096) == 0 && hda_driver.stream_start(&s) == 0,
          "capture starts");
    check(pin_ctl[4] == 0x24, "the jack mic is biased at vref 80%");
    hda_driver.stream_close(&s);

    row = "capture-selector";
    reset_model();
    model_adc();
    params[3][HDA_PARAM_CONN_LIST] = 2;
    connections[3][0] = 6;
    connections[3][1] = 5;
    params[5][HDA_PARAM_AUDIO_CAPS] = (HDA_WIDGET_AUDIO_SEL << 20) | (1u << 8);
    params[5][HDA_PARAM_CONN_LIST] = 2;
    connections[5][0] = 7;
    connections[5][1] = 4;
    check(hda_find_output(), "enumeration succeeds");
    check(in->adc == 3 && in->index == 1 && in->mid == 5 && in->mid_index == 1,
          "the adc reaches the mic through a selector");
    check(open_capture(&s, 48000, AUDIO_S16LE, 2, 4096) == 0 && hda_driver.stream_start(&s) == 0,
          "capture starts");
    check(selected[3] == 1 && selected[5] == 1, "both connections are selected");
    hda_driver.stream_close(&s);

    row = "capture-none";
    reset_model();
    check(hda_find_output() && hda_get_report()->input.adc == 0, "a codec with no adc");
    num_input_streams = 1;
    s.dir = AUDIO_CAPTURE;
    check(hda_driver.stream_open(&s) == -ENODEV, "has no capture");
}

/**
 * The input stream alone: stream_setup grants the ADC's rate and a period
 * whole in DMA descriptors; a start programs the descriptor; each service
 * hands over the periods the position has passed, converted to the reader's
 * format, and counts their frames; the position moves within a period; a
 * second open is refused; and after a stop nothing more is handed over.
 */
static void row_capture_stream(void)
{
    audio_stream_t s, t;
    const hda_input_report_t *in = &hda_get_report()->input;
    uint32_t i;

    row = "capture-stream";
    reset_model();
    model_adc();
    params[1][0x0A] = 0x20000 | (1u << 2) | (1u << 6); /* 16000 and 48000 Hz */
    check(hda_find_output(), "enumeration succeeds");
    check(open_capture(&s, 8000, AUDIO_U8, 1, 1000) == 0, "capture set up");
    check(s.rate == 16000, "the nearest rate the adc has is granted");
    check(s.period_bytes == 992 && s.device_periods == 16,
          "992 frames, 31 descriptors' 128 bytes in stereo, in 16 periods");
    k_memset(&t, 0, sizeof(t));
    t.dir = AUDIO_CAPTURE;
    check(hda_driver.stream_open(&t) == -EBUSY, "a second capture is refused");
    clock_stopped = 1;
    check(hda_driver.stream_start(&s) == 0, "capture starts");
    check(hda_test_read(0x80 + HDA_SD_CBL, 32) == 3968 * 16 &&
              hda_test_read(0x80 + HDA_SD_LVI, 16) == 15,
          "sixteen periods of 3968 bytes");
    check((hda_test_read(0x80 + HDA_SD_CTL, 32) & 0xF00006) ==
              (0x200000 | HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE),
          "the stream runs with its tag and its interrupt");
    check(cap_bdl[1].length == 3968 && cap_bdl[1].ioc == 1, "each period interrupts");
    check(in->running && in->rate == 16000 && in->requested_rate == 8000 && in->format == 0x0211,
          "the report has the stream");
    for (i = 0; i < 3968 * 16; i += 4) {
        cap_buffer[i + 1] = 0x40;
        cap_buffer[i + 3] = 0x20;
    }
    tick();
    check(got_transfers == 0, "nothing is handed over before a period ends");
    move_input(3968 * 3 + 400);
    clock_value += 3 * 24000 * 62;
    tick();
    check(got_transfers == 3 && got_periods == 3 && got_bytes == 3 * 992,
          "three periods are handed over");
    check(got_first[0] == 0xB0, "u8 mono is the mean of the channels, 0x3000, offset");
    check(in->frames == 3 * 992 && in->overruns == 0, "the frames are counted");
    check(hda_driver.stream_position(&s) == 3 * 992 + 100, "the position moves within a period");
    hda_irq_handler(NULL);
    check(got_transfers == 3, "an interrupt for no stream hands over nothing");
    move_input(3968 * 5);
    clock_value += 2 * 24000 * 62;
    hda_test_write(HDA_INTSTS, 1, 32);
    hda_irq_handler(NULL);
    check(got_transfers == 5 && in->frames == 5 * 992, "the input stream's interrupt serves it");
    hda_driver.stream_stop(&s);
    check(!in->running && !(hda_test_read(0x80 + HDA_SD_CTL, 8) & HDA_SD_CTL_RUN),
          "a stop halts the stream");
    move_input(3968 * 9);
    tick();
    check(got_transfers == 5, "nothing is handed over after a stop");
    check(hda_driver.stream_position(&s) == 5 * 992, "the position stays where it stopped");
    hda_driver.stream_close(&s);
    check(hda_driver.stream_open(&t) == 0, "a close gives the stream back");
    hda_driver.stream_close(&t);
    clock_stopped = 0;
}

/**
 * Capture runs beside playback: each has its own descriptor and tag, the
 * interrupt serves both, and a stop of one leaves the other running.
 */
static void row_capture_duplex(void)
{
    audio_stream_t s;
    const hda_input_report_t *in = &hda_get_report()->input;

    row = "capture-duplex";
    reset_model();
    model_adc();
    check(hda_find_output(), "enumeration succeeds");
    check(open_capture(&s, 48000, AUDIO_S16LE, 2, 4096) == 0, "capture set up");
    clock_stopped = 1;
    check(hda_driver.stream_start(&s) == 0, "capture starts");
    check(hda_start(48000, AUDIO_S16LE, 2) == 0, "playback starts beside it");
    check((hda_test_read(0xA0 + HDA_SD_CTL, 32) >> 20) == 1 &&
              (hda_test_read(0x80 + HDA_SD_CTL, 32) >> 20) == HDA_CAPTURE_TAG,
          "each stream has its own tag");
    check(channel[2] == 0x10 && channel[3] == (HDA_CAPTURE_TAG << 4), "and so has each converter");
    move_input(4096 * 2);
    clock_value += 2 * 24000 * 21;
    hda_test_write(HDA_INTSTS, 3, 32);
    hda_irq_handler(NULL);
    check(got_transfers == 2 && got_bytes == 2 * 4096, "the shared interrupt serves capture");
    hda_stop();
    check(in->running && (hda_test_read(0x80 + HDA_SD_CTL, 8) & HDA_SD_CTL_RUN),
          "a stop of playback leaves capture running");
    move_input(4096 * 4);
    clock_value += 2 * 24000 * 21;
    tick();
    check(got_transfers == 2, "the tick leaves a stream whose interrupts come to them");
    kernel_ticks += 32; /* a period and a half of 21 ms, and the next interrupt is late */
    tick();
    check(got_transfers == 4, "capture goes on alone, from the tick once its interrupt is late");
    check(hda_start(48000, AUDIO_S16LE, 2) == 0, "playback starts again");
    hda_driver.stream_stop(&s);
    check(playing && (hda_test_read(0xA0 + HDA_SD_CTL, 8) & HDA_SD_CTL_RUN),
          "a stop of capture leaves playback running");
    hda_stop();
    hda_driver.stream_close(&s);
    clock_stopped = 0;
}

/**
 * A service late by a whole revolution of the DMA buffer counts an overrun,
 * found by the wall clock where the position cannot show it, and hands over
 * the newest revolution: every period but the one the device writes. So
 * does one that finds the device come round to the period it hands over
 * next. One on time counts none.
 */
static void row_capture_overrun(void)
{
    audio_stream_t s;
    const hda_input_report_t *in = &hda_get_report()->input;

    row = "capture-overrun";
    reset_model();
    model_adc();
    check(hda_find_output(), "enumeration succeeds");
    /* 1024 frames a period at 48 kHz, 21.3 ms; sixteen periods, 341 ms. */
    check(open_capture(&s, 48000, AUDIO_S16LE, 2, 4096) == 0 && s.device_periods == 16,
          "capture set up");
    clock_stopped = 1;
    check(hda_driver.stream_start(&s) == 0, "capture starts");
    move_input(4096 * 2 + 100);
    clock_value += 24000 * 44;
    tick();
    check(got_transfers == 2 && in->overruns == 0 && s.lost == 0, "a service on time");
    move_input(4096 * 3);
    clock_value += 24000 * (21 + 341);
    tick();
    check(in->overruns == 1, "a revolution the position cannot show is an overrun");
    check(got_transfers == 2 + 15 && got_periods == 2 + 15,
          "every period but the one being written is handed over");
    check(hda_driver.stream_position(&s) == 3 * 1024 + 16 * 1024,
          "the position counts the revolution");
    check(s.lost == 2 * 1024, "the two periods neither handed over nor written are lost");
    move_input(4096 * 3 + 2000);
    clock_value += 24000 * 10;
    tick();
    check(got_transfers == 17 && in->overruns == 1 && s.lost == 2 * 1024,
          "a service within a period hands over none and loses none");
    /* 64036 bytes on, 16009 frames: 334 ms, less than a revolution. */
    move_input(4096 * 3 + 500);
    clock_value += 24000 * 334;
    tick();
    check(in->overruns == 2, "the device come round to the next period is an overrun");
    check(got_transfers == 17 + 15, "and the newest revolution is handed over");
    check(s.lost == 3 * 1024, "the period it overwrote is lost");
    hda_driver.stream_close(&s);
    clock_stopped = 0;
}

/**
 * An input interrupt that comes while the position still reads the second
 * half of the period before hands over nothing and leaves the stream pending
 * (`cap_irq_pending`); the tick asks at each call and hands the period over
 * once the position has passed it. An interrupt for a period the tick
 * already handed over, after a late one, finds the position in the first
 * half of the next and clears the flag.
 */
static void row_capture_early(void)
{
    audio_stream_t s;

    row = "capture-irq-early";
    reset_model();
    model_adc();
    check(hda_find_output(), "enumeration succeeds");
    /* 1024 frames a period at 48 kHz, 21.3 ms. */
    check(open_capture(&s, 48000, AUDIO_S16LE, 2, 4096) == 0, "capture set up");
    clock_stopped = 1;
    check(hda_driver.stream_start(&s) == 0, "capture starts");
    move_input(3000);
    clock_value += 24000 * 16;
    hda_test_write(HDA_INTSTS, 1u << input_stream_index, 32);
    hda_irq_handler(NULL);
    hda_test_write(HDA_INTSTS, 0, 32);
    check(cap_irq_pending && got_transfers == 0,
          "an interrupt before the period's end hands over nothing and stays pending");
    tick();
    check(cap_irq_pending && got_transfers == 0,
          "the tick hands over nothing while the position reads the first period");
    move_input(4096 + 200);
    clock_value += 24000 * 6;
    tick();
    check(!cap_irq_pending && got_transfers == 1,
          "the next tick hands the period over once the position has passed it");
    move_input(2 * 4096 + 100);
    clock_value += 24000 * 21;
    kernel_ticks += 33; /* a period and a half of 21.3 ms, and a tick more */
    tick();
    check(got_transfers == 2, "the tick hands the next period over once the interrupt is late");
    hda_test_write(HDA_INTSTS, 1u << input_stream_index, 32);
    hda_irq_handler(NULL);
    hda_test_write(HDA_INTSTS, 0, 32);
    check(!cap_irq_pending && got_transfers == 2,
          "the late interrupt hands over nothing and leaves the stream not pending");
    hda_driver.stream_close(&s);
    clock_stopped = 0;
}

/** Run the ticks of `n` jack polls, a quarter second each. */
static void poll_jacks(unsigned n)
{
    unsigned i;

    for (i = 0; i < n * HDA_JACK_TICKS; i++)
        tick();
}

/** Enumerate the G50-70's codec and start playback on it at 48 kHz; the wall
 * clock stops, so that the ticks refill nothing. */
static void lenovo_playing(void)
{
    reset_model();
    model_lenovo();
    check(hda_find_output(), "enumeration succeeds");
    dma_checked = true;
    clock_stopped = 1;
    check(hda_start(48000, AUDIO_S16LE, 2) == 0, "playback starts");
}

/**
 * On the G50-70's codec the headphone pin 22 and the jack mic 25 are
 * watched, and a Conexant codec starts in the automatic mode. A start takes
 * the headphone jack's state from its reading; the tick polls every quarter
 * second, takes a plug seen twice and moves the output to the headphones,
 * the speaker 23 off, and back on an unplug. Another mode keeps its pins.
 */
static void row_jack_switch(void)
{
    const hda_report_t *r = hda_get_report();
    const hda_jack_report_t *hp = &r->jacks[HDA_JACK_HP];
    unsigned i, n;

    row = "jack-switch";
    reset_model();
    model_lenovo();
    check(hda_find_output(), "enumeration succeeds");
    check(hda_get_output_mode() == 0, "a conexant codec starts in the automatic mode");
    check(hp->pin == 22 && r->jacks[HDA_JACK_MIC].pin == 25 && hp->state == HDA_JACK_UNKNOWN &&
              r->jacks[HDA_JACK_MIC].state == HDA_JACK_UNKNOWN,
          "the headphone and mic jacks are watched, their states unknown");
    dma_checked = true;
    clock_stopped = 1;
    check(hda_start(48000, AUDIO_S16LE, 2) == 0, "playback starts");
    check(hp->state == HDA_JACK_OUT && hp->polls == 1 && hp->changes == 1,
          "the start reads the headphone jack");
    check(pin_ctl[23] == 0x40 && pin_ctl[22] == 0 && r->primary_enabled && !r->hp_enabled,
          "the speaker plays without headphones");
    sense[22] = 0x80000000;
    n = verbs;
    for (i = 0; i + 1 < HDA_JACK_TICKS; i++)
        tick();
    check(verbs == n, "no poll before a quarter second");
    tick();
    check(verbs == n + 2 && hp->polls == 2 && hp->state == HDA_JACK_OUT && pin_ctl[22] == 0,
          "a plug seen once moves nothing");
    poll_jacks(1);
    check(hp->state == HDA_JACK_IN && hp->changes == 2, "a plug seen twice is taken");
    check(pin_ctl[22] == 0xC0 && pin_ctl[23] == 0 && r->hp_enabled && !r->primary_enabled,
          "the headphones play and the speaker is off");
    n = verbs;
    poll_jacks(1);
    check(verbs == n + 2 && pin_ctl[22] == 0xC0, "a steady plug is read and sets nothing");
    sense[22] = 0;
    poll_jacks(2);
    check(hp->state == HDA_JACK_OUT && hp->changes == 3 && pin_ctl[23] == 0x40 && pin_ctl[22] == 0,
          "an unplug seen twice gives the speaker back");
    hda_set_output_mode(1);
    sense[22] = 0x80000000;
    poll_jacks(2);
    check(hp->state == HDA_JACK_IN && pin_ctl[22] == 0 && pin_ctl[23] == 0x40,
          "the primary mode keeps the speaker whatever the jack");
    hda_set_output_mode(0);
    poll_jacks(1);
    check(pin_ctl[22] == 0xC0 && pin_ctl[23] == 0, "the automatic mode follows the jack again");
    n = verbs;
    i = hp->polls;
    kernel_ticks += HDA_JACK_TICKS - 1;
    tick();
    check(verbs == n + 2 && hp->polls == i + 1,
          "one tick after a gap of a quarter second polls the jacks");
    hda_stop();
    clock_stopped = 0;
}

/**
 * A plug that bounces at each poll moves nothing. A reading whose verb times
 * out, or finds the interface or the verb lock busy, is unknown and leaves
 * the headphones playing; the unplug is taken once the codec answers. The
 * tick polls nothing while code outside it holds the jacks.
 */
static void row_jack_reads(void)
{
    const hda_jack_report_t *hp = &hda_get_report()->jacks[HDA_JACK_HP];
    unsigned i, n;

    row = "jack-debounce";
    lenovo_playing();
    for (i = 0; i < 6; i++) {
        sense[22] = i % 2 ? 0 : 0x80000000;
        poll_jacks(1);
    }
    check(hp->state == HDA_JACK_OUT && hp->changes == 1 && pin_ctl[23] == 0x40 && pin_ctl[22] == 0,
          "a plug that bounces at each poll moves nothing");
    hda_stop();
    clock_stopped = 0;

    row = "jack-failed-read";
    lenovo_playing();
    sense[22] = 0x80000000;
    poll_jacks(2);
    check(pin_ctl[22] == 0xC0 && hp->polls == 3 && hp->failed == 0, "the headphones play");
    sense[22] = 0;
    codec_silent = 1;
    poll_jacks(2);
    check(hp->state == HDA_JACK_IN && hp->polls == 5 && hp->failed == 2,
          "a read that times out, then one behind it, are unknown");
    check(pin_ctl[22] == 0xC0 && pin_ctl[23] == 0, "and not an unplug");
    mmio[HDA_ICS] = 0;
    codec_silent = 0;
    verb_lock.locked = 1;
    poll_jacks(2);
    check(hp->state == HDA_JACK_IN && hp->failed == 4 && irqs_on,
          "a poll that finds the verb lock held reads nothing");
    verb_lock.locked = 0;
    poll_jacks(2);
    check(hp->state == HDA_JACK_OUT && pin_ctl[23] == 0x40 && pin_ctl[22] == 0,
          "the unplug is taken once the codec answers");

    row = "jack-hold";
    sense[22] = 0x80000000;
    jack_holds = 1;
    n = verbs;
    poll_jacks(2);
    check(verbs == n && hp->state == HDA_JACK_OUT,
          "the tick polls nothing while the jacks are held");
    jack_holds = 0;
    poll_jacks(2);
    check(hp->state == HDA_JACK_IN && pin_ctl[22] == 0xC0, "it polls again once they are not");
    hda_stop();
    clock_stopped = 0;
}

/**
 * A start turns EAPD on, with BTL on the speaker, and a stop turns it and
 * the output pins off. A plug while nothing plays turns no pin on, and the
 * next start plays on the headphones at once.
 */
static void row_jack_stop(void)
{
    const hda_report_t *r = hda_get_report();

    row = "jack-stop";
    reset_model();
    model_lenovo();
    params[22][HDA_PARAM_PIN_CAPS] |= 0x10000; /* EAPD */
    params[23][HDA_PARAM_PIN_CAPS] |= 0x10000;
    check(hda_find_output(), "enumeration succeeds");
    dma_checked = true;
    clock_stopped = 1;
    check(hda_start(48000, AUDIO_S16LE, 2) == 0, "playback starts");
    check(eapd[23] == (0x100 | 3) && eapd[22] == (0x100 | 2),
          "a start turns EAPD on, with BTL on the speaker");
    hda_stop();
    check(eapd[23] == 0x100 && eapd[22] == 0x100, "a stop turns EAPD off");
    check(pin_ctl[23] == 0 && pin_ctl[22] == 0 && !r->primary_enabled && !r->hp_enabled,
          "and the output pins");
    sense[22] = 0x80000000;
    poll_jacks(2);
    check(r->jacks[HDA_JACK_HP].state == HDA_JACK_IN && pin_ctl[22] == 0 && pin_ctl[23] == 0,
          "a plug while stopped turns no pin on");
    check(hda_start(48000, AUDIO_S16LE, 2) == 0 && pin_ctl[22] == 0xC0 && pin_ctl[23] == 0 &&
              eapd[23] == (0x100 | 3),
          "the next start plays on the headphones");
    hda_stop();
    clock_stopped = 0;
}

/**
 * The end of row_jack_capture, on its running stream S: a stop leaves the
 * pin's input on, a plug while stopped routes the jack mic with its
 * connection muted, and the next start records it. Closes S.
 */
static void jack_capture_stopped(audio_stream_t *s)
{
    const hda_input_report_t *in = &hda_get_report()->input;

    hda_driver.stream_stop(s);
    check(pin_ctl[26] == 0x20 && channel[19] == 0 && (in_amp[19][1][0] & 0x80) &&
              (in_amp[19][1][1] & 0x80),
          "a stop mutes and untags the adc and leaves the pin's input on");
    sense[25] = 0;
    poll_jacks(2);
    sense[25] = 0x80000000;
    poll_jacks(2);
    check(in->pin == 25 && selected[19] == 2 && pin_ctl[25] == 0x24 && pin_ctl[26] == 0 &&
              (in_amp[19][2][0] & 0x80),
          "a plug while stopped routes the jack mic at once, muted");
    check(hda_driver.stream_start(s) == 0 && selected[19] == 2 && pin_ctl[25] == 0x24,
          "the next start records it");
    hda_driver.stream_close(s);
    clock_stopped = 0;
}

/**
 * While capture runs on the G50-70's internal mic 26, a headset's plug seen
 * twice moves it to the jack mic 25: ADC 19 selects its connection 2 at
 * 0 dB and mutes the internal mic's, the jack mic is biased at VREF 80% and
 * the internal mic's input is off. An unplug moves it back. The mixer may
 * choose the internal mic with the plug in, which a steady jack leaves. A
 * stop leaves the pin's input on. A plug while capture is stopped routes
 * the jack mic with its connection muted, and the next start records it.
 */
static void row_jack_capture(void)
{
    audio_stream_t s;
    const hda_report_t *r = hda_get_report();
    const hda_input_report_t *in = &r->input;

    row = "jack-capture";
    reset_model();
    model_lenovo();
    check(hda_find_output(), "enumeration succeeds");
    clock_stopped = 1;
    check(open_capture(&s, 48000, AUDIO_S16LE, 2, 4096) == 0 && hda_driver.stream_start(&s) == 0,
          "capture starts");
    check(in->pin == 26 && selected[19] == 1 && pin_ctl[26] == 0x20,
          "the internal mic is recorded");
    sense[25] = 0x80000000;
    poll_jacks(1);
    check(in->pin == 26 && selected[19] == 1, "a plug seen once moves nothing");
    poll_jacks(1);
    check(r->jacks[HDA_JACK_MIC].state == HDA_JACK_IN && in->pin == 25 && in->index == 2 &&
              selected[19] == 2,
          "a plug seen twice moves capture to the jack mic");
    check(pin_ctl[25] == 0x24 && pin_ctl[26] == 0, "the jack mic is biased, the internal mic off");
    check(in_amp[19][2][0] == (0x100 | 74) && in_amp[19][1][0] == 0x180,
          "the jack's connection is at 0 dB and the internal mic's muted");
    check(in->running && cap_running, "capture runs on");
    sense[25] = 0;
    poll_jacks(2);
    check(in->pin == 26 && selected[19] == 1 && pin_ctl[26] == 0x20 && pin_ctl[25] == 0,
          "an unplug moves capture back to the internal mic");
    sense[25] = 0x80000000;
    poll_jacks(2);
    check(in->pin == 25, "a plug again");
    check(hda_driver.input_select(AUDIO_SRC_INT_MIC) == AUDIO_SRC_INT_MIC && selected[19] == 1 &&
              pin_ctl[25] == 0 && pin_ctl[26] == 0x20,
          "the mixer chooses the internal mic with the plug in");
    poll_jacks(2);
    check(in->pin == 26, "and a steady jack leaves its choice");
    jack_capture_stopped(&s);
}

/**
 * A capture start whose codec path fails: on the G50-70's codec, ADC 19
 * refuses its format verb, then internal mic 26 its widget control. Each
 * start returns -EIO and leaves capture stopped, its descriptor halted, the
 * ADC untagged and the pin's input off, and keeps jack mic 25, which
 * capture moved from, for the next try. Once the codec takes every verb,
 * the next start records and turns the jack mic's input off.
 */
static void row_capture_fails(void)
{
    audio_stream_t s;
    const hda_input_report_t *in = &hda_get_report()->input;

    row = "capture-fails";
    reset_model();
    model_lenovo();
    check(hda_find_output(), "enumeration succeeds");
    clock_stopped = 1;
    check(open_capture(&s, 48000, AUDIO_S16LE, 2, 4096) == 0, "capture set up");
    mic_left = 25;
    mic_program = true;
    fail_mask = 0x7FF0000;
    fail_command = (19u << 20) | HDA_VERB_SET_CONV_FMT;
    check(hda_driver.stream_start(&s) == -EIO, "a refused format verb fails the start");
    check(!cap_running && !in->running && !(hda_test_read(0x80 + HDA_SD_CTL, 8) & HDA_SD_CTL_RUN),
          "capture stays stopped");
    check(channel[19] == 0 && pin_ctl[26] == 0, "the adc is untagged and the pin's input off");
    check(mic_program && mic_left == 25, "the pin left waits for the next try");
    fail_mask = 0x7FFFF00;
    fail_command = (26u << 20) | HDA_VERB_SET_PIN_CTRL;
    check(hda_driver.stream_start(&s) == -EIO && !cap_running && channel[19] == 0,
          "a refused route verb fails the start too");
    check(mic_program && mic_left == 25, "and the pin left still waits");
    fail_mask = 0;
    pin_ctl[25] = 0x24;
    check(hda_driver.stream_start(&s) == 0 && cap_running && in->running, "the next start records");
    check(channel[19] == (HDA_CAPTURE_TAG << 4) && converter_format[19] == in->format &&
              pin_ctl[26] == 0x20,
          "the path is programmed");
    check(pin_ctl[25] == 0 && !mic_program && mic_left == 0, "and the pin left is turned off");
    hda_driver.stream_stop(&s);
    hda_driver.stream_close(&s);
    clock_stopped = 0;
}

/**
 * The input controls over the G50-70's codec: the two mics are the
 * sources, with a gain on ADC 19 and a boost on each pin. The gain reads 93
 * at its 0 dB step, and a level set is its step at the next start. A source
 * not recorded stays muted; one selected while the stream runs is routed at
 * once, its gain unmuted, the other connection muted and the jack biased.
 * A boost is set on the pin, 0 mutes the gain, a line in the codec lacks is
 * refused, and a source selected while stopped is routed at once with its
 * connection muted until the next start.
 */
static void row_input_controls(void)
{
    audio_stream_t s;
    const hda_input_report_t *in = &hda_get_report()->input;
    unsigned controls = 0;

    row = "input-controls";
    reset_model();
    model_lenovo();
    check(hda_find_output(), "enumeration succeeds");
    check(hda_driver.input_caps(&controls) == ((1u << AUDIO_SRC_MIC) | (1u << AUDIO_SRC_INT_MIC)) &&
              controls == ((1u << AUDIO_IN_GAIN) | (1u << AUDIO_IN_BOOST)),
          "the two mics, a gain and a boost");
    check(hda_driver.input_select(-1) == AUDIO_SRC_INT_MIC, "the internal mic is recorded");
    check(hda_driver.input_level(AUDIO_IN_GAIN, AUDIO_SRC_INT_MIC, -1) == 93 &&
              hda_driver.input_level(AUDIO_IN_BOOST, AUDIO_SRC_INT_MIC, -1) == 67 &&
              hda_driver.input_level(AUDIO_IN_BOOST, AUDIO_SRC_MIC, -1) == 0,
          "0 dB reads 93, the internal mic's boost of 24 dB 67, and the jack's 0");
    check(hda_driver.input_level(AUDIO_IN_GAIN, AUDIO_SRC_INT_MIC, 50) == 50 && in->level == 40,
          "50 is step 40");
    check(open_capture(&s, 48000, AUDIO_S16LE, 2, 4096) == 0 && hda_driver.stream_start(&s) == 0,
          "capture starts");
    check(in_amp[19][1][0] == (0x100 | 40) && in_amp[19][1][1] == (0x100 | 40),
          "the start sets step 40");
    check(hda_driver.input_level(AUDIO_IN_GAIN, AUDIO_SRC_MIC, 100) == 100 &&
              in_amp[19][2][0] == 0x180,
          "a source not recorded stays muted");
    check(hda_driver.input_select(AUDIO_SRC_MIC) == AUDIO_SRC_MIC, "the jack is selected");
    check(selected[19] == 2 && in->pin == 25 && in->index == 2 && in->level == 80,
          "the adc reads the jack");
    check(in_amp[19][2][0] == (0x100 | 80) && in_amp[19][1][0] == 0x180,
          "the jack at step 80, the internal mic muted");
    check(pin_ctl[25] == 0x24, "the jack is biased at vref 80%");
    check(hda_driver.input_level(AUDIO_IN_BOOST, AUDIO_SRC_MIC, 100) == 100 &&
              in_amp[25][0][0] == (0x100 | 3),
          "the jack's boost is its last step, 36 dB");
    check(hda_driver.input_level(AUDIO_IN_GAIN, AUDIO_SRC_MIC, 0) == 0 &&
              in_amp[19][2][0] == 0x180 && in_amp[19][2][1] == 0x180,
          "0 mutes the gain");
    check(hda_driver.input_level(AUDIO_IN_GAIN, AUDIO_SRC_LINE, 50) == -EINVAL &&
              hda_driver.input_select(AUDIO_SRC_LINE) == AUDIO_SRC_MIC,
          "no line in");
    hda_driver.stream_close(&s);
    check(hda_driver.input_select(AUDIO_SRC_INT_MIC) == AUDIO_SRC_INT_MIC && selected[19] == 1 &&
              pin_ctl[26] == 0x20 && pin_ctl[25] == 0 && (in_amp[19][1][0] & 0x80),
          "a source selected while stopped is routed at once, muted");

    row = "input-controls-none";
    reset_model();
    check(hda_find_output() && hda_driver.input_caps(&controls) == 0 && controls == 0 &&
              hda_driver.input_select(AUDIO_SRC_MIC) == -ENODEV,
          "a codec with no adc has no sources");
}

/** Run `n` ticks of the boot processor: the clock, then the speaker's and
 * HDA's work, as the kernel's tick does. */
static void beep_ticks(unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        kernel_ticks++;
        speaker_tick();
        hda_tick();
    }
}

/** Run the services asked for since the last call, as khda does once the
 * host test stands for it (hda_test_threaded). */
static void run_khda(void)
{
    uint32_t wanted = service_wanted;

    service_wanted = 0;
    hda_serve(wanted);
}

/**
 * The G50-70's beep with khda running: a tone and the tick's stop each hand
 * their divider to khda and send no verb, khda sends it, turning the
 * speaker's pin on first, and waits while code outside its service holds
 * the jacks; a tone that comes before khda sends a stop replaces it, so the
 * stop is never sent after the tone; a second of silence turns the pin off.
 */
static void beep_threaded(void)
{
    const hda_report_t *r = hda_get_report();
    struct speaker_counters c;
    unsigned before;

    reset_model();
    model_lenovo();
    check(hda_find_output(), "enumeration succeeds");
    dma_checked = true;
    speaker_set_beep(hda_driver.beep);
    hda_test_threaded = true;
    service_wanted = 0;
    before = beep_verbs;
    hda_jack_hold();
    speaker_tone(SPEAKER_CLOCK / 750, 100);
    speaker_get_counters(&c);
    check(beep_verbs == before && beep_owed && (service_wanted & HDA_SERVE_BEEP),
          "a tone hands divider 16 to khda and sends no verb");
    check(c.beeps == 1 && c.beep_divider == 16, "the counters name the divider handed on");
    run_khda();
    check(beep_verbs == before && pin_ctl[23] == 0 && beep_owed,
          "khda leaves the pins and the tone while the jacks are held");
    hda_jack_release();
    beep_ticks(1);
    run_khda();
    check(beep_ctl[18] == (0x100 | 16) && pin_ctl[23] == 0x40 && r->beep_path && !beep_owed,
          "once they are not, the tick asks again and khda turns the pin on and sends it");
    beep_ticks(99);
    speaker_get_counters(&c);
    check(beep_ctl[18] == (0x100 | 16) && beep_owed && c.ends == 1 && c.beep_retries == 0 &&
              c.beep_divider == 0,
          "the tick ending the tone hands the stop to khda and sends no verb");
    before = beep_verbs;
    speaker_tone(SPEAKER_CLOCK / 1000, 100);
    run_khda();
    check(beep_ctl[18] == (0x100 | 12) && beep_verbs == before + 1 && !beep_owed,
          "a tone before khda sends the stop replaces it");
    speaker_tone(0, 0);
    run_khda();
    check(beep_ctl[18] == 0x100 && pin_ctl[23] == 0x40 && !beep_owed,
          "khda sends a request's stop and leaves the pin on");
    beep_ticks(HDA_BEEP_IDLE_TICKS);
    run_khda();
    check(pin_ctl[23] == 0 && !r->beep_path, "and turns it off after a second of silence");
    hda_test_threaded = false;
    service_wanted = 0;
}

/**
 * The G50-70's beep during playback: a tone of 1000 Hz sends divider 12,
 * and a stop of playback during it leaves the speaker's pin on until the
 * tone ends and a second of silence follows.
 */
static void beep_during_playback(void)
{
    const hda_report_t *r = hda_get_report();

    clock_stopped = 1;
    check(hda_start(48000, AUDIO_S16LE, 2) == 0 && pin_ctl[23] == 0x40, "playback starts");
    speaker_tone(SPEAKER_CLOCK / 1000, 100);
    check(beep_ctl[18] == (0x100 | 12), "a tone of 1000 Hz during playback sends divider 12");
    hda_stop();
    check(pin_ctl[23] == 0x40 && r->beep_path, "a stop during the tone leaves the pin on");
    beep_ticks(100);
    check(beep_ctl[18] == 0x100 && pin_ctl[23] == 0x40, "the tick ends the tone");
    kernel_ticks += HDA_BEEP_IDLE_TICKS - 1;
    beep_ticks(1);
    check(pin_ctl[23] == 0 && !r->beep_path,
          "and the pin goes off at the one tick after a gap of a second");
    clock_stopped = 0;
}

/**
 * A beep widget with power control is powered and a mixer that lists it has
 * that input unmuted at 0 dB; a codec without one leaves the PIT alone to
 * sound, with no verb.
 */
static void beep_other_codecs(void)
{
    const hda_report_t *r = hda_get_report();
    unsigned before;
    int i;

    reset_model();
    model_lenovo();
    params[18][HDA_PARAM_AUDIO_CAPS] |= 1u << 10; /* Power Control */
    params[21][HDA_PARAM_CONN_LIST] = 3;
    connections[21][2] = 18;
    check(hda_find_output(), "enumeration succeeds with a powered beep");
    check(power[18] == 0x100 && in_amp[21][2][0] == (0x100 | 0x4A) &&
              in_amp[21][2][1] == (0x100 | 0x4A),
          "the widget is powered and the mixer's input from it unmuted at 0 dB");

    reset_model();
    check(hda_find_output(), "enumeration succeeds without a beep widget");
    check(r->beep == 0 && hda_driver.beep_level(-1) == -ENODEV &&
              hda_driver.beep(750, 0) == -ENODEV,
          "a codec without a beep widget has no generator");
    speaker_set_beep(hda_driver.beep_level(-1) >= 0 ? hda_driver.beep : NULL);
    before = verbs;
    speaker_tone(SPEAKER_CLOCK / 750, 100);
    check(verbs == before && pit_count == SPEAKER_CLOCK / 750 && (port61 & 3) == 3,
          "a tone sounds the PIT alone");
    for (i = 0; i < 100; i++)
        beep_ticks(1);
    check((port61 & 3) == 0 && verbs == before, "and the tick ends it without a verb");
}

/**
 * The G50-70's beep widget 18 has no power control and no connection, and
 * an output amplifier of 7 steps whose 0 dB step is the last: enumeration
 * sets it there and leaves the generator silent. KDMKTONE's 750 Hz for
 * 100 ms sounds the PIT and sends Set Beep Generation 16, 12000 / 750,
 * with the speaker's pin turned on, since nothing plays; the tick that ends
 * the tone sends 0, a busy interface delays that by a tick, and a second of
 * silence turns the pin off again. KIOCSOUND 0 stops a sound, a level of 0
 * sends 0, and a stop of playback during a tone leaves the pin on until the
 * tone ends. With khda running, a tone and the tick's stop are handed to
 * it, and a tone replaces a stop not yet sent. A widget with power control
 * is powered, a mixer that lists the widget has that input unmuted at 0 dB,
 * and a codec without one leaves the PIT alone to sound.
 */
static void row_beep(void)
{
    const hda_report_t *r = hda_get_report();
    struct speaker_counters c;
    unsigned before;

    row = "beep";
    reset_model();
    model_lenovo();
    check(hda_find_output(), "enumeration succeeds");
    dma_checked = true;
    check(r->beep == 18 && out_amp[18] == 0xB007 && beep_ctl[18] == 0x100 && power[18] == 0,
          "the beep widget is set at 0 dB and silent, with no power state of its own");
    check(power[1] == 0x100, "the function group is powered");
    check(hda_driver.beep_level(-1) == 100, "the beep level starts at the 0 dB step, 100");
    speaker_set_beep(hda_driver.beep_level(-1) >= 0 ? hda_driver.beep : NULL);
    kernel_ticks += 1000;
    port61 = 0;
    speaker_tone(SPEAKER_CLOCK / 750, 100);
    speaker_get_counters(&c);
    check(beep_ctl[18] == (0x100 | 16) && pit_count == SPEAKER_CLOCK / 750 && (port61 & 3) == 3,
          "a tone of 750 Hz sends divider 16 and sounds the PIT");
    check(pin_ctl[23] == 0x40 && r->primary_enabled && r->beep_path,
          "with nothing playing the speaker's pin is turned on");
    check(c.beeps == 1 && c.beep_divider == 16 && r->beeps == 1, "the counters name the beep");
    beep_ticks(99);
    check(beep_ctl[18] == (0x100 | 16) && (port61 & 3) == 3, "the tone sounds for 99 ticks");
    mmio[HDA_ICS] = HDA_ICS_ICB;
    beep_ticks(1);
    speaker_get_counters(&c);
    check((port61 & 3) == 0 && beep_ctl[18] == (0x100 | 16) && c.beep_retries == 1,
          "the tick ending the tone finds the interface busy and silences the PIT");
    mmio[HDA_ICS] = 0;
    beep_ticks(1);
    speaker_get_counters(&c);
    check(beep_ctl[18] == 0x100 && c.ends == 1 && c.beep_divider == 0,
          "the next tick sends the stop");
    beep_ticks(HDA_BEEP_IDLE_TICKS - 2);
    check(pin_ctl[23] == 0x40, "the pin stays on through a short silence");
    beep_ticks(2);
    check(pin_ctl[23] == 0 && !r->primary_enabled && !r->beep_path,
          "a second of silence turns it off");

    speaker_tone(SPEAKER_CLOCK / 750, 0);
    check(beep_ctl[18] == (0x100 | 16), "KIOCSOUND sounds the beep");
    beep_ticks(3);
    check(beep_ctl[18] == (0x100 | 16), "and no tick ends it");
    speaker_tone(0, 0);
    check(beep_ctl[18] == 0x100 && (port61 & 3) == 0, "KIOCSOUND 0 stops it");

    check(hda_driver.beep_level(50) == 57 && out_amp[18] == 0xB004,
          "a level of 50 sets step 4 of 7, which reads 57");
    check(hda_driver.beep_level(0) == 0, "a level of 0 reads 0");
    before = r->beeps;
    speaker_tone(SPEAKER_CLOCK / 750, 100);
    check(beep_ctl[18] == 0x100 && r->beeps == before && (port61 & 3) == 3,
          "at level 0 a tone sends 0 to the generator and sounds the PIT");
    speaker_tone(0, 0);
    hda_driver.beep_level(100);
    beep_ticks(HDA_BEEP_IDLE_TICKS);

    beep_during_playback();
    beep_threaded();
    beep_other_codecs();
}

/** Run codec and controller rows: 0 when every check passes, 1 otherwise. */
int main(void)
{
    row_rates();
    row_caps();
    row_underrun();
    row_connections();
    row_route();
    row_volume();
    row_hdmi();
    row_report();
    row_report_wrap();
    row_verbs();
    row_dma_check();
    row_periods();
    row_delay();
    row_fill();
    row_writer_stall();
    row_missed();
    row_play_gate();
    row_play_early();
    row_play_refilled();
    row_shutdown();
    row_mix_width();
    row_capture_path();
    row_capture_start();
    row_capture_routes();
    row_capture_stream();
    row_capture_duplex();
    row_capture_overrun();
    row_capture_early();
    row_capture_fails();
    row_input_controls();
    row_jack_switch();
    row_jack_reads();
    row_jack_stop();
    row_jack_capture();
    row_beep();
    printf("hda-test: result: %s\n", failures ? "fail" : "pass");
    return failures ? 1 : 0;
}

/** Ignore logging; the rows inspect commands and cached state. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}
/** The model's codec answers at once, so a delay waits for nothing; it is
 * counted, with those made with interrupts off. */
void k_udelay(uint32_t us)
{
    (void)us;
    delays++;
    if (!irqs_on)
        delays_irqs_off++;
}
/** Return the model's interrupt flag and turn it off. */
uint32_t kirq_save(void)
{
    uint32_t flags = (uint32_t)irqs_on;

    irqs_on = 0;
    return flags;
}
/** Put the model's interrupt flag back as `flags` has it. */
void kirq_restore(uint32_t flags)
{
    irqs_on = (int)flags;
}
/** The model has one processor, so a lock is free whenever it is taken. */
void spin_lock(struct spinlock *lock)
{
    check(!lock->locked, "a lock taken is free");
    lock->locked = 1;
}
/** Take `lock` when it is free: 1, or 0 when it is held. */
int spin_trylock(struct spinlock *lock)
{
    if (lock->locked)
        return 0;
    lock->locked = 1;
    return 1;
}
/** Turn interrupts off and take `lock`: the interrupt state before. */
uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    uint32_t flags = kirq_save();

    spin_lock(lock);
    return flags;
}

/** Release `lock` and put interrupts back as `flags` says. */
void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    spin_unlock(lock);
    kirq_restore(flags);
}

/** Give `lock` back. */
void spin_unlock(struct spinlock *lock)
{
    lock->locked = 0;
}
/** Model the speaker's ports: 0x61, and channel 2's count, low byte first. */
void outb(uint16_t port, uint8_t val)
{
    if (port == 0x61)
        port61 = val;
    if (port == 0x43)
        pit_bytes = 0;
    if (port == 0x42)
        pit_count = pit_bytes++ == 0 ? val : (uint16_t)(pit_count | (uint16_t)val << 8);
}
/** Read port 0x61 of the model. */
uint8_t inb(uint16_t port)
{
    return port == 0x61 ? port61 : 0;
}
/** No console is attached to the model. */
void console_putc(char c)
{
    (void)c;
}
/** No serial port is attached to the model. */
void serial_putc(char c)
{
    (void)c;
}
/** Count what is printed on the console; the text itself is not needed. */
void k_vprintf(void (*putc_fn)(char), const char *format, va_list ap)
{
    if (putc_fn == console_putc)
        console_writes++;
    (void)format;
    (void)ap;
}
/** PCI discovery is outside these codec rows. */
bool pci_find_device(uint8_t c, uint8_t s, pci_device_t *out)
{
    (void)c;
    (void)s;
    (void)out;
    return false;
}
/** No second controller exists in the model. */
bool pci_find_device_skip(uint8_t c, uint8_t s, pci_device_t *skip, pci_device_t *out)
{
    (void)c;
    (void)s;
    (void)skip;
    (void)out;
    return false;
}
/** The model has no PCI capabilities. */
uint8_t pci_find_capability(uint8_t b, uint8_t d, uint8_t f, uint8_t c)
{
    (void)b;
    (void)d;
    (void)f;
    (void)c;
    return 0;
}
/** PCI configuration is outside these rows. */
uint16_t pci_read_config16(uint8_t b, uint8_t d, uint8_t f, uint8_t o)
{
    (void)b;
    (void)d;
    (void)f;
    (void)o;
    return 0;
}
/** PCI configuration is outside these rows. */
uint8_t pci_read_config8(uint8_t b, uint8_t d, uint8_t f, uint8_t o)
{
    (void)b;
    (void)d;
    (void)f;
    (void)o;
    return 0;
}
/** PCI configuration is outside these rows. */
void pci_write_config16(uint8_t b, uint8_t d, uint8_t f, uint8_t o, uint16_t v)
{
    (void)b;
    (void)d;
    (void)f;
    (void)o;
    (void)v;
}
/** No interrupt allocation is used. */
int isr_alloc_vector(void)
{
    return -1;
}
/** No vector is given out, so none comes back. */
void isr_free_vector(int vector)
{
    (void)vector;
}
/** The model has no APIC. */
int isr_msi_ready(void)
{
    return 0;
}
/** The model has no MSI capability. */
int pci_msi_enable(const pci_device_t *dev, uint8_t vector)
{
    (void)dev;
    (void)vector;
    return -1;
}
/** Interrupt service is outside these rows. */
void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}
/** MMIO accesses use the test hooks. */
uintptr_t map_mmio(uintptr_t addr, size_t size)
{
    (void)size;
    return addr;
}
/** PCI mapping is outside these rows. */
uint64_t pci_bar_address64(const pci_device_t *dev, int i)
{
    (void)dev;
    (void)i;
    return 0;
}
/** PCI bus mastering is outside these rows. */
void pci_enable_bus_master(pci_device_t *dev)
{
    (void)dev;
}
/** PCI mapping is outside these rows. */
void pci_enable_memory_space(pci_device_t *dev)
{
    (void)dev;
}
/** PCI interrupts are outside these rows. */
void pci_enable_interrupts(pci_device_t *dev)
{
    (void)dev;
}
/** The model's buffers are in host memory, with no cache to write back. */
void arch_dma_flush(const volatile void *p, size_t len)
{
    (void)p;
    (void)len;
}

/** Nothing to write back, as for `arch_dma_flush`. */
void arch_dma_flush_all(void)
{}
/** Boot banner formatting is outside these rows. */
size_t k_itoa(int n, char *out, size_t size, int radix, bool sign)
{
    (void)n;
    (void)radix;
    (void)sign;
    if (size)
        out[0] = 0;
    return 0;
}
