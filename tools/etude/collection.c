/* collection.c: select and run native collections on prepared artifacts. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef _WIN32
#include <pwd.h>
#include <sys/statvfs.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#ifdef _WIN32
#include "win/files.h"
#endif
#include "check.h"
#include "hosts.h"
#include "posix/legacy_cases.h"
#include "etude/legacy.h"
#include "etude/portlock.h"
#include "verbs.h"
#include "measurement_context.h"
#include "collection_repeat.h"

#define COLLECTION_MAX 1024
#define WORDS_MAX      4096
#define PATH_SIZE      4096

struct suite_entry {
    char *name, *tools, *tier, *arches;
    char manifest[PATH_SIZE];
    struct json *doc;
    int selected, alone;
    uint64_t memory_mib;
};

struct collection {
    struct suite_entry suites[COLLECTION_MAX];
    size_t nsuites;
    struct etude_legacy_member members[COLLECTION_MAX * 2];
    size_t nmembers, selection_cases;
    char *wanted[WORDS_MAX];
    size_t nwanted;
    char *row_words[WORDS_MAX * 2];
    size_t nrow_words;
    char args[8192];
    unsigned char used[WORDS_MAX];
    char tree[PATH_SIZE], program[PATH_SIZE], work[PATH_SIZE];
    char results[PATH_SIZE], evidence[PATH_SIZE], schemas[PATH_SIZE];
    char record[PATH_SIZE], ledger[PATH_SIZE], locks[PATH_SIZE], status[PATH_SIZE];
    char account[128], site[PATH_SIZE], head[41];
    char outer[PATH_SIZE + 1]; /* ETUDE_ADMITTED as etude inherited it,
                                  before site_policy() sets it */
    const char *arches, *history, *commit, *context_path, *selection_export;
    struct etude_measurement_context *context;
    struct etude_collection_repeat repeat;
    unsigned width, cpus;
    uint64_t memory;
    uint64_t cap[3];
    /* Inside a launcher claim, the site's shared ledger and its processor
     * limit, where cases of a processor class claim their processors. */
    char site_ledger[PATH_SIZE];
    uint64_t site_cpu;
    int stop_on_fail, list, boots_machines;
    struct etude_portlock ports;
};

static volatile sig_atomic_t interrupted;

static void on_signal(int sig)
{
    interrupted = sig;
}

static int refused(const char *why)
{
    fprintf(stderr, "run: result: refused: %s\n", why);
    return 2;
}

static int copy_path(char *out, const char *path)
{
    size_t size = strlen(path);
    if (size >= PATH_SIZE)
        return -1;
    memmove(out, path, size + 1);
    return 0;
}

static int path_of(char *out, const char *root, const char *path)
{
    size_t base = strlen(root), tail = strlen(path);
    if (base > PATH_SIZE - 2 || tail > PATH_SIZE - base - 2)
        return -1;
    memmove(out, root, base);
    out[base++] = '/';
    memmove(out + base, path, tail + 1);
    return 0;
}

static int requested_path(char *out, const char *tree, const char *given, const char *fallback)
{
    const char *path = given ? given : fallback;
    return legacy_absolute(path) ? copy_path(out, path) : path_of(out, tree, path);
}

static int mkdirs(const char *path)
{
    char copy[PATH_SIZE];
    size_t i;
    if (strlen(path) >= sizeof(copy))
        return -1;
    strcpy(copy, path);
    for (i = 1; copy[i]; i++) {
        if (copy[i] != '/' && copy[i] != '\\')
            continue;
        if (i == 2 && copy[1] == ':')
            continue;
        char saved = copy[i];
        copy[i] = 0;
        if (mkdir(copy, 0700) != 0 && errno != EEXIST)
            return -1;
        copy[i] = saved;
    }
    return mkdir(copy, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

static uint64_t positive(const char *text)
{
    char *end;
    unsigned long long n;
    if (text == NULL || *text < '1' || *text > '9')
        return 0;
    errno = 0;
    n = strtoull(text, &end, 10);
    return errno || *end || n > JSON_INTEGER_MAX ? 0 : (uint64_t)n;
}

static uint64_t memory_bytes(const char *text)
{
    uint64_t mib = positive(text);
    return mib <= (uint64_t)JSON_INTEGER_MAX / 1048576 ? mib * 1048576 : 0;
}

static int contains_arch(const char *arches, const char *arch)
{
    const char *p;
    size_t n = strlen(arch);
    if (arches == NULL || !*arches)
        return 1;
    for (p = arches; *p;) {
        if (!strncmp(p, arch, n) && (p[n] == 0 || p[n] == ',' || p[n] == ' '))
            return 1;
        p += strcspn(p, ", ");
        p += strspn(p, ", ");
    }
    return 0;
}

static int runs_on(const struct json *k, const char *arch)
{
    const struct json *a = json_member(k, "architectures");
    size_t i;
    if (a == NULL)
        return 0;
    if (a->type == JSON_STRING)
        return !strcmp(a->string, "none");
    for (i = 0; a->type == JSON_ARRAY && i < a->count; i++)
        if (a->item[i]->type == JSON_STRING && !strcmp(a->item[i]->string, arch))
            return 1;
    return 0;
}

static int registry(struct collection *c)
{
    char path[PATH_SIZE], line[4096], *field[7], *p;
    FILE *f;
    size_t i;
    if (path_of(path, c->tree, "tests/registry") != 0 || (f = fopen(path, "r")) == NULL)
        return refused("cannot read tests/registry");
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        line[strcspn(line, "\r\n")] = 0;
        p = line;
        for (i = 0; i < 7; i++) {
            field[i] = p;
            p = strchr(p, '|');
            if (!p) {
                while (++i < 7)
                    field[i] = "";
                break;
            }
            *p++ = 0;
        }
        if (c->nsuites == COLLECTION_MAX || !*field[0] || !*field[2]) {
            fclose(f);
            return refused("malformed suite registry");
        }
        struct suite_entry *s = &c->suites[c->nsuites++];
        s->name = strdup(field[0]);
        s->tools = strdup(field[1]);
        s->tier = strdup(field[2]);
        s->arches = strdup(field[5]);
        s->alone = !strcmp(field[6], "serial");
        s->memory_mib = *field[4] ? positive(field[4]) : 128;
        if (!s->name || !s->tools || !s->tier || !s->arches) {
            fclose(f);
            return refused("out of memory");
        }
    }
    int bad = ferror(f);
    fclose(f);
    return bad ? refused("cannot read suite registry whole") : 0;
}

/* Append `n` bytes of `text` to `out`, which has `size` bytes, as far as
 * they fit. */
static void append(char *out, size_t size, const char *text, size_t n)
{
    size_t used = strlen(out);
    if (used + 1 >= size)
        return;
    if (n > size - used - 1)
        n = size - used - 1;
    memcpy(out + used, text, n);
    out[used + n] = 0;
}

/**
 * Find what the suite NAME proves: the comment of `mk`, the text of
 * tests/system/suites.mk, that begins with `tests/run NAME` in backticks,
 * read to the end of its first sentence, without that name. `out` is empty
 * when no comment names the suite.
 */
static void suite_proves(const char *mk, const char *name, char *out, size_t size)
{
    char head[256];
    const char *p = mk, *stop;
    size_t n, skip;
    int on = 0;
    out[0] = 0;
    snprintf(head, sizeof(head), "`tests/run %s`", name);
    while (p && *p) {
        const char *line = p;
        n = strcspn(p, "\n");
        p += n + (p[n] == '\n');
        if (line[0] != '#') {
            if (on)
                break;
            continue;
        }
        line++, n--;
        if (n && line[0] == ' ')
            line++, n--;
        if (!on) {
            if (n >= strlen(head) && !strncmp(line, head, strlen(head))) {
                on = 1;
                append(out, size, line, n);
            }
            continue;
        }
        if (n >= 11 && !strncmp(line, "`tests/run ", 11))
            break;
        append(out, size, " ", 1);
        append(out, size, line, n);
    }
    if (!on)
        return;
    if (name[strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789-")] == 0) {
        skip = strlen(head);
        skip += out[skip] == ':';
        skip += strspn(out + skip, " ");
        memmove(out, out + skip, strlen(out + skip) + 1);
    }
    if ((stop = strstr(out, ". ")) != NULL)
        out[stop - out + 1] = 0;
}

/**
 * Write the registry's tools field `tools` in words: `a,b+c/d+e` is "a,
 * either b and c or d and e", and `tooling` is etude's tooling machine.
 */
static void tools_prose(const char *tools, char *out, size_t size)
{
    const char *p = tools;
    out[0] = 0;
    while (*p) {
        char entry[512], words[1024] = "";
        size_t n = strcspn(p, ",");
        snprintf(entry, sizeof(entry), "/%.*s/", (int)n, p);
        p += n + (p[n] == ',');
        for (char *e = entry; *e;) {
            if (!strncmp(e, "/tooling/", 9)) {
                append(words, sizeof(words), "/etude's tooling machine/", 25);
                e += 9;
            } else {
                append(words, sizeof(words), e++, 1);
            }
        }
        words[strlen(words) - 1] = 0;
        int either = strchr(words + 1, '/') != NULL;
        if (out[0])
            append(out, size, ", ", 2);
        if (either)
            append(out, size, "either ", 7);
        for (char *w = words + 1; *w; w++) {
            if (*w == '+')
                append(out, size, " and ", 5);
            else if (*w == '/')
                append(out, size, " or ", 4);
            else
                append(out, size, w, 1);
        }
    }
}

/**
 * Print the suites as Markdown, one line each in the registry's order: the
 * tier, the tools, the most guest memory, the architectures of a suite that
 * does not run on both, and what it proves. tests/system/suites.md is this
 * output, and the style checks compare the two.
 */
static int inventory(const struct collection *c)
{
    char path[PATH_SIZE];
    char *mk;
    size_t length;
    if (path_of(path, c->tree, "tests/system/suites.mk") != 0)
        return refused("the path of tests/system/suites.mk is too long");
    mk = access(path, R_OK) == 0 ? read_file(path, &length) : NULL;
    puts("# The suites\n\nOne line per suite, in the order the pool starts them, from "
         "`tests/run inventory`: the tier, the external commands the suite needs, `qemu` being "
         "the emulator of the architecture `ARCH` names, the most guest memory its machines "
         "ask for at once, the architectures it runs on when it does not run on both, and what "
         "it proves, which is the sentence in `tests/system/suites.mk` that names it. "
         "`tests/run style` compares this file with that output.\n");
    for (size_t i = 0; i < c->nsuites; i++) {
        const struct suite_entry *s = &c->suites[i];
        char tools[2048] = "needs nothing beyond the build", memory[64] = "no machine";
        char arches[256] = "", proves[8192], machines[1024];
        if (strcmp(s->tools, "-")) {
            snprintf(tools, sizeof(tools), "needs ");
            tools_prose(s->tools, tools + 6, sizeof(tools) - 6);
        }
        snprintf(machines, sizeof(machines), ",%s,", s->tools);
        if (strstr(machines, ",qemu,") || strstr(machines, "qemu-system-"))
            snprintf(memory, sizeof(memory), "%llu MiB", (unsigned long long)s->memory_mib);
        if (s->arches[0]) {
            append(arches, sizeof(arches), "; ", 2);
            for (const char *a = s->arches; *a; a++)
                if (*a == ',')
                    append(arches, sizeof(arches), " and ", 5);
                else
                    append(arches, sizeof(arches), a, 1);
            append(arches, sizeof(arches), " only", 5);
        }
        suite_proves(mk, s->name, proves, sizeof(proves));
        if (!proves[0])
            snprintf(proves, sizeof(proves), "(no sentence in suites.mk names this suite)");
        printf("* `%s` (%s tier; %s; %s%s): %s\n", s->name, s->tier, tools, memory, arches, proves);
    }
    free(mk);
    return fflush(stdout) || ferror(stdout) ? 1 : 0;
}

static int arch_mask(const char *arches)
{
    return (contains_arch(arches, "i686") ? 1 : 0) | (contains_arch(arches, "x86_64") ? 2 : 0);
}

static int select_suites(struct collection *c, int argc, char **argv)
{
    size_t i;
    int set = argc == 1 && (!strcmp(argv[0], "all") || !strcmp(argv[0], "core"));
    if (argc == 1 && !strcmp(argv[0], "inventory")) {
        c->list = 1;
        return 0;
    }
    if (!argc)
        return refused("no suite named");
    if (set) {
        for (i = 0; i < c->nsuites; i++)
            if (!strcmp(argv[0], "all") || !strcmp(c->suites[i].tier, "core"))
                c->suites[i].selected = arch_mask(c->arches) & arch_mask(c->suites[i].arches);
        return 0;
    }
    for (int n = 0; n < argc; n++) {
        const char *name = argv[n], *colon = strchr(name, ':');
        int mask = arch_mask(c->arches);
        if (colon) {
            size_t length = (size_t)(colon - name);
            if (length == 4 && !strncmp(name, "i686", 4))
                mask &= 1;
            else if (length == 6 && !strncmp(name, "x86_64", 6))
                mask &= 2;
            else
                return refused("invalid architecture-qualified suite name");
            name = colon + 1;
        }
        for (i = 0; i < c->nsuites && strcmp(name, c->suites[i].name); i++) {
        }
        if (i == c->nsuites) {
            fprintf(stderr, "run: unknown suite '%s'\n", name);
            return 2;
        }
        mask &= arch_mask(c->suites[i].arches);
        if (!mask)
            return refused("a suite does not run on the selected architectures");
        if (c->suites[i].selected & mask)
            return refused("a suite is named twice");
        c->suites[i].selected |= mask;
    }
    return 0;
}

static int words(const char *text, char **out, size_t *n)
{
    char *copy, *word, *save;
    *n = 0;
    if (text == NULL)
        return 0;
    if ((copy = strdup(text)) == NULL)
        return -1;
    for (word = strtok_r(copy, " \t\r\n", &save); word; word = strtok_r(NULL, " \t\r\n", &save)) {
        if (*n == WORDS_MAX) {
            free(copy);
            return -1;
        }
        out[(*n)++] = strdup(word);
    }
    free(copy);
    return *n ? 0 : -1;
}

static int manifests(struct collection *c)
{
    char directory[PATH_SIZE], path[PATH_SIZE];
    DIR *d;
    struct dirent *e;
    size_t i, n;
    int status;
    if (path_of(directory, c->tree, "tests/cases") != 0 || (d = opendir(directory)) == NULL)
        return refused("cannot read tests/cases");
    while ((e = readdir(d)) != NULL) {
        n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 5, ".json"))
            continue;
        if (path_of(path, directory, e->d_name) != 0) {
            closedir(d);
            return 2;
        }
        for (i = 0; i < c->nsuites; i++) {
            struct suite_entry *s = &c->suites[i];
            size_t z = strlen(s->name);
            if (n <= z + 5 || e->d_name[n - z - 6] != '.' ||
                strncmp(e->d_name + n - z - 5, s->name, z))
                continue;
            if (!s->manifest[0] || strcmp(path, s->manifest) < 0)
                strcpy(s->manifest, path);
        }
    }
    closedir(d);
    for (i = 0; i < c->nsuites; i++) {
        struct suite_entry *s = &c->suites[i];
        if (!s->selected)
            continue;
        status = 0;
        if (!s->manifest[0] || (s->doc = read_json(s->manifest, &status)) == NULL)
            return refused("a suite has no readable case manifest");
        json_free(s->doc);
        s->doc = NULL;
        struct cases checked = {0};
        char *paths[] = {s->manifest};
        checked.schemas = c->schemas;
        if (load_manifests(&checked, paths, 1) != 0) {
            free_manifests(&checked);
            return 2;
        }
        s->doc = checked.m[0].doc;
        free(checked.m);
    }
    return 0;
}

static int selection_document(struct collection *c, struct suite_entry *s, const struct json *cases,
                              struct json **picked, size_t count)
{
    char path[PATH_SIZE], filename[PATH_SIZE];
    FILE *f;
    size_t j, w, length = strlen(s->name);
    if (length > sizeof(filename) - sizeof(".json"))
        return refused("case selection name is too long");
    memcpy(filename, s->name, length);
    memcpy(filename + length, ".json", sizeof(".json"));
    if (path_of(path, c->work, filename) != 0)
        return refused("case selection path is too long");
    struct json object = *s->doc, array = *cases;
    struct jbuf data = {0};
    object.item = calloc(object.count, sizeof(*object.item));
    if (!object.item) {
        return 2;
    }
    memcpy(object.item, s->doc->item, object.count * sizeof(*object.item));
    array.item = picked;
    array.count = count;
    for (j = 0; j < object.count; j++)
        if (object.item[j] == cases)
            object.item[j] = &array;
    json_canonical(&object, &data);
    f = fopen(path, "wb");
    int bad = f == NULL;
    if (f) {
        bad = fwrite(data.data, 1, data.len, f) != data.len;
        bad |= fclose(f) != 0;
    }
    jbuf_free(&data);
    free(object.item);
    if (bad) {
        return refused("cannot write case selection");
    }
    for (w = 0; w < 2; w++) {
        const char *arch = w ? "x86_64" : "i686";
        if (!(s->selected & (w ? 2 : 1)))
            continue;
        for (j = 0; j < count && !runs_on(picked[j], arch); j++) {
        }
        if (j == count)
            continue;
        struct etude_legacy_member *m = &c->members[c->nmembers++];
        m->suite = s->name;
        m->arch = arch;
        m->cases = strdup(path);
        m->serial = s->alone;
        m->stops = !strcmp(s->name, "style");
        for (j = 0; j < count; j++)
            if (runs_on(picked[j], arch) && strcmp(json_text(picked[j], "domain"), "host"))
                c->boots_machines = 1;
    }
    return 0;
}

struct case_selection {
    const struct json *cases;
    struct json **picked;
    const char *rows;
    char **ordered;
    size_t nrows, count;
};

static int selected_architecture(const struct suite_entry *s, const struct json *one)
{
    return ((s->selected & 1) && runs_on(one, "i686")) ||
           ((s->selected & 2) && runs_on(one, "x86_64"));
}

static int choose_identities(struct collection *c, const struct suite_entry *s,
                             const char *identities, struct case_selection *selection)
{
    size_t j, w;
    for (j = 0; j < selection->cases->count; j++) {
        const char *id = json_text(selection->cases->item[j], "case_id");
        int wanted = identities == NULL && selection->rows == NULL;
        for (w = 0; w < c->nwanted; w++)
            if (id && !strcmp(id, c->wanted[w])) {
                if (selection->rows) {
                    return refused("a suite is chosen from twice");
                }
                c->used[w] = 1;
                wanted = 1;
            }
        if (selection->rows)
            continue;
        if (!wanted)
            continue;
        if (!selected_architecture(s, selection->cases->item[j])) {
            if (identities == NULL)
                continue;
            return refused("a chosen case does not run on the selected architecture");
        }
        if (identities != NULL &&
            !etude_case_hosted(selection->cases->item[j], etude_host_system()))
            return refused("a chosen case does not run on this host");
        if (!etude_case_runs_here(selection->cases->item[j]))
            continue;
        selection->picked[selection->count++] = selection->cases->item[j];
    }
    return 0;
}

static int choose_rows(const struct suite_entry *s, struct case_selection *selection)
{
    size_t j, w;
    for (w = 0; w < selection->nrows; w++) {
        for (j = 0; j < w; j++)
            if (!strcmp(selection->ordered[j], selection->ordered[w])) {
                return refused("a row is repeated");
            }
        for (j = 0; j < selection->cases->count; j++) {
            const char *id = json_text(selection->cases->item[j], "case_id"),
                       *last = id ? strrchr(id, '.') : NULL;
            if (last && !strcmp(last + 1, selection->ordered[w]))
                break;
        }
        if (j == selection->cases->count || !selected_architecture(s, selection->cases->item[j])) {
            return refused("an unknown row or a row of another architecture was chosen");
        }
        if (!etude_case_hosted(selection->cases->item[j], etude_host_system()))
            return refused("a chosen row does not run on this host");
        selection->picked[selection->count++] = selection->cases->item[j];
    }
    return 0;
}

static const char *suite_rows(const struct suite_entry *s)
{
    const char *rows = !strcmp(s->name, "agents")         ? getenv("RACH2_AGENTS_ROWS")
                       : !strcmp(s->name, "agentsaccept") ? getenv("RACH2_ACCEPT_AGENTS")
                                                          : NULL;
    if (rows && !*rows)
        rows = NULL;
    return rows;
}

static int choose_cases(struct collection *c)
{
    const char *identities = getenv("RACH2_CASES");
    if (identities && !*identities)
        identities = NULL;
    size_t i, j, w;
    if (words(identities, c->wanted, &c->nwanted) != 0)
        return refused("RACH2_CASES names no case");
    for (i = 0; i < c->nwanted; i++)
        for (j = 0; j < i; j++)
            if (!strcmp(c->wanted[i], c->wanted[j]))
                return refused("a case is given twice");
    for (i = 0; i < c->nsuites; i++) {
        struct suite_entry *s = &c->suites[i];
        const struct json *cases;
        const char *rows;
        char *ordered[WORDS_MAX];
        size_t nrows = 0, count = 0;
        struct json **picked;
        if (!s->selected)
            continue;
        cases = json_member(s->doc, "cases");
        if (!cases || cases->type != JSON_ARRAY)
            return refused("manifest has no cases");
        rows = suite_rows(s);
        if (words(rows, ordered, &nrows) != 0)
            return refused("the rows name no case");
        for (w = 0; w < nrows; w++)
            c->row_words[c->nrow_words++] = ordered[w];
        picked = calloc(cases->count + 1, sizeof(*picked));
        if (!picked)
            return 2;
        struct case_selection selection = {cases, picked, rows, ordered, nrows, 0};
        int error = choose_identities(c, s, identities, &selection);
        if (!error)
            error = choose_rows(s, &selection);
        count = selection.count;
        if (error) {
            free(picked);
            return error;
        }
        if (!count) {
            free(picked);
            return refused("the selection leaves a suite with no case");
        }
        if (identities || rows)
            c->selection_cases += count;
        if (selection_document(c, s, cases, picked, count) != 0) {
            free(picked);
            return 2;
        }
        free(picked);
    }
    for (w = 0; w < c->nwanted; w++)
        if (!c->used[w])
            return refused("an identity is no case of a selected suite");
    return 0;
}

static void defaults(struct collection *c)
{
    uint64_t memory = UINT64_C(4) << 30;
    unsigned cpus = 4;
#ifdef _WIN32
    SYSTEM_INFO info;
    MEMORYSTATUSEX mem;
    GetSystemInfo(&info);
    cpus = info.dwNumberOfProcessors;
    memset(&mem, 0, sizeof(mem));
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem))
        memory = mem.ullTotalPhys;
#elif defined(__APPLE__)
    size_t size = sizeof(memory), nc = sizeof(cpus);
    sysctlbyname("hw.memsize", &memory, &size, NULL, 0);
    sysctlbyname("hw.ncpu", &cpus, &nc, NULL, 0);
#else
    long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    long processors = sysconf(_SC_NPROCESSORS_ONLN);
    if (pages > 0 && page > 0)
        memory = (uint64_t)pages * (uint64_t)page;
    if (processors > 0)
        cpus = (unsigned)processors;
#endif
    c->cpus = 2;
    c->width = cpus < 4 ? 4 : cpus;
    c->memory = memory / 4 * 3;
    c->arches = getenv("ARCH");
    if (!c->arches || !*c->arches)
        c->arches = "i686";
    if (getenv("RACH2_CPUS"))
        c->cpus =
            positive(getenv("RACH2_CPUS")) <= 64 ? (unsigned)positive(getenv("RACH2_CPUS")) : 0;
    if (getenv("RACH2_WORKERS"))
        c->width = positive(getenv("RACH2_WORKERS")) <= COLLECTION_MAX
                       ? (unsigned)positive(getenv("RACH2_WORKERS"))
                       : 0;
    if (getenv("RACH2_MEMORY_MIB"))
        c->memory = memory_bytes(getenv("RACH2_MEMORY_MIB"));
    c->stop_on_fail = getenv("RACH2_STOP_ON_FAIL") != NULL;
}

/* The installed ledger is authoritative; no shell launcher is involved. */
struct site_values {
    char capacity[160], endpoint[32];
    unsigned fields;
};

static int site_field(struct collection *c, struct site_values *values, const char *line,
                      const char *value)
{
    static const char *const keys[] = {"etude-site", "endpoint",     "launcher", "ledger",
                                       "locks",      "etude",        "capacity", "wait",
                                       "seconds",    "boundary_lazy"};
    size_t i;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]) && strcmp(line, keys[i]); i++) {
    }
    if (i == sizeof(keys) / sizeof(keys[0]) || !*value || (values->fields & (1u << i))) {
        return refused("unknown, empty or repeated site policy field");
    }
    values->fields |= 1u << i;
    if (i == 0 && strcmp(value, "2")) {
        return refused("incompatible site policy");
    }
    if (i == 1)
        snprintf(values->endpoint, sizeof(values->endpoint), "%s", value);
    if (i == 3)
        snprintf(c->ledger, sizeof(c->ledger), "%s", value);
    if (i == 4)
        snprintf(c->locks, sizeof(c->locks), "%s", value);
    if (i == 6)
        snprintf(values->capacity, sizeof(values->capacity), "%s", value);
    if ((i == 7 || i == 8) && (!positive(value) || positive(value) > 86400)) {
        return refused("invalid site wait or execution bound");
    }
    if (i == 9 && strcmp(value, "bigmem-boundary-v1")) {
        return refused("invalid site memory profile");
    }
    return 0;
}

static int read_site(struct collection *c, FILE *f, struct site_values *values)
{
    char line[PATH_SIZE + 64];
    while (fgets(line, sizeof(line), f)) {
        char *value = strchr(line, ' ');
        if (!value) {
            return refused("malformed installed site policy");
        }
        *value++ = 0;
        value[strcspn(value, "\r\n")] = 0;
        if (site_field(c, values, line, value) != 0)
            return 2;
    }
    return ferror(f) || (values->fields & 511) != 511 ? refused("incomplete installed site policy")
                                                      : 0;
}

static int site_policy(struct collection *c)
{
    const char *site = getenv("ETUDE_SITE");
    FILE *f;
    if (site == NULL) {
#ifdef _WIN32
        site = "C:/ProgramData/rach2/etude-site";
#else
        site = "/etc/rach2/etude-site";
#endif
    }
    f = fopen(site, "r");
    if (!f)
        return getenv("ETUDE_SITE") || errno != ENOENT
                   ? refused("installed site policy is unavailable")
                   : 0;
    struct site_values values = {{0}, {0}, 0};
    int error = read_site(c, f, &values);
    fclose(f);
    if (error)
        return error;
#ifdef _WIN32
    const char *os = "windows";
#elif defined(__APPLE__)
    const char *os = "macos";
#else
    const char *os = "linux";
#endif
    if (strcmp(values.endpoint, os))
        return refused("site endpoint is disabled or incompatible");
    char *p = values.capacity, *end;
    for (size_t i = 0; i < 3; i++) {
        if (*p < '1' || *p > '9')
            return refused("invalid site capacity");
        errno = 0;
        c->cap[i] = strtoull(p, &end, 10);
        if (errno || !c->cap[i] || c->cap[i] > JSON_INTEGER_MAX || *end != (i < 2 ? ',' : 0))
            return refused("invalid site capacity");
        p = end + 1;
    }
    if (!legacy_absolute(c->ledger) || !legacy_absolute(c->locks))
        return refused("site paths must be absolute");
    snprintf(c->site, sizeof(c->site), "%s", site);
#ifdef _WIN32
    _putenv_s("ETUDE_ADMITTED", site);
#else
    setenv("ETUDE_ADMITTED", site, 1);
#endif
    return 0;
}

/* Artifact production runs only when explicitly requested on a build host. */
static int prepare_collection(struct collection *c)
{
    struct etude_legacy_opts o;
    char **goals = calloc(4096, sizeof(*goals));
    size_t n = 0;
    int rc = 2;
    if (goals == NULL) {
        free(goals);
        return 2;
    }
    for (size_t i = 0; i < c->nmembers; i++) {
        struct legacy_manifest m;
        if (legacy_manifest_read(&m, c->tree, c->members[i].cases, c->members[i].suite,
                                 c->members[i].arch, getenv("RACH2_KERNEL")) != 0) {
            fprintf(stderr, "etude prepare: %s\n", m.why);
            legacy_manifest_free(&m);
            goto done;
        }
        for (size_t j = 0; j < m.ncases; j++)
            for (size_t k = 0; k < m.cases[j].ngoals; k++) {
                size_t z;
                for (z = 0; z < n && strcmp(goals[z], m.cases[j].goals[k]); z++) {
                }
                if (z < n)
                    continue;
                if (n == 4096) {
                    legacy_manifest_free(&m);
                    goto done;
                }
                goals[n] = strdup(m.cases[j].goals[k]);
                if (!goals[n]) {
                    legacy_manifest_free(&m);
                    goto done;
                }
                n++;
            }
        legacy_manifest_free(&m);
    }
    memset(&o, 0, sizeof(o));
    o.tree = c->tree;
    o.program = c->program;
    o.arch = c->arches;
    o.kernel = getenv("RACH2_KERNEL");
    o.record = c->record;
    o.seconds = ETUDE_LEGACY_SECONDS;
    o.goals = goals;
    o.ngoals = n;
    o.out = stdout;
    o.stop = &interrupted;
    rc = etude_legacy_prepare(&o);
done:
    for (size_t i = 0; i < n; i++)
        free(goals[i]);
    free(goals);
    return rc;
}

static int print_selection(struct collection *c)
{
    for (size_t i = 0; i < c->nmembers; i++) {
        const struct etude_legacy_member *m = &c->members[i];
        int status = 0;
        struct json *d = read_json(m->cases, &status);
        const struct json *a = json_member(d, "cases");
        for (size_t j = 0; a && j < a->count; j++) {
            const struct json *architectures = json_member(a->item[j], "architectures");
            int previous = 0;
            if (architectures && architectures->type == JSON_STRING)
                for (size_t k = 0; k < i; k++)
                    previous |= !strcmp(c->members[k].suite, m->suite);
            if (!previous && runs_on(a->item[j], m->arch))
                printf("%s %s %s\n", m->suite,
                       architectures && architectures->type == JSON_STRING ? "none" : m->arch,
                       json_text(a->item[j], "case_id"));
        }
        json_free(d);
    }
    return 0;
}

static int format_collection(struct collection *c, char *stream)
{
    char map[PATH_SIZE];
    if (stream[0] && access(stream, R_OK) == 0 && path_of(map, c->work, "map") == 0) {
        FILE *f = fopen(map, "w");
        if (!f)
            return refused("cannot write the result map");
        for (size_t i = 0; i < c->nsuites; i++)
            if (c->suites[i].selected)
                fprintf(f, "%s %s\n", c->suites[i].name, json_text(c->suites[i].doc, "family_id"));
        if (fclose(f) != 0)
            return refused("cannot write the result map");
        char *args[] = {c->program, "-s", c->schemas, "-a", (char *)c->arches, map, stream, NULL};
        optind = 1;
#ifdef __APPLE__
        optreset = 1;
#endif
        int formatted = etude_lines_main(7, args);
        if (formatted)
            return 1;
    }
    return 0;
}

/* Export the actual allocator's selection without starting its cases. */
static int export_selection(struct collection *c, const char *accel)
{
    struct etude_legacy_set_opts o;
    extern int etude_legacy_selection_export(const struct etude_legacy_set_opts *, const char *);
    memset(&o, 0, sizeof(o));
    o.repeat = c->repeat.seeded ? &c->repeat : NULL;
    o.tree = c->tree;
    o.arch = c->arches;
    o.program = c->program;
    o.account = c->account;
    o.members = c->members;
    o.nmembers = c->nmembers;
    o.native_locks = c->work;
    o.results = c->results;
    o.evidence = c->evidence;
    o.schemas = c->schemas;
    o.record = c->record;
    o.ledger = c->ledger;
    o.width = c->width;
    o.cpus = c->cpus;
    o.accel = accel;
    o.wait = 86400;
    o.seconds = ETUDE_LEGACY_SECONDS;
    o.kernel = getenv("RACH2_KERNEL");
    o.prepared = 1;
    o.stop_on_fail = c->stop_on_fail;
    memcpy(o.cap, c->cap, sizeof(o.cap));
    return etude_legacy_selection_export(&o, c->selection_export);
}

/* Say whether `a` and `b` are both regular files. */
static int regular_files(const char *a, const char *b)
{
    struct stat st;
    return stat(a, &st) == 0 && S_ISREG(st.st_mode) && stat(b, &st) == 0 && S_ISREG(st.st_mode);
}

/**
 * Set RACH2_OVMF_CODE, when it is unset, to the x86-64 UEFI firmware that
 * the cases booted by UEFI take, where tools/setup/ovmf_path.sh finds it:
 * QEMU's own beside the first directory of PATH that has it, or the
 * distribution's OVMF. A firmware is taken only with its variables file.
 */
static void find_firmware(void)
{
    static const char *const system[][2] = {
        {"/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"},
        {"/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_VARS.fd"}};
#ifdef _WIN32
    const char *separator = ";";
#else
    const char *separator = ":";
#endif
    const char *path = getenv("PATH"), *code = NULL;
    char found[PATH_SIZE], vars[PATH_SIZE];
    if (getenv("RACH2_OVMF_CODE") && *getenv("RACH2_OVMF_CODE"))
        return;
    for (const char *p = path; p && *p;) {
        int n = (int)strcspn(p, separator);
        int a = snprintf(found, sizeof(found), "%.*s/../share/qemu/edk2-x86_64-code.fd", n, p);
        int b = snprintf(vars, sizeof(vars), "%.*s/../share/qemu/edk2-i386-vars.fd", n, p);
        p += n + (p[n] != 0);
        if (a <= 0 || a >= (int)sizeof(found) || b <= 0 || b >= (int)sizeof(vars) ||
            !regular_files(found, found))
            continue;
        if (regular_files(found, vars))
            code = found;
        break;
    }
    for (size_t i = 0; !code && i < sizeof(system) / sizeof(system[0]); i++)
        if (regular_files(system[i][0], system[i][1]))
            code = system[i][0];
    if (!code)
        return;
#ifdef _WIN32
    _putenv_s("RACH2_OVMF_CODE", code);
#else
    setenv("RACH2_OVMF_CODE", code, 1);
#endif
}

/**
 * Apply the site's policy to the run and say whether the run is inside a
 * claim of the installed launcher.
 *
 * With an installed site, each case is a claim of the site's ledger, unless
 * ETUDE_ADMITTED named that site when etude started, before site_policy()
 * set it for the run's children: the run is then inside a claim of
 * the installed launcher, which holds the host lock and the port block
 * RACH2_PORT_OFFSET names, and its cases are claims of the run's own ledger
 * under the results, bounded by its workers, processors and memory.
 *
 * @return 1 inside a launcher claim, 0 outside one, or -1 after a refusal.
 */
static int run_admission(struct collection *c)
{
    char ledger[PATH_SIZE];
    uint64_t own[3];
    if (strlen(c->outer) >= PATH_SIZE) {
        refused("ETUDE_ADMITTED is too long");
        return -1;
    }
    memcpy(own, c->cap, sizeof(own));
    strcpy(ledger, c->ledger);
    if (site_policy(c) != 0)
        return -1;
    if (!c->site[0] || strcmp(c->outer, c->site))
        return 0;
    strcpy(c->site_ledger, c->ledger);
    c->site_cpu = c->cap[0];
    memcpy(c->cap, own, sizeof(own));
    strcpy(c->ledger, ledger);
    return 1;
}

/**
 * Find whether a selected case boots a machine, and whether a selected suite
 * needs the host alone.
 */
static void run_needs(const struct collection *c, int *machines, int *alone)
{
    *machines = *alone = 0;
    for (size_t i = 0; i < c->nsuites; i++) {
        const struct suite_entry *s = &c->suites[i];
        if (!s->selected)
            continue;
        *alone |= s->alone || s->memory_mib > c->cap[1] / 2 / 1048576;
        const struct json *a = json_member(s->doc, "cases");
        for (size_t j = 0; a && j < a->count; j++) {
            const char *domain = json_text(a->item[j], "domain");
            *machines |= domain && strcmp(domain, "host");
        }
    }
}

/**
 * Print the result line the set driver's status calls for and return the
 * run's status: 0 when every case passed, 1 when one failed, and 2 or 3 when
 * the set did not run.  `lines` is the formatter's status, nonzero when a
 * case failed.
 */
static int run_result(int rc, int lines)
{
    if (interrupted)
        return rc;
    if (rc == 2) {
        fputs("run: result: refused: etude did not run the set\n", stderr);
    } else if (rc == 3) {
        fputs("run: result: refused: the prepared artifacts failed their check\n", stderr);
    } else if (rc && !lines) {
        fprintf(stderr, "run: runner: the set driver exited with status %d\n", rc);
        fputs("run: result: failed: runner\n", stderr);
        rc = 1;
    } else if (lines && !rc) {
        rc = 1;
    }
    return rc;
}

/**
 * Run the selected cases on the prepared artifacts and print the run's
 * lines, ending with its result.  run_admission() says how the cases are
 * admitted.
 */
static int run_collection(struct collection *c, const char *accel, int dry)
{
    struct etude_legacy_set_opts o;
    int rc, machines, alone, enclosed;
    char stream[PATH_SIZE] = "";
    if (dry)
        return c->selection_export ? export_selection(c, accel) : print_selection(c);
    if (access(c->record, R_OK) != 0) {
        fprintf(stderr,
                "run: result: refused: prepared artifact record %s is unavailable; "
                "produce the selected artifacts before execution\n",
                c->record);
        return 2;
    }
    enclosed = run_admission(c);
    if (enclosed < 0)
        return 2;
    if (!c->commit && etude_context_head(c->tree, c->head) == 0)
        c->commit = c->head;
    find_firmware();
    run_needs(c, &machines, &alone);
    if (!c->locks[0]) {
        const char *dir = getenv("RACH2_PORT_DIR");
#ifdef _WIN32
        if (!dir)
            dir = etude_win_temp();
#else
        if (!dir)
            dir = "/tmp";
#endif
        snprintf(c->locks, sizeof(c->locks), "%s", dir);
    }
    if (!enclosed && (getenv("RACH2_PORT_OFFSET") || getenv("SSH_TEST_PORT")))
        return refused("port overrides bypass native admission");
    memset(&o, 0, sizeof(o));
    o.repeat = c->repeat.seeded ? &c->repeat : NULL;
    o.tree = c->tree;
    o.arch = c->arches;
    o.program = c->program;
    o.native_locks = enclosed ? NULL : c->locks;
    o.native_ports = machines;
    o.host_alone = alone;
    o.members = c->members;
    o.nmembers = c->nmembers;
    o.results = c->results;
    o.evidence = c->evidence;
    o.schemas = c->schemas;
    o.record = c->record;
    o.ledger = c->ledger;
    o.account = c->account;
    o.pool_memory_bytes = c->memory + ((uint64_t)c->width * 128 << 20);
    o.cpus = c->cpus;
    o.width = c->width;
    o.accel = accel;
    o.wait = 86400;
    o.seconds = ETUDE_LEGACY_SECONDS;
    o.context = c->context;
    o.run_id = etude_context_run_id(c->context);
    o.prepared = 1;
    o.history = c->history;
    o.commit = c->commit;
    o.args = c->args;
    o.status = c->status;
    o.stop = &interrupted;
    o.stop_on_fail = c->stop_on_fail;
    o.history_path = stream;
    o.history_path_size = sizeof(stream);
    o.site = c->site[0] ? c->site : NULL;
    o.site_ledger = enclosed && c->site_ledger[0] ? c->site_ledger : NULL;
    o.site_cpu = c->site_cpu;
    o.kernel = getenv("RACH2_KERNEL");
    memcpy(o.cap, c->cap, sizeof(o.cap));
    printf("run: starting: architecture %s, workers %u, accelerator %s", c->arches, c->width,
           accel);
    if (c->selection_cases)
        printf(", a selection of %lu case%s", (unsigned long)c->selection_cases,
               c->selection_cases == 1 ? "" : "s");
    putchar('\n');
    rc = etude_legacy_set(&o);
    return run_result(rc, format_collection(c, stream));
}

/* Prepare the selected artifacts, and run the cases on them when that
 * succeeded. */
static int prepare_and_run(struct collection *c, const char *accel)
{
    int rc = prepare_collection(c);
    if (rc == 0)
        return run_collection(c, accel, 0);
    if (interrupted)
        return rc;
    fputs(rc == 2 ? "run: result: refused: the preparation refused the request\n"
                  : "run: result: refused: preparation failed\n",
          stderr);
    return rc == 2 ? 2 : 1;
}

struct request {
    const char *tree, *results, *evidence, *record, *accel;
    int dry, build;
};

static int collection_options(struct collection *c, struct request *r, int argc, char **argv)
{
    int opt;
    optind = 1;
    while ((opt = getopt(argc, argv, "+t:a:o:e:p:j:M:H:g:A:C:I:J:R:S:L:kNB")) != -1) {
        switch (opt) {
        case 'R':
        case 'S':
        case 'L':
            if (etude_collection_repeat_option(&c->repeat, opt, optarg))
                return refused("invalid repetition seed, count or load");
            break;
        case 't':
            r->tree = optarg;
            break;
        case 'a':
            c->arches = optarg;
            break;
        case 'o':
            r->results = optarg;
            break;
        case 'e':
            r->evidence = optarg;
            break;
        case 'p':
            r->record = optarg;
            break;
        case 'j':
            c->width = positive(optarg) <= COLLECTION_MAX ? (unsigned)positive(optarg) : 0;
            break;
        case 'M':
            c->memory = memory_bytes(optarg);
            break;
        case 'H':
            c->history = optarg;
            break;
        case 'g':
            c->commit = optarg;
            break;
        case 'A':
            r->accel = optarg;
            break;
        case 'C':
            c->cpus = positive(optarg) <= 64 ? (unsigned)positive(optarg) : 0;
            break;
        case 'I':
            c->context_path = optarg;
            break;
        case 'J':
            c->selection_export = optarg;
            break;
        case 'k':
            c->stop_on_fail = 1;
            break;
        case 'N':
            r->dry = 1;
            break;
        case 'B':
            r->build = 1;
            break;
        default:
            return 2;
        }
    }
    if (!etude_collection_repeat_valid(&c->repeat))
        return refused("repetitions and controlled load require an explicit -S seed");
    if (!strcmp(c->arches, "i686 x86_64") || !strcmp(c->arches, "both"))
        c->arches = "i686,x86_64";
    if (strcmp(c->arches, "i686") && strcmp(c->arches, "x86_64") &&
        strcmp(c->arches, "i686,x86_64")) {
        refused("architectures must be i686, x86_64 or both");
        return 2;
    }
    if (!c->width || c->width > COLLECTION_MAX || !c->cpus || c->cpus > 64 || !c->memory) {
        refused("invalid workers, processors or memory limit");
        return 2;
    }
    if (!realpath(r->tree, c->tree) || !realpath(argv[0], c->program)) {
        refused("run etude by its path from a prepared tree");
        return 2;
    }
    return 0;
}

static int selection_workspace(struct collection *c)
{
    const char *tmp = getenv("TMPDIR");
#ifdef _WIN32
    if (!tmp)
        tmp = etude_win_temp();
#else
    if (!tmp)
        tmp = "/tmp";
#endif
    if (path_of(c->work, tmp, "rach2-collection.XXXXXX") != 0 || !mkdtemp(c->work)) {
        c->work[0] = 0;
        refused("cannot make selection workspace");
        return 2;
    }
    return 0;
}

static int collection_paths(struct collection *c, const struct request *r, int production)
{
    char status[64];
    int size = snprintf(status, sizeof(status), "build/.run.status.%ld", (long)getpid());
    if (requested_path(c->results, c->tree, r->results, "build/tests/results") ||
        requested_path(c->evidence, c->tree, r->evidence, "build/tests/failed") ||
        requested_path(c->record, c->tree, r->record, "build/tests/prepare.record") ||
        path_of(c->ledger, c->results, "ledger") || size < 0 || size >= (int)sizeof(status) ||
        path_of(c->status, c->tree, status))
        return refused("result, evidence, artifact record or status path is too long");
#ifdef _WIN32
    DWORD n = sizeof(c->account);
    if (!GetUserNameA(c->account, &n))
        strcpy(c->account, "windows");
#else
    struct passwd *pw = getpwuid(getuid());
    if (pw)
        snprintf(c->account, sizeof(c->account), "%s", pw->pw_name);
    else
        snprintf(c->account, sizeof(c->account), "uid%ld", (long)getuid());
#endif
    c->cap[0] = (uint64_t)c->width * c->cpus * 1000;
    c->cap[1] = c->memory + ((uint64_t)c->width * 128 << 20);
    c->cap[2] = UINT64_C(40) << 30;
#ifdef _WIN32
    ULARGE_INTEGER free_bytes;
    if (GetDiskFreeSpaceExA(c->tree, &free_bytes, NULL, NULL))
        c->cap[2] = free_bytes.QuadPart;
#else
    struct statvfs space;
    if (statvfs(c->tree, &space) == 0)
        c->cap[2] = (uint64_t)space.f_bavail * space.f_frsize;
#endif
    const char *floor = getenv("RACH2_MIN_FREE_GIB");
    uint64_t gib = !floor ? 40 : strcmp(floor, "0") ? positive(floor) : 0;
    if (floor && ((!gib && strcmp(floor, "0")) || gib > (UINT64_C(1) << 20))) {
        refused("RACH2_MIN_FREE_GIB must be a whole number of GiB");
        return 2;
    }
    if (!r->dry && (production || c->boots_machines) && c->cap[2] < (gib << 30)) {
        fprintf(stderr, "run: result: refused: the tree has less than %llu GiB free\n",
                (unsigned long long)gib);
        return 2;
    }
    if (!r->dry && (mkdirs(c->results) != 0 || mkdirs(c->evidence) != 0)) {
        refused("cannot make result directories");
        return 2;
    }
    return 0;
}

static const char *collection_accel(const char *accel)
{
    if (!accel || !*accel) {
#ifdef __linux__
        int kvm = open("/dev/kvm", O_RDWR);
        if (kvm < 0 && errno != ENOENT) {
            fprintf(stderr,
                    "run: result: refused: /dev/kvm cannot be opened for reading and writing: "
                    "%s; set RACH2_QEMU_ACCEL=tcg to run under the emulator\n",
                    strerror(errno));
            return NULL;
        }
        if (kvm >= 0)
            close(kvm);
        accel = kvm >= 0 ? "kvm" : "tcg";
#elif defined(__APPLE__) && defined(__x86_64__)
        int hv = 0;
        size_t size = sizeof(hv);
        accel = sysctlbyname("kern.hv_support", &hv, &size, NULL, 0) == 0 && hv ? "hvf" : "tcg";
#else
        accel = "tcg";
#endif
    }
    return accel;
}

/**
 * Run the collection again under `etude lock tree` with a run's hold, which
 * a later run takes over once nothing of this one runs, so that a cancelled
 * or killed run leaves no lock for a person to remove.
 *
 * @return the collection's status; -1 when the tree is already held for an
 *         ancestor and the caller goes on in this process; 2 without memory.
 */
static int hold_collection(struct collection *c, int argc, char **argv, int production)
{
    char *query[] = {c->program, "-q", c->tree, NULL};
    if (etude_lock_tree_main(3, query) == 0)
        return -1;
    size_t offset = production ? 6 : 5;
    char **command = calloc((size_t)argc + 7, sizeof(*command));
    if (!command)
        return 2;
    command[0] = c->program;
    command[1] = "-r";
    command[2] = c->tree;
    command[3] = "run";
    command[4] = c->program;
    command[5] = production ? "prepare" : "run";
    if (production)
        command[6] = "collection";
    for (int i = 1; i < argc; i++)
        command[i + offset] = argv[i];
    int rc = etude_lock_tree_main(argc + (int)offset, command);
    free(command);
    return rc;
}

static void collection_free(struct collection *c)
{
    if (c->work[0]) {
        legacy_remove_tree(c->work);
    }
    for (size_t i = 0; i < c->nsuites; i++) {
        free(c->suites[i].name);
        free(c->suites[i].tools);
        free(c->suites[i].tier);
        free(c->suites[i].arches);
        json_free(c->suites[i].doc);
    }
    for (size_t i = 0; i < c->nmembers; i++)
        free((char *)c->members[i].cases);
    for (size_t i = 0; i < c->nwanted; i++)
        free(c->wanted[i]);
    for (size_t i = 0; i < c->nrow_words; i++)
        free(c->row_words[i]);
    etude_context_close(c->context);
    free(c);
}

/**
 * Open the measurement context that `-I` names, bound to the tree, the
 * record, the site and the commit.  The site's policy is read here for the
 * site's path alone: the run's own capacity and ledger stay as they were, for
 * run_admission() to apply the policy once.
 */
static int collection_context(struct collection *c, int production, int dry)
{
    if (production || dry || !c->context_path)
        return 0;
    char error[256], ledger[PATH_SIZE];
    uint64_t own[3];
    memcpy(own, c->cap, sizeof(own));
    strcpy(ledger, c->ledger);
    int bad = site_policy(c);
    memcpy(c->cap, own, sizeof(own));
    strcpy(c->ledger, ledger);
    if (bad) {
        refused("the measurement context or a bound input is invalid");
        return -1;
    }
    const struct etude_context_inputs inputs = {c->tree, c->record, c->site, c->commit, c->program};
    if (etude_context_open(c->context_path, &inputs, &c->context, error, sizeof(error))) {
        refused("the measurement context or a bound input is invalid");
        return -1;
    }
    return 0;
}

/** Record the command's arguments in `c->args` for the run's history. */
static int collection_args(struct collection *c, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        size_t used = strlen(c->args);
        if (used + strlen(argv[i]) + 2 >= sizeof(c->args))
            return refused("arguments are too long");
        snprintf(c->args + used, sizeof(c->args) - used, "%s%s", i > 1 ? " " : "", argv[i]);
    }
    return 0;
}

/**
 * Prepare the selection, run it, or both, as the request asks, with the
 * signals that stop a run caught.
 */
static int collection_dispatch(struct collection *c, const struct request *r, int production)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifndef _WIN32
    signal(SIGHUP, on_signal);
#endif
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (production)
        return prepare_collection(c);
    if (r->build && !r->dry)
        return prepare_and_run(c, r->accel);
    return run_collection(c, r->accel, r->dry);
}

/** Print the line that ends a run a signal stopped, and return its status. */
static int collection_interrupted(int production)
{
    if (!production)
        fprintf(stderr, "run: result: interrupted by %s\n",
                interrupted == SIGINT    ? "int"
                : interrupted == SIGTERM ? "term"
                                         : "hup");
    return 128 + interrupted;
}

static int collection_main(int argc, char **argv, int production)
{
    struct collection *c = calloc(1, sizeof(*c));
    struct request r = {0};
    int rc = 2;
    if (!c)
        return refused("out of memory");
    if (argc < 2) {
        fputs("usage: etude run [-t TREE] [-a ARCH|both] [-p RECORD] [-o RESULTS] "
              "[-j WORKERS] [-M MIB] [-H HISTORY] [-A ACCEL] [-I CONTEXT] [-k] [-B] [-N [-J FILE]] "
              "[-S SEED [-R COUNT] [-L cpu:N|memory:BYTES|disk:BYTES]...] "
              "SUITE...|core|all|inventory\n",
              stderr);
        free(c);
        return 2;
    }
    r.tree = ".";
    r.results = getenv("RACH2_RESULTS");
    r.evidence = getenv("RACH2_FAILED");
    r.accel = getenv("RACH2_QEMU_ACCEL");
    snprintf(c->outer, sizeof(c->outer), "%s",
             getenv("ETUDE_ADMITTED") ? getenv("ETUDE_ADMITTED") : "");
    defaults(c);
    if (collection_args(c, argc, argv) != 0)
        goto done;
    if (collection_options(c, &r, argc, argv) != 0)
        goto done;
    if (c->selection_export && (production || !r.dry)) {
        refused("-J selection export requires -N on the native run entry");
        goto done;
    }
    if (production && r.build) {
        refused("-B is an option of the run entry");
        goto done;
    }
    if (path_of(c->schemas, c->tree, "tools/etude/schemas") != 0) {
        refused("schema path is too long");
        goto done;
    }
    if (registry(c) != 0 || select_suites(c, argc - optind, argv + optind) != 0)
        goto done;
    if (c->list) {
        rc = inventory(c);
        goto done;
    }
    if (manifests(c) != 0 || selection_workspace(c) != 0 || choose_cases(c) != 0)
        goto done;
    if (!r.dry) {
        int held = hold_collection(c, argc, argv, production);
        if (held >= 0) {
            rc = held;
            goto done;
        }
    }
    if (collection_paths(c, &r, production) != 0)
        goto done;
    r.accel = collection_accel(r.accel);
    if (!r.accel)
        goto done;
    if (collection_context(c, production, r.dry))
        goto done;
    rc = collection_dispatch(c, &r, production);
done:
    if (interrupted)
        rc = collection_interrupted(production);
    collection_free(c);
    return rc;
}

int etude_collection_main(int argc, char **argv)
{
    return collection_main(argc, argv, 0);
}

int etude_collection_prepare_main(int argc, char **argv)
{
    return collection_main(argc, argv, 1);
}
