/*
 * crash_scenario.c: the scenario of machine.crash, in which dirtyprobe's
 * clears of the set-id bits never reach the root node of a KRFFS disk, whose
 * spare mode bit is the dirty-mount flag, and the machine ended after them
 * leaves the flag set.
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
    static const char *const probes[] = {"probe-ok", "ended-by-quit", "fsck-dirty", NULL};
    struct lc l;

    if (strcmp(case_name(c), "dirtyprobe") != 0)
        return 1;
    lc_begin(&l, c, probes);
    dirtyprobe(&l);
    return lc_finish(&l);
}
