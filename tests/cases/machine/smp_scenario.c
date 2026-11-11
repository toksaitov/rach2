/*
 * smp_scenario.c: the scenario of machine.smp, the kernel on several
 * processors. Cases `smpmusl-1`, `smpmusl-4` and `maxcpus-2` run smpmusl
 * with the processors each boot has; `preempt` and `usermode` run the debug
 * shell's preemption test and ring-3 payload on two; `nmi` raises an NMI on
 * both of two, which stops the machine as a fault of the machine, and
 * `exception` runs an invalid opcode in the kernel, each with panic=0 and
 * its report read on the screen after its panic; and `build` compiles the
 * ish sources four times over on the installed system with four, and checks
 * that every processor's user ticks grew.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* The VGA's text cells: 80 by 25, and two bytes each at 0xB8000. */
#define SCREEN_CELLS (80 * 25)

/* The test account's shell prompt on the installed system. */
static const char prompt[] = "$ ";

/*
 * The lines the build case types, each ending with a marker it prints
 * whatever it found. `b` compiles the ish sources four times over, every
 * compile at once, and `g` says whether every processor's user ticks grew
 * between the readings `before` and `after` of /proc/stat.
 */
static const char nproc_line[] = "nproc | grep -qx 4 && echo NPROC_\"\"OK; echo NPROC_\"\"END\r";
static const char b_line[] = "b() { for i in 1 2 3 4; do for f in ish ish_cstring_utilities "
                             "ish_shell_utilities ish_syscalls; do /bin/cc -c -o $i-$f.o "
                             "/src/ish/$f.c & done; done; wait; }; echo B_\"\"SET\r";
static const char g_line[] = "g() { awk '/^cpu[0-9]/ { if (FILENAME == \"before\") u[$1] = $2; "
                             "else if ($2 > u[$1]) grew++; else stalled++ } END { exit !(grew == "
                             "4 && !stalled) }' before after; }; echo G_\"\"SET\r";
static const char build_line[] =
    "cd && rm -rf smpbuild && mkdir smpbuild && cd smpbuild && cat /proc/stat > before && n=0 && "
    "while [ $n -lt 5 ]; do n=$((n+1)); b; cat /proc/stat > after; g && break; done; ls *.o | wc "
    "-l | grep -qx 16 && echo BUILD_\"\"DONE; echo BUILD_\"\"END\r";
static const char grew_line[] = "g && echo USER_TIME_\"\"GREW; echo GREW_\"\"END\r";

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/**
 * Report assertion `online` from the kernel's record `smp: processors: N
 * online of M` on the console of machine `m`: it holds when N and M are
 * both `cpus`, the processors the case's profile gives, or as many as its
 * boot line allows.
 */
static void online(struct lc *l, struct etude_case_machine *m, int cpus)
{
    uint64_t at = 0;
    char want[64], line[96], why[200];

    if (!lc_wait(l, m, "smp: processors: ", &at, NULL) || !lc_wait(l, m, "\n", &at, NULL))
        return;
    lc_text(l, "fixture.1.console", "smp: processors: ", 0, line, sizeof(line));
    snprintf(want, sizeof(want), "%d online of %d", cpus, cpus);
    snprintf(why, sizeof(why), "the kernel says %.96s, not %s", line, want);
    lc_assert(l, "online", strcmp(line, want) == 0, why);
}

/**
 * Run case `smpmusl-N` or `maxcpus-N`: boot the test image and run smpmusl
 * with `cpus`, the processors it must find. Assertions `online`, `exit` and
 * `said-ok`.
 */
static void smpmusl(struct lc *l, int cpus)
{
    struct etude_case_machine *m;
    uint64_t at;
    char command[64], ok[64], *w;

    snprintf(command, sizeof(command), "exec /musl/smpmusl %d\r", cpus);
    snprintf(ok, sizeof(ok), "SMPMUSL OK (%d processor%s)", cpus, cpus == 1 ? "" : "s");
    if ((m = lc_shell(l, "probe", "fixture", &at)) == NULL)
        return;
    online(l, m, cpus);
    w = lc_run(l, m, "fixture", command, &at, "exit");
    if (w != NULL) {
        lc_assert(l, "said-ok", strstr(w, ok) != NULL && strstr(w, "SMPMUSL FAIL") == NULL,
                  "smpmusl did not say SMPMUSL OK with its count alone");
        free(w);
    }
    lc_quit(l, m, NULL);
}

/**
 * Run case `preempt`: the debug shell's preemption test, whose two kernel
 * threads must each be preempted by the tick and run again. Assertions
 * `online`, `ended`, the test's counts printed and the shell's prompt back,
 * and `passed`.
 */
static void preempt(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at, from;
    char *w;

    if ((m = lc_shell(l, "preempt", "fixture", &at)) == NULL)
        return;
    online(l, m, 2);
    from = at;
    if (lc_type(l, m, "preempt\r") && lc_wait(l, m, "Counts: A=", &at, NULL) &&
        lc_wait(l, m, "\n> ", &at, "ended") && (w = lc_console(l, "fixture", from, at)) != NULL) {
        lc_assert(l, "passed",
                  strstr(w, "Preemptive scheduling test passed!") != NULL &&
                      strstr(w, "test FAILED") == NULL,
                  "the preemption test did not pass");
        free(w);
    }
    lc_quit(l, m, NULL);
}

/**
 * Run case `usermode`: the debug shell's ring-3 payload writes its message
 * through a system call and exits, and the shell comes back. Assertions
 * `online`, `exit` and `wrote`.
 */
static void usermode(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at;
    char *w;

    if ((m = lc_shell(l, "usermode", "fixture", &at)) == NULL)
        return;
    online(l, m, 2);
    w = lc_run(l, m, "fixture", "usermode\r", &at, "exit");
    if (w != NULL) {
        lc_assert(l, "wrote", strstr(w, "Hello from ring 3!") != NULL,
                  "the payload did not write from ring 3");
        free(w);
    }
    lc_quit(l, m, NULL);
}

/**
 * Read the VGA's text cells with QMP's pmemsave into `cells`, 80 by 25
 * characters run together and NUL-ended, so that a record that wraps is still
 * one string. A panic that does not scroll leaves row 0 at the adapter's
 * front. @return 1 when they were read.
 */
static int read_screen(struct lc *l, struct etude_case_machine *m, char *cells)
{
    unsigned char raw[SCREEN_CELLS * 2];
    char args[8300], path[4096], *reply = NULL;
    const char *ev = etude_evidence(l->c, "screen.vga");
    enum etude_status st;
    FILE *f;
    size_t i, n;

    if (ev == NULL || strlen(ev) >= sizeof(path)) {
        lc_halt(l, ETUDE_FAILED, "the screen's path");
        return 0;
    }
    strcpy(path, ev);
    n = (size_t)snprintf(args, sizeof(args), "{\"val\": 753664, \"size\": %d, \"filename\": \"",
                         SCREEN_CELLS * 2);
    for (i = 0; path[i] != '\0' && n < sizeof(args) - 8; i++) {
        if (path[i] == '\\' || path[i] == '"')
            args[n++] = '\\';
        args[n++] = path[i];
    }
    snprintf(args + n, sizeof(args) - n, "\"}");
    st = etude_qmp(m, "pmemsave", args, &reply);
    free(reply);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "QMP's pmemsave");
        return 0;
    }
    f = fopen(path, "rb");
    n = f != NULL ? fread(raw, 1, sizeof(raw), f) : 0;
    if (f != NULL)
        fclose(f);
    if (n != sizeof(raw)) {
        lc_halt(l, ETUDE_FAILED, "the read of the screen's memory");
        return 0;
    }
    for (i = 0; i < SCREEN_CELLS; i++)
        cells[i] = raw[i * 2] != '\0' ? (char)raw[i * 2] : ' ';
    cells[SCREEN_CELLS] = '\0';
    return 1;
}

/** The lines of one panic on the screen and in the log. */
struct panic_text {
    const char *fault;  /* the report's line that names the fault */
    const char *reason; /* the panic's reason, after `panic: ` */
    const char *report; /* the fault's record as the log has it */
};

static const struct panic_text nmi_text = {"non-maskable interrupt", "the machine reported a fault",
                                           "hardware fault: "};
static const struct panic_text exception_text = {"invalid opcode", "unrecoverable CPU exception",
                                                 "kernel exception: "};

/**
 * Find in `cells` the panic report's line `ip 0x` and a hex digit after it,
 * the instruction the fault was raised at. @return the line, or NULL when
 * the screen has none.
 */
static const char *report_ip(const char *cells)
{
    const char *at = strstr(cells, "ip 0x");

    if (at == NULL || at[5] == '\0' || strchr("0123456789abcdef", at[5]) == NULL)
        return NULL;
    return at;
}

/**
 * End a panicked machine: read its screen while it still runs, quit it, and
 * report `one-report` and `one-panic`, that no second `report` follows byte
 * `reported` and no second panic byte `panicked`. The screen holds the
 * panic's short report: the instruction pointer, the fault's name after it,
 * and `panic: ` with the reason last.
 */
static void read_panic_then_quit(struct lc *l, struct etude_case_machine *m,
                                 const struct panic_text *t, uint64_t reported, uint64_t panicked)
{
    const char *reason = t->reason, *report = t->report;
    char cells[SCREEN_CELLS + 1], detail[SCREEN_CELLS + 64];
    const char *ip, *name = NULL, *stopped = NULL;

    if (!read_screen(l, m, cells))
        return;
    ip = report_ip(cells);
    if (ip != NULL)
        name = strstr(ip, t->fault);
    if (name != NULL)
        stopped = strstr(name, "panic: ");
    snprintf(detail, sizeof(detail), "the screen has: %s", cells);
    lc_assert(l, "screen-cause", name != NULL, detail);
    lc_assert(l, "screen-stopped",
              stopped != NULL && strncmp(stopped + 7, reason, strlen(reason)) == 0, detail);
    if (lc_quit(l, m, NULL)) {
        lc_find(l, m, "one-report", report, reported, 0);
        lc_find(l, m, "one-panic", "panic: stopped: ", panicked, 0);
    }
}

/**
 * Run case `nmi`: QMP's inject-nmi raises an NMI on both processors at the
 * debug shell. The first to take it reports a fault of the machine and
 * panics, and the other stops without a word; the panic is recorded as the
 * boot's end, which `etude.fault` judges against the machine's declared
 * fault. Assertions `online`, `reported`, `panicked`, `halted`, those of
 * `read_panic_then_quit`.
 */
static void nmi(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at, reported, panicked;
    enum etude_status st;
    char *reply = NULL;

    if ((m = lc_shell(l, "nmi", "fixture", &at)) == NULL)
        return;
    online(l, m, 2);
    if (!lc_ok(l))
        return;
    st = etude_qmp(m, "inject-nmi", NULL, &reply);
    free(reply);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "QMP's inject-nmi");
        return;
    }
    reported = at;
    if (!lc_wait(l, m, "isr: hardware fault: non-maskable interrupt", &reported, "reported"))
        return;
    panicked = reported;
    if (!lc_wait(l, m, "panic: stopped: the machine reported a fault", &panicked, "panicked"))
        return;
    st = etude_fault(m, ETUDE_END_PANIC);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "the record of the kernel's panic");
        return;
    }
    lc_halted(l, m, 2, "halted");
    if (!lc_ok(l))
        return;
    read_panic_then_quit(l, m, &nmi_text, reported, panicked);
}

/**
 * Run case `exception`: the debug shell's `badop` runs an invalid opcode in
 * the kernel, which is a kernel exception, and the machine stops as in
 * `nmi`. Assertions as `nmi`'s.
 */
static void exception(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at, reported, panicked;
    enum etude_status st;

    if ((m = lc_shell(l, "exception", "fixture", &at)) == NULL)
        return;
    online(l, m, 2);
    if (!lc_ok(l) || !lc_type(l, m, "badop\r"))
        return;
    reported = at;
    if (!lc_wait(l, m, "isr: kernel exception: invalid opcode", &reported, "reported"))
        return;
    panicked = reported;
    if (!lc_wait(l, m, "panic: stopped: unrecoverable CPU exception", &panicked, "panicked"))
        return;
    st = etude_fault(m, ETUDE_END_PANIC);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "the record of the kernel's panic");
        return;
    }
    lc_halted(l, m, 2, "halted");
    if (!lc_ok(l))
        return;
    read_panic_then_quit(l, m, &exception_text, reported, panicked);
}

/**
 * Read the first line of artifact `name`, a password, into `buf` of `size`
 * bytes followed by a carriage return, as it is typed. @return 1 when it
 * was read.
 */
static int password(struct lc *l, const char *name, char *buf, size_t size)
{
    const char *path = etude_artifact(l->c, name);
    FILE *f = path != NULL ? fopen(path, "r") : NULL;
    size_t n;

    if (f == NULL || fgets(buf, (int)size - 1, f) == NULL || (n = strcspn(buf, "\r\n")) == 0) {
        if (f != NULL)
            fclose(f);
        lc_halt(l, ETUDE_FAILED, "the read of the test account's password");
        return 0;
    }
    fclose(f);
    buf[n] = '\r';
    buf[n + 1] = '\0';
    return 1;
}

/**
 * Type `line` at the shell and wait for `end`, which the line prints last,
 * whatever it found.
 *
 * @return the console from the typing to `end`, NUL-ended, which the caller
 *         frees; or NULL when the scenario cannot go on.
 */
static char *command(struct lc *l, struct etude_case_machine *m, uint64_t *at, const char *line,
                     const char *end)
{
    uint64_t from = *at;

    if (!lc_type(l, m, line) || !lc_wait(l, m, end, at, NULL))
        return NULL;
    return lc_console(l, "fixture", from, *at);
}

/**
 * Report assertion `id`, when not NULL, from `w`, what a command printed: it
 * holds when `token` is there, and fails with `why` otherwise. Each token is
 * written split in the command, `A_""B`, so that its echo is not taken for
 * it. `w` is freed.
 *
 * @return 1 when there was a window to judge.
 */
static int check(struct lc *l, char *w, const char *token, const char *id, const char *why)
{
    if (w == NULL)
        return 0;
    if (id != NULL)
        lc_assert(l, id, strstr(w, token) != NULL, why);
    free(w);
    return 1;
}

/**
 * Run case `build`: boot the installed system with four processors, log in
 * at the console as the test account, require nproc to count four, and
 * compile the staged ish sources four times over, every compile at once,
 * between two readings of /proc/stat. A compile is a few milliseconds, so a
 * processor can end its share of a round between two ticks; the build runs
 * again, up to five rounds, until every processor has counted a user tick.
 * Each assertion is made in the guest, over the command's own output. The
 * build and its check are shell functions, since the console keeps 255 bytes
 * of a typed line. Assertions `online`, `login`, `nproc`, `sixteen` and
 * `grew`.
 */
static void build(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at = 0;
    char secret[256];

    if (!lc_step(l, "boot") || (m = lc_boot(l, "fixture")) == NULL ||
        !lc_wait(l, m, "login:", &at, NULL))
        return;
    online(l, m, 4);
    if (!lc_step(l, "login") || !lc_type(l, m, "test\r") ||
        !lc_wait(l, m, "Password:", &at, NULL) ||
        !password(l, "test-password", secret, sizeof(secret)) || !lc_type(l, m, secret) ||
        !lc_wait(l, m, prompt, &at, "login"))
        return;
    if (!lc_step(l, "nproc") || !check(l, command(l, m, &at, nproc_line, "NPROC_END"), "NPROC_OK",
                                       "nproc", "nproc does not count four processors"))
        return;
    if (!lc_step(l, "build") || !check(l, command(l, m, &at, b_line, "B_SET"), "", NULL, NULL) ||
        !check(l, command(l, m, &at, g_line, "G_SET"), "", NULL, NULL) ||
        !check(l, command(l, m, &at, build_line, "BUILD_END"), "BUILD_DONE", "sixteen",
               "the parallel build did not make its sixteen objects"))
        return;
    if (lc_step(l, "grew") &&
        check(l, command(l, m, &at, grew_line, "GREW_END"), "USER_TIME_GREW", "grew",
              "a processor's user ticks did not grow in five rounds of the parallel build"))
        lc_quit(l, m, NULL);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const smpmusl_ids[] = {"online", "exit", "said-ok", NULL};
    static const char *const preempt_ids[] = {"online", "ended", "passed", NULL};
    static const char *const usermode_ids[] = {"online", "exit", "wrote", NULL};
    static const char *const nmi_ids[] = {"online",     "reported",     "panicked",
                                          "halted",     "screen-cause", "screen-stopped",
                                          "one-report", "one-panic",    NULL};
    static const char *const build_ids[] = {"online", "login", "nproc", "sixteen", "grew", NULL};
    const char *name = case_name(c);
    struct lc l;

    if (strncmp(name, "smpmusl-", 8) == 0 || strncmp(name, "maxcpus-", 8) == 0) {
        lc_begin(&l, c, smpmusl_ids);
        smpmusl(&l, atoi(name + 8));
    } else if (strcmp(name, "preempt") == 0) {
        lc_begin(&l, c, preempt_ids);
        preempt(&l);
    } else if (strcmp(name, "usermode") == 0) {
        lc_begin(&l, c, usermode_ids);
        usermode(&l);
    } else if (strcmp(name, "nmi") == 0) {
        lc_begin(&l, c, nmi_ids);
        nmi(&l);
    } else if (strcmp(name, "exception") == 0) {
        lc_begin(&l, c, nmi_ids);
        exception(&l);
    } else if (strcmp(name, "build") == 0) {
        lc_begin(&l, c, build_ids);
        build(&l);
    } else {
        return 1;
    }
    return lc_finish(&l);
}
