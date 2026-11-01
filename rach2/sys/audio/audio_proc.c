/*
 * audio_proc.c: the text of /proc/audio, made from the audio driver's name
 * and the HDA driver's report, apart from procfs.c so that the HDA host test
 * needs nothing else of procfs.
 */

#include "audio_proc.h"

#include "audio.h"
#include "hda.h"

/* A text written into a caller's buffer: what fits is stored, and `len`
 * counts every byte, so a caller learns the size a short buffer lacked. */
struct audio_text {
    char *buf;
    int cap;
    int len;
};

/** Append the byte `c` to `t`. */
static void put_ch(struct audio_text *t, char c)
{
    if (t->len < t->cap)
        t->buf[t->len] = c;
    t->len++;
}

/** Append the string `s` to `t`. */
static void put_str(struct audio_text *t, const char *s)
{
    while (*s)
        put_ch(t, *s++);
}

/** Append `v` in decimal to `t`. */
static void put_u32(struct audio_text *t, uint32_t v)
{
    char d[10];
    int n = 0;
    do {
        d[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        put_ch(t, d[--n]);
}

/** Append the low `width` hexadecimal digits of `v`, in capitals, to `t`. */
static void put_hex(struct audio_text *t, uint32_t v, int width)
{
    while (width--)
        put_ch(t, "0123456789ABCDEF"[(v >> (width * 4)) & 15]);
}

/** Append `name` and `value` in decimal, one field of /proc/audio. */
static void put_field(struct audio_text *t, const char *name, uint32_t value)
{
    put_str(t, name);
    put_u32(t, value);
}

/** Append ` name 0x` and `value` in eight hexadecimal digits to `t`. */
static void put_word(struct audio_text *t, const char *name, uint32_t value)
{
    put_ch(t, ' ');
    put_str(t, name);
    put_str(t, " 0x");
    put_hex(t, value, 8);
}

/** The short name of widget type `type` (HDA_WIDGET_*). */
static const char *widget_type(uint8_t type)
{
    static const char *const names[] = {"out", "in",    "mix",    "sel",
                                        "pin", "power", "volume", "beep"};

    if (type < sizeof(names) / sizeof(names[0]))
        return names[type];
    return type == 0xF ? "vendor" : "other";
}

/** Append a line for each jack the driver watches to `t`: its pin, 0 for
 * none, its state, `in`, `out` or `unknown`, and its readings, the failed
 * ones and its changes. */
static void put_jacks(struct audio_text *t, const hda_report_t *r)
{
    static const char *const names[HDA_JACKS] = {"headphone", "mic"};
    int j;

    for (j = 0; j < HDA_JACKS; j++) {
        const hda_jack_report_t *k = &r->jacks[j];

        put_str(t, "jack ");
        put_str(t, names[j]);
        put_field(t, " pin ", k->pin);
        put_str(t, !k->pin                    ? " state unknown"
                   : k->state == HDA_JACK_IN  ? " state in"
                   : k->state == HDA_JACK_OUT ? " state out"
                                              : " state unknown");
        put_field(t, " polls ", k->polls);
        put_field(t, " failed ", k->failed);
        put_field(t, " changes ", k->changes);
        put_ch(t, '\n');
    }
}

/** Append the stream's queue, the GPIO count, a line for each widget of the
 * function group and the list of its ADCs to `t`. */
static void put_codec(struct audio_text *t, const hda_report_t *r)
{
    const hda_queue_report_t *q = &r->queue;
    int i, j;

    put_field(t, "queue fill ", q->fill);
    put_field(t, " high ", q->high);
    put_field(t, " delay-ms ", q->delay_ms);
    put_field(t, " written ", q->written);
    put_field(t, " dropped ", q->dropped);
    put_field(t, " periods ", q->periods);
    put_field(t, " underruns ", q->underruns);
    put_field(t, " missed ", q->missed);
    put_field(t, " lpib ", q->lpib);
    put_field(t, "\ngpio count ", r->gpio & 0xFF);
    put_word(t, "caps", r->gpio);
    put_ch(t, '\n');
    for (i = 0; i < r->widget_count; i++) {
        const hda_widget_report_t *w = &r->widgets[i];

        put_field(t, "widget ", w->nid);
        put_str(t, " type ");
        put_str(t, widget_type(w->type));
        put_word(t, "caps", w->caps);
        put_word(t, "amp-in", w->amp_in);
        put_word(t, "amp-out", w->amp_out);
        if (w->type == HDA_WIDGET_PIN) {
            put_word(t, "pin-caps", w->pin_caps);
            put_word(t, "config", w->config);
            if (w->sensed)
                put_word(t, "sense", w->sense);
        }
        put_str(t, w->conn_count < 0 ? " conn invalid" : " conn");
        for (j = 0; j < w->conn_count; j++)
            put_field(t, " ", w->conns[j]);
        put_ch(t, '\n');
    }
    put_str(t, "adcs");
    for (i = 0; i < r->widget_count; i++)
        if (r->widgets[i].type == HDA_WIDGET_AUDIO_IN)
            put_field(t, " ", r->widgets[i].nid);
    put_ch(t, '\n');
}

/** Append the input path, the ADC's capabilities and the input stream's
 * last setup and counters to `t`. */
static void put_input(struct audio_text *t, const hda_input_report_t *in)
{
    put_field(t, "input adc ", in->adc);
    put_field(t, " index ", in->index);
    put_field(t, " mid ", in->mid);
    put_field(t, " index ", in->mid_index);
    put_field(t, " pin ", in->pin);
    put_field(t, " int-mic ", in->int_mic);
    put_field(t, " jack-mic ", in->jack_mic);
    put_field(t, " inv-dmic ", in->inv_dmic);
    put_field(t, " level ", in->level);
    put_str(t, " pin-ctl 0x");
    put_hex(t, in->pin_ctl, 2);
    put_ch(t, '\n');
    if (in->caps.nid) {
        put_field(t, "caps adc ", in->caps.nid);
        put_field(t, " source ", in->caps.source);
        put_str(t, " pcm 0x");
        put_hex(t, in->caps.pcm, 8);
        put_str(t, " formats 0x");
        put_hex(t, in->caps.formats, 8);
        put_ch(t, '\n');
    }
    put_field(t, "capture running ", in->running);
    put_field(t, " requested ", in->requested_rate);
    put_field(t, " rate ", in->rate);
    put_str(t, " format 0x");
    put_hex(t, in->format, 4);
    put_field(t, " period ", in->period);
    put_field(t, " periods ", in->periods);
    put_field(t, " frames ", in->frames);
    put_field(t, " overruns ", in->overruns);
    put_field(t, " lpib ", in->lpib);
    put_ch(t, '\n');
}

/**
 * Write /proc/audio's text into `buf`, of `cap` bytes, with no terminator:
 * the audio driver's name and the HDA driver's report (`hda_get_report`),
 * the codec and the widgets it chose, the output routing mode, the jacks
 * and their states, the rates and formats of the last stream it programmed, the converters'
 * capabilities and the connection lists it read; the input path, the ADC's capabilities and the
 * input stream's counters; then the output stream's queue, and every widget of the codec's function
 * group with its ADCs. It sends no verb.
 *
 * With `buf` NULL it first reads the queue again (`hda_read_queue`); with a
 * buffer it writes the reading the last call without one took. procfs asks
 * for the length with NULL and then writes the text, so the two calls agree
 * however the counters move between them.
 *
 * Return the text's whole length; when that is more than `cap`, only the
 * first `cap` bytes were stored, and the caller asks again with more room.
 */
int audio_proc_text(char *buf, int cap)
{
    static const char *const modes[] = {"auto", "primary", "hp", "both"};
    const hda_report_t *r;
    struct audio_text text, *t = &text;
    int i, j;

    if (!buf)
        hda_read_queue();
    r = hda_get_report();
    t->buf = buf;
    t->cap = cap;
    t->len = 0;
    put_str(t, "driver ");
    put_str(t, audio_get_driver_name());
    put_field(t, "\nhda ready ", r->ready);
    put_field(t, " playing ", r->playing);
    put_field(t, "\ncodec ", r->codec);
    put_str(t, " vendor 0x");
    put_hex(t, r->vendor, 8);
    put_field(t, " afg ", r->afg);
    put_field(t, "\nprimary pin ", r->pin);
    put_field(t, " index ", r->pin_index);
    put_field(t, " mixer ", r->mixer);
    put_field(t, " index ", r->mixer_index);
    put_field(t, " dac ", r->dac);
    put_field(t, " enabled ", r->primary_enabled);
    put_field(t, "\nheadphone pin ", r->hp_pin);
    put_field(t, " index ", r->hp_index);
    put_field(t, " dac ", r->hp_dac);
    put_field(t, " enabled ", r->hp_enabled);
    put_str(t, " mode ");
    put_str(t, modes[hda_get_output_mode() & 3]);
    put_ch(t, '\n');
    put_jacks(t, r);
    put_field(t, "stream requested ", r->requested_rate);
    put_field(t, " programmed ", r->programmed_rate);
    put_str(t, " format 0x");
    put_hex(t, r->stream_format, 4);
    put_str(t, " dac-format 0x");
    put_hex(t, r->dac_format, 4);
    put_str(t, " hp-format 0x");
    put_hex(t, r->hp_format, 4);
    put_ch(t, '\n');
    for (i = 0; i < 2; i++) {
        const hda_caps_report_t *c = &r->caps[i];
        if (!c->nid)
            continue;
        put_field(t, "caps dac ", c->nid);
        put_field(t, " source ", c->source);
        put_str(t, " pcm 0x");
        put_hex(t, c->pcm, 8);
        put_str(t, " formats 0x");
        put_hex(t, c->formats, 8);
        put_ch(t, '\n');
    }
    put_input(t, &r->input);
    for (i = 0; i < 3; i++) {
        const hda_connection_report_t *c = &r->connections[i];
        if (!c->nid)
            continue;
        put_field(t, "connections nid ", c->nid);
        put_field(t, " long ", c->long_form);
        put_str(t, c->count < 0 ? " invalid raw" : " raw");
        for (j = 0; j < c->raw_count; j++) {
            put_str(t, " 0x");
            put_hex(t, c->raw[j], c->long_form ? 4 : 2);
        }
        put_str(t, "\nexpanded");
        for (j = 0; j < c->count; j++)
            put_field(t, " ", c->nodes[j]);
        put_ch(t, '\n');
    }
    put_codec(t, r);
    return t->len;
}
