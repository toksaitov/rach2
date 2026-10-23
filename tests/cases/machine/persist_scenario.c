/*
 * persist_scenario.c: the scenario of machine.persist.fsync
 * (../machine.persist.json). persistprobe writes and syncs a file on the KRFFS
 * root of a disk, the machine is cut off, and a second boot of the same copy
 * reads the file back.
 */
#include <stdio.h>
#include <stdlib.h>

#include "console_report.h"

/* The debug shell's banner, after which it reads commands. */
#define SHELL_READY "Type 'help' for available commands."

/**
 * Begin step `step`, boot `boot` of the machine, counted from 1, on the
 * copy of the disk the earlier boots wrote, run `command` at the kernel's
 * debug shell and take persistprobe's report from the console. Then end the
 * machine with QMP's quit, as a power cut would, so that nothing but the
 * probe's own calls has written the disk.
 *
 * @return 0 when the machine reached the quit, with assertion `cut`, when it
 *         is not NULL, holding when the machine still ran and the quit ended
 *         it; 1 when the step did not begin, a wait did not complete or the
 *         report was not whole.
 */
static int run(struct etude_case *c, const char *step, unsigned boot, const char *command,
               const char *cut)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    enum etude_status quit;
    char *reply = NULL;
    uint64_t typed;

    if (etude_step(c, step) != ETUDE_OK || etude_boot(c, "fixture", NULL, &m, &end) != ETUDE_OK ||
        etude_console_wait(m, SHELL_READY, 0, &typed) != ETUDE_OK ||
        etude_type(m, command) != ETUDE_OK ||
        console_report(c, m, "fixture", boot, typed, NULL) != ETUDE_OK)
        return 1;
    quit = etude_qmp(m, "quit", NULL, &reply);
    free(reply);
    if (quit == ETUDE_STOPPED || etude_wait_end(m, &end) != ETUDE_OK)
        return 1;
    if (cut != NULL)
        etude_assert(c, cut, quit == ETUDE_OK && end.end == ETUDE_END_QEMU_EXITED,
                     "the machine was not running until QMP's quit ended it");
    return 0;
}

int etude_scenario(struct etude_case *c)
{
    if (run(c, "write", 1, "exec /persistprobe write\r", "cut") != 0 ||
        run(c, "read", 2, "exec /persistprobe read\r", NULL) != 0)
        return 1;
    return 0;
}
