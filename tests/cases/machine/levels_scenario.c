/*
 * levels_scenario.c: the scenario of machine.screen.levels, in which init
 * keeps the backlight's and the mixer's levels across a reboot and a reset,
 * and the console blanks with the backlight off: three boots of one copy
 * of the shutdown disk, on an HDA machine whose SSDT gives the backlight
 * acpi_video0, the last with a blank time of a few seconds.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* The root shell's prompt, which init-shell starts on the console. */
static const char prompt[] = "# ";

/* The backlight's directory, and the line that reads its level, its
 * actual level and the volume, each named with an empty pair of quotes
 * inside it so that the line's echo is not taken for it. */
#define BL      "/sys/class/backlight/acpi_video0"
#define BL_KEPT "/var/lib/backlight/acpi_video0"
#define READ \
    "echo LEVELS\"\"=$(cat " BL "/brightness)/$(cat " BL "/actual_brightness)/$(mixer vol)\r"

/* QMP's send-key of Shift, which pokes the console and types nothing. */
static const char shift[] = "{\"keys\":[{\"type\":\"qcode\",\"data\":\"shift\"}]}";

/* The machine of a boot, its console's name, and the offset its console has
 * been read to. */
struct boot {
    struct lc *l;
    struct etude_case_machine *m;
    char console[32];
    uint64_t at;
};

/**
 * Begin step `step` and boot machine `machine` on the copy, the `n`th boot
 * of that machine, which names its console, to the shell's prompt.
 *
 * @return 1 when the prompt came.
 */
static int boot(struct lc *l, struct boot *b, const char *step, const char *machine, int n)
{
    memset(b, 0, sizeof(*b));
    b->l = l;
    snprintf(b->console, sizeof(b->console), "%s.%d.console", machine, n);
    return lc_step(l, step) && (b->m = lc_boot(l, machine)) != NULL &&
           lc_wait(l, b->m, prompt, &b->at, NULL);
}

/**
 * Type `command`, which prints `NAME=` and a value last; wait for the
 * value's line and put the value in `buf`, of `size` bytes.
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
 * Report assertion `id`, that the levels line `got` is `want`, with `what`
 * in the detail.
 */
static void same(struct lc *l, const char *id, const char *got, const char *want, const char *what)
{
    char detail[200];

    snprintf(detail, sizeof(detail), "%s: backlight/actual/mixer read '%s', not '%s'", what, got,
             want);
    lc_assert(l, id, strcmp(got, want) == 0, detail);
}

/** Press Shift on the machine's keyboard. */
static void press(struct boot *b)
{
    char *reply = NULL;
    enum etude_status st;

    if (!lc_ok(b->l))
        return;
    st = etude_qmp(b->m, "send-key", shift, &reply);
    free(reply);
    if (st != ETUDE_OK)
        lc_halt(b->l, st, "QMP's send-key");
}

/**
 * Boot 1: read the levels the firmware and the driver give, set the
 * backlight to 7 and the volume to 5, and reboot through init. Boot 2:
 * read both back and in the kept files, set 9 and 6, wait until init has
 * kept them, and quit QEMU while the guest is still running. Boot 3, whose
 * console blanks after four seconds: read 9 and 6, then the backlight at 0
 * with its level kept, then back at 9 after a key.
 */
static void levels(struct lc *l)
{
    struct boot b;
    char got[96], detail[200];
    uint64_t from;

    if (!boot(l, &b, "set", "fixture", 1) || !ask(&b, READ, "LEVELS", got, sizeof(got)))
        return;
    snprintf(detail, sizeof(detail), "the first boot reads '%s'", got);
    lc_assert(l, "default", strncmp(got, "20/20/vol ", 10) == 0 && strstr(got, "5:5") == NULL,
              detail);
    if (!ask(&b, "echo 7 > " BL "/brightness; mixer vol 5; " READ, "LEVELS", got, sizeof(got)))
        return;
    same(l, "set", got, "7/7/vol 5:5", "after the writes");
    if (!restart(&b, "reboot\r", "reset-saved"))
        return;

    if (!boot(l, &b, "restored", "fixture", 2) || !ask(&b, READ, "LEVELS", got, sizeof(got)))
        return;
    same(l, "restored", got, "7/7/vol 5:5", "after the reboot");
    if (!ask(&b,
             "echo KEPT\"\"=$(cat /var/lib/backlight/acpi_video0)/"
             "$(grep '^vol ' /var/lib/mixer)/$(echo '\\_SB.RVID.LCD0._BQC' > /proc/acpi/call; "
             "cat /proc/acpi/call)\r",
             "KEPT", got, sizeof(got)))
        return;
    snprintf(detail, sizeof(detail), "the kept files and _BQC read '%s'", got);
    lc_assert(l, "file", strncmp(got, "7/vol 5:5/", 10) == 0 && strstr(got, "0x23") != NULL,
              detail);
    if (!ask(&b,
             "echo 9 > " BL "/brightness; mixer vol 6; "
             "until grep -qx 9 " BL_KEPT " && grep -qx 'vol 6:6' /var/lib/mixer && "
             "[ ! -e " BL_KEPT ".new ] && [ ! -e /var/lib/mixer.new ]; "
             "do sleep 1; done; echo SAVED\"\"=yes\r",
             "SAVED", got, sizeof(got)))
        return;
    lc_assert(l, "saved", strcmp(got, "yes") == 0, "init did not keep 9 and vol 6:6");
    if (!lc_quit(l, b.m, "power-cut"))
        return;

    if (!boot(l, &b, "blank", "blanking", 1) || !ask(&b, READ, "LEVELS", got, sizeof(got)))
        return;
    if (strncmp(got, "9/", 2) != 0 || strstr(got, "/vol 6:6") == NULL) {
        snprintf(detail, sizeof(detail), "after the stop: '%s'", got);
        lc_assert(l, "reset-kept", 0, detail);
    } else {
        if (!ask(&b,
                 "grep -qx 9 " BL_KEPT " && grep -qx 'vol 6:6' /var/lib/mixer; "
                 "echo FILES\"\"=$?\r",
                 "FILES", got, sizeof(got)))
            return;
        lc_assert(l, "reset-kept", strcmp(got, "0") == 0, "a saved file is missing or stale");
    }
    from = 0;
    if (!lc_wait(l, b.m, "console: blanked: 1,", &from, NULL))
        return;
    if (!ask(&b, "until [ $(cat " BL "/actual_brightness) = 0 ]; do sleep 1; done; " READ, "LEVELS",
             got, sizeof(got)))
        return;
    same(l, "blank-off", got, "9/0/vol 6:6", "while blank");
    from = b.at;
    press(&b);
    if (!lc_wait(l, b.m, "console: unblanked: 1,", &from, NULL))
        return;
    if (!ask(&b, "until [ $(cat " BL "/actual_brightness) = 9 ]; do sleep 1; done; " READ, "LEVELS",
             got, sizeof(got)))
        return;
    lc_assert(l, "unblank-on", strncmp(got, "9/9/", 4) == 0,
              "the key's unblank did not give the backlight its level again");
    lc_quit(l, b.m, NULL);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const ids[] = {"default",   "set",        "reset-saved", "restored",
                                      "file",      "saved",      "power-cut",   "reset-kept",
                                      "blank-off", "unblank-on", NULL};
    struct lc l;

    lc_begin(&l, c, ids);
    levels(&l);
    return lc_finish(&l);
}
