/*
 * etudeexec_scenario.c: the scenario of the etudeexec case
 * (../machine.etudeexec.json): Rach2 builds etude by its TinyCC recipe from an
 * archive of its source on a second disk, and the POSIX executor, built the
 * same way, passes its test there.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* The results etudeexec_guest.sh gives, each a line `etudeexec: NAME: ok`
 * when it passes; each is the assertion of its name. */
static const char *const rows[] = {"built",   "reused",       "platform", "recipe",  "casecheck",
                                   "history", "history-lost", "compile",  "executor"};

/* The programs of etude's whose missing system calls the boot's trace names
 * with `(asked for by NAME)`. */
static const char *const programs[] = {"execcheck", "casecheck", "history_check"};

/**
 * Read the first line of artifact `demo-password` into `buf` of `size` bytes,
 * followed by a carriage return, as it is typed. @return 1 when it was read.
 */
static int password(struct lc *l, char *buf, size_t size)
{
    const char *path = etude_artifact(l->c, "demo-password");
    FILE *f = path != NULL ? fopen(path, "r") : NULL;
    size_t n;

    if (f == NULL || fgets(buf, (int)size - 1, f) == NULL || (n = strcspn(buf, "\r\n")) == 0) {
        if (f != NULL)
            fclose(f);
        lc_halt(l, ETUDE_FAILED, "the read of the demo account's password");
        return 0;
    }
    fclose(f);
    buf[n] = '\r';
    buf[n + 1] = '\0';
    return 1;
}

/**
 * Boot the machine and log root in at its console: assertion `login`.
 * @return the machine, with the console read to the prompt in `*at`, or NULL
 *         when the scenario cannot go on.
 */
static struct etude_case_machine *login(struct lc *l, uint64_t *at)
{
    struct etude_case_machine *m;
    char secret[256];

    *at = 0;
    if (!lc_step(l, "boot") || (m = lc_boot(l, "rach2")) == NULL ||
        !lc_wait(l, m, "rach2 login:", at, NULL) || !lc_step(l, "login") ||
        !lc_type(l, m, "root\r") || !lc_wait(l, m, "Password:", at, NULL) ||
        !password(l, secret, sizeof(secret)) || !lc_type(l, m, secret) ||
        !lc_wait(l, m, "# ", at, "login"))
        return NULL;
    return m;
}

/**
 * Unpack the archive from the second disk, which has no partition table, into
 * a tmpfs, since the version cache publishes by a hard link and KRFFS has
 * none: assertion `unpacked`. @return 1 when it was unpacked.
 */
static int unpack(struct lc *l, struct etude_case_machine *m, uint64_t *at)
{
    static const char line[] =
        "mkdir /tmp/etudeexec && mount -t tmpfs tmpfs /tmp/etudeexec && "
        "tar -xf /dev/disk1 -C /tmp/etudeexec && echo UNPACK\"\"ED; echo END\"\"_UNPACK\r";
    uint64_t from = *at;
    char *w;
    int held;

    if (!lc_step(l, "unpack") || !lc_type(l, m, line) || !lc_wait(l, m, "END_UNPACK", at, NULL) ||
        (w = lc_console(l, "rach2", from, *at)) == NULL)
        return 0;
    held = strstr(w, "UNPACKED") != NULL;
    free(w);
    lc_assert(l, "unpacked", held, "the archive did not unpack from /dev/disk1");
    return held && lc_wait(l, m, "# ", at, NULL);
}

/**
 * Say whether console text `w` has the line `etudeexec: ROW: ok` and no line
 * that says the row failed, and otherwise put the script's reason in `why`.
 */
static int row_ok(const char *w, const char *row, char *why, size_t size)
{
    char ok[64], bad[64];
    const char *at;
    size_t n;

    snprintf(ok, sizeof(ok), "\netudeexec: %s: ok\r", row);
    snprintf(bad, sizeof(bad), "\netudeexec: %s: fail: ", row);
    if ((at = strstr(w, bad)) != NULL) {
        at += strlen(bad);
        n = strcspn(at, "\r\n");
        snprintf(why, size, "%s: %.*s", row, (int)(n < 120 ? n : 120), at);
        return 0;
    }
    if (strstr(w, ok) != NULL)
        return 1;
    snprintf(why, size, "the script gave no result for %s", row);
    return 0;
}

/**
 * Report assertion `enosys`: the trace of the system calls the kernel lacks,
 * on for this boot, names none that an etude program asked for in console
 * text `w`.
 */
static void enosys(struct lc *l, const char *w)
{
    char pattern[64], why[160] = "";
    const char *at = NULL;
    size_t i, n;

    for (i = 0; i < sizeof(programs) / sizeof(programs[0]) && at == NULL; i++) {
        snprintf(pattern, sizeof(pattern), "(asked for by %s)", programs[i]);
        at = strstr(w, pattern);
    }
    if (at != NULL) {
        while (at > w && at[-1] != '\n')
            at--;
        n = strcspn(at, "\r\n");
        snprintf(why, sizeof(why), "%.*s", (int)(n < 150 ? n : 150), at);
    }
    lc_assert(l, "enosys", at == NULL, why);
}

/**
 * Run etudeexec_guest.sh from the archive under the quiet witness `guest`,
 * and report one assertion for each of its results, and `enosys`.
 */
static void build(struct lc *l, struct etude_case_machine *m, uint64_t *at)
{
    static const char line[] =
        "sh /tmp/etudeexec/tests/cases/machine/etudeexec_guest.sh /tmp/etudeexec 2>&1\r";
    uint64_t from = *at;
    char why[160], *w;
    size_t i;

    if (!lc_step(l, "build"))
        return;
    etude_progress(l->c, "guest", 0);
    if (!lc_type(l, m, line) || !lc_wait(l, m, "etudeexec: end", at, NULL) ||
        (w = lc_console(l, "rach2", from, *at)) == NULL)
        return;
    etude_progress(l->c, "guest", 1);
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
        lc_assert(l, rows[i], row_ok(w, rows[i], why, sizeof(why)), why);
    free(w);
    if ((w = lc_console(l, "rach2", 0, *at)) == NULL)
        return;
    enosys(l, w);
    free(w);
    if (lc_wait(l, m, "# ", at, NULL))
        lc_quit(l, m, NULL);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const ids[] = {
        "login",   "unpacked",     "built",   "reused",   "platform", "recipe", "casecheck",
        "history", "history-lost", "compile", "executor", "enosys",   NULL};
    struct etude_case_machine *m;
    uint64_t at;
    struct lc l;

    lc_begin(&l, c, ids);
    if ((m = login(&l, &at)) != NULL && unpack(&l, m, &at))
        build(&l, m, &at);
    return lc_finish(&l);
}
