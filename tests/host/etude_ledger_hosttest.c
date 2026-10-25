/**
 * Check etude's ledger of resource claims (tools/etude/ledger.c) with the
 * POSIX executor, this program run as the case's program, with
 * `--case MODE TOKEN ARG`, and as the workers whose death the test causes.
 *
 * host-deps: tools/etude/boot_identity.c
 * host-deps: tools/etude/history_collect.c
 * host-deps: tools/etude/history_json.c
 * host-deps: tools/etude/routine_site.c tools/etude/routine_proof.c
 * host-deps: tools/etude/json.c
 * host-deps: tools/etude/authority.c tools/etude/authority_parse.c
 * host-deps: tools/etude/posix/authority_platform.c
 * host-deps: tools/etude/win/authority_platform.c
 * host-deps: tools/etude/ledger.c tools/etude/capture.c utils/sha256.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-deps: tools/etude/posix/process.c
 * host-cflags: -I tools/etude/include -Wall -Wextra -Werror
 *
 * A claim is recorded, refused past a limit and for a taken identity, and
 * ended by its owner alone. A case finds its own process in the ledger file
 * before its program runs, and a reported descendant is registered. Eight
 * processes compete for a limit that four fit, and exactly four win. A
 * ledger file that does not parse is refused and left as it was.
 *
 * Recovery is judged from claims written by hand, with real processes: a
 * reused PID of an owner or a case, an owner or a process whose start is
 * unknown, another boot, another system, a process group that outlives its
 * leader, a cleanup its owner found unconfirmed, and a verdict missing,
 * unconfirmed or cut short; and on Linux, where this process reaps the
 * orphans below it, from workers killed while their cases run: a case that
 * outlives its worker keeps the claim until its keeper confirms the cleanup;
 * a descendant that escaped unreported, with POSIX interfaces alone, leaves
 * the claim uncertain; a reported descendant that outlives its worker,
 * keeper and case keeps the claim live, and uncertain once it has ended; and
 * a worker that ends after its keeper has reported the cleanup, before it
 * records it, leaves a claim that the keeper's verdict frees.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif
#if defined(__linux__) || defined(__rach2__)
#include <sys/utsname.h>
#include "../../tools/etude/boot_identity.h"
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include "compat/pathmax_host.h"
#include "compat/fixdir_host.c"
#include "etude/ledger.h"

extern char **environ;

static int fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                          \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            printf("etude-ledger-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), #cond); \
            fails++;                                                                              \
        }                                                                                         \
    } while (0)

#if defined(__linux__)
#define SYSTEM "linux"
#elif defined(__APPLE__)
#define SYSTEM "darwin"
#else
#define SYSTEM "posix"
#endif

#define LONG_GRACE  60000000u
#define SHORT_GRACE 100000u

/* This program, by an absolute path. */
static char self[PATH_MAX];

/* ---- The case's program. ---- */

/** Write `text` with one write(2), as a report record must be written. */
static void say(const char *text)
{
    if (write(1, text, strlen(text)) < 0)
        _exit(120);
}

/** Wait for signals forever. */
static void idle(void)
{
    for (;;)
        pause();
}

/**
 * Start a descendant in a session of its own that waits forever, report it
 * in a `child` record when `report` is set, and print `TOKEN pid N`.
 */
static void escape(const char *token, int report)
{
    char line[128];
    int sync[2];
    pid_t pid;
    char c;

    if (pipe(sync) != 0 || (pid = fork()) < 0)
        _exit(121);
    if (pid == 0) {
        setsid();
        if (write(sync[1], "s", 1) != 1)
            _exit(122);
        idle();
    }
    close(sync[1]);
    if (read(sync[0], &c, 1) != 1)
        _exit(123);
    if (report) {
        snprintf(line, sizeof(line), "@etude 1 child %ld\n", (long)pid);
        say(line);
    }
    snprintf(line, sizeof(line), "%s pid %ld\n", token, (long)pid);
    say(line);
}

/**
 * Say whether the ledger file `claims` registers this process as the case
 * of claim `id`.
 */
static int registered_self(const char *claims, const char *id)
{
    char line[512], want[128], head[300];
    int in = 0, found = 0;
    FILE *f = fopen(claims, "r");

    if (f == NULL)
        return 0;
    snprintf(head, sizeof(head), "claim %s\n", id);
    snprintf(want, sizeof(want), "process case %ld ", (long)getpid());
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "claim ", 6) == 0)
            in = strcmp(line, head) == 0;
        else if (in && strncmp(line, want, strlen(want)) == 0)
            found = 1;
    }
    fclose(f);
    return found;
}

/** Run as the case's program in mode `mode`, with `token` naming it. */
static int run_case(const char *mode, const char *token, const char *arg)
{
    if (strcmp(mode, "check") == 0) {
        return registered_self(arg, token) ? 0 : 3;
    } else if (strcmp(mode, "daemon") == 0) {
        escape(token, 1);
    } else if (strcmp(mode, "deaf") == 0) {
        signal(SIGTERM, SIG_IGN);
        say("ready\n");
        idle();
    } else if (strcmp(mode, "escape-hang") == 0 || strcmp(mode, "daemon-hang") == 0) {
        escape(token, strcmp(mode, "daemon-hang") == 0);
        say("ready\n");
        idle();
    } else if (strcmp(mode, "mark") == 0) {
        return open(arg, O_WRONLY | O_CREAT, 0644) < 0;
    }
    return 0;
}

/* ---- Helpers of the controller. ---- */

/* One attempt's program and specification. */
struct run {
    char *argv[6];
    char verdict[1024];
    struct etude_exec_spec spec;
};

/** Fill `r` to run claim `id` in mode `mode` on ledger `l`. */
static void run_for(struct run *r, struct etude_ledger *l, const char *id, const char *mode,
                    const char *arg, uint64_t grace_us)
{
    memset(r, 0, sizeof(*r));
    r->argv[0] = self;
    r->argv[1] = "--case";
    r->argv[2] = (char *)mode;
    r->argv[3] = (char *)id;
    r->argv[4] = (char *)(arg != NULL ? arg : "-");
    r->argv[5] = NULL;
    r->spec.attempt_id = id;
    r->spec.argv = r->argv;
    r->spec.envp = environ;
    r->spec.output_max = 4096;
    r->spec.grace_us = grace_us;
    r->spec.reg = etude_ledger_register;
    r->spec.child = etude_ledger_child;
    r->spec.reg_ctx = l;
    if (etude_ledger_verdict_path(l, id, r->verdict, sizeof(r->verdict)) == 0)
        r->spec.verdict = r->verdict;
}

/** Run attempt `p` of executor `ex` until it finishes. */
static void finish_run(struct etude_exec *ex, struct etude_proc *p)
{
    while (etude_exec_wait(ex, -1) != p) {
    }
}

/* What a listing found of one claim. */
struct seen {
    char id[64];
    enum etude_claim_status status;
    char state[16];
    char why[64];
    size_t nres, nproc;
};

/* The claims of one listing. */
struct listing {
    struct seen s[32];
    int n;
};

/** Note one claim of a listing. */
static int note(const struct etude_ledger_claim *c, void *ctx)
{
    struct listing *li = ctx;
    struct seen *s;

    if (li->n == 32)
        return 1;
    s = &li->s[li->n++];
    snprintf(s->id, sizeof(s->id), "%s", c->id);
    snprintf(s->state, sizeof(s->state), "%s", c->state);
    snprintf(s->why, sizeof(s->why), "%s", c->why);
    s->status = c->status;
    s->nres = c->nres;
    s->nproc = c->nproc;
    return 0;
}

/** List ledger `l` into `li`; 0 on success. */
static int list(struct etude_ledger *l, struct listing *li)
{
    memset(li, 0, sizeof(*li));
    return etude_ledger_list(l, note, li);
}

/** Give the listing's claim `id`, or NULL. */
static const struct seen *find(const struct listing *li, const char *id)
{
    int i;

    for (i = 0; i < li->n; i++)
        if (strcmp(li->s[i].id, id) == 0)
            return &li->s[i];
    return NULL;
}

/** Say whether claim `id` of ledger `l` has `status` and the reason `why`. */
static int status_is(struct etude_ledger *l, const char *id, enum etude_claim_status status,
                     const char *why)
{
    const struct seen *s;
    struct listing li;

    if (list(l, &li) != 0 || (s = find(&li, id)) == NULL)
        return 0;
    return s->status == status && (why == NULL || strstr(s->why, why) != NULL);
}

/** Say whether ledger `l` keeps claim `id`. */
static int kept(struct etude_ledger *l, const char *id)
{
    struct listing li;

    return list(l, &li) == 0 && find(&li, id) != NULL;
}

/** Give the identity of the running process `pid`, or one with pid 0. */
static struct etude_pid ident(long pid)
{
    struct etude_pid id;

    memset(&id, 0, sizeof(id));
    if (etude_process_state(pid, &id) != 1)
        id.pid = 0;
    return id;
}

/** Write `text` as file `path`; 0 on success. */
static int put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    int bad;

    if (f == NULL)
        return -1;
    bad = fputs(text, f) < 0;
    return fclose(f) != 0 || bad ? -1 : 0;
}

/** Read file `path` into `buf`; the bytes read, or -1. */
static long get(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    size_t n;

    if (f == NULL)
        return -1;
    n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}

/** Start a child that waits forever, and give its PID. */
static pid_t sleeper(void)
{
    pid_t pid = fork();

    if (pid == 0)
        idle();
    return pid;
}

/** Kill process `pid` and reap it. */
static void end_child(pid_t pid)
{
    if (pid <= 0)
        return;
    kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }
}

/* ---- Claims, limits and owners. ---- */

/** Record, refuse, list and end claims of this process. */
static void test_claims(void)
{
    const struct etude_ledger_res two[] = {{"cpu_millis", 1000}, {"memory_bytes", 1u << 20}};
    const struct etude_ledger_res cap[] = {{"cpu_millis", 2500}};
    const struct etude_ledger_res bad[] = {{"cpu millis", 1}};
    const struct etude_ledger_res twice[] = {{"ports", 1}, {"ports", 2}};
    struct etude_ledger *l = etude_ledger_open(fix_path("claims"));
    const struct seen *s;
    struct listing li;

    CHECK(l != NULL, "claims");
    if (l == NULL)
        return;
    CHECK(etude_ledger_reserve(l, "a", two, 2, cap, 1) == 0, "claims");
    CHECK(etude_ledger_reserve(l, "b", two, 2, cap, 1) == 0, "claims");
    CHECK(etude_ledger_reserve(l, "c", two, 2, cap, 1) == -1 && errno == EAGAIN, "a limit");
    CHECK(etude_ledger_reserve(l, "c", two, 1, NULL, 0) == 0, "no limit");
    CHECK(etude_ledger_reserve(l, "a", two, 1, NULL, 0) == -1 && errno == EEXIST, "taken");
    CHECK(etude_ledger_reserve(l, "d d", two, 1, NULL, 0) == -1 && errno == EINVAL, "bad id");
    CHECK(etude_ledger_reserve(l, "/d", two, 1, NULL, 0) == -1 && errno == EINVAL, "bad id");
    CHECK(etude_ledger_reserve(l, "d", bad, 1, NULL, 0) == -1 && errno == EINVAL, "bad name");
    CHECK(etude_ledger_reserve(l, "d", twice, 2, NULL, 0) == -1 && errno == EINVAL, "twice");
    CHECK(list(l, &li) == 0 && li.n == 3, "claims");
    s = find(&li, "a");
    CHECK(s != NULL && s->status == ETUDE_CLAIM_OWNED && s->nres == 2 && s->nproc == 0 &&
              strcmp(s->state, "reserved") == 0,
          "claims");
    CHECK(etude_ledger_drop(l, "a") == -1 && errno == EBUSY, "owned");
    CHECK(etude_ledger_finish(l, "a", ETUDE_CLEANUP_PENDING) == -1 && errno == EINVAL, "pending");
    CHECK(etude_ledger_finish(l, "a", ETUDE_CLEANUP_CONFIRMED) == 0, "confirmed");
    CHECK(etude_ledger_finish(l, "a", ETUDE_CLEANUP_CONFIRMED) == -1 && errno == ENOENT, "gone");
    CHECK(etude_ledger_finish(l, "b", ETUDE_CLEANUP_UNCONFIRMED) == 0, "unconfirmed");
    CHECK(list(l, &li) == 0 && li.n == 2, "claims");
    s = find(&li, "b");
    CHECK(s != NULL && strcmp(s->state, "uncertain") == 0 && s->status == ETUDE_CLAIM_OWNED,
          "unconfirmed");
    CHECK(etude_ledger_reserve(l, "e", two, 2, cap, 1) == -1 && errno == EAGAIN, "kept limit");
    etude_ledger_close(l);
}

/**
 * Run a case that finds itself registered before its program runs, one that
 * reports a descendant, a claim the ledger lacks, and one of another owner.
 */
static void test_register(void)
{
    const char *dir = fix_path("register");
    struct etude_exec *ex = etude_exec_new();
    struct etude_ledger *l = etude_ledger_open(dir);
    const struct etude_exec_result *r;
    struct etude_proc *p = NULL;
    struct etude_pid parent;
    char claims[1024], text[2048];
    const struct seen *s;
    struct listing li;
    struct stat st;
    struct run run;

    CHECK(ex != NULL && l != NULL, "register");
    if (ex == NULL || l == NULL)
        goto out;
    snprintf(claims, sizeof(claims), "%s/claims", dir);
    CHECK(etude_ledger_reserve(l, "check", NULL, 0, NULL, 0) == 0, "check");
    run_for(&run, l, "check", "check", claims, LONG_GRACE);
    CHECK(etude_exec_start(ex, &run.spec, &p) == 0, "check");
    if (p != NULL) {
        CHECK(list(l, &li) == 0 && (s = find(&li, "check")) != NULL && s->nproc == 2 &&
                  strcmp(s->state, "running") == 0,
              "check");
        finish_run(ex, p);
        r = etude_proc_result(p);
        CHECK(r->end == ETUDE_END_EXITED && r->value == 0, "check");
        CHECK(r->cleanup == ETUDE_CLEANUP_CONFIRMED, "check");
        CHECK(etude_ledger_finish(l, "check", r->cleanup) == 0 && !kept(l, "check"), "check");
        CHECK(stat(run.verdict, &st) != 0 && errno == ENOENT, "check");
        etude_proc_free(p);
    }

    p = NULL;
    CHECK(etude_ledger_reserve(l, "daemon", NULL, 0, NULL, 0) == 0, "daemon");
    run_for(&run, l, "daemon", "daemon", NULL, LONG_GRACE);
    CHECK(etude_exec_start(ex, &run.spec, &p) == 0, "daemon");
    if (p != NULL) {
        finish_run(ex, p);
        r = etude_proc_result(p);
        CHECK(r->children == 1 && r->cleanup == ETUDE_CLEANUP_CONFIRMED, "daemon");
        CHECK(list(l, &li) == 0 && (s = find(&li, "daemon")) != NULL && s->nproc == 3, "daemon");
        CHECK(etude_ledger_finish(l, "daemon", r->cleanup) == 0, "daemon");
        etude_proc_free(p);
    }

    p = NULL;
    run_for(&run, l, "absent", "mark", fix_path("absent-mark"), LONG_GRACE);
    CHECK(etude_exec_start(ex, &run.spec, &p) == -1 && errno == ENOENT && p == NULL, "absent");
    CHECK(stat(fix_path("absent-mark"), &st) != 0 && errno == ENOENT, "absent");

    parent = ident((long)getppid());
    snprintf(text, sizeof(text),
             "etude-ledger 1\nclaim other\nsystem %s\nboot -\nowner %ld %llu %d\n"
             "state reserved\nend\n",
             SYSTEM, parent.pid, (unsigned long long)parent.start, parent.start_known);
    CHECK(parent.pid != 0 && put(claims, text) == 0, "other");
    run_for(&run, l, "other", "mark", fix_path("other-mark"), LONG_GRACE);
    CHECK(etude_exec_start(ex, &run.spec, &p) == -1 && errno == EPERM && p == NULL, "other");
    CHECK(stat(fix_path("other-mark"), &st) != 0 && errno == ENOENT, "other");
    CHECK(etude_ledger_finish(l, "other", ETUDE_CLEANUP_CONFIRMED) == -1 && errno == EPERM,
          "other");
    CHECK(status_is(l, "other", ETUDE_CLAIM_OWNED, NULL), "other");
out:
    if (ex != NULL)
        etude_exec_free(ex);
    etude_ledger_close(l);
}

/**
 * Let eight processes, each its own owner, claim 1000 of a limit of 4000 at
 * once; each holds its claim until all have tried.
 */
static void test_contention(void)
{
    const char *dir = fix_path("contention");
    const struct etude_ledger_res want[] = {{"cpu_millis", 1000}};
    const struct etude_ledger_res cap[] = {{"cpu_millis", 4000}};
    struct etude_ledger *l;
    int result[2], hold[2], i, won = 0, refused = 0;
    pid_t kids[8];
    size_t freed = 0;
    char id[16], c;

    CHECK(pipe(result) == 0 && pipe(hold) == 0, "contention");
    for (i = 0; i < 8; i++) {
        kids[i] = fork();
        if (kids[i] == 0) {
            close(result[0]);
            close(hold[1]);
            snprintf(id, sizeof(id), "c%d", i);
            l = etude_ledger_open(dir);
            c = l == NULL                                           ? 'x'
                : etude_ledger_reserve(l, id, want, 1, cap, 1) == 0 ? 'w'
                : errno == EAGAIN                                   ? 'r'
                                                                    : 'x';
            if (write(result[1], &c, 1) != 1 || read(hold[0], &c, 1) < 0)
                _exit(1);
            _exit(0);
        }
    }
    close(result[1]);
    close(hold[0]);
    for (i = 0; i < 8 && read(result[0], &c, 1) == 1; i++) {
        won += c == 'w';
        refused += c == 'r';
    }
    close(hold[1]);
    close(result[0]);
    for (i = 0; i < 8; i++)
        while (kids[i] > 0 && waitpid(kids[i], NULL, 0) < 0 && errno == EINTR) {
        }
    CHECK(won == 4 && refused == 4, "contention");
    l = etude_ledger_open(dir);
    CHECK(l != NULL && etude_ledger_recover(l, &freed) == 0 && freed == 4, "contention");
    etude_ledger_close(l);
}

/** Refuse ledger files that do not parse, and leave them as they were. */
static void test_damaged(void)
{
    static const char *const files[] = {
        "etude-ledger 2\n",
        "etude-ledger 1\nclaim x\nsystem " SYSTEM "\nboot -\nowner 1 1 1\nstate reserved\n",
        "etude-ledger 1\nclaim x\nsystem " SYSTEM "\nboot -\nowner 1 1 1\nstate running\n"
        "process case 1 1 2\nend\n",
        "etude-ledger 1\nclaim x\nsystem " SYSTEM "\nboot -\nowner 1 1 1\nstate reserved\n"
        "process case 1 1 1\nend\n",
        "etude-ledger 1\nclaim x\nsystem " SYSTEM "\nboot -\nowner 1 01 1\nstate reserved\nend\n",
        "etude-ledger 1\nclaim x\nsystem " SYSTEM "\nboot -\nowner 1 1 1\nstate reserved\nend\n"
        "claim x\nsystem " SYSTEM "\nboot -\nowner 1 1 1\nstate reserved\nend\n",
        "etude-ledger 1\nclaim x\nsystem " SYSTEM "\nboot -\nowner 1 1 1\nstate reserved\nend",
        "etude-ledger 1\n\n",
        "etude-ledger 1\nclaim x\nsystem " SYSTEM "\nboot -\nowner 1 1 1\naccount a\n"
        "state waiting\nend\n",
        "etude-ledger 1\nclaim x\nsystem " SYSTEM "\nboot -\nowner 1 1 1\nstate waiting\n"
        "ticket 1\nend\n",
        "etude-ledger 1\nclaim x\nsystem " SYSTEM "\nboot -\nowner 1 1 1\naccount a\n"
        "state blocked\nticket 1\nprocess case 1 1 1\nend\n",
        "etude-ledger 1\nclaim x\nsystem " SYSTEM "\nboot -\nowner 1 1 1\nstate reserved\n"
        "ticket 1\nend\n",
        "etude-ledger 1\nclaim x\nsystem " SYSTEM "\nboot -\nowner 1 1 1\naccount a\n"
        "state blocked\nticket 1\nend\nclaim z\nsystem " SYSTEM "\nboot -\nowner 1 1 1\n"
        "account a\nstate blocked\nticket 2\nend\n",
    };
    const char *dir = fix_path("damaged");
    struct etude_ledger *l = etude_ledger_open(dir);
    char claims[1024], buf[512];
    size_t i, freed;

    CHECK(l != NULL, "damaged");
    if (l == NULL)
        return;
    snprintf(claims, sizeof(claims), "%s/claims", dir);
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        CHECK(put(claims, files[i]) == 0, files[i]);
        CHECK(etude_ledger_reserve(l, "y", NULL, 0, NULL, 0) == -1 && errno == EBADMSG, files[i]);
        CHECK(etude_ledger_recover(l, &freed) == -1 && errno == EBADMSG, files[i]);
        CHECK(get(claims, buf, sizeof(buf)) >= 0 && strcmp(buf, files[i]) == 0, files[i]);
    }
    etude_ledger_close(l);
}

/* ---- Recovery from claims written by hand. ---- */

/* A claim written by hand; `proc`, when not NULL, is its case. */
struct hand {
    const char *id, *system, *boot;
    const struct etude_pid *owner;
    const char *state;
    const struct etude_pid *proc;
};

/** Append to `text` the claim `h`. */
static void add_claim(char *text, size_t size, const struct hand *h)
{
    size_t n = strlen(text);

    snprintf(text + n, size - n, "claim %s\nsystem %s\nboot %s\nowner %ld %llu %d\nstate %s\n",
             h->id, h->system, h->boot, h->owner->pid, (unsigned long long)h->owner->start,
             h->owner->start_known, h->state);
    n = strlen(text);
    if (h->proc != NULL)
        snprintf(text + n, size - n, "process case %ld %llu %d\n", h->proc->pid,
                 (unsigned long long)h->proc->start, h->proc->start_known);
    n = strlen(text);
    snprintf(text + n, size - n, "end\n");
}

/** Write `text` as the verdict of claim `id` of ledger `l`. */
static void verdict(struct etude_ledger *l, const char *id, const char *text)
{
    char path[1024];

    CHECK(etude_ledger_verdict_path(l, id, path, sizeof(path)) == 0 && put(path, text) == 0, id);
}

/**
 * Start a process group whose leader has ended and been reaped while a
 * member, a child of this process, waits forever; give the leader's identity
 * and the member's PID.
 */
static pid_t orphaned_group(struct etude_pid *leader)
{
    pid_t lead, member;

    lead = fork();
    if (lead == 0) {
        setpgid(0, 0);
        idle();
    }
    if (lead < 0 || setpgid(lead, lead) != 0)
        return -1;
    member = fork();
    if (member == 0) {
        setpgid(0, lead);
        idle();
    }
    if (member < 0 || (setpgid(member, lead) != 0 && errno != EACCES)) {
        end_child(lead);
        end_child(member);
        return -1;
    }
    *leader = ident(lead);
    end_child(lead);
    return member;
}

/** Read the current platform's boot identity independently of its ledger. */
static void current_boot(char *boot, size_t size)
{
    strcpy(boot, "-");
#if defined(__linux__) || defined(__rach2__)
    struct utsname kernel;
    if (uname(&kernel))
        return;
    if (etude_boot_is_rach2(kernel.sysname, kernel.release, kernel.version)) {
        CHECK(etude_boot_file_read("/dev/shm/etude-guest-boot", boot, size) == 0,
              "Rach2 boot identity");
        return;
    }
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (f != NULL) {
        if (fgets(boot, (int)size, f) == NULL)
            strcpy(boot, "-");
        boot[strcspn(boot, "\n")] = '\0';
        fclose(f);
    }
#elif defined(__APPLE__)
    size_t n = size;
    if (sysctlbyname("kern.bootsessionuuid", boot, &n, NULL, 0))
        strcpy(boot, "-");
    boot[size - 1] = '\0';
#else
    (void)size;
#endif
}

/** Judge claims of reused, unknown and ended identities, boots and systems. */
static void test_identities(void)
{
    const char *dir = fix_path("identities");
    struct etude_ledger *l = etude_ledger_open(dir);
    struct etude_pid me = ident((long)getpid()), reused, unknown, sl, sl_reused, sl_unknown, leader;
    char claims[1024], boot[128], text[8192] = "etude-ledger 1\n";
    pid_t s = sleeper(), member = orphaned_group(&leader);
    size_t freed = 0;

    CHECK(l != NULL && me.pid != 0 && s > 0 && member > 0, "identities");
    if (l == NULL || me.pid == 0 || s <= 0 || member <= 0)
        goto out;
    current_boot(boot, sizeof(boot));
    snprintf(claims, sizeof(claims), "%s/claims", dir);
    reused = me;
    reused.start += 7;
    unknown = me;
    unknown.start = 0;
    unknown.start_known = 0;
    sl = ident(s);
    sl_reused = sl;
    sl_reused.start += 7;
    sl_unknown = sl;
    sl_unknown.start = 0;
    sl_unknown.start_known = 0;
    {
        const struct hand hands[] = {
            {"owner-reused", SYSTEM, "-", &reused, "running", &sl_reused},
            {"case-runs", SYSTEM, "-", &reused, "running", &sl},
            {"owner-unknown", SYSTEM, "-", &unknown, "reserved", NULL},
            {"case-unknown", SYSTEM, "-", &reused, "running", &sl_unknown},
            {"other-system", "win32", "-", &reused, "reserved", NULL},
            {"group-lives", SYSTEM, "-", &reused, "running", &leader},
            {"marked-uncertain", SYSTEM, "-", &reused, "uncertain", &sl_reused},
            {"no-verdict", SYSTEM, "-", &reused, "running", &sl_reused},
            {"unconfirmed", SYSTEM, "-", &reused, "running", &sl_reused},
            {"cut-short", SYSTEM, "-", &reused, "running", &sl_reused},
            {"reserved-dead", SYSTEM, "-", &reused, "reserved", NULL},
            {"owned", SYSTEM, "-", &me, "reserved", NULL},
            {"other-boot", SYSTEM, "0-earlier-boot", &me, "running", &sl},
        };
        size_t i, n = sizeof(hands) / sizeof(hands[0]);

        for (i = 0; i < n; i++)
            if (strcmp(hands[i].boot, "-") == 0 || strcmp(boot, "-") != 0)
                add_claim(text, sizeof(text), &hands[i]);
    }
    CHECK(put(claims, text) == 0, "identities");
    verdict(l, "owner-reused", ETUDE_VERDICT_CONFIRMED);
    verdict(l, "case-runs", ETUDE_VERDICT_CONFIRMED);
    verdict(l, "group-lives", ETUDE_VERDICT_CONFIRMED);
    verdict(l, "marked-uncertain", ETUDE_VERDICT_CONFIRMED);
    verdict(l, "unconfirmed", ETUDE_VERDICT_UNCONFIRMED);
    verdict(l, "cut-short", "confirm");

    CHECK(etude_ledger_recover(l, &freed) == 0, "identities");
    CHECK(freed == (strcmp(boot, "-") != 0 ? 3u : 2u), "identities");
    CHECK(!kept(l, "owner-reused"), "owner-reused");
    CHECK(!kept(l, "reserved-dead"), "reserved-dead");
    CHECK(!kept(l, "other-boot"), "other-boot");
    CHECK(status_is(l, "case-runs", ETUDE_CLAIM_LIVE, "runs"), "case-runs");
    CHECK(status_is(l, "owner-unknown", ETUDE_CLAIM_UNCERTAIN, "owner"), "owner-unknown");
    CHECK(status_is(l, "case-unknown", ETUDE_CLAIM_UNCERTAIN, "identified"), "case-unknown");
    CHECK(status_is(l, "other-system", ETUDE_CLAIM_UNCERTAIN, "operating system"), "other-system");
    CHECK(status_is(l, "group-lives", ETUDE_CLAIM_LIVE, "runs"), "group-lives");
    CHECK(status_is(l, "marked-uncertain", ETUDE_CLAIM_UNCERTAIN, "unconfirmed"),
          "marked-uncertain");
    CHECK(status_is(l, "no-verdict", ETUDE_CLAIM_UNCERTAIN, "keeper"), "no-verdict");
    CHECK(status_is(l, "unconfirmed", ETUDE_CLAIM_UNCERTAIN, "keeper"), "unconfirmed");
    CHECK(status_is(l, "cut-short", ETUDE_CLAIM_UNCERTAIN, "keeper"), "cut-short");
    CHECK(status_is(l, "owned", ETUDE_CLAIM_OWNED, NULL), "owned");
#if defined(__linux__) || defined(__rach2__) || defined(__APPLE__)
    CHECK(strcmp(boot, "-") != 0, "other-boot");
#endif

    end_child(s);
    end_child(member);
    s = member = -1;
    CHECK(etude_ledger_recover(l, &freed) == 0 && freed == 2, "ended");
    CHECK(!kept(l, "case-runs") && !kept(l, "group-lives"), "ended");
    CHECK(status_is(l, "no-verdict", ETUDE_CLAIM_UNCERTAIN, NULL), "ended");
    CHECK(etude_ledger_drop(l, "no-verdict") == 0 && !kept(l, "no-verdict"), "drop");
    CHECK(etude_ledger_drop(l, "owned") == -1 && errno == EBUSY, "drop");
    CHECK(etude_ledger_drop(l, "no-verdict") == -1 && errno == ENOENT, "drop");
out:
    end_child(s);
    end_child(member);
    etude_ledger_close(l);
}

/* ---- Workers that die while their cases run. ---- */

#if defined(__linux__)

/* What a worker reports of its attempt. */
struct worker_report {
    long process, keeper, descendant;
};

/* What a worker runs: claim `id`, its case in mode `mode`. */
struct job {
    const char *id, *mode;
    int posix_only;
    uint64_t grace_us;
};

/**
 * Be a worker of the ledger in `dir`: run job `j`, and once the case says it
 * is ready, report its processes on `fd` and wait to be killed.
 */
static void worker(const char *dir, const struct job *j, int fd)
{
    struct etude_ledger *l = etude_ledger_open(dir);
    struct etude_exec *ex = etude_exec_new();
    const struct etude_exec_result *r;
    struct worker_report rep;
    struct etude_proc *p;
    struct run run;
    char key[80];
    const char *at;

    if (l == NULL || ex == NULL || etude_ledger_reserve(l, j->id, NULL, 0, NULL, 0) != 0)
        _exit(1);
    run_for(&run, l, j->id, j->mode, NULL, j->grace_us);
    run.spec.posix_only = j->posix_only;
    if (etude_exec_start(ex, &run.spec, &p) != 0)
        _exit(1);
    r = etude_proc_result(p);
    while (strstr(r->out.data, "ready") == NULL)
        etude_exec_wait(ex, -1);
    snprintf(key, sizeof(key), "%s pid ", j->id);
    at = strstr(r->out.data, key);
    rep.process = r->process.pid;
    rep.keeper = r->keeper.pid;
    rep.descendant = at != NULL ? atol(at + strlen(key)) : -1;
    if (write(fd, &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
        _exit(1);
    idle();
}

/**
 * Start a worker of the ledger in `dir` on job `j` and give what it reports,
 * with its PID in `*pid`; 0 on success.
 */
static int start_worker(const char *dir, const struct job *j, struct worker_report *rep, pid_t *pid)
{
    int sync[2];
    ssize_t n;

    if (pipe(sync) != 0 || (*pid = fork()) < 0)
        return -1;
    if (*pid == 0) {
        close(sync[0]);
        worker(dir, j, sync[1]);
    }
    close(sync[1]);
    n = read(sync[0], rep, sizeof(*rep));
    close(sync[0]);
    return n == (ssize_t)sizeof(*rep) ? 0 : -1;
}

/** Reap process `pid`, which has become this process's child. */
static void reap(long pid)
{
    while (pid > 0 && waitpid((pid_t)pid, NULL, 0) < 0 && errno == EINTR) {
    }
}

/**
 * Kill a worker whose case ignores SIGTERM: the claim stays live while the
 * case runs, and is freed once its keeper has confirmed the cleanup.
 */
static void test_worker_death(void)
{
    const char *dir = fix_path("worker");
    struct etude_ledger *l = etude_ledger_open(dir);
    struct worker_report rep;
    size_t freed = 9;
    const struct job job = {"dead", "deaf", 0, LONG_GRACE};
    pid_t w;

    if (l == NULL || start_worker(dir, &job, &rep, &w) != 0) {
        CHECK(0, "worker");
        etude_ledger_close(l);
        return;
    }
    end_child(w);
    CHECK(etude_ledger_recover(l, &freed) == 0 && freed == 0, "worker");
    CHECK(status_is(l, "dead", ETUDE_CLAIM_LIVE, "runs"), "worker");
    kill((pid_t)-rep.process, SIGKILL);
    reap(rep.keeper);
    CHECK(etude_ledger_recover(l, &freed) == 0 && freed == 1 && !kept(l, "dead"), "worker");
    etude_ledger_close(l);
}

/**
 * Kill a worker, with POSIX interfaces alone, whose case started an
 * unreported descendant in a session of its own: the keeper cannot confirm
 * the cleanup, so the claim stays uncertain until a person drops it.
 */
static void test_unreported(void)
{
    const char *dir = fix_path("unreported");
    struct etude_ledger *l = etude_ledger_open(dir);
    struct worker_report rep;
    char path[1024], buf[64];
    size_t freed = 9;
    const struct job job = {"escaped", "escape-hang", 1, SHORT_GRACE};
    pid_t w;

    if (l == NULL || start_worker(dir, &job, &rep, &w) != 0) {
        CHECK(0, "unreported");
        etude_ledger_close(l);
        return;
    }
    end_child(w);
    reap(rep.keeper);
    CHECK(rep.descendant > 0 && kill((pid_t)rep.descendant, 0) == 0, "unreported");
    CHECK(etude_ledger_recover(l, &freed) == 0 && freed == 0, "unreported");
    CHECK(status_is(l, "escaped", ETUDE_CLAIM_UNCERTAIN, "keeper"), "unreported");
    CHECK(etude_ledger_verdict_path(l, "escaped", path, sizeof(path)) == 0 &&
              get(path, buf, sizeof(buf)) >= 0 && strcmp(buf, ETUDE_VERDICT_UNCONFIRMED) == 0,
          "unreported");
    end_child((pid_t)rep.descendant);
    CHECK(status_is(l, "escaped", ETUDE_CLAIM_UNCERTAIN, "keeper"), "unreported");
    CHECK(etude_ledger_drop(l, "escaped") == 0 && !kept(l, "escaped"), "unreported");
    etude_ledger_close(l);
}

/**
 * Wait until process `pid`, the child of another process, has ended; a
 * zombie has ended, as etude_process_state judges it.
 */
static void await_end(long pid)
{
    while (ident(pid).pid != 0) {
    }
}

/**
 * Kill a worker's keeper, then the worker and its case, as a sweep of a
 * stuck session might, while a reported descendant in a session of its own
 * runs: the claim stays live, and once the descendant has ended, uncertain,
 * since no keeper confirmed the cleanup.
 *
 * The keeper has ended before the worker is killed: a keeper that outlives
 * its worker finds the controller's pipe closed, cancels the case and ends
 * the reported descendant. The descendant is judged running by
 * etude_process_state, as the ledger judges it, since kill(2) also finds a
 * zombie.
 */
static void test_reported(void)
{
    const char *dir = fix_path("reported");
    struct etude_ledger *l = etude_ledger_open(dir);
    struct worker_report rep;
    size_t freed = 9;
    const struct job job = {"swept", "daemon-hang", 0, LONG_GRACE};
    pid_t w;

    if (l == NULL || start_worker(dir, &job, &rep, &w) != 0) {
        CHECK(0, "reported");
        etude_ledger_close(l);
        return;
    }
    kill((pid_t)rep.keeper, SIGKILL);
    await_end(rep.keeper);
    kill(w, SIGKILL);
    kill((pid_t)-rep.process, SIGKILL);
    reap(w);
    reap(rep.keeper);
    reap(rep.process);
    CHECK(rep.descendant > 0 && ident(rep.descendant).pid != 0, "reported");
    CHECK(etude_ledger_recover(l, &freed) == 0 && freed == 0, "reported");
    CHECK(status_is(l, "swept", ETUDE_CLAIM_LIVE, "runs"), "reported");
    end_child((pid_t)rep.descendant);
    CHECK(etude_ledger_recover(l, &freed) == 0 && freed == 0, "reported");
    CHECK(status_is(l, "swept", ETUDE_CLAIM_UNCERTAIN, "keeper"), "reported");
    etude_ledger_close(l);
}

/**
 * End a worker after its keeper has reported the cleanup and been reaped,
 * and before the worker records the cleanup in the ledger, as a sweep that
 * kills the case first and the worker a moment later may: the keeper's
 * verdict confirms the cleanup, and recovery frees the claim.
 */
static void test_unrecorded(void)
{
    const char *dir = fix_path("unrecorded");
    struct etude_ledger *l = etude_ledger_open(dir);
    size_t freed = 9;
    int st = 0;
    pid_t w;

    if (l == NULL || (w = fork()) < 0) {
        CHECK(0, "unrecorded");
        etude_ledger_close(l);
        return;
    }
    if (w == 0) {
        struct etude_ledger *wl = etude_ledger_open(dir);
        struct etude_exec *ex = etude_exec_new();
        struct etude_proc *p;
        struct run run;

        if (wl == NULL || ex == NULL ||
            etude_ledger_reserve(wl, "unrecorded", NULL, 0, NULL, 0) != 0)
            _exit(1);
        run_for(&run, wl, "unrecorded", "exit", NULL, SHORT_GRACE);
        if (etude_exec_start(ex, &run.spec, &p) != 0)
            _exit(1);
        finish_run(ex, p);
        _exit(etude_proc_result(p)->cleanup == ETUDE_CLEANUP_CONFIRMED ? 0 : 2);
    }
    CHECK(waitpid(w, &st, 0) == w && WIFEXITED(st) && WEXITSTATUS(st) == 0, "unrecorded");
    CHECK(etude_ledger_recover(l, &freed) == 0 && freed == 1 && !kept(l, "unrecorded"),
          "unrecorded");
    etude_ledger_close(l);
}

#endif

/** On Linux, where this process reaps orphans, test workers that die. */
static void test_workers(void)
{
#if defined(__linux__)
    CHECK(prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0, "worker");
    test_worker_death();
    test_unreported();
    test_reported();
    test_unrecorded();
#endif
}

#include "compat/authority_fixture.c"
#include "compat/routine_site_host.c"

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "--case") == 0)
        return run_case(argv[2], argv[3], argv[4]);
    if (realpath(argv[0], self) == NULL) {
        printf("etude-ledger-test: fail: %s cannot be found\n", argv[0]);
        return 1;
    }
    if (argc == 2 && !strcmp(argv[1], "--authority-only")) {
        test_authority();
        test_routine_site();
        if (fails)
            return 1;
        fix_passed();
        puts("etude_ledger_hosttest: authority pass");
        return 0;
    }
    test_claims();
    test_register();
    test_contention();
    test_damaged();
    test_identities();
    test_workers();
    test_authority();
    test_routine_site();
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_ledger_hosttest: pass\n");
    return 0;
}
