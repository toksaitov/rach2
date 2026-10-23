/* admit.c: `etude admit`, which admits an old checkout or a new command. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "etude/bridge.h"
#include "etude/ledger.h"
#include "posix.h"
#include "../measurement_context.h"

#ifdef _WIN32
#include "../win/files.h"
#else
extern char **environ;
#endif
static volatile sig_atomic_t stopped;

/** Record cancellation for the executor's next turn. */
static void stop(int sig)
{
    stopped = sig;
}

/** Read three positive decimal amounts, with no signs or trailing text. */
static int amounts(const char *s, struct etude_ledger_res *r)
{
    static const char *const names[] = {"cpu_millis", "memory_bytes", "disk_bytes"};
    size_t i;

    for (i = 0; i < 3; i++) {
        uint64_t n = 0;
        if (*s < '0' || *s > '9')
            return -1;
        while (*s >= '0' && *s <= '9') {
            unsigned digit = (unsigned)(*s++ - '0');
            if (n > (UINT64_MAX - digit) / 10)
                return -1;
            n = n * 10 + digit;
        }
        if (n == 0 || (i < 2 ? *s++ != ',' : *s != '\0'))
            return -1;
        r[i].name = names[i];
        r[i].amount = n;
    }
    return 0;
}

/** Write the newly captured bytes of one output stream. */
static void output(const struct etude_output *o, size_t *seen, FILE *f)
{
    if (o->kept > *seen) {
        fwrite(o->data + *seen, 1, o->kept - *seen, f);
        fflush(f);
        *seen = o->kept;
    }
}

#ifdef _WIN32
/** Copy the command's vector with its program resolved on native PATH. */
static char **windows_command(char **argv)
{
    char program[4096], **out;
    size_t n = 0;

    if (etude_win_program(argv[0], program, sizeof(program)) != 0)
        return NULL;
    while (argv[n] != NULL)
        n++;
    out = calloc(n + 1, sizeof(*out));
    if (out == NULL)
        return NULL;
    out[0] = strdup(program);
    if (out[0] == NULL) {
        free(out);
        return NULL;
    }
    for (size_t i = 1; i < n; i++)
        out[i] = argv[i];
    return out;
}
#endif

/**
 * Run one command, with a registered process and a bound on its whole life.
 *
 * @return the command's exit status when it exited by itself, with its
 *         cleanup confirmed and its output whole, and -1 otherwise.
 */
static int execute(struct etude_ledger *l, const char *id, char **argv, unsigned seconds, int input,
                   enum etude_cleanup *cleanup)
{
    struct etude_exec *ex = etude_exec_new();
    struct etude_exec_spec spec;
    struct etude_proc *p;
    const struct etude_exec_result *r;
    char verdict[4096];
    uint64_t until = posix_now_us() + (uint64_t)seconds * 1000000;
    size_t out = 0, err = 0;
    int rc = -1, done = 0;
#ifdef _WIN32
    char **command = NULL;
#endif

    if (ex == NULL) {
        fprintf(stderr, "etude admit: executor: %s\n", strerror(errno));
        goto done;
    }
    if (etude_ledger_verdict_path(l, id, verdict, sizeof(verdict)) != 0) {
        fprintf(stderr, "etude admit: verdict: %s\n", strerror(errno));
        goto done;
    }
#ifdef _WIN32
    if ((command = windows_command(argv)) == NULL) {
        fprintf(stderr, "etude admit: start: %s: %s\n", argv[0], strerror(errno));
        goto done;
    }
#endif
    memset(&spec, 0, sizeof(spec));
    spec.stdin_path = input ? "-" : NULL;
    spec.attempt_id = id;
#ifdef _WIN32
    spec.argv = command;
#else
    spec.argv = argv;
#endif
    spec.envp = environ;
    spec.output_max = 64u << 20;
    spec.grace_us = 2000000;
    spec.reg = etude_ledger_register;
    spec.child = etude_ledger_child;
    spec.reg_ctx = l;
    spec.verdict = verdict;
    if (stopped) {
        fprintf(stderr, "etude admit: start: cancelled by signal %d\n", (int)stopped);
        goto done;
    }
    if (etude_exec_start(ex, &spec, &p) != 0) {
        fprintf(stderr, "etude admit: start: %s: %s\n", argv[0], strerror(errno));
        goto done;
    }
    while (!done) {
        if (stopped || posix_now_us() >= until)
            etude_exec_cancel(p);
        done = etude_exec_wait(ex, 100) == p;
        r = etude_proc_result(p);
        output(&r->out, &out, stdout);
        output(&r->err, &err, stderr);
    }
    r = etude_proc_result(p);
    *cleanup = r->cleanup;
    rc = r->end == ETUDE_END_EXITED ? r->value : -1;
    if (r->end == ETUDE_END_SIGNALED)
        fprintf(stderr, "etude admit: end: signal %d\n", r->value);
    else if (r->end == ETUDE_END_STOPPED) {
        if (stopped)
            fprintf(stderr, "etude admit: end: cancelled by signal %d\n", (int)stopped);
        else
            fprintf(stderr, "etude admit: end: time limit exceeded: %u seconds\n", seconds);
    } else if (r->end != ETUDE_END_EXITED)
        fprintf(stderr, "etude admit: end: lost\n");
    if (r->cleanup != ETUDE_CLEANUP_CONFIRMED) {
        fprintf(stderr, "etude admit: cleanup: %s\n",
                r->cleanup == ETUDE_CLEANUP_PENDING ? "pending" : "unconfirmed");
        rc = -1;
    }
    if (r->out.total > r->out.kept) {
        fprintf(stderr, "etude admit: output: stdout exceeded %zu bytes\n", spec.output_max);
        rc = -1;
    }
    if (r->err.total > r->err.kept) {
        fprintf(stderr, "etude admit: output: stderr exceeded %zu bytes\n", spec.output_max);
        rc = -1;
    }
done:
    if (ex != NULL)
        etude_exec_free(ex);
#ifdef _WIN32
    if (command != NULL) {
        free(command[0]);
        free(command);
    }
#endif
    return rc;
}

/** Print the command's arguments and return the usage status. */
static int usage(void)
{
    fprintf(stderr, "etude admit: usage: etude admit [-i] [-p] [-d locks] [-w seconds] [-r file] "
                    "ledger id account shared|alone cpu,bytes,bytes capacity seconds "
                    "command [args...]\n");
    return 2;
}

/** Read a positive command bound of at most a day, or return zero. */
static unsigned command_seconds(const char *text)
{
    char *end;
    unsigned long n;

    errno = 0;
    n = strtoul(text, &end, 10);
    if (errno || text[0] < '0' || text[0] > '9' || *end || n == 0 || n > 86400)
        return 0;
    return (unsigned)n;
}

/**
 * Write the bridge's own result to `path`, when -r named one, as one line:
 * `exited N`, `refused`, `incomplete` or `stopped N`.
 */
static void result(const char *path, const char *word, int n)
{
    FILE *f;

    if (path == NULL)
        return;
    f = fopen(path, "w");
    if (f == NULL || (n >= 0 ? fprintf(f, "%s %d\n", word, n) : fprintf(f, "%s\n", word)) < 0 ||
        fclose(f) != 0)
        fprintf(stderr, "etude admit: the result cannot be written to %s: %s\n", path,
                strerror(errno));
}

/** Record a refusal, or the stop signal that caused it, and give the status. */
static int refused(const char *record)
{
    result(record, stopped ? "stopped" : "refused", stopped ? stopped : -1);
    return stopped ? 128 + stopped : 2;
}

/**
 * Admit claim `id`, asking again each second for up to `seconds` while the
 * request waits in the ledger, and once without waiting when `seconds` is 0.
 *
 * A measured preparation, with ETUDE_PREPARATION_GRANT, reserves all three
 * supplied resources. Unmeasured compatibility commands reserve only memory;
 * their existing processor sharing and free-disk checks remain unchanged.
 *
 * @return 0 once admitted; -1 with errno set when refused, ETIMEDOUT at the
 *         bound and EINTR after a stop signal, with no request left waiting.
 */
static int admit(struct etude_ledger *l, const char *id, const char *account,
                 const struct etude_ledger_res *res, const struct etude_ledger_res *cap,
                 unsigned seconds)
{
    uint64_t until = posix_now_us() + (uint64_t)seconds * 1000000;
    int e;
    size_t first = getenv("ETUDE_PREPARATION_GRANT") ? 0 : 1;
    size_t count = first ? 1 : 3;

    while (etude_ledger_admit(l, id, account, res + first, count,
                              cap + first, count, seconds != 0) != 0) {
        e = errno;
        if (seconds != 0 && e == EAGAIN && !stopped && posix_now_us() < until) {
#ifdef _WIN32
            Sleep(1000);
#else
            sleep(1);
#endif
            continue;
        }
        if (seconds != 0 && (e == EAGAIN || stopped))
            e = stopped ? EINTR : ETIMEDOUT;
        if (seconds != 0)
            etude_ledger_withdraw(l, id);
        errno = e;
        return -1;
    }
    return 0;
}

/* What the command line asks for. */
struct request {
    const char *dir, *record, *ledger, *id, *account;
    unsigned wait, seconds; /* -w's bound and the command's, in seconds */
    int legacy, alone, input;
    struct etude_ledger_res res[3], cap[3];
    char **command;
};

/** Read the command line into `q`; 0, or -1 for a wrong usage. */
static int parse(int argc, char **argv, struct request *q)
{
    int opt;

    memset(q, 0, sizeof(*q));
#ifdef _WIN32
    q->dir = etude_win_temp();
#else
    q->dir = "/tmp";
#endif
    while ((opt = getopt(argc, argv, "+ipd:w:r:")) != -1) {
        switch (opt) {
        case 'i':
            q->input = 1;
            break;
        case 'p':
            q->legacy = 1;
            break;
        case 'd':
            q->dir = optarg;
            break;
        case 'w':
            q->wait = command_seconds(optarg);
            if (q->wait == 0)
                return -1;
            break;
        case 'r':
            q->record = optarg;
            break;
        default:
            return -1;
        }
    }
    argc -= optind;
    argv += optind;
    if (argc < 8 || amounts(argv[4], q->res) != 0 || amounts(argv[5], q->cap) != 0)
        return -1;
#ifdef _WIN32
    if (!etude_win_absolute(argv[0]) || !etude_win_absolute(q->dir))
#else
    if (argv[0][0] != '/' || q->dir[0] != '/')
#endif
        return -1;
    q->alone = strcmp(argv[3], "alone") == 0;
    if (!q->alone && strcmp(argv[3], "shared") != 0)
        return -1;
    q->alone = q->alone || q->res[1].amount > q->cap[1].amount / 2;
    q->seconds = command_seconds(argv[6]);
    q->ledger = argv[0];
    q->id = argv[1];
    q->account = argv[2];
    q->command = argv + 7;
    return q->seconds == 0 ? -1 : 0;
}

/* Record the first granted reading before locks or a child can delay it. */
static int grant_start(const char *claim)
{
    const char *path = getenv("ETUDE_PREPARATION_GRANT");
    struct etude_preparation_grant g;
    if (!path) return 0;
    if (etude_history_now(&g.start) || etude_context_clock(&g.clock)) return -1;
    return etude_context_grant_write(path, claim, &g);
}

/**
 * Admit the claim of `q` in `l` and take its locks, within -w's bound when
 * one was given, and set RACH2_PORT_OFFSET for a legacy port block.
 *
 * @return the holder of the locks, or NULL with the claim released and the
 *         reason on standard error.
 */
static struct etude_bridge *take(struct etude_ledger *l, const struct request *q)
{
    struct etude_bridge_spec bs = {q->ledger, q->id, q->dir, q->alone, q->legacy, 600000};
    struct etude_bridge *b;
    uint64_t began = posix_now_us(), spent;
    char offset[32];

    if (l == NULL || admit(l, q->id, q->account, q->res, q->cap, q->wait) != 0) {
        if (errno == ETIMEDOUT)
            fprintf(stderr, "etude admit: result: refused: admission: no room in %u seconds\n",
                    q->wait);
        else
            fprintf(stderr, "etude admit: result: refused: admission: %s\n", strerror(errno));
        return NULL;
    }
    if (grant_start(q->id)) {
        int error = errno;
        fprintf(stderr, "etude admit: result: refused: preparation grant: %s\n", strerror(error));
        if (etude_ledger_finish(l, q->id, ETUDE_CLEANUP_CONFIRMED))
            fprintf(stderr, "etude admit: preparation grant cleanup: %s\n", strerror(errno));
        errno = error;
        return NULL;
    }
    if (q->wait != 0) {
        spent = (posix_now_us() - began) / 1000;
        bs.wait_ms =
            spent < (uint64_t)q->wait * 1000 ? (unsigned)((uint64_t)q->wait * 1000 - spent) : 1;
    }
    b = etude_bridge_start(&bs);
    if (b == NULL) {
        fprintf(stderr, "etude admit: result: refused: locks: %s\n", strerror(errno));
        etude_ledger_finish(l, q->id, ETUDE_CLEANUP_CONFIRMED);
        return NULL;
    }
    if (q->legacy) {
        snprintf(offset, sizeof(offset), "%d", etude_bridge_offset(b));
#ifdef _WIN32
        _putenv_s("RACH2_PORT_OFFSET", offset);
        _putenv_s("RACH2_HOST_LOCK", "");
#else
        setenv("RACH2_PORT_OFFSET", offset, 1);
        unsetenv("RACH2_HOST_LOCK");
#endif
    }
    return b;
}

/** Admit one bounded command and retain its locks through confirmed cleanup. */
int etude_admit_main(int argc, char **argv)
{
    struct request q;
    struct etude_ledger *l;
    struct etude_bridge *b;
    enum etude_cleanup cleanup = ETUDE_CLEANUP_CONFIRMED;
#ifndef _WIN32
    struct sigaction sa;
#endif
    int rc;

    if (parse(argc, argv, &q) != 0)
        return usage();
    if (q.legacy && (getenv("RACH2_PORT_OFFSET") != NULL || getenv("SSH_TEST_PORT") != NULL)) {
        fprintf(stderr,
                "etude admit: result: refused: legacy port overrides bypass reservations\n");
        return refused(q.record);
    }
#ifdef _WIN32
    if (signal(SIGTERM, stop) == SIG_ERR || signal(SIGINT, stop) == SIG_ERR)
        return refused(q.record);
#else
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stop;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
#endif
    l = etude_ledger_open(q.ledger);
    b = take(l, &q);
    if (b == NULL) {
        etude_ledger_close(l);
        return refused(q.record);
    }
    rc = execute(l, q.id, q.command, q.seconds, q.input, &cleanup);
    if (etude_ledger_finish(l, q.id, cleanup) != 0)
        cleanup = ETUDE_CLEANUP_UNCONFIRMED;
    if (cleanup == ETUDE_CLEANUP_CONFIRMED)
        etude_bridge_join(b);
    else {
        fprintf(stderr, "etude admit: result: incomplete: locks retained by process %ld\n",
                etude_bridge_pid(b));
        etude_bridge_detach(b);
        rc = -1;
    }
    etude_ledger_close(l);
    if (stopped)
        result(q.record, "stopped", stopped);
    else if (rc < 0)
        result(q.record, "incomplete", -1);
    else
        result(q.record, "exited", rc);
    return stopped ? 128 + stopped : rc < 0 ? 2 : rc;
}
