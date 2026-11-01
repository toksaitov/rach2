/*
 * doom_scenario.c: the scenario of machine.doom, in which a root shell on a
 * disk root runs the Doom port: without its WAD, in the background until it
 * has drawn its first frame, playing a demo to its end, and recording a
 * demo until its buffer fills; and of machine.doom.sound-hda and -sb16, in
 * which the game plays a demo with its sound on /dev/dsp. The game's own
 * report of its frames, its sound queue and the demo's end are the witnesses
 * that it drew, went on drawing and kept its sound near the present.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* The console of the case's one boot. */
static const char console[] = "fixture.1.console";

/* The root shell's prompt, which init-shell starts on the console. */
static const char prompt[] = "# ";

/*
 * The lines typed at the shell. Each ends by echoing a marker written with
 * an empty pair of quotes inside it, so that the console's echo of the line
 * does not have the marker, nor the keys the scenario reads with lc_text.
 */
static const char wad_line[] = "test -f /demo/doom1.wad && echo W\"\"AD=yes; echo BOOT\"\"ED\r";
static const char nowad_line[] =
    "mv /demo/doom1.wad /demo/doom1.off && { doom > /tmp/nowad.txt 2>&1;"
    " echo NO\"\"WAD=$?; mv /demo/doom1.off /demo/doom1.wad; };"
    " echo NOT\"\"FOUND=$(grep -c 'doom1.wad. not found' /tmp/nowad.txt);"
    " echo NOWAD\"\"DONE\r";
static const char start_line[] = "cd /root && (doom 2>&1 < /dev/null | tee /tmp/game.txt &)\r";
static const char look_line[] =
    "echo RUN\"\"NING=$(ps | grep -c '[d]oom');"
    " echo F\"\"B=$(grep -c 'framebuffer: x_res' /tmp/game.txt),"
    "$(grep -c 'Failed to open framebuffer' /tmp/game.txt);"
    " echo ZO\"\"NE=$(sed -n 's/.*, \\([0-9a-f]*\\) allocated.*/\\1/p' /tmp/game.txt);"
    " killall doom; while ps | grep -q '[d]oom'; do :; done; echo DRAW\"\"DONE\r";
static const char demo_line[] =
    "cd /tmp && doom -playdemo demo1 > /tmp/demo.txt 2>&1; echo DEMO\"\"EXIT=$?;"
    " echo DEMO\"\"FRAMES=$(sed -n 's/^doom: \\([0-9]*\\) frames.*/\\1/p' /tmp/demo.txt"
    " | tail -n 1); echo DEMO\"\"DONE\r";
static const char record_line[] =
    "rm -f /tmp/rec.lmp; doom -record /tmp/rec -maxdemo 1 > /tmp/rec.txt 2>&1;"
    " echo REC\"\"MODE=$(stat -c %a /tmp/rec.lmp 2>/dev/null || echo none);"
    " echo REC\"\"DONE\r";

/* The demo with sound: the queue of /dev/dsp in the game's last report, as
 * the samples after the last update, the most after any, and the period. */
static const char sound_line[] =
    "cd /tmp && doom -playdemo demo1 > /tmp/sound.txt 2>&1; echo DEMO\"\"EXIT=$?;"
    " echo Q\"\"UEUE=$(sed -n 's/.*; dsp queue \\([0-9]*\\) samples, the most"
    " \\([0-9]*\\), period \\([0-9]*\\)$/\\1,\\2,\\3/p' /tmp/sound.txt | tail -n 1);"
    " echo SOUND\"\"DONE\r";

/* The least zone the game may take from a machine of 128 MiB, in bytes. */
#define LEAST_ZONE (10L * 1024 * 1024)

/* The most periods the queue of /dev/dsp may hold after an update: the
 * driver's own, at most four on HDA and on the SB16, and the two the game
 * keeps in the device's buffer. */
#define MOST_PERIODS 6

/**
 * Type `line` at the shell and wait for its marker `mark`; `*from` is set
 * to where the line's output begins.
 *
 * @return 1 when the marker came.
 */
static int run(struct lc *l, struct etude_case_machine *m, uint64_t *at, uint64_t *from,
               const char *line, const char *mark)
{
    *from = *at;
    return lc_type(l, m, line) && lc_wait(l, m, mark, at, NULL);
}

/** Fail the assertions `ids`, ended by NULL, with the reason the scenario
 * stopped, unless the executor stopped it. */
static void missed(struct lc *l, const char *const ids[])
{
    size_t i;

    for (i = 0; ids[i] != NULL && !l->stopped; i++)
        lc_assert(l, ids[i], 0, l->why);
}

/**
 * Report assertion `id` from the value the console gives after `key`, past
 * `from`: it holds when the value is `want`.
 */
static void expect(struct lc *l, const char *id, const char *key, uint64_t from, const char *want,
                   const char *what)
{
    char value[64], detail[200];

    if (!lc_text(l, console, key, from, value, sizeof(value)))
        snprintf(value, sizeof(value), "nothing");
    snprintf(detail, sizeof(detail), "%s %s; %s was required", what, value, want);
    lc_assert(l, id, strcmp(value, want) == 0, detail);
}

/**
 * Give the number in `base` that the console gives after `key`, past
 * `from`, or -1 when there is none.
 */
static long number(struct lc *l, const char *key, uint64_t from, int base)
{
    char value[64], *end;
    long n;

    if (!lc_text(l, console, key, from, value, sizeof(value)) || value[0] == '\0')
        return -1;
    n = strtol(value, &end, base);
    return *end == '\0' || *end == ',' ? n : -1;
}

/** Boot the disk and say whether it has the WAD. */
static struct etude_case_machine *boot(struct lc *l, uint64_t *at)
{
    static const char *const ids[] = {"wad", NULL};
    struct etude_case_machine *m;
    uint64_t from;

    if (!lc_step(l, "boot") || (m = lc_boot(l, "fixture")) == NULL ||
        !lc_wait(l, m, prompt, at, NULL))
        return NULL;
    if (!run(l, m, at, &from, wad_line, "BOOTED")) {
        missed(l, ids);
        return NULL;
    }
    expect(l, "wad", "WAD=", from, "yes",
           "/demo/doom1.wad, which the image stages from ports/doom/doom1.wad"
           " (sh ports/doom/fetch_wad.sh), exists:");
    return m;
}

/** Run the game with its WAD moved away. */
static void nowad(struct lc *l, struct etude_case_machine *m, uint64_t *at)
{
    static const char *const ids[] = {"nowad-status", "nowad-message", NULL};
    uint64_t from;

    if (!lc_step(l, "nowad"))
        return;
    if (!run(l, m, at, &from, nowad_line, "NOWADDONE")) {
        missed(l, ids);
        return;
    }
    expect(l, "nowad-status", "NOWAD=", from, "255", "without its WAD the game exited");
    expect(l, "nowad-message", "NOTFOUND=", from, "1",
           "the lines of the engine's IWAD not found error were");
}

/**
 * Start the game in the background, wait for its report of the first
 * frame drawn, and look at it running before it is killed.
 */
static void draw(struct lc *l, struct etude_case_machine *m, uint64_t *at)
{
    static const char *const ids[] = {"running", "framebuffer", "zone", NULL};
    uint64_t from;
    long running, geometry, failed, zone;
    char detail[160];

    if (!lc_step(l, "draw") || !lc_type(l, m, start_line) ||
        !lc_wait(l, m, "doom: 1 frames in ", at, "drew"))
        return;
    if (!run(l, m, at, &from, look_line, "DRAWDONE")) {
        missed(l, ids);
        return;
    }
    running = number(l, "RUNNING=", from, 10);
    snprintf(detail, sizeof(detail), "%ld game processes ran after the first frame", running);
    lc_assert(l, "running", running >= 1, detail);
    if (!lc_text(l, console, "FB=", from, detail, sizeof(detail)) ||
        sscanf(detail, "%ld,%ld", &geometry, &failed) != 2)
        geometry = failed = -1;
    snprintf(detail, sizeof(detail),
             "the game reported the framebuffer's geometry %ld times and failed to open it %ld"
             " times",
             geometry, failed);
    lc_assert(l, "framebuffer", geometry >= 1 && failed == 0, detail);
    zone = number(l, "ZONE=", from, 16);
    if (zone < 0)
        snprintf(detail, sizeof(detail), "the game printed no zone line");
    else
        snprintf(detail, sizeof(detail),
                 "the zone is 0x%lx bytes, under the %ld of one sized from the machine's free"
                 " memory",
                 zone, LEAST_ZONE);
    lc_assert(l, "zone", zone >= LEAST_ZONE, detail);
}

/** Play the first demo to its end, and count the frames the game drew. */
static void demo(struct lc *l, struct etude_case_machine *m, uint64_t *at)
{
    static const char *const ids[] = {"demo-exit", "demo-frames", NULL};
    uint64_t from;
    long frames;
    char detail[160];

    if (!lc_step(l, "demo"))
        return;
    if (!run(l, m, at, &from, demo_line, "DEMODONE")) {
        missed(l, ids);
        return;
    }
    expect(l, "demo-exit", "DEMOEXIT=", from, "0", "the demo played to its end exited");
    frames = number(l, "DEMOFRAMES=", from, 10);
    snprintf(detail, sizeof(detail), "the game's last report of the demo counts %ld frames",
             frames);
    lc_assert(l, "demo-frames", frames > 1, detail);
}

/** Record a demo until its buffer of 1 KiB fills. */
static void record(struct lc *l, struct etude_case_machine *m, uint64_t *at)
{
    static const char *const ids[] = {"record-mode", NULL};
    uint64_t from;

    if (!lc_step(l, "record"))
        return;
    if (!run(l, m, at, &from, record_line, "RECDONE")) {
        missed(l, ids);
        return;
    }
    expect(l, "record-mode", "RECMODE=", from, "644",
           "the demo the game recorded through fopen has mode");
    lc_quit(l, m, NULL);
}

/**
 * Play the first demo with sound, and judge the queue of /dev/dsp in the
 * game's last report.
 */
static void sound(struct lc *l, struct etude_case_machine *m, uint64_t *at)
{
    static const char *const ids[] = {"demo-exit", "dsp", "queue", NULL};
    uint64_t from;
    long last = -1, most = -1, period = -1;
    char detail[200];

    if (!lc_step(l, "sound"))
        return;
    if (!run(l, m, at, &from, sound_line, "SOUNDDONE")) {
        missed(l, ids);
        return;
    }
    expect(l, "demo-exit", "DEMOEXIT=", from, "0", "the demo played to its end exited");
    if (!lc_text(l, console, "QUEUE=", from, detail, sizeof(detail)) ||
        sscanf(detail, "%ld,%ld,%ld", &last, &most, &period) != 3)
        last = most = period = -1;
    snprintf(detail, sizeof(detail),
             "the game's last report has %ld samples queued on /dev/dsp after its last update,"
             " of periods of %ld",
             last, period);
    lc_assert(l, "dsp", last > 0 && period > 0, detail);
    snprintf(detail, sizeof(detail),
             "the most samples queued after an update are %ld, where %d periods of %ld are %ld",
             most, MOST_PERIODS, period, MOST_PERIODS * period);
    lc_assert(l, "queue", period > 0 && most >= last && most <= MOST_PERIODS * period, detail);
    lc_quit(l, m, NULL);
}

/** The scenario of the cases of sound, machine.doom.sound-hda and -sb16. */
static int sound_scenario(struct etude_case *c)
{
    static const char *const ids[] = {"wad", "demo-exit", "dsp", "queue", NULL};
    struct etude_case_machine *m;
    uint64_t at = 0;
    struct lc l;

    lc_begin(&l, c, ids);
    if ((m = boot(&l, &at)) != NULL)
        sound(&l, m, &at);
    return lc_finish(&l);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const ids[] = {
        "wad",  "nowad-status", "nowad-message", "drew",        "running", "framebuffer",
        "zone", "demo-exit",    "demo-frames",   "record-mode", NULL};
    struct etude_case_machine *m;
    uint64_t at = 0;
    struct lc l;

    if (strcmp(etude_case_id(c), "machine.doom.sound-hda") == 0 ||
        strcmp(etude_case_id(c), "machine.doom.sound-sb16") == 0)
        return sound_scenario(c);
    if (strcmp(etude_case_id(c), "machine.doom.game") != 0)
        return 1;
    lc_begin(&l, c, ids);
    if ((m = boot(&l, &at)) != NULL) {
        nowad(&l, m, &at);
        draw(&l, m, &at);
        demo(&l, m, &at);
        record(&l, m, &at);
    }
    return lc_finish(&l);
}
