/*
 * audio_scenario.c: the scenario of machine.audio, in which the kernel's
 * debug shell plays `audio test scale` on an emulated sound card and
 * wavcheck measures the notes in what QEMU recorded the machine playing.
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
 * End the machine with QMP's quit, which makes QEMU finish the recording,
 * then report the console's silence about a stall and wavcheck's verdict
 * on the notes.
 */
static void judge(struct lc *l, struct etude_case_machine *m, int sb16)
{
    char name[] = "wavcheck", scale[] = "scale", c4[] = "262", d4[] = "294", e4[] = "330",
         f4[] = "349", g4[] = "392", path[1024], detail[160];
    char *argv[] = {name, scale, path, c4, d4, e4, f4, g4, NULL};
    struct etude_termination end;
    const char *wav;
    enum etude_status st;

    if (!lc_quit(l, m, NULL))
        return;
    lc_find(l, m, "no-stall", "audio: stalled", 0, 0);
    if (sb16)
        lc_find(l, m, "started", "AUDIO_START failed.", 0, 0);
    if (!lc_step(l, "judge"))
        return;
    wav = etude_audio(m);
    if (wav == NULL || strlen(wav) >= sizeof(path)) {
        lc_halt(l, ETUDE_REFUSED, "the machine's recording");
        return;
    }
    snprintf(path, sizeof(path), "%s", wav);
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

int etude_scenario(struct etude_case *c)
{
    static const char *const hdas[] = {"probe-before", "played", "probe-after", "messages",
                                       "no-stall",     "notes",  NULL};
    static const char *const sb16s[] = {"played", "no-stall", "started", "notes", NULL};
    const char *name = case_name(c);
    struct lc l;

    if (strcmp(name, "hda") == 0) {
        lc_begin(&l, c, hdas);
        hda(&l);
    } else if (strcmp(name, "sb16") == 0) {
        lc_begin(&l, c, sb16s);
        sb16(&l);
    } else {
        return 1;
    }
    return lc_finish(&l);
}
