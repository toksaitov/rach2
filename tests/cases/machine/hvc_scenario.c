/*
 * hvc_scenario.c: the scenario of guest.hvc's machine cases. In `restart`
 * the debug shell restarts a machine whose log goes to a virtio console's
 * port beside COM1: the driver must give the console back to COM1 and reset
 * the device before the kernel restarts. In `panic` the kernel panics with
 * the virtio console's transmit lock held (`rach2.vconpanic`) on a machine
 * whose only console is the port, and the panic's record must reach the
 * host through it.
 */
#include <stdint.h>
#include <string.h>

#include "lifecycle.h"

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/**
 * Boot to the debug shell, type reboot, and wait for the console to leave
 * the port, the device's reset and the restart, in that order, and for the
 * machine's end, which must be a reset. Assertions `enabled`, `halted`,
 * `then-restart` and `reset`.
 */
static void restart(struct lc *l)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    uint64_t at = 0;

    if ((m = lc_shell(l, "restart", "fixture", &at)) == NULL)
        return;
    lc_find(l, m, "enabled", "console: enabled: hvc0, port 0 of virtio0, for output alone", 0, 1);
    if (!lc_type(l, m, "reboot\r") ||
        !lc_wait(l, m, "console: disabled: hvc0: the system stops", &at, NULL) ||
        !lc_wait(l, m, "virtio_console: shutdown: virtio0: reset", &at, "halted") ||
        !lc_wait(l, m, "reboot: restarting", &at, "then-restart") || !lc_end(l, m, &end))
        return;
    lc_assert(l, "reset", end.end == ETUDE_END_RESET, "QEMU's event is not a reset");
}

/**
 * Boot with the test's panic and wait for the port to carry the console and
 * then the panic's record; then end the machine, which the panic left
 * halted. The machine's console is the port, on which etude shows no
 * fault's end, so the panic's records are its declared critical text.
 * Assertions `enabled` and `panicked`.
 */
static void panic(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at = 0;

    if (!lc_step(l, "boot") || (m = lc_boot(l, "fixture")) == NULL ||
        !lc_wait(l, m, "console: enabled: vport0p1, port 1 of ", &at, "enabled") ||
        !lc_wait(l, m, "panic: stopped: virtio_console: panic test: the transmit side is held", &at,
                 "panicked"))
        return;
    (void)lc_quit(l, m, NULL);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const restart_ids[] = {"enabled", "halted", "then-restart", "reset", NULL};
    static const char *const panic_ids[] = {"enabled", "panicked", NULL};
    const char *name = case_name(c);
    struct lc l;

    if (strcmp(name, "restart") == 0) {
        lc_begin(&l, c, restart_ids);
        restart(&l);
    } else if (strcmp(name, "panic") == 0) {
        lc_begin(&l, c, panic_ids);
        panic(&l);
    } else {
        return 1;
    }
    return lc_finish(&l);
}
