/*
 * mixerkept_scenario.c: the scenario of machine.audio.kept, in which init
 * keeps the mixer's levels across a reboot: three boots of one copy of the
 * shutdown disk, on an HDA machine, read mixer's level each time.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lifecycle.h"

/* The root shell's prompt, which init-shell starts on the console. */
static const char prompt[] = "# ";

/* The machine of a boot, its console's name, and the offset its console has
 * been read to. */
struct boot {
    struct lc *l;
    struct etude_case_machine *m;
    char console[32];
    uint64_t at;
};

/**
 * Begin step `step` and boot the copy for the `n`th time, to the shell's
 * prompt.
 *
 * @return 1 when the prompt came.
 */
static int boot(struct lc *l, struct boot *b, const char *step, int n)
{
    memset(b, 0, sizeof(*b));
    b->l = l;
    snprintf(b->console, sizeof(b->console), "fixture.%d.console", n);
    return lc_step(l, step) && (b->m = lc_boot(l, "fixture")) != NULL &&
           lc_wait(l, b->m, prompt, &b->at, NULL);
}

/**
 * Type `command`, which prints `NAME=` and a value last, the name written
 * with an empty pair of quotes inside it so that the line's echo is not
 * taken for it; wait for the value's line and put the value in `buf`, of
 * `size` bytes.
 *
 * @return 1 when the value came.
 */
static int ask(struct boot *b, const char *command, const char *name, char *buf, size_t size)
{
    char key[32];
    uint64_t from = b->at;

    snprintf(key, sizeof(key), "%s=", name);
    return lc_type(b->l, b->m, command) && lc_wait(b->l, b->m, key, &b->at, NULL) &&
           lc_wait(b->l, b->m, "\n", &b->at, NULL) &&
           lc_text(b->l, b->console, key, from, buf, size);
}

/**
 * Type `line`, which stops the machine, and report assertion `id`, that the
 * machine ended by a reset.
 *
 * @return 1 when it did.
 */
static int restart(struct boot *b, const char *line, const char *id)
{
    struct etude_termination end;
    int reset;

    if (!lc_type(b->l, b->m, line) || !lc_end(b->l, b->m, &end))
        return 0;
    reset = end.end == ETUDE_END_RESET;
    lc_assert(b->l, id, reset, "the machine did not end by a reset");
    if (!reset)
        lc_halt(b->l, ETUDE_FAILED, "the reboot");
    return reset;
}

/**
 * Set the volume to 5 and reboot through init; read 5 back and the kept
 * file; remove the file, which init writes again only when a level
 * changes, and reboot past init, which then saves nothing; and read the
 * driver's level of the first boot again.
 */
static void kept(struct lc *l)
{
    struct boot b;
    char first[64], got[64], detail[200];

    if (!boot(l, &b, "set", 1) ||
        !ask(&b, "echo VOL\"\"=$(mixer vol)\r", "VOL", first, sizeof(first)))
        return;
    snprintf(detail, sizeof(detail), "the first boot's mixer reads '%s'", first);
    lc_assert(l, "default", strncmp(first, "vol ", 4) == 0 && strcmp(first, "vol 5:5") != 0,
              detail);
    if (!ask(&b, "mixer vol 5; echo VOL\"\"=$(mixer vol)\r", "VOL", got, sizeof(got)))
        return;
    snprintf(detail, sizeof(detail), "mixer vol 5 reads '%s'", got);
    lc_assert(l, "set", strcmp(got, "vol 5:5") == 0, detail);
    if (!restart(&b, "reboot\r", "reset-saved"))
        return;

    if (!boot(l, &b, "restored", 2) ||
        !ask(&b, "echo VOL\"\"=$(mixer vol)\r", "VOL", got, sizeof(got)))
        return;
    snprintf(detail, sizeof(detail), "after the reboot mixer reads '%s', not 'vol 5:5'", got);
    lc_assert(l, "restored", strcmp(got, "vol 5:5") == 0, detail);
    if (!ask(&b, "echo KEPT\"\"=$(grep '^vol ' /var/lib/mixer)\r", "KEPT", got, sizeof(got)))
        return;
    snprintf(detail, sizeof(detail), "/var/lib/mixer keeps '%s', not 'vol 5:5'", got);
    lc_assert(l, "file", strcmp(got, "vol 5:5") == 0, detail);
    if (!ask(&b, "rm /var/lib/mixer; echo GONE\"\"=$(ls /var/lib)\r", "GONE", got, sizeof(got)))
        return;
    if (!restart(&b, "reboot -f\r", "reset-unsaved"))
        return;

    if (!boot(l, &b, "default", 3) ||
        !ask(&b, "echo VOL\"\"=$(mixer vol)\r", "VOL", got, sizeof(got)))
        return;
    snprintf(detail, sizeof(detail), "without the file mixer reads '%s', not the first boot's '%s'",
             got, first);
    lc_assert(l, "default-again", strcmp(got, first) == 0, detail);
    lc_quit(l, b.m, NULL);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const ids[] = {"default", "set",           "reset-saved",   "restored",
                                      "file",    "reset-unsaved", "default-again", NULL};
    struct lc l;

    lc_begin(&l, c, ids);
    kept(&l);
    return lc_finish(&l);
}
