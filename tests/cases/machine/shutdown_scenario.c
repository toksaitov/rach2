/*
 * shutdown_scenario.c: the scenario of machine.shutdown, in which userland
 * stops the machine from a root shell on a disk fixture: a power-off, a
 * halt, a reset, a refused halt and the death of init, each judged by how
 * QEMU ended or what it says of the processors, and the disk by fsck.krffs.
 */
#include <stdint.h>
#include <string.h>

#include "lifecycle.h"

/* The disk artifact every case boots a copy of. */
static const char disk[] = "shutdown-disk";

/* The root shell's prompt, which init-shell starts on the console. */
static const char prompt[] = "# ";

static const char clean[] = "shutdown: clean unmount: dirty-mount flag cleared";
static const char fallback[] = "halting instead";
static const char poweroff[] = "poweroff: powering off";
static const char restart[] = "reboot: restarting";

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/**
 * Run a case whose `line` stops the machine in step `step`: QEMU's event
 * must say a power-off, or a reset when `reset` is set, after a clean
 * unmount, and the copy must then be clean.
 */
static void stop(struct lc *l, const char *step, const char *line, int reset)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    uint64_t at = 0;
    int ended = 0;

    if (lc_step(l, step) && (m = lc_boot(l, "fixture")) != NULL &&
        lc_wait(l, m, prompt, &at, NULL) && lc_type(l, m, line) && lc_end(l, m, &end)) {
        ended = end.end == ETUDE_END_SHUTDOWN || end.end == ETUDE_END_RESET;
        lc_assert(l, "stopped", ended, "the machine did not end by QEMU's SHUTDOWN event");
        lc_assert(l, reset ? "reset-status" : "poweroff-status",
                  end.end == (reset ? ETUDE_END_RESET : ETUDE_END_SHUTDOWN),
                  reset ? "the event's reason is not a reset"
                        : "the event's reason is not a power-off");
        lc_find(l, m, "clean-unmount", clean, 0, 1);
        lc_find(l, m, "no-halt-fallback", fallback, 0, 0);
        lc_find(l, m, reset ? "said-restart" : "said-poweroff", reset ? restart : poweroff, 0, 1);
        lc_find(l, m, reset ? "not-poweroff" : "not-restart", reset ? poweroff : restart, 0, 0);
    }
    if (lc_step(l, "fsck"))
        lc_fsck(l, disk, 0, "fsck-clean");
}

/** Run the second boot of the poweroff case, which reads the marker back. */
static void readback(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at = 0, from;

    if (!lc_step(l, "readback") || (m = lc_boot(l, "fixture")) == NULL ||
        !lc_wait(l, m, prompt, &at, NULL))
        return;
    from = at;
    if (lc_type(l, m, "cat /root/marker; echo READ\"\"BACK\r") &&
        lc_wait(l, m, "READBACK", &at, NULL))
        lc_find(l, m, "marker-survives", "\nmarker", from, 1);
    lc_quit(l, m, NULL);
}

/**
 * Run a case whose `line` halts the machine: the kernel says so after a
 * clean unmount, takes no other route, and leaves every processor halted
 * with its interrupts off, so that a line typed then reaches the serial
 * port and is never read. The copy must be clean.
 */
static void halt(struct lc *l, const char *line)
{
    struct etude_case_machine *m;
    uint64_t at = 0;

    if (lc_step(l, "halt") && (m = lc_boot(l, "fixture")) != NULL &&
        lc_wait(l, m, prompt, &at, NULL) && lc_type(l, m, line) &&
        lc_wait(l, m, "halt: system halted", &at, "said-halt")) {
        lc_find(l, m, "clean-unmount", clean, 0, 1);
        lc_halted(l, m, 1, "halted");
        if (lc_type(l, m, "echo AWA\"\"KE\r")) {
            lc_halted(l, m, 1, "still-halted");
            lc_received(l, m, "line-reached");
        }
        if (lc_quit(l, m, "ended-by-quit")) {
            lc_find(l, m, "no-halt-fallback", fallback, 0, 0);
            lc_find(l, m, "not-poweroff", poweroff, 0, 0);
            lc_find(l, m, "not-restart", restart, 0, 0);
            lc_find(l, m, "no-answer", "AWA", at, 0);
        }
    }
    if (lc_step(l, "fsck"))
        lc_fsck(l, disk, 0, "fsck-clean");
}

/**
 * Run the case of an unprivileged halt: the kernel refuses the signal to
 * init, the shell goes on, nothing reaches the shutdown, and the copy, never
 * unmounted, stays dirty.
 */
static void refused(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at = 0, from;

    if (lc_step(l, "refuse") && (m = lc_boot(l, "fixture")) != NULL &&
        lc_wait(l, m, prompt, &at, NULL)) {
        from = at;
        if (lc_type(l, m, "su test -c halt; echo STILL\"\"HERE\r") &&
            lc_wait(l, m, "STILLHERE", &at, "still-here")) {
            lc_find(l, m, "not-permitted", "Operation not permitted", from, 1);
            if (lc_quit(l, m, "ended-by-quit"))
                lc_find(l, m, "no-shutdown", "shutdown:", 0, 0);
        }
    }
    if (lc_step(l, "fsck"))
        lc_fsck(l, disk, 1, "fsck-dirty");
}

/**
 * Run the case of init ending on four processors. A SIGKILL sent to init
 * from inside its namespace leaves it alive, as on Linux, so the first
 * boot only shows that and leaves a program that exits at once, mixer, as
 * the copy's /sbin/init; on the next boot pid 1 exits by itself, the
 * kernel says so and stops all four processors, halted with their
 * interrupts off, and the copy stays dirty.
 */
static void init_death(struct lc *l)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    uint64_t at = 0, online = 0;

    if (lc_step(l, "replace") && (m = lc_boot(l, "fixture")) != NULL &&
        lc_wait(l, m, prompt, &at, NULL) &&
        lc_wait(l, m, "smp: processors: 4 online of 4", &online, "four-online") &&
        lc_type(l, m, "kill -KILL 1; echo STILL\"\"HERE\r") &&
        lc_wait(l, m, "STILLHERE", &at, "init-survives-kill") &&
        lc_type(l, m,
                "cp /bin/mixer /sbin/init.new; mv /sbin/init.new /sbin/init; "
                "poweroff -n -f\r") &&
        lc_end(l, m, &end)) {
        at = 0;
        if (lc_step(l, "exit") && (m = lc_boot(l, "fixture")) != NULL &&
            lc_wait(l, m, "init (pid 1) exited; halting", &at, "init-died")) {
            lc_halted(l, m, 4, "four-halted");
            lc_quit(l, m, "ended-by-quit");
        }
    }
    if (lc_step(l, "fsck"))
        lc_fsck(l, disk, 1, "fsck-dirty");
}

int etude_scenario(struct etude_case *c)
{
    static const char *const stops[] = {
        "stopped",       "poweroff-status", "clean-unmount", "no-halt-fallback",
        "said-poweroff", "not-restart",     "fsck-clean",    NULL};
    static const char *const poweroffs[] = {"stopped",          "poweroff-status", "clean-unmount",
                                            "no-halt-fallback", "said-poweroff",   "not-restart",
                                            "fsck-clean",       "marker-survives", NULL};
    static const char *const resets[] = {
        "stopped",      "reset-status", "clean-unmount", "no-halt-fallback",
        "said-restart", "not-poweroff", "fsck-clean",    NULL};
    static const char *const halts[] = {"said-halt",        "clean-unmount", "halted",
                                        "still-halted",     "line-reached",  "ended-by-quit",
                                        "no-halt-fallback", "not-poweroff",  "not-restart",
                                        "no-answer",        "fsck-clean",    NULL};
    static const char *const refusals[] = {"still-here",  "not-permitted", "ended-by-quit",
                                           "no-shutdown", "fsck-dirty",    NULL};
    static const char *const deaths[] = {"four-online", "init-survives-kill", "init-died",
                                         "four-halted", "ended-by-quit",      "fsck-dirty",
                                         NULL};
    const char *name = case_name(c);
    struct lc l;

    if (strcmp(name, "poweroff") == 0) {
        lc_begin(&l, c, poweroffs);
        stop(&l, "poweroff", "echo marker > /root/marker; poweroff -n -f\r", 0);
        readback(&l);
    } else if (strcmp(name, "halt") == 0 || strcmp(name, "bare-halt") == 0) {
        lc_begin(&l, c, halts);
        halt(&l, name[0] == 'h' ? "halt -n -f\r" : "halt\r");
    } else if (strcmp(name, "bare-poweroff") == 0) {
        lc_begin(&l, c, stops);
        stop(&l, "stop", "poweroff\r", 0);
    } else if (strcmp(name, "bare-reboot") == 0) {
        lc_begin(&l, c, resets);
        stop(&l, "stop", "reboot\r", 1);
    } else if (strcmp(name, "refused-halt") == 0) {
        lc_begin(&l, c, refusals);
        refused(&l);
    } else if (strcmp(name, "init-death") == 0) {
        lc_begin(&l, c, deaths);
        init_death(&l);
    } else {
        return 1;
    }
    return lc_finish(&l);
}
