/*
 * exitline_scenario.c: the scenario of the exitline cases
 * (../machine.exitline.json). The kernel prints the plain process-exit line
 * only where the debug shell is home, and the records that hand the shell
 * back are whole lines; one boot of each case.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* The plain line, which `sys_exit` prints only under the debug shell. */
static const char plain[] = "process: exited with status";

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/** Say whether `p` begins with one or more digits, and give the end. */
static int digits(const char *p, const char **end)
{
    const char *q = p;

    while (*q >= '0' && *q <= '9')
        q++;
    *end = q;
    return q != p;
}

/**
 * Say whether some line of `w` is the kernel's record `pattern` whole: the
 * prefix `<L>[S.U] `, then the pattern, in which each `#` is a run of
 * digits, and then the line's end. The probe runner matches the whole line,
 * so a record renamed at either end would stop being scored.
 */
static int whole_line(const char *w, const char *pattern)
{
    const char *line, *next, *p, *q;

    for (line = w; line != NULL; line = (next = strchr(line, '\n')) != NULL ? next + 1 : NULL) {
        if (line[0] != '<' || !digits(line + 1, &q) || q[0] != '>' || q[1] != '[' ||
            !digits(q + 2, &q) || q[0] != '.' || !digits(q + 1, &q) || q[0] != ']' || q[1] != ' ')
            continue;
        p = q + 2;
        for (q = pattern; *q != '\0'; q++)
            if (*q == '#' ? !digits(p, &p) : *p++ != *q)
                break;
        if (*q == '\0' && (*p == '\r' || *p == '\n'))
            return 1;
    }
    return 0;
}

/** Log in at the console of `m` as root with the case's password. */
static int login(struct lc *l, struct etude_case_machine *m, uint64_t *at)
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
    return lc_wait(l, m, "login:", at, NULL) && lc_type(l, m, "root\r") &&
           lc_wait(l, m, "Password:", at, NULL) && lc_type(l, m, line) &&
           lc_wait(l, m, "# ", at, NULL);
}

/**
 * On os-real, logged in, run BusyBox by its path, which forks, execs and
 * exits, and then a builtin, which runs once the child is reaped: the
 * program's output must come, and the plain line must not. The tokens are
 * typed split, since the console echoes what is typed.
 */
static void quiet(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at = 0;

    if (!lc_step(l, "boot") || (m = lc_boot(l, "real")) == NULL || !login(l, m, &at) ||
        !lc_step(l, "run") ||
        !lc_type(l, m, "/bin/busybox echo FORKED\"\"-AND-EXITED; echo REAPED\"\"-OK\r"))
        return;
    lc_wait(l, m, "FORKED-AND-EXITED", &at, NULL);
    lc_wait(l, m, "REAPED-OK", &at, "ran");
    if (lc_quit(l, m, NULL))
        lc_find(l, m, "quiet", plain, 0, 0);
}

/**
 * Under the debug shell, forktest's child exits with 42 and the probe then
 * with 0: the plain line comes for each, named by its status, and the record
 * that hands the shell back is whole.
 */
static void fork_case(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at = 0, from;
    char *w;

    if ((m = lc_shell(l, "boot", "guest", &at)) == NULL || !lc_step(l, "run"))
        return;
    from = at;
    if (!lc_type(l, m, "exec /forktest\n") ||
        (w = lc_handback(l, m, "guest", from, &at, "handback")) == NULL)
        return;
    free(w);
    if (!lc_wait(l, m, "\n", &at, NULL) || (w = lc_console(l, "guest", from, at)) == NULL)
        return;
    lc_assert(l, "child", whole_line(w, "process: exited with status 42"),
              "no whole plain line gives the child's status 42");
    lc_assert(l, "probe", whole_line(w, "process: exited with status 0"),
              "no whole plain line gives the probe's status 0");
    lc_assert(
        l, "handback-whole",
        whole_line(w, "sys_exit: init process # exited with status 0, returning to kernel shell"),
        "the record that hands the shell back is not the whole line the probe runner reads");
    free(w);
    lc_quit(l, m, NULL);
}

/**
 * Under the debug shell, vmfault writes through a wild pointer: the record of
 * its death by signal 11, which scores a killed probe, is a whole line.
 */
static void signal_case(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at = 0, from;
    char *w;

    if ((m = lc_shell(l, "boot", "guest", &at)) == NULL || !lc_step(l, "run"))
        return;
    from = at;
    if (!lc_type(l, m, "exec /vmfault\n") ||
        !lc_wait(l, m, "killed by signal 11, returning to kernel shell", &at, "killed") ||
        !lc_wait(l, m, "\n", &at, NULL) || (w = lc_console(l, "guest", from, at)) == NULL)
        return;
    lc_assert(
        l, "marker",
        whole_line(w, "terminate: init process # killed by signal 11, returning to kernel shell"),
        "the record of the kill is not the whole line the probe runner reads");
    free(w);
    lc_quit(l, m, NULL);
}

/**
 * On the harness kernel, which ends QEMU through its isa-debug-exit port when
 * init exits, hello's exit must end QEMU, and the plain line, from which a
 * reader with no token takes the status, must give 0. QEMU 10 reports a
 * write to the port as a guest's shutdown, whose QMP event has no status; an
 * older QEMU exits without the event, with the status 1 the port gives for 0.
 */
static void harness_case(struct lc *l)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    uint64_t at = 0, from;
    char detail[120], *w;

    if ((m = lc_shell(l, "boot", "harness", &at)) == NULL || !lc_step(l, "run"))
        return;
    from = at;
    if (!lc_type(l, m, "exec /hello\n") || !lc_end(l, m, &end))
        return;
    snprintf(detail, sizeof(detail), "QEMU ended %s with %d, not by the port",
             end.end == ETUDE_END_QEMU_EXITED ? "by its exit" : "otherwise", end.value);
    lc_assert(l, "port",
              end.end == ETUDE_END_SHUTDOWN || (end.end == ETUDE_END_QEMU_EXITED && end.value == 1),
              detail);
    if ((w = lc_console(l, "harness", from, end.console_offset)) == NULL)
        return;
    lc_assert(l, "plain", whole_line(w, "process: exited with status 0"),
              "the harness kernel printed no whole plain line with hello's status 0");
    free(w);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const quiet_ids[] = {"ran", "quiet", NULL};
    static const char *const fork_ids[] = {"handback", "child", "probe", "handback-whole", NULL};
    static const char *const signal_ids[] = {"killed", "marker", NULL};
    static const char *const harness_ids[] = {"port", "plain", NULL};
    const char *name = case_name(c);
    struct lc l;

    if (strcmp(name, "quiet") == 0) {
        lc_begin(&l, c, quiet_ids);
        quiet(&l);
    } else if (strcmp(name, "fork") == 0) {
        lc_begin(&l, c, fork_ids);
        fork_case(&l);
    } else if (strcmp(name, "signal") == 0) {
        lc_begin(&l, c, signal_ids);
        signal_case(&l);
    } else {
        lc_begin(&l, c, harness_ids);
        harness_case(&l);
    }
    return lc_finish(&l);
}
