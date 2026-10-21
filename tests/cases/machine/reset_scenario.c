/*
 * reset_scenario.c: the scenario of machine.reset, in which the kernel's
 * debug shell restarts the machine with the first N reset mechanisms left
 * out, and sends the firmware's own reset only to a firmware that SMBIOS
 * names as Insyde's. QEMU's SHUTDOWN event must say a reset.
 */
#include <stdint.h>
#include <string.h>

#include "lifecycle.h"

/* The start of the restart's record; the number of mechanisms left out and
 * a closing parenthesis end it. */
static const char restarting[] = "reboot: restarting (skipping ";

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/**
 * Run the case whose machine leaves out `skip` mechanisms, a digit, and
 * whose SMBIOS names Insyde's firmware when `insyde` is set.
 */
static void restart(struct lc *l, char skip, int insyde)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    uint64_t at = 0;
    char said[64];
    size_t n = strlen(restarting);

    memcpy(said, restarting, n);
    said[n] = skip;
    said[n + 1] = ')';
    said[n + 2] = '\0';
    if (!lc_step(l, "reboot") || (m = lc_boot(l, "fixture")) == NULL ||
        !lc_wait(l, m, "Type 'help'", &at, NULL) || !lc_wait(l, m, "\n> ", &at, NULL) ||
        !lc_type(l, m, "reboot\r") || !lc_end(l, m, &end))
        return;
    lc_assert(l, "reset", end.end == ETUDE_END_RESET, "QEMU's event is not a reset");
    lc_find(l, m, "said-restart", said, 0, 1);
    lc_find(l, m, "no-fallback", "the platform did not reset", 0, 0);
    lc_find(l, m, "no-critical", "<2>[", 0, 0);
    if (insyde) {
        lc_find(l, m, "sent-insyde", "the firmware is Insyde Corp.: sending its own reset", 0, 1);
        lc_find(l, m, "not-unsent", "its reset is not sent", 0, 0);
    } else if (skip == '0') {
        lc_find(l, m, "not-insyde", "not Insyde: its reset is not sent", 0, 1);
    }
}

int etude_scenario(struct etude_case *c)
{
    static const char *const first[] = {"reset",       "said-restart", "no-fallback",
                                        "no-critical", "not-insyde",   NULL};
    static const char *const later[] = {"reset", "said-restart", "no-fallback", "no-critical",
                                        NULL};
    static const char *const insyde[] = {
        "reset", "said-restart", "no-fallback", "no-critical", "sent-insyde", "not-unsent", NULL};
    const char *name = case_name(c);
    struct lc l;

    if (strcmp(name, "skip0") == 0) {
        lc_begin(&l, c, first);
        restart(&l, '0', 0);
    } else if (strcmp(name, "skip1") == 0 || strcmp(name, "skip2") == 0) {
        lc_begin(&l, c, later);
        restart(&l, name[4], 0);
    } else if (strcmp(name, "insyde") == 0) {
        lc_begin(&l, c, insyde);
        restart(&l, '0', 1);
    } else {
        return 1;
    }
    return lc_finish(&l);
}
