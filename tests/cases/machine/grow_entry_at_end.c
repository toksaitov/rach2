/*
 * grow_entry_at_end.c: the scenario of machine.grow.entry-at-end: a disk
 * whose entry already reaches its end, as a grow stopped between the entry
 * and the resize leaves it, is finished by running growroot again, which
 * finds nothing to rewrite.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib_grow.h"

/** The line growroot prints once it has grown the root to the disk's end. */
static const char grown_line[] = "growroot: result: /dev/disk0p1 grown to 33552384 sectors";

/**
 * Boot recovery on the overlay of the disk whose entry is at its end, run
 * growroot, and judge its result and its record of the entry.
 */
int etude_scenario(struct etude_case *c)
{
    char why[200], *out = NULL;
    struct drive d;
    int st, grown;

    if (etude_step(c, "grow") != ETUDE_OK)
        return 1;
    grow_boot(&d, c, "recovery", NULL, "~ #");
    st = grow_shell(&d, "GROW", &out, "growroot MBRID=47470001,1");
    grown = out != NULL && strstr(out, grown_line) != NULL;
    free(out);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    snprintf(why, sizeof(why), "growroot ended with status %d without saying %s", st, grown_line);
    etude_assert(c, "grown", grown, d.failed ? d.why : why);
    etude_assert(c, "entry",
                 grow_console_has(&d, "growroot: entry: /dev/disk0p1 already fills the 33552384 "
                                      "sectors after it"),
                 "growroot did not find the entry already at the disk's end");
    return 0;
}
