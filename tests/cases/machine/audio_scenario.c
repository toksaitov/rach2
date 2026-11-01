/*
 * audio_scenario.c: the scenario of machine.audio, in which the kernel's
 * debug shell plays `audio test scale` on an emulated sound card and
 * wavcheck measures the notes in what QEMU recorded the machine playing,
 * runs `audiotest -s`, which measures the queue across a writer's stall,
 * runs `audiotest -d`, whose scales through /dev/dsp wavcheck measures, or
 * runs `audiotest -r` or `-R`, which record through /dev/dsp, and for `-R`
 * wavcheck measures the notes QEMU fed the machine in what it read.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* The console of the case's one boot. */
static const char console[] = "fixture.1.console";

/* The debug shell's prompt, after the line that ends the command before. */
static const char prompt[] = "\n> ";

/* What `audiotest -r` and `-R` say when the frames they read match the
 * kernel's count, and the lines in which `-R` prints the samples. */
static const char counted[] =
    "audiotest: result: complete; the frames read match the kernel's count";
static const char data_line[] = "audiotest: data: ";
static const char rate_key[] = "16-bit mono at ";

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/**
 * Run intrprobe from the debug shell, report assertion `id` from its line
 * once the shell is back, and give the count it read on the audio
 * controller's message vector in `*audio`: 0 when /proc/interrupts lists no
 * such vector, which intrprobe prints as -1, and -1 when it printed none.
 *
 * @return 1 when the shell came back.
 */
static int probe(struct lc *l, struct etude_case_machine *m, uint64_t *at, const char *id,
                 long *audio)
{
    uint64_t from = *at;
    char count[32];

    *audio = -1;
    if (!lc_type(l, m, "exec /intrprobe\r") || !lc_wait(l, m, prompt, at, NULL)) {
        if (!l->stopped)
            lc_assert(l, id, 0, l->why);
        return 0;
    }
    lc_find(l, m, id, "INTRPROBE OK", from, 1);
    if (lc_text(l, console, "INTRPROBE READ vector ", from, count, sizeof(count)) &&
        lc_text(l, console, ", audio ", from, count, sizeof(count)) &&
        (*audio = strtol(count, NULL, 10)) < 0)
        *audio = 0;
    return 1;
}

/**
 * Play the scale from the debug shell, and report `played` once the
 * command has ended: it said `Done.`.
 */
static void play(struct lc *l, struct etude_case_machine *m, uint64_t *at)
{
    uint64_t from = *at;

    if (lc_type(l, m, "audio test scale 2000\r") && lc_wait(l, m, prompt, at, NULL))
        lc_find(l, m, "played", "Done.", from, 1);
    else if (!l->stopped)
        lc_assert(l, "played", 0, l->why);
}

/**
 * Run wavcheck, whose arguments `argv` have the recording's path at `path`,
 * of `size` bytes, on the recording of machine `m`, which has ended, as the
 * step `judge`, and report its verdict as `notes`.
 */
static void wavcheck(struct lc *l, struct etude_case_machine *m, char **argv, char *path,
                     size_t size)
{
    char detail[160];
    struct etude_termination end;
    const char *wav;
    enum etude_status st;

    if (!lc_step(l, "judge"))
        return;
    wav = etude_audio(m);
    if (wav == NULL || strlen(wav) >= size) {
        lc_halt(l, ETUDE_REFUSED, "the machine's recording");
        return;
    }
    snprintf(path, size, "%s", wav);
    etude_progress(l->c, "wavcheck", 0);
    st = etude_host(l->c, argv, "wavcheck", &end);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "wavcheck");
        return;
    }
    etude_progress(l->c, "wavcheck", 1);
    if (end.end == ETUDE_END_EXITED)
        snprintf(detail, sizeof(detail), "wavcheck exited %d; its evidence wavcheck says why",
                 end.value);
    else
        snprintf(detail, sizeof(detail), "wavcheck did not exit");
    lc_assert(l, "notes", end.end == ETUDE_END_EXITED && end.value == 0, detail);
}

/**
 * End the machine with QMP's quit, which makes QEMU finish the recording,
 * then report the console's silence about a stall and wavcheck's verdict
 * on the notes the debug shell's scale plays.
 */
static void judge(struct lc *l, struct etude_case_machine *m, int sb16)
{
    char name[] = "wavcheck", scale[] = "scale", c4[] = "262", d4[] = "294", e4[] = "330",
         f4[] = "349", g4[] = "392", path[1024];
    char *argv[] = {name, scale, path, c4, d4, e4, f4, g4, NULL};

    if (!lc_quit(l, m, NULL))
        return;
    lc_find(l, m, "no-stall", "audio: stalled", 0, 0);
    if (sb16)
        lc_find(l, m, "started", "AUDIO_START failed.", 0, 0);
    wavcheck(l, m, argv, path, sizeof(path));
}

/**
 * Run the HDA case: the scale between two readings of /proc/interrupts,
 * whose count on the controller's message vector must have grown.
 */
static void hda(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at = 0;
    long before = -1, after = -1;
    char detail[160];

    if (!lc_step(l, "play") || (m = lc_boot(l, "fixture")) == NULL ||
        !lc_wait(l, m, "Type 'help'", &at, NULL) || !lc_wait(l, m, prompt, &at, NULL))
        return;
    if (!probe(l, m, &at, "probe-before", &before))
        return;
    play(l, m, &at);
    if (!lc_ok(l) || !probe(l, m, &at, "probe-after", &after))
        return;
    snprintf(detail, sizeof(detail),
             "the controller's message vector counted %ld before the scale and %ld after it",
             before, after);
    lc_assert(l, "messages", before >= 0 && after > before, detail);
    judge(l, m, 0);
}

/** Run the SB16 case: the scale from the debug shell of the kernel alone. */
static void sb16(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at = 0;

    if (!lc_step(l, "play") || (m = lc_boot(l, "fixture")) == NULL ||
        !lc_wait(l, m, "Type 'help'", &at, NULL) || !lc_wait(l, m, prompt, &at, NULL))
        return;
    play(l, m, &at);
    judge(l, m, 1);
}

/**
 * Run the queue case: `audiotest -s` from the debug shell writes numbered
 * blocks for a second, stalls a second and writes for another, reading the
 * queued delay from /proc/audio after each block, and says that the delay
 * kept within six periods; then the machine ends with QMP's quit.
 */
static void queue(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at = 0, from;

    if (!lc_step(l, "play") || (m = lc_boot(l, "fixture")) == NULL ||
        !lc_wait(l, m, "Type 'help'", &at, NULL) || !lc_wait(l, m, prompt, &at, NULL))
        return;
    from = at;
    if (lc_type(l, m, "exec /bin/audiotest -s\r") && lc_wait(l, m, prompt, &at, NULL))
        lc_find(l, m, "bounded", "audiotest: result: complete; the delay kept within six periods",
                from, 1);
    else if (!l->stopped)
        lc_assert(l, "bounded", 0, l->why);
    if (!lc_quit(l, m, NULL))
        return;
    lc_find(l, m, "no-stall", "audio: stalled", 0, 0);
}

/** An audiotest invocation and the assertions its output must satisfy. */
struct dsp_playback {
    const char *args;
    const char *id;
    const char *granted;
    const char *played;
};

/** Run `audiotest -d` from the debug shell and check `p`'s granted format
 * and completed scale, advancing the console offset `at`. */
static void dsp_play(struct lc *l, struct etude_case_machine *m, uint64_t *at,
                     const struct dsp_playback *p)
{
    char line[96];
    uint64_t from = *at;

    snprintf(line, sizeof(line), "exec /bin/audiotest -d%s\r", p->args);
    if (!lc_type(l, m, line) || !lc_wait(l, m, prompt, at, NULL)) {
        if (!l->stopped)
            lc_assert(l, p->id, 0, l->why);
        return;
    }
    lc_find(l, m, p->id, p->granted, from, 1);
    lc_find(l, m, p->played, "audiotest: result: complete; the scale played through /dev/dsp", from,
            1);
}

/**
 * Run the /dev/dsp case: `audiotest -d` plays its stereo scale as signed
 * 16-bit at 48000 Hz, then its mono scale at OSS's defaults, unsigned 8-bit
 * at 8000 Hz, which the driver converts; wavcheck then requires the left
 * scale and then the mono one in the first channel, and the right scale and
 * then the mono one in the second.
 */
static void dsp(struct lc *l)
{
    static const struct dsp_playback stereo = {" 48000 16 2", "stereo",
                                               "audiotest: dsp: granted 48000 hz, 16-bit stereo",
                                               "played-stereo"};
    static const struct dsp_playback defaults = {
        "", "defaults", "audiotest: dsp: granted 8000 hz, 8-bit mono", "played-defaults"};
    char name[] = "wavcheck", scale[] = "scale", slash[] = "/", path[1024];
    char l1[] = "262", l2[] = "294", l3[] = "330", l4[] = "349", l5[] = "392";
    char r1[] = "131", r2[] = "147", r3[] = "165", r4[] = "175", r5[] = "196";
    char m1[] = "80", m2[] = "100", m3[] = "111", m4[] = "222", m5[] = "444";
    char *argv[] = {name,  scale, path, l1, l2, l3, l4, l5, m1, m2, m3, m4,  m5,
                    slash, r1,    r2,   r3, r4, r5, m1, m2, m3, m4, m5, NULL};
    struct etude_case_machine *m;
    uint64_t at = 0;

    if (!lc_step(l, "play") || (m = lc_boot(l, "fixture")) == NULL ||
        !lc_wait(l, m, "Type 'help'", &at, NULL) || !lc_wait(l, m, prompt, &at, NULL))
        return;
    dsp_play(l, m, &at, &stereo);
    if (!lc_ok(l))
        return;
    dsp_play(l, m, &at, &defaults);
    if (!lc_ok(l) || !lc_quit(l, m, NULL))
        return;
    lc_find(l, m, "no-stall", "audio: stalled", 0, 0);
    wavcheck(l, m, argv, path, sizeof(path));
}

/**
 * Run `command`, an `audiotest -r` or `-R`, from the debug shell of the
 * case's one machine, booted, and report `counted` once the shell is back.
 *
 * @return the machine, or NULL when the scenario cannot go on; the console's
 *         offsets before the command and after the prompt that ends it are
 *         in `*from` and `*at`.
 */
static struct etude_case_machine *run_record(struct lc *l, const char *command, uint64_t *from,
                                             uint64_t *at)
{
    struct etude_case_machine *m;

    *at = 0;
    if (!lc_step(l, "record") || (m = lc_boot(l, "fixture")) == NULL ||
        !lc_wait(l, m, "Type 'help'", at, NULL) || !lc_wait(l, m, prompt, at, NULL))
        return NULL;
    *from = *at;
    if (lc_type(l, m, command) && lc_wait(l, m, prompt, at, NULL)) {
        lc_find(l, m, "counted", counted, *from, 1);
        return m;
    }
    if (!l->stopped)
        lc_assert(l, "counted", 0, l->why);
    return NULL;
}

/**
 * Run the record case: `audiotest -r` reads a second of 16-bit mono at
 * 48000 Hz through /dev/dsp on HDA, under QEMU's `none` backend, which
 * records silence, and says that the frames it read and those left in the
 * buffer are the frames /proc/audio says the input stream handed over.
 */
static void record(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t from, at;

    m = run_record(l, "exec /bin/audiotest -r 48000 48\r", &from, &at);
    if (m == NULL || !lc_quit(l, m, NULL))
        return;
    lc_find(l, m, "no-stall", "audio: stalled", 0, 0);
}

/** The value of hexadecimal digit `c`, or -1. */
static int hex_digit(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

/** Store the 32-bit `v` at `p`, little-endian. */
static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/**
 * Rebuild the samples `audiotest -R` printed in `text` as a WAVE file of
 * one 16-bit channel at the rate it said it recorded at, at `path`. Each
 * line that begins with `data_line` holds bytes in hexadecimal; a kernel
 * record's prefix before it is skipped.
 *
 * @return the bytes of samples written, or -1 having put why in `why`.
 */
static long rebuild(const char *text, const char *path, char *why, size_t size)
{
    static const unsigned char fmt[] = {'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0};
    unsigned char header[44];
    const char *at = strstr(text, rate_key), *line;
    unsigned long rate, bytes = 0;
    size_t len = strlen(text);
    FILE *f;

    if (at == NULL || (rate = strtoul(at + strlen(rate_key), NULL, 10)) == 0) {
        snprintf(why, size, "audiotest printed no rate");
        return -1;
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        snprintf(why, size, "the recording's evidence cannot be written");
        return -1;
    }
    memset(header, 0, sizeof(header));
    fwrite(header, 1, sizeof(header), f);
    for (line = text; line < text + len;) {
        const char *end = strchr(line, '\n'), *p;
        size_t start = lc_past_prefix(text, len, (size_t)(line - text));

        if (end == NULL)
            end = text + len;
        p = text + start;
        {
            const char *prefix =
                strncmp(p, "audioclients: data: ", strlen("audioclients: data: ")) == 0
                    ? "audioclients: data: "
                    : data_line;

            if ((size_t)(end - p) > strlen(prefix) && strncmp(p, prefix, strlen(prefix)) == 0)
                for (p += strlen(prefix);
                     p + 1 < end && hex_digit(p[0]) >= 0 && hex_digit(p[1]) >= 0; p += 2) {
                    fputc(hex_digit(p[0]) * 16 + hex_digit(p[1]), f);
                    bytes++;
                }
        }
        line = end + 1;
    }
    memcpy(header, "RIFF", 4);
    put32(header + 4, 36 + bytes);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, fmt, sizeof(fmt));
    put32(header + 24, rate);
    put32(header + 28, rate * 2);
    header[32] = 2;
    header[34] = 16;
    memcpy(header + 36, "data", 4);
    put32(header + 40, bytes);
    if (fseek(f, 0, SEEK_SET) != 0 || fwrite(header, 1, sizeof(header), f) != sizeof(header) ||
        fclose(f) != 0) {
        snprintf(why, size, "the recording's evidence cannot be written");
        return -1;
    }
    return (long)bytes;
}

/**
 * Run the tone case: under QEMU's `sdl` backend, whose disk driver feeds the
 * codec's input three notes from the case's input file, `audiotest -R`
 * records two seconds through /dev/dsp and prints the samples; the
 * scenario rebuilds them as a WAVE file and wavcheck must find the three
 * notes in it, in order.
 */
static void tone(struct lc *l)
{
    char name[] = "wavcheck", scale[] = "scale", n1[] = "250", n2[] = "400", n3[] = "500",
         path[1024], detail[200];
    char *argv[] = {name, scale, path, n1, n2, n3, NULL};
    struct etude_case_machine *m;
    struct etude_termination end;
    enum etude_status st;
    const char *wav;
    uint64_t from, at;
    char *text;
    long bytes;

    m = run_record(l, "exec /bin/audiotest -R 16000 32\r", &from, &at);
    if (m == NULL)
        return;
    text = lc_console(l, "fixture", from, at);
    if (text == NULL || !lc_quit(l, m, NULL)) {
        free(text);
        return;
    }
    lc_find(l, m, "no-stall", "audio: stalled", 0, 0);
    if (!lc_step(l, "judge")) {
        free(text);
        return;
    }
    wav = etude_evidence(l->c, "recorded.wav");
    if (wav == NULL || strlen(wav) >= sizeof(path)) {
        free(text);
        lc_halt(l, ETUDE_REFUSED, "the recording's evidence");
        return;
    }
    snprintf(path, sizeof(path), "%s", wav);
    bytes = rebuild(text, path, detail, sizeof(detail));
    free(text);
    if (bytes < 0) {
        lc_assert(l, "notes", 0, detail);
        return;
    }
    etude_progress(l->c, "wavcheck", 0);
    st = etude_host(l->c, argv, "wavcheck", &end);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "wavcheck");
        return;
    }
    etude_progress(l->c, "wavcheck", 1);
    if (end.end == ETUDE_END_EXITED)
        snprintf(detail, sizeof(detail),
                 "wavcheck exited %d on the %ld bytes read; its evidence wavcheck says why",
                 end.value, bytes);
    else
        snprintf(detail, sizeof(detail), "wavcheck did not exit");
    lc_assert(l, "notes", end.end == ETUDE_END_EXITED && end.value == 0, detail);
}

/** Run the concurrent players or loopback client, then independently judge
 * QEMU's recording or the recording read through the guest's DSP. */
static void clients(struct lc *l, const char *mode)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    enum etude_status st;
    uint64_t at = 0, from;
    char command[96], path[1024], detail[200];
    char tool[] = "wavcheck", scale[] = "scale", mixed[] = "mix";
    char n1[] = "262", n2[] = "294", n3[] = "330", n4[] = "349", n5[] = "392";
    char f1[] = "440", f2[] = "660";
    char *scale_argv[] = {tool, scale, path, n1, n2, n3, n4, n5, NULL};
    char *mix_argv[] = {tool, mixed, path, f1, f2, NULL};
    char *text;
    const char *evidence;
    long bytes;

    if (!lc_step(l, "play") || (m = lc_boot(l, "fixture")) == NULL ||
        !lc_wait(l, m, "Type 'help'", &at, NULL) || !lc_wait(l, m, prompt, &at, NULL))
        return;
    from = at;
    snprintf(command, sizeof(command), "exec /musl/audioclientsmusl %s\r", mode);
    if (!lc_type(l, m, command) || !lc_wait(l, m, prompt, &at, NULL)) {
        if (!l->stopped)
            lc_assert(l, "clients", 0, l->why);
        return;
    }
    lc_find(l, m, "clients", "audioclients: result: complete", from, 1);
    text = strcmp(mode, "mix") == 0 ? NULL : lc_console(l, "fixture", from, at);
    if (!lc_quit(l, m, NULL)) {
        free(text);
        return;
    }
    lc_find(l, m, "no-stall", "audio: stalled", 0, 0);
    if (strcmp(mode, "mix") == 0) {
        wavcheck(l, m, mix_argv, path, sizeof(path));
        return;
    }
    if (!lc_step(l, "judge")) {
        free(text);
        return;
    }
    evidence = etude_evidence(l->c, "loopback.wav");
    if (!text || !evidence || strlen(evidence) >= sizeof(path)) {
        free(text);
        lc_halt(l, ETUDE_REFUSED, "loopback recording");
        return;
    }
    snprintf(path, sizeof(path), "%s", evidence);
    bytes = rebuild(text, path, detail, sizeof(detail));
    free(text);
    if (bytes < 0) {
        lc_assert(l, "notes", 0, detail);
        return;
    }
    etude_progress(l->c, "wavcheck", 0);
    st = etude_host(l->c, scale_argv, "wavcheck", &end);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "wavcheck");
        return;
    }
    etude_progress(l->c, "wavcheck", 1);
    snprintf(detail, sizeof(detail), "wavcheck over %ld bytes of the captured mix", bytes);
    lc_assert(l, "notes", end.end == ETUDE_END_EXITED && end.value == 0, detail);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const hdas[] = {"probe-before", "played", "probe-after", "messages",
                                       "no-stall",     "notes",  NULL};
    static const char *const sb16s[] = {"played", "no-stall", "started", "notes", NULL};
    static const char *const queues[] = {"bounded", "no-stall", NULL};
    static const char *const dsps[] = {"stereo",   "played-stereo", "defaults", "played-defaults",
                                       "no-stall", "notes",         NULL};
    static const char *const records[] = {"counted", "no-stall", NULL};
    static const char *const client_assertions[] = {"clients", "no-stall", "notes", NULL};
    static const char *const tones[] = {"counted", "no-stall", "notes", NULL};
    const char *name = case_name(c);
    struct lc l;

    if (strcmp(name, "mix") == 0 || strcmp(name, "loopback") == 0 || strcmp(name, "sox") == 0) {
        lc_begin(&l, c, client_assertions);
        clients(&l, name);
    } else if (strcmp(name, "hda") == 0) {
        lc_begin(&l, c, hdas);
        hda(&l);
    } else if (strcmp(name, "sb16") == 0) {
        lc_begin(&l, c, sb16s);
        sb16(&l);
    } else if (strcmp(name, "queue") == 0) {
        lc_begin(&l, c, queues);
        queue(&l);
    } else if (strcmp(name, "dsp") == 0) {
        lc_begin(&l, c, dsps);
        dsp(&l);
    } else if (strcmp(name, "record") == 0) {
        lc_begin(&l, c, records);
        record(&l);
    } else if (strcmp(name, "tone") == 0) {
        lc_begin(&l, c, tones);
        tone(&l);
    } else {
        return 1;
    }
    return lc_finish(&l);
}
