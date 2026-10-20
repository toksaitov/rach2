/* etude_bridge.c: admit an old checkout or a new command. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "etude/bridge.h"
#include "etude/ledger.h"
#include "posix.h"

extern char **environ;
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

/** Run one command, with a registered process and a bound on its whole life. */
static int execute(struct etude_ledger *l, const char *id, char **argv, unsigned seconds,
                   enum etude_cleanup *cleanup)
{
    struct etude_exec *ex = etude_exec_new();
    struct etude_exec_spec spec;
    struct etude_proc *p;
    const struct etude_exec_result *r;
    char verdict[4096];
    uint64_t until = posix_now_us() + (uint64_t)seconds * 1000000;
    size_t out = 0, err = 0;
    int rc = 2, done = 0;

    if (ex == NULL || etude_ledger_verdict_path(l, id, verdict, sizeof(verdict)) != 0)
        goto done;
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = id;
    spec.argv = argv;
    spec.envp = environ;
    spec.output_max = 64u << 20;
    spec.grace_us = 2000000;
    spec.reg = etude_ledger_register;
    spec.child = etude_ledger_child;
    spec.reg_ctx = l;
    spec.verdict = verdict;
    if (stopped || etude_exec_start(ex, &spec, &p) != 0)
        goto done;
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
    rc = r->end == ETUDE_END_EXITED ? r->value : 2;
    if (r->cleanup != ETUDE_CLEANUP_CONFIRMED || r->out.total > r->out.kept ||
        r->err.total > r->err.kept)
        rc = 2;
done:
    if (ex != NULL)
        etude_exec_free(ex);
    return rc;
}

/** Print the command's arguments and return the usage status. */
static int usage(void)
{
    fprintf(stderr, "etude-bridge: usage: etude-bridge [-p] [-d locks] ledger id account "
                    "shared|alone cpu,bytes,bytes capacity seconds command [args...]\n");
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

/** Admit one bounded command and retain its locks through confirmed cleanup. */
int main(int argc, char **argv)
{
    struct etude_ledger_res res[3], cap[3];
    struct etude_ledger *l;
    struct etude_bridge *b;
    struct etude_bridge_spec bs;
    enum etude_cleanup cleanup = ETUDE_CLEANUP_CONFIRMED;
    const char *dir = "/tmp";
    char offset[32];
    unsigned seconds;
    int opt, legacy = 0, alone, rc;
    struct sigaction sa;

    while ((opt = getopt(argc, argv, "+pd:")) != -1) {
        if (opt == 'p')
            legacy = 1;
        else if (opt == 'd')
            dir = optarg;
        else
            return usage();
    }
    argc -= optind;
    argv += optind;
    if (argc < 8 || argv[0][0] != '/' || dir[0] != '/' || amounts(argv[4], res) != 0 ||
        amounts(argv[5], cap) != 0)
        return usage();
    alone = strcmp(argv[3], "alone") == 0;
    if (!alone && strcmp(argv[3], "shared") != 0)
        return usage();
    alone = alone || res[1].amount > cap[1].amount / 2;
    seconds = command_seconds(argv[6]);
    if (seconds == 0)
        return usage();
    if (legacy && (getenv("RACH2_PORT_OFFSET") != NULL || getenv("SSH_TEST_PORT") != NULL)) {
        fprintf(stderr,
                "etude-bridge: result: refused: legacy port overrides bypass reservations\n");
        return 2;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stop;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    l = etude_ledger_open(argv[0]);
    if (l == NULL || etude_ledger_admit(l, argv[1], argv[2], res, 3, cap, 3, 0) != 0) {
        fprintf(stderr, "etude-bridge: result: refused: admission: %s\n", strerror(errno));
        etude_ledger_close(l);
        return 2;
    }
    bs.ledger = argv[0];
    bs.id = argv[1];
    bs.dir = dir;
    bs.alone = alone;
    bs.legacy = legacy;
    bs.wait_ms = 600000;
    b = etude_bridge_start(&bs);
    if (b == NULL) {
        fprintf(stderr, "etude-bridge: result: refused: locks: %s\n", strerror(errno));
        etude_ledger_finish(l, argv[1], ETUDE_CLEANUP_CONFIRMED);
        etude_ledger_close(l);
        return 2;
    }
    if (legacy) {
        snprintf(offset, sizeof(offset), "%d", etude_bridge_offset(b));
        setenv("RACH2_PORT_OFFSET", offset, 1);
        unsetenv("RACH2_HOST_LOCK");
    }
    rc = execute(l, argv[1], argv + 7, (unsigned)seconds, &cleanup);
    if (etude_ledger_finish(l, argv[1], cleanup) != 0)
        cleanup = ETUDE_CLEANUP_UNCONFIRMED;
    if (cleanup == ETUDE_CLEANUP_CONFIRMED)
        etude_bridge_join(b);
    else {
        fprintf(stderr, "etude-bridge: result: incomplete: locks retained by process %ld\n",
                etude_bridge_pid(b));
        etude_bridge_detach(b);
        rc = 2;
    }
    etude_ledger_close(l);
    return stopped ? 128 + stopped : rc;
}
