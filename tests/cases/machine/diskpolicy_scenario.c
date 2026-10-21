/*
 * diskpolicy_scenario.c: the scenario of machine.diskpolicy.bytes
 * (tests/cases/machine.diskpolicy.json). The read-write kernel changes the
 * disk it boots, and the read-only kernel leaves it byte for byte.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "etude/scenario.h"

/**
 * Say whether files `a` and `b` have the same bytes: 1 when they do, 0 when
 * they differ, and -1 when either cannot be read.
 */
static int same_bytes(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    char ba[65536], bb[65536];
    size_t na, nb;
    int same = -1;

    if (fa != NULL && fb != NULL) {
        do {
            na = fread(ba, 1, sizeof(ba), fa);
            nb = fread(bb, 1, sizeof(bb), fb);
        } while (na == nb && na > 0 && memcmp(ba, bb, na) == 0);
        if (ferror(fa) == 0 && ferror(fb) == 0)
            same = na == 0 && nb == 0;
    }
    if (fa != NULL)
        fclose(fa);
    if (fb != NULL)
        fclose(fb);
    return same;
}

/** Say whether the console of `m`, which has ended, has `text`. */
static int said(struct etude_case_machine *m, const char *text)
{
    uint64_t at;

    return etude_console_wait(m, text, 0, &at) == ETUDE_OK;
}

/**
 * Boot machine `machine` of step `step` on its private copy of the disk to
 * the kernel's debug shell, and end it there with QMP's `quit`, as a power
 * cut would, so that the boot's own writes are all the copy has.
 *
 * @return the machine, ended, or NULL when the step could not begin or the
 *         machine did not reach the shell.
 */
static struct etude_case_machine *boot_to_shell(struct etude_case *c, const char *step,
                                                const char *machine)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    char *reply = NULL;
    uint64_t at;

    if (etude_step(c, step) != ETUDE_OK || etude_boot(c, machine, NULL, &m, &end) != ETUDE_OK)
        return NULL;
    if (etude_console_wait(m, "Type 'help'", 0, &at) != ETUDE_OK) {
        fprintf(stderr, "diskpolicy: %s: the kernel did not reach its debug shell\n", machine);
        return NULL;
    }
    etude_qmp(m, "quit", NULL, &reply);
    free(reply);
    etude_wait_end(m, &end);
    return m;
}

int etude_scenario(struct etude_case *c)
{
    struct etude_case_machine *m;

    if ((m = boot_to_shell(c, "readwrite", "readwrite")) == NULL)
        return 1;
    etude_assert(c, "rw-mounted", said(m, "krffs: mounted:"),
                 "the read-write kernel did not mount the disk");
    etude_assert(c, "rw-changed",
                 same_bytes(etude_artifact(c, "rw-disk"), etude_private_copy(c, "rw-disk")) == 0,
                 "the read-write boot left its copy as the artifact is");
    if ((m = boot_to_shell(c, "readonly", "readonly")) == NULL)
        return 1;
    etude_assert(c, "ro-policy", said(m, "disk: policy: read-only"),
                 "the read-only kernel did not report its policy");
    etude_assert(c, "ro-refused", said(m, "refused: command"),
                 "the read-only kernel refused no command by name");
    etude_assert(c, "ro-unchanged",
                 same_bytes(etude_artifact(c, "ro-disk"), etude_private_copy(c, "ro-disk")) == 1,
                 "the read-only boot changed its copy of the disk");
    return 0;
}
