/*
 * panic_scenario.c: the scenario of machine.panic, in which a kernel stack
 * run out at boot panics the kernel, which keeps its record for panic=N
 * seconds and then resets the machine by the methods reboot= names, in
 * Linux's order. QEMU's SHUTDOWN event must say a reset.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lifecycle.h"

/* The start of each method's record, as power.c writes it. */
static const char emergency[] = "reboot: emergency: ";

/* A case: its name, the seconds it waits, and the methods it tries, by the
 * text after `emergency`, in order, to a NULL. */
struct panic_case {
    const char *name;
    int timeout;
    const char *methods[3];
};

static const struct panic_case cases[] = {
    {"acpi", 5, {"acpi reset register", NULL}},
    {"kbd", 5, {"keyboard controller", NULL}},
    {"cf9", 5, {"port 0xcf9", NULL}},
    {"triple", 5, {"triple fault", NULL}},
    {"default", 5, {"acpi: the fadt has no reset register", "keyboard controller", NULL}},
    {"immediate", -1, {"acpi: the fadt has no reset register", "keyboard controller", NULL}},
};

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/**
 * Run case `pc`: wait for the panic, record it as the boot's fault, then
 * for the line that says when the machine resets, or check that none comes,
 * and for each method's record in turn; then wait for the machine's end,
 * which must be a reset, with no other method's record after the last.
 */
static void run(struct lc *l, const struct panic_case *pc)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    enum etude_status st;
    uint64_t at = 0, panicked;
    char line[64], said[96];
    int i;

    if (!lc_step(l, "boot") || (m = lc_boot(l, "fixture")) == NULL ||
        !lc_wait(l, m, "panic: stopped: double fault: a kernel stack ran out", &at, "panicked"))
        return;
    panicked = at;
    if (pc->timeout > 0) {
        snprintf(line, sizeof(line), "panic: rebooting in %d seconds..", pc->timeout);
        if (!lc_wait(l, m, line, &at, "said-wait"))
            return;
    }
    st = etude_fault(m, ETUDE_END_PANIC);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "the record of the kernel's panic");
        return;
    }
    for (i = 0; pc->methods[i] != NULL; i++) {
        snprintf(said, sizeof(said), "%s%s", emergency, pc->methods[i]);
        if (!lc_wait(l, m, said, &at, pc->methods[i + 1] == NULL ? "methods" : NULL))
            return;
    }
    if (!lc_end(l, m, &end))
        return;
    lc_assert(l, "reset", end.end == ETUDE_END_RESET, "QEMU's event is not a reset");
    lc_find(l, m, "no-other", emergency, at, 0);
    if (pc->timeout <= 0)
        lc_find(l, m, "no-wait", "panic: rebooting in ", panicked, 0);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const waits[] = {"panicked", "said-wait", "methods",
                                        "no-other", "reset",     NULL};
    static const char *const at_once[] = {"panicked", "no-wait", "methods",
                                          "no-other", "reset",   NULL};
    const char *name = case_name(c);
    struct lc l;
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (strcmp(name, cases[i].name) == 0) {
            lc_begin(&l, c, cases[i].timeout > 0 ? waits : at_once);
            run(&l, &cases[i]);
            return lc_finish(&l);
        }
    }
    return 1;
}
