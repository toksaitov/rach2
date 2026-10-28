/*
 * winledgerfix.c: the fixture of etude's ledger on Windows, ledger.c, which
 * registers attempts of wintarget.exe through etude/exec.h, reserves a claim
 * while another handle holds the ledger file open, and judges claims written
 * by hand about real processes.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "etude/ledger.h"
#include "winexec.h"
#include "winfixture.h"

/* What a listing found of one claim. */
struct seen {
    char id[32];
    enum etude_claim_status status;
    size_t nproc;
};

/* The claims of one listing. */
struct listing {
    struct seen s[16];
    int n;
};

/** Note one claim of a listing. */
static int note(const struct etude_ledger_claim *c, void *ctx)
{
    struct listing *li = ctx;

    if (li->n == 16)
        return 1;
    snprintf(li->s[li->n].id, sizeof li->s[li->n].id, "%s", c->id);
    li->s[li->n].status = c->status;
    li->s[li->n++].nproc = c->nproc;
    return 0;
}

/** Give the status of claim ID, or -1 when the ledger does not keep it. */
static int status_of(struct etude_ledger *l, const char *id, size_t *nproc)
{
    struct listing li;
    int i;

    memset(&li, 0, sizeof li);
    if (etude_ledger_list(l, note, &li) != 0)
        return -2;
    for (i = 0; i < li.n; i++) {
        if (strcmp(li.s[i].id, id) == 0) {
            if (nproc != NULL)
                *nproc = li.s[i].nproc;
            return (int)li.s[i].status;
        }
    }
    return -1;
}

/** Run ATTEMPT of executor EX until it finishes, within the bound. */
static int finish(struct etude_exec *ex, struct etude_proc *p)
{
    ULONGLONG t0 = GetTickCount64();

    while (etude_exec_wait(ex, 1000) != p) {
        if (GetTickCount64() - t0 > BOUND)
            return -1;
    }
    return 0;
}

/* A claim written by hand; PROC, when not NULL, is its case. */
struct hand {
    const char *id, *system;
    const struct etude_pid *owner;
    const char *state;
    const struct etude_pid *proc;
};

/** Append to TEXT the claim H. */
static void add_claim(char *text, size_t size, const struct hand *h)
{
    size_t n = strlen(text);

    snprintf(text + n, size - n, "claim %s\nsystem %s\nboot -\nowner %ld %llu %d\nstate %s\n",
             h->id, h->system, h->owner->pid, (unsigned long long)h->owner->start,
             h->owner->start_known, h->state);
    n = strlen(text);
    if (h->proc != NULL)
        snprintf(text + n, size - n, "process case %ld %llu %d\n", h->proc->pid,
                 (unsigned long long)h->proc->start, h->proc->start_known);
    n = strlen(text);
    snprintf(text + n, size - n, "end\n");
}

/** Write TEXT, with no change to its line ends, as file PATH. */
static int put(const char *path, const char *text)
{
    wchar_t *w = etude_win_long_path(path);
    FILE *f = w != NULL ? _wfopen(w, L"wb") : NULL;
    int bad;

    free(w);
    if (f == NULL)
        return -1;
    bad = fputs(text, f) < 0;
    return fclose(f) != 0 || bad ? -1 : 0;
}

/**
 * Run a case that reports a descendant, with the ledger as its registration,
 * and give the identity of its process, which has ended, in DEAD.
 */
static int registered_run(struct etude_ledger *l, struct etude_pid *dead)
{
    char *argv[] = {target, "orphan", NULL};
    const struct etude_exec_result *r;
    struct etude_exec_spec spec;
    struct etude_proc *p = NULL;
    struct etude_exec *ex = etude_exec_new();
    size_t nproc = 0;
    int k;

    CHECK(ex != NULL, "no executor");
    CHECK(etude_ledger_reserve(l, "orphan", NULL, 0, NULL, 0) == 0, "reserve: %s", strerror(errno));
    memset(&spec, 0, sizeof spec);
    spec.attempt_id = "orphan";
    spec.argv = argv;
    spec.output_max = 4096;
    spec.reg = etude_ledger_register;
    spec.child = etude_ledger_child;
    spec.reg_ctx = l;
    CHECK(etude_exec_start(ex, &spec, &p) == 0, "start: %s", strerror(errno));
    CHECK(finish(ex, p) == 0, "the attempt did not finish");
    r = etude_proc_result(p);
    *dead = r->process;
    k = status_of(l, "orphan", &nproc);
    CHECK(k == ETUDE_CLAIM_OWNED && nproc == 3, "the claim is %d with %lu processes, not 3", k,
          (unsigned long)nproc);
    CHECK(r->children == 1 && r->cleanup == ETUDE_CLEANUP_CONFIRMED, "%u children, cleanup %d",
          r->children, (int)r->cleanup);
    CHECK(etude_ledger_finish(l, "orphan", r->cleanup) == 0 && status_of(l, "orphan", NULL) == -1,
          "the confirmed claim stays");
    etude_exec_free(ex);
    return 0;
}

/* The ledger file held open, and for how long. */
struct holder {
    HANDLE h;
    DWORD ms;
};

/** Close the holder's handle once its time has passed. */
static DWORD WINAPI release(void *arg)
{
    struct holder *k = arg;

    Sleep(k->ms);
    CloseHandle(k->h);
    return 0;
}

/**
 * Hold the ledger file CLAIMS open without sharing its deletion for a third
 * of a second, as a virus scanner may, while a claim is reserved: the ledger
 * must wait for it, and the reservation succeed.
 */
static int held_claims(struct etude_ledger *l, const char *claims)
{
    wchar_t *w = etude_win_long_path(claims);
    struct holder k = {INVALID_HANDLE_VALUE, 300};
    HANDLE t;
    int rc;

    if (w != NULL)
        k.h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    CHECK(k.h != INVALID_HANDLE_VALUE, "cannot hold %s", claims);
    t = CreateThread(NULL, 0, release, &k, 0, NULL);
    if (t == NULL) {
        CloseHandle(k.h);
        return failed("cannot start the thread that releases %s", claims);
    }
    rc = etude_ledger_reserve(l, "held", NULL, 0, NULL, 0);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CHECK(rc == 0, "reserve while the ledger file is held: %s", strerror(errno));
    CHECK(status_of(l, "held", NULL) == ETUDE_CLAIM_OWNED, "the reserved claim is not owned");
    CHECK(etude_ledger_finish(l, "held", ETUDE_CLEANUP_CONFIRMED) == 0 &&
              status_of(l, "held", NULL) == -1,
          "the claim reserved while the file was held stays");
    return 0;
}

int fx_ledger(void)
{
    char *argv[] = {target, "hang", NULL};
    char dir[1024], claims[1100], text[4096] = "etude-ledger 1\n";
    struct etude_pid me, dead, h, h_reused, reused, unknown;
    struct etude_exec_spec spec;
    struct etude_proc *p = NULL;
    struct etude_ledger *l;
    struct etude_exec *ex;
    size_t freed = 9;
    char *slash;
    int r = 0;

    memset(&dead, 0, sizeof dead);
    snprintf(dir, sizeof dir, "%s", target);
    slash = strrchr(dir, '\\');
    CHECK(slash != NULL, "no directory in %s", target);
    strcpy(slash, "\\ledger");
    snprintf(claims, sizeof claims, "%s\\claims", dir);
    l = etude_ledger_open(dir);
    CHECK(l != NULL, "open: %s", strerror(errno));
    if (registered_run(l, &dead) != 0 || held_claims(l, claims) != 0) {
        etude_ledger_close(l);
        return 1;
    }

    ex = etude_exec_new();
    memset(&spec, 0, sizeof spec);
    spec.attempt_id = "hang";
    spec.argv = argv;
    spec.output_max = 4096;
    if (ex == NULL || etude_exec_start(ex, &spec, &p) != 0 ||
        etude_process_state((long)GetCurrentProcessId(), &me) != 1) {
        etude_ledger_close(l);
        return failed("cannot start the case that hangs");
    }
    h = etude_proc_result(p)->process;
    reused = me;
    reused.start += 7;
    unknown = me;
    unknown.start = 0;
    unknown.start_known = 0;
    h_reused = h;
    h_reused.start += 7;
    {
        const struct hand hands[] = {
            {"case-runs", "win32", &dead, "running", &h},
            {"owner-reused", "win32", &reused, "running", &h_reused},
            {"owner-unknown", "win32", &unknown, "reserved", NULL},
            {"reserved-dead", "win32", &dead, "reserved", NULL},
            {"owned", "win32", &me, "reserved", NULL},
            {"other-system", "linux", &dead, "reserved", NULL},
        };
        size_t i;

        for (i = 0; i < sizeof hands / sizeof hands[0]; i++)
            add_claim(text, sizeof text, &hands[i]);
    }
    if (put(claims, text) != 0)
        r = failed("cannot write %s", claims);
    else if (etude_ledger_recover(l, &freed) != 0 || freed != 2)
        r = failed("recovery freed %lu claims, not 2", (unsigned long)freed);
    else if (status_of(l, "owner-reused", NULL) != -1 || status_of(l, "reserved-dead", NULL) != -1)
        r = failed("a claim of an ended owner stays");
    else if (status_of(l, "case-runs", NULL) != ETUDE_CLAIM_LIVE)
        r = failed("the claim of a running case is not live");
    else if (status_of(l, "owner-unknown", NULL) != ETUDE_CLAIM_UNCERTAIN ||
             status_of(l, "other-system", NULL) != ETUDE_CLAIM_UNCERTAIN)
        r = failed("a claim in doubt is not uncertain");
    else if (status_of(l, "owned", NULL) != ETUDE_CLAIM_OWNED)
        r = failed("the claim of this process is not owned");
    etude_exec_cancel(p);
    if (finish(ex, p) != 0 && r == 0)
        r = failed("the case that hangs did not end");
    if (r == 0 && (etude_ledger_recover(l, &freed) != 0 || freed != 1 ||
                   status_of(l, "case-runs", NULL) != -1))
        r = failed("the claim of an ended job stays");
    etude_exec_free(ex);
    etude_ledger_close(l);
    return r;
}
