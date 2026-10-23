/**
 * Check etude's POSIX executor (tools/etude/posix/) with this program as
 * the case's program, run with `--case MODE TOKEN`.
 *
 * host-deps: tools/etude/capture.c tools/etude/posix/exec.c
 * host-deps: tools/etude/posix/keeper.c tools/etude/posix/process.c
 * host-cflags: -I tools/etude/include -Wall -Wextra -Werror
 *
 * Eleven attempts run at once: exits with two statuses, a death by a
 * signal, three cancellations (of a process that obeys SIGTERM, of one that
 * ignores it, and of one whose child is stopped by SIGSTOP and must be
 * resumed to run its handler), a descendant in a session of its own that the
 * program reports, one left in its group, a tree that uses processor time
 * and memory, output past the bound kept with a report after it, and a
 * program that checks its descriptors, signals and session. Each must keep
 * its own attempt, process, output and end, and leave no process behind. On
 * Linux and Rach2 each case's process must be seen held before its exec when
 * it is registered. With `posix_only`, a reported descendant must still be
 * ended, an unreported one in its own session must leave the cleanup
 * unconfirmed, and processor time of an orphan must be a lower bound; on
 * Linux and Rach2, without it, the keeper must end and count both. A build
 * for Rach2 defines __rach2__, as etude's recipe there does. A refused
 * registration and a failed exec must leave nothing running. A keeper killed
 * during registration must leave its held child ended by EOF; one killed
 * after exec must give a lost end. A controller that ends must have its case
 * ended by the keeper. A case that holds memory and has used processor time
 * must be sampled with
 * both while it runs on Linux and Rach2, and with neither elsewhere, and not
 * once it has ended.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__) || defined(__rach2__)
#include <sys/prctl.h>
#endif

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#include "compat/pathmax_host.h"
#include "compat/fixdir_host.c"
#include "etude/exec.h"

extern char **environ;

static int fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                        \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            printf("etude-exec-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), #cond); \
            fails++;                                                                            \
        }                                                                                       \
    } while (0)

/* The processor time and memory the `burn` and `orphan` cases use. */
#define BURN_US    200000u
#define BURN_BYTES (64u << 20)

/* This program, by an absolute path. */
static char self[PATH_MAX];

/* ---- The case's program. ---- */

/** Write `text` with one write(2), as a report record must be written. */
static void say(const char *text)
{
    if (write(1, text, strlen(text)) < 0)
        _exit(120);
}

/** Use processor time until this process has used `us` microseconds. */
static void burn(uint64_t us)
{
    volatile unsigned long spin = 0;
    struct rusage ru;
    uint64_t used = 0;

    while (used < us) {
        for (spin = 0; spin < 100000; spin++) {
        }
        getrusage(RUSAGE_SELF, &ru);
        used = (uint64_t)ru.ru_utime.tv_sec * 1000000u + (uint64_t)ru.ru_utime.tv_usec +
               (uint64_t)ru.ru_stime.tv_sec * 1000000u + (uint64_t)ru.ru_stime.tv_usec;
    }
}

/** Wait for signals forever. */
static void idle(void)
{
    for (;;)
        pause();
}

/**
 * Start a descendant that waits forever, in a session of its own when
 * `alone` is set, and return its PID once it is in place.
 */
static pid_t descendant(int alone)
{
    int sync[2];
    pid_t pid;
    char c;

    if (pipe(sync) != 0 || (pid = fork()) < 0)
        _exit(121);
    if (pid == 0) {
        if (alone)
            setsid();
        if (write(sync[1], "s", 1) != 1)
            _exit(122);
        idle();
    }
    close(sync[1]);
    if (read(sync[0], &c, 1) != 1)
        _exit(123);
    close(sync[0]);
    return pid;
}

/**
 * Print the PID of a descendant on descriptor `fd`, reported as a child on
 * standard output when `report` is set.
 */
static void name_descendant(const char *token, pid_t pid, int report, int fd)
{
    char line[128];

    if (report) {
        snprintf(line, sizeof(line), "@etude 1 child %ld\n", (long)pid);
        say(line);
    }
    snprintf(line, sizeof(line), "%s pid %ld\n", token, (long)pid);
    if (write(fd, line, strlen(line)) < 0)
        _exit(120);
}

/** Write `count` bytes of x and a newline to standard output. */
static void flood(long count)
{
    char buf[4096];
    long n;

    memset(buf, 'x', sizeof(buf));
    for (; count > 0; count -= n) {
        n = count < (long)sizeof(buf) ? count : (long)sizeof(buf);
        if (write(1, buf, (size_t)n) != n)
            _exit(124);
    }
    say("\n");
}

/** Use memory and, in a child that is waited for, processor time. */
static void burn_tree(void)
{
    char *mem = malloc(BURN_BYTES);
    pid_t pid;

    if (mem == NULL)
        _exit(125);
    memset(mem, 1, BURN_BYTES);
    pid = fork();
    if (pid == 0) {
        burn(BURN_US);
        _exit(0);
    }
    if (pid < 0 || waitpid(pid, NULL, 0) != pid)
        _exit(126);
    free(mem);
}

/**
 * Use processor time in a grandchild orphaned before it starts, and return
 * once it has finished, as the end of a pipe it holds shows.
 */
static void burn_orphan(void)
{
    int done[2];
    pid_t pid;
    char c;

    if (pipe(done) != 0 || (pid = fork()) < 0)
        _exit(121);
    if (pid == 0) {
        if (fork() == 0) {
            close(done[0]);
            burn(BURN_US);
            _exit(0);
        }
        _exit(0);
    }
    close(done[1]);
    waitpid(pid, NULL, 0);
    while (read(done[0], &c, 1) > 0) {
    }
}

static const char *case_token;

/** Say that SIGTERM ran its handler, and exit 7. */
static void on_term(int sig)
{
    (void)sig;
    say(case_token);
    say(" term\n");
    _exit(7);
}

/** Say `TOKEN ready`, the line the controller waits for before it cancels. */
static void ready(const char *token)
{
    char line[128];

    snprintf(line, sizeof(line), "%s ready\n", token);
    say(line);
}

/** Hold BURN_BYTES, use BURN_US of processor time, say ready, and wait. */
static void keep_busy(const char *token)
{
    char *mem = malloc(BURN_BYTES);

    if (mem == NULL)
        _exit(125);
    memset(mem, 1, BURN_BYTES);
    burn(BURN_US);
    ready(token);
    idle();
}

/**
 * Start a child that handles SIGTERM and stops itself, say ready once it is
 * seen stopped, and exit with the child's status. This process ignores
 * SIGTERM, so it ends by SIGKILL unless the child is resumed to run its
 * handler.
 */
static int stopped_child(const char *token)
{
    pid_t pid;
    int st;

    signal(SIGTERM, SIG_IGN);
    pid = fork();
    if (pid == 0) {
        signal(SIGTERM, on_term);
        raise(SIGSTOP);
        idle();
    }
    if (pid < 0 || waitpid(pid, &st, WUNTRACED) != pid || !WIFSTOPPED(st))
        return 1;
    ready(token);
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st))
        return 1;
    return WEXITSTATUS(st);
}

/**
 * Check that the case starts as the executor promises: no descriptor beyond
 * the three, no signal blocked, SIGPIPE at its default though the controller
 * ignores it, and a session of its own.
 */
static int check_start(void)
{
    struct sigaction sa;
    sigset_t mask;
    int fd, bad = 0;

    for (fd = 3; fd < 1024; fd++)
        if (fcntl(fd, F_GETFD) >= 0) {
            fprintf(stderr, "descriptor %d is open\n", fd);
            bad = 1;
        }
    sigprocmask(SIG_SETMASK, NULL, &mask);
    if (sigismember(&mask, SIGTERM) || sigismember(&mask, SIGUSR1))
        bad = fprintf(stderr, "signals are blocked\n");
    sigaction(SIGPIPE, NULL, &sa);
    if (sa.sa_handler != SIG_DFL)
        bad = fprintf(stderr, "SIGPIPE is not at its default\n");
    if (getsid(0) != getpid())
        bad = fprintf(stderr, "the case leads no session of its own\n");
    return bad != 0;
}

/** Run as the case's program in mode `mode`, with `token` naming it. */
static int run_case(const char *mode, const char *token, const char *arg)
{
    char line[128];

    case_token = token;
    snprintf(line, sizeof(line), "%s out\n", token);
    say(line);
    if (strcmp(mode, "say") == 0) {
        fprintf(stderr, "%s err\n", token);
        return atoi(arg);
    } else if (strcmp(mode, "signal") == 0) {
        raise(SIGUSR1);
    } else if (strcmp(mode, "hang") == 0 || strcmp(mode, "deaf") == 0) {
        if (strcmp(mode, "deaf") == 0)
            signal(SIGTERM, SIG_IGN);
        ready(token);
        idle();
    } else if (strcmp(mode, "stop") == 0) {
        return stopped_child(token);
    } else if (strcmp(mode, "daemon") == 0 || strcmp(mode, "escape") == 0 ||
               strcmp(mode, "group") == 0) {
        name_descendant(token, descendant(strcmp(mode, "group") != 0), strcmp(mode, "daemon") == 0,
                        1);
    } else if (strcmp(mode, "flood") == 0) {
        flood(atol(arg));
        name_descendant(token, descendant(1), 1, 2);
    } else if (strcmp(mode, "burn") == 0) {
        burn_tree();
    } else if (strcmp(mode, "orphan") == 0) {
        burn_orphan();
    } else if (strcmp(mode, "mark") == 0) {
        return open(arg, O_WRONLY | O_CREAT, 0644) < 0;
    } else if (strcmp(mode, "clean") == 0) {
        return check_start();
    } else if (strcmp(mode, "keep") == 0) {
        keep_busy(token);
    } else if (strcmp(mode, "hold") == 0) {
        if (open(arg, O_WRONLY) < 0)
            return 1;
        ready(token);
        idle();
    }
    return 0;
}

/* ---- The controller. ---- */

/* What one attempt of the test runs, and what it must end as. */
struct want {
    const char *mode;
    const char *token;
    const char *arg;
    uint64_t grace_us;
    int cancel;         /* cancel once the case says it is ready */
    enum etude_end end; /* the end and value the result must give */
    int value;
    enum etude_end exit_end; /* the end and value of the process itself */
    int exit_value;
    enum etude_cleanup cleanup;
};

/* One attempt of the test. */
struct attempt {
    struct want w;
    char attempt_id[32];
    char *argv[6];
    struct etude_proc *p;
    int cancelled;
    int done;
};

/* The registrations the executor made, in order. */
struct registration {
    char attempt_id[32];
    long keeper;
    long process;
    int held; /* as held() found the process when it was registered */
};

static struct registration regs[64];
static int nregs;

#if defined(__linux__) || defined(__rach2__)

/** Say whether the command line of `pid` names a case, or it has none left. */
static int ran_case(long pid)
{
    char path[64], buf[4096];
    size_t n, i;
    FILE *f;

    snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
    f = fopen(path, "r");
    if (f == NULL)
        return 1;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    for (i = 0; i < n; i++)
        if (buf[i] == '\0')
            buf[i] = ' ';
    buf[n] = '\0';
    return n == 0 || strstr(buf, "--case") != NULL;
}

#endif

#if defined(__linux__)

/**
 * Say whether process `pid` is held before its exec: seen blocked in
 * read(2), which only the wait for the keeper's go does before the exec,
 * while its command line is still this program's own. Waits until it is in
 * one state or has run.
 *
 * @return 1 when held, 0 when it ran first, -1 when Linux will not say.
 */
static int held(long pid)
{
    char path[64], buf[256];
    long nr;
    size_t n;
    FILE *f;

    snprintf(path, sizeof(path), "/proc/%ld/syscall", pid);
    for (;;) {
        f = fopen(path, "r");
        if (f == NULL)
            return errno == EACCES || errno == EPERM ? -1 : 0;
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        if (sscanf(buf, "%ld", &nr) == 1 && nr == SYS_read)
            return !ran_case(pid);
        if (ran_case(pid))
            return 0;
    }
}

#elif defined(__rach2__)

/** Give the state letter of `pid` from /proc/PID/stat, or 0 for none. */
static char state_of(long pid)
{
    char path[64], buf[256], *p;
    size_t n;
    FILE *f;

    snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
    f = fopen(path, "r");
    if (f == NULL)
        return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    p = strrchr(buf, ')');
    return p != NULL && p[1] == ' ' ? p[2] : 0;
}

/**
 * Say whether process `pid` is held before its exec: seen asleep while its
 * command line is still this program's own. Rach2 has no /proc/PID/syscall;
 * before its exec the case's process sleeps only in the read(2) that waits
 * for the keeper's go. Waits until it is in one state or has run.
 *
 * @return 1 when held, 0 when it ran first.
 */
static int held(long pid)
{
    for (;;) {
        if (ran_case(pid))
            return 0;
        if (state_of(pid) == 'S')
            return !ran_case(pid);
    }
}

#else

static int held(long pid)
{
    (void)pid;
    return -1;
}

#endif

/**
 * Record a registration; `ctx`, when not NULL, is an error to refuse it
 * with.
 */
static int record(void *ctx, const char *attempt_id, const struct etude_pid *keeper,
                  const struct etude_pid *process)
{
    struct registration *r = &regs[nregs < 64 ? nregs++ : 63];

    snprintf(r->attempt_id, sizeof(r->attempt_id), "%s", attempt_id);
    r->keeper = keeper->pid;
    r->process = process->pid;
    r->held = held(process->pid);
    return ctx != NULL ? *(const int *)ctx : 0;
}

/** Give the registration of attempt `id`, or NULL; it must be the only one. */
static const struct registration *registered(const char *id)
{
    const struct registration *found = NULL;
    int i, count = 0;

    for (i = 0; i < nregs; i++)
        if (strcmp(regs[i].attempt_id, id) == 0) {
            found = &regs[i];
            count++;
        }
    return count == 1 ? found : NULL;
}

/** Fill `s` to run attempt `a`. */
static void spec_for(struct etude_exec_spec *s, struct attempt *a, int posix_only)
{
    snprintf(a->attempt_id, sizeof(a->attempt_id), "attempt-%s", a->w.token);
    a->argv[0] = self;
    a->argv[1] = "--case";
    a->argv[2] = (char *)a->w.mode;
    a->argv[3] = (char *)a->w.token;
    a->argv[4] = (char *)(a->w.arg != NULL ? a->w.arg : "0");
    a->argv[5] = NULL;
    memset(s, 0, sizeof(*s));
    s->attempt_id = a->attempt_id;
    s->argv = a->argv;
    s->envp = environ;
    s->output_max = 4096;
    s->grace_us = a->w.grace_us;
    s->reg = record;
    s->posix_only = posix_only;
}

/**
 * Start the `n` attempts of `a` at once, cancel each that asks for it once
 * it is ready, and wait until all have finished.
 */
static void run_all(struct etude_exec *ex, const struct want *w, struct attempt *a, int n,
                    int posix_only)
{
    struct etude_exec_spec s;
    struct etude_proc *p;
    int i, left = 0;

    memset(a, 0, (size_t)n * sizeof(*a));
    for (i = 0; i < n; i++) {
        a[i].w = w[i];
        spec_for(&s, &a[i], posix_only);
        a[i].p = NULL;
        CHECK(etude_exec_start(ex, &s, &a[i].p) == 0, a[i].w.token);
        left += a[i].p != NULL;
    }
    while (left > 0) {
        p = etude_exec_wait(ex, -1);
        for (i = 0; i < n; i++) {
            if (a[i].p == NULL)
                continue;
            if (a[i].p == p) {
                a[i].done = 1;
                left--;
            }
            if (a[i].w.cancel && !a[i].cancelled &&
                strstr(etude_proc_result(a[i].p)->out.data, "ready") != NULL) {
                a[i].cancelled = 1;
                etude_exec_cancel(a[i].p);
            }
        }
    }
}

/** Give the PID a case printed as `TOKEN pid N` on either stream, or -1. */
static long printed_pid(const struct etude_exec_result *r, const char *token)
{
    char key[64];
    const char *at;

    snprintf(key, sizeof(key), "%s pid ", token);
    at = strstr(r->out.data, key);
    if (at == NULL)
        at = strstr(r->err.data, key);
    return at == NULL ? -1 : atol(at + strlen(key));
}

/** Check that the result of attempt `a` is its own and ends as it must. */
static void check_attempt(const struct attempt *a)
{
    const struct etude_exec_result *r = etude_proc_result(a->p);
    const struct registration *reg = registered(a->attempt_id);
    struct etude_pid id;
    char line[64];

    snprintf(line, sizeof(line), "%s out\n", a->w.token);
    CHECK(a->done, a->w.token);
    CHECK(strcmp(r->attempt_id, a->attempt_id) == 0, a->w.token);
    CHECK(strncmp(r->out.data, line, strlen(line)) == 0, a->w.token);
    CHECK(reg != NULL && reg->process == r->process.pid && reg->keeper == r->keeper.pid,
          a->w.token);
    CHECK(r->process.start_known && r->process.pid != r->keeper.pid, a->w.token);
    CHECK(r->end == a->w.end && r->value == a->w.value, a->w.token);
    CHECK(r->exit_end == a->w.exit_end && r->exit_value == a->w.exit_value, a->w.token);
    CHECK(r->cleanup == a->w.cleanup, a->w.token);
    CHECK(r->started_us <= r->ended_us && r->ended_us <= r->finished_us, a->w.token);
    if (a->w.cleanup == ETUDE_CLEANUP_CONFIRMED) {
        CHECK(r->out.closed && r->err.closed, a->w.token);
        CHECK(etude_process_state(r->process.pid, &id) == 0, a->w.token);
    }
}

/** Check that the descendant a case printed has ended. */
static void check_gone(const struct attempt *a)
{
    struct etude_pid id;
    long pid = printed_pid(etude_proc_result(a->p), a->w.token);

    CHECK(pid > 0 && etude_process_state(pid, &id) == 0, a->w.token);
}

#define LONG_GRACE  60000000u
#define SHORT_GRACE 100000u

#define EXITED    ETUDE_END_EXITED
#define SIGNALED  ETUDE_END_SIGNALED
#define STOPPED   ETUDE_END_STOPPED
#define CONFIRMED ETUDE_CLEANUP_CONFIRMED

/** Check the processor time and memory of the `burn` case. */
static void check_usage(const struct attempt *a, enum etude_quality cpu_quality)
{
    const struct etude_exec_result *r = etude_proc_result(a->p);

    CHECK(r->cpu_us.quality == cpu_quality && r->cpu_us.value >= BURN_US, a->w.token);
    CHECK(r->peak_bytes.quality == ETUDE_QUALITY_LOWER_BOUND, a->w.token);
    CHECK(r->peak_bytes.value >= BURN_BYTES, a->w.token);
}

/** Run eleven attempts at once and check that each keeps its own result. */
static void test_concurrent(struct etude_exec *ex)
{
    struct want w[] = {
        {"say", "a", "0", LONG_GRACE, 0, EXITED, 0, EXITED, 0, CONFIRMED},
        {"say", "b", "3", LONG_GRACE, 0, EXITED, 3, EXITED, 3, CONFIRMED},
        {"signal", "c", NULL, LONG_GRACE, 0, SIGNALED, SIGUSR1, SIGNALED, SIGUSR1, CONFIRMED},
        {"hang", "d", NULL, LONG_GRACE, 1, STOPPED, -1, SIGNALED, SIGTERM, CONFIRMED},
        {"deaf", "e", NULL, SHORT_GRACE, 1, STOPPED, -1, SIGNALED, SIGKILL, CONFIRMED},
        {"stop", "f", NULL, LONG_GRACE, 1, STOPPED, -1, EXITED, 7, CONFIRMED},
        {"daemon", "g", NULL, LONG_GRACE, 0, EXITED, 0, EXITED, 0, CONFIRMED},
        {"group", "h", NULL, LONG_GRACE, 0, EXITED, 0, EXITED, 0, CONFIRMED},
        {"burn", "i", NULL, LONG_GRACE, 0, EXITED, 0, EXITED, 0, CONFIRMED},
        {"flood", "j", "100000", LONG_GRACE, 0, EXITED, 0, EXITED, 0, CONFIRMED},
        {"clean", "k", NULL, LONG_GRACE, 0, EXITED, 0, EXITED, 0, CONFIRMED},
    };
    struct attempt a[sizeof(w) / sizeof(w[0])];
    int n = (int)(sizeof(w) / sizeof(w[0])), i, j;
    const struct etude_exec_result *r;

    run_all(ex, w, a, n, 0);
    for (i = 0; i < n; i++) {
        if (a[i].p == NULL)
            continue;
        check_attempt(&a[i]);
        for (j = 0; j < i; j++)
            CHECK(a[j].p == NULL || etude_proc_result(a[j].p)->process.pid !=
                                        etude_proc_result(a[i].p)->process.pid,
                  a[i].w.token);
    }
    if (a[0].p != NULL) {
        r = etude_proc_result(a[0].p);
        CHECK(strcmp(r->out.data, "a out\n") == 0 && strcmp(r->err.data, "a err\n") == 0, "a");
    }
    if (a[5].p != NULL)
        CHECK(strstr(etude_proc_result(a[5].p)->out.data, "f term") != NULL, "f");
    for (i = 6; i <= 9; i++)
        if (a[i].p != NULL && i != 8)
            check_gone(&a[i]);
    if (a[6].p != NULL)
        CHECK(etude_proc_result(a[6].p)->children == 1, "g");
    if (a[7].p != NULL)
        CHECK(etude_proc_result(a[7].p)->children == 0, "h");
#if defined(__linux__) || defined(__rach2__)
    if (a[8].p != NULL)
        check_usage(&a[8], ETUDE_QUALITY_EXACT);
#else
    if (a[8].p != NULL)
        check_usage(&a[8], ETUDE_QUALITY_LOWER_BOUND);
#endif
    if (a[9].p != NULL) {
        r = etude_proc_result(a[9].p);
        CHECK(r->children == 1 && r->foreign == 0, "j");
        CHECK(r->out.kept == 4096 && r->out.total > 100000, "j");
    }
    for (i = 0; i < n; i++)
        if (a[i].p != NULL)
            etude_proc_free(a[i].p);
}

/**
 * Run, with POSIX interfaces alone, a reported descendant, an unreported one
 * in a session of its own, and an orphan that uses processor time.
 */
static void test_posix_only(struct etude_exec *ex)
{
    struct want w[] = {
        {"daemon", "p", NULL, LONG_GRACE, 0, EXITED, 0, EXITED, 0, CONFIRMED},
        {"escape", "q", NULL, SHORT_GRACE, 0, EXITED, 0, EXITED, 0, ETUDE_CLEANUP_UNCONFIRMED},
        {"orphan", "r", NULL, LONG_GRACE, 0, EXITED, 0, EXITED, 0, CONFIRMED},
    };
    const struct etude_exec_result *r;
    struct attempt a[3];
    long pid;
    int i;

    run_all(ex, w, a, 3, 1);
    for (i = 0; i < 3; i++)
        if (a[i].p != NULL)
            check_attempt(&a[i]);
    if (a[0].p != NULL)
        check_gone(&a[0]);
    if (a[1].p != NULL) {
        pid = printed_pid(etude_proc_result(a[1].p), "q");
        CHECK(pid > 0, "q");
        if (pid > 0)
            kill((pid_t)pid, SIGKILL);
    }
    if (a[2].p != NULL) {
        r = etude_proc_result(a[2].p);
        CHECK(r->cpu_us.quality == ETUDE_QUALITY_LOWER_BOUND, "r");
    }
    for (i = 0; i < 3; i++)
        if (a[i].p != NULL)
            etude_proc_free(a[i].p);
}

/**
 * Run, where the keeper reaps orphans, an unreported descendant in a session
 * of its own and an orphan that uses processor time: the keeper must end
 * the first and count the second.
 */
static void test_reaper(struct etude_exec *ex)
{
#if defined(__linux__) || defined(__rach2__)
    struct want w[] = {
        {"escape", "s", NULL, LONG_GRACE, 0, EXITED, 0, EXITED, 0, CONFIRMED},
        {"orphan", "t", NULL, LONG_GRACE, 0, EXITED, 0, EXITED, 0, CONFIRMED},
    };
    const struct etude_exec_result *r;
    struct attempt a[2];
    int i;

    run_all(ex, w, a, 2, 0);
    for (i = 0; i < 2; i++)
        if (a[i].p != NULL)
            check_attempt(&a[i]);
    if (a[0].p != NULL)
        check_gone(&a[0]);
    if (a[1].p != NULL) {
        r = etude_proc_result(a[1].p);
        CHECK(r->cpu_us.quality == ETUDE_QUALITY_EXACT && r->cpu_us.value >= BURN_US, "t");
    }
    for (i = 0; i < 2; i++)
        if (a[i].p != NULL)
            etude_proc_free(a[i].p);
#else
    (void)ex;
    printf("etude_exec_hosttest: not run: the keeper as the reaper of orphans: "
           "this host has no subreaper\n");
#endif
}

/**
 * Start an attempt that must not run: refused by its registration, or with
 * a program that does not exist. Neither its program nor its process may be
 * left.
 */
static void test_unrun(struct etude_exec *ex)
{
    struct want w = {"mark", "u", NULL, LONG_GRACE, 0, EXITED, 0, EXITED, 0, CONFIRMED};
    const struct registration *reg;
    struct etude_exec_spec s;
    struct etude_proc *p = NULL;
    struct etude_pid id;
    struct attempt a;
    struct stat st;
    int refuse = EPERM;

    memset(&a, 0, sizeof(a));
    a.w = w;
    a.w.arg = fix_path("mark");
    spec_for(&s, &a, 0);
    s.reg_ctx = &refuse;
    CHECK(etude_exec_start(ex, &s, &p) == -1 && errno == EPERM && p == NULL, "refused");
    CHECK(stat(a.w.arg, &st) != 0 && errno == ENOENT, "refused");
    reg = registered(a.attempt_id);
    CHECK(reg != NULL && reg->held != 0 && etude_process_state(reg->process, &id) == 0, "refused");

    a.w.token = "v";
    spec_for(&s, &a, 0);
    a.argv[0] = "/nonexistent/etude_exec_hosttest";
    CHECK(etude_exec_start(ex, &s, &p) == -1 && errno == ENOENT && p == NULL, "no program");
    reg = registered(a.attempt_id);
    CHECK(reg != NULL && reg->held != 0 && etude_process_state(reg->process, &id) == 0,
          "no program");
}

/** Record a held case, kill its keeper, and refuse the registration. */
static int kill_at_registration(void *ctx, const char *attempt_id, const struct etude_pid *keeper,
                                const struct etude_pid *process)
{
    record(ctx, attempt_id, keeper, process);
    CHECK(kill((pid_t)keeper->pid, SIGKILL) == 0, "held keeper killed");
    return EIO;
}

/**
 * Kill the keeper before it releases the held child. The child must end on
 * EOF without executing the program. Reap it here where subreapers exist;
 * otherwise the host's init reaps it. A failure ends the child explicitly.
 */
static void test_held_keeper(struct etude_exec *ex)
{
    struct want w = {"mark", "held", NULL, LONG_GRACE, 0, EXITED, 0, EXITED, 0, CONFIRMED};
    const struct registration *reg;
    struct etude_exec_spec s;
    struct etude_proc *p = NULL;
    struct etude_pid id;
    struct attempt a;
    struct stat st;
    int gone = 0;
#if defined(__linux__) || defined(__rach2__)
    int subreaper = 0, status = 0;
    pid_t reaped;
#else
    struct timespec pause = {0, 1000000};
#endif

#if defined(__linux__) || defined(__rach2__)
    CHECK(prctl(PR_GET_CHILD_SUBREAPER, &subreaper, 0, 0, 0) == 0, "held adoption");
    CHECK(prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0, "held adoption");
#endif

    memset(&a, 0, sizeof(a));
    a.w = w;
    a.w.arg = fix_path("held-mark");
    spec_for(&s, &a, 0);
    s.reg = kill_at_registration;
    CHECK(etude_exec_start(ex, &s, &p) == -1 && errno == EIO && p == NULL, "held start");
    reg = registered(a.attempt_id);
    CHECK(reg != NULL && reg->held != 0, "held registration");
    if (reg != NULL) {
#if defined(__linux__) || defined(__rach2__)
        while ((reaped = waitpid((pid_t)reg->process, &status, 0)) < 0 && errno == EINTR) {
        }
        gone = reaped == (pid_t)reg->process;
#else
        while (!(gone = etude_process_state(reg->process, &id) == 0))
            /* interval: the child is checked every 1 ms */
            nanosleep(&pause, NULL);
#endif
        CHECK(gone, "held child EOF");
#if defined(__linux__) || defined(__rach2__)
        if (gone)
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 127, "held child exit");
#endif
        if (!gone) {
            kill((pid_t)reg->process, SIGKILL);
            while (waitpid((pid_t)reg->process, NULL, 0) < 0 && errno == EINTR) {
            }
        }
        CHECK(etude_process_state(reg->process, &id) == 0, "held child gone");
        CHECK(etude_process_state(reg->keeper, &id) == 0, "held keeper gone");
    }
    CHECK(stat(a.w.arg, &st) != 0 && errno == ENOENT, "held program unrun");
#if defined(__linux__) || defined(__rach2__)
    CHECK(prctl(PR_SET_CHILD_SUBREAPER, subreaper, 0, 0, 0) == 0, "held adoption restored");
#endif
}

/**
 * Sample a case that holds memory and has used processor time, and sample it
 * again once it has been cancelled and has finished. /proc counts processor
 * time in clock ticks, which lag the case's own count by up to a tick.
 */
static void test_sample(struct etude_exec *ex)
{
    struct want w = {"keep", "x", NULL, LONG_GRACE, 1, STOPPED, -1, SIGNALED, SIGTERM, CONFIRMED};
    struct etude_exec_spec s;
    struct attempt a;
    uint64_t cpu = 0, rss = 0, cpu_end, rss_end;
    int running = -2;

    memset(&a, 0, sizeof(a));
    a.w = w;
    spec_for(&s, &a, 0);
    CHECK(etude_exec_start(ex, &s, &a.p) == 0, "sample");
    if (a.p == NULL)
        return;
    while (etude_exec_wait(ex, -1) != a.p) {
        if (!a.cancelled && strstr(etude_proc_result(a.p)->out.data, "ready") != NULL) {
            running = etude_proc_sample(a.p, &cpu, &rss);
            a.cancelled = 1;
            etude_exec_cancel(a.p);
        }
    }
#if defined(__linux__) || defined(__rach2__)
    CHECK(running == 1 && cpu >= BURN_US / 2 && rss >= BURN_BYTES, "sample");
#else
    CHECK(running == -1, "sample");
#endif
    CHECK(etude_proc_sample(a.p, &cpu_end, &rss_end) == 0, "sample");
    etude_proc_free(a.p);
}

/**
 * Kill an attempt's keeper while its case runs: the end is lost, the cleanup
 * unconfirmed and the usage unavailable. The test then ends the case itself.
 */
static void test_lost_keeper(struct etude_exec *ex)
{
    struct want w[] = {
        {"hang", "w", NULL, LONG_GRACE, 0, ETUDE_END_LOST, -1, ETUDE_END_LOST, -1,
         ETUDE_CLEANUP_UNCONFIRMED},
    };
    const struct etude_exec_result *r;
    struct etude_exec_spec s;
    struct attempt a[1];
    struct etude_proc *p;

    memset(a, 0, sizeof(a));
    a[0].w = w[0];
    spec_for(&s, &a[0], 0);
    if (etude_exec_start(ex, &s, &a[0].p) != 0) {
        CHECK(0, "w");
        return;
    }
    r = etude_proc_result(a[0].p);
    while (strstr(r->out.data, "ready") == NULL)
        etude_exec_wait(ex, -1);
    kill((pid_t)r->keeper.pid, SIGKILL);
    while ((p = etude_exec_wait(ex, -1)) != a[0].p) {
    }
    a[0].done = 1;
    CHECK(r->end == ETUDE_END_LOST && r->exit_end == ETUDE_END_LOST, "w");
    CHECK(r->cleanup == ETUDE_CLEANUP_UNCONFIRMED, "w");
    CHECK(r->cpu_us.quality == ETUDE_QUALITY_UNAVAILABLE, "w");
    kill((pid_t)-r->process.pid, SIGKILL);
    etude_proc_free(a[0].p);
}

/**
 * End a controller while its case runs: the keeper must end the case, which
 * closes the only writer of a FIFO this process reads.
 */
static void test_controller_gone(void)
{
    struct want w = {"hold", "x", NULL, LONG_GRACE, 0, EXITED, 0, EXITED, 0, CONFIRMED};
    struct etude_exec_spec s;
    struct etude_exec *ex;
    struct attempt a;
    pid_t pid;
    char c;
    int fd;

    memset(&a, 0, sizeof(a));
    a.w = w;
    a.w.arg = fix_path("hold");
    CHECK(mkfifo(a.w.arg, 0600) == 0, "x");
    fd = open(a.w.arg, O_RDONLY | O_NONBLOCK);
    CHECK(fd >= 0, "x");
    pid = fork();
    if (pid == 0) {
        ex = etude_exec_new();
        spec_for(&s, &a, 0);
        if (ex == NULL || etude_exec_start(ex, &s, &a.p) != 0)
            _exit(1);
        while (strstr(etude_proc_result(a.p)->out.data, "ready") == NULL)
            etude_exec_wait(ex, -1);
        _exit(0);
    }
    CHECK(pid > 0 && waitpid(pid, NULL, 0) == pid, "x");
    if (fd < 0)
        return;
    fcntl(fd, F_SETFL, 0);
    CHECK(read(fd, &c, 1) == 0, "x");
    close(fd);
}

/** Optional complete files keep both streams beyond the bounded capture. */
static void test_output_files(struct etude_exec *ex)
{
    struct etude_exec_spec spec;
    struct etude_proc *p;
    const struct etude_exec_result *r;
    char *args[] = {self, "--case", "say", "complete", "0", NULL};
    char text[256];
    FILE *f;
    size_t n;

    for (int merged = 0; merged < 2; merged++) {
        memset(&spec, 0, sizeof(spec));
        spec.attempt_id = merged ? "merged-output" : "split-output";
        spec.argv = args;
        spec.envp = environ;
        spec.output_max = 4;
        spec.stdout_path = fix_path("complete.out");
        spec.stderr_path = merged ? spec.stdout_path : fix_path("complete.err");
        f = fopen(spec.stdout_path, "w");
        CHECK(f != NULL, "complete output fixture");
        if (f != NULL) {
            fputs("old output must be truncated", f);
            fclose(f);
        }
        p = NULL;
        CHECK(etude_exec_start(ex, &spec, &p) == 0, "complete output starts");
        if (p == NULL)
            continue;
        while (etude_exec_wait(ex, -1) != p) {
        }
        r = etude_proc_result(p);
        CHECK(r->end == ETUDE_END_EXITED && r->value == 0 && !r->output_error,
              "complete output ends without a write error");
        CHECK(r->out.kept == 4 && r->out.total > 4, "bounded capture remains bounded");
        f = fopen(spec.stdout_path, "rb");
        n = f == NULL ? 0 : fread(text, 1, sizeof(text) - 1, f);
        if (f != NULL)
            fclose(f);
        text[n] = 0;
        CHECK(strstr(text, "complete out\n") != NULL && strstr(text, "old output") == NULL,
              "complete stdout is written after truncation");
        if (merged)
            CHECK(strstr(text, "complete err\n") != NULL, "equal paths merge both streams");
        else {
            CHECK(strstr(text, "complete err") == NULL, "distinct paths keep streams separate");
            f = fopen(spec.stderr_path, "rb");
            n = f == NULL ? 0 : fread(text, 1, sizeof(text) - 1, f);
            if (f != NULL)
                fclose(f);
            text[n] = 0;
            CHECK(strcmp(text, "complete err\n") == 0, "complete stderr is preserved");
        }
        etude_proc_free(p);
    }
}

int main(int argc, char **argv)
{
    struct etude_exec *ex;
    int i;

    if (argc == 5 && strcmp(argv[1], "--case") == 0)
        return run_case(argv[2], argv[3], argv[4]);
    if (realpath(argv[0], self) == NULL) {
        printf("etude-exec-test: fail: %s cannot be found\n", argv[0]);
        return 1;
    }
    ex = etude_exec_new();
    if (ex == NULL)
        return 1;
    test_output_files(ex);
    test_concurrent(ex);
    test_posix_only(ex);
    test_reaper(ex);
    test_unrun(ex);
    test_held_keeper(ex);
    test_sample(ex);
    test_lost_keeper(ex);
    test_controller_gone();
    etude_exec_free(ex);
    for (i = 0; i < nregs; i++)
        if (regs[i].held < 0) {
            printf("etude_exec_hosttest: not run: the hold of each case before its exec: "
                   "this host does not show a process's system call\n");
            break;
        }
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_exec_hosttest: pass\n");
    return 0;
}
