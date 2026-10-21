/*
 * crash_scenario.c: the scenario of machine.crash, in which the dirty-mount
 * flag of a KRFFS disk tells an orderly stop from a crash: a machine ended
 * after the mount leaves the flag set, a halt and a power-off from the
 * kernel's debug shell clear it, and dirtyprobe's clears of the set-id bits
 * never reach the root node that keeps it.
 */
#include <stdint.h>
#include <string.h>

#include "lifecycle.h"

/* The disk artifact every case boots a copy of. */
static const char disk[] = "crash-disk";

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/** Boot the machine of step `step` and wait for the debug shell's prompt. */
static struct etude_case_machine *shell(struct lc *l, const char *step, uint64_t *at)
{
    struct etude_case_machine *m;

    *at = 0;
    if (!lc_step(l, step) || (m = lc_boot(l, "fixture")) == NULL ||
        !lc_wait(l, m, "Type 'help'", at, NULL) || !lc_wait(l, m, "\n> ", at, NULL))
        return NULL;
    return m;
}

/** Run the case of a machine ended once the disk is mounted. */
static void killed(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at = 0;

    if (lc_step(l, "crash") && (m = lc_boot(l, "fixture")) != NULL &&
        lc_wait(l, m, " (disk)", &at, "mounted"))
        lc_quit(l, m, "ended-by-quit");
    if (lc_step(l, "fsck"))
        lc_fsck(l, disk, 1, "fsck-dirty");
}

/** Run the case of the debug shell's halt, which leaves the disk clean. */
static void halt(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at;

    if ((m = shell(l, "halt", &at)) != NULL && lc_type(l, m, "halt\r") &&
        lc_wait(l, m, "halt: system halted", &at, "said-halt"))
        lc_quit(l, m, "ended-by-quit");
    if (lc_step(l, "fsck"))
        lc_fsck(l, disk, 0, "fsck-clean");
}

/**
 * Run the case of the debug shell's power-off, which ends QEMU by the
 * power-off's event after a clean unmount.
 */
static void poweroff(struct lc *l)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    uint64_t at;

    if ((m = shell(l, "poweroff", &at)) != NULL && lc_type(l, m, "poweroff\r") &&
        lc_end(l, m, &end)) {
        lc_assert(l, "stopped", end.end == ETUDE_END_SHUTDOWN || end.end == ETUDE_END_RESET,
                  "the machine did not end by QEMU's SHUTDOWN event");
        lc_assert(l, "poweroff-status", end.end == ETUDE_END_SHUTDOWN,
                  "the event's reason is not a power-off");
        lc_find(l, m, "clean-unmount", "shutdown: clean unmount: dirty-mount flag cleared", 0, 1);
        lc_find(l, m, "no-halt-fallback", "halting instead", 0, 0);
    }
    if (lc_step(l, "fsck"))
        lc_fsck(l, disk, 0, "fsck-clean");
}

/**
 * Run the case of dirtyprobe, whose line must say OK; the machine is then
 * ended, and the flag must still be set on the disk.
 */
static void dirtyprobe(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at, from;

    if ((m = shell(l, "probe", &at)) != NULL && lc_type(l, m, "exec /dirtyprobe\r")) {
        from = at;
        if (lc_wait(l, m, "DIRTYPROBE ", &at, NULL) && lc_wait(l, m, "\n", &at, NULL)) {
            lc_find(l, m, "probe-ok", "DIRTYPROBE OK", from, 1);
            lc_quit(l, m, "ended-by-quit");
        } else if (!l->stopped) {
            lc_assert(l, "probe-ok", 0, l->why);
        }
    }
    if (lc_step(l, "fsck"))
        lc_fsck(l, disk, 1, "fsck-dirty");
}

int etude_scenario(struct etude_case *c)
{
    static const char *const kills[] = {"mounted", "ended-by-quit", "fsck-dirty", NULL};
    static const char *const halts[] = {"said-halt", "ended-by-quit", "fsck-clean", NULL};
    static const char *const poweroffs[] = {"stopped",          "poweroff-status", "clean-unmount",
                                            "no-halt-fallback", "fsck-clean",      NULL};
    static const char *const probes[] = {"probe-ok", "ended-by-quit", "fsck-dirty", NULL};
    const char *name = case_name(c);
    struct lc l;

    if (strcmp(name, "killed") == 0) {
        lc_begin(&l, c, kills);
        killed(&l);
    } else if (strcmp(name, "halt") == 0) {
        lc_begin(&l, c, halts);
        halt(&l);
    } else if (strcmp(name, "poweroff") == 0) {
        lc_begin(&l, c, poweroffs);
        poweroff(&l);
    } else if (strcmp(name, "dirtyprobe") == 0) {
        lc_begin(&l, c, probes);
        dirtyprobe(&l);
    } else {
        return 1;
    }
    return lc_finish(&l);
}
