/*
 * scenario.c: libetude-case on POSIX systems, the library a machine scenario
 * links with. It reads the attempt's invocation and its case, reports the
 * scenario's assertions, boots the case's machines on the QEMU launcher with
 * private copies of their disks, runs host tools and etude-guest's attempts
 * beside them, bounds every wait by its step, and ends what it owns.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../history_json.h"
#include "../serial.h"
#include "etude/artifact.h"
#include "etude/qemu.h"
#include "etude/report.h"
#include "etude/scenario.h"
#include "posix.h"
#include "sha256.h"

extern char **environ;

/* The text the kernel's debug shell prints when it is ready for a command,
 * and the command that makes etude-guest the boot's designated init. */
#define SHELL_READY "Type 'help' for available commands."
#define GUEST_EXEC  "exec /etude-guest --stage / --resend-ms "

/* The most disks, devices and boot options a profile gives the launcher. */
#define DISKS_MAX   4
#define DEVICES_MAX 16
#define OPTIONS_MAX 32

/* The longest invocation, manifest or guest invocation the library reads. */
#define DOCUMENT_MAX (4u * 1024 * 1024)

/* An artifact of the case's `prepare`, with the digest the plan gave it. */
struct artifact {
    const char *name;
    const char *digest;
    char *path; /* the object's path once it checked, else NULL */
    int checked;
    char *copy; /* the attempt's private writable copy, or NULL */
    char *exe;  /* the attempt's executable copy, or NULL */
};

/* A host tool of the case's requirements, with the site's path. */
struct tool {
    const char *name;
    const char *path;
};

/* A step of the manifest, with the bound the plan fixed for it. */
struct step {
    const char *id;
    const char *machine; /* NULL for a step on the host alone */
    uint64_t bound_us;
};

/* A machine of the manifest. */
struct machine_decl {
    const char *id;
    const struct eh_json *qemu;
    int console, qmp, helper;
    unsigned boots;                 /* the boots begun so far */
    struct etude_case_machine *now; /* its latest boot, or NULL */
};

/* A declared assertion of the scenario's, and what it was given. */
struct assertion {
    const char *id;
    int reported; /* reports so far */
    int held;     /* the last report was a pass */
};

/* A file the library made, removed after a pass. */
struct made {
    struct made *next;
    char *path;
    int dir;
};

struct etude_case_machine {
    struct etude_case *c;
    struct etude_case_machine *next;
    struct machine_decl *decl;
    struct etude_machine *m;
    char attempt_id[300];
    const char *disks[DISKS_MAX + 1];
    const char *devices[DEVICES_MAX + 1];
    const char *options[OPTIONS_MAX + 1];
    struct etude_qemu_profile profile;
    struct etude_machine_spec spec;
    struct etude_serial *serial; /* etude-guest's session, for a helper */
    size_t fed;                  /* console bytes fed to the session */
    int serial_closed;
    char line[ETUDE_REPORT_LINE_MAX + 1]; /* the guest's output line being read */
    size_t line_len;
    int line_long;
};

struct etude_case {
    struct eh_json *inv, *manifest;
    const struct eh_json *kase;
    const char *run_id, *attempt_id, *case_id, *arch, *scratch;
    struct etude_store *store;
    struct artifact *arts;
    size_t art_count;
    struct tool *tools;
    size_t tool_count;
    struct step *steps;
    size_t step_count;
    struct machine_decl *decls;
    size_t decl_count;
    struct assertion *asserts;
    size_t assert_count;
    const char *step_witness; /* the first witness of kind `step`, or NULL */
    struct etude_qemu_site site;
    uint64_t grace_us, cleanup_us, console_max, output_max, resend_us;
    long cur;            /* the step that runs, or -1 */
    long next;           /* the first step that may begin */
    uint64_t deadline;   /* when it runs out */
    unsigned steps_done; /* steps ended */
    unsigned records;    /* assert, notrun and branch records written */
    int faults;          /* misuse, failed assertions and lost work */
    int unconfirmed;     /* a process's cleanup could not be confirmed */
    struct etude_qemu *q;
    struct etude_case_machine *machines;
    struct made *made;
    char path[4096]; /* the path etude_evidence gave last */
};

/* The descriptors of the pipe that the stop signals write. */
static int stop_pipe[2] = {-1, -1};
static volatile sig_atomic_t stop_asked;

/** Say what went wrong on stderr, as `etude-case: detail`. */
static void diag(const char *fmt, ...)
{
    va_list ap;

    fputs("etude-case: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/** Note a stop signal in the flag and the pipe. */
static void on_stop(int sig)
{
    int e = errno;
    char b = (char)sig;

    stop_asked = 1;
    if (stop_pipe[1] >= 0 && write(stop_pipe[1], &b, 1) < 0) {
        /* The pipe is full, so a wait is already due to wake. */
    }
    errno = e;
}

/** Make the stop pipe and catch SIGTERM, SIGINT and SIGHUP. */
static int catch_stops(void)
{
    struct sigaction sa;
    int i;

    if (pipe(stop_pipe) != 0)
        return -1;
    for (i = 0; i < 2; i++) {
        fcntl(stop_pipe[i], F_SETFD, FD_CLOEXEC);
        fcntl(stop_pipe[i], F_SETFL, fcntl(stop_pipe[i], F_GETFL) | O_NONBLOCK);
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) != 0 || sigaction(SIGINT, &sa, NULL) != 0 ||
        sigaction(SIGHUP, &sa, NULL) != 0)
        return -1;
    return 0;
}

/* ---- Documents. ---- */

/** Read the file at `path` whole, NUL-terminated; NULL with a diagnostic. */
static char *read_whole(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    size_t n = 0, cap = 4096;

    if (f == NULL) {
        diag("%s: %s", path, strerror(errno));
        return NULL;
    }
    buf = malloc(cap + 1);
    while (buf != NULL) {
        size_t got = fread(buf + n, 1, cap - n, f);

        n += got;
        if (n < cap)
            break;
        if (cap >= DOCUMENT_MAX) {
            diag("%s: longer than %u bytes", path, DOCUMENT_MAX);
            free(buf);
            buf = NULL;
            break;
        }
        cap *= 2;
        {
            char *grown = realloc(buf, cap + 1);

            if (grown == NULL)
                free(buf);
            buf = grown;
        }
    }
    if (buf != NULL && ferror(f)) {
        diag("%s: %s", path, strerror(errno));
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf == NULL)
        return NULL;
    buf[n] = '\0';
    if (len != NULL)
        *len = n;
    return buf;
}

/** Read and parse the JSON document at `path`; NULL with a diagnostic. */
static struct eh_json *read_json(const char *path)
{
    char *text = read_whole(path, NULL);
    struct eh_json *j;

    if (text == NULL)
        return NULL;
    j = eh_json_parse(text);
    free(text);
    if (j == NULL || j->type != 'o') {
        diag("%s: not a JSON object", path);
        eh_json_free(j);
        return NULL;
    }
    return j;
}

/** Give string member `key` of `o`, or NULL. */
static const char *jtext(const struct eh_json *o, const char *key)
{
    const struct eh_json *v = eh_json_get(o, key);

    return v != NULL && v->type == 's' ? v->string : NULL;
}

/** Give integer member `key` of `o`, or 0 for none. */
static uint64_t jnum(const struct eh_json *o, const char *key)
{
    const struct eh_json *v = eh_json_get(o, key);

    return v != NULL && v->type == 'n' && v->number > 0 ? (uint64_t)v->number : 0;
}

/** Give boolean member `key` of `o`: 1, 0, or -1 for none. */
static int jbool(const struct eh_json *o, const char *key)
{
    const struct eh_json *v = eh_json_get(o, key);

    if (v == NULL || (v->type != 't' && v->type != 'f'))
        return -1;
    return v->type == 't';
}

/** Count the items of array or object `v`. */
static size_t jcount(const struct eh_json *v)
{
    const struct eh_json *i;
    size_t n = 0;

    for (i = v != NULL ? v->child : NULL; i != NULL; i = i->next)
        n++;
    return n;
}

/** Say whether array `v` has the string `s`. */
static int jhas(const struct eh_json *v, const char *s)
{
    const struct eh_json *i;

    if (v == NULL || v->type != 'a')
        return 0;
    for (i = v->child; i != NULL; i = i->next)
        if (i->type == 's' && strcmp(i->string, s) == 0)
            return 1;
    return 0;
}

/** Say whether `s` is a file name: no slash, not `.` or `..`, printable. */
static int file_name(const char *s)
{
    const char *p;

    if (s == NULL || *s == '\0' || strcmp(s, ".") == 0 || strcmp(s, "..") == 0 || strlen(s) > 128)
        return 0;
    for (p = s; *p != '\0'; p++)
        if (*p == '/' || *p < '!' || *p > '~')
            return 0;
    return 1;
}

/** Find the case `id` among the manifest's cases. */
static const struct eh_json *find_case(const struct eh_json *manifest, const char *id)
{
    const struct eh_json *k;

    for (k = eh_json_get(manifest, "cases") != NULL ? eh_json_get(manifest, "cases")->child : NULL;
         k != NULL; k = k->next)
        if (jtext(k, "case_id") != NULL && strcmp(jtext(k, "case_id"), id) == 0)
            return k;
    return NULL;
}

/** Read the artifacts: each of the case's `prepare` with the plan's digest. */
static int load_artifacts(struct etude_case *c)
{
    const struct eh_json *prep = eh_json_get(c->kase, "prepare");
    const struct eh_json *given = eh_json_get(c->inv, "artifacts");
    const struct eh_json *p;
    size_t i = 0;

    c->art_count = jcount(prep);
    c->arts = calloc(c->art_count + 1, sizeof(*c->arts));
    if (c->arts == NULL)
        return -1;
    for (p = prep != NULL ? prep->child : NULL; p != NULL; p = p->next, i++) {
        c->arts[i].name = jtext(p, "name");
        c->arts[i].digest = jtext(given, c->arts[i].name != NULL ? c->arts[i].name : "");
        if (c->arts[i].name == NULL || c->arts[i].digest == NULL) {
            diag("the invocation gives no digest of artifact %s",
                 c->arts[i].name != NULL ? c->arts[i].name : "(unnamed)");
            return -1;
        }
    }
    return 0;
}

/** Read the tools: each of the case's requirements with the site's path. */
static int load_tools(struct etude_case *c)
{
    const struct eh_json *names = eh_json_get(eh_json_get(c->kase, "requirements"), "tools");
    const struct eh_json *given = eh_json_get(c->inv, "tools");
    const struct eh_json *t;
    size_t i = 0;

    c->tool_count = jcount(names);
    c->tools = calloc(c->tool_count + 1, sizeof(*c->tools));
    if (c->tools == NULL)
        return -1;
    for (t = names != NULL ? names->child : NULL; t != NULL; t = t->next, i++) {
        c->tools[i].name = t->type == 's' ? t->string : NULL;
        c->tools[i].path = c->tools[i].name != NULL ? jtext(given, c->tools[i].name) : NULL;
        if (c->tools[i].path == NULL || c->tools[i].path[0] != '/') {
            diag("the invocation gives no absolute path of tool %s",
                 c->tools[i].name != NULL ? c->tools[i].name : "(unnamed)");
            return -1;
        }
    }
    return 0;
}

/** Read the steps and the machines, and the bound of each step. */
static int load_steps(struct etude_case *c)
{
    const struct eh_json *steps = eh_json_get(c->kase, "steps");
    const struct eh_json *machines = eh_json_get(c->kase, "machines");
    const struct eh_json *bounds = eh_json_get(c->inv, "steps");
    const struct eh_json *s;
    size_t i = 0;

    c->step_count = jcount(steps);
    c->decl_count = jcount(machines);
    c->steps = calloc(c->step_count + 1, sizeof(*c->steps));
    c->decls = calloc(c->decl_count + 1, sizeof(*c->decls));
    if (c->steps == NULL || c->decls == NULL)
        return -1;
    for (s = steps != NULL ? steps->child : NULL; s != NULL; s = s->next, i++) {
        c->steps[i].id = jtext(s, "step_id");
        c->steps[i].machine = jtext(s, "machine");
        c->steps[i].bound_us = c->steps[i].id != NULL ? jnum(bounds, c->steps[i].id) : 0;
        if (c->steps[i].bound_us == 0) {
            diag("the invocation gives no bound of step %s",
                 c->steps[i].id != NULL ? c->steps[i].id : "(unnamed)");
            return -1;
        }
    }
    for (i = 0, s = machines != NULL ? machines->child : NULL; s != NULL; s = s->next, i++) {
        const char *console = jtext(s, "console");

        c->decls[i].id = jtext(s, "machine_id");
        c->decls[i].qemu = eh_json_get(s, "qemu");
        c->decls[i].console = console != NULL && strcmp(console, "serial") == 0;
        c->decls[i].qmp = jbool(s, "qmp") == 1;
        c->decls[i].helper = jbool(s, "helper") == 1;
        if (c->decls[i].id == NULL || c->decls[i].qemu == NULL) {
            diag("machine %lu of the case has no identity or profile", (unsigned long)i);
            return -1;
        }
    }
    return 0;
}

/** Read the scenario's assertions and the first witness of kind `step`. */
static int load_assertions(struct etude_case *c)
{
    const struct eh_json *list = eh_json_get(c->kase, "assertions");
    const struct eh_json *a;
    size_t i = 0;

    c->assert_count = jcount(list);
    c->asserts = calloc(c->assert_count + 1, sizeof(*c->asserts));
    if (c->asserts == NULL)
        return -1;
    for (a = list != NULL ? list->child : NULL; a != NULL; a = a->next) {
        const char *source = jtext(a, "source");

        if (source != NULL && strcmp(source, "scenario") == 0 && jtext(a, "assertion_id") != NULL)
            c->asserts[i++].id = jtext(a, "assertion_id");
    }
    c->assert_count = i;
    for (a = eh_json_get(c->kase, "progress") != NULL ? eh_json_get(c->kase, "progress")->child
                                                      : NULL;
         a != NULL && c->step_witness == NULL; a = a->next) {
        const char *kind = jtext(a, "kind");

        if (kind != NULL && strcmp(kind, "step") == 0)
            c->step_witness = jtext(a, "witness_id");
    }
    return 0;
}

/** Read the site: the accelerator, QEMU's programs and the firmware. */
static int load_site(struct etude_case *c)
{
    const struct eh_json *site = eh_json_get(c->inv, "site");
    const char *accel = jtext(site, "accel");

    if (accel == NULL || etude_accel_parse(accel, &c->site.accel) != 0) {
        diag("the invocation's site gives no accelerator the launcher knows");
        return -1;
    }
    c->site.qemu_i686 = jtext(site, "qemu_i686");
    c->site.qemu_x86_64 = jtext(site, "qemu_x86_64");
    c->site.ovmf_i686 = jtext(site, "ovmf_i686");
    c->site.ovmf_x86_64 = jtext(site, "ovmf_x86_64");
    if (c->site.qemu_i686 == NULL || c->site.qemu_x86_64 == NULL) {
        diag("the invocation's site names no QEMU");
        return -1;
    }
    return 0;
}

/** Read the scenario invocation at `path`, and give the paths of its
 * manifest and store. */
static int load_invocation(struct etude_case *c, const char *path, const char **manifest,
                           const char **store)
{
    int version;

    c->inv = read_json(path);
    if (c->inv == NULL)
        return -1;
    version = (int)jnum(c->inv, "schema_version");
    c->run_id = jtext(c->inv, "run_id");
    c->attempt_id = jtext(c->inv, "attempt_id");
    c->case_id = jtext(c->inv, "case_id");
    c->arch = jtext(c->inv, "architecture");
    c->scratch = jtext(c->inv, "scratch");
    *manifest = jtext(c->inv, "manifest");
    *store = jtext(c->inv, "store");
    c->grace_us = jnum(c->inv, "grace_us");
    c->cleanup_us = jnum(c->inv, "cleanup_us");
    c->console_max = jnum(c->inv, "console_max_bytes");
    c->output_max = jnum(c->inv, "output_max_bytes");
    c->resend_us = jnum(c->inv, "resend_us");
    if (version != ETUDE_CONTRACT_VERSION || c->run_id == NULL || c->attempt_id == NULL ||
        c->case_id == NULL || c->arch == NULL || c->scratch == NULL || *manifest == NULL ||
        *store == NULL || c->grace_us == 0 || c->cleanup_us == 0 || c->console_max == 0 ||
        c->output_max == 0 || c->resend_us == 0 ||
        (strcmp(c->arch, "i686") != 0 && strcmp(c->arch, "x86_64") != 0)) {
        diag("%s: not a scenario invocation of version %d", path, ETUDE_CONTRACT_VERSION);
        return -1;
    }
    return 0;
}

/** Read the invocation at `path` and the case its manifest gives. */
static int load(struct etude_case *c, const char *path)
{
    const char *manifest, *store, *domain;

    if (load_invocation(c, path, &manifest, &store) != 0)
        return -1;
    c->manifest = read_json(manifest);
    if (c->manifest == NULL)
        return -1;
    c->kase = find_case(c->manifest, c->case_id);
    domain = jtext(c->kase, "domain");
    if (c->kase == NULL || domain == NULL || strcmp(domain, "machine") != 0) {
        diag("%s: no machine case %s", manifest, c->case_id);
        return -1;
    }
    if (load_artifacts(c) != 0 || load_tools(c) != 0 || load_steps(c) != 0 ||
        load_assertions(c) != 0 || load_site(c) != 0)
        return -1;
    c->store = etude_store_open(store);
    if (c->store == NULL) {
        diag("%s: the store cannot be opened: %s", store, strerror(errno));
        return -1;
    }
    return 0;
}

/* ---- Records. ---- */

/** Write one report record, made of `fmt`, with one write(2). */
static void record(const char *fmt, ...)
{
    char line[ETUDE_REPORT_LINE_MAX + 1];
    size_t n = sizeof(ETUDE_REPORT_PREFIX) - 1, done = 0;
    ssize_t w;
    va_list ap;
    int k;

    memcpy(line, ETUDE_REPORT_PREFIX, n);
    va_start(ap, fmt);
    k = vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
    va_end(ap);
    if (k < 0)
        return;
    n += (size_t)k < sizeof(line) - n - 1 ? (size_t)k : sizeof(line) - n - 2;
    line[n++] = '\n';
    fflush(stdout);
    while (done < n) {
        w = write(STDOUT_FILENO, line + done, n - done);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return;
        done += (size_t)w;
    }
}

/** Copy `in` into `out` as a record's detail: printable ASCII, not empty. */
static const char *detail_text(const char *in, char *out, size_t size)
{
    size_t i;

    if (in == NULL || *in == '\0')
        in = "-";
    for (i = 0; in[i] != '\0' && i + 1 < size; i++)
        out[i] = in[i] >= ' ' && in[i] <= '~' ? in[i] : '?';
    out[i] = '\0';
    return out;
}

/** Find the declared assertion `id` of the scenario's, or NULL. */
static struct assertion *find_assertion(struct etude_case *c, const char *id)
{
    size_t i;

    for (i = 0; i < c->assert_count; i++)
        if (strcmp(c->asserts[i].id, id) == 0)
            return &c->asserts[i];
    return NULL;
}

/** Count a report of assertion `id`, `held` or not, and note misuse. */
static void count_report(struct etude_case *c, const char *id, int held)
{
    struct assertion *a = find_assertion(c, id);

    c->records++;
    if (a == NULL) {
        diag("assertion %s is not one of the scenario's", id);
        c->faults++;
        return;
    }
    if (a->reported++ > 0) {
        diag("assertion %s is reported again", id);
        c->faults++;
    }
    a->held = held;
    if (!held)
        c->faults++;
}

void etude_assert(struct etude_case *c, const char *id, int held, const char *detail)
{
    char text[ETUDE_REPORT_LINE_MAX];

    count_report(c, id, held);
    if (held)
        record("%s %s %s", ETUDE_REPORT_ASSERT, id, ETUDE_REPORT_PASS);
    else
        record("%s %s %s %s", ETUDE_REPORT_ASSERT, id, ETUDE_REPORT_FAIL,
               detail_text(detail, text, sizeof(text)));
}

void etude_notrun(struct etude_case *c, const char *id, const char *reason)
{
    char text[ETUDE_REPORT_LINE_MAX];

    count_report(c, id, 0);
    record("%s %s %s", ETUDE_REPORT_NOTRUN, id, detail_text(reason, text, sizeof(text)));
}

void etude_branch(struct etude_case *c, const char *group, const char *alternative)
{
    c->records++;
    record("%s %s %s", ETUDE_REPORT_BRANCH, group, alternative);
}

void etude_progress(struct etude_case *c, const char *witness, uint64_t units)
{
    (void)c;
    record("%s %s %llu", ETUDE_REPORT_PROGRESS, witness, (unsigned long long)units);
}

/* ---- Files of the attempt. ---- */

/** Remember `path`, which the library made, for removal after a pass. */
static void made(struct etude_case *c, const char *path, int dir)
{
    struct made *m = calloc(1, sizeof(*m));

    if (m == NULL || (m->path = malloc(strlen(path) + 1)) == NULL) {
        free(m);
        c->faults++;
        return;
    }
    strcpy(m->path, path);
    m->dir = dir;
    m->next = c->made;
    c->made = m;
}

/** Give in `buf` the path of `name` in the scratch's directory `sub`, made
 * if it is missing; -1 when it does not fit or cannot be made. */
static int scratch_path(struct etude_case *c, const char *sub, const char *name, char *buf,
                        size_t size)
{
    struct stat st;

    if ((size_t)snprintf(buf, size, "%s/%s", c->scratch, sub) >= size)
        return -1;
    if (stat(buf, &st) != 0) {
        if (mkdir(buf, 0700) != 0)
            return -1;
        made(c, buf, 1);
    }
    if ((size_t)snprintf(buf, size, "%s/%s/%s", c->scratch, sub, name) >= size)
        return -1;
    return 0;
}

/** Duplicate `s`, or NULL. */
static char *dup_text(const char *s)
{
    char *d = malloc(strlen(s) + 1);

    if (d != NULL)
        strcpy(d, s);
    return d;
}

/** Find the artifact `name` of the case's `prepare`, or NULL. */
static struct artifact *find_artifact(struct etude_case *c, const char *name)
{
    size_t i;

    for (i = 0; name != NULL && i < c->art_count; i++)
        if (strcmp(c->arts[i].name, name) == 0)
            return &c->arts[i];
    return NULL;
}

const char *etude_artifact(struct etude_case *c, const char *name)
{
    struct artifact *a = find_artifact(c, name);
    char path[4096];

    if (a == NULL) {
        diag("artifact %s is not one of the case's", name != NULL ? name : "(null)");
        c->faults++;
        return NULL;
    }
    if (!a->checked) {
        a->checked = 1;
        if (etude_store_check(c->store, a->digest, NULL) != 0) {
            diag("artifact %s, object %s: %s", a->name, a->digest,
                 errno == EBADMSG ? "its content differs from its digest" : strerror(errno));
            c->faults++;
        } else if (etude_store_path(c->store, a->digest, path, sizeof(path)) == 0) {
            a->path = dup_text(path);
        }
    }
    return a->path;
}

/** Say whether `name` is in a `disk` fixture of the case's isolation. */
static int disk_fixture(const struct etude_case *c, const char *name)
{
    const struct eh_json *f;

    for (f = eh_json_get(eh_json_get(c->kase, "execution"), "isolation") != NULL
                 ? eh_json_get(eh_json_get(c->kase, "execution"), "isolation")->child
                 : NULL;
         f != NULL; f = f->next)
        if (jtext(f, "fixture") != NULL && strcmp(jtext(f, "fixture"), "disk") == 0 &&
            jhas(eh_json_get(f, "names"), name))
            return 1;
    return 0;
}

const char *etude_private_copy(struct etude_case *c, const char *name)
{
    struct artifact *a = find_artifact(c, name);
    char path[4096];

    if (a == NULL || !disk_fixture(c, name)) {
        diag("artifact %s is not a disk of the case's isolation", name != NULL ? name : "(null)");
        c->faults++;
        return NULL;
    }
    if (a->copy != NULL)
        return a->copy;
    if (etude_artifact(c, name) == NULL)
        return NULL;
    if (scratch_path(c, "copies", name, path, sizeof(path)) != 0 ||
        etude_store_copy(c->store, a->digest, path) != 0) {
        diag("artifact %s: no private copy: %s", name, strerror(errno));
        c->faults++;
        return NULL;
    }
    made(c, path, 0);
    a->copy = dup_text(path);
    return a->copy;
}

/** Give an executable copy of artifact `name`, or NULL. */
static const char *executable(struct etude_case *c, struct artifact *a)
{
    char path[4096];

    if (a->exe != NULL)
        return a->exe;
    if (etude_artifact(c, a->name) == NULL)
        return NULL;
    if (scratch_path(c, "bin", a->name, path, sizeof(path)) != 0 ||
        etude_store_copy(c->store, a->digest, path) != 0 || chmod(path, 0555) != 0) {
        diag("artifact %s: no executable copy: %s", a->name, strerror(errno));
        c->faults++;
        return NULL;
    }
    made(c, path, 0);
    a->exe = dup_text(path);
    return a->exe;
}

const char *etude_evidence(struct etude_case *c, const char *name)
{
    if (!file_name(name) || scratch_path(c, "evidence", name, c->path, sizeof(c->path)) != 0)
        return NULL;
    return c->path;
}

/* ---- Steps and waits. ---- */

/** End the step that runs, as progress of the case's step witness. */
static void end_step(struct etude_case *c)
{
    if (c->cur < 0)
        return;
    c->cur = -1;
    c->steps_done++;
    if (c->step_witness != NULL)
        etude_progress(c, c->step_witness, c->steps_done);
}

enum etude_status etude_step(struct etude_case *c, const char *step)
{
    long i;

    if (stop_asked)
        return ETUDE_STOPPED;
    for (i = 0; i < (long)c->step_count; i++)
        if (strcmp(c->steps[i].id, step) == 0)
            break;
    if (i == (long)c->step_count || i < c->next) {
        diag("step %s is not a step of the manifest after the last one begun", step);
        c->faults++;
        return ETUDE_REFUSED;
    }
    end_step(c);
    c->cur = i;
    c->next = i + 1;
    c->deadline = posix_now_us() + c->steps[i].bound_us;
    return ETUDE_OK;
}

/** Give `m`'s launcher result. */
static const struct etude_machine_result *mres(const struct etude_case_machine *m)
{
    return etude_machine_result(m->m);
}

/** Write console input for a session of etude-guest's. */
static int serial_write(void *ctx, const char *buf, size_t n)
{
    struct etude_case_machine *m = ctx;

    return etude_machine_type(m->m, buf, n);
}

/** Forward one whole line of a guest attempt's standard output. */
static void guest_line(struct etude_case_machine *m, const char *line)
{
    struct etude_case *c = m->c;
    const char *rest = line + sizeof(ETUDE_REPORT_PREFIX) - 1;
    char id[ETUDE_REPORT_LINE_MAX], verb[16];
    int n = 0;

    if (strncmp(line, ETUDE_REPORT_PREFIX, sizeof(ETUDE_REPORT_PREFIX) - 1) != 0)
        return;
    if (sscanf(rest, "%15s %254s%n", verb, id, &n) < 2)
        verb[0] = '\0';
    if (strcmp(verb, ETUDE_REPORT_BEGIN) == 0) {
        if (strcmp(id, c->case_id) != 0) {
            diag("the guest's report begins case %s", id);
            c->faults++;
        }
        return;
    }
    if (strcmp(verb, ETUDE_REPORT_END) == 0 || strcmp(verb, ETUDE_REPORT_CHILD) == 0)
        return;
    if (strcmp(verb, ETUDE_REPORT_ASSERT) == 0)
        count_report(c, id,
                     strncmp(rest + n, " " ETUDE_REPORT_PASS, 5) == 0 && (rest[n + 5] == '\0'));
    else if (strcmp(verb, ETUDE_REPORT_NOTRUN) == 0)
        count_report(c, id, 0);
    else if (strcmp(verb, ETUDE_REPORT_BRANCH) == 0)
        c->records++;
    else if (strcmp(verb, ETUDE_REPORT_PROGRESS) != 0)
        return;
    record("%s", rest);
}

/** Take `n` bytes of a guest attempt's output on descriptor `fd`. */
static void serial_output(void *ctx, int fd, const char *buf, size_t n)
{
    struct etude_case_machine *m = ctx;
    size_t i;

    if (fd != 1)
        return;
    for (i = 0; i < n; i++) {
        if (buf[i] != '\n') {
            if (m->line_len + 1 < sizeof(m->line))
                m->line[m->line_len++] = buf[i];
            else
                m->line_long = 1;
            continue;
        }
        m->line[m->line_len] = '\0';
        if (!m->line_long)
            guest_line(m, m->line);
        m->line_len = 0;
        m->line_long = 0;
    }
}

/** Feed each session the console bytes it has not read, and say when a
 * console closed, ended or passed its bound. */
static void serve_sessions(struct etude_case *c, uint64_t now)
{
    struct etude_case_machine *m;

    for (m = c->machines; m != NULL; m = m->next) {
        const struct etude_machine_result *r;

        if (m->serial == NULL || m->serial_closed)
            continue;
        r = mres(m);
        if (m->fed < r->console.kept) {
            etude_serial_feed(m->serial, r->console.data + m->fed, r->console.kept - m->fed, now);
            m->fed = r->console.kept;
        }
        if (r->console.closed || r->state == ETUDE_MACHINE_ENDED || r->console.total > m->fed) {
            etude_serial_closed(m->serial);
            m->serial_closed = 1;
            continue;
        }
        etude_serial_tick(m->serial, now);
    }
}

/** Give the earliest resend of any session, or 0. */
static uint64_t sessions_due(const struct etude_case *c)
{
    const struct etude_case_machine *m;
    uint64_t soonest = 0, t;

    for (m = c->machines; m != NULL; m = m->next) {
        if (m->serial == NULL || m->serial_closed)
            continue;
        t = etude_serial_deadline(m->serial);
        if (t != 0 && (soonest == 0 || t < soonest))
            soonest = t;
    }
    return soonest;
}

/** Give the milliseconds from `now` to `until`, rounded up, at most a
 * minute. */
static int wait_ms(uint64_t until, uint64_t now)
{
    if (until <= now)
        return 0;
    if (until - now > 60000000u)
        return 60000;
    return (int)((until - now + 999) / 1000);
}

/* What a wait waits for: a predicate on the case's state. */
typedef int (*done_fn)(void *arg);

/**
 * Serve the machines, the sessions and the host attempt `host` until `done`
 * says so, the step's bound runs out at `until`, or, when `stoppable`, a
 * stop is asked; `host` is set to NULL once that attempt has finished.
 */
static enum etude_status serve_until(struct etude_case *c, done_fn done, void *arg,
                                     struct etude_proc **host, uint64_t until, int stoppable)
{
    struct etude_watch w;
    struct etude_proc *finished;
    uint64_t now, soonest;
    char drain[64];

    for (;;) {
        now = posix_now_us();
        serve_sessions(c, now);
        if (done(arg))
            return ETUDE_OK;
        if (stop_asked && stoppable)
            return ETUDE_STOPPED;
        if (now >= until)
            return ETUDE_BOUND;
        soonest = sessions_due(c);
        if (soonest == 0 || soonest > until)
            soonest = until;
        w.fd = stop_pipe[0];
        w.events = ETUDE_WATCH_IN;
        etude_qemu_watch(c->q, wait_ms(soonest, now), &w, 1, &finished);
        if (w.revents != 0)
            while (read(stop_pipe[0], drain, sizeof(drain)) > 0) {
            }
        if (finished != NULL && host != NULL && finished == *host)
            *host = NULL;
    }
}

/** Give the deadline of the step that runs; 0 for none. */
static uint64_t step_deadline(const struct etude_case *c)
{
    return c->cur >= 0 ? c->deadline : 0;
}

/* ---- Machines. ---- */

/** Fill `end` from what the launcher observed of a machine that ended. */
static void machine_end(const struct etude_case_machine *m, struct etude_termination *end)
{
    const struct etude_machine_result *r = mres(m);

    end->end = r->end;
    end->value = -1;
    end->console_offset = r->end_console;
    if (r->end == ETUDE_END_QEMU_EXITED && r->qemu != NULL &&
        (r->qemu->exit_end == ETUDE_END_EXITED || r->qemu->exit_end == ETUDE_END_SIGNALED))
        end->value = r->qemu->exit_value;
}

/** Say whether machine `arg` runs or has ended. */
static int machine_started(void *arg)
{
    const struct etude_machine_result *r = mres(arg);

    return r->state != ETUDE_MACHINE_STARTING;
}

/** Say whether machine `arg` has ended. */
static int machine_ended(void *arg)
{
    return mres(arg)->state == ETUDE_MACHINE_ENDED;
}

/** Find the machine `id` of the manifest, or NULL. */
static struct machine_decl *find_decl(struct etude_case *c, const char *id)
{
    size_t i;

    for (i = 0; id != NULL && i < c->decl_count; i++)
        if (strcmp(c->decls[i].id, id) == 0)
            return &c->decls[i];
    return NULL;
}

/** Copy the strings of array `v` into `out`, at most `max`, ended by NULL;
 * -1 when there are more or one is not a string. */
static int strings(const struct eh_json *v, const char **out, size_t max)
{
    const struct eh_json *i;
    size_t n = 0;

    for (i = v != NULL ? v->child : NULL; i != NULL; i = i->next) {
        if (n == max || i->type != 's')
            return -1;
        out[n++] = i->string;
    }
    out[n] = NULL;
    return 0;
}

/** Make the launcher's profile of boot `m` with the disks `disks`, names of
 * the profile's own disks, or all of them for NULL. */
static enum etude_status make_profile(struct etude_case_machine *m, const char *const disks[])
{
    struct etude_case *c = m->c;
    const struct eh_json *qp = m->decl->qemu;
    const char *names[DISKS_MAX + 1], *firmware = jtext(qp, "firmware");
    struct etude_qemu_profile *p = &m->profile;
    size_t i;

    if (strings(eh_json_get(qp, "disks"), names, DISKS_MAX) != 0 ||
        strings(eh_json_get(qp, "devices"), m->devices, DEVICES_MAX) != 0 ||
        strings(eh_json_get(qp, "boot_options"), m->options, OPTIONS_MAX) != 0) {
        diag("machine %s: a profile of more disks, devices or options than the launcher takes",
             m->decl->id);
        return ETUDE_REFUSED;
    }
    for (i = 0; (disks != NULL ? disks[i] : names[i]) != NULL; i++) {
        const char *name = disks != NULL ? disks[i] : names[i];

        if (i == DISKS_MAX || !jhas(eh_json_get(qp, "disks"), name)) {
            diag("machine %s: disk %s is not one of its profile's", m->decl->id, name);
            return ETUDE_REFUSED;
        }
        m->disks[i] = etude_private_copy(c, name);
        if (m->disks[i] == NULL)
            return ETUDE_REFUSED;
    }
    m->disks[i] = NULL;
    p->architecture = c->arch;
    p->kernel = jtext(qp, "kernel") != NULL ? etude_artifact(c, jtext(qp, "kernel")) : NULL;
    p->initrd = jtext(qp, "initrd") != NULL ? etude_artifact(c, jtext(qp, "initrd")) : NULL;
    p->disks = m->disks;
    p->cpu = jtext(qp, "cpu");
    p->processors = (unsigned)jnum(qp, "processors");
    p->memory_bytes = jnum(qp, "memory_bytes");
    p->devices = m->devices;
    p->boot_options = m->options;
    if (firmware != NULL && strcmp(firmware, "seabios") == 0)
        p->firmware = ETUDE_FIRMWARE_SEABIOS;
    else if (firmware != NULL && strcmp(firmware, "ovmf") == 0)
        p->firmware = ETUDE_FIRMWARE_OVMF;
    else
        p->firmware = ETUDE_FIRMWARE_DIRECT;
    if (p->kernel == NULL || (jtext(qp, "initrd") != NULL && p->initrd == NULL)) {
        diag("machine %s: its kernel or RAM image is not an artifact that checks", m->decl->id);
        return ETUDE_REFUSED;
    }
    return ETUDE_OK;
}

/** Say whether the session of machine `arg` has finished its operation. */
static int session_done(void *arg)
{
    const struct etude_case_machine *m = arg;

    return etude_serial_status(m->serial) != ETUDE_SERIAL_BUSY;
}

/** Give the status of a session's operation that did not complete. */
static enum etude_status session_status(const struct etude_case_machine *m)
{
    switch (etude_serial_status(m->serial)) {
    case ETUDE_SERIAL_DONE:
        return ETUDE_OK;
    case ETUDE_SERIAL_LOST:
        return ETUDE_LOST;
    default:
        diag("machine %s: etude-guest: %s", m->decl->id, etude_serial_why(m->serial));
        return ETUDE_FAILED;
    }
}

/** Search `n` bytes at `hay` for `text`; its end's offset, or -1. */
static long find_text(const char *hay, size_t n, const char *text)
{
    size_t len = strlen(text), i;

    for (i = 0; len > 0 && i + len <= n; i++)
        if (hay[i] == text[0] && memcmp(hay + i, text, len) == 0)
            return (long)(i + len);
    return -1;
}

/* A wait for text on a console. */
struct text_wait {
    struct etude_case_machine *m;
    const char *text;
    uint64_t from;
    long at;  /* the offset past the text once found, else -1 */
    int gone; /* the console can show no more */
};

/** Say whether the text has appeared, or can no longer appear. */
static int text_found(void *arg)
{
    struct text_wait *t = arg;
    const struct etude_machine_result *r = mres(t->m);
    long at;

    if (t->from < r->console.kept) {
        at = find_text(r->console.data + t->from, r->console.kept - (size_t)t->from, t->text);
        if (at >= 0) {
            t->at = (long)t->from + at;
            return 1;
        }
    }
    t->gone =
        r->state == ETUDE_MACHINE_ENDED || r->console.closed || r->console.total > r->console.kept;
    return t->gone;
}

/** Start etude-guest at the debug shell of a helper's machine and open its
 * session. */
static enum etude_status start_guest(struct etude_case_machine *m)
{
    struct etude_case *c = m->c;
    struct etude_serial_conf sc;
    struct text_wait t;
    enum etude_status st;
    char command[96];

    memset(&t, 0, sizeof(t));
    t.m = m;
    t.text = SHELL_READY;
    t.at = -1;
    st = serve_until(c, text_found, &t, NULL, step_deadline(c), 1);
    if (st != ETUDE_OK)
        return st;
    if (t.at < 0)
        return ETUDE_LOST;
    snprintf(command, sizeof(command), GUEST_EXEC "%llu\n",
             (unsigned long long)((c->resend_us + 999) / 1000));
    if (etude_machine_type(m->m, command, strlen(command)) != 0)
        return ETUDE_LOST;
    memset(&sc, 0, sizeof(sc));
    sc.run_id = m->attempt_id;
    sc.resend_us = c->resend_us;
    sc.output_max = (size_t)c->output_max;
    sc.write = serial_write;
    sc.output = serial_output;
    sc.ctx = m;
    m->fed = (size_t)t.at;
    m->serial = etude_serial_new(&sc);
    if (m->serial == NULL || etude_serial_open(m->serial, posix_now_us()) != 0)
        return ETUDE_FAILED;
    st = serve_until(c, session_done, m, NULL, step_deadline(c), 1);
    return st != ETUDE_OK ? st : session_status(m);
}

enum etude_status etude_boot(struct etude_case *c, const char *machine, const char *const disks[],
                             struct etude_case_machine **mp, struct etude_termination *end)
{
    struct machine_decl *d = find_decl(c, machine);
    struct etude_case_machine *m;
    enum etude_status st;
    char name[200];
    const char *console;

    *mp = NULL;
    end->end = ETUDE_END_LOST;
    end->value = -1;
    end->console_offset = 0;
    if (stop_asked)
        return ETUDE_STOPPED;
    if (d == NULL || c->cur < 0 || c->steps[c->cur].machine == NULL ||
        strcmp(c->steps[c->cur].machine, machine) != 0) {
        diag("machine %s is not the machine of the step that runs", machine);
        c->faults++;
        return ETUDE_REFUSED;
    }
    if (d->now != NULL && mres(d->now)->state != ETUDE_MACHINE_ENDED) {
        diag("machine %s runs already", machine);
        c->faults++;
        return ETUDE_REFUSED;
    }
    m = calloc(1, sizeof(*m));
    if (m == NULL)
        return ETUDE_FAILED;
    m->c = c;
    m->decl = d;
    st = make_profile(m, disks);
    if (st != ETUDE_OK) {
        free(m);
        c->faults++;
        return st;
    }
    d->boots++;
    snprintf(m->attempt_id, sizeof(m->attempt_id), "%s.%s.%u", c->attempt_id, d->id, d->boots);
    snprintf(name, sizeof(name), "%s.%u.console", d->id, d->boots);
    console = etude_evidence(c, name);
    m->spec.attempt_id = m->attempt_id;
    m->spec.profile = &m->profile;
    m->spec.site = &c->site;
    m->spec.envp = environ;
    m->spec.console_path = console;
    m->spec.console_max = (size_t)c->console_max;
    m->spec.grace_us = c->grace_us;
    if (console == NULL || etude_qemu_start(c->q, &m->spec, &m->m) != 0) {
        diag("machine %s: QEMU did not start: %s", machine, strerror(errno));
        free(m);
        c->faults++;
        end->end = ETUDE_END_QEMU_EXITED;
        return ETUDE_FAILED;
    }
    made(c, console, 0);
    m->next = c->machines;
    c->machines = m;
    d->now = m;
    st = serve_until(c, machine_started, m, NULL, step_deadline(c), 1);
    if (st == ETUDE_OK && mres(m)->state == ETUDE_MACHINE_ENDED) {
        machine_end(m, end);
        return end->end == ETUDE_END_LOST ? ETUDE_LOST : ETUDE_FAILED;
    }
    if (st == ETUDE_OK && d->helper)
        st = start_guest(m);
    if (st != ETUDE_OK) {
        etude_machine_cancel(m->m);
        if (mres(m)->state == ETUDE_MACHINE_ENDED)
            machine_end(m, end);
        else
            end->end = ETUDE_END_STOPPED;
        return st;
    }
    *mp = m;
    return ETUDE_OK;
}

enum etude_status etude_console_wait(struct etude_case_machine *m, const char *text, uint64_t from,
                                     uint64_t *at)
{
    struct text_wait t;
    enum etude_status st;

    if (!m->decl->console || m->c->cur < 0 || text == NULL || *text == '\0') {
        diag("machine %s: no console to wait on, or no step", m->decl->id);
        m->c->faults++;
        return ETUDE_REFUSED;
    }
    memset(&t, 0, sizeof(t));
    t.m = m;
    t.text = text;
    t.from = from;
    t.at = -1;
    st = serve_until(m->c, text_found, &t, NULL, step_deadline(m->c), 1);
    if (st != ETUDE_OK)
        return st;
    if (t.at >= 0) {
        *at = (uint64_t)t.at;
        return ETUDE_OK;
    }
    if (mres(m)->console.total > mres(m)->console.kept) {
        diag("machine %s: the console passed the %llu bytes kept", m->decl->id,
             (unsigned long long)m->c->console_max);
        return ETUDE_FAILED;
    }
    return ETUDE_LOST;
}

enum etude_status etude_type(struct etude_case_machine *m, const char *text)
{
    if (!m->decl->console || m->serial != NULL) {
        diag("machine %s: its console takes no typing", m->decl->id);
        m->c->faults++;
        return ETUDE_REFUSED;
    }
    if (stop_asked)
        return ETUDE_STOPPED;
    if (mres(m)->state == ETUDE_MACHINE_ENDED || mres(m)->console.closed ||
        etude_machine_type(m->m, text, strlen(text)) != 0)
        return ETUDE_LOST;
    return ETUDE_OK;
}

/* A wait for a QMP reply. */
struct reply_wait {
    struct etude_case_machine *m;
    long id;
    int state; /* etude_machine_reply's */
    const char *reply;
};

/** Say whether the reply has come, or never will. */
static int reply_come(void *arg)
{
    struct reply_wait *w = arg;

    w->state = etude_machine_reply(w->m->m, w->id, &w->reply);
    return w->state != 0;
}

enum etude_status etude_qmp(struct etude_case_machine *m, const char *execute,
                            const char *arguments, char **reply)
{
    struct reply_wait w;
    struct eh_json *j;
    enum etude_status st;

    *reply = NULL;
    if (!m->decl->qmp || m->c->cur < 0) {
        diag("machine %s: no QMP, or no step", m->decl->id);
        m->c->faults++;
        return ETUDE_REFUSED;
    }
    if (stop_asked)
        return ETUDE_STOPPED;
    memset(&w, 0, sizeof(w));
    w.m = m;
    w.id = etude_machine_qmp(m->m, execute, arguments);
    if (w.id < 0) {
        if (errno == EINVAL) {
            m->c->faults++;
            return ETUDE_REFUSED;
        }
        return errno == EAGAIN && mres(m)->state != ETUDE_MACHINE_ENDED ? ETUDE_FAILED : ETUDE_LOST;
    }
    st = serve_until(m->c, reply_come, &w, NULL, step_deadline(m->c), 1);
    if (st != ETUDE_OK)
        return st;
    if (w.state < 0)
        return ETUDE_LOST;
    *reply = dup_text(w.reply);
    if (*reply == NULL)
        return ETUDE_FAILED;
    j = eh_json_parse(w.reply);
    st = eh_json_get(j, "return") != NULL ? ETUDE_OK : ETUDE_FAILED;
    eh_json_free(j);
    return st;
}

enum etude_status etude_wait_end(struct etude_case_machine *m, struct etude_termination *end)
{
    enum etude_status st;

    end->end = ETUDE_END_LOST;
    end->value = -1;
    end->console_offset = 0;
    if (m->c->cur < 0) {
        m->c->faults++;
        return ETUDE_REFUSED;
    }
    st = serve_until(m->c, machine_ended, m, NULL, step_deadline(m->c), 1);
    if (st != ETUDE_OK)
        return st;
    machine_end(m, end);
    return end->end == ETUDE_END_LOST ? ETUDE_LOST : ETUDE_OK;
}

/* ---- etude-guest's attempts. ---- */

/** Give the SHA-256 of `n` bytes at `p` in hex. */
static void digest_of(const void *p, size_t n, char hex[ETUDE_DIGEST_HEX + 1])
{
    struct sha256 h;
    unsigned char d[32];

    sha256_init(&h);
    sha256_update(&h, p, n);
    sha256_final(&h, d);
    sha256_hex(d, hex);
}

/** Run one operation of a session to its end within the step's bound. */
static enum etude_status session_op(struct etude_case_machine *m, int begun)
{
    enum etude_status st;

    if (begun != 0)
        return ETUDE_FAILED;
    st = serve_until(m->c, session_done, m, NULL, step_deadline(m->c), 1);
    return st != ETUDE_OK ? st : session_status(m);
}

/** Run a put to its end, and lose the session when the step's bound or a stop
 * ends the wait first, since the controller reads the caller's bytes until the
 * put ends. */
static enum etude_status put_op(struct etude_case_machine *m, int begun)
{
    enum etude_status st = session_op(m, begun);

    if (st == ETUDE_BOUND || st == ETUDE_STOPPED) {
        etude_serial_closed(m->serial);
        m->serial_closed = 1;
    }
    return st;
}

/** Store file `f` of an invocation in etude-guest's store: from its stage
 * directory when it has it, else with the bytes of the store's object. */
static enum etude_status put_file(struct etude_case_machine *m, const struct eh_json *f)
{
    struct etude_case *c = m->c;
    const char *name = jtext(f, "name"), *sha = jtext(f, "sha256"), *mode = jtext(f, "mode");
    uint64_t size =
        (uint64_t)(eh_json_get(f, "size_bytes") != NULL ? eh_json_get(f, "size_bytes")->number
                                                        : -1);
    char path[4096];
    char *bytes;
    size_t len;
    enum etude_status st;

    if (name == NULL || sha == NULL || mode == NULL)
        return ETUDE_REFUSED;
    st = put_op(m, etude_serial_put(m->serial, name, size, sha, mode, NULL, posix_now_us()));
    if (st != ETUDE_FAILED || etude_serial_status(m->serial) != ETUDE_SERIAL_REFUSED)
        return st;
    if (etude_store_check(c->store, sha, NULL) != 0 ||
        etude_store_path(c->store, sha, path, sizeof(path)) != 0 ||
        (bytes = read_whole(path, &len)) == NULL) {
        diag("file %s of the guest invocation is neither staged nor in the store", name);
        return ETUDE_FAILED;
    }
    st = put_op(m, etude_serial_put(m->serial, name, size, sha, mode, bytes, posix_now_us()));
    free(bytes);
    return st;
}

/** End a session's run that the step's bound or a stop interrupted, within
 * the grace period. */
static void cancel_run(struct etude_case_machine *m)
{
    if (etude_serial_status(m->serial) != ETUDE_SERIAL_BUSY ||
        etude_serial_cancel(m->serial, posix_now_us()) != 0)
        return;
    serve_until(m->c, session_done, m, NULL, posix_now_us() + m->c->grace_us, 0);
}

enum etude_status etude_guest(struct etude_case_machine *m, const char *name,
                              struct etude_termination *end)
{
    struct etude_case *c = m->c;
    const char *path = etude_artifact(c, name), *role, *token, *kase;
    const struct etude_serial_result *r;
    const struct eh_json *f;
    struct eh_json *inv = NULL;
    enum etude_status st;
    char sha[ETUDE_DIGEST_HEX + 1];
    char *text = NULL;
    size_t len;

    end->end = ETUDE_END_LOST;
    end->value = -1;
    end->console_offset = 0;
    if (m->serial == NULL || c->cur < 0 || path == NULL ||
        (text = read_whole(path, &len)) == NULL || (inv = eh_json_parse(text)) == NULL) {
        diag("machine %s: no session of etude-guest, no step or no invocation %s", m->decl->id,
             name != NULL ? name : "(null)");
        free(text);
        c->faults++;
        return ETUDE_REFUSED;
    }
    role = jtext(inv, "role");
    token = jtext(inv, "attempt_token");
    kase = jtext(inv, "case_id");
    if (role == NULL || strcmp(role, "ordinary") != 0 || token == NULL || kase == NULL ||
        strcmp(kase, c->case_id) != 0) {
        diag("invocation %s: not an ordinary attempt of case %s", name, c->case_id);
        st = ETUDE_REFUSED;
        c->faults++;
        goto done;
    }
    for (f = eh_json_get(inv, "files") != NULL ? eh_json_get(inv, "files")->child : NULL; f != NULL;
         f = f->next)
        if ((st = put_file(m, f)) != ETUDE_OK)
            goto done;
    digest_of(text, len, sha);
    st = put_op(
        m, etude_serial_put(m->serial, "invocation.json", len, sha, "0644", text, posix_now_us()));
    if (st != ETUDE_OK)
        goto done;
    m->line_len = 0;
    m->line_long = 0;
    st = session_op(m, etude_serial_run(m->serial, token, sha, posix_now_us()));
    if (st == ETUDE_BOUND || st == ETUDE_STOPPED)
        cancel_run(m);
    r = etude_serial_result(m->serial);
    end->end = r->end;
    end->value = r->value;
    end->console_offset = mres(m)->console.total;
    if (st == ETUDE_OK && !r->complete)
        st = ETUDE_FAILED;
done:
    if (st != ETUDE_OK)
        c->faults++;
    eh_json_free(inv);
    free(text);
    return st;
}

/* ---- Host tools. ---- */

/** Write the `n` bytes of `data` to the evidence file `name`. */
static void keep_evidence(struct etude_case *c, const char *name, const char *data, size_t n)
{
    const char *path = etude_evidence(c, name);
    int fd;

    if (path == NULL)
        return;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0 || (n > 0 && write(fd, data, n) != (ssize_t)n)) {
        diag("evidence %s: %s", name, strerror(errno));
        c->faults++;
    } else {
        made(c, path, 0);
    }
    if (fd >= 0)
        close(fd);
}

/** Say whether host attempt `arg`, a pointer to its handle, has finished. */
static int host_done(void *arg)
{
    return *(struct etude_proc **)arg == NULL;
}

enum etude_status etude_host(struct etude_case *c, char *const argv[], const char *output,
                             struct etude_termination *end)
{
    struct etude_exec_spec spec;
    struct etude_proc *p, *running;
    const struct etude_exec_result *r;
    const struct artifact *a = NULL;
    const char *program = NULL;
    enum etude_status st;
    char attempt[300], err_name[160];
    char **args;
    size_t i, n;

    end->end = ETUDE_END_LOST;
    end->value = -1;
    end->console_offset = 0;
    if (stop_asked)
        return ETUDE_STOPPED;
    for (i = 0; argv[0] != NULL && i < c->tool_count; i++)
        if (strcmp(c->tools[i].name, argv[0]) == 0)
            program = c->tools[i].path;
    if (program == NULL && (a = find_artifact(c, argv[0])) != NULL)
        program = executable(c, (struct artifact *)a);
    if (c->cur < 0 || program == NULL || !file_name(output) ||
        (size_t)snprintf(err_name, sizeof(err_name), "%s.err", output) >= sizeof(err_name)) {
        diag("%s is no tool or artifact of the case, or no step runs",
             argv[0] != NULL ? argv[0] : "(null)");
        c->faults++;
        return ETUDE_REFUSED;
    }
    for (n = 0; argv[n] != NULL; n++) {
    }
    args = calloc(n + 1, sizeof(*args));
    if (args == NULL)
        return ETUDE_FAILED;
    args[0] = (char *)program;
    for (i = 1; i < n; i++)
        args[i] = argv[i];
    snprintf(attempt, sizeof(attempt), "%s.host.%s", c->attempt_id, output);
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = attempt;
    spec.argv = args;
    spec.envp = environ;
    spec.cwd = c->scratch;
    spec.output_max = (size_t)c->output_max;
    spec.grace_us = c->grace_us;
    if (etude_exec_start(etude_qemu_exec(c->q), &spec, &p) != 0) {
        diag("%s did not start: %s", argv[0], strerror(errno));
        free(args);
        c->faults++;
        return ETUDE_FAILED;
    }
    free(args);
    running = p;
    st = serve_until(c, host_done, &running, &running, step_deadline(c), 1);
    if (st != ETUDE_OK) {
        etude_exec_cancel(p);
        serve_until(c, host_done, &running, &running, UINT64_MAX, 0);
    }
    r = etude_proc_result(p);
    keep_evidence(c, output, r->out.data, r->out.kept);
    keep_evidence(c, err_name, r->err.data, r->err.kept);
    end->end = r->end;
    end->value = r->value;
    if (r->cleanup != ETUDE_CLEANUP_CONFIRMED)
        c->unconfirmed = 1;
    etude_proc_free(p);
    if (st == ETUDE_OK && end->end == ETUDE_END_LOST)
        st = ETUDE_LOST;
    return st;
}

/* ---- The attempt. ---- */

/** Say whether every machine has ended. */
static int all_ended(void *arg)
{
    const struct etude_case *c = arg;
    const struct etude_case_machine *m;

    for (m = c->machines; m != NULL; m = m->next)
        if (mres(m)->state != ETUDE_MACHINE_ENDED)
            return 0;
    return 1;
}

/** End every machine within the cleanup's bound, and note how each was
 * cleaned up. */
static void end_machines(struct etude_case *c)
{
    struct etude_case_machine *m;

    for (m = c->machines; m != NULL; m = m->next)
        etude_machine_cancel(m->m);
    serve_until(c, all_ended, c, NULL, posix_now_us() + c->cleanup_us, 0);
    for (m = c->machines; m != NULL; m = m->next) {
        const struct etude_machine_result *r = mres(m);

        if (r->state != ETUDE_MACHINE_ENDED || r->qemu == NULL ||
            r->qemu->cleanup != ETUDE_CLEANUP_CONFIRMED)
            c->unconfirmed = 1;
    }
}

/** Say whether the attempt may pass: every assertion of the scenario's was
 * reported once and held, and nothing was misused or lost. */
static int passed(const struct etude_case *c, int rc)
{
    size_t i;

    if (rc != 0 || c->faults > 0 || c->unconfirmed)
        return 0;
    for (i = 0; i < c->assert_count; i++)
        if (c->asserts[i].reported != 1 || !c->asserts[i].held)
            return 0;
    return 1;
}

/** Remove what the library made, the newest first. */
static void remove_made(struct etude_case *c)
{
    struct made *m;

    for (m = c->made; m != NULL; m = m->next)
        if ((m->dir ? rmdir(m->path) : unlink(m->path)) != 0 && errno != ENOENT)
            diag("%s: not removed: %s", m->path, strerror(errno));
}

/** Free the case and what it holds. */
static void case_free(struct etude_case *c)
{
    struct etude_case_machine *m;
    struct made *f;
    size_t i;

    while ((m = c->machines) != NULL) {
        c->machines = m->next;
        if (m->serial != NULL)
            etude_serial_free(m->serial);
        free(m);
    }
    while ((f = c->made) != NULL) {
        c->made = f->next;
        free(f->path);
        free(f);
    }
    for (i = 0; i < c->art_count; i++) {
        free(c->arts[i].path);
        free(c->arts[i].copy);
        free(c->arts[i].exe);
    }
    free(c->arts);
    free(c->tools);
    free(c->steps);
    free(c->decls);
    free(c->asserts);
    if (c->store != NULL)
        etude_store_close(c->store);
    eh_json_free(c->manifest);
    eh_json_free(c->inv);
}

int etude_case_main(int argc, char **argv)
{
    struct etude_case c;
    int rc, stopped;

    if (argc != 2) {
        fprintf(stderr, "usage: %s INVOCATION\n", argc > 0 ? argv[0] : "etude-case");
        return 2;
    }
    memset(&c, 0, sizeof(c));
    c.cur = -1;
    if (load(&c, argv[1]) != 0 || catch_stops() != 0 || (c.q = etude_qemu_new()) == NULL) {
        case_free(&c);
        return 2;
    }
    record("%s %s", ETUDE_REPORT_BEGIN, c.case_id);
    rc = etude_scenario(&c) != 0;
    stopped = stop_asked;
    if (rc == 0 && !stopped) {
        end_step(&c);
        record("%s %u", ETUDE_REPORT_END, c.records);
    }
    end_machines(&c);
    if (passed(&c, rc || stopped))
        remove_made(&c);
    else
        diag("kept: %s", c.scratch);
    etude_qemu_free(c.q);
    rc = rc || stopped || c.unconfirmed;
    case_free(&c);
    return rc;
}
