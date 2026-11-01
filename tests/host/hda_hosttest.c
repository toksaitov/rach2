/**
 * HDA enumeration and stream programming against a model codec and MMIO,
 * and the driver's report as /proc/audio's text (audio_proc.c) gives it.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 */
#include "audio/hda.c"
#include "audio/audio_proc.c"

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
static unsigned conn_reads, verbs, bad_offsets, sense_triggers;
static int codec_silent;  /* the codec takes commands and never answers */
static int clock_stopped; /* the wall clock does not advance */
/* The interrupt flag kirq_save and kirq_restore keep; the delays made and
 * those made with interrupts off; the reads of ICS with interrupts on. */
static int irqs_on = 1;
static unsigned delays, delays_irqs_off, ics_reads_irqs_on;
static int refuse_alloc; /* kmalloc fails */
extern void *malloc(unsigned long size);
extern void free(void *ptr);
static uint32_t clock_value; /* the wall clock, 24000 ticks on at each read */
static unsigned clock_reads, console_writes;
static unsigned mmio_reads, mmio_writes;

/** Report the row and assertion that failed. */
static void check(int ok, const char *message)
{
    if (!ok) {
        printf("hda-test: fail: %s: %s\n", row, message);
        failures++;
    }
}

/** Answer verbs from an independently packed codec connection list. */
static uint32_t model_verb(uint32_t command)
{
    unsigned nid = (command >> 20) & 0x7F;
    unsigned verb = command & 0xFFFFF;
    unsigned offset = verb & 255;
    uint32_t result = 0;
    unsigned i, width, count;

    verbs++;
    check(!(command & (1u << 27)), "no indirect or truncated node verb");
    if ((verb & 0xFFF00) == HDA_VERB_GET_PARAM)
        return params[nid][offset & 31];
    if ((verb & 0xFFF00) == HDA_VERB_GET_CONN_LIST) {
        width = params[nid][HDA_PARAM_CONN_LIST] & 128 ? 16 : 8;
        count = 32 / width;
        conn_reads++;
        if (offset % count != 0)
            bad_offsets++;
        for (i = 0; i < count && offset + i < (params[nid][HDA_PARAM_CONN_LIST] & 127); i++)
            result |= (uint32_t)connections[nid][offset + i] << (width * i);
        return result;
    }
    if (verb == HDA_VERB_GET_CONFIG_DEF)
        return configurations[nid];
    if (verb == HDA_VERB_GET_CONN_SEL)
        return selected[nid];
    if (verb == HDA_VERB_GET_PIN_SENSE)
        return sense[nid];
    if (verb == HDA_VERB_SET_PIN_SENSE)
        sense_triggers++;
    if ((verb & 0xFFF00) == HDA_VERB_SET_CONN_SEL)
        selected[nid] = offset;
    if ((verb & 0xF0000) == HDA_VERB_SET_CONV_FMT)
        converter_format[nid] = (uint16_t)verb;
    if ((verb & 0xF0000) == HDA_VERB_SET_AMP_GAIN && (verb & 0x8000))
        out_amp[nid] = (uint16_t)verb;
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
        return clock_stopped ? clock_value : (clock_value += 24000);
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
        if ((value & HDA_ICS_ICB) && codec_silent) {
            model_verb(hda_test_read(HDA_ICW, 32));
            mmio[HDA_ICS] |= HDA_ICS_ICB;
        } else if (value & HDA_ICS_ICB) {
            uint32_t reply = model_verb(hda_test_read(HDA_ICW, 32));
            hda_test_write(HDA_IRR, reply, 32);
            mmio[HDA_ICS] = HDA_ICS_IRV;
        }
        return;
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
    conn_reads = verbs = bad_offsets = sense_triggers = 0;
    codec_silent = clock_stopped = 0;
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

/**
 * Empty periods retain fractional phase without consuming later writes.
 * The 16.16 step for 44100 Hz input and 32000 Hz output is 90316.
 */
static void row_underrun(void)
{
    int16_t output[8192];
    uint8_t input[4096];
    uint32_t phase = 0;
    unsigned i, pass;

    row = "underrun";
    reset_model();
    ring_read_ptr = ring_write_ptr = 0;
    params[1][0x0A] = 0x20000 | (1u << 4) | (1u << 6);
    check(hda_find_output(), "enumeration succeeds");
    check(hda_start(44100, AUDIO_S16LE, 2) == 0 && current_hw_sample_rate == 32000,
          "44100 input selects 32000 output");
    for (i = 0; i < sizeof(input) / 4; i++) {
        unsigned sample = 1000 + i;

        input[4 * i] = input[4 * i + 2] = (uint8_t)sample;
        input[4 * i + 1] = input[4 * i + 3] = (uint8_t)(sample >> 8);
    }
    for (pass = 0; pass < 2; pass++) {
        ring_render_to_hw_buffer(output, 4096);
        phase = (phase + 4096u * 90316u) & 65535u;
        check(ring_available() == 0, "period exhausts input");
        if (pass == 0) {
            for (i = 0; i < 8192; i++)
                check(output[i] == 0, "empty period renders silence");
        }
        check(output[8190] == 0 && output[8191] == 0, "empty tail renders silence");
        check(hda_write(input, sizeof(input)) == (int)sizeof(input), "refill accepted");
        ring_render_to_hw_buffer(output, 16);
        for (i = 0; i < 16; i++) {
            int16_t sample = (int16_t)(1000 + (phase + i * 90316u) / 65536u);

            check(output[2 * i] == sample && output[2 * i + 1] == sample,
                  "refill samples preserve sample-hold order");
        }
        check(ring_available() == sizeof(input) - ((phase + 16u * 90316u) / 65536u) * 4,
              "silence does not consume refill frames");
        phase = (phase + 16u * 90316u) & 65535u;
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
    /* 800 frames in the ring at 32 kHz are 25 ms; 28672 bytes of DMA ahead
     * of position 4096, 7168 frames, are 224 ms. */
    check(strstr(text, "queue fill 3200 high 3200 delay-ms 249 written 3200 dropped 0 periods 0 "
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

/** Only the first start after setup waits for the DMA check; none prints. */
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
    check(clock_reads - reads <= 1, "second start does not wait");
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
    hda_tick();
    check(mmio_reads == reads && mmio_writes == writes, "tick after shutdown does no MMIO");
    hda_shutdown();
    check(mmio_reads == reads && mmio_writes == writes, "repeated shutdown does no MMIO");
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
    row_verbs();
    row_dma_check();
    row_shutdown();
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
/** Give `lock` back. */
void spin_unlock(struct spinlock *lock)
{
    lock->locked = 0;
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
