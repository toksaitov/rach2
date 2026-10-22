/*
 * servicelog_scenario.c: the scenario of the servicelog case
 * (../machine.servicelog.json). Two boots of one copy of an ext2 root whose
 * init is init-shell: the first fills init's service log past its cap and
 * makes orphans until init empties it, fills it again and powers off; the
 * second finds the full log removed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* The log's cap in bytes, SERVICE_LOG_CAP of utils/init.c. */
#define CAP 262144

/* The line /etc/profile of the root prints when the login shell starts. */
static const char shell_up[] = "servicelog: shell up";

/** Read `NAME=` and the number after it from boot `boot`'s console after
 * `from`; give -1 when there is none. */
static long value(struct lc *l, unsigned boot, const char *name, uint64_t from)
{
    char console[64], key[32], buf[32], *end;
    long n;

    snprintf(console, sizeof(console), "fixture.%u.console", boot);
    snprintf(key, sizeof(key), "%s=", name);
    if (!lc_text(l, console, key, from, buf, sizeof(buf)))
        return -1;
    n = strtol(buf, &end, 10);
    return end != buf ? n : -1;
}

/**
 * Type `command` at the shell and wait for `marker`, which it prints last,
 * written in the command with an empty pair of quotes inside it so that the
 * line's echo is not taken for it.
 */
static int command(struct lc *l, struct etude_case_machine *m, uint64_t *at, const char *command,
                   const char *marker)
{
    return lc_type(l, m, command) && lc_wait(l, m, marker, at, NULL);
}

/**
 * The first boot: fill the log past its cap; make orphans, each reaped by
 * init in a turn of its loop, until init has emptied the log or a thousand
 * have been made; read its size and the lines saying it was emptied; fill it
 * again and power off, which unmounts the root. Init looks at the log every
 * 60 turns, so a working init empties it after some 60 orphans, and one that
 * never does is judged after the thousandth rather than at the step's bound.
 */
static void first(struct lc *l)
{
    static const char fill[] = "dd if=/dev/zero bs=1024 count=300 >> /var/log/services "
                               "2>/dev/null; echo FILLED\"\"=$(wc -c < /var/log/services)\r";
    static const char orphans[] =
        "n=0; while [ $n -lt 1000 ] && [ $(wc -c < /var/log/services) -ge 262144 ]; do "
        "(true &); n=$((n + 1)); done; echo ORPHANS\"\"=$n\r";
    static const char look[] = "echo SIZE\"\"=$(wc -c < /var/log/services); "
                               "echo NOTES\"\"=$(grep -c 'reached its limit' /var/log/services)\r";
    static const char refill[] =
        "dd if=/dev/zero bs=1024 count=300 >> /var/log/services "
        "2>/dev/null; sync; echo REFILLED\"\"=$(wc -c < /var/log/services)\r";
    struct etude_case_machine *m;
    struct etude_termination end;
    uint64_t at = 0, from;
    long filled, made, size, notes, refilled;
    char detail[120];

    if (!lc_step(l, "boot") || (m = lc_boot(l, "fixture")) == NULL ||
        !lc_wait(l, m, shell_up, &at, NULL) || !lc_step(l, "fill"))
        return;
    from = at;
    if (!command(l, m, &at, fill, "FILLED=") || !lc_wait(l, m, "\n", &at, NULL))
        return;
    filled = value(l, 1, "FILLED", from);
    snprintf(detail, sizeof(detail), "the log was filled to %ld bytes, not past its cap", filled);
    lc_assert(l, "filled", filled >= CAP, detail);
    if (!command(l, m, &at, orphans, "ORPHANS=") || !command(l, m, &at, look, "NOTES=") ||
        !lc_wait(l, m, "\n", &at, NULL))
        return;
    made = value(l, 1, "ORPHANS", from);
    size = value(l, 1, "SIZE", from);
    notes = value(l, 1, "NOTES", from);
    printf("servicelog: init emptied the log after %ld orphans\n", made);
    snprintf(detail, sizeof(detail),
             "after %ld orphans the log has %ld bytes and %ld lines saying it was emptied", made,
             size, notes);
    lc_assert(l, "emptied", size >= 0 && size < 4096 && notes == 1, detail);
    if (!command(l, m, &at, refill, "REFILLED=") || !lc_wait(l, m, "\n", &at, NULL))
        return;
    refilled = value(l, 1, "REFILLED", from);
    snprintf(detail, sizeof(detail), "the log was refilled to %ld bytes, not past its cap",
             refilled);
    lc_assert(l, "refilled", refilled >= CAP, detail);
    if (!lc_type(l, m, "poweroff\r") || !lc_end(l, m, &end))
        return;
    lc_assert(l, "powered-off", end.end == ETUDE_END_SHUTDOWN,
              "the machine did not end by QEMU's SHUTDOWN event");
    lc_find(l, m, "unmounted", "shutdown: root: unmounted", from, 1);
}

/**
 * The second boot, on the root the first left: the log must have been
 * removed, so that it is small and has no line saying it was emptied.
 */
static void second(struct lc *l)
{
    static const char look[] = "echo SIZE\"\"=$(wc -c < /var/log/services); "
                               "echo NOTES\"\"=$(grep -c 'reached its limit' /var/log/services)\r";
    struct etude_case_machine *m;
    uint64_t at = 0;
    long size, notes;
    char detail[120];

    if (!lc_step(l, "reboot") || (m = lc_boot(l, "fixture")) == NULL ||
        !lc_wait(l, m, shell_up, &at, NULL) || !lc_step(l, "check") ||
        !command(l, m, &at, look, "NOTES=") || !lc_wait(l, m, "\n", &at, NULL))
        return;
    size = value(l, 2, "SIZE", 0);
    notes = value(l, 2, "NOTES", 0);
    snprintf(detail, sizeof(detail), "the log has %ld bytes and %ld lines saying it was emptied",
             size, notes);
    lc_assert(l, "removed", size >= 0 && size < 4096 && notes == 0, detail);
    lc_quit(l, m, NULL);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const ids[] = {"filled",    "emptied", "refilled", "powered-off",
                                      "unmounted", "removed", NULL};
    struct lc l;

    lc_begin(&l, c, ids);
    first(&l);
    second(&l);
    return lc_finish(&l);
}
