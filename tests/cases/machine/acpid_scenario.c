/*
 * acpid_scenario.c: the scenario of the acpid case (../machine.acpid.json).
 * os-real on an Intel HDA controller, logged in as root: BusyBox's acpid,
 * which init starts, runs mixer for the volume keys QMP presses on the PS/2
 * keyboard, and powers the machine off in order for the ACPI power button
 * that QMP's system_powerdown presses.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* The console of the case's one boot. */
#define CONSOLE "real.1.console"

/** Read the password artifact and type it with a carriage return. */
static int type_password(struct lc *l, struct etude_case_machine *m)
{
    const char *path = etude_artifact(l->c, "password");
    char password[128], line[140];
    FILE *f = path != NULL ? fopen(path, "r") : NULL;
    int read = f != NULL && fgets(password, sizeof(password), f) != NULL;

    if (f != NULL)
        fclose(f);
    if (!read) {
        lc_halt(l, ETUDE_FAILED, "the read of the password");
        return 0;
    }
    password[strcspn(password, "\r\n")] = '\0';
    snprintf(line, sizeof(line), "%s\r", password);
    return lc_type(l, m, line);
}

/**
 * Type `command`, which prints `NAME=` and a value last, the name written
 * with an empty pair of quotes inside it so that the line's echo is not
 * taken for it; wait for the value's line and put the value in `buf`.
 *
 * @return 1 when the value came.
 */
static int ask(struct lc *l, struct etude_case_machine *m, uint64_t *at, const char *command,
               const char *name, char *buf, size_t size)
{
    char key[32];
    uint64_t from = *at;

    snprintf(key, sizeof(key), "%s=", name);
    return lc_type(l, m, command) && lc_wait(l, m, key, at, NULL) &&
           lc_wait(l, m, "\n", at, NULL) && lc_text(l, CONSOLE, key, from, buf, size);
}

/**
 * Press the key `qcode` through QMP, then wait at the shell until mixer's
 * reading is no longer `before`, for at most 30 looks a second apart, so
 * that acpid's action has run; report assertion `id`, that the reading is
 * then `want`.
 *
 * @return 1 when the scenario can go on.
 */
static int press(struct lc *l, struct etude_case_machine *m, uint64_t *at, const char *qcode,
                 const char *before, const char *want, const char *id)
{
    char arguments[96], command[256], got[64], detail[160];
    char *reply = NULL;
    enum etude_status st;

    snprintf(arguments, sizeof(arguments), "{\"keys\": [{\"type\": \"qcode\", \"data\": \"%s\"}]}",
             qcode);
    st = etude_qmp(m, "send-key", arguments, &reply);
    free(reply);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "QMP's send-key");
        return 0;
    }
    snprintf(command, sizeof(command),
             "n=0; while [ \"$(mixer)\" = '%s' ] && [ $n -lt 30 ]; do sleep 1; n=$((n+1)); "
             "done; echo VOL\"\"=$(mixer)\r",
             before);
    if (!ask(l, m, at, command, "VOL", got, sizeof(got)))
        return 0;
    snprintf(detail, sizeof(detail), "after %s mixer reads '%s', not '%s'", qcode, got, want);
    lc_assert(l, id, strcmp(got, want) == 0, detail);
    return 1;
}

/**
 * Log in; see acpid running; set the volume to 50 and press the volume keys
 * and the mute key twice; then press the power button and wait for the
 * machine's end.
 */
static void real(struct lc *l)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    char got[64], detail[160];
    uint64_t at = 0, from;
    char *reply = NULL;
    enum etude_status st;

    if (!lc_step(l, "boot") || (m = lc_boot(l, "real")) == NULL ||
        !lc_wait(l, m, "login:", &at, NULL) || !lc_type(l, m, "root\r") ||
        !lc_wait(l, m, "Password:", &at, NULL) || !type_password(l, m) ||
        !lc_wait(l, m, "# ", &at, NULL) || !lc_step(l, "volume"))
        return;
    if (!ask(l, m, &at, "echo ACPID\"\"=$(pidof acpid | wc -w)\r", "ACPID", got, sizeof(got)))
        return;
    snprintf(detail, sizeof(detail), "%s acpid processes run, not one", got);
    lc_assert(l, "running", strcmp(got, "1") == 0, detail);
    if (!ask(l, m, &at, "mixer vol 50; echo VOL\"\"=$(mixer)\r", "VOL", got, sizeof(got)))
        return;
    snprintf(detail, sizeof(detail), "mixer vol 50 reads '%s'", got);
    lc_assert(l, "set", strcmp(got, "vol 50:50") == 0, detail);
    if (!press(l, m, &at, "volumeup", "vol 50:50", "vol 55:55", "up") ||
        !press(l, m, &at, "volumedown", "vol 55:55", "vol 50:50", "down") ||
        !press(l, m, &at, "audiomute", "vol 50:50", "vol 0:0 muted", "mute") ||
        !press(l, m, &at, "audiomute", "vol 0:0 muted", "vol 50:50", "unmute") ||
        !lc_step(l, "power"))
        return;
    from = at;
    st = etude_qmp(m, "system_powerdown", NULL, &reply);
    free(reply);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "QMP's system_powerdown");
        return;
    }
    if (!lc_end(l, m, &end))
        return;
    lc_assert(l, "powered-off", end.end == ETUDE_END_SHUTDOWN,
              "the machine did not end by QEMU's SHUTDOWN event");
    lc_find(l, m, "in-order", "shutdown: Stopping the system", from, 1);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const ids[] = {"running", "set",         "up",       "down", "mute",
                                      "unmute",  "powered-off", "in-order", NULL};
    struct lc l;

    lc_begin(&l, c, ids);
    real(&l);
    return lc_finish(&l);
}
