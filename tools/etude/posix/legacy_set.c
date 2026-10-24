/*
 * legacy_set.c: the set driver on POSIX systems and Windows. It runs the
 * suites of one run of tests/run as one run of etude, each by the host, guest
 * and machine cases of its manifest: it prepares their artifacts under a
 * claim of every limited resource of the machine, admits each repetition
 * through the machine's ledger, with the old runner's host lock and, for a
 * guest case, a port block, runs a host case's program and a machine case's
 * scenario in its workspace and a guest case's machine (legacy_guest.c)
 * under the case's own watchdog, and writes the run's history and its status
 * record.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/utsname.h>
#endif
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#ifdef _WIN32
#include "../win/files.h"
#endif

#include "../compare.h"
#include "../cost.h"
#include "../collection_repeat.h"
#include "../collection_load.h"
#include "../measurement_context.h"
#include "../observe_history.h"
#include "etude/artifact.h"
#include "etude/bridge.h"
#include "etude/history.h"
#include "etude/ledger.h"
#include "etude/legacy.h"
#include "etude/sched.h"
#include "etude/status.h"
#include "etude/watchdog.h"
#include "legacy_cases.h"
#include "legacy_guest.h"
#include "posix.h"
#include "sha256.h"

#ifndef _WIN32
extern char **environ;
#endif

/* How often a request that waits for room, or a claim that waits for the
 * old locks, is asked again, since neither gives notice when it frees. */
#define ASK_MS  200
#define LOCK_US UINT64_C(1000000)

/* The longest the status record goes unwritten while the run goes on. */
#define STATUS_US UINT64_C(5000000)

/* From the preparation's termination signal to its kill. */
#define PREP_GRACE_US UINT64_C(15000000)

/* From a case's termination signal to its kill, which a machine case's
 * scenario has beside its cleanup's bound; and what a scenario keeps of each
 * console and of each output, and how often etude-guest's frames are sent
 * again. */
#define CASE_GRACE_US  UINT64_C(5000000)
#define CONSOLE_BYTES  (UINT64_C(64) << 20)
#define SCENARIO_BYTES (UINT64_C(1) << 20)
#define RESEND_US      UINT64_C(2000000)

/* The seconds the watchdog of the preparation allows past the bound that
 * `etude prepare` keeps itself, for it to end and report. */
#define MARGIN_S 120u

/* The first local port of the machine cases, before the offset of the port
 * block: with LEGACY_PORTS after it, their ports are 2400 to 2999 of a block
 * of a thousand, so that no two blocks share a port. */
#define PORT_BASE 2400u

/* The bytes of history one run may write. */
#define HISTORY_BYTES (UINT64_C(64) << 20)

/* The room for a path, for a line of output, and for a reason. */
#define PATH_LEN   4096
#define LINE_LEN   8192
#define REASON_LEN 400

/* The bytes of a command's output kept as the evidence of a failure, from
 * its end, and read for its result line. */
#define EVIDENCE_BYTES ((size_t)16 << 20)
#define TAIL_BYTES     ((size_t)65536)

/* The start of the result line `etude prepare` ends its output with. */
#define RESULT "etude prepare: result: "

/* The most variables a case's program is given by its item. */
#define CASE_VARS 7

/* The memory profile of the 72 GiB boundary boot, and the line of the site
 * policy that permits its lazy backing, which the owner adds once the
 * site's acceptance of that boot has passed. */
#define BOUNDARY_PROFILE "bigmem-boundary-v1"
#define BOUNDARY_PERMIT  "boundary_lazy " BOUNDARY_PROFILE

/* Whether a host case runs its work in etude's tooling machine here, as on
 * every host that is not Linux. */
#ifdef __linux__
#define TOOLING_HOST 0
#else
#define TOOLING_HOST 1
#endif

static const char *const resource_names[] = {"cpu_millis", "memory_bytes", "disk_bytes"};
static const char *const outcome_names[] = {"pass", "failure", "incomplete", "cancelled"};

/* How a case, the preparation or the run came out. */
enum outcome { PASS, FAILURE, INCOMPLETE, CANCELLED };

/* Where a command of the run is. */
enum state {
    QUEUED,  /* not admitted; its request may wait in the ledger */
    LOCKING, /* its claim is admitted and waits for the old locks */
    LOADING, /* its registered load is starting before the case */
    RUNNING, /* it runs */
    DONE     /* it ended, or never started */
};

/* A command the driver runs under the supervisor: the preparation, or the
 * program, the scenario or the machine of one repetition of a case. */
struct command {
    char id[300];     /* the identity of its claim and its attempt */
    const char *name; /* the case's label, or "prepare" */
    enum state state;
    struct etude_ledger_res res[3];
    size_t nres;
    int exclusive; /* nothing else of the run runs beside it */
    int alone;     /* its old host lock is taken alone */
    int ports;     /* its claim takes a port block, as a guest case's does */
    int claim;     /* its claim is in the ledger */
    struct etude_bridge *bridge;
    uint64_t tried_us;  /* when the old locks were last tried */
    char out[PATH_LEN]; /* the file of its output */
    char **argv;
    char **envp;
    const char *cwd; /* its working directory: the tree's root, or a case's
                        workspace */
    struct etude_watchdog_spec wd;
    uint64_t grace_us;
    struct etude_exec_spec spec;
    char verdict[PATH_LEN];
    struct etude_supervised *sup;
    struct etude_collection_load *load;
    int load_stopped;
    struct etude_reading queued, admitted, done;
    uint64_t queued_us, queued_epoch_us; /* when it was first asked for */
    int asked, was_admitted, ran, cancelled;
    enum outcome outcome;
    char reason[REASON_LEN];
    char disruption[REASON_LEN];
    enum etude_end end;
    int value;
    enum etude_cleanup cleanup;
    struct etude_measure cpu_us;
    struct etude_pid keeper, process;
    uint64_t started_us, ended_us, max_gap_us; /* monotonic */
    int is_case;                               /* it is a case's program */
    int live;                                  /* its output is printed as it comes */
    long printed;                              /* the bytes of its output read for printing */
    long fed;                                  /* the bytes of its output fed to its watchdog */
    char line[LINE_LEN];                       /* an output line not yet whole */
    size_t nline;
};

/* One item of the plan: a repetition of a case of a suite's manifest. */
struct member {
    const struct etude_legacy_member *m;    /* its suite */
    const struct legacy_manifest *manifest; /* its case's manifest */
    const struct legacy_case *k;            /* its case */
    long long rep;                          /* its repetition */
    const char *arch;                       /* its item's architecture */
    int first;                              /* the first member of its suite */
    char case_id[160];
    char label[180]; /* a case's identity, and its repetition after the first */
    char item_id[300];
    char record_id[300];                            /* its attempt record, once written */
    char work[PATH_LEN];                            /* its workspace, kept when it does not pass */
    char program[PATH_LEN];                         /* a case's program */
    char judged_out[PATH_LEN], judge_err[PATH_LEN]; /* the judge's output */
    char *judged; /* the results document the judge gave, or NULL */
    size_t njudged;
    char vars[CASE_VARS][PATH_LEN + 32]; /* the variables a case's program is
                                            given */
    struct legacy_guest *g;              /* a guest case's attempt while it runs */
    char guest_err[PATH_LEN];            /* the standard error of a guest's program */
    /* A machine case's scenario invocation, and the digests of its artifacts
     * by its goals once they are published. */
    char invocation[PATH_LEN];
    char (*digests)[ETUDE_DIGEST_HEX + 1];
    unsigned port; /* a machine case's first local port, or 0 */
    char inventory[65];
    char execution_record[300];
    size_t history_samples, gap_samples;
    struct etude_cost cost; /* compatible history, before command pointers exist */
    char workload[400], assertions[400], profile[400]; /* blob references */
    const struct etude_load *load;
    struct legacy_case *learned;
    struct command c;
};

/* One run of the set. */
struct set {
    const struct etude_legacy_set_opts *o;
    const char *arches[2]; /* the architectures of `o->arch`, i686 first */
    size_t narches;
    /* The request's members, one for each architecture each runs on, and
     * by each whether an earlier one runs the same suite, and with it the
     * suite's cases of no architecture. */
    struct etude_legacy_member *members;
    unsigned char *again;
    size_t nmembers;
    FILE *out;
    char run_id[256];
    char store[PATH_LEN];
    char output[PATH_LEN];
    char schema[PATH_LEN];
    char record[PATH_LEN]; /* the preparation's record */
    char machine_id[128], boot_id[128], os_version[128];
    const char *os;
    char seconds[16];
    struct etude_history *h;
    struct cmp_history baseline;
    struct etude_bridge *collection_bridge;
    char collection_claim[256];
    int uncertain;
    struct etude_ledger *l;
    struct etude_supervisor *sup;
    struct etude_ledger_res cap[3];
    size_t ncap;
    struct etude_repeat_case *repeat_cases;
    struct etude_repeat_item *repeat_items;
    size_t repeat_count;
    char *repeat_selection;
    struct member *mem; /* serial suites and quiet host groups first, then
                           the rest: as given in a set of one architecture,
                           and in a set of both in etude_sched_order's
                           order */
    size_t n;
    char tree[PATH_LEN];               /* the tree's absolute path */
    struct etude_store *objects;       /* the guest and machine cases' artifacts,
                                          or NULL */
    char objects_dir[PATH_LEN];        /* their store, under the evidence */
    unsigned port_base;                /* the machine cases' first local port */
    unsigned ports;                    /* the local ports given out so far */
    struct legacy_manifest *manifests; /* by the request's members */
    struct etude_job *jobs;            /* beside mem */
    char digests[PATH_LEN];            /* the tree's digests file */
    struct command prep;
    char prep_step[300];
    char plan_record[300];
    char run_record[300];
    char policy[400];             /* the reference of the budget policy */
    char stop_reason[REASON_LEN]; /* why the members not ended are cancelled */
    int stopping;
    /* The site's policy permits the lazy boundary profile. */
    int boundary_lazy;
    int failed; /* the history or the ledger failed */
    uint64_t status_us;
    struct etude_reading start, first, end;
    struct etude_observer *observer;
    char observation_path[PATH_LEN];
    char *load;
    uint64_t goal_us, growth_us, ceiling_us, reserve_us, deadline_us;
    int budget_expired, budget_failed, owned_cleaned, cleanup_failed, observation_ended;
    int context_valid;
    int granted;
    unsigned running, peak;
    uint64_t busy_us, last_us;
};

/* ---- Text and files. ---- */

/** Print one line of progress, and flush it. */
static void say(struct set *x, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(x->out, fmt, ap);
    va_end(ap);
    fputc('\n', x->out);
    fflush(x->out);
}

/** Append `text` to `t` as a JSON string. A byte outside printable ASCII
 * becomes `?`, since the history is ASCII. */
static void string(struct cmp_text *t, const char *text)
{
    const unsigned char *p;

    cmp_printf(t, "\"");
    for (p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\')
            cmp_printf(t, "\\%c", *p);
        else if (*p < 0x20 || *p > 0x7e)
            cmp_printf(t, "?");
        else
            cmp_printf(t, "%c", *p);
    }
    cmp_printf(t, "\"");
}

/** Say whether `ch` is a letter or a digit. */
static int alnum(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
}

/** Copy `in` into `out` as a token of an identity: each byte that is not a
 * letter, a digit, `.`, `_` or `-` becomes `-`, and the first is a letter or
 * a digit. `in` and `out` may be the same. */
static void token_of(const char *in, char *out, size_t size)
{
    size_t i;

    for (i = 0; in[i] != '\0' && i + 1 < size; i++)
        out[i] = alnum(in[i]) || in[i] == '.' || in[i] == '_' || in[i] == '-' ? in[i] : '-';
    out[i] = '\0';
    if (i == 0)
        snprintf(out, size, "x");
    else if (!alnum(out[0]))
        out[0] = 'x';
}

/** Say whether `s` has the form of a suite's name: lowercase letters, digits
 * and `-`, beginning with no `-`. */
static int suite_name(const char *s)
{
    size_t i;

    if (s == NULL || s[0] == '\0' || s[0] == '-' || strlen(s) > 64)
        return 0;
    for (i = 0; s[i] != '\0'; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-'))
            return 0;
    return 1;
}

/** Write into `buf` as printf does; 0, or -1 with errno ENAMETOOLONG when
 * the text does not fit. */
static int fit(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

/** Make directory `path` unless it exists; 0, or -1 with errno set. */
static int make_dir(const char *path)
{
    return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/** Write the SHA-256 of `n` bytes into `hex`. */
static void digest_of(const char *bytes, size_t n, char hex[65])
{
    struct sha256 s;
    unsigned char sum[32];

    sha256_init(&s);
    sha256_update(&s, (const unsigned char *)bytes, n);
    sha256_final(&s, sum);
    sha256_hex(sum, hex);
}

/** Fill `boot` with the identity of this boot, or "-" where there is none. */
static void boot_read(char *boot, size_t size)
{
#if defined(_WIN32)
    if (etude_win_boot_id(boot, size) != 0)
        boot[0] = '\0';
#elif defined(__linux__)
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");

    if (f == NULL || fgets(boot, (int)size, f) == NULL)
        boot[0] = '\0';
    if (f != NULL)
        fclose(f);
    boot[strcspn(boot, "\n")] = '\0';
#elif defined(__APPLE__)
    size_t len = size;

    if (sysctlbyname("kern.bootsessionuuid", boot, &len, NULL, 0) != 0)
        len = 0;
    boot[len < size ? len : size - 1] = '\0';
#endif
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    if (boot[0] == '\0')
        snprintf(boot, size, "-");
    else
        token_of(boot, boot, size);
#else
    snprintf(boot, size, "-");
#endif
}

/**
 * Read the last `max` bytes of file `path` into memory of its own, ended by
 * a NUL, with their count in `*n`.
 *
 * @return 0, or -1 with errno set.
 */
static int read_tail(const char *path, size_t max, char **bytes, size_t *n)
{
    FILE *f = fopen(path, "rb");
    long size = -1;
    char *data = NULL;
    size_t got;

    *bytes = NULL;
    *n = 0;
    if (f == NULL)
        return -1;
    if (fseek(f, 0, SEEK_END) == 0)
        size = ftell(f);
    if (size < 0 || fseek(f, (size_t)size > max ? size - (long)max : 0, SEEK_SET) != 0 ||
        (data = malloc(max + 1)) == NULL) {
        fclose(f);
        return -1;
    }
    got = fread(data, 1, max, f);
    fclose(f);
    data[got] = '\0';
    *bytes = data;
    *n = got;
    return 0;
}

/* ---- The history's pieces. ---- */

/** Keep `n` bytes in the store, and append a blob reference to them to
 * `out`. */
static int keep(const struct set *x, const char *bytes, size_t n, struct cmp_text *out)
{
    char path[PATH_LEN + 80], hex[65];

    if (cmp_store(x->store, bytes, n, path, sizeof(path), hex) != 0)
        return -1;
    cmp_printf(out, "{\"sha256\":\"%s\",\"size_bytes\":%lu,\"locations\":[", hex, (unsigned long)n);
    string(out, path);
    cmp_printf(out, "]}");
    return out->failed ? -1 : 0;
}

/** Keep document `doc`, which is emptied, and append its reference to
 * `out`. */
static int keep_doc(const struct set *x, struct cmp_text *doc, struct cmp_text *out)
{
    int rc = doc->failed ? -1 : keep(x, doc->data, doc->length, out);

    free(doc->data);
    memset(doc, 0, sizeof(*doc));
    return rc;
}

/** Keep document `doc`, which is emptied, with its reference in `ref`. */
static int keep_ref(const struct set *x, struct cmp_text *doc, char *ref, size_t size)
{
    struct cmp_text t = {NULL, 0, 0, 0};
    int rc = keep_doc(x, doc, &t);

    if (rc == 0 && (t.failed || t.length >= size))
        rc = -1;
    if (rc == 0)
        memcpy(ref, t.data, t.length + 1);
    free(t.data);
    return rc;
}

/** Append measurement `m` of the executor as the history writes one. */
static void measure(struct cmp_text *t, const struct etude_measure *m)
{
    if (m->quality == ETUDE_QUALITY_UNAVAILABLE) {
        cmp_printf(t, "{\"value\":null,\"reason\":");
        string(t, m->method != NULL ? m->method : "it was not measured");
        cmp_printf(t, "}");
        return;
    }
    cmp_printf(t, "{\"value\":%llu,\"method\":", (unsigned long long)m->value);
    string(t, m->method);
    cmp_printf(t, ",\"quality\":\"%s\",\"interval_us\":null,\"error_bound\":null}",
               m->quality == ETUDE_QUALITY_EXACT ? "exact" : "lower_bound");
}

/** Give a measurement of the history for one of the executor. */
static struct etude_measurement as_history(const struct etude_measure *m)
{
    struct etude_measurement h;

    memset(&h, 0, sizeof(h));
    if (m->quality == ETUDE_QUALITY_UNAVAILABLE) {
        h.reason = m->method != NULL ? m->method : "it was not measured";
        return h;
    }
    h.value = m->value;
    h.method = m->method;
    h.quality = m->quality == ETUDE_QUALITY_EXACT ? "exact" : "lower_bound";
    return h;
}

/** Append the time from `a` to `b` as a measurement. */
static int elapsed(struct cmp_text *t, const struct etude_reading *a, const struct etude_reading *b)
{
    struct etude_measurement m = etude_history_elapsed(a, b);
    char json[1024];

    if (etude_measurement_json(&m, json, sizeof(json)) != 0)
        return -1;
    cmp_printf(t, "%s", json);
    return 0;
}

/** Append an interval from `a` to `b`, or one with no end when `b` is NULL. */
static int interval(struct cmp_text *t, const struct set *x, const struct etude_reading *a,
                    const struct etude_reading *b, const char *why_open)
{
    struct etude_measurement m;
    char json[1024];

    if (b != NULL)
        m = etude_history_elapsed(a, b);
    else {
        memset(&m, 0, sizeof(m));
        m.reason = why_open;
    }
    if (etude_measurement_json(&m, json, sizeof(json)) != 0)
        return -1;
    cmp_printf(t, "{\"start_utc\":\"%s\",\"end_utc\":", a->utc);
    if (b != NULL && m.reason == NULL)
        cmp_printf(t, "\"%s\"", b->utc);
    else
        cmp_printf(t, "null");
    cmp_printf(t, ",\"clock\":{\"machine_id\":");
    string(t, x->machine_id);
    cmp_printf(t, ",\"boot_id\":");
    string(t, x->boot_id);
    cmp_printf(t, ",\"clock_id\":\"monotonic\",\"resolution_us\":%llu},\"start_us\":%llu,",
               (unsigned long long)a->resolution_us, (unsigned long long)a->monotonic_us);
    if (b != NULL && m.reason == NULL)
        cmp_printf(t, "\"end_us\":%llu", (unsigned long long)b->monotonic_us);
    else
        cmp_printf(t, "\"end_us\":null");
    cmp_printf(t, ",\"elapsed_us\":%s}", json);
    return 0;
}

/** Append record `data` of `kind` to the history; its identity in `id`. */
static int append(const struct set *x, const char *kind, const struct cmp_text *data, char *id,
                  size_t size)
{
    struct etude_reading now;

    if (data->failed || etude_history_now(&now) != 0 ||
        etude_history_record_id(x->h, id, size) != 0)
        return -1;
    return etude_history_append(x->h, id, now.utc, kind, data->data);
}

/** Append the resources of command `c`: its processes, kept, and its time. */
static int resources(const struct set *x, const struct command *c, struct cmp_text *t)
{
    struct cmp_text d = {NULL, 0, 0, 0};
    int rc;

    cmp_printf(&d, "{\"attempt_id\":");
    string(&d, c->id);
    cmp_printf(&d, ",\"processes\":[");
    if (c->ran)
        cmp_printf(&d,
                   "{\"role\":\"keeper\",\"pid\":%ld,\"start\":\"%llu\"},{\"role\":\"case\","
                   "\"pid\":%ld,\"start\":\"%llu\"}",
                   c->keeper.pid, (unsigned long long)c->keeper.start, c->process.pid,
                   (unsigned long long)c->process.start);
    cmp_printf(&d, "]}\n");
    cmp_printf(t, "{\"scope\":");
    rc = keep_doc(x, &d, t);
    cmp_printf(t, ",\"tree_cpu_us\":");
    measure(t, &c->cpu_us);
    cmp_printf(t, ",\"combined_peak_bytes\":{\"value\":null,\"reason\":\"the set driver samples "
                  "no combined memory\"},\"samples\":[]}");
    return rc;
}

/** Append the result of command `c`, with its output kept as the evidence
 * when it ran and did not pass. */
static int result(const struct set *x, const struct command *c, struct cmp_text *t)
{
    char *bytes = NULL;
    size_t n = 0;
    int rc = 0;

    cmp_printf(t, "{\"outcome\":\"%s\",\"reason\":", outcome_names[c->outcome]);
    if (c->outcome == PASS)
        cmp_printf(t, "null");
    else
        string(t, c->reason[0] != '\0' ? c->reason : outcome_names[c->outcome]);
    cmp_printf(t, ",\"cleanup\":\"%s\",\"evidence\":[",
               c->cleanup == ETUDE_CLEANUP_CONFIRMED ? "confirmed" : "unconfirmed");
    if (c->outcome != PASS && c->ran && read_tail(c->out, EVIDENCE_BYTES, &bytes, &n) == 0 && n > 0)
        rc = keep(x, bytes, n, t);
    free(bytes);
    cmp_printf(t, "],\"capture_errors\":[],\"disruptions\":[");
    if (c->disruption[0] != '\0')
        string(t, c->disruption);
    cmp_printf(t, "]}");
    return rc;
}

/* ---- The plan. ---- */

/** Keep `n` bytes in the store, with a blob reference to them in `ref`. */
static int keep_bytes(const struct set *x, const char *bytes, size_t n, char *ref, size_t size)
{
    struct cmp_text t = {NULL, 0, 0, 0};
    int rc = keep(x, bytes, n, &t);

    if (rc == 0 && (t.failed || t.length >= size))
        rc = -1;
    if (rc == 0)
        memcpy(ref, t.data, t.length + 1);
    free(t.data);
    return rc;
}

/* Serialize one bound document in a buffer owned only by this call. */
static int repeated_doc(const struct set *x, const struct jbuf *declared,
                        const struct cmp_text *binding, char *ref, size_t size)
{
    struct cmp_text d = {0};
    if (binding->failed)
        return -1;
    cmp_printf(&d, "{\"declared\":%s,\"repetition\":%s}\n", declared->data, binding->data);
    int rc = d.failed ? -1 : keep_bytes(x, d.data, d.length, ref, size);
    free(d.data);
    return rc;
}

/** Keep the canonical documents of the case of member `m` as its item's
 * blobs, and give its inventory digest. */
static int item_docs(const struct set *x, struct member *m)
{
    const struct legacy_case *k = m->k;
    int rc;

    digest_of(k->inventory.data, k->inventory.len, m->inventory);
    if (x->repeat_selection) {
        char order[65];
        digest_of(x->repeat_selection, strlen(x->repeat_selection), order);
        struct cmp_text binding = {0};
        cmp_printf(&binding, "{\"policy\":\"%s\",\"seed\":%llu,\"order_sha256\":\"%s\",\"load\":",
                   ETUDE_REPEAT_POLICY, (unsigned long long)x->o->repeat->seed, order);
        if (m->load) {
            const char *kind = m->load->kind == ETUDE_LOAD_CPU      ? "cpu"
                               : m->load->kind == ETUDE_LOAD_MEMORY ? "memory"
                                                                    : "disk";
            cmp_printf(&binding, "{\"load_id\":\"%s\",\"kind\":\"%s\",\"amount\":%llu}",
                       m->load->load_id, kind, (unsigned long long)m->load->amount);
        } else
            cmp_printf(&binding, "null");
        cmp_printf(&binding, "}");
        rc = repeated_doc(x, &k->workload, &binding, m->workload, sizeof(m->workload));
        rc |= repeated_doc(x, &k->profile, &binding, m->profile, sizeof(m->profile));
        free(binding.data);
    } else {
        rc = keep_bytes(x, k->workload.data, k->workload.len, m->workload, sizeof(m->workload));
        rc |= keep_bytes(x, k->profile.data, k->profile.len, m->profile, sizeof(m->profile));
    }
    rc |= keep_bytes(x, k->inventory.data, k->inventory.len, m->assertions, sizeof(m->assertions));
    return rc;
}

/** Append the machine's limits as members of an object. */
static void capacity(const struct set *x, struct cmp_text *t)
{
    size_t i;

    for (i = 0; i < x->ncap; i++)
        cmp_printf(t, "%s\"%s\":%llu", i ? "," : "", x->cap[i].name,
                   (unsigned long long)x->cap[i].amount);
}

/** Append `key` and a reference to document `doc`, which is kept and
 * emptied. */
static int part(const struct set *x, struct cmp_text *t, const char *key, struct cmp_text *doc)
{
    cmp_printf(t, ",\"%s\":", key);
    return keep_doc(x, doc, t);
}

/* Keep a verified context fragment, or the explicit unmeasured fallback. */
static int context_part(const struct set *x, struct cmp_text *t, const char *key,
                        struct cmp_text *fallback)
{
    const char *text = etude_context_text(x->o->context, key);
    if (!text)
        return part(x, t, key, fallback);
    free(fallback->data);
    memset(fallback, 0, sizeof(*fallback));
    cmp_printf(t, ",\"%s\":", key);
    return keep(x, text, strlen(text), t);
}

static const char *purpose(const struct set *x)
{
    const char *p = etude_context_text(x->o->context, "purpose");
    return p ? p : "\"focused\"";
}

static const struct etude_reading *charged_start(const struct set *x)
{
    const struct etude_preparation_receipt *r = etude_context_preparation(x->o->context);
    return r ? &r->start : &x->start;
}

static void budget_limits(struct set *x)
{
    x->goal_us = UINT64_C(3600000000);
    x->growth_us = UINT64_C(3600000000);
    x->ceiling_us = UINT64_C(14400000000);
    x->reserve_us = UINT64_C(600000000);
    if (!strcmp(purpose(x), "\"push\"") || !strcmp(purpose(x), "\"ci\"")) {
        int push = !strcmp(purpose(x), "\"push\"");
        x->goal_us = x->ceiling_us = push ? UINT64_C(300000000) : UINT64_C(600000000);
        x->reserve_us = push ? UINT64_C(30000000) : UINT64_C(60000000);
        x->growth_us = 0;
    }
    if (x->o->context && strcmp(purpose(x), "\"phase_first_run\""))
        x->deadline_us = charged_start(x)->monotonic_us + x->ceiling_us - x->reserve_us;
}

static int exemption_lists(const struct eh_json *array, const char *text)
{
    if (!array || array->type != 'a')
        return 0;
    for (const struct eh_json *v = array->child; v; v = v->next)
        if (v->type == 's' && !strcmp(v->string, text))
            return 1;
    return 0;
}

/* Bind the first-run exception to this exact selection and its allocations. */
static int exemption_matches(const struct set *x, const struct cmp_text *selection_doc)
{
    if (strcmp(purpose(x), "\"phase_first_run\""))
        return 1;
    const char *text = etude_context_text(x->o->context, "exemption");
    struct eh_json *ex = text ? eh_json_parse(text) : NULL;
    char hash[65];
    digest_of(selection_doc->data, selection_doc->length, hash);
    const char *digest = cmp_digest(ex, "manifest");
    const struct eh_json *manifest = eh_json_get(ex, "manifest");
    const struct eh_json *size = eh_json_get(manifest, "size_bytes");
    int ok = digest && !strcmp(hash, digest) && size && size->type == 'n' &&
             size->number == (int64_t)selection_doc->length &&
             exemption_lists(eh_json_get(ex, "run_ids"), x->run_id);
    const struct eh_json *runs = eh_json_get(ex, "run_ids");
    const struct eh_json *attempts = eh_json_get(ex, "attempt_ids");
    size_t count = 0;
    for (const struct eh_json *id = attempts ? attempts->child : NULL; id; id = id->next)
        count++;
    ok = ok && runs && runs->type == 'a' && runs->child && !runs->child->next && attempts &&
         attempts->type == 'a' && count == x->n;
    for (size_t i = 0; ok && i < x->n; i++)
        ok = exemption_lists(attempts, x->mem[i].c.id);
    eh_json_free(ex);
    return ok;
}

/* The limits of a run that routine testing has not classed: the example
 * policy's for a focused run. */
static const char wide_class[] = "{\"goal_us\":3600000000,\"growth_us\":3600000000,"
                                 "\"ceiling_us\":14400000000,\"cleanup_reserve_us\":600000000}";

/** Keep the run's budget policy, the example's classes on this machine's
 * limits, with its reference in `x->policy`. */
static int budget_policy(struct set *x)
{
    struct cmp_text d = {NULL, 0, 0, 0};

    cmp_printf(&d, "{\"schema_version\":1,\"policy\":\"budget-v1\",\"site\":{\"machine_id\":");
    string(&d, x->machine_id);
    cmp_printf(&d, ",\"os\":\"%s\",\"accelerator\":", x->os);
    string(&d, x->o->accel);
    cmp_printf(&d, ",\"capacity\":{");
    capacity(x, &d);
    cmp_printf(&d,
               "}},\"classes\":{\"push\":{\"goal_us\":300000000,\"growth_us\":0,\"ceiling_us\":"
               "300000000,\"cleanup_reserve_us\":30000000},\"ci\":{\"goal_us\":600000000,"
               "\"growth_us\":0,\"ceiling_us\":600000000,\"cleanup_reserve_us\":60000000},"
               "\"release\":%s,\"focused\":%s,\"phase_first_run\":%s},\"one_time_setup\":[]}\n",
               wide_class, wide_class, wide_class);
    return keep_ref(x, &d, x->policy, sizeof(x->policy));
}

/** Give the architecture that case `k`'s item builds and runs for: its own,
 * or the set's first for a case of no architecture. */
static const char *item_arch(const struct set *x, const struct legacy_case *k)
{
    return strcmp(k->arch, "none") != 0 ? k->arch : x->arches[0];
}

/** Append variable `var`, `NAME=VALUE`, as an entry of an environment,
 * after a comma unless `first`. */
static void variable(struct cmp_text *d, const char *var, int first)
{
    char name[64];

    snprintf(name, sizeof(name), "%.*s", (int)strcspn(var, "="), var);
    cmp_printf(d, "%s{\"name\":", first ? "" : ",");
    string(d, name);
    cmp_printf(d, ",\"value\":");
    string(d, var + strcspn(var, "=") + (var[strcspn(var, "=")] != '\0'));
    cmp_printf(d, "}");
}

/** Append the reproduction of host case member `m`: its program and
 * arguments, run in the tree with its variables, ETUDE_TREE among them,
 * after make of its artifacts. */
static void reproduce_case(const struct set *x, const struct member *m, struct cmp_text *d)
{
    const struct legacy_case *k = m->k;
    size_t i;

    cmp_printf(d, ",\"environment\":[");
    for (i = 0; i < CASE_VARS && m->vars[i][0] != '\0'; i++)
        if (strncmp(m->vars[i], "TMPDIR=", 7) != 0)
            variable(d, m->vars[i], i == 0);
    cmp_printf(d, "],\"preparation\":[[\"make\",\"-C\",");
    string(d, x->tree);
    cmp_printf(d, ",\"ARCH=%s\"", item_arch(x, k));
    for (i = 0; i < k->ngoals; i++) {
        cmp_printf(d, ",");
        string(d, k->goals[i]);
    }
    cmp_printf(d, "]],\"case\":[");
    string(d, m->program);
    for (i = 0; i < k->nargs; i++) {
        cmp_printf(d, ",");
        string(d, k->args[i]);
    }
    cmp_printf(d, "],\"profile\":{\"reason\":\"the set driver runs no profiler\"}}");
}

/** Append the reproduction commands of each item: the program of a host or
 * machine case, or tests/run of its suite for a guest case. */
static void reproduce(const struct set *x, struct cmp_text *d)
{
    size_t i;

    const char *cache = etude_context_text(x->o->context, "cache_evidence");
    cmp_printf(d, "{\"schema_version\":1,\"cache_evidence\":%s", cache ? cache : "null");
    if (x->o->repeat) {
        struct etude_repeat_spec spec;
        etude_collection_repeat_spec(x->o->repeat, x->repeat_cases, x->repeat_count, x->o->width,
                                     &spec);
        char *record = etude_repeat_record(&spec, x->run_id, x->repeat_items, x->n);
        if (!record) {
            d->failed = 1;
            return;
        }
        cmp_printf(d, ",\"repetition\":%s", record);
        free(record);
    }
    cmp_printf(d, ",\"items\":[");
    for (i = 0; i < x->n; i++) {
        cmp_printf(d, "%s{\"item_id\":", i ? "," : "");
        string(d, x->mem[i].item_id);
        cmp_printf(d, ",\"directory\":");
        string(d, !x->mem[i].k->guest ? x->tree : x->o->tree);
        if (!x->mem[i].k->guest) {
            reproduce_case(x, &x->mem[i], d);
            continue;
        }
        cmp_printf(d, ",\"environment\":[{\"name\":\"ARCH\",\"value\":\"%s\"}",
                   item_arch(x, x->mem[i].k));
        if (x->o->kernel != NULL) {
            cmp_printf(d, ",{\"name\":\"RACH2_KERNEL\",\"value\":");
            string(d, x->o->kernel);
            cmp_printf(d, "}");
        }
        cmp_printf(d, "],\"preparation\":[],\"case\":[\"tests/run\",");
        string(d, x->mem[i].m->suite);
        cmp_printf(d, "],\"profile\":{\"reason\":\"tests/run runs no profiler\"}}");
    }
    cmp_printf(d, "]}\n");
}

/** Append the names of the members for which `pick` is nonzero. */
static void names(const struct set *x, struct cmp_text *d, int (*pick)(const struct member *))
{
    size_t i, n = 0;

    cmp_printf(d, "[");
    for (i = 0; i < x->n; i++)
        if (pick(&x->mem[i])) {
            cmp_printf(d, n++ ? "," : "");
            string(d, x->mem[i].c.name);
        }
    cmp_printf(d, "]");
}

/* These groups contain host-alone checks with short response windows.
 * They are exclusive within this collection, like an explicit serial suite. */
static int quiet_case(const struct legacy_case *k)
{
    return !strcmp(k->case_id, "host.host.etude") || !strcmp(k->case_id, "host.host.boots");
}

/** Say whether suite or quiet-group policy makes member `m` serial. */
static int is_serial(const struct member *m)
{
    return m->m->serial || quiet_case(m->k);
}

/** Say whether member `m` runs with nothing else of the run beside it. */
static int is_exclusive(const struct member *m)
{
    return m->c.exclusive;
}

/** Give the family of member `m`'s item, its manifest's. */
static const char *family_of(const struct member *m)
{
    return m->manifest->family;
}

static int runs_case(const struct set *x, size_t i, const struct legacy_case *k);

/** Serialize the requested inventory in request order. Workspace paths and
 * history-dependent execution order do not change the selected work. */
static void selection(const struct set *x, struct cmp_text *d)
{
    size_t i, c, n = 0;
    cmp_printf(d, "{\"schema_version\":1,\"architecture\":\"%s\",\"suites\":[", x->o->arch);
    for (i = 0; i < x->nmembers; i++) {
        const struct etude_legacy_member *m = &x->members[i];
        const struct legacy_manifest *man = &x->manifests[i];
        cmp_printf(d, "%s{\"suite\":", n++ ? "," : "");
        string(d, m->suite);
        cmp_printf(d, ",\"architecture\":\"%s\",\"serial\":%s,\"stops\":%s,\"cases\":[", m->arch,
                   m->serial ? "true" : "false", m->stops ? "true" : "false");
        size_t count = 0;
        for (c = 0; c < man->ncases; c++) {
            const struct legacy_case *k = &man->cases[c];
            if (!runs_case(x, i, k))
                continue;
            cmp_printf(d, "%s{\"case_id\":", count++ ? "," : "");
            string(d, k->case_id);
            cmp_printf(d, ",\"architecture\":\"%s\",\"repetitions\":%lld}", k->arch,
                       k->reps * (x->o->repeat && x->o->repeat->count ? x->o->repeat->count : 1));
        }
        cmp_printf(d, "]}");
    }
    cmp_printf(d, "],\"stop_on_fail\":%s", x->o->stop_on_fail ? "true" : "false");
    if (x->repeat_selection)
        cmp_printf(d, ",\"repetition\":%s", x->repeat_selection);
    cmp_printf(d, "}\n");
}

/** Load prior complete histories before this run opens its own stream. */
static int load_history(struct set *x)
{
    char directory[PATH_LEN], path[PATH_LEN];
    char **paths = NULL;
    size_t count = 0;
    DIR *d;
    struct dirent *e;
    int rc;
    if (x->o->history != NULL)
        rc = fit(directory, sizeof(directory), "%s", x->o->history);
    else
        rc = fit(directory, sizeof(directory), "%s/history", x->o->results);
    if (rc != 0)
        return -1;
    d = opendir(directory);
    if (d == NULL)
        return errno == ENOENT ? 0 : -1;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 7 || strcmp(e->d_name + n - 6, ".jsonl"))
            continue;
        if (count == 4096 || fit(path, sizeof(path), "%s/%s", directory, e->d_name) != 0) {
            rc = -1;
            goto done;
        }
        char **more = realloc(paths, (count + 1) * sizeof(*paths));
        if (more == NULL) {
            rc = -1;
            goto done;
        }
        paths = more;
        paths[count] = strdup(path);
        if (paths[count] == NULL) {
            rc = -1;
            goto done;
        }
        count++;
    }
    rc = count ? cmp_load(&x->baseline, x->schema, paths, (int)count) : 0;
    for (size_t i = 0; i < x->baseline.note_count; i++)
        say(x, "etude run: history: %s", x->baseline.notes[i]);
done:
    closedir(d);
    for (size_t i = 0; i < count; i++)
        free(paths[i]);
    free(paths);
    return rc;
}

/** A gap is eligible only when its successful attempt lists the step. */
static int history_gap(const struct cmp_history *h, const struct cmp_sample *s, uint64_t *gap)
{
    const struct eh_json *steps = eh_json_get(s->attempt->data, "steps");
    const struct eh_json *id;
    uint64_t largest = 0;
    int found = 0;
    for (id = steps ? steps->child : NULL; id; id = id->next) {
        const struct cmp_record *r = id->type == 's' ? cmp_find(h, id->string) : NULL;
        if (!r || strcmp(r->kind, "step") || strcmp(r->run, s->attempt->run))
            continue;
        const char *attempt = cmp_text(r->data, "attempt_id");
        const char *plan = cmp_text(r->data, "plan_id");
        const struct eh_json *step_result = eh_json_get(r->data, "result");
        const char *outcome = cmp_text(step_result, "outcome"),
                   *cleanup = cmp_text(step_result, "cleanup");
        const struct eh_json *disruptions = eh_json_get(step_result, "disruptions");
        const struct eh_json *value =
            eh_json_get(eh_json_get(r->data, "max_progress_gap_us"), "value");
        if (!attempt || strcmp(attempt, cmp_text(s->attempt->data, "attempt_id")) || !plan ||
            strcmp(plan, s->plan->id) || !outcome || strcmp(outcome, "pass") || !cleanup ||
            strcmp(cleanup, "confirmed") || (disruptions && disruptions->child) || !value ||
            value->type != 'n' || value->number < 0)
            continue;
        if ((uint64_t)value->number > largest)
            largest = (uint64_t)value->number;
        found = 1;
    }
    *gap = largest;
    return found;
}

/** Apply matching undisrupted successes to the bounds before attempts start. */
static int learn_bounds(struct set *x, const char *plan_text)
{
    struct eh_json *plan = eh_json_parse(plan_text);
    const struct eh_json *items = eh_json_get(plan, "items"), *item;
    uint64_t *durations = calloc(x->baseline.sample_count + 1, sizeof(*durations));
    uint64_t *gaps = calloc(x->baseline.sample_count + 1, sizeof(*gaps));
    const char **runs = calloc(x->baseline.sample_count + 1, sizeof(*runs));
    size_t i = 0;
    int rc = -1;
    if (!plan || !durations || !gaps || !runs)
        goto done;
    for (item = items ? items->child : NULL; item && i < x->n; item = item->next, i++) {
        struct cmp_key key;
        size_t n = 0, ngaps = 0;
        cmp_item_key(plan, item, &key);
        for (size_t j = 0; j < x->baseline.sample_count; j++) {
            const struct cmp_sample *sample = &x->baseline.samples[j];
            char differences[256];
            const char *cleanup = cmp_text(eh_json_get(sample->attempt->data, "result"), "cleanup");
            const char *run_cleanup =
                sample->run ? cmp_text(eh_json_get(sample->run->data, "result"), "cleanup") : NULL;
            if (!cleanup || strcmp(cleanup, "confirmed") || !run_cleanup ||
                strcmp(run_cleanup, "confirmed"))
                continue;
            if (!sample->watchdog_eligible ||
                cmp_key_differs(&key, &sample->key, 0, 10, differences, sizeof(differences)) != 0 ||
                cmp_key_differs(&key, &sample->key, 11, 12, differences, sizeof(differences)) != 0)
                continue;
            size_t k;
            for (k = 0; k < n && strcmp(runs[k], sample->attempt->run); k++) {
            }
            if (k < n)
                continue;
            runs[n] = sample->attempt->run;
            durations[n++] = sample->elapsed_us;
            uint64_t gap;
            if (history_gap(&x->baseline, sample, &gap))
                gaps[ngaps++] = gap;
        }
        struct legacy_case *k = (struct legacy_case *)x->mem[i].k;
        if (legacy_case_learn(k, durations, n, gaps, ngaps) != 0)
            goto done;
        x->mem[i].history_samples = n;
        x->mem[i].gap_samples = ngaps;
        x->mem[i].c.wd.case_us = k->case_us;
        x->mem[i].c.wd.gap_us = k->gap_us;
        say(x, "etude run: bounds: %s %s: case %llu us (%lu samples), gap %llu us (%lu samples)",
            k->case_id, k->arch, (unsigned long long)k->case_us, (unsigned long)n,
            (unsigned long long)k->gap_us, (unsigned long)ngaps);
    }
    rc = i == x->n ? 0 : -1;
done:
    free(durations);
    free(gaps);
    free(runs);
    eh_json_free(plan);
    return rc;
}

static int runner_document(struct set *x, struct cmp_text *t, struct cmp_text *d)
{
    const struct etude_legacy_set_opts *o = x->o;
    size_t i;
    cmp_printf(d, "{\"schema_version\":1,\"runner\":\"etude run\",\"program\":");
    string(d, o->program);
    cmp_printf(d, ",\"wait_s\":%u,\"bound_s\":%s,\"watchdog_policy\":\"%s\",\"bounds\":[", o->wait,
               x->seconds, ETUDE_BOUND_POLICY);
    for (i = 0; i < x->n; i++) {
        const struct member *m = &x->mem[i];
        cmp_printf(d, "%s{\"item_id\":", i ? "," : "");
        string(d, m->item_id);
        cmp_printf(d,
                   ",\"case_us\":%llu,\"case_samples\":%lu,\"gap_us\":%llu,"
                   "\"gap_samples\":%lu,\"expected_us\":%llu,\"cost_samples\":%lu,"
                   "\"cpu_millis\":%llu,\"cpu_samples\":%lu,\"reserved_cpu_millis\":%llu}",
                   (unsigned long long)m->c.wd.case_us, (unsigned long)m->history_samples,
                   (unsigned long long)m->c.wd.gap_us, (unsigned long)m->gap_samples,
                   (unsigned long long)m->cost.elapsed_us, (unsigned long)m->cost.samples,
                   (unsigned long long)m->cost.cpu_millis, (unsigned long)m->cost.cpu_samples,
                   (unsigned long long)m->c.res[0].amount);
    }
    cmp_printf(d, "]}\n");
    return part(x, t, "runner", d);
}

static int plan_environment(struct set *x, struct cmp_text *t, struct cmp_text *d)
{
    const struct etude_legacy_set_opts *o = x->o;
    int rc = 0;
    cmp_printf(t, "],\"environment\":{\"machine_id\":");
    string(t, x->machine_id);
    cmp_printf(t, ",\"boot_id\":");
    string(t, x->boot_id);
    cmp_printf(t, ",\"os\":\"%s\",\"os_version\":", x->os);
    string(t, x->os_version);
    cmp_printf(t, ",\"accelerator\":");
    string(t, o->accel);
    cmp_printf(d, "{\"schema_version\":1,\"ledger\":");
    string(d, o->ledger);
    cmp_printf(d, ",\"locks\":");
    if (o->locks != NULL)
        string(d, o->locks);
    else
        cmp_printf(d, "null");
    cmp_printf(d, ",\"account\":");
    string(d, o->account);
    cmp_printf(d, "}\n");
    rc |= context_part(x, t, "site_profile", d);
    cmp_printf(d, "{\"schema_version\":1,\"accelerator\":");
    string(d, o->accel);
    cmp_printf(d, ",\"processors\":%u}\n", o->cpus);
    rc |= context_part(x, t, "tools", d);
    cmp_printf(d, "{\"schema_version\":1,");
    capacity(x, d);
    cmp_printf(d, "}\n");
    rc |= part(x, t, "capacity", d);
    cmp_printf(d, "{\"schema_version\":1,\"state\":\"the driver does not measure caches\"}\n");
    rc |= context_part(x, t, "cache_state", d);
    cmp_printf(d, "{\"schema_version\":1,\"method\":\"native-load-v1\","
                  "\"stage\":\"planned\"}\n");
    rc |= part(x, t, "load", d);
    cmp_printf(t, "}");
    return rc;
}

static int has_node(const char *const *nodes, size_t count, const char *name)
{
    for (size_t i = 0; i < count; i++)
        if (!strcmp(nodes[i], name))
            return 1;
    return 0;
}

/** Bind aggregate preparation to all selected goals and architecture flags. */
static int preparation_nodes(struct set *x, struct cmp_text *t)
{
    struct cmp_text d = {0};
    size_t slots = 0, count = 0;
    for (size_t i = 0; i < x->n; i++)
        slots += x->mem[i].k->ngoals;
    const char **nodes = calloc(slots + 1, sizeof(*nodes));
    if (!nodes)
        return -1;
    cmp_printf(&d,
               "{\"schema_version\":1,\"format\":\"etude-preparation-nodes\",\"architectures\":[");
    for (size_t i = 0; i < x->narches; i++) {
        cmp_printf(&d, "%s", i ? "," : "");
        string(&d, x->arches[i]);
    }
    cmp_printf(&d, "],\"kernel\":");
    if (x->o->kernel)
        string(&d, x->o->kernel);
    else
        cmp_printf(&d, "null");
    cmp_printf(&d, ",\"nodes\":[");
    for (size_t i = 0; i < x->n; i++) {
        const struct legacy_case *k = x->mem[i].k;
        for (size_t j = 0; j < k->ngoals; j++)
            if (!has_node(nodes, count, k->goals[j])) {
                cmp_printf(&d, "%s", count ? "," : "");
                string(&d, k->goals[j]);
                nodes[count++] = k->goals[j];
            }
    }
    const char *preparation = etude_context_text(x->o->context, "preparation");
    cmp_printf(&d, "],\"preparation\":%s}\n", preparation ? preparation : "null");
    free(nodes);
    cmp_printf(t, ",");
    int rc = keep_doc(x, &d, t);
    return rc;
}

/* Serialize item identities identically for history matching and the plan. */
static void plan_items(const struct set *x, struct cmp_text *t)
{
    size_t i;
    cmp_printf(t, "\"items\":[");
    for (i = 0; i < x->n; i++) {
        const struct member *m = &x->mem[i];

        cmp_printf(t, "%s{\"item_id\":", i ? "," : "");
        string(t, m->item_id);
        cmp_printf(t, ",\"case_id\":");
        string(t, m->case_id);
        cmp_printf(t, ",\"family_id\":");
        string(t, family_of(m));
        cmp_printf(t,
                   ",\"architecture\":\"%s\",\"domain\":\"%s\",\"workload\":%s,"
                   "\"assertions\":%s,\"profile\":%s,\"repetition\":%lld,\"attempt_ids\":[",
                   m->arch, m->k->guest || m->k->machine ? "qemu" : "host", m->workload,
                   m->assertions, m->profile, m->rep);
        string(t, m->c.id);
        cmp_printf(t, "]}");
    }
    cmp_printf(t, "]");
}

/* Read compatible costs before sorting members or retaining command pointers.
 * Item documents and environment use exactly the final plan's serializers. */
static int learn_costs(struct set *x)
{
    struct cmp_text t = {0}, d = {0};
    struct eh_json *plan = NULL;
    int rc = -1;
    for (size_t i = 0; i < x->n; i++)
        if (item_docs(x, &x->mem[i]))
            goto done;
    cmp_printf(&t, "{");
    plan_items(x, &t);
    cmp_printf(&t, ",\"artifacts\":[");
    if (plan_environment(x, &t, &d))
        goto done;
    cmp_printf(&t, "}");
    plan = t.failed ? NULL : eh_json_parse(t.data);
    if (!plan)
        goto done;
    const struct eh_json *items = eh_json_get(plan, "items"), *item;
    size_t i = 0;
    for (item = items ? items->child : NULL; item && i < x->n; item = item->next, i++) {
        struct member *m = &x->mem[i];
        struct cmp_key key;
        cmp_item_key(plan, item, &key);
        if (etude_cost_of(&x->baseline, &key, m->k->expected_us, m->k->cpu_millis,
                          m->k->cpu_ceiling_millis, &m->cost))
            goto done;
        say(x, "etude run: cost: %s %s: %llu us (%lu samples), CPU %llu (%lu samples)",
            m->k->case_id, m->k->arch, (unsigned long long)m->cost.elapsed_us,
            (unsigned long)m->cost.samples, (unsigned long long)m->cost.cpu_millis,
            (unsigned long)m->cost.cpu_samples);
    }
    rc = i == x->n ? 0 : -1;
done:
    eh_json_free(plan);
    free(t.data);
    free(d.data);
    return rc;
}

/** Write the plan record. */
static int write_plan(struct set *x)
{
    const struct etude_legacy_set_opts *o = x->o;
    struct cmp_text t = {NULL, 0, 0, 0}, d = {NULL, 0, 0, 0};
    int rc = budget_policy(x);
    cmp_printf(&t, "{\"purpose\":%s", purpose(x));
    selection(x, &d);
    if (!exemption_matches(x, &d)) {
        free(t.data);
        free(d.data);
        errno = EINVAL;
        return -1;
    }
    rc |= part(x, &t, "selection", &d);
    cmp_printf(&t, ",");
    plan_items(x, &t);
    cmp_printf(&t, ",\"commit\":\"%s\"",
               o->commit != NULL ? o->commit : "0000000000000000000000000000000000000000");
    cmp_printf(&d, "{\"schema_version\":1,\"tree\":");
    string(&d, o->tree);
    cmp_printf(&d, ",\"commit\":");
    if (o->commit != NULL)
        string(&d, o->commit);
    else
        cmp_printf(&d, "null");
    cmp_printf(&d, "}\n");
    rc |= context_part(x, &t, "source", &d);
    cmp_printf(&t, ",\"artifacts\":[");
    if (x->o->context) {
        char *bytes = NULL;
        size_t count = 0;
        if (cmp_read_file(x->record, EVIDENCE_BYTES, &bytes, &count))
            rc = -1;
        else
            rc |= keep(x, bytes, count, &t);
        free(bytes);
        rc |= preparation_nodes(x, &t);
    }
    rc |= plan_environment(x, &t, &d);
    cmp_printf(&d, "{\"schema_version\":1,\"width\":%u,\"serial\":", o->width);
    names(x, &d, is_serial);
    cmp_printf(&d, ",\"exclusive\":");
    names(x, &d, is_exclusive);
    if (x->repeat_selection)
        cmp_printf(&d, ",\"repetition\":%s,\"load_ready_us\":30000000,\"load_grace_us\":5000000",
                   x->repeat_selection);
    cmp_printf(&d, "}\n");
    rc |= part(x, &t, "concurrency", &d);
    const char *exemption = etude_context_text(o->context, "exemption");
    cmp_printf(&t, ",\"policies\":%s,\"exemption\":%s", x->policy, exemption ? exemption : "null");
    reproduce(x, &d);
    rc |= part(x, &t, "reproduce", &d);
    cmp_printf(&t, "}");
    if (rc == 0)
        rc = learn_bounds(x, t.data);
    if (rc == 0) {
        /* The temporary closing brace made the key available to the learner. */
        t.data[--t.length] = 0;
        rc = runner_document(x, &t, &d);
        cmp_printf(&t, "}");
    }
    if (rc == 0)
        rc = append(x, "plan", &t, x->plan_record, sizeof(x->plan_record));
    free(t.data);
    return rc;
}

/* ---- Commands. ---- */

/** Say whether a stop signal has come, and note it as the reason of the
 * cancellations it causes. */
static int signalled(struct set *x)
{
    if (x->deadline_us && posix_now_us() >= x->deadline_us) {
        x->budget_expired = 1;
        if (!x->stop_reason[0])
            snprintf(x->stop_reason, sizeof x->stop_reason,
                     "the run reached its budget with the cleanup reserve remaining");
        return 1;
    }
    if (x->o->stop == NULL || *x->o->stop == 0)
        return 0;
    if (x->stop_reason[0] == '\0')
        snprintf(x->stop_reason, sizeof(x->stop_reason), "the run was stopped by signal %d",
                 (int)*x->o->stop);
    return 1;
}

/** Note a change of the count of running cases, for the useful
 * concurrency. */
static void running_changed(struct set *x, int delta)
{
    uint64_t now = posix_now_us();

    if (x->last_us != 0)
        x->busy_us += (uint64_t)x->running * (now - x->last_us);
    x->last_us = now;
    x->running = (unsigned)((int)x->running + delta);
    if (x->running > x->peak)
        x->peak = x->running;
}

/** Note that command `c` was first asked for. */
static void asked(struct command *c)
{
    if (c->asked)
        return;
    c->asked = 1;
    etude_history_now(&c->queued);
    c->queued_us = posix_now_us();
    c->queued_epoch_us = etude_status_now();
}

/** Note the admission of command `c`, and the first grant of the run. */
static void granted(struct set *x, struct command *c)
{
    c->claim = 1;
    if (!c->was_admitted) {
        etude_history_now(&c->admitted);
        c->was_admitted = 1;
    }
    if (!x->granted) {
        x->first = c->admitted;
        x->granted = 1;
    }
}

/** Say whether command `c` has waited past the run's bound of waiting. */
static int waited_out(const struct set *x, const struct command *c)
{
    return c->asked && posix_now_us() - c->queued_us > (uint64_t)x->o->wait * 1000000u;
}

/** Give the milliseconds of waiting that command `c` has left, at least 1. */
static unsigned wait_left_ms(const struct set *x, const struct command *c)
{
    uint64_t used = (posix_now_us() - c->queued_us) / 1000, all = (uint64_t)x->o->wait * 1000;

    return used >= all ? 1u : (unsigned)(all - used);
}

/** Say whether `var`, `NAME=VALUE`, has the name of one of the `n`
 * variables `vars`. */
static int named(const char *var, char *const *vars, size_t n)
{
    size_t i, k;

    for (i = 0; i < n; i++) {
        k = strcspn(vars[i], "=") + 1;
        if (strncmp(var, vars[i], k) == 0)
            return 1;
    }
    return 0;
}

/* The variables a case's program is given only by its item, which the
 * driver's own environment does not pass on. */
static char *const case_only[] = {"ARCH=", "ETUDE_SEED="};

/** Fill `c->envp` with this process's environment, with each of the `nvars`
 * variables `vars`, `NAME=VALUE`, in place of one of its name, and for a
 * case, `c->is_case`, without the variables of case_only. */
static int environment(struct command *c, char *const *vars, size_t nvars)
{
    size_t n = 0, i, k = 0;

    while (environ[n] != NULL)
        n++;
    free(c->envp);
    c->envp = calloc(n + nvars + 1, sizeof(*c->envp));
    if (c->envp == NULL)
        return -1;
    for (i = 0; i < n; i++)
        if (!named(environ[i], vars, nvars) && !(c->is_case && named(environ[i], case_only, 2)))
            c->envp[k++] = environ[i];
    for (i = 0; i < nvars; i++)
        c->envp[k++] = vars[i];
    c->envp[k] = NULL;
    return 0;
}

/**
 * Take the old runner's locks for command `c`, whose claim is admitted,
 * waiting `wait_ms` milliseconds for them, or trying once with 0.
 *
 * @return 1 once they are held, or when the run takes none; 0 when they are
 *         taken by others or a signal came; -1 with `c->reason` set.
 */
static int lock(const struct set *x, struct command *c, unsigned wait_ms)
{
    struct etude_bridge_spec b;

    if (x->o->locks == NULL)
        return 1;
    memset(&b, 0, sizeof(b));
    b.ledger = x->o->ledger;
    b.id = c->id;
    b.dir = x->o->locks;
    b.alone = c->alone;
    b.legacy = c->ports;
    b.wait_ms = wait_ms;
    c->tried_us = posix_now_us();
    c->bridge = etude_bridge_start(&b);
    if (c->bridge != NULL)
        return 1;
    if (errno == EAGAIN || errno == EINTR || errno == ECANCELED)
        return 0;
    if (errno == ETIMEDOUT)
        snprintf(c->reason, sizeof(c->reason), "the old runner's locks in %s stayed taken for %u s",
                 x->o->locks, x->o->wait);
    else
        snprintf(c->reason, sizeof(c->reason),
                 "the old runner's locks in %s could not be taken: %s", x->o->locks,
                 strerror(errno));
    return -1;
}

/* End the registered load before its shared reservation can be released. */
static enum etude_cleanup stop_load(struct command *c, enum etude_cleanup cleanup)
{
    if (!c->load || c->load_stopped)
        return cleanup;
    c->load_stopped = 1;
    if (etude_collection_load_stop(c->load) != ETUDE_CLEANUP_CONFIRMED)
        cleanup = ETUDE_CLEANUP_UNCONFIRMED;
    struct etude_measure load = etude_collection_load_cpu(c->load);
    if (c->cpu_us.quality && load.quality) {
        c->cpu_us.value += load.value;
        if (load.quality == ETUDE_QUALITY_LOWER_BOUND)
            c->cpu_us.quality = ETUDE_QUALITY_LOWER_BOUND;
        c->cpu_us.method = "case-and-reserved-load-v1";
    } else {
        c->cpu_us.quality = ETUDE_QUALITY_UNAVAILABLE;
        c->cpu_us.method = "case or reserved-load CPU accounting unavailable";
    }
    if (etude_collection_load_failed(c->load)) {
        c->outcome = INCOMPLETE;
        snprintf(c->reason, sizeof(c->reason),
                 "the reserved load failed or ended before cancellation");
        snprintf(c->disruption, sizeof(c->disruption), "%s", c->reason);
    }
    if (cleanup != ETUDE_CLEANUP_CONFIRMED) {
        c->outcome = INCOMPLETE;
        snprintf(c->disruption, sizeof(c->disruption), "reserved-load cleanup is unconfirmed");
    }
    etude_history_now(&c->done);
    return cleanup;
}

/** End the claim of command `c` with `cleanup`, and let its locks go with
 * it, or leave them with the claim when the cleanup is not confirmed. */
static enum etude_cleanup release(struct set *x, struct command *c, enum etude_cleanup cleanup)
{
    int ok = 1;
    cleanup = stop_load(c, cleanup);
    c->cleanup = cleanup;
    if (c->claim && etude_ledger_finish(x->l, c->id, cleanup) != 0) {
        ok = 0;
        x->failed = 1;
        say(x, "etude run: the claim %s could not be ended: %s", c->id, strerror(errno));
    }
    c->claim = 0;
    if (!ok || cleanup != ETUDE_CLEANUP_CONFIRMED)
        x->uncertain = 1;
    if (c->bridge != NULL) {
        if (ok && cleanup == ETUDE_CLEANUP_CONFIRMED)
            etude_bridge_join(c->bridge);
        else
            etude_bridge_detach(c->bridge);
        c->bridge = NULL;
    }
    return cleanup;
}

/** End command `c` before it ran, with `outcome` for `reason`; a reason of
 * the machine rather than the case is a disruption too. */
static void not_run(struct set *x, struct command *c, enum outcome outcome, const char *reason,
                    int disrupted)
{
    enum etude_cleanup cleanup = ETUDE_CLEANUP_CONFIRMED;
    if (c->claim || c->bridge != NULL)
        cleanup = release(x, c, cleanup);
    int load_failed = etude_collection_load_failed(c->load);
    c->outcome = cleanup == ETUDE_CLEANUP_CONFIRMED && !load_failed ? outcome : INCOMPLETE;
    if (!load_failed && reason != c->reason)
        snprintf(c->reason, sizeof(c->reason), "%s", reason);
    if (disrupted && !load_failed)
        snprintf(c->disruption, sizeof(c->disruption), "%s", reason);
    c->end = ETUDE_END_STOPPED;
    c->value = -1;
    c->cleanup = cleanup;
    c->state = DONE;
    etude_history_now(&c->done);
}

/** Start command `c`, whose claim and locks are held, with the `n`
 * variables `vars` added to the environment; 0, or -1 with errno set. */
static int start(struct set *x, struct command *c, char *const *vars, size_t n)
{
    const struct etude_supervised_result *r;

    if (environment(c, vars, n) != 0)
        return -1;
    memset(&c->spec, 0, sizeof(c->spec));
    c->spec.attempt_id = c->id;
    c->spec.argv = c->argv;
    c->spec.envp = c->envp;
    c->spec.cwd = c->cwd;
    c->spec.output_max = 0;
    c->spec.stdout_path = c->out;
    c->spec.stderr_path = c->out;
    c->spec.grace_us = c->grace_us;
    c->spec.reg = c->load ? etude_collection_load_register_case : etude_ledger_register;
    c->spec.child = etude_ledger_child;
    c->spec.reg_ctx = x->l;
    if (!c->load && etude_ledger_verdict_path(x->l, c->id, c->verdict, sizeof(c->verdict)) == 0)
        c->spec.verdict = c->verdict;
    if (etude_supervise_host(x->sup, &c->spec, &c->wd, &c->sup) != 0) {
        c->sup = NULL;
        return -1;
    }
    r = etude_supervised_result(c->sup);
    c->ran = 1;
    c->keeper = r->proc->keeper;
    c->process = r->proc->process;
    c->started_us = r->proc->started_us;
    c->state = RUNNING;
    return 0;
}

/** Print the line of command `c`'s output that has been gathered. */
static void flush_line(struct set *x, struct command *c)
{
    c->line[c->nline] = '\0';
    fputs(c->line, x->out);
    fputc('\n', x->out);
    c->nline = 0;
}

/** Feed the output command `c` has written since the last feed to its
 * watchdog, whose records of progress are a case's progress. */
static void feed(struct command *c)
{
    char buf[4096];
    size_t got;
    FILE *f;

    if (c->sup == NULL || (f = fopen(c->out, "rb")) == NULL)
        return;
    if (fseek(f, c->fed, SEEK_SET) == 0)
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
            etude_supervised_feed(c->sup, buf, got);
            c->fed += (long)got;
        }
    fclose(f);
}

/** Print the whole lines of command `c`'s output not printed yet, and with
 * `all` the rest as well. */
static void drain(struct set *x, struct command *c, int all)
{
    char buf[4096];
    size_t got, i;
    FILE *f;

    if (!c->ran || (f = fopen(c->out, "rb")) == NULL)
        return;
    if (fseek(f, c->printed, SEEK_SET) == 0)
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
            c->printed += (long)got;
            for (i = 0; i < got; i++) {
                if (buf[i] == '\n') {
                    flush_line(x, c);
                    continue;
                }
                if (c->nline == sizeof(c->line) - 1)
                    flush_line(x, c);
                c->line[c->nline++] = buf[i];
            }
        }
    fclose(f);
    if (all && c->nline > 0)
        flush_line(x, c);
    fflush(x->out);
}

/** Copy into `line` what follows the last result line of `etude prepare` in
 * the preparation `c`'s output, or "" when it wrote none. */
static void last_result(const struct command *c, char *line, size_t size)
{
    char *bytes, *p, *found = NULL;
    size_t n, k = strlen(RESULT);

    line[0] = '\0';
    if (read_tail(c->out, TAIL_BYTES, &bytes, &n) != 0)
        return;
    for (p = bytes; (p = strstr(p, RESULT)) != NULL; p += k)
        if (p == bytes || p[-1] == '\n')
            found = p + k;
    if (found != NULL)
        snprintf(line, size, "%.*s", (int)strcspn(found, "\n"), found);
    free(bytes);
}

/** Set the outcome of command `c` to `outcome` for `reason`; a reason of
 * the machine rather than the case is a disruption too. */
static void came_out(struct command *c, enum outcome outcome, int disrupted, const char *fmt, ...)
{
    va_list ap;

    c->outcome = outcome;
    va_start(ap, fmt);
    vsnprintf(c->reason, sizeof(c->reason), fmt, ap);
    va_end(ap);
    if (disrupted)
        snprintf(c->disruption, sizeof(c->disruption), "%s", c->reason);
}

/** Give what follows `word` at the start of `line`, or NULL. */
static const char *after(const char *line, const char *word)
{
    size_t k = strlen(word);

    return strncmp(line, word, k) == 0 ? line + k : NULL;
}

/** Judge the preparation `c`, which exited with status `v`, by its last
 * result line `line`. */
static void judge_exit(struct command *c, const char *line, int v)
{
    const char *rest;

    if (v == 0 && after(line, "prepared: ") != NULL)
        c->outcome = PASS;
    else if (v == 1 && (rest = after(line, "failed: ")) != NULL)
        came_out(c, FAILURE, 0, "%s", rest);
    else if (v == 2 && (rest = after(line, "refused: ")) != NULL)
        came_out(c, FAILURE, 0, "it was refused: %s", rest);
    else if (v > 128 && after(line, "stopped by signal ") != NULL)
        came_out(c, INCOMPLETE, 1, "etude prepare was stopped by signal %d", v - 128);
    else if (line[0] != '\0')
        came_out(c, INCOMPLETE, 1, "etude prepare exited with status %d after the result \"%s\"", v,
                 line);
    else
        came_out(c, INCOMPLETE, 1, "etude prepare exited with status %d and no result", v);
}

/** Take the end of command `c` from its result `r`: a host process's, or
 * a machine's, whose processor time is QEMU's. */
static void took(struct command *c, const struct etude_supervised_result *r)
{
    c->end = r->end;
    c->value = r->value;
    c->cleanup = r->cleanup;
    if (r->proc != NULL)
        c->cpu_us = r->proc->cpu_us;
    else if (r->machine != NULL && r->machine->qemu != NULL)
        c->cpu_us = r->machine->qemu->cpu_us;
    c->ended_us = r->finished_us;
    c->max_gap_us = r->max_gap_us;
    etude_history_now(&c->done);
}

/** Settle the outcome of command `c` with its cancellation and its
 * cleanup: a cancelled command that neither passed nor failed is cancelled,
 * and one whose cleanup is unconfirmed cannot pass. */
static void settle(const struct set *x, struct command *c)
{
    if (c->cancelled && c->outcome != PASS && c->outcome != FAILURE) {
        c->disruption[0] = '\0';
        came_out(c, CANCELLED, 0, "%s", x->stop_reason);
    }
    if (c->outcome == PASS && c->cleanup != ETUDE_CLEANUP_CONFIRMED)
        came_out(c, INCOMPLETE, 1, "its cleanup is unconfirmed");
    else if (c->cleanup != ETUDE_CLEANUP_CONFIRMED && c->disruption[0] == '\0')
        snprintf(c->disruption, sizeof(c->disruption), "its cleanup is unconfirmed");
}

/** Judge the preparation `c`, which has finished, by its end and its last
 * result line. */
static void judge_prep(struct set *x, struct command *c)
{
    const struct etude_supervised_result *r = etude_supervised_result(c->sup);
    char line[REASON_LEN];
    int v = r->value;

    took(c, r);
    last_result(c, line, sizeof(line));
    if (r->incomplete && r->expiry != ETUDE_EXPIRY_NONE)
        came_out(c, INCOMPLETE, 1, "etude prepare did not end within its %s bound of %llu s",
                 etude_expiry_name(r->expiry), (unsigned long long)(r->bound_us / 1000000u));
    else if (r->end == ETUDE_END_SIGNALED)
        came_out(c, INCOMPLETE, 1, "etude prepare was killed by signal %d", v);
    else if (r->end != ETUDE_END_EXITED)
        came_out(c, INCOMPLETE, 1, "the end of etude prepare was not observed");
    else
        judge_exit(c, line, v);
    settle(x, c);
}

/** Give the end of a case as the judge takes it from result `r`: stopped
 * for a watchdog's expiry or a cancellation, lost when it was not observed,
 * and otherwise exited or signaled. */
static const char *end_of(const struct etude_supervised_result *r)
{
    if (r->expiry != ETUDE_EXPIRY_NONE || r->end == ETUDE_END_STOPPED)
        return "stopped";
    return r->end == ETUDE_END_EXITED     ? "exited"
           : r->end == ETUDE_END_SIGNALED ? "signaled"
                                          : "lost";
}

/** Give the outcome of an attempt that the judge judged `j`. */
static enum outcome outcome_of(enum legacy_judged j)
{
    return j == LEGACY_PASS ? PASS : j == LEGACY_FAILURE ? FAILURE : INCOMPLETE;
}

/** Give the end that the judge's name `end` stands for. */
static enum etude_end end_named(const char *end)
{
    return strcmp(end, "exited") == 0     ? ETUDE_END_EXITED
           : strcmp(end, "signaled") == 0 ? ETUDE_END_SIGNALED
           : strcmp(end, "stopped") == 0  ? ETUDE_END_STOPPED
                                          : ETUDE_END_LOST;
}

/** Compare two strings for qsort. */
static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/**
 * Give in `out` the consoles of machine case member `m`, the files
 * `MACHINE.N.console` that its scenario keeps in `evidence/` of its
 * workspace, in the order of their names, at most LEGACY_CONSOLES of them,
 * from malloc; their count, or 0 for none or a directory that cannot be
 * read.
 */
static size_t consoles_of(const struct member *m, char **out)
{
    char dir[PATH_LEN];
    struct dirent *e;
    size_t n = 0, len;
    DIR *d;

    if (fit(dir, sizeof(dir), "%s/evidence", m->work) != 0 || (d = opendir(dir)) == NULL)
        return 0;
    while ((e = readdir(d)) != NULL && n < LEGACY_CONSOLES) {
        len = strlen(e->d_name);
        if (len > 8 && strcmp(e->d_name + len - 8, ".console") == 0 &&
            (out[n] = malloc(strlen(dir) + len + 2)) != NULL) {
            snprintf(out[n], strlen(dir) + len + 2, "%s/%s", dir, e->d_name);
            n++;
        }
    }
    closedir(d);
    qsort(out, n, sizeof(*out), by_name);
    return n;
}

/**
 * Judge the case of member `m`, which has finished, by `etude check`'s
 * judgement of its output and its end. A guest case is judged as placed
 * under QEMU, with the machine's console, by the end etude-guest reported;
 * a fault of its session is a disruption beside the judge's reason. A
 * machine case is judged as placed under QEMU too, by its scenario's end,
 * with the consoles its scenario kept.
 */
static void judge_case(struct set *x, struct member *m)
{
    struct command *c = &m->c;
    const struct etude_supervised_result *r = etude_supervised_result(c->sup);
    const char *why = m->g != NULL ? legacy_guest_why(m->g) : "";
    char *consoles[LEGACY_CONSOLES];
    struct legacy_judge j;
    struct legacy_judgement v;
    size_t i;

    took(c, r);
    memset(&j, 0, sizeof(j));
    j.etude = x->o->program;
    j.schemas = x->o->schemas;
    j.manifest = m->manifest->path;
    j.case_id = m->case_id;
    j.arch = m->arch;
    j.consoles = consoles;
    j.output = c->out;
    j.run_id = x->run_id;
    j.attempt_id = c->id;
    j.item_id = m->item_id;
    j.placement = m->k->machine ? "qemu" : "host";
    if (m->k->machine)
        j.nconsoles = consoles_of(m, consoles);
    if (m->g != NULL) {
        j.placement = "qemu";
        consoles[0] = (char *)legacy_guest_console(m->g);
        j.nconsoles = 1;
        j.end = legacy_guest_end(m->g, r, &j.value);
        c->end = end_named(j.end);
        c->value = j.value;
    } else {
        j.end = end_of(r);
        j.value = strcmp(j.end, "exited") == 0 || strcmp(j.end, "signaled") == 0 ? r->value : -1;
    }
    j.results = m->judged_out;
    j.errors = m->judge_err;
    legacy_judge_run(&j, &v);
    for (i = 0; m->k->machine && i < j.nconsoles; i++)
        free(consoles[i]);
    m->judged = v.doc;
    m->njudged = v.len;
    if (v.outcome == LEGACY_UNJUDGED)
        came_out(c, INCOMPLETE, 1, "its results could not be judged: %s", v.reason);
    else if (r->incomplete && r->expiry != ETUDE_EXPIRY_NONE)
        came_out(c, outcome_of(v.outcome), 0, "it ran past its %s bound of %llu ms: %s",
                 etude_expiry_name(r->expiry), (unsigned long long)(r->bound_us / 1000u), v.reason);
    else if (why[0] != '\0' && v.outcome != LEGACY_PASS)
        came_out(c, outcome_of(v.outcome), 1, "%s; %s", why, v.reason);
    else
        came_out(c, outcome_of(v.outcome), 0, "%s", v.reason);
    if (r->proc != NULL && r->proc->output_error != 0)
        came_out(c, INCOMPLETE, 1, "its output could not be saved: %s",
                 strerror(r->proc->output_error));
    settle(x, c);
}

/* ---- The status record. ---- */

/* The attempts of a status record as it is made, and the clocks it is
 * made at: the monotonic clock's time and the epoch's, in microseconds. */
struct roll {
    struct etude_status_attempt *a;
    size_t n;
    uint64_t now, epoch;
};

/** Add command `c` of case `case_id` on `arch` to the attempts of the
 * status record `r`, when it waits or runs. */
static void status_of(const struct command *c, const char *case_id, const char *arch,
                      struct roll *r)
{
    struct etude_status_attempt *s = &r->a[r->n];

    memset(s, 0, sizeof(*s));
    s->attempt_id = c->id;
    s->case_id = case_id;
    s->architecture = arch;
    if (c->state == RUNNING && c->sup != NULL) {
        etude_status_watch(s, etude_supervised_watchdog(c->sup), r->now, r->epoch);
        r->n++;
    } else if ((c->state == QUEUED && c->asked) || c->state == LOCKING || c->state == LOADING) {
        s->waiting = 1;
        s->since_us = c->queued_epoch_us;
        r->n++;
    }
}

/** Write the status record, when the run has one and it is due or
 * `force` is set. A command's start and its end force it, so that the
 * record does not name as running a command that has ended, nor leave out
 * one that runs. */
static void status(struct set *x, int force)
{
    struct roll r;
    struct etude_status s;
    size_t i;

    r.now = posix_now_us();
    r.epoch = etude_status_now();
    r.n = 0;
    if (x->o->status == NULL || (!force && r.now - x->status_us < STATUS_US))
        return;
    x->status_us = r.now;
    if ((r.a = calloc(x->n + 1, sizeof(*r.a))) == NULL)
        return;
    status_of(&x->prep, "legacy.prepare", x->narches > 1 ? "none" : x->o->arch, &r);
    for (i = 0; i < x->n; i++)
        status_of(&x->mem[i].c, x->mem[i].case_id, x->mem[i].arch, &r);
    memset(&s, 0, sizeof(s));
    if (etude_process_state((long)getpid(), &s.owner) != 1) {
        memset(&s.owner, 0, sizeof(s.owner));
        s.owner.pid = (long)getpid();
    }
    s.run_id = x->run_id;
    s.ledger = x->o->ledger;
    s.args = x->o->args != NULL ? x->o->args : "";
    s.written_us = r.epoch;
    s.attempts = r.a;
    s.nattempts = r.n;
    if (etude_status_write(x->o->status, &s) != 0 && force)
        say(x, "etude run: %s: the status record could not be written: %s", x->o->status,
            strerror(errno));
    free(r.a);
}

/* ---- Records of what ran. ---- */

/** Give the name of the end `e` as an attempt's results give it. */
static const char *end_name(enum etude_end e)
{
    return e == ETUDE_END_EXITED     ? "exited"
           : e == ETUDE_END_SIGNALED ? "signaled"
           : e == ETUDE_END_STOPPED  ? "stopped"
                                     : "lost";
}

/** Append the results document of member `m`, kept in the store, to `t`:
 * the one the judge gave, or one of its termination alone. */
static int results(const struct set *x, const struct member *m, struct cmp_text *t)
{
    const struct command *c = &m->c;
    struct cmp_text d = {NULL, 0, 0, 0};
    const char *by = m->k->guest ? "etude_guest" : "host_process", *check;
    const char *placement = m->k->guest || m->k->machine ? "qemu" : "host";
    int value = c->value;

    if (m->judged != NULL)
        return keep(x, m->judged, m->njudged, t);
    if (c->end == ETUDE_END_STOPPED || c->end == ETUDE_END_LOST || !c->ran) {
        by = "executor";
        value = -1;
    }
    check = c->outcome == PASS ? "pass" : c->outcome == FAILURE ? "fail" : "missing";
    cmp_printf(&d, "{\"schema_version\":1,\"run_id\":");
    string(&d, x->run_id);
    cmp_printf(&d, ",\"attempt_id\":");
    string(&d, c->id);
    cmp_printf(&d, ",\"item_id\":");
    string(&d, m->item_id);
    cmp_printf(&d, ",\"case_id\":");
    string(&d, m->case_id);
    cmp_printf(&d,
               ",\"inventory_sha256\":\"%s\",\"placement\":\"%s\",\"termination\":{\"end\":"
               "\"%s\",\"value\":",
               m->inventory, placement, end_name(c->end));
    if (value >= 0 && value <= 255)
        cmp_printf(&d, "%d", value);
    else
        cmp_printf(&d, "null");
    cmp_printf(&d, ",\"observed_by\":\"%s\",\"detail\":", by);
    if (c->outcome != PASS)
        string(&d, c->reason);
    else
        cmp_printf(&d, "null");
    cmp_printf(&d,
               "},\"report\":{\"state\":\"not_used\",\"records\":0,\"errors\":[]},"
               "\"branches\":[],\"assertions\":[{\"assertion_id\":\"%s\",\"source\":"
               "\"executor\",\"outcome\":\"%s\",\"detail\":",
               ETUDE_ASSERT_TERMINATION, check);
    if (c->outcome == PASS)
        cmp_printf(&d, "null");
    else
        string(&d, c->reason);
    cmp_printf(&d, "}],\"undeclared\":[]}\n");
    return keep_doc(x, &d, t);
}

/** Record the execution gap measured by the existing watchdog. */
static int execution_step(struct set *x, struct member *m)
{
    struct command *c = &m->c;
    struct cmp_text t = {0};
    int rc = 0;
    cmp_printf(&t, "{\"plan_id\":");
    string(&t, x->plan_record);
    cmp_printf(&t, ",\"item_id\":");
    string(&t, m->item_id);
    cmp_printf(&t, ",\"attempt_id\":");
    string(&t, c->id);
    cmp_printf(&t, ",\"step_id\":");
    string(&t, c->id);
    cmp_printf(&t, ",\"stage\":\"execute\",\"time\":");
    rc |= interval(&t, x, &c->admitted, &c->done, NULL);
    cmp_printf(&t, ",\"resources\":");
    rc |= resources(x, c, &t);
    cmp_printf(&t,
               ",\"witnesses\":[],\"max_progress_gap_us\":{\"value\":%llu,"
               "\"method\":\"watchdog-progress-v1\",\"quality\":\"exact\","
               "\"interval_us\":null,\"error_bound\":1},\"result\":",
               (unsigned long long)c->max_gap_us);
    rc |= result(x, c, &t);
    cmp_printf(&t, "}");
    if (!rc)
        rc = append(x, "step", &t, m->execution_record, sizeof(m->execution_record));
    free(t.data);
    return rc;
}

/** Write the attempt record of member `m`, which is done. */
static int write_attempt(struct set *x, struct member *m)
{
    struct command *c = &m->c;
    struct cmp_text t = {NULL, 0, 0, 0};
    const struct etude_reading *from = c->was_admitted ? &c->admitted : &c->queued;
    int rc = 0;

    cmp_printf(&t, "{\"plan_id\":");
    string(&t, x->plan_record);
    cmp_printf(&t, ",\"item_id\":");
    string(&t, m->item_id);
    cmp_printf(&t, ",\"attempt_id\":");
    string(&t, c->id);
    if (x->o->prepared && c->ran && execution_step(x, m) != 0)
        rc = -1;
    cmp_printf(&t, ",\"previous_attempt_id\":null,\"steps\":[");
    if (m->execution_record[0])
        string(&t, m->execution_record);
    cmp_printf(&t, "],\"time\":");
    rc |= interval(&t, x, from, &c->done, NULL);
    cmp_printf(&t, ",\"queue_us\":");
    rc |= elapsed(&t, &c->queued, from);
    cmp_printf(&t, ",\"resources\":");
    rc |= resources(x, c, &t);
    cmp_printf(&t, ",\"assertion_results\":");
    rc |= results(x, m, &t);
    cmp_printf(&t, ",\"result\":");
    rc |= result(x, c, &t);
    cmp_printf(&t, "}");
    if (rc == 0)
        rc = append(x, "attempt", &t, m->record_id, sizeof(m->record_id));
    free(t.data);
    if (rc != 0) {
        x->failed = 1;
        say(x, "etude run: the attempt record of %s could not be written: %s", c->name,
            strerror(errno != 0 ? errno : EIO));
    }
    return rc;
}

/** Write the step record of the preparation, which is done. */
static int write_step(struct set *x)
{
    struct command *c = &x->prep;
    struct cmp_text t = {NULL, 0, 0, 0};
    const struct etude_reading *from = c->was_admitted ? &c->admitted : &c->queued;
    int rc = 0;

    cmp_printf(&t, "{\"plan_id\":");
    string(&t, x->plan_record);
    cmp_printf(&t, ",\"item_id\":null,\"attempt_id\":null,\"step_id\":");
    string(&t, c->id);
    cmp_printf(&t, ",\"stage\":\"build\",\"time\":");
    rc |= interval(&t, x, from, &c->done, NULL);
    cmp_printf(&t, ",\"resources\":");
    rc |= resources(x, c, &t);
    cmp_printf(&t, ",\"witnesses\":[],\"max_progress_gap_us\":{\"value\":null,\"reason\":\"the "
                   "preparation reports no progress\"},\"result\":");
    rc |= result(x, c, &t);
    cmp_printf(&t, "}");
    if (rc == 0)
        rc = append(x, "step", &t, x->prep_step, sizeof(x->prep_step));
    free(t.data);
    if (rc != 0) {
        x->failed = 1;
        x->prep_step[0] = '\0';
        say(x, "etude run: the step record of the preparation could not be written: %s",
            strerror(errno != 0 ? errno : EIO));
    }
    return rc;
}

/** Give the rank of an outcome: pass, cancelled, incomplete, failure. */
static int rank(enum outcome o)
{
    static const int ranks[] = {0, 3, 2, 1};

    return ranks[o];
}

/** Say whether member `m` has an attempt record. */
static int recorded(const struct member *m)
{
    return m->record_id[0] != '\0';
}

/** Whether member `i` is the first of its family, on either architecture. */
static int family_first(const struct set *x, size_t i)
{
    size_t j;

    for (j = 0; j < i; j++)
        if (strcmp(family_of(&x->mem[j]), family_of(&x->mem[i])) == 0)
            return 0;
    return 1;
}

/** Append the total of the family of member `i`, the first of its family,
 * after a comma unless `n` is 0: the attempts of its family's members that
 * ran, and their time. */
static void family_total(const struct set *x, size_t i, size_t n, struct cmp_text *f)
{
    const struct member *m;
    uint64_t sum = 0;
    size_t j, k = 0;

    cmp_printf(f, n ? ",{\"family_id\":" : "{\"family_id\":");
    string(f, family_of(&x->mem[i]));
    cmp_printf(f, ",\"members\":[");
    for (j = i; j < x->n; j++) {
        m = &x->mem[j];
        if (strcmp(family_of(m), family_of(&x->mem[i])) != 0)
            continue;
        if (!m->c.ran)
            continue;
        sum += m->c.ended_us - m->c.started_us;
        if (recorded(m)) {
            cmp_printf(f, k++ ? "," : "");
            string(f, m->record_id);
        }
    }
    cmp_printf(f, "],\"sum_us\":%llu}", (unsigned long long)sum);
}

/** Append the run's totals of each stage and of each family. */
static int totals(const struct set *x, struct cmp_text *t)
{
    struct cmp_text d = {NULL, 0, 0, 0}, f = {NULL, 0, 0, 0};
    uint64_t sum = 0;
    size_t i, n;
    int rc;

    cmp_printf(&d, "{\"schema_version\":1,\"stages\":[{\"stage\":\"build\",\"members\":[");
    if (x->prep_step[0] != '\0' && x->prep.ran) {
        string(&d, x->prep_step);
        sum = x->prep.ended_us - x->prep.started_us;
    }
    cmp_printf(&d, "],\"sum_us\":%llu,\"missing\":[]},{\"stage\":\"execute\",\"members\":[",
               (unsigned long long)sum);
    for (i = 0, n = 0, sum = 0; i < x->n; i++)
        if (x->mem[i].c.ran && recorded(&x->mem[i])) {
            cmp_printf(&d, n++ ? "," : "");
            string(&d, x->mem[i].record_id);
            sum += x->mem[i].c.ended_us - x->mem[i].c.started_us;
        }
    cmp_printf(&d, "],\"sum_us\":%llu,\"missing\":[", (unsigned long long)sum);
    for (i = 0, n = 0; i < x->n; i++)
        if (!x->mem[i].c.ran && recorded(&x->mem[i])) {
            cmp_printf(&d, n++ ? ",{\"record_id\":" : "{\"record_id\":");
            string(&d, x->mem[i].record_id);
            cmp_printf(&d, ",\"reason\":\"the case did not start\"}");
        }
    cmp_printf(&d, "]}]}\n");
    cmp_printf(&f, "{\"schema_version\":1,\"families\":[");
    for (i = 0, n = 0; i < x->n; i++)
        if (family_first(x, i))
            family_total(x, i, n++, &f);
    cmp_printf(&f, "]}\n");
    rc = part(x, t, "stage_totals", &d);
    rc |= part(x, t, "family_totals", &f);
    return rc;
}

/* Keep a load's complete bounded readiness and diagnostic output by digest. */
static int load_capture(const struct set *x, struct etude_collection_load *load,
                        struct cmp_text *text)
{
    cmp_printf(text, ",\"capture\":{");
    for (int error = 0; error < 2; error++) {
        const char *path = etude_collection_load_output(load, error);
        char *bytes = NULL;
        size_t size = 0;
        cmp_printf(text, "%s\"%s\":", error ? "," : "", error ? "stderr" : "stdout");
        if (!path)
            cmp_printf(text, "null");
        else {
            if (read_tail(path, 65536, &bytes, &size))
                return -1;
            int rc = keep(x, bytes, size, text);
            free(bytes);
            if (rc)
                return -1;
        }
    }
    cmp_printf(text, "}");
    return text->failed ? -1 : 0;
}

/** Append useful concurrency and the observed reserved loads. */
static int observed(const struct set *x, struct cmp_text *t)
{
    struct cmp_text u = {NULL, 0, 0, 0}, l = {NULL, 0, 0, 0};
    uint64_t now = x->end.monotonic_us;
    int rc;

    cmp_printf(&u,
               "{\"schema_version\":1,\"method\":\"driver-transitions-v1\",\"integral_us\":"
               "%llu,\"peak\":%u,\"elapsed_us\":%llu}\n",
               (unsigned long long)x->busy_us, x->peak,
               (unsigned long long)(x->granted ? now - x->first.monotonic_us : 0));
    cmp_printf(&l, "%s",
               x->load ? x->load
                       : "{\"schema_version\":1,\"method\":\"native-load-v1\",\"comparison_"
                         "available\":false}");
    if (x->o->repeat) {
        while (l.length && (l.data[l.length - 1] == '\n' || l.data[l.length - 1] == ' '))
            l.length--;
        if (!l.length || l.data[l.length - 1] != '}') {
            free(l.data);
            free(u.data);
            return -1;
        }
        l.data[--l.length] = 0;
        cmp_printf(&l, ",\"applied\":[");
        size_t n = 0;
        for (size_t i = 0; i < x->n; i++) {
            if (!x->mem[i].load)
                continue;
            cmp_printf(&l, "%s{\"item_id\":", n++ ? "," : "");
            string(&l, x->mem[i].item_id);
            cmp_printf(&l, ",\"load\":");
            cmp_printf(&l, "%s", etude_collection_load_record(x->mem[i].c.load));
            if (load_capture(x, x->mem[i].c.load, &l)) {
                free(l.data);
                free(u.data);
                return -1;
            }
            cmp_printf(&l, "}");
        }
        cmp_printf(&l, "]}\n");
    }
    rc = part(x, t, "useful_concurrency", &u);
    rc |= part(x, t, "load", &l);
    return rc;
}

/** Append the run's resources: its scope, kept, and its processor time. */
static int run_resources(const struct set *x, struct cmp_text *t)
{
    struct cmp_text d = {NULL, 0, 0, 0};
    struct etude_measurement *m, sum;
    char json[1024];
    size_t i, k, n = 0;
    int rc;

    if ((m = calloc(x->n + 1, sizeof(*m))) == NULL)
        return -1;
    cmp_printf(&d, "{\"run_id\":");
    string(&d, x->run_id);
    cmp_printf(&d, ",\"steps\":[");
    if (x->prep_step[0] != '\0')
        string(&d, x->prep_step);
    cmp_printf(&d, "],\"attempts\":[");
    if (x->prep.ran)
        m[n++] = as_history(&x->prep.cpu_us);
    for (i = 0, k = 0; i < x->n; i++) {
        if (!recorded(&x->mem[i]))
            continue;
        cmp_printf(&d, k++ ? "," : "");
        string(&d, x->mem[i].record_id);
        if (x->mem[i].c.ran)
            m[n++] = as_history(&x->mem[i].c.cpu_us);
    }
    cmp_printf(&d, "]}\n");
    sum = etude_history_cpu_sum(m, n);
    free(m);
    cmp_printf(t, "{\"scope\":");
    rc = keep_doc(x, &d, t);
    if (etude_measurement_json(&sum, json, sizeof(json)) != 0)
        return -1;
    cmp_printf(t,
               ",\"tree_cpu_us\":%s,\"combined_peak_bytes\":{\"value\":null,\"reason\":\"the "
               "set driver samples no combined memory\"},\"samples\":[]}",
               json);
    return rc;
}

/** Write into `out` why the run did not pass. */
static void run_reason(const struct set *x, char *out, size_t size)
{
    unsigned counts[4] = {0, 0, 0, 0};
    size_t i;

    if (x->cleanup_failed || x->uncertain) {
        snprintf(out, size, "owned cleanup could not be confirmed");
        return;
    }
    if (x->failed) {
        snprintf(out, size,
                 "the run has incomplete history, input verification or observation evidence");
        return;
    }
    if (x->prep.outcome != PASS) {
        snprintf(out, size, "the preparation did not pass: %s", x->prep.reason);
        return;
    }
    for (i = 0; i < x->n; i++)
        counts[x->mem[i].c.outcome]++;
    snprintf(out, size, "of %lu items, %u failed, %u did not finish and %u were cancelled",
             (unsigned long)x->n, counts[FAILURE], counts[INCOMPLETE], counts[CANCELLED]);
}

/** Append a disruption of command `c`, after a comma unless `*n` is 0. */
static void disruption(struct cmp_text *t, const struct command *c, size_t *n)
{
    char text[REASON_LEN + 320];

    if (c->disruption[0] == '\0')
        return;
    snprintf(text, sizeof(text), "%s: %s", c->id, c->disruption);
    cmp_printf(t, (*n)++ ? "," : "");
    string(t, text);
}

static int run_budget(struct set *x, struct cmp_text *t, int open)
{
    const struct etude_reading *from = charged_start(x), *end = &x->end;
    int rc = 0;
    cmp_printf(t, ",\"budget\":{\"policy\":%s,\"charged_us\":", x->policy);
    if (open)
        cmp_printf(t, "{\"value\":null,\"reason\":\"the run has not ended\"}");
    else if (x->o->prepared && !etude_context_preparation(x->o->context))
        cmp_printf(t,
                   "{\"value\":null,\"reason\":\"ordinary preparation has no same-run receipt\"}");
    else
        rc |= elapsed(t, from, end);
    cmp_printf(t, ",\"outside_queue_us\":{\"value\":0,\"method\":\"set-driver-v1\",\"quality\":"
                  "\"exact\",\"interval_us\":null,\"error_bound\":0},\"one_time_setup_us\":"
                  "{\"value\":0,\"method\":\"set-driver-v1\",\"quality\":\"exact\","
                  "\"interval_us\":null,\"error_bound\":0},");
    struct etude_measurement charge = etude_history_elapsed(from, end);
    int known =
        !open && !charge.reason && (!x->o->prepared || etude_context_preparation(x->o->context));
    const char *verdict = !strcmp(purpose(x), "\"phase_first_run\"")            ? "exempt"
                          : !known                                              ? "unknown"
                          : charge.value <= x->ceiling_us && !x->budget_expired ? "pass"
                                                                                : "failure";
    x->budget_failed = !strcmp(verdict, "failure");
    cmp_printf(t,
               "\"goal_us\":%llu,\"growth_us\":%llu,\"ceiling_us\":%llu,"
               "\"cleanup_reserve_us\":%llu,\"verdict\":\"%s\"}",
               (unsigned long long)x->goal_us, (unsigned long long)x->growth_us,
               (unsigned long long)x->ceiling_us, (unsigned long long)x->reserve_us, verdict);
    return rc;
}

/** Write the run record, with each item's attempt and the worst outcome;
 * give the worst outcome in `*worst`. */
static int write_run(struct set *x, enum outcome *worst)
{
    struct cmp_text t = {NULL, 0, 0, 0};
    const struct etude_reading *end = &x->end;
    const struct etude_reading *from = charged_start(x);
    char reason[1024];
    int rc = 0, open = x->cleanup_failed || x->uncertain ||
                       (x->prep.ran && x->prep.cleanup != ETUDE_CLEANUP_CONFIRMED);
    size_t i, n = 0;

    running_changed(x, 0);
    *worst = PASS;
    for (i = 0; i < x->n; i++) {
        if (x->mem[i].c.cleanup != ETUDE_CLEANUP_CONFIRMED)
            open = 1;
        if (rank(x->mem[i].c.outcome) > rank(*worst))
            *worst = x->mem[i].c.outcome;
    }
    if (x->prep.outcome != PASS && rank(FAILURE) > rank(*worst))
        *worst = FAILURE;
    if ((x->failed || open) && rank(INCOMPLETE) > rank(*worst))
        *worst = INCOMPLETE;
    cmp_printf(&t, "{\"plan_id\":");
    string(&t, x->plan_record);
    cmp_printf(&t, ",\"items\":[");
    for (i = 0; i < x->n; i++) {
        const struct member *m = &x->mem[i];

        cmp_printf(&t, "%s{\"item_id\":", i ? "," : "");
        string(&t, m->item_id);
        cmp_printf(&t, ",\"attempt_records\":[");
        if (recorded(m))
            string(&t, m->record_id);
        cmp_printf(&t, "],\"outcome\":\"%s\"}", outcome_names[m->c.outcome]);
    }
    cmp_printf(&t, "],\"preparation_steps\":[");
    if (x->prep_step[0] != '\0')
        string(&t, x->prep_step);
    cmp_printf(&t, "],\"time\":");
    rc |= interval(&t, x, from, open ? NULL : end,
                   "a cleanup is unconfirmed, so the run has not ended");
    cmp_printf(&t, ",\"resources\":");
    rc |= run_resources(x, &t);
    rc |= totals(x, &t);
    rc |= observed(x, &t);
    rc |= run_budget(x, &t, open);
    cmp_printf(&t, ",\"result\":{\"outcome\":\"%s\",\"reason\":", outcome_names[*worst]);
    run_reason(x, reason, sizeof(reason));
    if (*worst == PASS)
        cmp_printf(&t, "null");
    else
        string(&t, reason);
    cmp_printf(&t, ",\"cleanup\":\"%s\",\"evidence\":[],\"capture_errors\":[],\"disruptions\":[",
               open ? "unconfirmed" : "confirmed");
    disruption(&t, &x->prep, &n);
    for (i = 0; i < x->n; i++)
        disruption(&t, &x->mem[i].c, &n);
    cmp_printf(&t, "]}}");
    if (rc == 0)
        rc = append(x, "run", &t, x->run_record, sizeof(x->run_record));
    free(t.data);
    return rc;
}

/* ---- The preparation. ---- */

/** Validate the artifacts supplied by a producer, without running a build. */
static int native_program(const char *path)
{
    unsigned char magic[4];
    FILE *f = fopen(path, "rb");
    int binary =
        f != NULL && fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
        ((!memcmp(magic, "\177ELF", 4)) || (magic[0] == 'M' && magic[1] == 'Z') ||
         (!memcmp(magic, "\317\372\355\376", 4)) || (!memcmp(magic, "\316\372\355\376", 4)) ||
         (!memcmp(magic, "\312\376\272\276", 4)) || (!memcmp(magic, "\312\376\272\277", 4)));
    if (f)
        fclose(f);
    return binary;
}

static int prepared(struct set *x)
{
    struct command *c = &x->prep;
    size_t i, j;
    struct stat st;
    char path[PATH_LEN];
    FILE *f;
    char *line = NULL;
    size_t room = 0;
    int listed, at, rc = 0;
    char unsupported[PATH_LEN] = "";
    char digest[65];
    unsigned long long bytes, inode;
    long long mtime;

    asked(c);
    c->admitted = c->queued;
    const struct etude_preparation_receipt *receipt = etude_context_preparation(x->o->context);
    if (receipt)
        c->queued = c->admitted = receipt->start;
    c->was_admitted = 1;
    c->cleanup = ETUDE_CLEANUP_CONFIRMED;
    if (etude_legacy_check_tree(x->record, x->tree, 1, x->out) != 0)
        rc = 1;
    for (i = 0; !rc && i < x->n; i++) {
        const struct legacy_case *k = x->mem[i].k;
        if (!k->guest) {
            int binary = native_program(x->mem[i].program);
            if (!binary) {
                snprintf(unsupported, sizeof(unsupported), "%s", x->mem[i].program);
                rc = 1;
                break;
            }
        }
        for (j = 0; j < k->ngoals; j++) {
            if (fit(path, sizeof(path), "%s/%s", x->tree, k->goals[j]) != 0 ||
                stat(path, &st) != 0) {
                rc = 1;
                break;
            }
            if (S_ISDIR(st.st_mode))
                continue;
            listed = 0;
            f = fopen(x->record, "r");
            if (f == NULL) {
                rc = 1;
                break;
            }
            while (getline(&line, &room, f) >= 0) {
                at = -1;
                if (sscanf(line, "file %64s %llu %llu %lld %n", digest, &bytes, &inode, &mtime,
                           &at) == 4 &&
                    at >= 0) {
                    line[strcspn(line, "\n")] = 0;
                    listed |= strcmp(line + at, k->goals[j]) == 0;
                }
            }
            fclose(f);
            if (!listed) {
                rc = 1;
                break;
            }
        }
    }
    free(line);
    c->state = DONE;
    etude_history_now(&c->done);
    if (unsupported[0])
        came_out(c, FAILURE, 0, "native execution requires a compiled program: %s", unsupported);
    else if (rc)
        came_out(c, FAILURE, 0, "prepared artifacts are missing, changed or absent from %s",
                 x->record);
    else
        c->outcome = PASS;
    write_step(x);
    return rc;
}

/**
 * Prepare the cases' artifacts: claim every limited resource of the machine
 * and the old host lock alone, run `etude prepare` with its output
 * printed as it comes, and write its step record.
 *
 * @return 0 when it passed, or 1.
 */
static int prepare(struct set *x)
{
    struct command *c = &x->prep;
    struct etude_supervised *f;
    int rc;

    if (x->o->prepared)
        return prepared(x);
    say(x, "run: preparing");
    asked(c);
    status(x, 1);
    for (;;) {
        if (signalled(x)) {
            etude_ledger_withdraw(x->l, c->id);
            not_run(x, c, CANCELLED, x->stop_reason, 0);
            return 1;
        }
        if (etude_ledger_admit(x->l, c->id, x->o->account, c->res, c->nres, x->cap, x->ncap, 1) ==
            0)
            break;
        if (errno != EAGAIN) {
            came_out(c, INCOMPLETE, 1, "the machine's ledger did not admit it: %s",
                     errno == ERANGE   ? "the machine's limits cannot hold it"
                     : errno == ENOSPC ? "it cannot fit beside claims only a person can release"
                                       : strerror(errno));
            not_run(x, c, INCOMPLETE, c->reason, 1);
            return 1;
        }
        if (waited_out(x, c)) {
            etude_ledger_withdraw(x->l, c->id);
            came_out(c, INCOMPLETE, 1, "it waited more than %u s for the machine", x->o->wait);
            not_run(x, c, INCOMPLETE, c->reason, 1);
            return 1;
        }
        status(x, 0);
#ifdef _WIN32
        Sleep(ASK_MS);
#else
        poll(NULL, 0, ASK_MS);
#endif
    }
    granted(x, c);
    c->state = LOCKING;
    status(x, 1);
    while ((rc = lock(x, c, wait_left_ms(x, c))) == 0 && !signalled(x) && !waited_out(x, c)) {
    }
    if (rc != 1) {
        if (signalled(x))
            not_run(x, c, CANCELLED, x->stop_reason, 0);
        else if (rc == 0)
            not_run(x, c, INCOMPLETE, "the old runner's locks stayed taken", 1);
        else
            not_run(x, c, INCOMPLETE, c->reason, 1);
        return 1;
    }
    if (start(x, c, NULL, 0) != 0) {
        came_out(c, INCOMPLETE, 1, "etude prepare could not start: %s", strerror(errno));
        not_run(x, c, INCOMPLETE, c->reason, 1);
        return 1;
    }
    status(x, 1);
    for (;;) {
        f = etude_supervise_wait(x->sup, ASK_MS);
        drain(x, c, 0);
        if (signalled(x) && !c->cancelled) {
            etude_supervised_cancel(c->sup);
            c->cancelled = 1;
        }
        status(x, 0);
        if (f == c->sup)
            break;
        if (f != NULL)
            etude_supervised_free(f);
    }
    judge_prep(x, c);
    drain(x, c, 1);
    release(x, c, c->cleanup);
    etude_supervised_free(c->sup);
    c->sup = NULL;
    c->state = DONE;
    status(x, 1);
    write_step(x);
    if (x->o->stamp != NULL) {
        int fd = open(x->o->stamp, O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (fd >= 0)
            close(fd);
    }
    return c->outcome == PASS ? 0 : 1;
}

/* ---- The machine cases. ---- */

/** Open the store of the guest and machine cases' artifacts, under the
 * evidence, once; 0, or -1 with why in `why`. */
static int open_objects(struct set *x, char *why, size_t size)
{
    if (x->objects != NULL)
        return 0;
    if (fit(x->objects_dir, sizeof(x->objects_dir), "%s/%s.store", x->o->evidence, x->run_id) !=
            0 ||
        fit(x->digests, sizeof(x->digests), "%s/%s", x->tree, ETUDE_LEGACY_DIGESTS) != 0 ||
        (x->objects = etude_store_open(x->objects_dir)) == NULL ||
        etude_store_digests(x->objects, x->digests) != 0) {
        (void)fit(why, size, "the store of its artifacts, %s, cannot be opened: %s", x->objects_dir,
                  strerror(errno));
        return -1;
    }
    return 0;
}

/** Write into `out` the path of the program `name`: `name` itself when it
 * has a slash, or the first executable file of that name in a directory of
 * PATH; 0, or -1 when there is none. */
static int on_path(const struct set *x, const char *name, char *out, size_t size)
{
#ifdef _WIN32
    if (etude_win_program(name, out, size) == 0)
        return 0;
    return fit(out, size, "%s/build/tools/etude/commands/%s.exe", x->tree, name) == 0 &&
                   access(out, X_OK) == 0
               ? 0
               : -1;
#else
    const char *dir = getenv("PATH"), *end;

    (void)x;
    if (strchr(name, '/') != NULL)
        return fit(out, size, "%s", name) == 0 && access(out, X_OK) == 0 ? 0 : -1;
    for (; dir != NULL && *dir != '\0'; dir = *end == ':' ? end + 1 : end) {
        end = dir + strcspn(dir, ":");
        if (end > dir && fit(out, size, "%.*s/%s", (int)(end - dir), dir, name) == 0 &&
            access(out, X_OK) == 0)
            return 0;
    }
    return -1;
#endif
}

/**
 * Write the scenario invocation of machine case member `m`, whose artifacts
 * are published, for the emulators `qemu` of i686 and x86-64 and the x86-64
 * OVMF code image that `RACH2_OVMF_CODE` names, none when it is unset or
 * empty: each step's bound is the case's gap, and its cleanup's bound is the
 * manifest's, or the case's grace when it gives none.
 *
 * @return 0, or -1 with the reason in `why`.
 */
static int write_invocation(const struct set *x, const struct member *m, char qemu[2][PATH_LEN],
                            char *why, size_t size)
{
    const struct legacy_case *k = m->k;
    const struct json *tools = json_member(json_member(k->json, "requirements"), "tools");
    const struct json *steps = json_member(k->json, "steps");
    const char *ovmf = getenv("RACH2_OVMF_CODE");
    struct cmp_text d = {NULL, 0, 0, 0};
    char path[PATH_LEN];
    size_t i;
    FILE *f;
    int rc = -1;

    cmp_printf(&d, "{\"schema_version\":1,\"run_id\":");
    string(&d, x->run_id);
    cmp_printf(&d, ",\"attempt_id\":");
    string(&d, m->c.id);
    cmp_printf(&d, ",\"manifest\":");
    string(&d, m->manifest->path);
    cmp_printf(&d, ",\"case_id\":");
    string(&d, k->case_id);
    cmp_printf(&d, ",\"architecture\":");
    string(&d, k->arch);
    cmp_printf(&d, ",\"store\":");
    string(&d, x->objects_dir);
    cmp_printf(&d, ",\"digests\":");
    string(&d, x->digests);
    cmp_printf(&d, ",\"artifacts\":{");
    for (i = 0; i < k->ngoals; i++) {
        cmp_printf(&d, i > 0 ? "," : "");
        string(&d, k->names[i]);
        cmp_printf(&d, ":\"%s\"", m->digests[i]);
    }
    cmp_printf(&d, "},\"tools\":{");
    for (i = 0; tools != NULL && tools->type == JSON_ARRAY && i < tools->count; i++) {
        if (tools->item[i]->type != JSON_STRING ||
            on_path(x, tools->item[i]->string, path, sizeof(path)) != 0) {
            fit(why, size, "its tool %s is not on PATH",
                tools->item[i]->type == JSON_STRING ? tools->item[i]->string : "?");
            free(d.data);
            return -1;
        }
        cmp_printf(&d, i > 0 ? "," : "");
        string(&d, tools->item[i]->string);
        cmp_printf(&d, ":");
        string(&d, path);
    }
    cmp_printf(&d, "},\"site\":{\"accel\":");
    string(&d, x->o->accel);
    cmp_printf(&d, ",\"qemu_i686\":");
    string(&d, qemu[0]);
    cmp_printf(&d, ",\"qemu_x86_64\":");
    string(&d, qemu[1]);
    cmp_printf(&d, ",\"ovmf_i686\":null,\"ovmf_x86_64\":");
    if (ovmf != NULL && *ovmf != '\0')
        string(&d, ovmf);
    else
        cmp_printf(&d, "null");
    cmp_printf(&d, ",\"boundary_lazy\":%s},\"steps\":{", x->boundary_lazy ? "true" : "false");
    for (i = 0; steps != NULL && steps->type == JSON_ARRAY && i < steps->count; i++) {
        cmp_printf(&d, i > 0 ? "," : "");
        string(&d, json_text(steps->item[i], "step_id"));
        cmp_printf(&d, ":%llu", (unsigned long long)k->gap_us);
    }
    cmp_printf(&d, "},\"ports\":[");
    for (i = 0; i < k->ports; i++)
        cmp_printf(&d, i > 0 ? ",%u" : "%u", m->port + (unsigned)i);
    cmp_printf(&d, "],\"cleanup_us\":%llu,\"scratch\":",
               (unsigned long long)(k->stage_us[ETUDE_STAGE_CLEANUP] > 0
                                        ? k->stage_us[ETUDE_STAGE_CLEANUP]
                                        : CASE_GRACE_US));
    string(&d, m->work);
    cmp_printf(&d,
               ",\"grace_us\":%llu,\"console_max_bytes\":%llu,\"output_max_bytes\":%llu,"
               "\"resend_us\":%llu}\n",
               (unsigned long long)CASE_GRACE_US, (unsigned long long)CONSOLE_BYTES,
               (unsigned long long)SCENARIO_BYTES, (unsigned long long)RESEND_US);
    if (d.failed)
        errno = ENOMEM;
    else if ((f = fopen(m->invocation, "w")) != NULL) {
        rc = fwrite(d.data, 1, d.length, f) == d.length ? 0 : -1;
        rc |= fclose(f) != 0 ? -1 : 0;
    }
    if (rc != 0)
        fit(why, size, "its invocation %s could not be written: %s", m->invocation,
            strerror(errno));
    free(d.data);
    return rc;
}

/**
 * Say whether machine case member `m` may run here. A case that declares a
 * machine of the boundary profile needs the site's permission and a claim of
 * its whole `memory_bytes`, which the ledger's memory limit would otherwise
 * cut short; the launcher takes the case's `memory_bytes` as what the claim
 * holds.
 *
 * @return 0, or -1 with the reason in `why`.
 */
static int boundary_admits(const struct set *x, const struct member *m, char *why, size_t size)
{
    const struct json *machines = json_member(m->k->json, "machines");
    const char *profile;
    size_t i;
    int boundary = 0;

    for (i = 0; machines != NULL && machines->type == JSON_ARRAY && i < machines->count; i++) {
        profile = json_text(json_member(machines->item[i], "qemu"), "memory_profile");
        boundary |= profile != NULL && strcmp(profile, BOUNDARY_PROFILE) == 0;
    }
    if (!boundary)
        return 0;
    if (!x->boundary_lazy) {
        (void)fit(why, size, "the site's policy does not permit the lazy memory profile %s",
                  BOUNDARY_PROFILE);
        return -1;
    }
    if (m->k->memory_bytes > x->cap[ETUDE_LEGACY_MEMORY].amount) {
        (void)fit(why, size,
                  "its %llu bytes of memory are more than the run's limit of %llu, and a "
                  "machine of %s runs only on its whole reservation",
                  (unsigned long long)m->k->memory_bytes,
                  (unsigned long long)x->cap[ETUDE_LEGACY_MEMORY].amount, BOUNDARY_PROFILE);
        return -1;
    }
    return 0;
}

#ifdef RACH2_HOSTTEST
/* In the host test, the copy of a file to the run's store: the test replaces
 * it to count the copies, to fail one and to change a file after one. */
int (*legacy_set_put)(struct etude_store *s, const char *path, const char *expect,
                      char digest[ETUDE_DIGEST_HEX + 1], uint64_t *size) = etude_store_put;
#else
#define legacy_set_put etude_store_put
#endif

/** Publish the artifacts of machine case member `m` to the run's store, and
 * write its invocation; 0, or -1 with the reason in `why`. */
static int publish_member(struct set *x, struct member *m, char qemu[2][PATH_LEN], char *why,
                          size_t size)
{
    const struct legacy_case *k = m->k;
    const char *emulator = qemu[strcmp(k->arch, "x86_64") == 0];
    char path[PATH_LEN];
    size_t g;

    if (boundary_admits(x, m, why, size) != 0)
        return -1;
    if (strchr(emulator, '/') == NULL) {
        fit(why, size, "its emulator %s is not on PATH", emulator);
        return -1;
    }
    if ((m->digests = calloc(k->ngoals + 1, sizeof(*m->digests))) == NULL) {
        fit(why, size, "memory ran out");
        return -1;
    }
    for (g = 0; g < k->ngoals; g++)
        if (fit(path, sizeof(path), "%s/%s", x->tree, k->goals[g]) != 0 ||
            legacy_set_put(x->objects, path, NULL, m->digests[g], NULL) != 0) {
            fit(why, size, "its artifact %s, %s, could not be published: %s", k->names[g],
                k->goals[g], strerror(errno));
            return -1;
        }
    return write_invocation(x, m, qemu, why, size);
}

/**
 * Publish the artifacts of each machine case member to the run's store, and
 * write its scenario invocation. A member whose artifacts or invocation
 * cannot be made ends before it runs, incomplete, with the reason as a
 * disruption.
 */
static void publish(struct set *x)
{
    static const char *const emulators[] = {"qemu-system-i386", "qemu-system-x86_64"};
    char qemu[2][PATH_LEN], why[REASON_LEN];
    struct member *m;
    size_t i;
    int opened;

    for (i = 0; i < x->n && !x->mem[i].k->machine; i++) {
    }
    if (i == x->n)
        return;
    for (i = 0; i < 2; i++) {
        const char *name = emulators[i];
#ifdef _WIN32
        if (strcmp(x->o->accel, "whpx") == 0)
            name = emulators[1];
#endif
        if (on_path(x, name, qemu[i], PATH_LEN) != 0)
            snprintf(qemu[i], PATH_LEN, "%s", name);
    }
    opened = open_objects(x, why, sizeof(why)) == 0;
    for (i = 0; i < x->n; i++) {
        m = &x->mem[i];
        if (!m->k->machine || m->c.state != QUEUED)
            continue;
        if (opened && publish_member(x, m, qemu, why, sizeof(why)) == 0)
            continue;
        asked(&m->c);
        came_out(&m->c, INCOMPLETE, 1, "%s", why);
        not_run(x, &m->c, INCOMPLETE, m->c.reason, 1);
        write_attempt(x, m);
    }
}

/* ---- The cases. ---- */

/** Say whether command `c` holds or runs on what the machine admitted. */
static int active(const struct command *c)
{
    return c->state == LOCKING || c->state == LOADING || c->state == RUNNING;
}

/** The memory reserved by one command's claim. */
static uint64_t command_memory(const struct command *c)
{
    for (size_t i = 0; i < c->nres; i++)
        if (!strcmp(c->res[i].name, "memory_bytes"))
            return c->res[i].amount;
    return 0;
}

/**
 * Mark the members that may be admitted now. The order is kept: a member
 * that runs alone waits until nothing of the run is active and keeps the
 * members after it waiting, and the others are ready while fewer than the
 * width are active or ready.
 */
static void mark_ready(struct set *x)
{
    size_t i, busy = 0, room, marked = 0;
    int exclusive = 0, barrier = 0;
    uint64_t memory = 0, budget = x->o->pool_memory_bytes;

    for (i = 0; i < x->n; i++)
        if (active(&x->mem[i].c)) {
            busy++;
            memory += command_memory(&x->mem[i].c);
            exclusive |= x->mem[i].c.exclusive;
        }
    room = x->o->width == 0 ? x->n : x->o->width > busy ? x->o->width - busy : 0;
    for (i = 0; i < x->n; i++) {
        struct command *c = &x->mem[i].c;

        x->jobs[i].ready = 0;
        if (c->state != QUEUED || x->stopping || exclusive || barrier)
            continue;
        uint64_t amount = command_memory(c);
        if (budget && amount > budget)
            c->exclusive = 1;
        if (budget && (busy || marked) && (amount > budget || memory > budget - amount))
            continue;
        if (c->exclusive) {
            barrier = 1;
            if (busy > 0 || marked > 0)
                continue;
        } else if (room == 0) {
            continue;
        } else {
            room--;
        }
        x->jobs[i].ready = 1;
        memory += amount;
        marked++;
        asked(c);
    }
}

/** Collect into `out` the names of the members in state `s`. */
static void listed(const struct set *x, enum state s, char *out, size_t size)
{
    size_t i, used = 0;

    out[0] = '\0';
    for (i = 0; i < x->n && used < size; i++)
        if (x->mem[i].c.state == s || (s == QUEUED && x->mem[i].c.state == LOCKING))
            used += (size_t)snprintf(out + used, size - used, " %s", x->mem[i].c.name);
}

/**
 * Stop the run for `why`: cancel each member that has not started, and the
 * cases that run. `what`, when not NULL, is printed before the names of
 * those not started and of those stopped part way.
 */
static void stop_all(struct set *x, const char *why, const char *what)
{
    char queued[4096], running[4096];
    size_t i;

    if (x->stopping)
        return;
    x->stopping = 1;
    if (why != x->stop_reason)
        snprintf(x->stop_reason, sizeof(x->stop_reason), "%s", why);
    listed(x, QUEUED, queued, sizeof(queued));
    listed(x, RUNNING, running, sizeof(running));
    if (what != NULL && queued[0] != '\0')
        say(x, "run: %s; not started:%s", what, queued);
    if (what != NULL && running[0] != '\0')
        say(x, "run: %s; stopped part way:%s", what, running);
    if (etude_sched_withdraw(x->l, x->jobs, x->n) != 0)
        x->failed = 1;
    for (i = 0; i < x->n; i++) {
        struct command *c = &x->mem[i].c;

        if (c->state == QUEUED && x->jobs[i].state == ETUDE_JOB_ADMITTED)
            c->claim = 1;
        if (c->state == QUEUED || c->state == LOCKING || c->state == LOADING)
            not_run(x, c, CANCELLED, why, 0);
        else if (c->state == RUNNING && !c->cancelled) {
            if (x->mem[i].g != NULL)
                legacy_guest_cancel(x->mem[i].g);
            else
                etude_supervised_cancel(c->sup);
            c->cancelled = 1;
        }
    }
}

/** Give the variables of case member `m`'s program in `vars`, and their
 * count. */
static size_t case_vars(struct member *m, char **vars)
{
    size_t n = 0;

    while (n < CASE_VARS && m->vars[n][0] != '\0') {
        vars[n] = m->vars[n];
        n++;
    }
    return n;
}

/**
 * Start the machine of guest case member `m`, whose claim and locks are
 * held and whose workspace is made, with the `n` variables `vars` in QEMU's
 * environment; 0, or -1 with why in `why`.
 */
static int start_guest(struct set *x, struct member *m, char *const *vars, size_t n, char *why,
                       size_t size)
{
    struct command *c = &m->c;
    struct legacy_guest_spec s;
    const struct etude_supervised_result *r;

    if (environment(c, vars, n) != 0) {
        snprintf(why, size, "%s", strerror(errno));
        return -1;
    }
    if (open_objects(x, why, size) != 0)
        return -1;
    memset(&s, 0, sizeof(s));
    s.k = m->k;
    s.run_id = x->run_id;
    s.attempt_id = c->id;
    s.arch = m->k->arch;
    s.accel = x->o->accel;
    s.tree = x->tree;
    s.record = x->record;
    s.store = x->objects;
    s.work = m->work;
    s.out = c->out;
    s.err = m->guest_err;
    s.envp = c->envp;
    s.memory_bytes = m->k->memory_bytes;
    s.grace_us = c->grace_us;
    s.reg = c->load ? etude_collection_load_register_case : etude_ledger_register;
    s.reg_ctx = x->l;
    s.wd = &c->wd;
    if (legacy_guest_start(x->sup, &s, &m->g, &c->sup, why, size) != 0) {
        c->sup = NULL;
        return -1;
    }
    r = etude_supervised_result(c->sup);
    c->ran = 1;
    if (r->machine != NULL && r->machine->qemu != NULL) {
        c->keeper = r->machine->qemu->keeper;
        c->process = r->machine->qemu->process;
    }
    c->started_us = posix_now_us();
    c->state = RUNNING;
    return 0;
}

/* The case and load share one admitted resource vector and registered claim. */
static int begin_load(struct set *x, struct member *m)
{
    struct etude_collection_load_spec spec;
    if (make_dir(m->work))
        return -1;
    memset(&spec, 0, sizeof(spec));
    spec.program = x->o->program;
    spec.claim = m->c.id;
    spec.workspace = m->work;
    spec.output = m->c.out;
    spec.load = m->load;
    spec.ledger = x->l;
    m->c.load = etude_collection_load_start(&spec);
    if (!m->c.load)
        return -1;
    m->c.state = LOADING;
    return 0;
}

/** Start the case of member `m`, whose claim and locks are held, in its
 * workspace: a host case's program, a machine case's scenario, or a guest
 * case's machine. */
static void start_case(struct set *x, struct member *m)
{
    struct command *c = &m->c;
    char *vars[CASE_VARS], why[REASON_LEN];
    size_t n = case_vars(m, vars);
    int rc;

    say(x, "run: %s: %s: starting", m->m->suite, m->label);
    why[0] = '\0';
    if ((rc = make_dir(m->work)) != 0)
        (void)fit(why, sizeof(why), "%s: %s", m->work, strerror(errno));
    else if (m->k->guest)
        rc = start_guest(x, m, vars, n, why, sizeof(why));
    else if ((rc = start(x, c, vars, n)) != 0)
        snprintf(why, sizeof(why), "%s", strerror(errno));
    if (rc != 0) {
        came_out(c, INCOMPLETE, 1, "the case could not start: %s", why);
        not_run(x, c, INCOMPLETE, c->reason, 1);
        write_attempt(x, m);
        return;
    }
    running_changed(x, 1);
    status(x, 1);
}

/** Remove the workspace of case member `m` after a pass, and name it
 * otherwise, as its evidence. */
static void scratch_end(struct set *x, const struct member *m)
{
    if (m->c.outcome != PASS)
        say(x, "run: %s: %s: evidence: %s", m->m->suite, m->label, m->work);
    else if (legacy_remove_tree(m->work) != 0) {
        say(x, "run: %s: %s: not removed: %s: %s", m->m->suite, m->label, m->work, strerror(errno));
        x->cleanup_failed = x->failed = 1;
    }
}

/** Take the end of the case `f` that the supervisor gave. */
static void finished(struct set *x, struct etude_supervised *f)
{
    struct member *m = NULL;
    struct command *c;
    size_t i;

    for (i = 0; i < x->n && m == NULL; i++)
        if (x->mem[i].c.sup == f)
            m = &x->mem[i];
    if (m == NULL) {
        etude_supervised_free(f);
        return;
    }
    c = &m->c;
    if (m->g != NULL)
        legacy_guest_serve(m->g);
    judge_case(x, m);
    release(x, c, c->cleanup);
    c->state = DONE;
    running_changed(x, -1);
    if (c->cancelled && c->outcome == CANCELLED && !c->live)
        say(x, "run: %s: %s: stopped before its end; its output so far:", m->m->suite, m->label);
    drain(x, c, 1);
    scratch_end(x, m);
    etude_supervised_free(f);
    c->sup = NULL;
    legacy_guest_free(m->g);
    m->g = NULL;
    write_attempt(x, m);
    status(x, 1);
    if ((c->outcome == FAILURE || c->outcome == INCOMPLETE) && x->o->stop_on_fail)
        stop_all(x, "the run stopped on the first failure", "stopped on the first failure");
}

/** Stop the run once every member of a suite that `stops` has ended and one
 * of them did not pass, so that the other cases of such a suite still run
 * after one of them fails. */
static void stop_on_suites(struct set *x)
{
    char what[200];
    size_t i, j;

    for (i = 0; i < x->n && !x->stopping; i++) {
        const struct etude_legacy_member *s = x->mem[i].m;
        int ended = 1, failed = 0;

        if (!x->mem[i].first || !s->stops)
            continue;
        for (j = i; j < x->n; j++) {
            const struct command *c = &x->mem[j].c;

            if (x->mem[j].m != s)
                continue;
            ended &= c->state == DONE;
            failed |= c->outcome == FAILURE || c->outcome == INCOMPLETE;
        }
        if (ended && failed) {
            snprintf(what, sizeof(what), "stopped: %s failed", s->suite);
            stop_all(x, "a suite that stops the run did not pass", what);
        }
    }
}

/** Move member `m` on: from its admission to its locks, and from its locks
 * to its start, or end it when it waited too long. */
static void advance(struct set *x, size_t i)
{
    struct member *m = &x->mem[i];
    struct command *c = &m->c;
    struct etude_job *j = &x->jobs[i];
    int rc;

    if (c->state == LOADING) {
        rc = etude_collection_load_poll(c->load);
        if (rc > 0)
            start_case(x, m);
        else if (rc < 0) {
            not_run(x, c, INCOMPLETE, "the reserved load did not become ready", 1);
            write_attempt(x, m);
        }
        return;
    }
    if (c->state == QUEUED) {
        if (j->state == ETUDE_JOB_ADMITTED) {
            granted(x, c);
            c->state = LOCKING;
            c->tried_us = 0;
        } else if (j->state == ETUDE_JOB_REFUSED) {
            came_out(c, INCOMPLETE, 1, "the machine's ledger did not admit it: %s",
                     j->error == ERANGE ? "the machine's limits cannot hold it"
                                        : "it cannot fit beside claims only a person can release");
            not_run(x, c, INCOMPLETE, c->reason, 1);
            write_attempt(x, m);
            return;
        } else if (waited_out(x, c)) {
            if (j->state == ETUDE_JOB_WAITING && etude_ledger_withdraw(x->l, c->id) != 0 &&
                errno != ENOENT)
                x->failed = 1;
            j->state = ETUDE_JOB_PENDING;
            came_out(c, INCOMPLETE, 1, "it waited more than %u s for the machine", x->o->wait);
            not_run(x, c, INCOMPLETE, c->reason, 1);
            write_attempt(x, m);
            return;
        }
    }
    if (c->state != LOCKING || posix_now_us() - c->tried_us < LOCK_US)
        return;
    if (c->exclusive)
        status(x, 1);
    rc = lock(x, c, c->exclusive ? wait_left_ms(x, c) : 0);
    if (rc == 1) {
        if (!m->load)
            start_case(x, m);
        else if (begin_load(x, m)) {
            not_run(x, c, INCOMPLETE, "the reserved load could not start", 1);
            write_attempt(x, m);
        }
    } else if (rc < 0) {
        not_run(x, c, INCOMPLETE, c->reason, 1);
        write_attempt(x, m);
    } else if (waited_out(x, c) && !signalled(x)) {
        came_out(c, INCOMPLETE, 1, "the old runner's locks stayed taken for more than %u s",
                 x->o->wait);
        not_run(x, c, INCOMPLETE, c->reason, 1);
        write_attempt(x, m);
    }
}

/* Readiness has a pipe event; wait on it rather than polling a quiet load. */
static int wait_loading(struct set *x)
{
    for (size_t i = 0; i < x->n; i++)
        if (x->mem[i].c.state == LOADING) {
            etude_collection_load_wait(x->mem[i].c.load, ASK_MS);
            return 1;
        }
    return 0;
}

/** Run the members until each is done. */
static void drive(struct set *x)
{
    struct etude_supervised *f;
    size_t i, busy, left;

    for (;;) {
        if (signalled(x) && !x->stopping)
            stop_all(x, x->stop_reason, NULL);
        stop_on_suites(x);
        mark_ready(x);
        if (!x->stopping &&
            etude_sched_admit(x->l, x->o->account, x->jobs, x->n, x->cap, x->ncap, NULL) != 0) {
            x->failed = 1;
            say(x, "etude run: the machine's ledger failed: %s", strerror(errno));
            stop_all(x, "the machine's ledger failed", NULL);
        }
        for (i = 0, busy = 0, left = 0; i < x->n; i++) {
            struct command *c = &x->mem[i].c;

            if (!x->stopping)
                advance(x, i);
            if (c->state == RUNNING) {
                busy++;
                if (c->load && etude_collection_load_poll(c->load) < 0 && !c->cancelled) {
                    if (x->mem[i].g)
                        legacy_guest_cancel(x->mem[i].g);
                    else
                        etude_supervised_cancel(c->sup);
                    c->cancelled = 1;
                }
                if (x->mem[i].g != NULL)
                    legacy_guest_serve(x->mem[i].g);
                feed(c);
                if (c->live)
                    drain(x, c, 0);
            }
            if (c->state != DONE)
                left++;
        }
        if (left == 0)
            return;
        status(x, 0);
        if (busy == 0) {
            if (wait_loading(x))
                continue;
#ifdef _WIN32
            Sleep(ASK_MS);
#else
            poll(NULL, 0, ASK_MS);
#endif
            continue;
        }
        f = etude_supervise_wait(x->sup, ASK_MS);
        if (f != NULL)
            finished(x, f);
    }
}

/* ---- The run. ---- */

/** Print the refusal of the request for `why` and give its status, 2. */
static int refuse(struct set *x, const char *why)
{
    say(x, "etude run: result: refused: %s", why);
    return 2;
}

/** Say whether `s` is a commit's name: 40 or 64 lowercase hex digits. */
static int commit_name(const char *s)
{
    size_t n = strspn(s, "0123456789abcdef");

    return s[n] == '\0' && (n == 40 || n == 64);
}

/** Take the architectures of the request, `i686`, `x86_64` or
 * `i686,x86_64`; 0, or 2 after a refusal. */
static int check_arches(struct set *x)
{
    const char *arch = x->o->arch;

    if (arch != NULL && strcmp(arch, "i686,x86_64") == 0) {
        x->arches[0] = "i686";
        x->arches[1] = "x86_64";
        x->narches = 2;
    } else if (arch != NULL && (strcmp(arch, "i686") == 0 || strcmp(arch, "x86_64") == 0)) {
        x->arches[0] = arch;
        x->narches = 1;
    } else {
        return refuse(x, "the architecture must be i686 or x86_64, or both as i686,x86_64");
    }
    return 0;
}

/** Say whether `arch` is one of the set's architectures. */
static int set_arch(const struct set *x, const char *arch)
{
    size_t i;

    for (i = 0; i < x->narches; i++)
        if (strcmp(x->arches[i], arch) == 0)
            return 1;
    return 0;
}

/** Check the suites of the request, and give each a member of its own for
 * each architecture it runs on: the one it names, or each of the set's;
 * 0, or 2 after a refusal. */
static int check_members(struct set *x)
{
    const struct etude_legacy_set_opts *o = x->o;
    struct etude_legacy_member *m;
    size_t i, j, a;

    if (o->nmembers == 0)
        return refuse(x, "the set has no suite");
    x->members = calloc(o->nmembers * x->narches, sizeof(*x->members));
    x->again = calloc(o->nmembers * x->narches, 1);
    if (x->members == NULL || x->again == NULL)
        return refuse(x, "memory ran out");
    for (i = 0; i < o->nmembers; i++) {
        if (!suite_name(o->members[i].suite))
            return refuse(x, "a suite's name is lowercase letters, digits and '-'");
        if (o->members[i].cases == NULL)
            return refuse(x, "a suite is run by the cases of a manifest, and one names none");
        if (o->members[i].arch != NULL && !set_arch(x, o->members[i].arch))
            return refuse(x, "a suite's architecture is not one of the set's");
        for (a = 0; a < x->narches; a++) {
            if (o->members[i].arch != NULL && strcmp(o->members[i].arch, x->arches[a]) != 0)
                continue;
            m = &x->members[x->nmembers];
            *m = o->members[i];
            m->arch = x->arches[a];
            for (j = 0; j < x->nmembers; j++) {
                if (strcmp(m->suite, x->members[j].suite) != 0)
                    continue;
                if (strcmp(m->arch, x->members[j].arch) == 0)
                    return refuse(x, "the set names a suite twice");
                x->again[x->nmembers] = 1;
            }
            x->nmembers++;
        }
    }
    return 0;
}

/** Check the paths and the limits the request gives; 0, or 2 after a
 * refusal. */
static int check_given(struct set *x)
{
    const struct etude_legacy_set_opts *o = x->o;
    size_t i;

    if (o->tree == NULL || o->program == NULL || o->results == NULL || o->evidence == NULL ||
        o->schemas == NULL || o->account == NULL || o->account[0] == '\0')
        return refuse(x, "the tree, the program, the results, the evidence, the schemas and the "
                         "account must be given");
    if (o->ledger == NULL || !legacy_absolute(o->ledger) ||
        (o->locks != NULL && !legacy_absolute(o->locks)))
        return refuse(x, "the ledger and the lock directory must be absolute paths");
    for (i = 0; i < 3; i++)
        if (o->cap[i] == 0)
            return refuse(x, "every limit of the machine must be above 0");
    if (o->accel == NULL || (strcmp(o->accel, "kvm") != 0 && strcmp(o->accel, "hvf") != 0 &&
                             strcmp(o->accel, "whpx") != 0 && strcmp(o->accel, "tcg") != 0))
        return refuse(x, "the accelerator must be kvm, hvf, whpx or tcg");
    if (o->cpus == 0 || o->cpus > 64 || o->seconds == 0 || o->wait == 0)
        return refuse(x, "the processors, the wait and the bound must be above 0");
    return 0;
}

/**
 * Check that the set can give its machine cases their `ports` local ports,
 * and find the first in `x->port_base`; 0, or 2 after a refusal. They are
 * of the port block that RACH2_PORT_OFFSET names, which the caller holds, so
 * a set with `-d`, whose claims take blocks of their own, cannot give them.
 */
static int check_ports(struct set *x, unsigned long long ports)
{
    const char *offset = getenv("RACH2_PORT_OFFSET");
    unsigned long long base;

    if (x->o->native_locks != NULL) {
        x->port_base = PORT_BASE;
        return ports <= LEGACY_PORTS
                   ? 0
                   : refuse(x, "the machine cases need more local ports than the port block has");
    }
    if (x->o->locks != NULL)
        return refuse(x, "a machine case's local ports are of the caller's port block, and "
                         "with a lock directory the caller holds none");
    if (offset == NULL || offset[0] == '\0' || offset[strspn(offset, "0123456789")] != '\0' ||
        strlen(offset) > 5)
        return refuse(x, "the machine cases need local ports, so RACH2_PORT_OFFSET must name "
                         "the port block the caller holds");
    base = PORT_BASE + strtoull(offset, NULL, 10);
    if (ports > LEGACY_PORTS || base + LEGACY_PORTS > 65536u)
        return refuse(x, "the machine cases need more local ports than the port block has");
    x->port_base = (unsigned)base;
    return 0;
}

/** Say whether member `i` of the request runs case `k` of its manifest: a
 * case of no architecture runs with the first member of its suite alone. */
static int runs_case(const struct set *x, size_t i, const struct legacy_case *k)
{
    return !x->again[i] || strcmp(k->arch, "none") != 0;
}

/** Read the manifest of each suite, and count the members of its cases in
 * `x->n`; 0, or 2 after a refusal. */
static int check_cases(struct set *x)
{
    const struct etude_legacy_set_opts *o = x->o;
    char why[PATH_LEN + 80];
    size_t i, k;
    unsigned long long ports = 0;
    int machines = 0;

    if (realpath(o->tree, x->tree) == NULL) {
        snprintf(why, sizeof(why), "%s: %s", o->tree, strerror(errno));
        return refuse(x, why);
    }
    if ((x->manifests = calloc(x->nmembers, sizeof(*x->manifests))) == NULL)
        return refuse(x, "memory ran out");
    for (i = 0; i < x->nmembers; i++) {
        if (legacy_manifest_read(&x->manifests[i], x->tree, x->members[i].cases,
                                 x->members[i].suite, x->members[i].arch, o->kernel) != 0)
            return refuse(x, x->manifests[i].why);
        for (k = 0; k < x->manifests[i].ncases; k++) {
            if (!runs_case(x, i, &x->manifests[i].cases[k]))
                continue;
            size_t factor = o->repeat && o->repeat->count ? o->repeat->count : 1;
            uint64_t repetitions = (uint64_t)x->manifests[i].cases[k].reps;
            if ((o->repeat && repetitions > UINT_MAX / factor) || repetitions > SIZE_MAX / factor ||
                x->n > SIZE_MAX / sizeof(struct member) - repetitions * factor)
                return refuse(x, "the requested repetition inventory is too large");
            x->n += (size_t)repetitions * factor;
            machines |= x->manifests[i].cases[k].machine;
            ports += (unsigned long long)x->manifests[i].cases[k].ports *
                     (unsigned long long)x->manifests[i].cases[k].reps *
                     (o->repeat && o->repeat->count ? o->repeat->count : 1);
        }
    }
    if (ports > 0 && check_ports(x, ports) != 0)
        return 2;
    if (machines && (!legacy_absolute(o->results) || !legacy_absolute(o->evidence)))
        return refuse(x, "a machine case's scenario runs in its workspace and is given the "
                         "paths of its invocation, so the results and the evidence must be "
                         "absolute paths");
    return 0;
}

/** Read whether the site's policy `x->o->site` permits the lazy boundary
 * profile: it does when a whole line of it is BOUNDARY_PERMIT. 0, or 2
 * after a refusal when the policy cannot be read. */
static int check_site(struct set *x)
{
    char line[256], why[PATH_LEN + 80];
    size_t n;
    int first = 1, last;
    FILE *f;

    if (x->o->site == NULL)
        return 0;
    if ((f = fopen(x->o->site, "r")) == NULL) {
        snprintf(why, sizeof(why), "the site's policy %s: %s", x->o->site, strerror(errno));
        return refuse(x, why);
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        n = strlen(line);
        last = (n > 0 && line[n - 1] == '\n') || feof(f);
        if (n > 0 && line[n - 1] == '\n')
            line[--n] = '\0';
        if (first && last && strcmp(line, BOUNDARY_PERMIT) == 0)
            x->boundary_lazy = 1;
        first = last;
    }
    if (ferror(f)) {
        snprintf(why, sizeof(why), "the site's policy %s cannot be read", x->o->site);
        fclose(f);
        return refuse(x, why);
    }
    fclose(f);
    return 0;
}

/** Check the request before anything runs; 0, or 2 after a refusal. */
static int check(struct set *x)
{
    const struct etude_legacy_set_opts *o = x->o;

    if (o->repeat && (!o->repeat->seeded || !etude_collection_repeat_valid(o->repeat)))
        return refuse(x, "repetitions and loads require an explicit valid seed");
    if (check_arches(x) != 0 || check_members(x) != 0)
        return 2;
    if (check_given(x) != 0 || check_site(x) != 0)
        return 2;
    if (o->commit != NULL && !commit_name(o->commit))
        return refuse(x, "the commit must be 40 or 64 lowercase hexadecimal digits");
    return check_cases(x);
}

/** Name the files of case member `m`, the repetition `m->rep` of its case:
 * its item, its attempt, its workspace, its output and the judge's, its
 * program, a machine case's invocation, and the variables its program is
 * given; 0, or -1. In a set of both architectures, the names of its files
 * and its label give its item's architecture as well. */
static int case_paths(struct set *x, struct member *m)
{
    const struct legacy_case *k = m->k;
    struct command *c = &m->c;
    char tail[200], label[180];
    size_t v = 0;
    int both = x->narches > 1, arch = both && strcmp(k->arch, "none") != 0;

    m->arch = k->arch;
    if ((both ? fit(tail, sizeof(tail), "%s.%s.r%lld", k->case_id, k->arch, m->rep)
              : fit(tail, sizeof(tail), "%s.r%lld", k->case_id, m->rep)) != 0 ||
        fit(m->case_id, sizeof(m->case_id), "%s", k->case_id) != 0 ||
        (arch ? fit(label, sizeof(label), "%s on %s", k->case_id, k->arch)
              : fit(label, sizeof(label), "%s", k->case_id)) != 0 ||
        (m->rep > 0 ? fit(m->label, sizeof(m->label), "%s r%lld", label, m->rep)
                    : fit(m->label, sizeof(m->label), "%s", label)) != 0 ||
        fit(m->item_id, sizeof(m->item_id), "%s:%s:%s:r%lld", x->run_id, k->case_id, k->arch,
            m->rep) != 0 ||
        fit(c->id, sizeof(c->id), "%s:a0", m->item_id) != 0 ||
        fit(m->work, sizeof(m->work), "%s/%s.%s", x->o->evidence, x->run_id, tail) != 0 ||
        fit(c->out, sizeof(c->out), "%s/%s.log", x->output, tail) != 0 ||
        fit(m->judged_out, sizeof(m->judged_out), "%s/%s.results", x->output, tail) != 0 ||
        fit(m->judge_err, sizeof(m->judge_err), "%s/%s.judge", x->output, tail) != 0 ||
        fit(m->guest_err, sizeof(m->guest_err), "%s/%s.err", x->output, tail) != 0 ||
        fit(m->invocation, sizeof(m->invocation), "%s/%s.invocation", x->output, tail) != 0 ||
        fit(m->program, sizeof(m->program), "%s/%s", x->tree, k->goals[k->program]) != 0)
        return -1;
    snprintf(m->vars[v++], sizeof(m->vars[0]), "ETUDE_CASE=%s", k->case_id);
    if (k->seeded)
        snprintf(m->vars[v++], sizeof(m->vars[0]), "ETUDE_SEED=%lld", k->seed);
    if (strcmp(k->arch, "none") != 0)
        snprintf(m->vars[v++], sizeof(m->vars[0]), "ARCH=%s", k->arch);
    if (k->scratch)
        snprintf(m->vars[v++], sizeof(m->vars[0]), "TMPDIR=%s", m->work);
    if (!k->guest)
        snprintf(m->vars[v++], sizeof(m->vars[0]), "ETUDE_TREE=%s", x->tree);
#ifdef _WIN32
    snprintf(m->vars[v++], sizeof(m->vars[0]), "ETUDE_COLLECTION_ARCH=%s", item_arch(x, k));
    if (fit(m->vars[v++], sizeof(m->vars[0]), "PATH=%s/build/tools/etude/commands;%s", x->tree,
            getenv("PATH") != NULL ? getenv("PATH") : "") != 0)
        return -1;
#endif
    return 0;
}

/* Reserve the selected estimate, not the initial declaration as a floor.
 * A demand above capacity cannot become admissible by reducing its claim. */
static int case_cpu(const struct set *x, const struct member *m, uint64_t *amount)
{
    *amount = m->cost.cpu_millis;
    if (*amount > x->cap[ETUDE_LEGACY_CPU].amount) {
        errno = ERANGE;
        return -1;
    }
    return 0;
}

/**
 * Fill the command of case member `m` from its case, to run in its
 * workspace under the case's own bounds, with a claim of its resources held
 * within the ledger's limits. A host case's program takes the old host lock
 * shared and no port. A guest case's machine, whose memory is what its QEMU
 * profile reserves, takes a port block of the old runner's beside the host
 * lock, which it takes alone when it claims more than half the memory, so
 * that no old run of the machine shares its ports; its watchdog bounds the
 * stages the manifest bounds. A machine case's scenario is given its
 * invocation, and the bound of its cleanup stage beside the grace.
 */
static int case_command(struct set *x, struct member *m)
{
    const struct legacy_case *k = m->k;
    struct command *c = &m->c;
    uint64_t want[3];
    size_t i, a = 0;

    if (case_paths(x, m) != 0)
        return -1;
    c->name = m->label;
    c->cwd = m->work;
    m->arch = k->arch;
    if (case_cpu(x, m, &want[ETUDE_LEGACY_CPU]))
        return -1;
    want[ETUDE_LEGACY_MEMORY] = legacy_case_memory(k, x->o->prepared ? 0 : TOOLING_HOST);
    want[ETUDE_LEGACY_DISK] = k->disk_bytes;
    if (m->load) {
        uint64_t extra[3];
        if (etude_collection_load_resources(m->load, extra))
            return -1;
        for (i = 0; i < 3; i++) {
            if (want[i] > x->cap[i].amount || extra[i] > x->cap[i].amount - want[i]) {
                errno = ERANGE;
                return -1;
            }
            want[i] += extra[i];
        }
    }
    for (i = 0; i < 3; i++) {
        if (want[i] == 0)
            continue;
        c->res[c->nres].name = resource_names[i];
        c->res[c->nres++].amount = want[i] < x->cap[i].amount ? want[i] : x->cap[i].amount;
    }
    if (k->guest) {
        c->ports = x->o->locks != NULL;
        c->alone = c->ports && k->memory_bytes > x->cap[ETUDE_LEGACY_MEMORY].amount / 2;
        memcpy(c->wd.stage_us, k->stage_us, sizeof(c->wd.stage_us));
    }
    c->exclusive = is_serial(m) || c->alone;
    c->is_case = 1;
    c->live = x->n == 1;
    c->grace_us = CASE_GRACE_US + (k->machine ? k->stage_us[ETUDE_STAGE_CLEANUP] : 0);
    c->wd.case_us = k->case_us;
    c->wd.gap_us = k->gap_us;
    c->wd.witnesses = k->witnesses;
    c->wd.witness_count = k->nwitnesses;
    c->cpu_us.method = "the case did not start";
    c->cleanup = ETUDE_CLEANUP_CONFIRMED;
    c->end = ETUDE_END_STOPPED;
    c->value = -1;
    if ((c->argv = calloc(k->nargs + 7, sizeof(*c->argv))) == NULL) {
        errno = ENOMEM;
        return -1;
    }
    c->argv[a++] = m->program;
    if (k->machine)
        c->argv[a++] = m->invocation;
    for (i = 0; i < k->nargs; i++)
        c->argv[a++] = (char *)k->args[i];
    x->jobs[m - x->mem].id = c->id;
    x->jobs[m - x->mem].case_id = m->case_id;
    x->jobs[m - x->mem].arch = k->arch;
    x->jobs[m - x->mem].cost_us = m->cost.elapsed_us;
    x->jobs[m - x->mem].res = c->res;
    x->jobs[m - x->mem].nres = c->nres;
    return 0;
}

/** Say whether goal `goal` is among the `n` arguments `argv` that follow a
 * `-b`. */
static int given(char *const *argv, size_t n, const char *goal)
{
    size_t i;

    for (i = 1; i < n; i++)
        if (strcmp(argv[i - 1], "-b") == 0 && strcmp(argv[i], goal) == 0)
            return 1;
    return 0;
}

/** Add to the preparation's arguments `argv`, of which `*k` are given, a
 * `-b` for each artifact of a case member, once. */
static void case_goals(const struct set *x, char **argv, size_t *k)
{
    const struct legacy_case *c;
    size_t i, g;

    for (i = 0; i < x->n; i++) {
        c = x->mem[i].k;
        for (g = 0; g < c->ngoals; g++)
            if (!given(argv, *k, c->goals[g])) {
                argv[(*k)++] = "-b";
                argv[(*k)++] = c->goals[g];
            }
    }
}

/** Give the count of the artifacts of the case members, each counted as
 * often as its members have it. */
static size_t goal_count(const struct set *x)
{
    size_t i, n = 0;

    for (i = 0; i < x->n; i++)
        n += x->mem[i].k->ngoals;
    return n;
}

/** Fill the command of the preparation from the request, with the
 * artifacts of the case members as its goals. */
static int prep_command(struct set *x)
{
    const struct etude_legacy_set_opts *o = x->o;
    struct command *c = &x->prep;
    size_t i, k = 0;

    if (fit(c->id, sizeof(c->id), "%s:prepare", x->run_id) != 0 ||
        fit(c->out, sizeof(c->out), "%s/prepare.log", x->output) != 0)
        return -1;
    c->name = "prepare";
    for (i = 0; i < x->ncap; i++)
        c->res[i] = x->cap[i];
    c->nres = x->ncap;
    c->exclusive = c->alone = 1;
    c->live = 1;
    c->grace_us = PREP_GRACE_US;
    c->wd.case_us = ((uint64_t)o->seconds + MARGIN_S) * 1000000u;
    c->wd.gap_us = c->wd.case_us;
    c->cpu_us.method = "the preparation did not start";
    c->cleanup = ETUDE_CLEANUP_CONFIRMED;
    c->end = ETUDE_END_STOPPED;
    c->value = -1;
    c->cwd = o->tree;
    if (o->prepared)
        return 0;
    if ((c->argv = calloc(2 * goal_count(x) + 20, sizeof(*c->argv))) == NULL) {
        errno = ENOMEM;
        return -1;
    }
    c->argv[k++] = (char *)o->program;
    c->argv[k++] = "prepare";
    c->argv[k++] = "-t";
    c->argv[k++] = (char *)o->tree;
    c->argv[k++] = "-a";
    c->argv[k++] = (char *)o->arch;
    if (o->kernel != NULL) {
        c->argv[k++] = "-n";
        c->argv[k++] = (char *)o->kernel;
    }
    c->argv[k++] = "-o";
    c->argv[k++] = x->record;
    c->argv[k++] = "-s";
    c->argv[k++] = x->seconds;
    case_goals(x, c->argv, &k);
    return 0;
}

/** Make the run's identity and read the machine's; 0, or 2 after a
 * refusal. */
static int identify(struct set *x)
{
    const struct etude_legacy_set_opts *o = x->o;
    char host[256], stamp[32];
#ifndef _WIN32
    struct utsname u;
#endif
    time_t now = time(NULL);
    struct tm tm;

    if (gmtime_r(&now, &tm) == NULL || strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &tm) == 0)
        return refuse(x, "the clock cannot be read");
    if (o->context) {
        const char *text = etude_context_text(o->context, "source");
        struct eh_json *source = text ? eh_json_parse(text) : NULL;
        const struct eh_json *commit = eh_json_get(source, "commit");
        int matching =
            o->commit && commit && commit->type == 's' && !strcmp(o->commit, commit->string);
        eh_json_free(source);
        if (!matching)
            return refuse(x, "the requested commit differs from the verified source commit");
    }
    const char *allocated = etude_context_run_id(o->context);
    if (allocated && o->run_id && strcmp(allocated, o->run_id))
        return refuse(x, "the allocated run identity differs from its context");
    if (!allocated)
        allocated = o->run_id;
    if (allocated) {
        if (!eh_identity(allocated) || strlen(allocated) >= sizeof x->run_id)
            return refuse(x, "the allocated run identity is invalid or too long");
        strcpy(x->run_id, allocated);
    } else
        snprintf(x->run_id, sizeof(x->run_id), "tests.%s.%s.%ld",
                 x->narches > 1 ? "i686-x86_64" : o->arch, stamp, (long)getpid());
    snprintf(x->seconds, sizeof(x->seconds), "%u", o->seconds);
#ifdef _WIN32
    DWORD host_size = sizeof(host);
    if (!GetComputerNameA(host, &host_size))
#else
    if (gethostname(host, sizeof(host)) != 0)
#endif
        snprintf(host, sizeof(host), "localhost");
    host[sizeof(host) - 1] = '\0';
    token_of(host, x->machine_id, sizeof(x->machine_id));
    boot_read(x->boot_id, sizeof(x->boot_id));
    struct etude_context_clock clock;
    if (!etude_context_clock(&clock)) {
        strcpy(x->machine_id, clock.machine_id);
        strcpy(x->boot_id, clock.boot_id);
    } else if (o->context)
        return refuse(x, "the measurement clock identity cannot be read");
#if defined(_WIN32)
    x->os = "windows";
    if (strcmp(x->boot_id, "-") == 0)
        return refuse(x, "the Windows boot identifier cannot be read");
#elif defined(__APPLE__)
    x->os = "macos";
#else
    x->os = "linux";
#endif
#ifdef _WIN32
    etude_win_version(x->os_version, sizeof x->os_version);
#else
    if (uname(&u) == 0)
        snprintf(x->os_version, sizeof(x->os_version), "%s", u.release);
    else
        snprintf(x->os_version, sizeof(x->os_version), "unknown");
#endif
    return 0;
}

/** Make the run's directories under the results, and name its stream `path`
 * in the directory `dir`; 0, or 2 after a refusal or an error. */
static int make_paths(struct set *x, char dir[PATH_LEN], char path[PATH_LEN])
{
    const struct etude_legacy_set_opts *o = x->o;

    if (fit(x->store, sizeof(x->store), "%s/store", o->results) != 0 ||
        fit(x->output, sizeof(x->output), "%s/output", o->results) != 0 ||
        fit(dir, PATH_LEN, "%s/history", o->results) != 0 ||
        fit(path, PATH_LEN, "%s/%s.jsonl", dir, x->run_id) != 0 ||
        fit(x->schema, sizeof(x->schema), "%s/history.schema.json", o->schemas) != 0 ||
        (o->record != NULL ? fit(x->record, sizeof(x->record), "%s", o->record)
                           : fit(x->record, sizeof(x->record), "%s/prepare.record", o->results)))
        return refuse(x, "a path of the run is too long");
    if (o->history_path != NULL && fit(o->history_path, o->history_path_size, "%s", path) != 0)
        return refuse(x, "the stream path does not fit its caller's buffer");
    if (make_dir(o->results) != 0 || make_dir(dir) != 0 || make_dir(x->store) != 0 ||
        make_dir(x->output) != 0 || make_dir(o->evidence) != 0) {
        say(x, "etude run: result: error: %s: %s", o->results, strerror(errno));
        return 2;
    }
    return 0;
}

/** Give suite `s` of the request its members from `k` on, each repetition
 * of each case of its manifest that it runs; give the next member's place. */
static size_t take_members(struct set *x, size_t s, size_t k)
{
    const struct legacy_manifest *man = &x->manifests[s];
    size_t c;
    long long r;

    for (c = 0; c < man->ncases; c++)
        for (r = 0; runs_case(x, s, &man->cases[c]) &&
                    r < man->cases[c].reps *
                            (x->o->repeat && x->o->repeat->count ? x->o->repeat->count : 1);
             r++, k++) {
            x->mem[k].m = &x->members[s];
            x->mem[k].manifest = man;
            x->mem[k].k = &man->cases[c];
            x->mem[k].rep = r;
        }
    return k;
}

/* Apply the seed policy before paths or job pointers refer to members. */
static int repeat_members(struct set *x)
{
    const struct etude_collection_repeat *r = x->o->repeat;
    if (!r)
        return 0;
    struct member *ordered = calloc(x->n, sizeof(*ordered));
    size_t *first = calloc(x->n, sizeof(*first));
    x->repeat_cases = calloc(x->n, sizeof(*x->repeat_cases));
    if (!ordered || !first || !x->repeat_cases) {
        free(ordered);
        free(first);
        return -1;
    }
    for (size_t i = 0; i < x->n; i++) {
        struct member *m = &x->mem[i];
        if (m->rep)
            continue;
        size_t at = x->repeat_count++;
        first[at] = i;
        x->repeat_cases[at].case_id = m->k->case_id;
        x->repeat_cases[at].arch = m->k->arch;
        x->repeat_cases[at].repetitions = (unsigned)m->k->reps * (r->count ? r->count : 1);
    }
    struct etude_repeat_spec spec;
    etude_collection_repeat_spec(r, x->repeat_cases, x->repeat_count, x->o->width, &spec);
    size_t count = 0;
    int rc = etude_repeat_plan(&spec, &x->repeat_items, &count);
    if (!rc && count == x->n) {
        x->repeat_selection = etude_repeat_record(&spec, "selection", x->repeat_items, count);
        rc = x->repeat_selection ? 0 : -1;
        for (size_t i = 0; i < count; i++) {
            struct etude_repeat_item *item = &x->repeat_items[i];
            ordered[i] = x->mem[first[item->case_index] + item->repetition];
            ordered[i].load = item->load == ETUDE_REPEAT_NO_LOAD ? NULL : &r->loads[item->load];
        }
        if (!rc)
            memcpy(x->mem, ordered, count * sizeof(*ordered));
    } else
        rc = -1;
    free(first);
    free(ordered);
    return rc;
}

/**
 * Put the members from `x->mem[from]` on in the order etude_sched_order
 * gives their jobs: the costlier case first, by its expected time, then by
 * case and architecture, so that the long cases of both architectures start
 * first. Each member's paths are named and nothing points into it yet; 0, or
 * -1 when memory ran out.
 */
static int order_members(struct set *x, size_t from)
{
    size_t n = x->n - from, i, j;
    struct etude_job *jobs = calloc(n + 1, sizeof(*jobs));
    struct member *sorted = calloc(n + 1, sizeof(*sorted));

    if (jobs == NULL || sorted == NULL) {
        free(jobs);
        free(sorted);
        errno = ENOMEM;
        return -1;
    }
    for (i = 0; i < n; i++) {
        jobs[i].id = x->mem[from + i].c.id;
        jobs[i].case_id = x->mem[from + i].case_id;
        jobs[i].arch = x->mem[from + i].k->arch;
        jobs[i].cost_us = x->mem[from + i].cost.elapsed_us;
    }
    etude_sched_order(jobs, n);
    for (i = 0; i < n; i++) {
        for (j = from; x->mem[j].c.id != jobs[i].id; j++) {
        }
        sorted[i] = x->mem[j];
    }
    memcpy(x->mem + from, sorted, n * sizeof(*sorted));
    free(jobs);
    free(sorted);
    return 0;
}

/* Keep explicit serial suites and quiet host groups before the shared tail.
 * Seeded plans bypass this partition and retain their allocated item order. */
static int quiet_members(struct set *x, size_t *prefix)
{
    struct member *ordered = calloc(x->n, sizeof(*ordered));
    size_t i, k = 0;
    if (ordered == NULL)
        return -1;
    for (i = 0; i < x->n; i++)
        if (is_serial(&x->mem[i]))
            ordered[k++] = x->mem[i];
    *prefix = k;
    for (i = 0; i < x->n; i++)
        if (!is_serial(&x->mem[i]))
            ordered[k++] = x->mem[i];
    memcpy(x->mem, ordered, x->n * sizeof(*ordered));
    free(ordered);
    return 0;
}

/** Mark the first item of each member of the request, and give the machine
 * cases their local ports, in the order of the plan; 0, or -1. */
static int number_members(struct set *x)
{
    unsigned char *seen = calloc(x->nmembers + 1, 1);
    size_t i;

    if (seen == NULL) {
        errno = ENOMEM;
        return -1;
    }
    for (i = 0; i < x->n; i++) {
        struct member *m = &x->mem[i];

        m->first = !seen[m->m - x->members];
        seen[m->m - x->members] = 1;
        if (m->k->ports > 0) {
            m->port = x->port_base + x->ports;
            x->ports += m->k->ports;
        }
    }
    free(seen);
    return 0;
}

/** Keep one block until every case claim of this collection is gone. */
static int collection_locks(struct set *x)
{
    struct etude_ledger_res resource = {"collections", 1}, limit = {"collections", 1024};
    struct etude_bridge_spec spec;
    char offset[32];
    if (x->o->native_locks == NULL)
        return 0;
    if (fit(x->collection_claim, sizeof(x->collection_claim), "%s:ports", x->run_id) != 0)
        return -1;
    for (;;) {
        if (signalled(x)) {
            errno = EINTR;
            return -1;
        }
        if (etude_ledger_admit(x->l, x->collection_claim, x->o->account, &resource, 1, &limit, 1,
                               1) == 0)
            break;
        if (errno != EAGAIN)
            return -1;
#ifdef _WIN32
        Sleep(200);
#else
        poll(NULL, 0, 200);
#endif
    }
    memset(&spec, 0, sizeof(spec));
    spec.ledger = x->o->ledger;
    spec.id = x->collection_claim;
    spec.dir = x->o->native_locks;
    spec.alone = x->o->host_alone;
    spec.legacy = x->o->native_ports;
    spec.wait_ms = 200;
    for (;;) {
        if (signalled(x)) {
            errno = EINTR;
            return -1;
        }
        x->collection_bridge = etude_bridge_collection_start(&spec, x->run_id);
        if (x->collection_bridge)
            break;
        if (errno != EAGAIN && errno != EINTR && errno != ECANCELED)
            return -1;
#ifdef _WIN32
        Sleep(200);
#else
        poll(NULL, 0, 200);
#endif
    }
    int port_offset = etude_bridge_offset(x->collection_bridge);
    if (port_offset < 0)
        port_offset = 0;
    snprintf(offset, sizeof(offset), "%d", port_offset);
    x->port_base = PORT_BASE + (unsigned)port_offset;
#ifdef _WIN32
    _putenv_s("RACH2_PORT_OFFSET", offset);
#else
    setenv("RACH2_PORT_OFFSET", offset, 1);
#endif
    return 0;
}

static int start_observer(struct set *x)
{
    struct etude_pid self;
    if (fit(x->observation_path, sizeof x->observation_path, "%s/%s.load.jsonl", x->output,
            x->run_id) ||
        etude_process_state((long)getpid(), &self) != 1 ||
        !(x->observer = etude_observe_start(x->observation_path, &self, x->run_id)) ||
        etude_history_now(&x->start))
        return refuse(x, "the native load observer could not start");
    return 0;
}

/* No member owns a process until admission and execution begin. */
static int allocate_members(struct set *x)
{
    size_t i;
    x->mem = calloc(x->n, sizeof(*x->mem));
    for (i = 0; x->mem && i < x->n; i++)
        x->mem[i].c.cleanup = ETUDE_CLEANUP_CONFIRMED;
    x->jobs = calloc(x->n, sizeof(*x->jobs));
    x->sup = etude_supervisor_new();
    if (x->mem == NULL || x->jobs == NULL || x->sup == NULL) {
        say(x, "etude run: result: error: the run: %s", strerror(ENOMEM));
        return 2;
    }
    return 0;
}

/* Allocate and order the members before commands retain pointers into them. */
static int prepare_members(struct set *x)
{
    const struct etude_legacy_set_opts *o = x->o;
    size_t i, k = 0, serial;
    if (allocate_members(x))
        return 2;
    for (i = 0; i < x->nmembers; i++)
        if (o->repeat || x->members[i].serial)
            k = take_members(x, i, k);
    serial = k;
    for (i = 0; i < x->nmembers; i++)
        if (!o->repeat && !x->members[i].serial)
            k = take_members(x, i, k);
    if (repeat_members(x))
        return refuse(x, "the seeded repetition plan could not be allocated");
    if (!o->repeat && quiet_members(x, &serial))
        return refuse(x, "the quiet-case order could not be allocated");
    if (o->repeat)
        for (i = 0; i < x->n; i++) {
            struct member *m = &x->mem[i];
            m->learned = malloc(sizeof(*m->learned));
            if (!m->learned)
                return refuse(x, "repetition bounds could not be allocated");
            *m->learned = *m->k;
            m->k = m->learned;
        }
    for (i = 0; i < x->n; i++)
        if (case_paths(x, &x->mem[i]) != 0) {
            say(x, "etude run: result: error: the run: %s", strerror(errno));
            return 2;
        }
    if (learn_costs(x))
        return refuse(x, "compatible case costs could not be read");
    if ((!o->repeat && order_members(x, serial) != 0) || number_members(x) != 0) {
        say(x, "etude run: result: error: the run: %s", strerror(errno));
        return 2;
    }
    for (i = 0; i < x->n; i++)
        if (case_command(x, &x->mem[i]) != 0) {
            say(x, "etude run: result: error: the run: %s", strerror(errno));
            return 2;
        }
    if (prep_command(x) != 0) {
        say(x, "etude run: result: error: the run: %s", strerror(errno));
        return 2;
    }
    return 0;
}

/** Make the run's identity, directories, history, ledger and members; 0,
 * or 2 after a refusal or an error. */
static int set_up(struct set *x)
{
    const struct etude_legacy_set_opts *o = x->o;
    char dir[PATH_LEN], path[PATH_LEN];
    size_t i;

    if (identify(x) != 0 || make_paths(x, dir, path) != 0)
        return 2;
    if (load_history(x) != 0)
        return refuse(x, "matching history could not be loaded");
    if ((x->h = etude_history_open(path, x->schema, x->run_id, x->run_id, HISTORY_BYTES)) == NULL) {
        say(x, "etude run: result: error: %s: %s", path, strerror(errno));
        return 2;
    }
    if (start_observer(x))
        return 2;
    budget_limits(x);
    if ((x->l = etude_ledger_open(o->ledger)) == NULL) {
        say(x, "etude run: result: error: %s: %s", o->ledger, strerror(errno));
        return 2;
    }
    if (collection_locks(x) != 0)
        return refuse(x, "the native collection's host and port locks could not be taken");
    for (i = 0; i < 3; i++) {
        x->cap[i].name = resource_names[i];
        x->cap[i].amount = o->cap[i];
    }
    x->ncap = 3;
    return prepare_members(x);
}

/** Give the count of the request's members that have an item. */
static unsigned suites_run(const struct set *x)
{
    unsigned n = 0;
    size_t i;

    for (i = 0; i < x->n; i++)
        n += (unsigned)x->mem[i].first;
    return n;
}

/** Give the count of the suites whose members all passed. */
static unsigned suites_passed(const struct set *x)
{
    unsigned n = 0;
    size_t i, j;
    int ok;

    for (i = 0; i < x->n; i++) {
        if (!x->mem[i].first)
            continue;
        for (j = i, ok = 1; j < x->n; j++)
            ok &= x->mem[j].m != x->mem[i].m || x->mem[j].c.outcome == PASS;
        n += (unsigned)ok;
    }
    return n;
}

/* Finish every owned execution and holder before declaring the run ended. */
static void cleanup_owned(struct set *x)
{
    if (x->owned_cleaned)
        return;
    x->owned_cleaned = 1;
    if (x->sup) {
        etude_supervisor_free(x->sup);
        x->sup = NULL;
    }
    for (size_t i = 0; x->mem && i < x->n; i++) {
        legacy_guest_free(x->mem[i].g);
        x->mem[i].g = NULL;
        x->mem[i].c.cleanup = stop_load(&x->mem[i].c, x->mem[i].c.cleanup);
        if (x->mem[i].c.cleanup != ETUDE_CLEANUP_CONFIRMED)
            x->uncertain = 1;
    }
    if (x->objects) {
        etude_store_close(x->objects);
        x->objects = NULL;
        if (legacy_remove_tree(x->objects_dir)) {
            say(x, "etude run: %s: not removed: %s", x->objects_dir, strerror(errno));
            x->cleanup_failed = 1;
        }
    }
    if (x->l && x->collection_claim[0]) {
        enum etude_cleanup cleanup =
            x->uncertain ? ETUDE_CLEANUP_UNCONFIRMED : ETUDE_CLEANUP_CONFIRMED;
        if (etude_ledger_finish(x->l, x->collection_claim, cleanup)) {
            if (etude_ledger_withdraw(x->l, x->collection_claim))
                x->uncertain = 1;
            x->cleanup_failed = 1;
        }
        x->collection_claim[0] = 0;
    }
    if (x->collection_bridge) {
        if (x->uncertain)
            etude_bridge_detach(x->collection_bridge);
        else
            etude_bridge_join(x->collection_bridge);
        x->collection_bridge = NULL;
    }
    if (x->l) {
        etude_ledger_close(x->l);
        x->l = NULL;
    }
    if (x->o->status && remove(x->o->status) && errno != ENOENT)
        x->cleanup_failed = 1;
    if (x->cleanup_failed || x->uncertain)
        x->failed = 1;
}

static void finish_observation(struct set *x)
{
    struct etude_observation_run r;
    memset(&r, 0, sizeof r);
    if (etude_history_now(&x->end)) {
        x->failed = 1;
        return;
    }
    r.context = x->o->context;
    r.runtime = x->observer;
    r.path = x->observation_path;
    r.store = x->store;
    r.run_id = x->run_id;
    r.start = *charged_start(x);
    r.runtime_start = x->start;
    r.end = x->end;
    r.cleanup_confirmed = !x->uncertain && !x->cleanup_failed && x->context_valid &&
                          (!x->o->prepared || etude_context_preparation(x->o->context));
#ifdef __linux__
    r.physical_required = strstr(x->os_version, "microsoft") ||
                          strstr(x->os_version, "Microsoft") || strstr(x->os_version, "WSL");
#endif
    struct etude_measurement *cpu = calloc(x->n + 1, sizeof(*cpu));
    if (!cpu) {
        x->failed = 1;
        return;
    }
    size_t count = 0;
    if (x->prep.ran)
        cpu[count++] = as_history(&x->prep.cpu_us);
    for (size_t i = 0; i < x->n; i++)
        if (x->mem[i].c.ran)
            cpu[count++] = as_history(&x->mem[i].c.cpu_us);
    r.owned_cpu = etude_history_cpu_sum(cpu, count);
    if (etude_observation_finish(&r, &x->load)) {
        say(x, "etude run: the native load trace is incomplete");
        x->failed = 1;
    }
    free(cpu);
    x->observation_ended = 1;
}

/** Free the remaining record buffers after owned cleanup. */
static void tear_down(struct set *x)
{
    cleanup_owned(x);
    if (!x->observation_ended)
        etude_observation_cancel(x->o->context, x->run_id);
    etude_observe_free(x->observer);
    free(x->load);
    for (size_t i = 0; x->mem && i < x->n; i++) {
        etude_collection_load_free(x->mem[i].c.load);
        free(x->mem[i].c.argv);
        free(x->mem[i].c.envp);
        free(x->mem[i].judged);
        free(x->mem[i].digests);
        free(x->mem[i].learned);
    }
    for (size_t i = 0; x->manifests && i < x->nmembers; i++)
        legacy_manifest_free(&x->manifests[i]);
    free(x->manifests);
    cmp_free(&x->baseline);
    free(x->members);
    free(x->again);
    free(x->prep.argv);
    free(x->prep.envp);
    free(x->repeat_cases);
    free(x->repeat_items);
    free(x->repeat_selection);
    free(x->mem);
    free(x->jobs);
    if (x->h && etude_history_close(x->h))
        x->failed = 1;
}

int etude_legacy_set(const struct etude_legacy_set_opts *o)
{
    struct set x;
    enum outcome worst = PASS;
    unsigned passed = 0;
    size_t i;
    int rc;

    memset(&x, 0, sizeof(x));
    x.o = o;
    x.out = o->out != NULL ? o->out : stdout;
    if ((rc = check(&x)) != 0 || (rc = set_up(&x)) != 0)
        goto out;
    if (write_plan(&x) != 0) {
        say(&x, "etude run: result: error: the plan could not be written: %s",
            strerror(errno != 0 ? errno : EIO));
        rc = 2;
        goto out;
    }
    if (prepare(&x) == 0) {
        publish(&x);
        drive(&x);
    } else {
        for (i = 0; i < x.n; i++)
            not_run(&x, &x.mem[i].c, CANCELLED,
                    signalled(&x) ? x.stop_reason : "the preparation did not pass", 0);
    }
    status(&x, 1);
    if (o->prepared && x.prep.outcome == PASS &&
        etude_legacy_check_tree(x.record, x.tree, 1, x.out) != 0) {
        came_out(&x.prep, INCOMPLETE, 1, "prepared artifacts changed during execution");
        x.failed = 1;
    }
    char context_error[256];
    x.context_valid = !etude_context_recheck(o->context, context_error, sizeof context_error);
    if (!x.context_valid) {
        say(&x, "etude run: %s", context_error);
        x.failed = 1;
    }
    cleanup_owned(&x);
    finish_observation(&x);
    if (write_run(&x, &worst) != 0) {
        x.failed = 1;
        say(&x, "etude run: the run record could not be written: %s",
            strerror(errno != 0 ? errno : EIO));
    }
    passed = suites_passed(&x);
    if (o->stop && *o->stop) {
        rc = 128 + (int)*o->stop;
        say(&x, "etude run: result: stopped by signal %d", (int)*o->stop);
    } else if (x.prep.outcome != PASS) {
        rc = 3;
        say(&x, "etude run: result: preparation failed: %s", x.prep.reason);
    } else {
        rc = worst == PASS && !x.failed && !x.budget_failed ? 0 : 1;
        say(&x, "etude run: result: ran: %u suites, %u passed%s", suites_run(&x), passed,
            x.failed ? ", with an error of the history or the ledger" : "");
    }
out:
    tear_down(&x);
    return !rc && x.failed ? 1 : rc;
}

/* Preserve all selection bytes, including the final LF, in the export. */
static void selection_string(struct cmp_text *d, const char *bytes)
{
    cmp_printf(d, "\"");
    for (const unsigned char *p = (const unsigned char *)bytes; *p; p++) {
        if (*p == '\\' || *p == '\"')
            cmp_printf(d, "\\%c", *p);
        else if (*p < 32)
            cmp_printf(d, "\\u%04x", *p);
        else
            cmp_printf(d, "%c", *p);
    }
    cmp_printf(d, "\"");
}

int etude_legacy_selection_export(const struct etude_legacy_set_opts *o, const char *path)
{
    struct set x;
    struct cmp_text selected = {0}, d = {0};
    char hex[65];
    int rc = 2;
    memset(&x, 0, sizeof x);
    x.o = o;
    x.out = stderr;
    if (check(&x) != 0)
        goto done;
    x.mem = calloc(x.n ? x.n : 1, sizeof *x.mem);
    if (!x.mem)
        goto done;
    size_t count = 0;
    for (size_t i = 0; i < x.nmembers; i++)
        count = take_members(&x, i, count);
    if (count != x.n || repeat_members(&x))
        goto done;
    selection(&x, &selected);
    if (selected.failed)
        goto done;
    digest_of(selected.data, selected.length, hex);
    cmp_printf(&d,
               "{\"schema_version\":1,\"selection_sha256\":\"%s\","
               "\"selection_size_bytes\":%lu,\"selection_bytes\":",
               hex, (unsigned long)selected.length);
    selection_string(&d, selected.data);
    cmp_printf(&d, ",\"items\":[");
    for (size_t i = 0; i < x.n; i++) {
        const struct member *m = &x.mem[i];
        cmp_printf(&d, "%s{\"case_id\":", i ? "," : "");
        string(&d, m->k->case_id);
        cmp_printf(&d, ",\"architecture\":\"%s\",\"repetition\":%lld}", m->k->arch, m->rep);
    }
    cmp_printf(&d, "]}\n");
    if (!d.failed) {
        FILE *f = fopen(path, "wbx");
        if (f) {
            int failed = fwrite(d.data, 1, d.length, f) != d.length;
            failed |= fclose(f) != 0;
            if (failed)
                remove(path);
            else
                rc = 0;
        }
    }
done:
    for (size_t i = 0; x.manifests && i < x.nmembers; i++)
        legacy_manifest_free(&x.manifests[i]);
    free(x.manifests);
    free(x.members);
    free(x.again);
    free(x.repeat_cases);
    free(x.repeat_items);
    free(x.repeat_selection);
    free(x.mem);
    free(selected.data);
    free(d.data);
    return rc;
}
