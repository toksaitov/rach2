/*
 * scenario.c: libetude-case on POSIX systems, the library a machine scenario
 * links with. It reads the attempt's invocation and its case, reports the
 * scenario's assertions, boots the case's machines on the QEMU launcher with
 * private copies, empty disks and overlays as their disks, runs host tools,
 * services and etude-guest's attempts beside them, bounds every wait by its
 * step, and ends what it owns after collecting its diagnostics.
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

#ifdef _WIN32
#include "../win/files.h"
#include "../win/winexec.h"
#endif

#include "../history_json.h"
#include "../guest/wire.h"
#include "../serial.h"
#include "../volume/volume.h"
#include "etude/artifact.h"
#include "etude/qemu.h"
#include "etude/report.h"
#include "etude/scenario.h"
#include "posix.h"
#include "store.h"
#include "sha256.h"

#ifndef _WIN32
extern char **environ;
#endif

/* The text the kernel's debug shell prints when ready for a command. */
#define SHELL_READY "Type 'help' for available commands."

/* The most disks, devices, boot options, forwards of each kind and words of
 * a relay a profile gives the launcher, and the most strings of its devices'
 * properties: each name, each value, and the end of each device's list. */
#define DISKS_MAX      ETUDE_QEMU_DISKS
#define DEVICES_MAX    32
#define OPTIONS_MAX    32
#define FORWARDS_MAX   16
#define RELAY_MAX      8
#define PROPERTIES_MAX 64

/* The longest invocation, manifest or guest invocation the library reads. */
#define DOCUMENT_MAX (4u * 1024 * 1024)

/* The QMP commands whose replies are a machine's diagnostics. */
#define CAPTURES 3

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

/* A service of the manifest. */
struct service_decl {
    const char *id;
    const char *program;            /* a tool's or an artifact's name */
    const char *ready;              /* the text its standard output has once it serves */
    unsigned starts;                /* the starts begun so far */
    struct etude_case_service *now; /* its latest start, or NULL */
};

/* A suspension of the case's workload. */
struct suspension_decl {
    const char *id;
    const char *machine;
    const char *witness; /* a counter witness: the stops seen so far */
    uint64_t cycles, run_us, stop_us;
    uint64_t stops; /* the stops seen so far, over every call */
};

/* A declared assertion of the scenario's, and what it was given. */
struct assertion {
    const char *id;
    int reported; /* reports so far */
    int held;     /* the last report was a pass */
};

/* A file the library made, removed after a pass unless it is kept for the
 * executor, as the directory of the consoles is. */
struct made {
    struct made *next;
    char *path;
    int dir;
    int kept;
};

/* An empty disk or an overlay of the attempt, made at its first use. */
struct blank {
    struct blank *next;
    const char *name; /* the manifest's */
    char *path;
};

/* A private file bound to one declared empty disk slot. */
struct disk_binding {
    struct disk_binding *next;
    struct machine_decl *machine;
    char *name, *path;
};

/* A disk of a machine's profile, as the manifest declares it. */
struct disk_decl {
    const char *name;
    enum etude_disk_bus bus;
    int cdrom, discard, boot, unsafe;
    const char *serial; /* NULL for none */
    uint64_t size;      /* an empty disk's size in bytes, or 0 for an artifact's */
    const char *id;     /* the identity a device's `drive` names, on a device */
    const char *base;   /* an overlay's base artifact, or NULL */
    uint64_t cluster;   /* an overlay's cluster size in bytes, or 0 */
};

struct etude_case_machine {
    struct etude_case *c;
    struct etude_case_machine *next;
    struct machine_decl *decl;
    struct etude_machine *m;
    char attempt_id[300];
    struct etude_qemu_disk disks[DISKS_MAX + 1];
    struct etude_qemu_device devices[DEVICES_MAX + 1];
    const char *properties[PROPERTIES_MAX];
    const char *options[OPTIONS_MAX + 1];
    struct etude_qemu_forward forwards[FORWARDS_MAX];
    struct etude_qemu_guest_forward guest_forwards[FORWARDS_MAX];
    const char *relays[FORWARDS_MAX][RELAY_MAX + 1];
    const char *dump_cards[DEVICES_MAX + 1];          /* the profile's captures */
    struct etude_qemu_capture dumps[DEVICES_MAX + 1]; /* and their files */
    struct etude_qemu_profile profile;
    struct etude_machine_spec spec;
    unsigned boot;               /* the machine's boot, counted from 1 */
    struct etude_serial *serial; /* etude-guest's session, for a helper */
    uint64_t commands;
    int command_output; /* installed commands do not emit case reports */
    char guest_service_output[241];
    size_t fed; /* console bytes fed to the session */
    int serial_closed;
    char line[ETUDE_REPORT_LINE_MAX + 1]; /* the guest's output line being read */
    size_t line_len;
    int line_long;
    long capture[CAPTURES]; /* the ids of the diagnostics' QMP commands, or -1 */
    int ended;              /* a machine record has given this boot's end */
};

struct etude_case_service {
    struct etude_case *c;
    struct etude_case_service *next;
    struct service_decl *decl;
    struct etude_proc *p;
    unsigned n;   /* its start, counted from 1 */
    int finished; /* the executor has ended it and its descendants */
    int seen;     /* the scenario has seen its end */
    int kept;     /* its output is kept as evidence */
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
    struct service_decl *sdecls;
    size_t sdecl_count;
    struct suspension_decl *holds;
    size_t hold_count;
    unsigned *ports; /* the ports the plan reserved */
    size_t port_count;
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
    int unseen;          /* a service ended without the scenario seeing it */
    struct etude_qemu *q;
    struct etude_case_machine *machines;
    struct etude_case_service *services;
    struct made *made;
    struct blank *blanks;
    struct disk_binding *bindings;
    char path[4096]; /* the path etude_evidence gave last */
};

/* The descriptors of the pipe that the stop signals write. */
#ifndef _WIN32
static int stop_pipe[2] = {-1, -1};
#endif
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
    (void)sig;
    stop_asked = 1;
#ifndef _WIN32
    {
        char b = (char)sig;
        if (stop_pipe[1] >= 0 && write(stop_pipe[1], &b, 1) < 0) {
        }
    }
#endif
    errno = e;
}

/** Make the stop pipe and catch SIGTERM, SIGINT and SIGHUP. */
static int catch_stops(void)
{
#ifdef _WIN32
    return signal(SIGTERM, on_stop) == SIG_ERR || signal(SIGINT, on_stop) == SIG_ERR ? -1 : 0;
#else
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
#endif
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

/**
 * Read `v`, a disk of a profile's `disks`, into `d`: an artifact's name, a
 * private copy on IDE, or an object that gives the name, the bus, the
 * medium, the serial, `discard`, `flush`, `boot`, an empty disk's size, an
 * overlay's base and cluster size, and the identity of a disk that a device
 * attaches.
 *
 * @return 0, or -1 for a disk of another form, of a bus or medium that the
 *         library does not attach, or with a base and no cluster size or a
 *         cluster size and no base.
 */
static int disk_decl(const struct eh_json *v, struct disk_decl *d)
{
    const char *bus, *media;

    memset(d, 0, sizeof(*d));
    if (v != NULL && v->type == 's') {
        d->name = v->string;
        return 0;
    }
    if (v == NULL || v->type != 'o' || (d->name = jtext(v, "name")) == NULL)
        return -1;
    bus = jtext(v, "bus");
    media = jtext(v, "media");
    if (bus != NULL && strcmp(bus, "nvme") == 0)
        d->bus = ETUDE_BUS_NVME;
    else if (bus != NULL && strcmp(bus, "ahci") == 0)
        d->bus = ETUDE_BUS_AHCI;
    else if (bus != NULL && strcmp(bus, "device") == 0)
        d->bus = ETUDE_BUS_DEVICE;
    else if (bus != NULL && strcmp(bus, "ide") != 0)
        return -1;
    if (media != NULL && strcmp(media, "cdrom") == 0)
        d->cdrom = 1;
    else if (media != NULL && strcmp(media, "disk") != 0)
        return -1;
    d->serial = jtext(v, "serial");
    d->discard = jbool(v, "discard") == 1;
    d->unsafe = jbool(v, "flush") == 0;
    d->boot = jbool(v, "boot") == 1;
    d->size = jnum(v, "size_bytes");
    d->id = jtext(v, "id");
    d->base = jtext(v, "base");
    d->cluster = jnum(v, "cluster_bytes");
    return (d->base == NULL) == (d->cluster == 0) && d->cluster <= UINT32_MAX ? 0 : -1;
}

/** Find the disk `name` of the profile `qp` and read it into `d`; -1 when
 * the profile has no such disk. */
static int find_disk(const struct eh_json *qp, const char *name, struct disk_decl *d)
{
    const struct eh_json *list = eh_json_get(qp, "disks"), *v;

    if (list == NULL || list->type != 'a')
        return -1;
    for (v = list->child; v != NULL; v = v->next)
        if (disk_decl(v, d) == 0 && name != NULL && strcmp(d->name, name) == 0)
            return 0;
    return -1;
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
        if (c->tools[i].path == NULL ||
#ifdef _WIN32
            !etude_win_absolute(c->tools[i].path)
#else
            c->tools[i].path[0] != '/'
#endif
        ) {
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
        const char *console = jtext(s, "console"), *arch;

        c->decls[i].id = jtext(s, "machine_id");
        c->decls[i].qemu = eh_json_get(s, "qemu");
        c->decls[i].console = console != NULL && strcmp(console, "serial") == 0;
        c->decls[i].qmp = jbool(s, "qmp") == 1;
        c->decls[i].helper = jbool(s, "helper") == 1;
        if (c->decls[i].id == NULL || c->decls[i].qemu == NULL) {
            diag("machine %lu of the case has no identity or profile", (unsigned long)i);
            return -1;
        }
        arch = jtext(c->decls[i].qemu, "architecture");
        if (eh_json_get(c->decls[i].qemu, "architecture") != NULL &&
            (!c->decls[i].helper || arch == NULL ||
             (strcmp(arch, "i686") != 0 && strcmp(arch, "x86_64") != 0))) {
            diag("machine %s: only a helper may select architecture i686 or x86_64",
                 c->decls[i].id);
            return -1;
        }
    }
    return 0;
}

/** Say whether `name` is a tool of the case's requirements. */
static int is_tool(const struct etude_case *c, const char *name)
{
    size_t i;

    for (i = 0; name != NULL && i < c->tool_count; i++)
        if (strcmp(c->tools[i].name, name) == 0)
            return 1;
    return 0;
}

/** Say whether `name` is an artifact of the case's `prepare`. */
static int is_artifact(const struct etude_case *c, const char *name)
{
    size_t i;

    for (i = 0; name != NULL && i < c->art_count; i++)
        if (strcmp(c->arts[i].name, name) == 0)
            return 1;
    return 0;
}

/** Read the services, each of whose programs is a tool or an artifact. */
static int load_services(struct etude_case *c)
{
    const struct eh_json *list = eh_json_get(c->kase, "services");
    const struct eh_json *s;
    size_t i = 0;

    c->sdecl_count = jcount(list);
    c->sdecls = calloc(c->sdecl_count + 1, sizeof(*c->sdecls));
    if (c->sdecls == NULL)
        return -1;
    for (s = list != NULL ? list->child : NULL; s != NULL; s = s->next, i++) {
        c->sdecls[i].id = jtext(s, "service_id");
        c->sdecls[i].program = jtext(s, "program");
        c->sdecls[i].ready = jtext(s, "ready");
        if (!file_name(c->sdecls[i].id) || c->sdecls[i].ready == NULL ||
            *c->sdecls[i].ready == '\0' ||
            (!is_tool(c, c->sdecls[i].program) && !is_artifact(c, c->sdecls[i].program))) {
            diag("service %lu of the case has no identity, no ready text, or a program that is "
                 "neither a tool nor an artifact of the case's",
                 (unsigned long)i);
            return -1;
        }
    }
    return 0;
}

static struct machine_decl *find_decl(struct etude_case *c, const char *id);

/** Read the suspensions of the workload, each of a machine of the case's
 * with a witness, at least one cycle and both intervals. */
static int load_suspensions(struct etude_case *c)
{
    const struct eh_json *list = eh_json_get(eh_json_get(c->kase, "workload"), "suspensions");
    const struct eh_json *s;
    size_t i = 0;

    c->hold_count = jcount(list);
    c->holds = calloc(c->hold_count + 1, sizeof(*c->holds));
    if (c->holds == NULL)
        return -1;
    for (s = list != NULL ? list->child : NULL; s != NULL; s = s->next, i++) {
        struct suspension_decl *h = &c->holds[i];

        h->id = jtext(s, "suspension_id");
        h->machine = jtext(s, "machine");
        h->witness = jtext(s, "witness");
        h->cycles = jnum(s, "cycles");
        h->run_us = jnum(s, "run_us");
        h->stop_us = jnum(s, "stop_us");
        if (h->id == NULL || find_decl(c, h->machine) == NULL || h->witness == NULL ||
            h->cycles == 0 || h->run_us == 0 || h->stop_us == 0) {
            diag("suspension %lu of the workload has no identity, machine of the case, witness, "
                 "cycles or intervals",
                 (unsigned long)i);
            return -1;
        }
    }
    return 0;
}

/** Read the ports the plan reserved: as many as the case declares, each
 * a port above 1023 that is given once. */
static int load_ports(struct etude_case *c)
{
    const struct eh_json *list = eh_json_get(c->inv, "ports");
    const struct eh_json *p;
    size_t i = 0, j;

    c->port_count = jcount(list);
    c->ports = calloc(c->port_count + 1, sizeof(*c->ports));
    if (c->ports == NULL)
        return -1;
    if (list == NULL || list->type != 'a' ||
        c->port_count != jnum(eh_json_get(c->kase, "resources"), "ports")) {
        diag("the invocation gives not the %llu ports the case declares",
             (unsigned long long)jnum(eh_json_get(c->kase, "resources"), "ports"));
        return -1;
    }
    for (p = list->child; p != NULL; p = p->next, i++) {
        c->ports[i] =
            p->type == 'n' && p->number > 1023 && p->number < 65536 ? (unsigned)p->number : 0;
        for (j = 0; j < i; j++)
            if (c->ports[j] == c->ports[i])
                c->ports[i] = 0;
        if (c->ports[i] == 0) {
            diag("port %lu of the invocation is not a port above 1023 given once",
                 (unsigned long)i);
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
    c->site.boundary_lazy = jbool(site, "boundary_lazy") == 1;
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
        (strcmp(c->arch, "i686") != 0 && strcmp(c->arch, "x86_64") != 0 &&
         strcmp(c->arch, "none") != 0)) {
        diag("%s: not a scenario invocation of version %d", path, ETUDE_CONTRACT_VERSION);
        return -1;
    }
    return 0;
}

/** Match the invocation's identity to the case's declared architectures. */
static int case_architecture(const struct etude_case *c)
{
    const struct eh_json *arches = eh_json_get(c->kase, "architectures"), *arch;

    if (arches != NULL && arches->type == 's')
        return !strcmp(arches->string, "none") && !strcmp(c->arch, "none");
    if (arches == NULL || arches->type != 'a')
        return 0;
    for (arch = arches->child; arch != NULL; arch = arch->next)
        if (arch->type == 's' && !strcmp(arch->string, c->arch))
            return 1;
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
    if (!case_architecture(c)) {
        diag("%s: case %s does not declare architecture %s", manifest, c->case_id, c->arch);
        return -1;
    }
    if (load_artifacts(c) != 0 || load_tools(c) != 0 || load_steps(c) != 0 ||
        load_services(c) != 0 || load_suspensions(c) != 0 || load_ports(c) != 0 ||
        load_assertions(c) != 0 || load_site(c) != 0)
        return -1;
    c->store = etude_store_open(store);
    if (c->store == NULL) {
        diag("%s: the store cannot be opened: %s", store, strerror(errno));
        return -1;
    }
    if (jtext(c->inv, "digests") != NULL &&
        etude_store_digests(c->store, jtext(c->inv, "digests")) != 0) {
        diag("%s: the digests file cannot be used: %s", store, strerror(errno));
        return -1;
    }
    return 0;
}

/* ---- Records. ---- */

/** Write one report record, made of `fmt`, with one write(2): cut to the
 * line's limit, with no space before its newline. */
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
    while (n > sizeof(ETUDE_REPORT_PREFIX) - 1 && line[n - 1] == ' ')
        n--;
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

/** Copy `in` into `out` as a record's detail: printable ASCII, not empty,
 * and neither beginning nor ending with a space, which a report refuses. */
static const char *detail_text(const char *in, char *out, size_t size)
{
    size_t i;

    if (in != NULL)
        in += strspn(in, " ");
    for (i = 0; in != NULL && in[i] != '\0' && i + 1 < size; i++)
        out[i] = in[i] >= ' ' && in[i] <= '~' ? in[i] : '?';
    while (i > 0 && out[i - 1] == ' ')
        i--;
    if (i == 0)
        out[i++] = '-';
    out[i] = '\0';
    return out;
}

const char *etude_case_id(const struct etude_case *c)
{
    return c->case_id;
}

const char *etude_case_architecture(const struct etude_case *c)
{
    return c->arch;
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

/** Keep the scratch's directory `sub`, which the library made, after a pass,
 * for the executor to read and remove what it holds. */
static void keep_made(struct etude_case *c, const char *sub)
{
    char path[4096];
    struct made *m;

    snprintf(path, sizeof(path), "%s/%s", c->scratch, sub);
    for (m = c->made; m != NULL; m = m->next)
        if (m->dir && strcmp(m->path, path) == 0)
            m->kept = 1;
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

/** Give the size of the empty disk `name` that a machine's profile declares,
 * or 0 when none does. */
static uint64_t empty_size(const struct etude_case *c, const char *name)
{
    struct disk_decl d;
    size_t i;

    for (i = 0; i < c->decl_count; i++)
        if (find_disk(c->decls[i].qemu, name, &d) == 0 && d.size != 0)
            return d.size;
    return 0;
}

/**
 * Give the path of the empty disk `name` of `size` bytes, a sparse file in
 * `copies/` of the scratch, made at its first use and given again after.
 */
static const char *empty_disk(struct etude_case *c, const char *name, uint64_t size)
{
    struct blank *b;
    char path[4096];
    int fd;

    for (b = c->blanks; b != NULL; b = b->next)
        if (strcmp(b->name, name) == 0)
            return b->path;
    if (find_artifact(c, name) != NULL || !file_name(name)) {
        diag("disk %s: an empty disk is no artifact, and its name is a file's", name);
        c->faults++;
        return NULL;
    }
    if (scratch_path(c, "copies", name, path, sizeof(path)) != 0 ||
        (fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0) {
        diag("disk %s: no empty disk: %s", name, strerror(errno));
        c->faults++;
        return NULL;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        diag("disk %s: no empty disk of %llu bytes: %s", name, (unsigned long long)size,
             strerror(errno));
        close(fd);
        unlink(path);
        c->faults++;
        return NULL;
    }
    close(fd);
    made(c, path, 0);
    b = calloc(1, sizeof(*b));
    if (b == NULL || (b->path = dup_text(path)) == NULL) {
        free(b);
        c->faults++;
        return NULL;
    }
    b->name = name;
    b->next = c->blanks;
    c->blanks = b;
    return b->path;
}

/** Find the overlay `name` that a machine's profile declares, into `d`; -1
 * when none does. */
static int find_overlay(const struct etude_case *c, const char *name, struct disk_decl *d)
{
    size_t i;

    for (i = 0; i < c->decl_count; i++)
        if (find_disk(c->decls[i].qemu, name, d) == 0 && d->base != NULL)
            return 0;
    return -1;
}

/**
 * Give the path of overlay `d`, an empty qcow2 file in `copies/` of the
 * scratch whose backing file is its base's object, made at its first use and
 * given again after.
 */
static const char *overlay_disk(struct etude_case *c, const struct disk_decl *d)
{
    struct blank *b;
    char path[4096];

    for (b = c->blanks; b != NULL; b = b->next)
        if (strcmp(b->name, d->name) == 0)
            return b->path;
    if (find_artifact(c, d->name) != NULL || !file_name(d->name)) {
        diag("disk %s: an overlay is no artifact, and its name is a file's", d->name);
        c->faults++;
        return NULL;
    }
    if (etude_artifact(c, d->base) == NULL)
        return NULL;
    if (scratch_path(c, "copies", d->name, path, sizeof(path)) != 0 ||
        etude_store_overlay(c->store, find_artifact(c, d->base)->digest, (uint32_t)d->cluster,
                            path) != 0) {
        diag("disk %s: no overlay over %s: %s", d->name, d->base, strerror(errno));
        c->faults++;
        return NULL;
    }
    made(c, path, 0);
    b = calloc(1, sizeof(*b));
    if (b == NULL || (b->path = dup_text(path)) == NULL) {
        free(b);
        c->faults++;
        return NULL;
    }
    b->name = d->name;
    b->next = c->blanks;
    c->blanks = b;
    return b->path;
}

const char *etude_private_copy(struct etude_case *c, const char *name)
{
    struct artifact *a = find_artifact(c, name);
    uint64_t size = name != NULL ? empty_size(c, name) : 0;
    struct disk_decl d;
    char path[4096];

    if (name != NULL && find_overlay(c, name, &d) == 0)
        return overlay_disk(c, &d);
    if (size != 0)
        return empty_disk(c, name, size);
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
    if (scratch_path(c, "bin", a->name, path, sizeof(path)) != 0)
        goto failed;
#ifdef _WIN32
    if (strlen(path) > sizeof(path) - sizeof(".exe")) {
        errno = ENAMETOOLONG;
        goto failed;
    }
    strcat(path, ".exe");
#endif
    if (etude_store_copy(c->store, a->digest, path) != 0 || chmod(path, 0555) != 0)
        goto failed;
    made(c, path, 0);
    a->exe = dup_text(path);
    return a->exe;
failed:
    diag("artifact %s: no executable copy: %s", a->name, strerror(errno));
    c->faults++;
    return NULL;
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

    if (fd != 1 || m->command_output)
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

/** Note that host attempt `p` has finished: the foreground one `*host`, which
 * is set to NULL, or a service's. */
static void host_finished(struct etude_case *c, struct etude_proc *p, struct etude_proc **host)
{
    struct etude_case_service *s;

    if (host != NULL && p == *host) {
        *host = NULL;
        return;
    }
    for (s = c->services; s != NULL; s = s->next)
        if (s->p == p)
            s->finished = 1;
}

/**
 * Serve the machines, the sessions, the services and the host attempt
 * `host` until `done` says so, the step's bound runs out at `until`, or, when
 * `stoppable`, a stop is asked; `host` is set to NULL once that attempt has
 * finished.
 */
static enum etude_status serve_until(struct etude_case *c, done_fn done, void *arg,
                                     struct etude_proc **host, uint64_t until, int stoppable)
{
#ifndef _WIN32
    struct etude_watch w;
#endif
    struct etude_proc *finished;
    uint64_t now, soonest;
#ifndef _WIN32
    char drain[64];
#endif

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
#ifndef _WIN32
        w.fd = stop_pipe[0];
        w.events = ETUDE_WATCH_IN;
#endif
#ifdef _WIN32
        etude_qemu_watch(c->q, wait_ms(soonest, now) < 10 ? wait_ms(soonest, now) : 10, NULL, 0,
                         &finished);
#else
        etude_qemu_watch(c->q, wait_ms(soonest, now), &w, 1, &finished);
#endif
#ifndef _WIN32
        if (w.revents != 0)
            while (read(stop_pipe[0], drain, sizeof(drain)) > 0) {
            }
#endif
        if (finished != NULL)
            host_finished(c, finished, host);
    }
}

/** Give the deadline of the step that runs; 0 for none. */
static uint64_t step_deadline(const struct etude_case *c)
{
    return c->cur >= 0 ? c->deadline : 0;
}

/* ---- Machines. ---- */

/** Give the name a machine record gives the end `end` of a machine, or
 * NULL for an end of a process. */
static const char *end_name(enum etude_end end)
{
    switch (end) {
    case ETUDE_END_PANIC:
        return "panic";
    case ETUDE_END_HALTED:
        return "halted";
    case ETUDE_END_RESET:
        return "reset";
    case ETUDE_END_SHUTDOWN:
        return "shutdown";
    case ETUDE_END_QEMU_EXITED:
        return "qemu_exited";
    case ETUDE_END_ACCEL_FAILED:
        return "accel_failed";
    case ETUDE_END_STOPPED:
        return "stopped";
    case ETUDE_END_LOST:
        return "lost";
    default:
        return NULL;
    }
}

/** Write the machine record of boot `m`'s end `end`, the first one alone. */
static void record_end(struct etude_case_machine *m, enum etude_end end)
{
    const char *name = end_name(end);

    if (m->ended || name == NULL)
        return;
    m->ended = 1;
    record("%s %s.%u %s", ETUDE_REPORT_MACHINE, m->decl->id, m->boot, name);
}

/** Fill `end` from what the launcher observed of a machine that ended, and
 * record it as the boot's end. */
static void machine_end(struct etude_case_machine *m, struct etude_termination *end)
{
    const struct etude_machine_result *r = mres(m);

    end->end = r->end;
    end->value = -1;
    end->console_offset = r->end_console;
    if (r->end == ETUDE_END_QEMU_EXITED && r->qemu != NULL &&
        (r->qemu->exit_end == ETUDE_END_EXITED || r->qemu->exit_end == ETUDE_END_SIGNALED))
        end->value = r->qemu->exit_value;
    record_end(m, end->end);
}

/** Say whether the machine ended and every owned process is confirmed gone. */
static int machine_released(const struct etude_case_machine *m)
{
    const struct etude_machine_result *r = mres(m);

    return r->state == ETUDE_MACHINE_ENDED && r->qemu != NULL &&
           r->qemu->cleanup == ETUDE_CLEANUP_CONFIRMED;
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

/** Find the private file bound to a slot, or NULL. */
static const char *bound_disk(const struct etude_case *c, const struct machine_decl *machine,
                              const char *name)
{
    const struct disk_binding *b;

    for (b = c->bindings; b != NULL; b = b->next)
        if (b->machine == machine && strcmp(b->name, name) == 0)
            return b->path;
    return NULL;
}

/** Resolve a private regular file and reject links through its descendants. */
static int private_disk_path(struct etude_case *c, const char *path, char file[4096])
{
    struct stat st;
    char root[4096];
    size_t n, i;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || realpath(c->scratch, root) == NULL ||
        realpath(path, file) == NULL)
        return 0;
    n = strlen(root);
    if (strncmp(root, file, n) != 0 || file[n] != '/' || lstat(file, &st) != 0 ||
        !S_ISREG(st.st_mode) || st.st_nlink != 1)
        return 0;
    /* Windows realpath normalizes names without resolving reparse points.
     * Check every descendant as well as the file on both platforms. */
    for (i = n + 1; file[i] != '\0'; i++) {
        if (file[i] == '/') {
            file[i] = '\0';
            if (lstat(file, &st) != 0 || !S_ISDIR(st.st_mode))
                return 0;
            file[i] = '/';
        }
    }
    return 1;
}

static int disk_active(struct etude_case *c, const char *file)
{
    struct etude_case_machine *m;
    size_t i;
    for (m = c->machines; m != NULL; m = m->next) {
        if (machine_released(m))
            continue;
        for (i = 0; m->disks[i].path != NULL; i++)
            if (strcmp(m->disks[i].path, file) == 0)
                return 1;
    }
    return 0;
}

enum etude_status etude_bind_disk(struct etude_case *c, const char *machine, const char *slot,
                                  const char *path)
{
    struct machine_decl *d = find_decl(c, machine);
    struct disk_decl disk;
    struct disk_binding *b;
    char file[4096];

    if (d == NULL || slot == NULL || path == NULL || find_disk(d->qemu, slot, &disk) != 0 ||
        disk.size == 0 || disk.cdrom || disk.base != NULL ||
        (d->now != NULL && !machine_released(d->now)) || !private_disk_path(c, path, file) ||
        disk_active(c, file))
        return ETUDE_REFUSED;
    for (b = c->bindings; b != NULL; b = b->next)
        if (b->machine == d && strcmp(b->name, slot) == 0)
            break;
    if (b == NULL) {
        b = calloc(1, sizeof(*b));
        if (b == NULL || (b->name = dup_text(slot)) == NULL) {
            free(b);
            return ETUDE_FAILED;
        }
        b->machine = d;
        b->next = c->bindings;
        c->bindings = b;
    }
    free(b->path);
    b->path = dup_text(file);
    return b->path != NULL ? ETUDE_OK : ETUDE_FAILED;
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

/**
 * Read device names or device objects into a list ended by a NULL model,
 * and the properties of each into `props`, of `props_max` strings, unless it
 * is NULL. Return 0, or -1 for a malformed entry, more than `max` devices or
 * more properties than `props` takes.
 */
static int devices_of(const struct eh_json *v, struct etude_qemu_device *out, size_t max,
                      const char **props, size_t props_max)
{
    const struct eh_json *i, *p;
    size_t n = 0, k = 0;

    for (i = v != NULL ? v->child : NULL; i != NULL; i = i->next) {
        if (n == max || (i->type != 's' && i->type != 'o'))
            return -1;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].model = i->type == 's' ? i->string : jtext(i, "model");
        if (out[n].model == NULL)
            return -1;
        out[n].id = jtext(i, "id");
        out[n].bus = jtext(i, "bus");
        out[n].port = jtext(i, "port");
        out[n].drive = jtext(i, "drive");
        p = i->type == 'o' ? eh_json_get(i, "properties") : NULL;
        if (p != NULL && props != NULL) {
            out[n].properties = props + k;
            for (p = p->child; p != NULL; p = p->next) {
                if (k + 3 > props_max || p->type != 's')
                    return -1;
                props[k++] = p->key;
                props[k++] = p->string;
            }
            props[k++] = NULL;
        }
        out[n].net = jtext(i, "net");
        out[n].host = jtext(i, "host");
        out[n].dhcpstart = jtext(i, "dhcpstart");
        out[n].dns = jtext(i, "dns");
        out[n].mac = jtext(i, "mac");
        out[n].hub = i->type == 'o' && jbool(i, "hub") == 1;
        n++;
    }
    memset(&out[n], 0, sizeof(out[n]));
    return 0;
}

/** Give the firmware a profile's `firmware` names: direct when it names
 * neither SeaBIOS nor OVMF. */
static enum etude_firmware firmware_of(const char *firmware)
{
    if (firmware != NULL && strcmp(firmware, "seabios") == 0)
        return ETUDE_FIRMWARE_SEABIOS;
    if (firmware != NULL && strcmp(firmware, "ovmf") == 0)
        return ETUDE_FIRMWARE_OVMF;
    return ETUDE_FIRMWARE_DIRECT;
}

/** Resolve a helper's explicit architecture, or the case architecture. */
static const char *machine_architecture(const struct etude_case *c, const struct machine_decl *d)
{
    const char *arch = d->helper ? jtext(d->qemu, "architecture") : NULL;
    return arch != NULL ? arch : c->arch;
}

/**
 * Give the memory a boot of declaration `d` reserves on the case's
 * architecture: its resident RAM and QEMU's overhead, as etude_qemu_memory
 * resolves its profile. A profile the launcher refuses counts as an ordinary
 * machine of its configured RAM; the launcher refuses its boot.
 */
static uint64_t decl_need(const struct etude_case *c, const struct machine_decl *d)
{
    const struct eh_json *qp = d->qemu;
    const char *options[OPTIONS_MAX + 1];
    struct etude_qemu_device devices[DEVICES_MAX + 1];
    struct etude_qemu_disk disks[DISKS_MAX + 1];
    struct etude_qemu_profile p;
    struct etude_qemu_memory mem;
    size_t i, n = jcount(eh_json_get(qp, "disks"));

    memset(&p, 0, sizeof(p));
    memset(disks, 0, sizeof(disks));
    for (i = 0; i < n && i < DISKS_MAX; i++)
        disks[i].path = "disk";
    p.architecture = machine_architecture(c, d);
    p.initrd = jtext(qp, "initrd");
    p.cpu = jtext(qp, "cpu");
    p.processors = (unsigned)jnum(qp, "processors");
    p.memory_bytes = jnum(qp, "memory_bytes");
    p.memory_profile = jtext(qp, "memory_profile");
    p.audio = jtext(qp, "audio");
    p.firmware = firmware_of(jtext(qp, "firmware"));
    p.no_i8042 = jbool(qp, "i8042") == 0;
    p.q35 = jtext(qp, "machine") != NULL && strcmp(jtext(qp, "machine"), "q35") == 0;
    if (n <= DISKS_MAX &&
        devices_of(eh_json_get(qp, "devices"), devices, DEVICES_MAX, NULL, 0) == 0 &&
        strings(eh_json_get(qp, "boot_options"), options, OPTIONS_MAX) == 0) {
        p.disks = disks;
        p.devices = devices;
        p.boot_options = options;
        if (etude_qemu_memory(&p, &mem) == 0)
            return mem.resident_bytes + mem.overhead_bytes;
    }
    return p.memory_bytes > UINT64_MAX - ETUDE_QEMU_OVERHEAD ? UINT64_MAX
                                                             : p.memory_bytes + ETUDE_QEMU_OVERHEAD;
}

/**
 * Check that the case's `resources.memory_bytes` covers every machine it
 * declares, whether it runs now or not, and give in `*share` what that leaves
 * for a boot of `d`: the reservation less what the other machines reserve.
 * The launcher requires the share to cover a boundary machine's reservation.
 *
 * @return ETUDE_OK, or ETUDE_REFUSED when the machines together need more
 *         than the case reserves.
 */
static enum etude_status memory_share(const struct etude_case *c, const struct machine_decl *d,
                                      uint64_t *share)
{
    uint64_t reserved = jnum(eh_json_get(c->kase, "resources"), "memory_bytes");
    uint64_t own = decl_need(c, d), others = 0, need;
    size_t i;

    for (i = 0; i < c->decl_count; i++) {
        if (&c->decls[i] == d)
            continue;
        need = decl_need(c, &c->decls[i]);
        others = others > UINT64_MAX - need ? UINT64_MAX : others + need;
    }
    if (own > reserved || others > reserved - own) {
        diag("machine %s: the case reserves %llu bytes of memory, less than its %lu machines need",
             d->id, (unsigned long long)reserved, (unsigned long)c->decl_count);
        return ETUDE_REFUSED;
    }
    *share = reserved - others;
    return ETUDE_OK;
}

/** Give integer member `key` of `o` when it is one of 0 to 65535, or -1. */
static long jport(const struct eh_json *o, const char *key)
{
    const struct eh_json *v = eh_json_get(o, key);

    if (v == NULL || v->type != 'n' || v->number < 0 || v->number > 65535)
        return -1;
    return (long)v->number;
}

static const char *program_path(struct etude_case *c, const char *name);

/**
 * Give card `dev` of boot `m` the guest forwards of its manifest's card `d`,
 * from the `*k`th of the boot's guest forwards on: each with its address,
 * the guest's port, the case's local port and the relay, whose program is a
 * tool or an executable copy of an artifact of the case's.
 */
static enum etude_status make_guest_forwards(struct etude_case_machine *m,
                                             struct etude_qemu_device *dev, const struct eh_json *d,
                                             size_t *k)
{
    struct etude_qemu_guest_forward *gf;
    const struct eh_json *g, *w;
    size_t i;

    dev->guest_forwards = m->guest_forwards + *k;
    g = d->type == 'o' ? eh_json_get(d, "guest_forwards") : NULL;
    for (g = g != NULL ? g->child : NULL; g != NULL; g = g->next, (*k)++) {
        if (*k == FORWARDS_MAX || jtext(g, "address") == NULL || jport(g, "guest_port") < 0 ||
            jport(g, "port") < 0)
            goto unread;
        gf = &m->guest_forwards[*k];
        gf->address = jtext(g, "address");
        gf->guest_port = (unsigned)jport(g, "guest_port");
        gf->port = (unsigned)jport(g, "port");
        w = eh_json_get(g, "relay");
        for (i = 0, w = w != NULL ? w->child : NULL; w != NULL; w = w->next, i++) {
            if (i == RELAY_MAX || w->type != 's')
                goto unread;
            m->relays[*k][i] = i == 0 ? program_path(m->c, w->string) : w->string;
            if (m->relays[*k][i] == NULL) {
                diag("machine %s: the relay %s is no tool or artifact of the case's", m->decl->id,
                     w->string);
                return ETUDE_REFUSED;
            }
        }
        m->relays[*k][i] = NULL;
        gf->relay = m->relays[*k];
        dev->guest_forward_count++;
    }
    return ETUDE_OK;
unread:
    diag("machine %s: more guest forwards or words of a relay than the launcher takes, or a "
         "guest forward it cannot read",
         m->decl->id);
    return ETUDE_REFUSED;
}

/**
 * Give the cards of boot `m` the user networks of the devices of its
 * manifest's profile, `list`, which devices_of has read into `m->devices`:
 * whether each is restricted, each forward with its protocol, the case's
 * local port it names, counted from 0, and the guest's port, and each guest
 * forward. The launcher refuses a port the case does not hold.
 */
static enum etude_status make_networks(struct etude_case_machine *m, const struct eh_json *list)
{
    const struct eh_json *d, *f;
    const char *protocol;
    size_t i = 0, k = 0, g = 0;

    for (d = list != NULL ? list->child : NULL; d != NULL; d = d->next, i++) {
        m->devices[i].restricted = d->type == 'o' && jbool(d, "restrict") == 1;
        m->devices[i].forwards = m->forwards + k;
        f = d->type == 'o' ? eh_json_get(d, "forwards") : NULL;
        for (f = f != NULL ? f->child : NULL; f != NULL; f = f->next, k++) {
            protocol = jtext(f, "protocol");
            if (k == FORWARDS_MAX || protocol == NULL ||
                (strcmp(protocol, "tcp") != 0 && strcmp(protocol, "udp") != 0) ||
                jport(f, "port") < 0 || jport(f, "guest_port") < 0)
                goto refused;
            m->forwards[k].udp = strcmp(protocol, "udp") == 0;
            m->forwards[k].port = (unsigned)jport(f, "port");
            m->forwards[k].guest_port = (unsigned)jport(f, "guest_port");
            m->devices[i].forward_count++;
        }
        if (make_guest_forwards(m, &m->devices[i], d, &g) != ETUDE_OK)
            return ETUDE_REFUSED;
    }
    m->profile.ports = m->c->ports;
    m->profile.port_count = m->c->port_count;
    return ETUDE_OK;
refused:
    diag("machine %s: a profile of more forwards than the launcher takes, or a forward it "
         "cannot read",
         m->decl->id);
    return ETUDE_REFUSED;
}

/**
 * Make the launcher's profile of boot `m` with the disks `disks`, names of
 * the profile's own disks, or all of them for NULL. A CD's medium is the
 * artifact's object in the store; any other disk is the attempt's own file.
 */
static enum etude_status make_profile(struct etude_case_machine *m, const char *const disks[])
{
    struct etude_case *c = m->c;
    const struct eh_json *qp = m->decl->qemu, *v = eh_json_get(qp, "disks");
    const char *name, *firmware = jtext(qp, "firmware");
    struct etude_qemu_profile *p = &m->profile;
    struct disk_decl d;
    size_t i;

    if (devices_of(eh_json_get(qp, "devices"), m->devices, DEVICES_MAX, m->properties,
                   PROPERTIES_MAX) != 0 ||
        strings(eh_json_get(qp, "boot_options"), m->options, OPTIONS_MAX) != 0) {
        diag("machine %s: a profile of more devices or options than the launcher takes",
             m->decl->id);
        return ETUDE_REFUSED;
    }
    if (make_networks(m, eh_json_get(qp, "devices")) != ETUDE_OK)
        return ETUDE_REFUSED;
    v = v != NULL && v->type == 'a' ? v->child : NULL;
    for (i = 0; disks != NULL ? disks[i] != NULL : v != NULL; i++) {
        if (disks != NULL)
            name = disks[i];
        else
            name = disk_decl(v, &d) == 0 ? d.name : "(unread)";
        if (disks == NULL)
            v = v->next;
        if (i == DISKS_MAX || find_disk(qp, name, &d) != 0) {
            diag("machine %s: disk %s is not one of its profile's, or past the %d it may have",
                 m->decl->id, name, DISKS_MAX);
            return ETUDE_REFUSED;
        }
        m->disks[i].path = bound_disk(c, m->decl, name);
        if (m->disks[i].path == NULL)
            m->disks[i].path = d.cdrom ? etude_artifact(c, name) : etude_private_copy(c, name);
        if (m->disks[i].path == NULL || (!d.cdrom && disk_active(c, m->disks[i].path)))
            return ETUDE_REFUSED;
        m->disks[i].bus = d.bus;
        m->disks[i].cdrom = d.cdrom;
        m->disks[i].serial = d.serial;
        m->disks[i].discard = d.discard;
        m->disks[i].id = d.id;
        m->disks[i].boot = d.boot;
        m->disks[i].qcow2 = d.base != NULL;
        m->disks[i].unsafe = d.unsafe;
    }
    m->disks[i].path = NULL;
    p->architecture = machine_architecture(c, m->decl);
    p->kernel = jtext(qp, "kernel") != NULL ? etude_artifact(c, jtext(qp, "kernel")) : NULL;
    p->initrd = jtext(qp, "initrd") != NULL ? etude_artifact(c, jtext(qp, "initrd")) : NULL;
    p->disks = m->disks;
    p->cpu = jtext(qp, "cpu");
    p->processors = (unsigned)jnum(qp, "processors");
    p->memory_bytes = jnum(qp, "memory_bytes");
    p->memory_profile = jtext(qp, "memory_profile");
    p->audio = jtext(qp, "audio");
    p->devices = m->devices;
    p->boot_options = m->options;
    p->smbios_vendor = jtext(qp, "smbios_vendor");
    p->firmware = firmware_of(firmware);
    p->no_i8042 = jbool(qp, "i8042") == 0;
    p->q35 = jtext(qp, "machine") != NULL && strcmp(jtext(qp, "machine"), "q35") == 0;
    if (p->kernel == NULL || (jtext(qp, "initrd") != NULL && p->initrd == NULL)) {
        diag("machine %s: its kernel or RAM image is not an artifact that checks", m->decl->id);
        return ETUDE_REFUSED;
    }
    return ETUDE_OK;
}

/**
 * Give boot `m` the captures of its profile, each the evidence
 * `MACHINE.N.netK.pcap`, which is removed after a pass. The launcher refuses
 * a card the profile lacks.
 */
static enum etude_status make_dumps(struct etude_case_machine *m)
{
    const char **cards = m->dump_cards, *path;
    char name[200], *end;
    unsigned long card;
    size_t i;

    if (strings(eh_json_get(m->decl->qemu, "captures"), cards, DEVICES_MAX) != 0) {
        diag("machine %s: a profile of more captures than the launcher takes", m->decl->id);
        return ETUDE_REFUSED;
    }
    for (i = 0; cards[i] != NULL; i++) {
        if (strncmp(cards[i], "net", 3) != 0 || cards[i][3] < '0' || cards[i][3] > '9' ||
            (card = strtoul(cards[i] + 3, &end, 10)) > DEVICES_MAX || *end != '\0') {
            diag("machine %s: capture %s names no card's network", m->decl->id, cards[i]);
            return ETUDE_REFUSED;
        }
        snprintf(name, sizeof(name), "%s.%u.%s.pcap", m->decl->id, m->boot, cards[i]);
        path = etude_evidence(m->c, name);
        if (path == NULL || (m->dumps[i].path = dup_text(path)) == NULL)
            return ETUDE_FAILED;
        m->dumps[i].card = (unsigned)card;
        made(m->c, path, 0);
    }
    m->dumps[i].path = NULL;
    m->profile.captures = m->dumps;
    return ETUDE_OK;
}

/** Free the paths of boot `m`'s captures. */
static void free_dumps(struct etude_case_machine *m)
{
    size_t i;

    for (i = 0; m->dumps[i].path != NULL; i++)
        free((char *)m->dumps[i].path);
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

/** Open the framed session at the console offset the caller selected. */
static enum etude_status open_guest(struct etude_case_machine *m)
{
    struct etude_case *c = m->c;
    struct etude_serial_conf sc;
    enum etude_status st;

    memset(&sc, 0, sizeof(sc));
    sc.run_id = m->attempt_id;
    sc.resend_us = c->resend_us;
    sc.output_max = (size_t)c->output_max;
    sc.write = serial_write;
    sc.output = serial_output;
    sc.ctx = m;

    m->serial = etude_serial_new(&sc);
    if (m->serial == NULL || etude_serial_open(m->serial, posix_now_us()) != 0)
        return ETUDE_FAILED;
    st = serve_until(c, session_done, m, NULL, step_deadline(c), 1);
    return st != ETUDE_OK ? st : session_status(m);
}

/** Only the small image helper keeps transfer objects outside its RAM root. */
static int guest_image_state(const struct etude_case_machine *m)
{
    return m->decl->helper && strcmp(m->decl->id, "image-helper") == 0;
}

enum etude_status etude_guest_open(struct etude_case_machine *m)
{
    char command[96];

    if (m->serial != NULL)
        return m->serial_closed ? ETUDE_LOST : ETUDE_OK;
    if (m->c->cur < 0 || !m->decl->console)
        return ETUDE_REFUSED;
    m->fed = mres(m)->console.kept;
    if (etude_serial_guest_line(command, sizeof(command), 0, guest_image_state(m), m->c->resend_us))
        return ETUDE_REFUSED;
    if (etude_machine_type(m->m, command, strlen(command)) != 0)
        return ETUDE_LOST;
    return open_guest(m);
}

/** Start etude-guest at the debug shell of a helper's machine and open its
 * session. */
static enum etude_status start_guest(struct etude_case_machine *m)
{
    struct etude_case *c = m->c;
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
    if (etude_serial_guest_line(command, sizeof(command), 1, guest_image_state(m), c->resend_us))
        return ETUDE_REFUSED;
    if (etude_machine_type(m->m, command, strlen(command)) != 0)
        return ETUDE_LOST;
    m->fed = (size_t)t.at;
    return open_guest(m);
}

int etude_has_machine(const struct etude_case *c, const char *name)
{
    size_t i;

    for (i = 0; name != NULL && i < c->decl_count; i++)
        if (strcmp(c->decls[i].id, name) == 0)
            return 1;
    return 0;
}

enum etude_status etude_boot(struct etude_case *c, const char *machine, const char *const disks[],
                             struct etude_case_machine **mp, struct etude_termination *end)
{
    struct machine_decl *d = find_decl(c, machine);
    struct etude_case_machine *m;
    enum etude_status st;
    char name[200], audio[4096];
    const char *console;
    uint64_t share;

    *mp = NULL;
    end->end = ETUDE_END_LOST;
    end->value = -1;
    end->console_offset = 0;
    if (stop_asked)
        return ETUDE_STOPPED;
    if (d == NULL || c->cur < 0 ||
        (!d->helper &&
         (c->steps[c->cur].machine == NULL || strcmp(c->steps[c->cur].machine, machine) != 0))) {
        diag("machine %s is not the machine of the step that runs", machine);
        c->faults++;
        return ETUDE_REFUSED;
    }
    if (d->now != NULL && !machine_released(d->now)) {
        diag("machine %s has not confirmed its cleanup", machine);
        c->faults++;
        return ETUDE_REFUSED;
    }
    if (memory_share(c, d, &share) != ETUDE_OK) {
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
    m->boot = ++d->boots;
    st = make_dumps(m);
    if (st != ETUDE_OK) {
        free_dumps(m);
        free(m);
        c->faults++;
        return st;
    }
    snprintf(m->attempt_id, sizeof(m->attempt_id), "%s.%s.%u", c->attempt_id, d->id, d->boots);
    if (m->profile.audio != NULL) {
        snprintf(name, sizeof(name), "%s.%u.wav", d->id, d->boots);
        if (scratch_path(c, "evidence", name, audio, sizeof(audio)) != 0) {
            free_dumps(m);
            free(m);
            c->faults++;
            return ETUDE_FAILED;
        }
        m->spec.audio_path = audio;
    }
    snprintf(name, sizeof(name), "%s.%u.console", d->id, d->boots);
    console = etude_evidence(c, name);
    m->spec.attempt_id = m->attempt_id;
    m->spec.profile = &m->profile;
    m->spec.site = &c->site;
    m->spec.envp = environ;
    m->spec.console_path = console;
    m->spec.console_max = (size_t)c->console_max;
    m->spec.grace_us = c->grace_us;
    m->spec.memory_reserved_bytes = share;
    if (console == NULL || etude_qemu_start(c->q, &m->spec, &m->m) != 0) {
        diag("machine %s: QEMU did not start: %s", machine, strerror(errno));
        free_dumps(m);
        free(m);
        c->faults++;
        end->end = ETUDE_END_QEMU_EXITED;
        return ETUDE_FAILED;
    }
    keep_made(c, "evidence");
    if (mres(m)->audio_path != NULL)
        made(c, mres(m)->audio_path, 0);
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

enum etude_status etude_console_find(struct etude_case_machine *m, const char *text, uint64_t from,
                                     uint64_t *at)
{
    const struct etude_machine_result *r = mres(m);
    long found;

    if (!m->decl->console || text == NULL || *text == '\0') {
        diag("machine %s: no console to search, or no text", m->decl->id);
        m->c->faults++;
        return ETUDE_REFUSED;
    }
    if (from < r->console.kept) {
        found = find_text(r->console.data + from, r->console.kept - (size_t)from, text);
        if (found >= 0) {
            *at = from + (uint64_t)found;
            return ETUDE_OK;
        }
    }
    return r->console.total > r->console.kept ? ETUDE_LOST : ETUDE_FAILED;
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
    if (mres(m)->state == ETUDE_MACHINE_ENDED || mres(m)->console.closed)
        return ETUDE_LOST;
    if (etude_machine_type(m->m, text, strlen(text)) != 0) {
        if (errno != EPERM)
            return ETUDE_LOST;
        diag("machine %s: its boundary console does not take this text", m->decl->id);
        m->c->faults++;
        return ETUDE_REFUSED;
    }
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
        if (errno == EINVAL || errno == EPERM) {
            diag("machine %s: QMP command %s refused", m->decl->id, execute);
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

/** Give the WAV path of this boot, or NULL when no audio card was selected. */
const char *etude_audio(const struct etude_case_machine *m)
{
    return mres(m)->audio_path;
}

const char *etude_capture(struct etude_case_machine *m, const char *card)
{
    size_t i;

    for (i = 0; card != NULL && m->dumps[i].path != NULL; i++)
        if (strcmp(m->dump_cards[i], card) == 0)
            return m->dumps[i].path;
    diag("machine %s: no capture of %s", m->decl->id, card != NULL ? card : "a card");
    m->c->faults++;
    return NULL;
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
    if (!machine_released(m)) {
        m->c->unconfirmed = 1;
        return ETUDE_LOST;
    }
    return end->end == ETUDE_END_LOST ? ETUDE_LOST : ETUDE_OK;
}

/** Say whether the console bytes read from boot `m` have a record of the
 * kernel's panic: a line that begins `<2>[` whose subsystem is `panic`. */
static int console_panicked(const struct etude_case_machine *m)
{
    const struct etude_machine_result *r = mres(m);
    const char *c = r->console.data, *close;
    size_t n = r->console.kept, i, end;

    for (i = 0; c != NULL && i + 4 <= n; i++) {
        if ((i > 0 && c[i - 1] != '\n') || memcmp(c + i, "<2>[", 4) != 0)
            continue;
        for (end = i; end < n && c[end] != '\r' && c[end] != '\n'; end++)
            ;
        close = memchr(c + i, ']', end - i);
        if (close != NULL && (size_t)(c + end - close) >= 9 && memcmp(close, "] panic: ", 9) == 0)
            return 1;
    }
    return 0;
}

/**
 * Say whether `text`, the human monitor's `info registers -a`, shows each of
 * `processors` processors halted with interrupts off: one section from
 * `CPU#` for each, whose flags register, EFL or RFL, has IF, bit 9, clear,
 * and which says HLT=1.
 */
static int all_halted(const char *text, unsigned processors)
{
    const char *p = text, *next, *flags, *hlt;
    unsigned seen = 0;

    while ((p = strstr(p, "CPU#")) != NULL) {
        next = strstr(p + 4, "CPU#");
        flags = strstr(p, "FL=");
        hlt = strstr(p, "HLT=");
        if (flags == NULL || hlt == NULL || (next != NULL && (flags > next || hlt > next)))
            return 0;
        if ((strtoul(flags + 3, NULL, 16) & 0x200) != 0 || hlt[4] != '1')
            return 0;
        seen++;
        p += 4;
    }
    return seen > 0 && seen == processors;
}

enum etude_status etude_fault(struct etude_case_machine *m, enum etude_end end)
{
    const struct eh_json *ret;
    struct eh_json *j;
    enum etude_status st;
    char *reply = NULL;
    int shown;

    if (m->ended || (end != ETUDE_END_PANIC && end != ETUDE_END_HALTED) ||
        (end == ETUDE_END_PANIC && !m->decl->console)) {
        diag("machine %s: boot %u: no fault of this kind to record", m->decl->id, m->boot);
        m->c->faults++;
        return ETUDE_REFUSED;
    }
    if (end == ETUDE_END_PANIC) {
        shown = console_panicked(m);
    } else {
        st = etude_qmp(m, "human-monitor-command", "{\"command-line\": \"info registers -a\"}",
                       &reply);
        if (st != ETUDE_OK) {
            free(reply);
            return st;
        }
        j = eh_json_parse(reply);
        ret = eh_json_get(j, "return");
        shown = ret != NULL && ret->type == 's' && all_halted(ret->string, m->profile.processors);
        eh_json_free(j);
        free(reply);
    }
    if (!shown) {
        diag("machine %s: boot %u does not show the fault", m->decl->id, m->boot);
        return ETUDE_FAILED;
    }
    record_end(m, end);
    return ETUDE_OK;
}

/* ---- Suspensions. ---- */

/** Say whether machine `arg` has ended, or its QEMU has been seen stopped. */
static int machine_stopped(void *arg)
{
    const struct etude_machine_result *r = mres(arg);

    return r->state == ETUDE_MACHINE_ENDED || r->qemu->suspension == ETUDE_SUSPEND_STOPPED;
}

/** Say whether machine `arg` has ended, or its QEMU has been seen to go on
 * after a stop. */
static int machine_resumed(void *arg)
{
    const struct etude_machine_result *r = mres(arg);

    return r->state == ETUDE_MACHINE_ENDED || r->qemu->suspension == ETUDE_SUSPEND_NONE;
}

/**
 * Serve the case until `until`, an end of one interval of a suspension of
 * machine `m`, or until `done` says so when it is not NULL.
 *
 * @return ETUDE_OK at `until` or when `done` says so, with the machine still
 *         there; ETUDE_LOST when it has ended; ETUDE_BOUND when the step's
 *         bound comes first; or ETUDE_STOPPED.
 */
static enum etude_status interval(struct etude_case_machine *m, done_fn done, uint64_t until)
{
    uint64_t deadline = step_deadline(m->c);
    enum etude_status st;

    st = serve_until(m->c, done != NULL ? done : machine_ended, m, NULL,
                     until != 0 && until < deadline ? until : deadline, 1);
    if (st == ETUDE_BOUND && posix_now_us() < deadline)
        st = ETUDE_OK;
    if (mres(m)->state == ETUDE_MACHINE_ENDED)
        return ETUDE_LOST;
    return st;
}

/** Find the suspension `id` of the workload, or NULL. */
static struct suspension_decl *find_suspension(struct etude_case *c, const char *id)
{
    size_t i;

    for (i = 0; id != NULL && i < c->hold_count; i++)
        if (strcmp(c->holds[i].id, id) == 0)
            return &c->holds[i];
    return NULL;
}

/**
 * Run one cycle of suspension `h` on machine `m`: the run, the stop and the
 * resumption, which the executor has seen. Give the stop in `stop`.
 */
static enum etude_status cycle(struct etude_case_machine *m, struct suspension_decl *h,
                               struct etude_stop *stop)
{
    enum etude_status st = interval(m, NULL, posix_now_us() + h->run_us);

    if (st != ETUDE_OK)
        return st;
    if (etude_machine_suspend(m->m) != 0)
        return mres(m)->state == ETUDE_MACHINE_ENDED || errno == ESRCH ? ETUDE_LOST : ETUDE_FAILED;
    st = interval(m, machine_stopped, 0);
    if (st != ETUDE_OK)
        return st;
    stop->stopped_us = mres(m)->qemu->suspended_us;
    etude_progress(m->c, h->witness, ++h->stops);
    st = interval(m, NULL, stop->stopped_us + h->stop_us);
    stop->resumed_us = posix_now_us();
    if (st != ETUDE_OK)
        return st;
    if (etude_machine_resume(m->m) != 0)
        return ETUDE_FAILED;
    return interval(m, machine_resumed, 0);
}

enum etude_status etude_suspend(struct etude_case_machine *m, const char *suspension,
                                struct etude_stop *stops, size_t max, unsigned *done)
{
    struct suspension_decl *h = find_suspension(m->c, suspension);
    enum etude_status st = ETUDE_OK;
    struct etude_stop stop;
    uint64_t i;

    *done = 0;
    if (h == NULL || strcmp(h->machine, m->decl->id) != 0 || m->c->cur < 0) {
        diag("machine %s: suspension %s is not one of its own, or no step runs", m->decl->id,
             suspension != NULL ? suspension : "(none)");
        m->c->faults++;
        return ETUDE_REFUSED;
    }
    if (stop_asked)
        return ETUDE_STOPPED;
    for (i = 0; i < h->cycles && st == ETUDE_OK; i++) {
        st = cycle(m, h, &stop);
        if (st == ETUDE_OK && *done < max)
            stops[*done] = stop;
        if (st == ETUDE_OK)
            (*done)++;
    }
    if (etude_machine_resume(m->m) != 0 && st == ETUDE_OK)
        st = ETUDE_FAILED;
    return st;
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
    m->command_output = 0;
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

/* ---- Installed guest commands and files. ---- */

static void keep_evidence(struct etude_case *, const char *, const char *, size_t);

/** Hash a regular input stream and rewind it for a framed transfer. */
static int guest_file_hash(FILE *f, uint64_t *size, char hex[65])
{
    unsigned char bytes[65536], d[32];
    struct sha256 h;
    size_t n;

    *size = 0;
    sha256_init(&h);
    while ((n = fread(bytes, 1, sizeof(bytes), f)) != 0) {
        *size += n;
        if (*size > ((uint64_t)1 << 30))
            return -1;
        sha256_update(&h, bytes, n);
    }
    if (ferror(f) || fseek(f, 0, SEEK_SET) != 0)
        return -1;
    sha256_final(&h, d);
    sha256_hex(d, hex);
    return 0;
}

/** Read a transfer part from its stream, whose position follows OFFSET. */
static int guest_file_read(void *ctx, uint64_t offset, void *bytes, size_t n)
{
    FILE *f = ctx;

    (void)offset;
    return fread(bytes, 1, n, f) == n ? 0 : -1;
}

/** Send a file as a stored relative name or an absolute destination. */
static enum etude_status guest_send_file(struct etude_case_machine *m, const char *host,
                                         const char *guest, const char *mode, uint64_t *size,
                                         char hex[65])
{
    FILE *f;
    enum etude_status st;

    if (m->serial == NULL || m->serial_closed || m->c->cur < 0 || host == NULL || guest == NULL)
        return ETUDE_REFUSED;
    f = fopen(host, "rb");
    if (f == NULL)
        return ETUDE_FAILED;
    if (guest_file_hash(f, size, hex) != 0) {
        fclose(f);
        return ETUDE_FAILED;
    }
    st = put_op(m, etude_serial_put_reader(m->serial, guest, *size, hex, mode, guest_file_read, f,
                                           posix_now_us()));
    fclose(f);
    return st;
}

enum etude_status etude_guest_put(struct etude_case_machine *m, const char *host, const char *guest,
                                  const char *mode)
{
    uint64_t size;
    char hex[65];

    if (guest == NULL || guest[0] != '/')
        return ETUDE_REFUSED;
    return guest_send_file(m, host, guest, mode, &size, hex);
}

/** Write one received part to the private evidence file. */
static int guest_file_write(void *ctx, const void *bytes, size_t n)
{
    return fwrite(bytes, 1, n, ctx) == n ? 0 : -1;
}

enum etude_status etude_guest_get(struct etude_case_machine *m, const char *guest, const char *name)
{
    const char *path;
    char target[4096], part[4120];
    FILE *f;
    enum etude_status st;

    if (m->serial == NULL || m->serial_closed || m->c->cur < 0 || guest == NULL ||
        guest[0] != '/' || (path = etude_evidence(m->c, name)) == NULL ||
        strlen(path) >= sizeof(target))
        return ETUDE_REFUSED;
    strcpy(target, path);
    snprintf(part, sizeof(part), "%s.part", target);
    f = fopen(part, "wb");
    if (f == NULL)
        return ETUDE_FAILED;
    made(m->c, part, 0);
    st = put_op(m, etude_serial_get(m->serial, guest, guest_file_write, f, posix_now_us()));
    if (fclose(f) != 0 && st == ETUDE_OK)
        st = ETUDE_FAILED;
    if (st == ETUDE_OK && rename(part, target) != 0)
        st = ETUDE_FAILED;
    if (st == ETUDE_OK)
        made(m->c, target, 0);
    else
        unlink(part);
    return st;
}

/* A bounded JSON invocation being built. */
struct command_text {
    char *text;
    size_t len;
    int failed;
};

/** Append formatted text to an invocation. */
static void command_add(struct command_text *t, const char *format, ...)
{
    va_list ap;
    int n;

    if (t->failed)
        return;
    va_start(ap, format);
    n = vsnprintf(t->text + t->len, DOCUMENT_MAX - t->len, format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= DOCUMENT_MAX - t->len)
        t->failed = 1;
    else
        t->len += (size_t)n;
}

/** Append a JSON string, including its quotes. */
static void command_string(struct command_text *t, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    command_add(t, "\"");
    for (; *p && !t->failed; p++) {
        if (*p == '"' || *p == '\\')
            command_add(t, "\\%c", *p);
        else if (*p < ' ' || *p > '~')
            command_add(t, "\\u%04x", *p);
        else
            command_add(t, "%c", *p);
    }
    command_add(t, "\"");
}

/** Append the arguments and environment, rejecting malformed assignments. */
static void command_vectors(struct command_text *t, const char *const argv[],
                            const char *const env[])
{
    static const char *const defaults[] = {"PATH=/bin:/sbin:/usr/bin:/usr/sbin:/", "HOME=/root",
                                           "TERM=vt100", NULL};
    size_t i;
    const char *eq;
    const char *const *assignments = env != NULL ? env : defaults;
    char key[256];

    command_add(t, ",\"arguments\":[");
    for (i = 1; argv[i] != NULL; i++) {
        if (i > 1)
            command_add(t, ",");
        command_string(t, argv[i]);
    }
    command_add(t, "],\"environment\":{");
    for (i = 0; assignments[i] != NULL; i++) {
        eq = strchr(assignments[i], '=');
        if (eq == NULL || eq == assignments[i] || (size_t)(eq - assignments[i]) >= sizeof(key)) {
            t->failed = 1;
            return;
        }
        if (i > 0)
            command_add(t, ",");
        memcpy(key, assignments[i], (size_t)(eq - assignments[i]));
        key[eq - assignments[i]] = '\0';
        command_string(t, key);
        command_add(t, ":");
        command_string(t, eq + 1);
    }
    command_add(t, "}");
}

/** Build the installed command's ordinary invocation. */
static int command_invocation(struct etude_case_machine *m, struct command_text *t,
                              const char *const argv[], const char *const env[], const char *input,
                              uint64_t input_size, const char *input_sha, const char *token)
{
    uint64_t now = posix_now_us(), deadline = step_deadline(m->c);

    t->text = malloc(DOCUMENT_MAX);
    t->len = 0;
    t->failed = t->text == NULL;
    command_add(t, "{\"schema_version\":1,\"run_id\":");
    command_string(t, m->attempt_id);
    command_add(t, ",\"attempt_id\":");
    command_string(t, token);
    command_add(t, ",\"attempt_token\":");
    command_string(t, token);
    command_add(t, ",\"case_id\":");
    command_string(t, m->c->case_id);
    command_add(t, ",\"program\":");
    command_string(t, argv[0]);
    command_vectors(t, argv, env);
    command_add(t, ",\"role\":\"ordinary\",\"stdin\":");
    if (input != NULL)
        command_string(t, input);
    else
        command_add(t, "null");
    command_add(t, ",\"files\":[");
    if (input != NULL && input[0] != '/')
        command_add(t,
                    "{\"name\":\"stdin\",\"sha256\":\"%s\",\"size_bytes\":%llu,"
                    "\"mode\":\"0600\"}",
                    input_sha, (unsigned long long)input_size);
    command_add(t,
                "],\"fixtures\":[{\"fixture\":\"workspace\",\"given\":[]}],"
                "\"deadline_us\":%llu,\"grace_us\":%llu,\"output_max_bytes\":%llu}",
                (unsigned long long)(deadline > now ? deadline - now : 1),
                (unsigned long long)m->c->grace_us, (unsigned long long)m->c->output_max);
    return t->failed ? -1 : 0;
}

static enum etude_status start_command(struct etude_case_machine *m, const char *const argv[],
                                       const char *const env[], const char *input,
                                       int installed_input)
{
    struct command_text t;
    char sha[65], token[17], identity[384], input_sha[65] = "";
    uint64_t size = 0;
    enum etude_status st;

    if (m->serial == NULL || m->serial_closed || m->c->cur < 0 || argv == NULL || argv[0] == NULL ||
        argv[0][0] != '/')
        return ETUDE_REFUSED;
    if (input != NULL && !installed_input) {
        st = guest_send_file(m, input, "stdin", "0600", &size, input_sha);
        if (st != ETUDE_OK)
            return st;
    }
    snprintf(identity, sizeof(identity), "%s.command.%llu", m->attempt_id,
             (unsigned long long)++m->commands);
    digest_of(identity, strlen(identity), sha);
    memcpy(token, sha, 16);
    token[16] = '\0';
    if (command_invocation(m, &t, argv, env,
                           input != NULL ? (installed_input ? input : "stdin") : NULL, size,
                           input_sha, token) != 0) {
        free(t.text);
        return ETUDE_REFUSED;
    }
    digest_of(t.text, t.len, sha);
    st = put_op(m, etude_serial_put(m->serial, "invocation.json", t.len, sha, "0644", t.text,
                                    posix_now_us()));
    free(t.text);
    if (st != ETUDE_OK)
        return st;
    m->command_output = 1;
    return etude_serial_run(m->serial, token, sha, posix_now_us()) == 0 ? ETUDE_OK : ETUDE_FAILED;
}

enum etude_status etude_guest_start(struct etude_case_machine *m, const char *const argv[],
                                    const char *const env[], const char *input)
{
    return start_command(m, argv, env, input, 0);
}

/* A wait for text on the running command's output. */
struct command_wait {
    struct etude_case_machine *m;
    const char *text;
    int found;
};

/** Say whether text arrived, or the command can produce no more. */
static int command_output_found(void *ctx)
{
    struct command_wait *w = ctx;
    const struct etude_serial_result *r = etude_serial_result(w->m->serial);

    w->found = find_text(r->out.data, r->out.kept, w->text) >= 0;
    return w->found || etude_serial_status(w->m->serial) != ETUDE_SERIAL_BUSY;
}

enum etude_status etude_guest_output_wait(struct etude_case_machine *m, const char *text)
{
    struct command_wait w;
    enum etude_status st;

    if (m->serial == NULL || !m->command_output || text == NULL || text[0] == '\0')
        return ETUDE_REFUSED;
    memset(&w, 0, sizeof(w));
    w.m = m;
    w.text = text;
    st = serve_until(m->c, command_output_found, &w, NULL, step_deadline(m->c), 1);
    return st == ETUDE_OK && !w.found ? ETUDE_LOST : st;
}

int etude_guest_running(struct etude_case_machine *m)
{
    struct etude_proc *finished = NULL;

    if (m->serial == NULL || !m->command_output)
        return 0;
    etude_qemu_watch(m->c->q, 0, NULL, 0, &finished);
    if (finished != NULL)
        host_finished(m->c, finished, NULL);
    serve_sessions(m->c, posix_now_us());
    return etude_serial_running(m->serial);
}

enum etude_status etude_guest_finish(struct etude_case_machine *m, int cancel, const char *output,
                                     struct etude_guest_result *result)
{
    const struct etude_serial_result *r;
    char err[256];
    enum etude_status st;

    memset(result, 0, sizeof(*result));
    result->termination.end = ETUDE_END_LOST;
    result->termination.value = -1;
    result->cleanup = ETUDE_CLEANUP_UNCONFIRMED;
    if (m->serial == NULL || !m->command_output || output == NULL || strlen(output) > 240)
        return ETUDE_REFUSED;
    if (cancel && etude_serial_status(m->serial) == ETUDE_SERIAL_BUSY &&
        etude_serial_cancel(m->serial, posix_now_us()) != 0)
        return ETUDE_FAILED;
    st = session_op(m, 0);
    if (st == ETUDE_BOUND || st == ETUDE_STOPPED)
        cancel_run(m);
    r = etude_serial_result(m->serial);
    result->termination.end = r->end;
    result->termination.value = r->value;
    result->termination.console_offset = mres(m)->console.total;
    result->process = r->process;
    result->cleanup = r->cleanup;
    result->complete = r->complete;
    keep_evidence(m->c, output, r->out.data, r->out.kept);
    snprintf(err, sizeof(err), "%s.err", output);
    keep_evidence(m->c, err, r->err.data, r->err.kept);
    m->command_output = 0;
    return st;
}

static int command_started(void *context)
{
    struct etude_case_machine *m = context;

    return etude_serial_result(m->serial)->started ||
           etude_serial_status(m->serial) != ETUDE_SERIAL_BUSY;
}

enum etude_status etude_guest_service_start(struct etude_case_machine *m, const char *const argv[],
                                            const char *const env[], const char *input,
                                            const char *output)
{
    enum etude_status st;
    struct etude_guest_result failed;
    int begun;

    if (output == NULL || !output[0] || strlen(output) > 240 || m->guest_service_output[0] ||
        m->serial == NULL || etude_serial_status(m->serial) == ETUDE_SERIAL_BUSY)
        return ETUDE_REFUSED;
    st = etude_guest_start(m, argv, env, input);
    begun = st == ETUDE_OK;
    if (begun)
        st = serve_until(m->c, command_started, m, NULL, step_deadline(m->c), 1);
    if (st == ETUDE_OK && etude_serial_background(m->serial) != 0)
        st = ETUDE_FAILED;
    if (st == ETUDE_OK) {
        strcpy(m->guest_service_output, output);
        m->command_output = 0;
    } else if (begun)
        etude_guest_finish(m, 1, output, &failed);
    return st;
}

int etude_guest_service_running(struct etude_case_machine *m)
{
    struct etude_proc *finished = NULL;

    if (m->serial == NULL || !m->guest_service_output[0])
        return 0;
    etude_qemu_watch(m->c->q, 0, NULL, 0, &finished);
    if (finished != NULL)
        host_finished(m->c, finished, NULL);
    serve_sessions(m->c, posix_now_us());
    return etude_serial_background_running(m->serial);
}

enum etude_status etude_guest_service_finish(struct etude_case_machine *m, int cancel,
                                             struct etude_guest_result *result)
{
    enum etude_status st;

    if (m->serial == NULL || !m->guest_service_output[0] || etude_serial_foreground(m->serial) != 0)
        return ETUDE_REFUSED;
    m->command_output = 1;
    st = etude_guest_finish(m, cancel, m->guest_service_output, result);
    m->guest_service_output[0] = '\0';
    return st;
}

enum etude_status etude_guest_command(struct etude_case_machine *m, const char *const argv[],
                                      const char *const env[], const char *input,
                                      const char *output, struct etude_guest_result *result)
{
    enum etude_status st;

    memset(result, 0, sizeof(*result));
    result->termination.end = ETUDE_END_LOST;
    result->termination.value = -1;
    result->cleanup = ETUDE_CLEANUP_UNCONFIRMED;
    st = etude_guest_start(m, argv, env, input);
    return st == ETUDE_OK ? etude_guest_finish(m, 0, output, result) : st;
}

enum etude_status etude_guest_command_file(struct etude_case_machine *m, const char *const argv[],
                                           const char *const env[], const char *input,
                                           const char *output, struct etude_guest_result *result)
{
    enum etude_status st;

    memset(result, 0, sizeof(*result));
    result->termination.end = ETUDE_END_LOST;
    result->termination.value = -1;
    result->cleanup = ETUDE_CLEANUP_UNCONFIRMED;
    if (input == NULL || input[0] != '/')
        return ETUDE_REFUSED;
    st = start_command(m, argv, env, input, 1);
    return st == ETUDE_OK ? etude_guest_finish(m, 0, output, result) : st;
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

/** Give the path of the program `name`, a tool or an executable copy of an
 * artifact of the case's, or NULL. */
static const char *program_path(struct etude_case *c, const char *name)
{
    struct artifact *a;
    size_t i;

    for (i = 0; name != NULL && i < c->tool_count; i++)
        if (strcmp(c->tools[i].name, name) == 0)
            return c->tools[i].path;
    a = find_artifact(c, name);
    return a != NULL ? executable(c, a) : NULL;
}

const char *etude_program(struct etude_case *c, const char *name)
{
    return program_path(c, name);
}

/** Make the argument vector of `program` with `args` after it, ended by
 * NULL; NULL when memory ran out. */
static char **arguments(const char *program, char *const args[])
{
    char **v;
    size_t i, n = 0;

    while (args != NULL && args[n] != NULL)
        n++;
    v = calloc(n + 2, sizeof(*v));
    if (v == NULL)
        return NULL;
    v[0] = (char *)program;
    for (i = 0; i < n; i++)
        v[i + 1] = args[i];
    return v;
}

/* ---- Image tools in the declared Rach2 helper. ---- */

/** The tools whose command line the image helper implements. */
static int image_tool(const char *name)
{
    static const char *const names[] = {"mattrib",  "mcopy",   "mdel",      "mdeltree",
                                        "mdir",     "mformat", "minfo",     "mlabel",
                                        "mmd",      "mmove",   "mrd",       "mren",
                                        "mtype",    "debugfs", "e2fsck",    "mke2fs",
                                        "dumpe2fs", "tune2fs", "resize2fs", "image-channel-fixture",
                                        NULL};
    size_t i;

    for (i = 0; name != NULL && names[i] != NULL; i++)
        if (strcmp(name, names[i]) == 0)
            return 1;
    return 0;
}

/** Split a debugfs request, including its quoted pathname operands. */
static int image_words(char *text, char **words, size_t max)
{
    char *p = text, *out = text;
    size_t n = 0;
    int quote;

    while (*p != '\0') {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;
        if (n == max)
            return -1;
        words[n++] = out;
        quote = 0;
        while (*p != '\0' && (quote || (*p != ' ' && *p != '\t'))) {
            if ((*p == '\'' || *p == '"') && (!quote || quote == *p)) {
                quote = quote ? 0 : *p;
                p++;
            } else if (*p == '\\' && p[1] != '\0') {
                p++;
                *out++ = *p++;
            } else {
                *out++ = *p++;
            }
        }
        if (quote)
            return -1;
        while (*p == ' ' || *p == '\t')
            p++;
        *out++ = '\0';
    }
    return (int)n;
}

/** Copy a fetched output into a private pathname of this attempt. */
static int image_output(struct etude_case *c, const char *from, const char *to)
{
    char root[4096], parent[4096], canonical[4096], *slash;
    unsigned char buf[65536];
    size_t n, count;
    FILE *in = NULL;
    int fd = -1, rc = -1;
    struct stat st;

    if (strlen(to) >= sizeof(parent) || realpath(c->scratch, root) == NULL)
        return -1;
    strcpy(parent, to);
    slash = strrchr(parent, '/');
    if (slash == NULL)
        return -1;
    *slash = '\0';
    if (realpath(parent, canonical) == NULL)
        return -1;
    n = strlen(root);
    if (strncmp(canonical, root, n) != 0 || (canonical[n] != '\0' && canonical[n] != '/') ||
        (lstat(to, &st) == 0 && (!S_ISREG(st.st_mode) || st.st_nlink != 1)))
        return -1;
    in = fopen(from, "rb");
    fd = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (in == NULL || fd < 0)
        goto done;
    while ((count = fread(buf, 1, sizeof(buf), in)) != 0) {
        size_t at = 0;
        while (at < count) {
            ssize_t wrote = write(fd, buf + at, count - at);
            if (wrote < 0 && errno == EINTR)
                continue;
            if (wrote <= 0)
                goto done;
            at += (size_t)wrote;
        }
    }
    if (!ferror(in))
        rc = 0;
    made(c, to, 0);
done:
    if (in != NULL)
        fclose(in);
    if (fd >= 0 && close(fd) != 0)
        rc = -1;
    return rc;
}

/* The translated command and the storage of its pathname arguments. */
struct image_invocation {
    const char *args[68], *input, *directory, *destination;
    char disk[4096], device[4096], program[80], request[8192], parsed[4096], input_file[4096];
};

/** Translate the file operand in a debugfs request. */
static enum etude_status image_request(const char *const argv[], int request_at, size_t prefix,
                                       struct image_invocation *v)
{
    char *words[8];
    int nw, file_at;

    if (strcmp(argv[0], "debugfs") != 0 || request_at < 0)
        return ETUDE_OK;
    {
        if (strlen(argv[request_at]) >= sizeof(v->parsed))
            return ETUDE_REFUSED;
        strcpy(v->parsed, argv[request_at]);
        nw = image_words(v->parsed, words, 8);
        if (nw < 1)
            return ETUDE_REFUSED;
        if (strcmp(words[0], "dump") == 0 || strcmp(words[0], "write") == 0) {
            if (nw != 3)
                return ETUDE_REFUSED;
            file_at = strcmp(words[0], "write") == 0 ? 1 : 2;
            if (file_at == 1)
                v->input = words[file_at];
            else
                v->destination = words[file_at];
            words[file_at] = "/dev/shm/image-file";
            if (strchr(words[1], '"') != NULL || strchr(words[2], '"') != NULL ||
                (size_t)snprintf(v->request, sizeof(v->request), "%s \"%s\" \"%s\"", words[0],
                                 words[1], words[2]) >= sizeof(v->request))
                return ETUDE_REFUSED;
            v->args[prefix + (size_t)request_at - 1] = v->request;
        } else if (strcmp(words[0], "rdump") == 0) {
            return ETUDE_REFUSED;
        }
    }
    return ETUDE_OK;
}

/** Keep the basename that mcopy uses for an implicit guest destination. */
static int image_input_path(struct image_invocation *v, const char *path)
{
    const char *name = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');
    if (backslash != NULL && (name == NULL || backslash > name))
        name = backslash;
#endif
    name = name != NULL ? name + 1 : path;
    if (*name == '\0' || !strcmp(name, ".") || !strcmp(name, ".."))
        return 0;
    return (size_t)snprintf(v->input_file, sizeof(v->input_file), "/dev/shm/image-input/%s", name) <
           sizeof(v->input_file);
}

static enum etude_status image_copy_arguments(const char *const argv[], size_t count, size_t prefix,
                                              struct image_invocation *v)
{
    size_t i;
    if (count < 5)
        return ETUDE_REFUSED;
    for (i = count - 2; i < count; i++) {
        if (strncmp(argv[i], "::", 2) != 0) {
            if (i == count - 1)
                v->destination = argv[i];
            else {
                v->input = argv[i];
                if (!image_input_path(v, argv[i]))
                    return ETUDE_REFUSED;
            }
            v->args[prefix + i - 1] = i == count - 1 ? "/dev/shm/image-file" : v->input_file;
        }
    }
    if (v->input != NULL && v->destination != NULL)
        return ETUDE_REFUSED;
    return ETUDE_OK;
}

static size_t image_disk_argument(const char *const argv[], size_t count, int mt, int copy,
                                  struct image_invocation *v)
{
    size_t i, disk_at = 0;
    const char *suffix;
    if (mt || copy) {
        for (i = 1; i + 1 < count; i++)
            if (strcmp(argv[i], "-i") == 0)
                disk_at = i + 1;
    } else {
        disk_at = count - 1;
        if (strcmp(argv[0], "mke2fs") == 0 && argv[disk_at][0] >= '0' && argv[disk_at][0] <= '9')
            disk_at--;
    }
    if (disk_at == 0 || strlen(argv[disk_at]) >= sizeof(v->disk))
        return 0;
    strcpy(v->disk, argv[disk_at]);
    suffix = strstr(v->disk, mt ? "@@" : "?offset=");
    if (suffix != NULL) {
        snprintf(v->device, sizeof(v->device), "/dev/disk0%s", suffix);
        v->disk[suffix - v->disk] = '\0';
    } else {
        strcpy(v->device, "/dev/disk0");
    }
    return disk_at;
}

/** Translate explicit disk and file operands, never a host shell command. */
static enum etude_status image_arguments(struct etude_case *c, char *const argv[],
                                         const char *output, struct image_invocation *v)
{
    size_t count = 0, prefix, i, disk_at = 0;
    int mt, copy, request_at = -1;

    while (count < 64 && argv[count] != NULL)
        count++;
    if (count < 2 || count == 64 || c->cur < 0 || !file_name(output))
        return ETUDE_REFUSED;
    mt = argv[0][0] == 'm' && strcmp(argv[0], "mke2fs") != 0;
    copy = strcmp(argv[0], "mcopy") == 0 || strcmp(argv[0], "image-channel-fixture") == 0;
    prefix = mt ? 3 : 1;
    snprintf(v->program, sizeof(v->program), "/bin/%s", mt ? "mtools" : argv[0]);
    v->args[0] = v->program;
    if (mt) {
        v->args[1] = "-c";
        v->args[2] = argv[0];
    }
    disk_at = image_disk_argument((const char *const *)argv, count, mt, copy, v);
    if (!disk_at)
        return ETUDE_REFUSED;
    for (i = 1; i < count; i++) {
        v->args[prefix + i - 1] = i == disk_at ? v->device : argv[i];
        if (strcmp(argv[i], "-R") == 0 && i + 1 < count)
            request_at = (int)i + 1;
        if (strcmp(argv[0], "mke2fs") == 0 && strcmp(argv[i], "-d") == 0) {
            if (i + 1 == count || v->directory != NULL)
                return ETUDE_REFUSED;
            v->directory = argv[++i];
            v->args[prefix + i - 1] = "/dev/shm/image-directory";
        }
    }
    v->args[prefix + count - 1] = NULL;
    if (copy && image_copy_arguments((const char *const *)argv, count, prefix, v) != ETUDE_OK)
        return ETUDE_REFUSED;
    return image_request((const char *const *)argv, request_at, prefix, v);
}

/** Stage a directory as a native archive for mke2fs's -d operand. */
static enum etude_status image_directory(struct etude_case *c, struct etude_case_machine *m,
                                         const char *directory, const char *output)
{
    const char *args[] = {"/bin/busybox", "tar",      "-xf", "/dev/shm/image-directory.tar",
                          "-C",           "/dev/shm", NULL};
    char path[4096], why[256], name[200];
    struct etude_volume *v;
    struct etude_guest_result result;
    enum etude_status st;
    int added, closed;

    if ((size_t)snprintf(path, sizeof(path), "%s/image-directory.tar", c->scratch) >= sizeof(path))
        return ETUDE_FAILED;
    v = etude_volume_create(path, 0, why, sizeof(why));
    if (v == NULL)
        return ETUDE_FAILED;
    made(c, path, 0);
    added = etude_volume_add(v, "image-directory", directory);
    closed = etude_volume_close(v);
    if (added != 0 || closed != 0) {
        diag("image directory: %s", why);
        return ETUDE_FAILED;
    }
    st = etude_guest_put(m, path, "/dev/shm/image-directory.tar", "0600");
    if (st != ETUDE_OK)
        return st;
    snprintf(name, sizeof(name), "image-directory.%s", output);
    st = etude_guest_command(m, args, NULL, NULL, name, &result);
    if (st == ETUDE_OK && (!result.complete || result.cleanup != ETUDE_CLEANUP_CONFIRMED ||
                           result.termination.end != ETUDE_END_EXITED || result.termination.value))
        st = ETUDE_FAILED;
    return st;
}

/** Require a complete command and confirmed cleanup while placing an input. */
static enum etude_status image_input_command(struct etude_case *c, struct etude_case_machine *m,
                                             const char *const args[], const char *output)
{
    struct etude_guest_result result;
    enum etude_status st = etude_guest_command(m, args, NULL, NULL, output, &result);

    if (st != ETUDE_OK)
        return st;
    if (result.cleanup != ETUDE_CLEANUP_CONFIRMED) {
        c->unconfirmed = 1;
        return ETUDE_LOST;
    }
    if (!result.complete || result.termination.end != ETUDE_END_EXITED ||
        result.termination.value != 0)
        return ETUDE_FAILED;
    return ETUDE_OK;
}

/* A declared disk for bulk image-command inputs and outputs. */
struct image_transfer {
    char disk[4096], digest[65];
    int active, pending;
};

#define IMAGE_TRANSFER_BYTES (64u * 1024 * 1024)

/** Prepare only the transfer slot the case explicitly declared. */
static enum etude_status image_transfer_create(struct etude_case *c,
                                               const struct image_invocation *v,
                                               struct image_transfer *t)
{
    struct machine_decl *d = find_decl(c, "image-helper");
    const struct eh_json *disks = d != NULL ? eh_json_get(d->qemu, "disks") : NULL, *item;
    struct etude_volume *volume;
    struct stat st;
    char why[256];
    int added = 0, closed;

    for (item = disks != NULL ? disks->child : NULL; item != NULL; item = item->next)
        if (jtext(item, "name") != NULL && !strcmp(jtext(item, "name"), "image-transfer"))
            break;
    if (item == NULL || (v->input == NULL && v->destination == NULL))
        return ETUDE_OK;
    if (jnum(item, "size_bytes") != IMAGE_TRANSFER_BYTES ||
        (jtext(item, "bus") != NULL && strcmp(jtext(item, "bus"), "ide") != 0))
        return ETUDE_REFUSED;
    if ((size_t)snprintf(t->disk, sizeof(t->disk), "%s/image-transfer.%u", c->scratch,
                         d->boots + 1) >= sizeof(t->disk))
        return ETUDE_FAILED;
    volume = etude_volume_create(t->disk, IMAGE_TRANSFER_BYTES, why, sizeof(why));
    if (volume == NULL)
        return ETUDE_FAILED;
    made(c, t->disk, 0);
    if (v->input != NULL) {
        added = etude_digest_file(NULL, v->input, &st, t->digest);
        if (added == 0)
            added = etude_volume_add(volume, "image-file", v->input);
    }
    closed = etude_volume_close(volume);
    if (added != 0 || closed != 0 ||
        etude_bind_disk(c, "image-helper", "image-transfer", t->disk) != ETUDE_OK)
        return ETUDE_FAILED;
    t->active = 1;
    return ETUDE_OK;
}

/** Read the guest's digest through the bounded command-result channel. */
static enum etude_status image_transfer_hash(struct etude_case *c, struct etude_case_machine *m,
                                             const char *output, char digest[65])
{
    const char *args[] = {"/bin/busybox", "sha256sum", "/dev/shm/image-file", NULL};
    char name[200], *text;
    const char *path;
    enum etude_status st;
    size_t n, i;

    snprintf(name, sizeof(name), "image-hash.%s", output);
    st = image_input_command(c, m, args, name);
    if (st != ETUDE_OK)
        return st;
    path = etude_evidence(c, name);
    text = path != NULL ? read_whole(path, &n) : NULL;
    if (text == NULL)
        return ETUDE_FAILED;
    if (n != 64 + strlen("  /dev/shm/image-file\n") ||
        strcmp(text + 64, "  /dev/shm/image-file\n") != 0) {
        free(text);
        return ETUDE_FAILED;
    }
    for (i = 0; i < 64; i++)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) {
            free(text);
            return ETUDE_FAILED;
        }
    memcpy(digest, text, 64);
    digest[64] = '\0';
    free(text);
    return ETUDE_OK;
}

/** Read a complete output archive only after its machine has ended. */
static enum etude_status image_transfer_finish(struct etude_case *c,
                                               const struct image_invocation *v,
                                               const struct image_transfer *t, const char *output)
{
    char name[200], path[4096], digest[65], why[256];
    struct stat st;
    const char *target;

    if (!t->pending)
        return !t->active || unlink(t->disk) == 0 ? ETUDE_OK : ETUDE_FAILED;
    snprintf(name, sizeof(name), "image-file.%s", output);
    target = etude_evidence(c, name);
    if (target == NULL || strlen(target) >= sizeof(path))
        return ETUDE_FAILED;
    strcpy(path, target);
    if (etude_volume_file(t->disk, "image-file", path, why, sizeof(why)) != 0) {
        diag("image output: %s", why);
        return ETUDE_FAILED;
    }
    made(c, path, 0);
    if (etude_digest_file(NULL, path, &st, digest) != 0 || strcmp(digest, t->digest) != 0) {
        diag("image output: the guest and host digests differ");
        return ETUDE_FAILED;
    }
    if (image_output(c, path, v->destination) != 0 || unlink(t->disk) != 0)
        return ETUDE_FAILED;
    return ETUDE_OK;
}

/** Transfer through one protocol field, then place the exact basename. */
static enum etude_status image_input(struct etude_case *c, struct etude_case_machine *m,
                                     const struct image_invocation *v, const char *output,
                                     const struct image_transfer *transfer)
{
    const char *extract[] = {"/bin/busybox", "tar", "-xf", "/dev/disk1", "-C", "/dev/shm", NULL};
    const char *mkdir_args[] = {"/bin/busybox",         "mkdir", "-m", "0700",
                                "/dev/shm/image-input", NULL};
    const char *move_args[] = {"/bin/busybox", "mv", "/dev/shm/image-file", v->input_file, NULL};
    enum etude_status st;
    char name[200], digest[65];

    if (v->input == NULL)
        return ETUDE_OK;
    if (transfer->active) {
        snprintf(name, sizeof(name), "image-extract.%s", output);
        st = image_input_command(c, m, extract, name);
        if (st == ETUDE_OK)
            st = image_transfer_hash(c, m, output, digest);
        if (st == ETUDE_OK && strcmp(digest, transfer->digest) != 0)
            st = ETUDE_FAILED;
    } else {
        st = etude_guest_put(m, v->input, "/dev/shm/image-file", "0600");
    }
    if (st != ETUDE_OK || v->input_file[0] == '\0')
        return st;
    snprintf(name, sizeof(name), "image-input.%s", output);
    st = image_input_command(c, m, mkdir_args, name);
    if (st != ETUDE_OK)
        return st;
    snprintf(name, sizeof(name), "image-place.%s", output);
    return image_input_command(c, m, move_args, name);
}

/** Keep a file an image command wrote, even when the command failed. */
static enum etude_status image_fetch(struct etude_case *c, struct etude_case_machine *m,
                                     const char *destination, const char *output, int succeeded,
                                     struct image_transfer *transfer)
{
    const char *args[] = {"/bin/busybox", "test", "-f", "/dev/shm/image-file", NULL};
    struct etude_guest_result result;
    char name[200];
    const char *path;
    enum etude_status st;

    snprintf(name, sizeof(name), "image-exists.%s", output);
    st = etude_guest_command(m, args, NULL, NULL, name, &result);
    if (st != ETUDE_OK)
        return st;
    if (!result.complete || result.cleanup != ETUDE_CLEANUP_CONFIRMED ||
        result.termination.end != ETUDE_END_EXITED)
        return ETUDE_LOST;
    if (result.termination.value != 0)
        return result.termination.value == 1 && !succeeded ? ETUDE_OK : ETUDE_FAILED;
    if (transfer->active) {
        const char *archive[] = {"/bin/busybox", "tar",      "-cf",        "/dev/shm/image-archive",
                                 "-C",           "/dev/shm", "image-file", NULL};
        const char *copy[] = {"/bin/busybox",  "dd",       "if=/dev/shm/image-archive",
                              "of=/dev/disk1", "bs=65536", NULL};
        st = image_transfer_hash(c, m, output, transfer->digest);
        if (st != ETUDE_OK)
            return st;
        snprintf(name, sizeof(name), "image-archive.%s", output);
        st = image_input_command(c, m, archive, name);
        /* tar writes a file's short tail before its sector padding. */
        if (st == ETUDE_OK) {
            snprintf(name, sizeof(name), "image-write.%s", output);
            st = image_input_command(c, m, copy, name);
        }
        transfer->pending = st == ETUDE_OK;
        return st;
    }
    snprintf(name, sizeof(name), "image-file.%s", output);
    st = etude_guest_get(m, "/dev/shm/image-file", name);
    if (st != ETUDE_OK)
        return st;
    path = etude_evidence(c, name);
    return path != NULL && image_output(c, path, destination) == 0 ? ETUDE_OK : ETUDE_FAILED;
}

/** Run one image command; the helper has no mounted volume from its disk. */
static enum etude_status image_host(struct etude_case *c, char *const argv[], const char *output,
                                    struct etude_termination *end)
{
    struct image_invocation v;
    struct image_transfer transfer;
    const char *sync_args[] = {"/bin/busybox", "sync", NULL};
    const char *disks[] = {"image-disk", "image-transfer", NULL};
    char sync_output[200], *reply = NULL;
    struct etude_case_machine *m = NULL;
    struct etude_termination stopped;
    struct etude_guest_result result, synced;
    enum etude_status st, stop_status;

    memset(&v, 0, sizeof(v));
    memset(&transfer, 0, sizeof(transfer));
    st = image_arguments(c, argv, output, &v);
    if (st != ETUDE_OK)
        return st;
    if (etude_bind_disk(c, "image-helper", "image-disk", v.disk) != ETUDE_OK)
        return ETUDE_REFUSED;
    st = image_transfer_create(c, &v, &transfer);
    if (st != ETUDE_OK)
        return st;
    if (!transfer.active)
        disks[1] = NULL;
    st = etude_boot(c, "image-helper", disks, &m, &stopped);
    if (st != ETUDE_OK)
        return st;
    if ((st = image_input(c, m, &v, output, &transfer)) != ETUDE_OK)
        goto done;
    if (v.directory != NULL && (st = image_directory(c, m, v.directory, output)) != ETUDE_OK)
        goto done;
    st = etude_guest_command(m, v.args, NULL, NULL, output, &result);
    *end = result.termination;
    if (st != ETUDE_OK || !result.complete)
        goto done;
    if (result.cleanup != ETUDE_CLEANUP_CONFIRMED) {
        c->unconfirmed = 1;
        st = ETUDE_LOST;
        goto done;
    }
    if (v.destination != NULL && (st = image_fetch(c, m, v.destination, output,
                                                   result.termination.end == ETUDE_END_EXITED &&
                                                       result.termination.value == 0,
                                                   &transfer)) != ETUDE_OK)
        goto done;
    snprintf(sync_output, sizeof(sync_output), "image-sync.%s", output);
    st = etude_guest_command(m, sync_args, NULL, NULL, sync_output, &synced);
    if (st == ETUDE_OK &&
        (!synced.complete || synced.cleanup != ETUDE_CLEANUP_CONFIRMED ||
         synced.termination.end != ETUDE_END_EXITED || synced.termination.value != 0))
        st = ETUDE_FAILED;
done:
    stop_status = etude_qmp(m, "quit", NULL, &reply);
    free(reply);
    if (stop_status == ETUDE_OK)
        stop_status = etude_wait_end(m, &stopped);
    if (st == ETUDE_OK && stop_status == ETUDE_OK)
        return image_transfer_finish(c, &v, &transfer, output);
    return st == ETUDE_OK ? stop_status : st;
}

/* The fixed independent filesystem command and its optional private copy. */
struct fstool_invocation {
    const char *args[8];
    char disk[4096];
    uint64_t start, count;
    int format;
};

static int fstool_kind(const char *kind, size_t n, const char *mode, struct fstool_invocation *v)
{
    if (n == 7 && !strcmp(kind, "exfat-mkfs")) {
        v->format = 1;
        return 1;
    }
    if (n == 5 && !strcmp(kind, "fat-fsck"))
        return 1;
    return (n == 5 || (n == 6 && !strcmp(mode, "repair"))) && !strcmp(kind, "exfat-fsck");
}

static enum etude_status fstool_copy(struct etude_case *c, const char *original, const char *output,
                                     struct fstool_invocation *v)
{
    char name[200];
    const char *copy;
    if ((size_t)snprintf(name, sizeof(name), "fstool-volume.%s", output) >= sizeof(name) ||
        (copy = etude_evidence(c, name)) == NULL || strlen(copy) >= sizeof(v->disk))
        return ETUDE_REFUSED;
    strcpy(v->disk, copy);
    if (store_copy_range(original, v->start * 512, v->count * 512, v->disk))
        return ETUDE_FAILED;
    made(c, v->disk, 0);
    return ETUDE_OK;
}

static enum etude_status fstool_arguments(struct etude_case *c, char *const argv[],
                                          const char *output, struct fstool_invocation *v)
{
    size_t n = 0, at;
    char original[4096];
    enum etude_status st;
    while (n < 8 && argv[n])
        n++;
    if (n < 2 || n == 8 || !fstool_kind(argv[1], n, n == 6 ? argv[5] : "", v))
        return ETUDE_REFUSED;
    at = v->format ? 4 : 3;
    if (!wire_number(argv[at], &v->start) || !wire_number(argv[at + 1], &v->count) || !v->count ||
        v->start > INT64_MAX / 512 || v->count > INT64_MAX / 512 - v->start ||
        !private_disk_path(c, argv[2], original) || disk_active(c, original))
        return ETUDE_REFUSED;
    if (v->format) {
        strcpy(v->disk, original);
        for (at = 3; at < n; at++)
            v->args[at] = argv[at];
    } else {
        st = fstool_copy(c, original, output, v);
        if (st != ETUDE_OK)
            return st;
        if (n == 6)
            v->args[3] = "repair";
    }
    v->args[0] = "/host-products/fstool";
    v->args[1] = argv[1];
    v->args[2] = "/dev/disk1";
    return ETUDE_OK;
}

static enum etude_status fstool_command(struct etude_case *c, struct etude_case_machine *m,
                                        const struct fstool_invocation *v, const char *output,
                                        struct etude_termination *end)
{
    const char *sync_args[] = {"/bin/busybox", "sync", NULL};
    struct etude_guest_result result;
    char name[200];
    enum etude_status st;
    memset(&result, 0, sizeof(result));
    st = etude_guest_command(m, v->args, NULL, NULL, output, &result);
    *end = result.termination;
    if (st != ETUDE_OK)
        return st;
    if (!result.complete || result.cleanup != ETUDE_CLEANUP_CONFIRMED) {
        c->unconfirmed |= result.cleanup != ETUDE_CLEANUP_CONFIRMED;
        return ETUDE_LOST;
    }
    snprintf(name, sizeof(name), "fstool-sync.%s", output);
    st = etude_guest_command(m, sync_args, NULL, NULL, name, &result);
    if (st != ETUDE_OK)
        return st;
    if (!result.complete || result.cleanup != ETUDE_CLEANUP_CONFIRMED) {
        c->unconfirmed |= result.cleanup != ETUDE_CLEANUP_CONFIRMED;
        return ETUDE_LOST;
    }
    return result.termination.end == ETUDE_END_EXITED && !result.termination.value ? ETUDE_OK
                                                                                   : ETUDE_FAILED;
}

static enum etude_status fstool_host(struct etude_case *c, char *const argv[], const char *output,
                                     struct etude_termination *end)
{
    const char *disks[] = {"fstool-root", "fstool-disk", NULL};
    struct fstool_invocation v;
    struct etude_case_machine *m = NULL;
    struct etude_termination stopped;
    char *reply = NULL;
    enum etude_status st, stop;
    if (c->cur < 0 || !file_name(output))
        return ETUDE_REFUSED;
    memset(&v, 0, sizeof(v));
    st = fstool_arguments(c, argv, output, &v);
    if (st != ETUDE_OK)
        return st;
    st = etude_bind_disk(c, "fstool-helper", "fstool-disk", v.disk);
    if (st != ETUDE_OK)
        goto done;
    st = etude_boot(c, "fstool-helper", disks, &m, &stopped);
    if (st != ETUDE_OK)
        goto done;
    st = fstool_command(c, m, &v, output, end);
    stop = etude_qmp(m, "quit", NULL, &reply);
    free(reply);
    if (stop == ETUDE_OK)
        stop = etude_wait_end(m, &stopped);
    if (st == ETUDE_OK)
        st = stop;
done:
    if (!v.format && st == ETUDE_OK && end->end == ETUDE_END_EXITED && !end->value &&
        unlink(v.disk))
        return ETUDE_FAILED;
    return st;
}

enum etude_status etude_host(struct etude_case *c, char *const argv[], const char *output,
                             struct etude_termination *end)
{
    struct etude_exec_spec spec;
    struct etude_proc *p, *running;
    const struct etude_exec_result *r;
    const char *program, *evidence;
    enum etude_status st;
    char attempt[300], err_name[160], out_path[4096], err_path[4096];
    char **args;

    end->end = ETUDE_END_LOST;
    end->value = -1;
    end->console_offset = 0;
    if (stop_asked)
        return ETUDE_STOPPED;
    if (argv != NULL && argv[0] != NULL && !strcmp(argv[0], "fstool"))
        return etude_has_machine(c, "fstool-helper") ? fstool_host(c, argv, output, end)
                                                     : ETUDE_REFUSED;
    if (argv != NULL && image_tool(argv[0]) && etude_has_machine(c, "image-helper"))
        return image_host(c, argv, output, end);
    program = argv != NULL ? program_path(c, argv[0]) : NULL;
    if (c->cur < 0 || program == NULL || !file_name(output) ||
        (size_t)snprintf(err_name, sizeof(err_name), "%s.err", output) >= sizeof(err_name)) {
        diag("%s is no tool or artifact of the case, or no step runs",
             argv != NULL && argv[0] != NULL ? argv[0] : "(null)");
        c->faults++;
        return ETUDE_REFUSED;
    }
    args = arguments(program, argv + 1);
    if (args == NULL)
        return ETUDE_FAILED;
    snprintf(attempt, sizeof(attempt), "%s.host.%s", c->attempt_id, output);
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = attempt;
    spec.argv = args;
    spec.envp = environ;
    spec.cwd = c->scratch;
    spec.output_max = (size_t)c->output_max;
    spec.grace_us = c->grace_us;
    evidence = etude_evidence(c, output);
    if (evidence != NULL)
        snprintf(out_path, sizeof(out_path), "%s", evidence);
    if (evidence == NULL || (evidence = etude_evidence(c, err_name)) == NULL) {
        free(args);
        c->faults++;
        return ETUDE_FAILED;
    }
    snprintf(err_path, sizeof(err_path), "%s", evidence);
    spec.stdout_path = out_path;
    spec.stderr_path = err_path;
    made(c, out_path, 0);
    made(c, err_path, 0);
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
    end->end = r->end;
    end->value = r->value;
    if (r->cleanup != ETUDE_CLEANUP_CONFIRMED)
        c->unconfirmed = 1;
    etude_proc_free(p);
    if (st == ETUDE_OK && end->end == ETUDE_END_LOST)
        st = ETUDE_LOST;
    return st;
}

/* ---- Services. ---- */

/** Find the service `id` of the manifest, or NULL. */
static struct service_decl *find_service(struct etude_case *c, const char *id)
{
    size_t i;

    for (i = 0; id != NULL && i < c->sdecl_count; i++)
        if (strcmp(c->sdecls[i].id, id) == 0)
            return &c->sdecls[i];
    return NULL;
}

unsigned etude_port(struct etude_case *c, unsigned i)
{
    if (i >= c->port_count) {
        diag("the case has no port %u", i);
        c->faults++;
        return 0;
    }
    return c->ports[i];
}

/** Say whether service `s` can write no more: it has finished, closed its
 * standard output, or passed the bytes kept of it. */
static int service_gone(const struct etude_case_service *s)
{
    const struct etude_exec_result *r = etude_proc_result(s->p);

    return s->finished || r->out.closed || r->out.total > r->out.kept;
}

/* A wait for text on a service's standard output. */
struct out_wait {
    struct etude_case_service *s;
    struct etude_case_machine *guest;
    const char *text;
    uint64_t from;
    long at; /* the offset past the text once found, else -1 */
};

/** Say whether the text has appeared, or can no longer appear. */
static int out_found(void *arg)
{
    struct out_wait *t = arg;
    const struct etude_exec_result *r = etude_proc_result(t->s->p);
    long at;

    if (t->from < r->out.kept) {
        at = find_text(r->out.data + t->from, r->out.kept - (size_t)t->from, t->text);
        if (at >= 0) {
            t->at = (long)t->from + at;
            return 1;
        }
    }
    return service_gone(t->s) ||
           (t->guest != NULL && etude_serial_status(t->guest->serial) != ETUDE_SERIAL_BUSY);
}

/** Say whether service `arg` has finished. */
static int service_finished(void *arg)
{
    return ((const struct etude_case_service *)arg)->finished;
}

/** Keep the output of service `s`, once, and note a cleanup not confirmed. */
static void keep_service(struct etude_case_service *s)
{
    struct etude_case *c = s->c;
    const struct etude_exec_result *r = etude_proc_result(s->p);
    char name[160];

    if (s->kept)
        return;
    s->kept = 1;
    snprintf(name, sizeof(name), "%s.%u.out", s->decl->id, s->n);
    keep_evidence(c, name, r->out.data, r->out.kept);
    snprintf(name, sizeof(name), "%s.%u.err", s->decl->id, s->n);
    keep_evidence(c, name, r->err.data, r->err.kept);
    if (!s->finished || r->cleanup != ETUDE_CLEANUP_CONFIRMED)
        c->unconfirmed = 1;
}

/** Give the end of service `s`, which has finished, seen by the scenario. */
static void service_end(struct etude_case_service *s, struct etude_termination *end)
{
    const struct etude_exec_result *r = etude_proc_result(s->p);

    end->end = r->end;
    end->value = r->value;
    end->console_offset = r->out.total;
    s->seen = 1;
    keep_service(s);
}

/** Stop service `s` and wait until the executor has ended it. */
static void service_halt(struct etude_case_service *s)
{
#ifdef _WIN32
    if (!s->finished) {
        const struct etude_exec_result *result = etude_proc_result(s->p);
        const struct etude_pid *id = &result->process;
        if (id->start_known && !etude_win_service_stop((DWORD)id->pid, id->start)) {
            uint64_t now = posix_now_us(), grace = s->c->grace_us;
            uint64_t until = grace < UINT64_MAX - now ? now + grace : UINT64_MAX;
            serve_until(s->c, service_finished, s, NULL, until, 0);
        }
    }
#endif
    if (!s->finished) {
        etude_exec_cancel(s->p);
        serve_until(s->c, service_finished, s, NULL, UINT64_MAX, 0);
    }
}

enum etude_status etude_service(struct etude_case *c, const char *service, char *const args[],
                                struct etude_case_service **sp, struct etude_termination *end)
{
    struct service_decl *d = find_service(c, service);
    struct etude_exec_spec spec;
    struct etude_case_service *s;
    struct out_wait t;
    enum etude_status st;
    const char *program;
    char attempt[400], **argv;

    *sp = NULL;
    end->end = ETUDE_END_LOST;
    end->value = -1;
    end->console_offset = 0;
    if (stop_asked)
        return ETUDE_STOPPED;
    if (d == NULL || c->cur < 0 || (d->now != NULL && !d->now->finished)) {
        diag("service %s is not one of the case's, runs already, or no step runs",
             service != NULL ? service : "(null)");
        c->faults++;
        return ETUDE_REFUSED;
    }
    program = program_path(c, d->program);
    if (program == NULL) {
        diag("service %s: its program %s cannot be used", d->id, d->program);
        c->faults++;
        return ETUDE_REFUSED;
    }
    s = calloc(1, sizeof(*s));
    argv = arguments(program, args);
    if (s == NULL || argv == NULL) {
        free(s);
        free(argv);
        return ETUDE_FAILED;
    }
    s->c = c;
    s->decl = d;
    s->n = d->starts + 1;
    snprintf(attempt, sizeof(attempt), "%s.service.%s.%u", c->attempt_id, d->id, s->n);
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = attempt;
    spec.argv = argv;
    spec.envp = environ;
    spec.cwd = c->scratch;
    spec.output_max = (size_t)c->output_max;
    spec.grace_us = c->grace_us;
    if (etude_exec_start(etude_qemu_exec(c->q), &spec, &s->p) != 0) {
        diag("service %s did not start: %s", d->id, strerror(errno));
        free(argv);
        free(s);
        c->faults++;
        return ETUDE_FAILED;
    }
    free(argv);
    d->starts = s->n;
    d->now = s;
    s->next = c->services;
    c->services = s;
    memset(&t, 0, sizeof(t));
    t.s = s;
    t.text = d->ready;
    t.at = -1;
    st = serve_until(c, out_found, &t, NULL, step_deadline(c), 1);
    if (st == ETUDE_OK && t.at >= 0) {
        *sp = s;
        return ETUDE_OK;
    }
    if (st == ETUDE_OK)
        diag("service %s %s before it was ready", d->id,
             s->finished ? "ended" : "closed or filled its output");
    service_halt(s);
    service_end(s, end);
    return st == ETUDE_OK ? ETUDE_FAILED : st;
}

static enum etude_status service_wait(struct out_wait *t, uint64_t *at)
{
    struct etude_case_service *s = t->s;
    const struct etude_exec_result *r = etude_proc_result(s->p);
    enum etude_status st;

    if (s->c->cur < 0 || t->text == NULL || *t->text == '\0') {
        diag("service %s: no text to wait for, or no step", s->decl->id);
        s->c->faults++;
        return ETUDE_REFUSED;
    }
    t->at = -1;
    st = serve_until(s->c, out_found, t, NULL, step_deadline(s->c), 1);
    if (st != ETUDE_OK)
        return st;
    if (t->at >= 0) {
        *at = (uint64_t)t->at;
        return ETUDE_OK;
    }
    if (r->out.total > r->out.kept) {
        diag("service %s: its output passed the %llu bytes kept", s->decl->id,
             (unsigned long long)s->c->output_max);
        return ETUDE_FAILED;
    }
    if (service_gone(s))
        s->seen = 1;
    return ETUDE_LOST;
}

enum etude_status etude_service_wait(struct etude_case_service *s, const char *text, uint64_t from,
                                     uint64_t *at)
{
    struct out_wait t = {s, NULL, text, from, -1};

    return service_wait(&t, at);
}

enum etude_status etude_service_wait_guest(struct etude_case_service *s,
                                           struct etude_case_machine *guest, const char *text,
                                           uint64_t from, uint64_t *at)
{
    struct out_wait t = {s, guest, text, from, -1};

    if (guest == NULL || guest->c != s->c || guest->serial == NULL || !guest->command_output)
        return ETUDE_REFUSED;
    return service_wait(&t, at);
}

enum etude_status etude_service_end(struct etude_case_service *s, struct etude_termination *end)
{
    enum etude_status st;

    end->end = ETUDE_END_LOST;
    end->value = -1;
    end->console_offset = 0;
    if (s->c->cur < 0) {
        s->c->faults++;
        return ETUDE_REFUSED;
    }
    st = serve_until(s->c, service_finished, s, NULL, step_deadline(s->c), 1);
    if (st != ETUDE_OK)
        return st;
    service_end(s, end);
    return ETUDE_OK;
}

enum etude_status etude_service_stop(struct etude_case_service *s, struct etude_termination *end)
{
    service_halt(s);
    service_end(s, end);
    return ETUDE_OK;
}

/* ---- The attempt. ---- */

/** Say whether every machine and service has ended. */
static int all_ended(void *arg)
{
    const struct etude_case *c = arg;
    const struct etude_case_machine *m;
    const struct etude_case_service *s;

    for (m = c->machines; m != NULL; m = m->next)
        if (mres(m)->state != ETUDE_MACHINE_ENDED)
            return 0;
    for (s = c->services; s != NULL; s = s->next)
        if (!s->finished)
            return 0;
    return 1;
}

/* The QMP commands of a machine's diagnostics, and their arguments. */
static const char *const capture_command[CAPTURES] = {"query-status", "query-cpus-fast",
                                                      "human-monitor-command"};
static const char *const capture_args[CAPTURES] = {NULL, NULL,
                                                   "{\"command-line\": \"info registers -a\"}"};

/** Say whether every command of the diagnostics has its reply, or never
 * will. */
static int captured(void *arg)
{
    const struct etude_case *c = arg;
    const struct etude_case_machine *m;
    const char *reply;
    int i;

    for (m = c->machines; m != NULL; m = m->next)
        for (i = 0; i < CAPTURES; i++)
            if (m->capture[i] >= 0 && etude_machine_reply(m->m, m->capture[i], &reply) == 0)
                return 0;
    return 1;
}

/** Keep the diagnostics of machine `m` as evidence `MACHINE.N.capture`, a
 * line for each command: its reply, `no reply` or `not sent`. */
static void keep_capture(struct etude_case_machine *m)
{
    const char *reply[CAPTURES];
    char name[200], *text;
    size_t size = 1, n = 0;
    int i;

    for (i = 0; i < CAPTURES; i++) {
        reply[i] = m->capture[i] < 0 ? "not sent" : "no reply";
        if (m->capture[i] >= 0 && etude_machine_reply(m->m, m->capture[i], &reply[i]) != 1)
            reply[i] = "no reply";
        size += strlen(capture_command[i]) + strlen(reply[i]) + 3;
    }
    text = malloc(size);
    if (text == NULL)
        return;
    for (i = 0; i < CAPTURES; i++)
        n += (size_t)snprintf(text + n, size - n, "%s: %s\n", capture_command[i], reply[i]);
    snprintf(name, sizeof(name), "%s.%u.capture", m->decl->id, m->boot);
    keep_evidence(m->c, name, text, n);
    free(text);
}

/**
 * Collect the diagnostics of every machine that runs with QMP, its status,
 * its processors and its registers, waiting for the replies until `until`,
 * and keep them.
 */
static void capture(struct etude_case *c, uint64_t until)
{
    struct etude_case_machine *m;
    int i, any = 0;

    for (m = c->machines; m != NULL; m = m->next) {
        int running = m->decl->qmp && mres(m)->state == ETUDE_MACHINE_RUNNING;

        for (i = 0; i < CAPTURES; i++)
            m->capture[i] =
                running ? etude_machine_qmp(m->m, capture_command[i], capture_args[i]) : -1;
        any |= running;
    }
    if (!any)
        return;
    serve_until(c, captured, c, NULL, until, 0);
    for (m = c->machines; m != NULL; m = m->next)
        if (m->decl->qmp && (m->capture[0] >= 0 || mres(m)->state == ETUDE_MACHINE_RUNNING))
            keep_capture(m);
}

/** Say whether service `s` ended by the signal of a stop, SIGTERM or
 * SIGKILL, and not by itself. */
static int stopped_by_signal(const struct etude_case_service *s)
{
    const struct etude_exec_result *r = etude_proc_result(s->p);

#ifdef _WIN32
    return r->end == ETUDE_END_STOPPED ||
           (r->exit_end == ETUDE_END_EXITED && (uint32_t)r->exit_value == ETUDE_WIN_STOP_STATUS);
#else
    return r->exit_end == ETUDE_END_SIGNALED &&
           (r->exit_value == SIGTERM || r->exit_value == SIGKILL);
#endif
}

/** Retain the bounded QEMU diagnostic stream of a failed machine. */
static void keep_qemu_error(struct etude_case_machine *m)
{
    const struct etude_machine_result *r = mres(m);
    char name[200];

    if (r->qemu == NULL)
        return;
    snprintf(name, sizeof(name), "%s.%u.qemu.stderr", m->decl->id, m->boot);
    keep_evidence(m->c, name, r->qemu->err.data, r->qemu->err.kept);
}

/* Finish the retained guest attempt before ending its machine. */
static void end_guest_service(struct etude_case_machine *m, uint64_t until)
{
    const struct etude_serial_result *r;
    char error[256];

    if (m->serial == NULL || !m->guest_service_output[0])
        return;
    if (etude_serial_status(m->serial) == ETUDE_SERIAL_BUSY)
        cancel_run(m);
    if (etude_serial_foreground(m->serial) == 0) {
        if (etude_serial_status(m->serial) == ETUDE_SERIAL_BUSY)
            etude_serial_cancel(m->serial, posix_now_us());
        serve_until(m->c, session_done, m, NULL, until, 0);
        r = etude_serial_result(m->serial);
        keep_evidence(m->c, m->guest_service_output, r->out.data, r->out.kept);
        snprintf(error, sizeof(error), "%s.err", m->guest_service_output);
        keep_evidence(m->c, error, r->err.data, r->err.kept);
        if (!r->complete || r->cleanup != ETUDE_CLEANUP_CONFIRMED)
            m->c->faults++;
    } else
        m->c->faults++;
    m->guest_service_output[0] = '\0';
}

/**
 * End what the attempt owns within the cleanup's bound: after a failure or a
 * stop, collect the machines' diagnostics first; then stop every service and
 * machine still running, keep the services' output, and note how each was
 * cleaned up. A service whose end the scenario did not see must end by the
 * stop's signal; one that exited, or was killed by another signal, ended by
 * itself while the case ran, which is a fault.
 */
static void end_all(struct etude_case *c, int failing)
{
    uint64_t until = posix_now_us() + c->cleanup_us, grace = posix_now_us() + c->grace_us;
    struct etude_case_machine *m;
    struct etude_case_service *s;

    if (failing)
        capture(c, grace < until ? grace : until);
    for (s = c->services; s != NULL; s = s->next)
        etude_exec_cancel(s->p);
    for (m = c->machines; m != NULL; m = m->next) {
        end_guest_service(m, until);
        etude_machine_cancel(m->m);
    }
    serve_until(c, all_ended, c, NULL, until, 0);
    for (m = c->machines; m != NULL; m = m->next) {
        const struct etude_machine_result *r = mres(m);

        if (r->state != ETUDE_MACHINE_ENDED || r->qemu == NULL ||
            r->qemu->cleanup != ETUDE_CLEANUP_CONFIRMED)
            c->unconfirmed = 1;
        if (failing)
            keep_qemu_error(m);
    }
    for (s = c->services; s != NULL; s = s->next) {
        if (s->finished && !s->seen && !stopped_by_signal(s)) {
            diag("service %s ended before the case stopped it", s->decl->id);
            c->faults++;
            c->unseen = 1;
        }
        keep_service(s);
    }
}

/** Say whether every assertion of the scenario's was reported once and
 * held, and nothing was misused or lost. */
static int held(const struct etude_case *c)
{
    size_t i;

    if (c->faults > 0)
        return 0;
    for (i = 0; i < c->assert_count; i++)
        if (c->asserts[i].reported != 1 || !c->asserts[i].held)
            return 0;
    return 1;
}

/** Remove what the library made, the newest first, but what it keeps. */
static void remove_made(struct etude_case *c)
{
    struct made *m;

    for (m = c->made; m != NULL; m = m->next)
        if (!m->kept && (m->dir ? rmdir(m->path) : unlink(m->path)) != 0 && errno != ENOENT)
            diag("%s: not removed: %s", m->path, strerror(errno));
}

/** Free the case and what it holds. */
static void case_free(struct etude_case *c)
{
    struct etude_case_machine *m;
    struct etude_case_service *s;
    struct made *f;
    struct blank *b;
    struct disk_binding *binding;
    size_t i;

    while ((m = c->machines) != NULL) {
        c->machines = m->next;
        if (m->serial != NULL)
            etude_serial_free(m->serial);
        free_dumps(m);
        free(m);
    }
    while ((s = c->services) != NULL) {
        c->services = s->next;
        free(s);
    }
    while ((f = c->made) != NULL) {
        c->made = f->next;
        free(f->path);
        free(f);
    }
    while ((b = c->blanks) != NULL) {
        c->blanks = b->next;
        free(b->path);
        free(b);
    }
    while ((binding = c->bindings) != NULL) {
        c->bindings = binding->next;
        free(binding->name);
        free(binding->path);
        free(binding);
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
    free(c->sdecls);
    free(c->holds);
    free(c->ports);
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
    end_all(&c, rc || stopped || !held(&c));
    if (!rc && !stopped && held(&c) && !c.unconfirmed)
        remove_made(&c);
    else
        diag("kept: %s", c.scratch);
    etude_qemu_free(c.q);
    rc = rc || stopped || c.unconfirmed || c.unseen;
    case_free(&c);
    return rc;
}
