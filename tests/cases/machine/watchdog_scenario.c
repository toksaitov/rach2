/* Observe a q35 watchdog expiry through QMP; refuse std-vga in gfxtool. */
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

/** Dispatch the two independent diagnostic boots. */
int etude_scenario(struct etude_case *c)
{
    static const char *const expiry[] = {"armed", "watchdog", NULL};
    static const char *const refusal[] = {"refused", NULL};
    struct lc l;

    if (strstr(etude_case_id(c), ".expire")) {
        lc_begin(&l, c, expiry);
        expire(&l);
    } else {
        lc_begin(&l, c, refusal);
        refuse(&l);
    }
    return lc_finish(&l);
}
