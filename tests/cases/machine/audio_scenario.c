/*
 * audio_scenario.c: the scenario of machine.audio, in which the kernel's
 * debug shell plays `audio test scale` on an emulated sound card and
 * wavcheck measures the notes in what QEMU recorded the machine playing,
 * runs `audiotest -s`, which measures the queue across a writer's stall, or
 * runs `audiotest -d`, whose scales through /dev/dsp wavcheck measures.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* The console of the case's one boot. */
static const char console[] = "fixture.1.console";

/* The debug shell's prompt, after the line that ends the command before. */
static const char prompt[] = "\n> ";

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

/**
 * Run `audiotest -d` with `args` from the debug shell, and once the shell is
 * back report assertion `id`, that the program printed `granted`, and
 * `played`, that it said the scale played.
 */
static void dsp_play(struct lc *l, struct etude_case_machine *m, uint64_t *at, const char *args,
                     const char *id, const char *granted, const char *played)
{
    char line[96];
    uint64_t from = *at;

    snprintf(line, sizeof(line), "exec /bin/audiotest -d%s\r", args);
    if (!lc_type(l, m, line) || !lc_wait(l, m, prompt, at, NULL)) {
        if (!l->stopped)
            lc_assert(l, id, 0, l->why);
        return;
    }
    lc_find(l, m, id, granted, from, 1);
    lc_find(l, m, played, "audiotest: result: complete; the scale played through /dev/dsp", from,
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
    dsp_play(l, m, &at, " 48000 16 2", "stereo", "audiotest: dsp: granted 48000 hz, 16-bit stereo",
             "played-stereo");
    if (!lc_ok(l))
        return;
    dsp_play(l, m, &at, "", "defaults", "audiotest: dsp: granted 8000 hz, 8-bit mono",
             "played-defaults");
    if (!lc_ok(l) || !lc_quit(l, m, NULL))
        return;
    lc_find(l, m, "no-stall", "audio: stalled", 0, 0);
    wavcheck(l, m, argv, path, sizeof(path));
}

int etude_scenario(struct etude_case *c)
{
    static const char *const hdas[] = {"probe-before", "played", "probe-after", "messages",
                                       "no-stall",     "notes",  NULL};
    static const char *const sb16s[] = {"played", "no-stall", "started", "notes", NULL};
    static const char *const queues[] = {"bounded", "no-stall", NULL};
    static const char *const dsps[] = {"stereo",   "played-stereo", "defaults", "played-defaults",
                                       "no-stall", "notes",         NULL};
    const char *name = case_name(c);
    struct lc l;

    if (strcmp(name, "hda") == 0) {
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
    } else {
        return 1;
    }
    return lc_finish(&l);
}
