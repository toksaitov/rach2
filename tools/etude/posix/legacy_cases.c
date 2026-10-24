/*
 * legacy_cases.c: the host, guest and machine cases of a suite's manifest as
 * the set driver runs them. It reads the manifest, gives each case that runs
 * on the run's architecture its program, its arguments, its bounds, its
 * claim and the canonical documents of its item, and judges an attempt by
 * `etude check attempt`.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <unistd.h>

#ifdef _WIN32
#include "../win/files.h"
#endif

#include "digest.h"
#include "etude/qemu.h"
#include "legacy_cases.h"
#include "posix.h"

/* The most bytes of a manifest, and of a results document, that are read. */
#define MANIFEST_BYTES ((size_t)16 << 20)
#define RESULTS_BYTES  ((size_t)16 << 20)

/* How often a judge that runs is looked at, in milliseconds. */
#define JUDGE_POLL_MS 20

/* ---- Text and files. ---- */

/** Give a string from malloc made as printf makes it, or NULL. */
static char *text_of(const char *fmt, ...)
{
    va_list ap;
    char *s;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || (s = malloc((size_t)n + 1)) == NULL)
        return NULL;
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

/**
 * Read the whole of file `path`, at most `max` bytes, into memory of its own
 * ended by a NUL, with its length in `*len`.
 *
 * @return the bytes, or NULL with errno set, EFBIG for a file over `max`.
 */
static char *read_file(const char *path, size_t max, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *data = NULL;
    size_t got = 0;
    int e;

    *len = 0;
    if (f == NULL)
        return NULL;
    if ((data = malloc(max + 1)) != NULL)
        got = fread(data, 1, max + 1, f);
    e = data == NULL ? ENOMEM : ferror(f) ? EIO : got > max ? EFBIG : 0;
    fclose(f);
    if (e != 0) {
        free(data);
        errno = e;
        return NULL;
    }
    data[got] = '\0';
    *len = got;
    return data;
}

/** Copy the first line of file `path` into `out`, or "" when it has none. */
static void first_line(const char *path, char *out, size_t size)
{
    FILE *f = fopen(path, "r");

    out[0] = '\0';
    if (f != NULL && fgets(out, (int)size, f) == NULL)
        out[0] = '\0';
    if (f != NULL)
        fclose(f);
    out[strcspn(out, "\n")] = '\0';
}

int legacy_remove_tree(const char *path)
{
    struct stat st;
    struct dirent *e;
    DIR *d;
    char *sub;
    int bad = 0;

    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
    chmod(path, (st.st_mode & 07777) | S_IRWXU);
    if ((d = opendir(path)) == NULL)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if ((sub = text_of("%s/%s", path, e->d_name)) == NULL || legacy_remove_tree(sub) != 0)
            bad = -1;
        free(sub);
    }
    closedir(d);
    return rmdir(path) == 0 || errno == ENOENT ? bad : -1;
}

/* ---- The manifest. ---- */

/** Give member `name` of `o` when it is of type `t`, or NULL. */
static const struct json *typed(const struct json *o, const char *name, enum json_type t)
{
    const struct json *v = json_member(o, name);

    return v != NULL && v->type == t ? v : NULL;
}

/** Give the integer member `name` of `o`, or -1 when it is not one. */
static long long count_of(const struct json *o, const char *name)
{
    const struct json *v = typed(o, name, JSON_INTEGER);

    return v != NULL ? v->integer : -1;
}

/**
 * Give bound `b` as etude_bound_fix fixes it without history, which is its
 * initial value, or 0 when it is absent or malformed.
 */
static uint64_t fixed_of(const struct json *b)
{
    long long floor = count_of(b, "floor_us"), initial = count_of(b, "initial_us"),
              ceiling = count_of(b, "ceiling_us");
    struct etude_bound bound;
    struct etude_bound_fixed f;

    if (floor <= 0 || initial <= 0 || ceiling <= 0)
        return 0;
    bound.floor_us = (uint64_t)floor;
    bound.initial_us = (uint64_t)initial;
    bound.ceiling_us = (uint64_t)ceiling;
    return etude_bound_fix(&bound, NULL, 0, &f) == 0 ? f.us : 0;
}

/** Write why manifest `m` is refused into `m->why`, after its path; -1. */
static int refuse(struct legacy_manifest *m, const char *fmt, ...)
{
    va_list ap;
    int n = snprintf(m->why, sizeof(m->why), "%s: ", m->path != NULL ? m->path : "?");

    va_start(ap, fmt);
    if (n > 0 && (size_t)n < sizeof(m->why))
        vsnprintf(m->why + n, sizeof(m->why) - (size_t)n, fmt, ap);
    va_end(ap);
    return -1;
}

/** Say whether the last part of `family`, split at dots, is `suite`. */
static int of_suite(const char *family, const char *suite)
{
    const char *last = strrchr(family, '.');

    return strcmp(last != NULL ? last + 1 : family, suite) == 0;
}

/** Give the architecture of the item of case `c` on `arch`: "none", `arch`,
 * or NULL when the case does not run there. */
static const char *runs_on(const struct json *c, const char *arch)
{
    const struct json *a = json_member(c, "architectures");
    size_t i;

    if (a != NULL && a->type == JSON_STRING)
        return strcmp(a->string, "none") == 0 ? "none" : NULL;
    for (i = 0; a != NULL && a->type == JSON_ARRAY && i < a->count; i++)
        if (a->item[i]->type == JSON_STRING && strcmp(a->item[i]->string, arch) == 0)
            return arch;
    return NULL;
}

/** Say whether a card of guest case `c`'s profile forwards a local port. */
static int forwards_ports(const struct json *c)
{
    const struct json *devices = typed(
        typed(typed(c, "requirements", JSON_OBJECT), "qemu", JSON_OBJECT), "devices", JSON_ARRAY);
    const struct json *f;
    size_t i;

    for (i = 0; devices != NULL && i < devices->count; i++) {
        f = typed(devices->item[i], "forwards", JSON_ARRAY);
        if (f != NULL && f->count > 0)
            return 1;
    }
    return 0;
}

/** Say whether case `k` takes its local ports and loop devices only in a
 * native run, which the driver does not make: a guest case whose profile
 * forwards no port. */
static int native_only(const struct legacy_case *k)
{
    return k->guest && !forwards_ports(k->json);
}

/** Take the local ports of case `k`, whose resources are `r`, which the
 * driver gives a machine case alone, and a case `native_only` none; 0, or
 * -1 after a refusal. */
static int ports_of(struct legacy_manifest *m, struct legacy_case *k, const struct json *r)
{
    long long n = count_of(r, "ports");

    if (native_only(k)) {
        k->ports = 0;
        return 0;
    }
    if (n < 0 || n > LEGACY_PORTS || (n > 0 && !k->machine))
        return refuse(m,
                      "%s: the case needs local ports, which the set driver gives a machine "
                      "case alone, at most %u",
                      k->case_id, LEGACY_PORTS);
    k->ports = (unsigned)n;
    return 0;
}

/** Take the tooling memory of host or machine case `k` from its resources
 * `r`, when it has any; 0, or -1 after a refusal. */
static int tooling_of(struct legacy_manifest *m, struct legacy_case *k, const struct json *r)
{
    if (json_member(r, "tooling_memory_bytes") == NULL)
        return 0;
    if (k->guest || count_of(r, "tooling_memory_bytes") < 1)
        return refuse(m,
                      "%s: the case's tooling memory is not a host or machine case's count of "
                      "bytes",
                      k->case_id);
    k->tooling_bytes = (uint64_t)count_of(r, "tooling_memory_bytes");
    return 0;
}

/* Older manifests have only a CPU floor. */
static uint64_t cpu_ceiling(const struct json *resources, uint64_t floor)
{
    long long ceiling = count_of(resources, "cpu_ceiling_millis");
    return ceiling > 0 ? (uint64_t)ceiling : floor;
}

/** Check what case `k` needs of the driver, and take its claim; 0, or -1
 * after a refusal. */
static int needs_of(struct legacy_manifest *m, struct legacy_case *k)
{
    const struct json *e = typed(k->json, "execution", JSON_OBJECT);
    const struct json *r = typed(k->json, "resources", JSON_OBJECT);
    const struct json *iso = typed(e, "isolation", JSON_ARRAY), *in = json_member(e, "stdin");
    const struct json *locks = typed(r, "locks", JSON_ARRAY);
    const char *domain = json_text(k->json, "domain"), *f;
    size_t i;

    if (domain == NULL || (strcmp(domain, "host") != 0 && strcmp(domain, "guest") != 0 &&
                           strcmp(domain, "machine") != 0))
        return refuse(m,
                      "%s: a case of the domain %s; the set driver runs host, guest and machine "
                      "cases alone",
                      k->case_id, domain != NULL ? domain : "?");
    k->guest = strcmp(domain, "guest") == 0;
    k->machine = strcmp(domain, "machine") == 0;
    if (iso == NULL || in == NULL || locks == NULL)
        return refuse(m, "%s: the case lacks its isolation, its input or its locks", k->case_id);
    if (in->type != JSON_NULL)
        return refuse(m, "%s: the case reads standard input, which the set driver does not give",
                      k->case_id);
    if (locks->count != 0 || (count_of(r, "loop_devices") != 0 && !native_only(k)))
        return refuse(m,
                      "%s: the case needs loop devices or host locks, which the set driver "
                      "does not give",
                      k->case_id);
    if (ports_of(m, k, r) != 0)
        return -1;
    for (i = 0; i < iso->count; i++) {
        f = json_text(iso->item[i], "fixture");
        if (f != NULL && (k->guest || k->machine) && strcmp(f, "disk") == 0)
            continue;
        if (f == NULL || strcmp(f, "scratch") != 0)
            return refuse(m,
                          "%s: the case needs the fixture %s; the set driver gives scratch alone, "
                          "and disk to a guest or machine case",
                          k->case_id, f != NULL ? f : "?");
        k->scratch = 1;
    }
    k->scratch |= k->machine;
    k->cpu_millis = (uint64_t)count_of(r, "cpu_millis");
    k->cpu_ceiling_millis = cpu_ceiling(r, k->cpu_millis);
    k->memory_bytes = (uint64_t)count_of(r, "memory_bytes");
    k->disk_bytes = (uint64_t)count_of(r, "disk_bytes");
    if (count_of(r, "cpu_millis") < 1 || count_of(r, "memory_bytes") < 1 ||
        count_of(r, "disk_bytes") < 0)
        return refuse(m, "%s: the case's resources lack a processor, memory or disk", k->case_id);
    return tooling_of(m, k, r);
}

/** Say whether `name` is in a `disk` fixture of case `k`'s isolation. */
static int disk_fixture(const struct legacy_case *k, const char *name)
{
    const struct json *iso =
        typed(typed(k->json, "execution", JSON_OBJECT), "isolation", JSON_ARRAY);
    const struct json *names;
    const char *f;
    size_t i, j;

    for (i = 0; iso != NULL && i < iso->count; i++) {
        f = json_text(iso->item[i], "fixture");
        names = typed(iso->item[i], "names", JSON_ARRAY);
        for (j = 0; f != NULL && strcmp(f, "disk") == 0 && names != NULL && j < names->count; j++)
            if (names->item[j]->type == JSON_STRING && strcmp(names->item[j]->string, name) == 0)
                return 1;
    }
    return 0;
}

/** Say whether case `k` has an artifact named `name`. */
static int artifact_of(const struct legacy_case *k, const char *name)
{
    size_t i;

    for (i = 0; name != NULL && i < k->ngoals; i++)
        if (strcmp(k->names[i], name) == 0)
            return 1;
    return 0;
}

/**
 * Check the disks of guest case `k`'s QEMU profile `q`: each a CD whose
 * medium is an artifact, an empty disk of its size, or an artifact of a
 * `disk` fixture, which the driver copies; 0, or -1 after a refusal.
 */
static int disks_of(struct legacy_manifest *m, const struct legacy_case *k, const struct json *q)
{
    const struct json *disks = typed(q, "disks", JSON_ARRAY), *d;
    const char *name, *media;
    size_t i;

    for (i = 0; disks != NULL && i < disks->count; i++) {
        d = disks->item[i];
        name = d->type == JSON_STRING ? d->string : json_text(d, "name");
        media = d->type == JSON_OBJECT ? json_text(d, "media") : NULL;
        if (name == NULL)
            return refuse(m, "%s: a disk of the case's QEMU profile has no name", k->case_id);
        if (d->type == JSON_OBJECT && json_member(d, "size_bytes") != NULL)
            continue;
        if (!artifact_of(k, name))
            return refuse(m, "%s: the disk %s is no artifact of the case's prepare list",
                          k->case_id, name);
        if ((media == NULL || strcmp(media, "cdrom") != 0) && !disk_fixture(k, name))
            return refuse(m,
                          "%s: the disk %s is in no disk fixture, so it would be written in place",
                          k->case_id, name);
    }
    return 0;
}

/** Give the stage that a manifest's `bounds.stages` names `name`, or -1. */
static int stage_of(const char *name)
{
    static const struct {
        const char *name;
        enum etude_stage stage;
    } stages[] = {{"prepare", ETUDE_STAGE_PREPARE},
                  {"transfer", ETUDE_STAGE_TRANSFER},
                  {"boot", ETUDE_STAGE_BOOT},
                  {"capture", ETUDE_STAGE_CAPTURE},
                  {"cleanup", ETUDE_STAGE_CLEANUP}};
    size_t i;

    for (i = 0; i < sizeof(stages) / sizeof(stages[0]); i++)
        if (strcmp(stages[i].name, name) == 0)
            return (int)stages[i].stage;
    return -1;
}

/**
 * Check what guest case `k` needs of the driver beyond a host case's: its
 * role, its end, its QEMU profile and its artifacts; take the memory
 * etude_qemu_memory reserves for its machine as its claim; 0, or -1 after a
 * refusal.
 */
static int guest_of(struct legacy_manifest *m, struct legacy_case *k, const char *arch)
{
    const struct json *e = typed(k->json, "execution", JSON_OBJECT);
    const char *role = json_text(e, "role");
    const char *end = json_text(typed(k->json, "termination", JSON_OBJECT), "end");
    const char *firmware, *initrd;
    struct etude_qemu_profile p;
    struct etude_qemu_memory mem;

    k->qemu = typed(typed(k->json, "requirements", JSON_OBJECT), "qemu", JSON_OBJECT);
    if (role == NULL || (strcmp(role, "ordinary") != 0 && strcmp(role, "boot") != 0))
        return refuse(m,
                      "%s: the case's role is %s; the set driver runs ordinary and boot guest "
                      "cases alone",
                      k->case_id, role != NULL ? role : "?");
    if (end == NULL || (strcmp(end, "exited") != 0 && strcmp(end, "signaled") != 0))
        return refuse(m, "%s: the case ends as %s; the set driver runs a guest case that exits",
                      k->case_id, end != NULL ? end : "?");
    if (k->qemu == NULL)
        return refuse(m, "%s: the guest case has no QEMU profile", k->case_id);
    firmware = json_text(k->qemu, "firmware");
    initrd = json_text(k->qemu, "initrd");
    if (firmware == NULL || strcmp(firmware, "ovmf") == 0)
        return refuse(m, "%s: the case boots by %s, which the set driver does not find", k->case_id,
                      firmware != NULL ? firmware : "?");
    if (!artifact_of(k, json_text(k->qemu, "kernel")) ||
        (initrd != NULL && !artifact_of(k, initrd)))
        return refuse(m, "%s: the case's kernel or RAM image is no artifact of its prepare list",
                      k->case_id);
    if (disks_of(m, k, k->qemu) != 0)
        return -1;
    memset(&p, 0, sizeof(p));
    p.architecture = arch;
    p.cpu = json_text(k->qemu, "cpu");
    p.processors = (unsigned)count_of(k->qemu, "processors");
    p.memory_bytes = (uint64_t)count_of(k->qemu, "memory_bytes");
    p.memory_profile = json_text(k->qemu, "memory_profile");
    if ((p.memory_profile != NULL && strcmp(p.memory_profile, "reserved") != 0) ||
        etude_qemu_memory(&p, &mem) != 0)
        return refuse(m,
                      "%s: the case's machine is not of reserved memory in whole mebibytes, "
                      "which the set driver runs alone",
                      k->case_id);
    k->memory_bytes = mem.resident_bytes + mem.overhead_bytes;
    return 0;
}

/** Take the workload of case `k`: its arguments, repetitions and seed; 0,
 * or -1 after a refusal. */
static int workload_of(struct legacy_manifest *m, struct legacy_case *k)
{
    const struct json *w = typed(k->json, "workload", JSON_OBJECT);
    const struct json *args = typed(w, "arguments", JSON_ARRAY), *seed = json_member(w, "seed");
    size_t i;

    k->reps = count_of(w, "repetitions");
    if (args == NULL || seed == NULL || k->reps < 1 ||
        (seed->type != JSON_NULL && seed->type != JSON_INTEGER))
        return refuse(m, "%s: the case's workload lacks its arguments, repetitions or seed",
                      k->case_id);
    if (k->machine && args->count > 0)
        return refuse(m,
                      "%s: a machine case has arguments, but its scenario takes its invocation "
                      "alone",
                      k->case_id);
    if ((k->args = calloc(args->count + 1, sizeof(*k->args))) == NULL)
        return refuse(m, "memory ran out");
    for (i = 0; i < args->count; i++) {
        if (args->item[i]->type != JSON_STRING)
            return refuse(m, "%s: an argument of the case is not a string", k->case_id);
        k->args[k->nargs++] = args->item[i]->string;
    }
    k->seeded = seed->type == JSON_INTEGER;
    k->seed = seed->integer;
    return 0;
}

/** Give the kind of witness named `kind`, or -1. */
static int witness_kind(const char *kind)
{
    if (kind == NULL)
        return -1;
    return strcmp(kind, "step") == 0      ? ETUDE_WITNESS_STEP
           : strcmp(kind, "counter") == 0 ? ETUDE_WITNESS_COUNTER
           : strcmp(kind, "quiet") == 0   ? ETUDE_WITNESS_QUIET
                                          : -1;
}

/** Take the bounds of case `k`, its stages' for a guest or machine case, and
 * its witnesses of progress, each fixed without history; 0, or -1 after a
 * refusal. */
static int bounds_of(struct legacy_manifest *m, struct legacy_case *k)
{
    const struct json *b = typed(k->json, "bounds", JSON_OBJECT);
    const struct json *p = typed(k->json, "progress", JSON_ARRAY), *w;
    const struct json *stages = typed(b, "stages", JSON_OBJECT);
    struct etude_witness_bound *wb;
    size_t i;
    int kind, s;

    k->case_us = fixed_of(typed(b, "case", JSON_OBJECT));
    k->gap_us = fixed_of(typed(b, "gap", JSON_OBJECT));
    for (i = 0; (k->guest || k->machine) && stages != NULL && i < stages->count; i++)
        if ((s = stage_of(stages->item[i]->name)) >= 0 &&
            (k->stage_us[s] = fixed_of(stages->item[i])) == 0)
            return refuse(m, "%s: the case's %s bound is malformed", k->case_id,
                          stages->item[i]->name);
    if (k->case_us == 0 || k->gap_us == 0 || p == NULL)
        return refuse(m, "%s: the case lacks its bounds or its progress", k->case_id);
    k->expected_us = (uint64_t)count_of(typed(b, "case", JSON_OBJECT), "initial_us");
    if ((k->witnesses = calloc(p->count + 1, sizeof(*k->witnesses))) == NULL)
        return refuse(m, "memory ran out");
    for (i = 0; i < p->count; i++) {
        w = p->item[i];
        wb = &k->witnesses[k->nwitnesses];
        wb->witness_id = json_text(w, "witness_id");
        kind = witness_kind(json_text(w, "kind"));
        wb->limit = count_of(w, "limit") > 0 ? (uint64_t)count_of(w, "limit") : 0;
        wb->bound_us = kind == ETUDE_WITNESS_QUIET ? fixed_of(typed(w, "bound", JSON_OBJECT)) : 0;
        if (wb->witness_id == NULL || kind < 0 ||
            (kind == ETUDE_WITNESS_QUIET && wb->bound_us == 0))
            return refuse(m, "%s: a witness of the case's progress is malformed", k->case_id);
        wb->kind = (enum etude_witness)kind;
        k->nwitnesses++;
    }
    return 0;
}

/**
 * Take the artifacts of case `k`'s `prepare` as make goals of the tree on
 * the run's architecture `arch`, and find the one its program names; 0, or
 * -1 after a refusal. With `kernel`, the kernel a guest case boots from the
 * node `rach2/kernel` is the goal of the node `rach2-KERNEL/kernel`.
 */
static int goals_of(struct legacy_manifest *m, struct legacy_case *k, const char *arch,
                    const char *kernel)
{
    const struct json *prep = typed(k->json, "prepare", JSON_ARRAY), *a, *per;
    const char *program = json_text(typed(k->json, "execution", JSON_OBJECT), "program");
    const char *boots = json_text(
        typed(typed(k->json, "requirements", JSON_OBJECT), "qemu", JSON_OBJECT), "kernel");
    const char *node, *name, *dir;
    size_t i;
    int found = 0;

    if (prep == NULL || program == NULL)
        return refuse(m, "%s: the case lacks its prepare list or its program", k->case_id);
    if ((k->goals = calloc(prep->count + 1, sizeof(*k->goals))) == NULL ||
        (k->names = calloc(prep->count + 1, sizeof(*k->names))) == NULL)
        return refuse(m, "memory ran out");
    for (i = 0; i < prep->count; i++) {
        a = prep->item[i];
        node = json_text(a, "node");
        name = json_text(a, "name");
        per = typed(a, "per_architecture", JSON_BOOLEAN);
        if (node == NULL || name == NULL || per == NULL)
            return refuse(m, "%s: an artifact of the case is malformed", k->case_id);
        dir = per->integer && strcmp(arch, "x86_64") == 0 ? "build/x86_64" : "build";
        if (kernel != NULL && k->guest && boots != NULL && strcmp(name, boots) == 0 &&
            strcmp(node, "rach2/kernel") == 0)
            k->goals[k->ngoals] = text_of("%s/rach2-%s/kernel", dir, kernel);
        else
            k->goals[k->ngoals] = text_of("%s/%s", dir, node);
        if (k->goals[k->ngoals] == NULL)
            return refuse(m, "memory ran out");
        k->names[k->ngoals] = name;
        if (!found && strcmp(name, program) == 0) {
            k->program = k->ngoals;
            found = 1;
        }
        k->ngoals++;
    }
    if (!found)
        return refuse(m, "%s: the case's program %s is no artifact of its prepare list", k->case_id,
                      program);
    return 0;
}

/** Take case `c` of `m`, whose item is of `item_arch`, on the run's
 * architecture `arch` with the kernel `kernel`; 0, or -1 after a refusal. */
static int take_case(struct legacy_manifest *m, const struct json *c, const char *item_arch,
                     const char *arch, const char *kernel)
{
    struct legacy_case *k = &m->cases[m->ncases++];

    k->json = c;
    k->arch = item_arch;
    if ((k->case_id = json_text(c, "case_id")) == NULL)
        return refuse(m, "a case has no identity");
    if (needs_of(m, k) != 0 || workload_of(m, k) != 0 || bounds_of(m, k) != 0 ||
        goals_of(m, k, arch, kernel) != 0 || (k->guest && guest_of(m, k, arch) != 0))
        return -1;
    workload_document(c, item_arch, &k->workload);
    inventory_document(c, &k->inventory);
    profile_document(c, k->guest || k->machine ? "qemu" : "host", &k->profile);
    return 0;
}

int legacy_manifest_read(struct legacy_manifest *m, const char *tree, const char *path,
                         const char *suite, const char *arch, const char *kernel)
{
    const struct json *cases;
    const char *where;
    char err[200], *text;
    size_t len, i;
    int rc;

    memset(m, 0, sizeof(*m));
    m->path = legacy_absolute(path) ? text_of("%s", path) : text_of("%s/%s", tree, path);
    if (m->path == NULL)
        return refuse(m, "memory ran out");
    if ((text = read_file(m->path, MANIFEST_BYTES, &len)) == NULL)
        return refuse(m, "%s", strerror(errno));
    rc = json_parse(text, len, 1, &m->doc, err, sizeof(err));
    free(text);
    if (rc != 0)
        return refuse(m, "%s", err);
    m->family = json_text(m->doc, "family_id");
    if (m->family == NULL || (cases = typed(m->doc, "cases", JSON_ARRAY)) == NULL)
        return refuse(m, "the manifest has no family or no cases");
    if (!of_suite(m->family, suite))
        return refuse(m, "its family %s is not of the suite %s", m->family, suite);
    if ((m->cases = calloc(cases->count + 1, sizeof(*m->cases))) == NULL)
        return refuse(m, "memory ran out");
    for (i = 0; i < cases->count; i++)
        if ((where = runs_on(cases->item[i], arch)) != NULL &&
            take_case(m, cases->item[i], where, arch, kernel) != 0)
            return -1;
    if (m->ncases == 0)
        return refuse(m, "no case of it runs on %s", arch);
    return 0;
}

uint64_t legacy_case_memory(const struct legacy_case *k, int tooling)
{
    return tooling && k->tooling_bytes > 0 ? k->tooling_bytes : k->memory_bytes;
}

void legacy_manifest_free(struct legacy_manifest *m)
{
    size_t i, g;

    for (i = 0; m->cases != NULL && i < m->ncases; i++) {
        struct legacy_case *k = &m->cases[i];

        for (g = 0; g < k->ngoals; g++)
            free(k->goals[g]);
        free(k->goals);
        free(k->names);
        free(k->args);
        free(k->witnesses);
        jbuf_free(&k->workload);
        jbuf_free(&k->inventory);
        jbuf_free(&k->profile);
    }
    free(m->cases);
    json_free(m->doc);
    free(m->path);
    m->cases = NULL;
    m->doc = NULL;
    m->path = NULL;
    m->ncases = 0;
}

/* ---- The judge. ---- */

#ifndef _WIN32
/**
 * Start `argv`, the judge, with its input from /dev/null, its output to
 * `j->results` and its errors to `j->errors`, and no other descriptor.
 *
 * @return its process, or -1 with errno set.
 */
static pid_t judge_start(const struct legacy_judge *j, char *const *argv)
{
    static const int keep[] = {0, 1, 2};
    int fd[3];
    pid_t pid = fork();

    if (pid != 0)
        return pid;
    fd[0] = open("/dev/null", O_RDONLY);
    fd[1] = open(j->results, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    fd[2] = open(j->errors, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd[0] < 0 || fd[1] < 0 || fd[2] < 0 || dup2(fd[0], 0) < 0 || dup2(fd[1], 1) < 0 ||
        dup2(fd[2], 2) < 0)
        _exit(127);
    posix_close_others(keep, 3);
    execv(argv[0], argv);
    _exit(127);
}

/** Wait for the judge `pid` for LEGACY_JUDGE_S seconds, and kill it past
 * them; 0 with its status in `*status`, or -1 once it was killed. */
static int judge_wait(pid_t pid, int *status)
{
    uint64_t end = posix_now_us() + (uint64_t)LEGACY_JUDGE_S * 1000000u;
    pid_t got;

    for (;;) {
        got = waitpid(pid, status, WNOHANG);
        if (got == pid)
            return 0;
        if (got < 0 && errno != EINTR)
            return -1;
        if (posix_now_us() > end)
            break;
        poll(NULL, 0, JUDGE_POLL_MS);
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, status, 0) < 0 && errno == EINTR) {
    }
    return -1;
}

#endif

/** Give the first assertion of `list` whose outcome is `a` or `b`, or NULL. */
static const struct json *first_of(const struct json *list, const char *a, const char *b)
{
    const char *o;
    size_t i;

    for (i = 0; list != NULL && i < list->count; i++) {
        o = json_text(list->item[i], "outcome");
        if (o != NULL && (strcmp(o, a) == 0 || (b != NULL && strcmp(o, b) == 0)))
            return list->item[i];
    }
    return NULL;
}

/** Write assertion `a`, after `prefix`, with its outcome and its detail. */
static void tell_assertion(const struct json *a, const char *prefix, char *out, size_t size)
{
    const char *o = json_text(a, "outcome"), *id = json_text(a, "assertion_id");
    const char *detail = json_text(a, "detail");
    const char *verb = strcmp(o, "fail") == 0      ? "failed"
                       : strcmp(o, "missing") == 0 ? "is missing"
                                                   : "was not run";

    snprintf(out, size, "%s%s %s%s%s", prefix, id != NULL ? id : "?", verb,
             detail != NULL ? ": " : "", detail != NULL ? detail : "");
}

/** Write into `out` the assertion of results `doc` that decides `outcome`,
 * a failure or an incomplete attempt, with its detail. */
static void reason_of(const struct json *doc, enum legacy_judged outcome, char *out, size_t size)
{
    const struct json *list = typed(doc, "assertions", JSON_ARRAY), *a;
    const char *state = json_text(typed(doc, "report", JSON_OBJECT), "state");

    if (outcome == LEGACY_FAILURE && (a = first_of(list, "fail", NULL)) != NULL)
        tell_assertion(a, "", out, size);
    else if (outcome == LEGACY_FAILURE &&
             (a = first_of(typed(doc, "undeclared", JSON_ARRAY), "fail", NULL)) != NULL)
        tell_assertion(a, "the undeclared assertion ", out, size);
    else if (outcome == LEGACY_INCOMPLETE && (a = first_of(list, "missing", "not_run")) != NULL)
        tell_assertion(a, "", out, size);
    else if (state != NULL && strcmp(state, "complete") != 0 && strcmp(state, "not_used") != 0)
        snprintf(out, size, "the report is %s", state);
    else
        snprintf(out, size, "the results give %s",
                 outcome == LEGACY_FAILURE ? "a failure" : "an incomplete attempt");
}

/** Take what the judge gave after it ended with `status`. */
static void judge_take(const struct legacy_judge *j, int status, struct legacy_judgement *out)
{
    struct json *doc = NULL;
    char err[200];
#ifdef _WIN32
    int code = status, sig = 0;
#else
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
#endif

    if (code != 0 && code != 1 && code != 3) {
        first_line(j->errors, err, sizeof(err));
        if (code < 0)
            snprintf(out->reason, sizeof(out->reason), "etude check was killed by signal %d", sig);
        else
            snprintf(out->reason, sizeof(out->reason), "etude check exited with status %d%s%s",
                     code, err[0] != '\0' ? ": " : "", err);
        return;
    }
    out->doc = read_file(j->results, RESULTS_BYTES, &out->len);
    if (out->doc == NULL)
        snprintf(out->reason, sizeof(out->reason), "its results cannot be read: %s",
                 strerror(errno));
    else if (json_parse(out->doc, out->len, 1, &doc, err, sizeof(err)) != 0 ||
             doc->type != JSON_OBJECT)
        snprintf(out->reason, sizeof(out->reason), "etude check gave no results document: %s",
                 doc == NULL ? err : "not an object");
    else
        out->outcome = (enum legacy_judged)code;
    if (out->outcome != LEGACY_UNJUDGED && code != 0)
        reason_of(doc, out->outcome, out->reason, sizeof(out->reason));
    if (out->outcome == LEGACY_UNJUDGED) {
        free(out->doc);
        out->doc = NULL;
        out->len = 0;
    }
    json_free(doc);
}

#ifdef _WIN32
/** Write a captured stream to its evidence file. */
static int judge_output(const char *path, const struct etude_output *output)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (f == NULL)
        return -1;
    ok = fwrite(output->data, 1, output->kept, f) == output->kept;
    if (fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

/** Run the native judge under a job and retain both of its output streams. */
static void windows_judge(const struct legacy_judge *j, char *const *argv,
                          struct legacy_judgement *out)
{
    struct etude_exec *ex = etude_exec_new();
    struct etude_exec_spec spec;
    struct etude_proc *proc = NULL;
    const struct etude_exec_result *r;
    uint64_t until = posix_now_us() + (uint64_t)LEGACY_JUDGE_S * 1000000u;
    memset(&spec, 0, sizeof spec);
    spec.attempt_id = "check";
    spec.argv = argv;
    spec.output_max = RESULTS_BYTES;
    if (ex == NULL || etude_exec_start(ex, &spec, &proc) != 0) {
        snprintf(out->reason, sizeof out->reason, "etude check could not start: %s",
                 strerror(errno));
        goto done;
    }
    while (etude_exec_wait(ex, JUDGE_POLL_MS) != proc && posix_now_us() < until) {
    }
    r = etude_proc_result(proc);
    if (r->finished_us == 0) {
        etude_exec_cancel(proc);
        while (etude_exec_wait(ex, -1) != proc) {
        }
        snprintf(out->reason, sizeof out->reason, "etude check did not end within %u s",
                 LEGACY_JUDGE_S);
        goto done;
    }
    if (r->end != ETUDE_END_EXITED || r->cleanup != ETUDE_CLEANUP_CONFIRMED) {
        snprintf(out->reason, sizeof out->reason, "etude check did not end cleanly");
        goto done;
    }
    if (judge_output(j->results, &r->out) != 0 || judge_output(j->errors, &r->err) != 0) {
        snprintf(out->reason, sizeof out->reason, "etude check's output could not be kept: %s",
                 strerror(errno));
        goto done;
    }
    judge_take(j, r->value, out);
done:
    if (ex != NULL)
        etude_exec_free(ex);
}
#endif

void legacy_judge_run(const struct legacy_judge *j, struct legacy_judgement *out)
{
    char value[16];
    char *argv[20 + 2 * LEGACY_CONSOLES];
    size_t k = 0, i;
#ifndef _WIN32
    pid_t pid;
    int status;
#endif

    memset(out, 0, sizeof(*out));
    out->outcome = LEGACY_UNJUDGED;
    argv[k++] = (char *)j->etude;
    argv[k++] = "check";
    argv[k++] = "-s";
    argv[k++] = (char *)j->schemas;
    argv[k++] = "attempt";
    for (i = 0; i < j->nconsoles && i < LEGACY_CONSOLES; i++) {
        argv[k++] = "-k";
        argv[k++] = j->consoles[i];
    }
    argv[k++] = (char *)j->manifest;
    argv[k++] = (char *)j->case_id;
    argv[k++] = (char *)j->arch;
    argv[k++] = (char *)j->placement;
    argv[k++] = (char *)j->output;
    argv[k++] = (char *)j->run_id;
    argv[k++] = (char *)j->attempt_id;
    argv[k++] = (char *)j->item_id;
    argv[k++] = (char *)j->end;
    if (j->value >= 0) {
        snprintf(value, sizeof(value), "%d", j->value);
        argv[k++] = value;
    }
    argv[k] = NULL;
    fflush(NULL);
#ifdef _WIN32
    windows_judge(j, argv, out);
#else
    if ((pid = judge_start(j, argv)) < 0)
        snprintf(out->reason, sizeof(out->reason), "etude check could not start: %s",
                 strerror(errno));
    else if (judge_wait(pid, &status) != 0)
        snprintf(out->reason, sizeof(out->reason), "etude check did not end within %u s",
                 LEGACY_JUDGE_S);
    else
        judge_take(j, status, out);
#endif
}

int legacy_absolute(const char *path)
{
#ifdef _WIN32
    return etude_win_absolute(path);
#else
    return path != NULL && path[0] == '/';
#endif
}

/** Fix one manifest bound from the eligible samples supplied by the runner. */
static int learned_bound(const struct json *object, const uint64_t *samples, size_t n,
                         uint64_t *out)
{
    struct etude_bound b;
    struct etude_bound_fixed fixed;
    b.floor_us = (uint64_t)count_of(object, "floor_us");
    b.initial_us = (uint64_t)count_of(object, "initial_us");
    b.ceiling_us = (uint64_t)count_of(object, "ceiling_us");
    if (etude_bound_fix(&b, samples, n, &fixed) != 0)
        return -1;
    *out = fixed.us;
    return 0;
}

int legacy_case_learn(struct legacy_case *k, const uint64_t *elapsed, size_t n,
                      const uint64_t *gaps, size_t ngaps)
{
    const struct json *bounds = json_member(k->json, "bounds");
    if (learned_bound(json_member(bounds, "case"), elapsed, n, &k->case_us) != 0 ||
        learned_bound(json_member(bounds, "gap"), gaps, ngaps, &k->gap_us) != 0)
        return -1;
    return 0;
}
