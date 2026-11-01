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

/**
 * Write /proc/audio's text into `buf`, of `cap` bytes, with no terminator:
 * the audio driver's name and the HDA driver's report (`hda_get_report`),
 * the codec and the widgets it chose, the rates and formats of the last
 * stream it programmed, the converters' capabilities and the connection
 * lists it read. It sends no verb.
 *
 * Return the text's whole length; when that is more than `cap`, only the
 * first `cap` bytes were stored, and the caller asks again with more room.
 */
int audio_proc_text(char *buf, int cap)
{
    const hda_report_t *r = hda_get_report();
    struct audio_text text, *t = &text;
    int i, j;

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
    put_field(t, "\nstream requested ", r->requested_rate);
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
    return t->len;
}
