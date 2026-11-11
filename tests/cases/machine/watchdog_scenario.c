/* Observe a q35 watchdog expiry through QMP, keep a panic's record across
 * the reset the kernel's TCO makes, and refuse std-vga in gfxtool. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lifecycle.h"

/** Set QEMU's action to pause, arm through the guest's real device ABI,
 * and wait for QMP to identify the paused run state as watchdog. The
 * case's ordinary progress bounds stop a timer that never expires. */
static void expire(struct lc *l)
{
    struct etude_case_machine *m;
    enum etude_status st;
    uint64_t at = 0;
    char *reply = NULL;
    struct timespec delay = {0, 10000000};
    int expired = 0;

    if ((m = lc_shell(l, "expire", "q35", &at)) == NULL) {
        return;
    }
    st = etude_qmp(m, "watchdog-set-action", "{\"action\":\"pause\"}", &reply);
    free(reply);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "select QMP watchdog pause");
        return;
    }
    if (!lc_type(l, m, "exec /watchdogprobe\r") ||
        !lc_wait(l, m, "WATCHDOGPROBE ARMED", &at, "armed")) {
        return;
    }
    while (!expired) {
        reply = NULL;
        st = etude_qmp(m, "query-status", NULL, &reply);
        if (st != ETUDE_OK) {
            free(reply);
            lc_halt(l, st, "read QMP watchdog state");
            return;
        }
        expired = reply && strstr(reply, "\"watchdog\"") != NULL;
        if (expired) {
            printf("watchdog: QMP query-status: %s\n", reply);
        }
        free(reply);
        if (!expired) {
            nanosleep(&delay, NULL);
        }
    }
    lc_assert(l, "watchdog", expired, "QMP did not report the watchdog run state");
    lc_quit(l, m, NULL);
}

/** Whether the console of the q35 machine from `from` to `to` holds `text`;
 * 0 when it cannot be read, which halts the scenario. */
static int console_holds(struct lc *l, uint64_t from, uint64_t to, const char *text)
{
    char *w = lc_console(l, "q35", from, to);
    int held = w != NULL && strstr(w, text) != NULL;

    free(w);
    return held;
}

/**
 * Boot with ramoops's region and the TCO started by the kernel, show that
 * the kernel feeds it for three heartbeats, panic, and let the TCO, unfed
 * once the tick stops, reset the guest; panic=0 halts the panic, so that
 * only the TCO can reset it, which the absence of the panic's countdown
 * shows; QEMU keeps the RAM across the
 * reset, and the second boot reads the record. QEMU runs with -no-reboot,
 * so the reboot action is set to reset first. pstoreprobe exits 0 only
 * after its checks, and prints none of the old log, whose records would
 * read as this boot's.
 */
static void crash(struct lc *l)
{
    struct etude_case_machine *m;
    enum etude_status st;
    uint64_t at = 0, from;
    char *reply = NULL, *w;

    if ((m = lc_shell(l, "crash", "q35", &at)) == NULL) {
        return;
    }
    st = etude_qmp(m, "set-action", "{\"reboot\":\"reset\",\"watchdog\":\"reset\"}", &reply);
    free(reply);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "select QMP reboot and watchdog reset");
        return;
    }
    lc_assert(
        l, "armed",
        console_holds(l, 0, at, "ramoops: ready: 0x100000 bytes at 0x4000000, 14 dmesg zones") &&
            console_holds(l, 0, at, "running, fed by the kernel until opened, heartbeat 5 s"),
        "the boot did not record ramoops ready and the TCO fed by the kernel");
    w = lc_run(l, m, "q35", "exec /pstoreprobe fed 15\r", &at, "fed");
    if (w == NULL) {
        return;
    }
    free(w);
    if (!lc_type(l, m, "fault\r") || !lc_wait(l, m, "panic: stopped: ", &at, "panic")) {
        return;
    }
    from = at;
    if (!lc_wait(l, m, "Type 'help'", &at, NULL) || !lc_wait(l, m, "\n> ", &at, NULL)) {
        return;
    }
    lc_assert(l, "reset",
              !console_holds(l, from, at, "panic: rebooting in") &&
                  console_holds(l, from, at, "ramoops: found: dmesg-ramoops-0"),
              "the panic counted down to its own restart, or the second boot found no dmesg "
              "record");
    w = lc_run(l, m, "q35", "exec /pstoreprobe read\r", &at, "record");
    if (w == NULL) {
        return;
    }
    free(w);
    lc_quit(l, m, NULL);
}

/** Run the real utility on std-vga, require its refusal and a failed
 * process status, then end the guest through QMP. */
static void refuse(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at = 0, from;
    char *text;

    if ((m = lc_shell(l, "refuse", "vga", &at)) == NULL) {
        return;
    }
    from = at;
    if (!lc_type(l, m, "exec /gfxtool dump\r")) {
        return;
    }
    if (!lc_wait(l, m, ", returning to kernel shell", &at, NULL)) {
        return;
    }
    text = lc_console(l, "vga", from, at);
    if (text) {
        lc_assert(l, "refused",
                  strstr(text, "gfxtool: refused:") != NULL &&
                      strstr(text, " exited with status 1, returning to kernel shell") != NULL,
                  "gfxtool did not refuse std-vga with exit status 1");
        free(text);
    }
    lc_quit(l, m, NULL);
}

/** Dispatch the three independent diagnostic boots. */
int etude_scenario(struct etude_case *c)
{
    static const char *const expiry[] = {"armed", "watchdog", NULL};
    static const char *const record[] = {"armed", "fed", "panic", "reset", "record", NULL};
    static const char *const refusal[] = {"refused", NULL};
    struct lc l;

    if (strstr(etude_case_id(c), ".expire")) {
        lc_begin(&l, c, expiry);
        expire(&l);
    } else if (strstr(etude_case_id(c), ".pstore")) {
        lc_begin(&l, c, record);
        crash(&l);
    } else {
        lc_begin(&l, c, refusal);
        refuse(&l);
    }
    return lc_finish(&l);
}
