/*
 * acpid_scenario.c: the scenario of the acpid case (../machine.acpid.json).
 * os-real on an Intel HDA controller with one software Wi-Fi radio, logged
 * in as root: BusyBox's acpid, which init starts with the sbin directories
 * on its path, runs mixer for the volume keys QMP presses on the PS/2
 * keyboard, while the ordinary user test holds an exclusive lock on
 * /dev/mixer, which neither the keys nor init's save of the levels wait
 * on; its airplane action run in its environment blocks and unblocks the
 * radio, and it powers the machine off in order for the ACPI power button
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

/* The machine of the case's one boot, and the offset its console has been
 * read to. */
struct guest {
    struct lc *l;
    struct etude_case_machine *m;
    uint64_t at;
};

/**
 * Type `command`, which prints `NAME=` and a value last, the name written
 * with an empty pair of quotes inside it so that the line's echo is not
 * taken for it; wait for the value's line and put the value in `buf`, of
 * `size` bytes.
 *
 * @return 1 when the value came.
 */
static int ask(struct guest *g, const char *command, const char *name, char *buf, size_t size)
{
    char key[32];
    uint64_t from = g->at;

    snprintf(key, sizeof(key), "%s=", name);
    return lc_type(g->l, g->m, command) && lc_wait(g->l, g->m, key, &g->at, NULL) &&
           lc_wait(g->l, g->m, "\n", &g->at, NULL) && lc_text(g->l, CONSOLE, key, from, buf, size);
}

/**
 * Press the key `qcode` through QMP, then wait at the shell for the mark of
 * acpid's action: mixer's reading is no longer `before`, and the action's
 * lock, /var/run/acpi.lock, is free, so that the action has ended. Report
 * assertion `id`, that the reading is then `want`. A key that changes
 * nothing stalls the case, which the runner ends.
 *
 * @return 1 when the scenario can go on.
 */
static int press(struct guest *g, const char *qcode, const char *before, const char *want,
                 const char *id)
{
    char arguments[96], command[256], got[64], detail[160];
    char *reply = NULL;
    enum etude_status st;

    snprintf(arguments, sizeof(arguments), "{\"keys\": [{\"type\": \"qcode\", \"data\": \"%s\"}]}",
             qcode);
    st = etude_qmp(g->m, "send-key", arguments, &reply);
    free(reply);
    if (st != ETUDE_OK) {
        lc_halt(g->l, st, "QMP's send-key");
        return 0;
    }
    /* interval: poll mixer's reading every second */
    snprintf(command, sizeof(command),
             "while [ \"$(mixer vol)\" = '%s' ]; do sleep 1; done; "
             "flock /var/run/acpi.lock true; echo VOL\"\"=$(mixer vol)\r",
             before);
    if (!ask(g, command, "VOL", got, sizeof(got)))
        return 0;
    snprintf(detail, sizeof(detail), "after %s mixer reads '%s', not '%s'", qcode, got, want);
    lc_assert(g->l, id, strcmp(got, want) == 0, detail);
    return 1;
}

/* The line that starts the ordinary user test holding an exclusive lock on
 * /dev/mixer in the background, until the power button ends it, and prints
 * whether a second exclusive lock of test's is refused, as it is while the
 * first is held. */
#define HOLD                                                                      \
    "setuidgid test flock -x /dev/mixer sleep 3600 </dev/null >/dev/null 2>&1 & " \
    "until ! setuidgid test flock -n /dev/mixer true; do sleep 1; done; echo HOLD\"\"=held\r"

/* The line that waits until init has saved the muted volume, the level 0
 * and the record of 50, and then prints the modes of the two locks and
 * whether test can open either. */
#define SAVED                                                                                    \
    "until grep -qx 'vol 0:0' /var/lib/mixer && grep -qx 'mute 50:50' /var/lib/mixer; do sleep " \
    "1; "                                                                                        \
    "done; echo SAVED\"\"=$(stat -c %a /var/run/acpi.lock /var/run/mixer.lock | tr '\\n' ' ')"   \
    "$(setuidgid test flock -n /var/run/acpi.lock true 2>/dev/null || echo no)"                  \
    "$(setuidgid test flock -n /var/run/mixer.lock true 2>/dev/null || echo no)\r"

/* The path init gives every service, BusyBox init's (utils/init.c). */
#define SERVICE_PATH "/sbin:/usr/sbin:/bin:/usr/bin"

/* The soft block of the Wi-Fi switches, one digit for each, as
 * /etc/acpi/wifi reads them. */
#define SOFT_BLOCKS                             \
    "$(for s in /sys/class/rfkill/rfkill*; do " \
    "[ \"$(cat $s/type)\" = wlan ] && cat $s/soft; done)"

/**
 * Report assertion `id`, that the PATH in the environment of the one
 * process named `name` is SERVICE_PATH.
 *
 * @return 1 when the scenario can go on.
 */
static int service_path(struct guest *g, const char *name, const char *id)
{
    char command[256], got[128], detail[256];

    snprintf(command, sizeof(command),
             "echo P\"\"=$(tr '\\0' '\\n' < /proc/$(pidof %s)/environ | sed -n 's,^PATH=,,p')\r",
             name);
    if (!ask(g, command, "P", got, sizeof(got)))
        return 0;
    snprintf(detail, sizeof(detail), "%s's PATH is '%s', not '%s'", name, got, SERVICE_PATH);
    lc_assert(g->l, id, strcmp(got, SERVICE_PATH) == 0, detail);
    return 1;
}

/**
 * Run acpid's airplane action, /etc/acpi/wifi, in acpid's own environment
 * and nothing else, and report assertion `id`, that the radio's switch then
 * reads soft block `want`.
 *
 * @return 1 when the scenario can go on.
 */
static int airplane(struct guest *g, const char *want, const char *id)
{
    char got[64], detail[160];

    if (!ask(g,
             "env -i $(tr '\\0' ' ' < /proc/$(pidof acpid)/environ) /etc/acpi/wifi; "
             "echo SOFT\"\"=$? " SOFT_BLOCKS "\r",
             "SOFT", got, sizeof(got)))
        return 0;
    snprintf(detail, sizeof(detail), "the action's status and soft block are '%s', not '0 %s'", got,
             want);
    lc_assert(g->l, id, got[0] == '0' && got[1] == ' ' && strcmp(got + 2, want) == 0, detail);
    return 1;
}

/**
 * Log in; see acpid running; set the volume to 50; have test hold
 * /dev/mixer, and press the volume keys and the mute key twice, reading
 * init's save of the mute and the locks' modes; read acpid's path, and run
 * the airplane action twice in acpid's environment; then press the power
 * button and wait for the machine's end.
 */
static void real(struct lc *l)
{
    struct etude_termination end;
    struct guest g;
    char got[64], detail[160];
    uint64_t from;
    char *reply = NULL;
    enum etude_status st;

    memset(&g, 0, sizeof(g));
    g.l = l;
    if (!lc_step(l, "boot") || (g.m = lc_boot(l, "real")) == NULL ||
        !lc_wait(l, g.m, "login:", &g.at, NULL) || !lc_type(l, g.m, "root\r") ||
        !lc_wait(l, g.m, "Password:", &g.at, NULL) || !type_password(l, g.m) ||
        !lc_wait(l, g.m, "# ", &g.at, NULL) || !lc_step(l, "volume"))
        return;
    if (!ask(&g, "echo ACPID\"\"=$(pidof acpid | wc -w)\r", "ACPID", got, sizeof(got)))
        return;
    snprintf(detail, sizeof(detail), "%s acpid processes run, not one", got);
    lc_assert(l, "running", strcmp(got, "1") == 0, detail);
    if (!ask(&g, "mixer vol 50; echo VOL\"\"=$(mixer vol)\r", "VOL", got, sizeof(got)))
        return;
    snprintf(detail, sizeof(detail), "mixer vol 50 reads '%s'", got);
    lc_assert(l, "set", strcmp(got, "vol 50:50") == 0, detail);
    if (!ask(&g, HOLD, "HOLD", got, sizeof(got)))
        return;
    lc_assert(l, "held", strcmp(got, "held") == 0, "test does not hold /dev/mixer");
    if (!press(&g, "volumeup", "vol 50:50", "vol 55:55", "up") ||
        !press(&g, "volumedown", "vol 55:55", "vol 50:50", "down") ||
        !press(&g, "audiomute", "vol 50:50", "vol 0:0 muted", "mute"))
        return;
    if (!ask(&g, SAVED, "SAVED", got, sizeof(got)))
        return;
    snprintf(detail, sizeof(detail),
             "the locks' modes and test's opens are '%s', not '600 600 nono'", got);
    lc_assert(l, "saved", strcmp(got, "600 600 nono") == 0, detail);
    if (!press(&g, "audiomute", "vol 0:0 muted", "vol 50:50", "unmute") || !lc_step(l, "wifi"))
        return;
    if (!service_path(&g, "acpid", "acpid-path"))
        return;
    if (!ask(&g, "echo SOFT\"\"=" SOFT_BLOCKS "\r", "SOFT", got, sizeof(got)))
        return;
    snprintf(detail, sizeof(detail), "the Wi-Fi switches' soft blocks are '%s', not '0'", got);
    lc_assert(l, "radio", strcmp(got, "0") == 0, detail);
    if (!airplane(&g, "1", "blocked") || !airplane(&g, "0", "unblocked") || !lc_step(l, "power"))
        return;
    from = g.at;
    st = etude_qmp(g.m, "system_powerdown", NULL, &reply);
    free(reply);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "QMP's system_powerdown");
        return;
    }
    if (!lc_end(l, g.m, &end))
        return;
    lc_assert(l, "powered-off", end.end == ETUDE_END_SHUTDOWN,
              "the machine did not end by QEMU's SHUTDOWN event");
    lc_find(l, g.m, "in-order", "shutdown: Stopping the system", from, 1);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const ids[] = {"running", "set",       "held",        "up",         "down",
                                      "mute",    "saved",     "unmute",      "acpid-path", "radio",
                                      "blocked", "unblocked", "powered-off", "in-order",   NULL};
    struct lc l;

    lc_begin(&l, c, ids);
    real(&l);
    return lc_finish(&l);
}
