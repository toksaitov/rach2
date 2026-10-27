/*
 * launch.c: `etude launch`, the installed entry to shared admission. It
 * reads the site policy, sizes the claim of a build or an old runner's test
 * run from its command, and runs the command through `etude admit` of the
 * program the policy names. tools/etude/Readme.md, Installed launch
 * admission, gives the rules.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include "win/files.h"
#else
#include <pwd.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include "authority_platform.h"
#include "launch.h"
#include "verbs.h"

#ifdef _WIN32
#define LAUNCH_SITE      "C:/ProgramData/rach2/etude-site"
#define LAUNCH_ENDPOINT  "windows"
#define LAUNCH_SEPARATOR ";"
#elif defined(__APPLE__)
#define LAUNCH_SITE      "/etc/rach2/etude-site"
#define LAUNCH_ENDPOINT  "macos"
#define LAUNCH_SEPARATOR ":"
#else
#define LAUNCH_SITE      "/etc/rach2/etude-site"
#define LAUNCH_ENDPOINT  "linux"
#define LAUNCH_SEPARATOR ":"
#endif

#define MIB         1048576ull
#define GIB         1073741824ull
#define BUILD_DISK  (40 * GIB) /* a root and its images, the GNU root's among them */
#define BUILD_FLOOR (5 * GIB)  /* the least memory a claim that prepares takes */

/* The fields of a site policy, in the order of `field_names`. */
enum {
    HEADER,
    ENDPOINT,
    LAUNCHER,
    LEDGER,
    LOCKS,
    ETUDE,
    CAPACITY,
    WAIT,
    SECONDS,
    BOUNDARY,
    FIELDS
};

static const char *const field_names[FIELDS] = {"etude-site", "endpoint",     "launcher", "ledger",
                                                "locks",      "etude",        "capacity", "wait",
                                                "seconds",    "boundary_lazy"};

/* One launch: its command, the policy it is admitted under and its claim. */
struct launch {
    const char *result; /* the file -r names, or NULL */
    const char *kind;   /* build, legacy or alone */
    char **command;     /* the command, which ends with a null pointer */
    int ncommand;
    const char *site;            /* the policy's path */
    char *text;                  /* the policy's bytes, which `field` points into */
    const char *field[FIELDS];   /* each field's value, or NULL */
    uint64_t capacity[3];        /* thousandths of a processor, memory and disk bytes */
    uint64_t workers, pool;      /* RACH2_WORKERS, and RACH2_MEMORY_MIB in MiB */
    uint64_t jobs, make_jobs;    /* a build's job count and make's own, or 0 */
    uint64_t processors, memory; /* the claim, in the units of `capacity` */
    int alone;
};

/** Refuse the launch: report why, write `refused` to the -r file, give 2. */
static int refuse(const struct launch *l, const char *format, ...)
{
    va_list ap;
    FILE *f;
    fputs("etude-launch: result: refused: ", stderr);
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (l->result && (f = fopen(l->result, "w")) != NULL) {
        fputs("refused\n", f);
        fclose(f);
    }
    return 2;
}

/** Give the last component of `path`, after its last slash. */
static const char *base(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/** Say whether the string `s` ends with `tail`. */
static int ends(const char *s, const char *tail)
{
    size_t n = strlen(s), m = strlen(tail);
    return n >= m && !strcmp(s + n - m, tail);
}

/** Read a positive number of at most `digits` digits, or give -1. */
static int amount(const char *text, size_t digits, uint64_t *value)
{
    if (!launch_number(text) || strlen(text) > digits)
        return -1;
    *value = strtoull(text, NULL, 10);
    return 0;
}

/** Read the number `text`, which the message calls `name`, or refuse. */
static int positive(const struct launch *l, const char *text, const char *name, uint64_t *value)
{
    return amount(text, 15, value) ? refuse(l, "invalid %s", name) : 0;
}

/** Add or multiply, holding a result past 2^64 at the largest value. */
static uint64_t sum(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

static uint64_t product(uint64_t a, uint64_t b)
{
    return b && a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

/** Give the variable `name`, or `fallback` when it is unset or empty. */
static const char *setting(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return value && *value ? value : fallback;
}

/** Export `value` as `name`, or refuse. */
static int export(const struct launch *l, const char *name, const char *value)
{
    return setenv(name, value, 1) ? refuse(l, "cannot set %s", name) : 0;
}

/** Export the number `value` as `name`, or refuse. */
static int export_number(const struct launch *l, const char *name, uint64_t value)
{
    char text[32];
    snprintf(text, sizeof text, "%llu", (unsigned long long)value);
    return export(l, name, text);
}

/** Read the whole file `path`, with a null byte after it, or give NULL. */
static char *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    char *text = NULL, *grown;
    size_t n = 0, capacity = 0, got;
    if (!f)
        return NULL;
    do {
        if (n + 4096 + 1 > capacity) {
            capacity = n + 4096 + 1 + capacity;
            if (!(grown = realloc(text, capacity))) {
                free(text);
                fclose(f);
                return NULL;
            }
            text = grown;
        }
        got = fread(text + n, 1, 4096, f);
        n += got;
    } while (got);
    if (ferror(f)) {
        free(text);
        text = NULL;
    } else {
        text[n] = 0;
        *size = n;
    }
    fclose(f);
    return text;
}

/**
 * Read one line of the policy, as `read -r key value` splits it at spaces:
 * the first word is the field and the rest, without its outer spaces, the
 * value.
 */
static int site_line(struct launch *l, char *line, unsigned *seen)
{
    char *key = line + strspn(line, " "), *value = key + strcspn(key, " "), *end;
    size_t i;
    if (*value)
        *value++ = 0;
    value += strspn(value, " ");
    for (end = value + strlen(value); end > value && end[-1] == ' '; end--)
        ;
    *end = 0;
    for (i = 0; i < FIELDS && strcmp(key, field_names[i]); i++)
        ;
    if (i < FIELDS && (*seen & (1u << i)))
        return refuse(l, "duplicate field %s", key);
    if (!*value)
        return refuse(l, "empty field %s", key);
    if (i == FIELDS)
        return refuse(l, "unknown field %s", key);
    *seen |= 1u << i;
    l->field[i] = value;
    return 0;
}

/** Read the fields of the policy at `l->site`. */
static int site_fields(struct launch *l)
{
    size_t size;
    unsigned seen = 0;
    char *line, *end, *next;
    if (access(l->site, R_OK) || !(l->text = slurp(l->site, &size)))
        return refuse(l, "cannot read %s", l->site);
    for (line = l->text, end = l->text + size; line < end; line = next + 1) {
        next = memchr(line, '\n', (size_t)(end - line));
        if (!next)
            next = end;
        *next = 0;
        if (next == end && !line[strspn(line, " ")])
            break;
        if (site_line(l, line, &seen))
            return 2;
    }
    return 0;
}

/** Give a field's value, or an empty string for a field the policy lacks. */
static const char *field(const struct launch *l, int i)
{
    return l->field[i] ? l->field[i] : "";
}

/** Say whether `path` is a directory. */
static int directory(const char *path)
{
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

/** Say whether the first line of the ledger's claims is its format's. */
static int ledger_format(const char *ledger)
{
    char path[4200], line[64] = "";
    FILE *f;
    if (snprintf(path, sizeof path, "%s/claims", ledger) >= (int)sizeof path ||
        !(f = fopen(path, "rb")))
        return 0;
    if (!fgets(line, sizeof line, f))
        line[0] = 0;
    fclose(f);
    line[strcspn(line, "\n")] = 0;
    return !strcmp(line, "etude-ledger 1");
}

/** Read the capacity's three amounts. */
static int site_capacity(struct launch *l)
{
    const char *p = field(l, CAPACITY);
    char part[32];
    for (int i = 0; i < 3; i++) {
        size_t n = strcspn(p, ",");
        if (n >= sizeof part || (i < 2) != (p[n] == ','))
            return refuse(l, "capacity must give three positive amounts of at most 15 digits");
        memcpy(part, p, n);
        part[n] = 0;
        if (amount(part, 15, &l->capacity[i]))
            return refuse(l, "capacity must give three positive amounts of at most 15 digits");
        p += n + (i < 2);
    }
    return 0;
}

/** Check the policy as the launcher requires it, before any execution. */
static int site_check(struct launch *l)
{
    const char *paths[] = {l->site, field(l, LAUNCHER), field(l, LEDGER), field(l, LOCKS),
                           field(l, ETUDE)};
    uint64_t bound;
    if (strcmp(field(l, HEADER), "2"))
        return refuse(l, "incompatible site format");
    if (l->field[BOUNDARY] && strcmp(l->field[BOUNDARY], "bigmem-boundary-v1"))
        return refuse(l, "boundary_lazy must be bigmem-boundary-v1");
    if (strcmp(field(l, ENDPOINT), LAUNCH_ENDPOINT))
        return refuse(l, "endpoint is disabled or incompatible");
    for (size_t i = 0; i < sizeof paths / sizeof *paths; i++)
        if (!launch_absolute(paths[i]))
            return refuse(l, "not an absolute path: %s", paths[i]);
    if (access(field(l, ETUDE), X_OK) || access(field(l, LAUNCHER), X_OK) ||
        !directory(field(l, LOCKS)) || !directory(field(l, LEDGER)))
        return refuse(l, "installed endpoint is unavailable");
    if (!ledger_format(field(l, LEDGER)))
        return refuse(l, "ledger is unavailable or incompatible");
    if (site_capacity(l))
        return 2;
    if (amount(field(l, WAIT), 5, &bound) || bound > 86400 ||
        amount(field(l, SECONDS), 5, &bound) || bound > 86400)
        return refuse(l, "invalid time bound");
    return 0;
}

/** Say whether the checkout's suites read a reserved port offset. */
static int port_offsets(void)
{
    size_t size;
    char *text = slurp("tests/system/suites.mk", &size);
    int found = 0;
    for (size_t i = 0; text && i + 17 <= size && !found; i++)
        found = !memcmp(text + i, "RACH2_PORT_OFFSET", 17);
    free(text);
    return found;
}

/**
 * Run a command inside a claim that admitted the same site already. With
 * -r, the file gets `exited N`, or `incomplete` for a status of 128 or
 * more, which a signal gives.
 */
static int nested(struct launch *l)
{
    FILE *f;
    int status;
    if (strcmp(getenv("ETUDE_ADMITTED"), l->site))
        return refuse(l, "nested launch changed the site");
    if (!l->result)
        return launch_replace("etude-launch", l->command, 1);
    status = launch_child("etude-launch", l->command);
    if ((f = fopen(l->result, "w")) != NULL) {
        if (status >= 128)
            fputs("incomplete\n", f);
        else
            fprintf(f, "exited %d\n", status);
        fclose(f);
    }
    return status;
}

/** Compare two positive numbers written without leading zeros. */
static int compare(const char *a, const char *b)
{
    size_t n = strlen(a), m = strlen(b);
    return n != m ? (n > m ? 1 : -1) : strcmp(a, b);
}

/**
 * Count a build's jobs: at least JOBS, 4 by default, and more when a job
 * flag of the command or of MAKEFLAGS asks for more; an unbounded flag asks
 * for the site's processors. A build takes a processor and a GiB of memory
 * for each job, and a GiB more.
 */
static int build_resources(struct launch *l, char *const *argv)
{
    char unbounded[32];
    struct launch_jobs s = {0};
    const char *count;
    uint64_t jobs = 0, make_jobs = 0;
    int bad;
    snprintf(unbounded, sizeof unbounded, "%llu", (unsigned long long)(l->capacity[0] / 1000));
    s.unbounded = unbounded;
    s.count = "";
    s.jobs = setting("JOBS", "4");
    bad = launch_jobs_read(&s, argv);
    count = *s.count ? s.count : "1";
    bad = bad || !launch_number(s.jobs) || !launch_number(count) ||
          amount(compare(count, s.jobs) > 0 ? count : s.jobs, 15, &jobs) ||
          amount(count, 15, &make_jobs);
    launch_jobs_free(&s);
    if (bad || jobs > 1024)
        return refuse(l, "invalid build job count");
    l->jobs = jobs;
    l->make_jobs = make_jobs;
    l->processors = jobs * 1000;
    l->memory = (jobs + 1) * GIB;
    return 0;
}

/**
 * Claim for a run of RACH2_WORKERS workers whose guests take `guest` MiB in
 * all: half a processor for each of the RACH2_CPUS processors of a worker's
 * guest, and at least one, and the guests' memory with 1152 MiB a worker and
 * 1024 MiB more for the native checks beside them, and at least 5 GiB.
 */
static int run_share(struct launch *l, uint64_t guest)
{
    uint64_t cpus;
    if (positive(l, setting("RACH2_CPUS", "2"), "RACH2_CPUS", &cpus))
        return 2;
    if (cpus > 64)
        return refuse(l, "invalid RACH2_CPUS");
    l->processors = l->workers * cpus * 500;
    if (l->processors < l->workers * 1000)
        l->processors = l->workers * 1000;
    l->memory = product(sum(sum(guest, l->workers * 1152), 1024), MIB);
    if (l->memory < BUILD_FLOOR)
        l->memory = BUILD_FLOOR;
    return 0;
}

/** Say whether `program` is a shell whose script the launcher cannot read. */
static int shell(const char *program)
{
    const char *name = base(program);
    return !strcmp(name, "sh") || !strcmp(name, "bash") || !strcmp(name, "dash");
}

/**
 * Size a build. A goal `test` takes the host alone, and a build through a
 * shell claims at least what a run that fills the guest pool claims.
 */
static int build_claim(struct launch *l)
{
    uint64_t processors, memory;
    if (build_resources(l, l->command))
        return 2;
    for (int i = 0; i < l->ncommand; i++)
        if (!strcmp(l->command[i], "test"))
            l->alone = 1;
    if (!shell(l->command[0]))
        return 0;
    processors = l->processors;
    memory = l->memory;
    if (run_share(l, l->pool))
        return 2;
    if (l->processors < processors)
        l->processors = processors;
    if (l->memory < memory)
        l->memory = memory;
    return 0;
}

/** Give the machine's physical memory in bytes, or 0. */
static uint64_t physical_memory(void)
{
#ifdef _WIN32
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? (uint64_t)m.ullTotalPhys : 0;
#elif defined(__APPLE__)
    uint64_t memory = 0;
    size_t size = sizeof memory;
    return sysctlbyname("hw.memsize", &memory, &size, NULL, 0) ? 0 : memory;
#else
    long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    return pages > 0 && page > 0 ? (uint64_t)pages * (uint64_t)page : 0;
#endif
}

/** The suites a legacy run selected, once for each architecture they run on. */
struct selection {
    double *memory; /* each one's guest memory, in MiB */
    size_t n;
    int bad;    /* a memory is not a positive number */
    int serial; /* a suite's host isolation is `serial` */
};

/**
 * Add a selected suite whose registry gives `memory` and `isolation`. Its
 * memory and isolation are the first two words of the two fields joined by a
 * space, as awk splits the line it printed.
 */
static int select_suite(struct selection *s, const char *memory, const char *isolation)
{
    size_t size = strlen(memory) + strlen(isolation) + 2;
    char *line = malloc(size), *first, *second;
    double *grown = realloc(s->memory, (s->n + 1) * sizeof *grown);
    if (grown)
        s->memory = grown;
    if (!line || !grown) {
        free(line);
        return -1;
    }
    snprintf(line, size, "%s %s", memory, isolation);
    first = line + strspn(line, " \t");
    second = first + strcspn(first, " \t");
    if (*second)
        *second++ = 0;
    second += strspn(second, " \t");
    second[strcspn(second, " \t")] = 0;
    s->bad |= !launch_number(first);
    s->memory[s->n++] = strtod(first, NULL);
    s->serial |= !strcmp(second, "serial");
    free(line);
    return 0;
}

/* A line of tests/registry: name|tools|tier|images|memory|arches|isolation. */
struct suite {
    const char *field[7];
};

/**
 * Read the suites of the registry `text`, in order, skipping comments and
 * lines with no name, into `suites`, which has a place for each line.
 */
static size_t registry(char *text, struct suite *suites)
{
    size_t n = 0;
    for (char *line = text, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        for (int i = 0; i < 7; i++) {
            suites[n].field[i] = line;
            line += strcspn(line, "|");
            if (*line)
                *line++ = 0;
        }
        if (suites[n].field[0][0] != '#' && suites[n].field[0][0])
            n++;
    }
    return n;
}

/** Give the last line of the suite `name`, whose fields awk's arrays keep. */
static const struct suite *latest(const struct suite *suites, size_t n, const char *name)
{
    const struct suite *found = NULL;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(suites[i].field[0], name))
            found = &suites[i];
    return found;
}

/**
 * Say whether awk takes the field `text` as true: a number that is not zero,
 * or text that does not read as a number and is not empty.
 */
static int awk_true(const char *text)
{
    const char *p = text + strspn(text, " \t"), *q = p + (*p == '+' || *p == '-');
    char *end;
    if (((*q >= '0' && *q <= '9') || (*q == '.' && q[1] >= '0' && q[1] <= '9')) &&
        !(q[0] == '0' && (q[1] == 'x' || q[1] == 'X'))) {
        double value = strtod(p, &end);
        if (!end[strspn(end, " \t")])
            return value != 0;
    }
    return *text != 0;
}

/** Say whether `word` is one of the null-terminated `words`. */
static int named(char *const *words, const char *word)
{
    for (size_t i = 0; words[i]; i++)
        if (!strcmp(words[i], word))
            return 1;
    return 0;
}

/** Say whether a suite whose architectures are `list` runs on `arch`. */
static int runs_on(const char *list, const char *arch)
{
    char wrapped[4200], word[4200];
    if (!*list)
        return 1;
    snprintf(wrapped, sizeof wrapped, ",%s,", list);
    snprintf(word, sizeof word, ",%s,", arch);
    return strstr(wrapped, word) != NULL;
}

/** Copy the next word of `*p`, which a blank or a newline ends, to `word`. */
static int next_word(const char **p, char *word, size_t size)
{
    size_t n;
    *p += strspn(*p, " \t\n");
    n = strcspn(*p, " \t\n");
    if (!n || n >= size)
        return 0;
    memcpy(word, *p, n);
    word[n] = 0;
    *p += n;
    return 1;
}

/** Select the suites the request `words` name on each architecture of ARCH. */
static int select_suites(const struct suite *suites, size_t n, char *const *words,
                         struct selection *s)
{
    char arch[4096], qualified[8200];
    int all = named(words, "all"), core = named(words, "core");
    for (size_t i = 0; i < n; i++) {
        const struct suite *u = latest(suites, n, suites[i].field[0]);
        const char *arches = setting("ARCH", "i686");
        while (next_word(&arches, arch, sizeof arch)) {
            snprintf(qualified, sizeof qualified, "%s:%s", arch, u->field[0]);
            if (runs_on(u->field[5], arch) &&
                (named(words, u->field[0]) || named(words, qualified) || all ||
                 (core && !strcmp(u->field[2], "core"))) &&
                select_suite(s, awk_true(u->field[4]) ? u->field[4] : "128", u->field[6]))
                return -1;
        }
    }
    return 0;
}

/** Compare two amounts of memory, the larger first. */
static int descending(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

/**
 * Give the guest memory of a selection, in MiB: the sum of the largest
 * suites the workers can run at once, no more than the pool, and at least
 * the largest suite.
 */
static uint64_t guest_memory(struct selection *s, uint64_t workers, uint64_t pool)
{
    double total = 0, largest = 0;
    for (size_t i = 0; i < s->n; i++)
        if (s->memory[i] > largest)
            largest = s->memory[i];
    qsort(s->memory, s->n, sizeof *s->memory, descending);
    for (size_t i = 0; i < s->n && i < workers; i++)
        total += s->memory[i];
    if (total > (double)pool)
        total = (double)pool;
    if (total < largest)
        total = largest;
    return total >= 18446744073709551615.0 ? UINT64_MAX : (uint64_t)(total + 0.5);
}

/**
 * Size a selection: a suite larger than half the machine's memory, or a
 * serial one, takes the host alone, and a serial one runs with one worker.
 */
static int selection_claim(struct launch *l, struct selection *s)
{
    char text[32];
    uint64_t guest, half;
    if (!s->n)
        return refuse(l, "no suites selected");
    if (s->bad)
        return refuse(l, "invalid suite memory");
    guest = guest_memory(s, l->workers, l->pool);
    snprintf(text, sizeof text, "%llu", (unsigned long long)physical_memory());
    if (positive(l, text, "host memory", &half))
        return 2;
    half = half / 2 / MIB;
    for (size_t i = 0; i < s->n; i++)
        if (s->memory[i] > (double)half)
            l->alone = 1;
    if (s->serial) {
        l->alone = 1;
        l->workers = 1;
    }
    return run_share(l, guest);
}

/**
 * Join the arguments with newlines and split them at each newline, as awk
 * reads `printf '%s\n' "$@"`; no arguments give one empty word.
 */
static char **words_of(char *const *argv, int argc, char **text)
{
    size_t size = 1, n = 1;
    char **words;
    for (int i = 0; i < argc; i++)
        size += strlen(argv[i]) + 1;
    if (!(*text = malloc(size)))
        return NULL;
    **text = 0;
    for (int i = 0; i < argc; i++) {
        strcat(*text, argv[i]);
        if (i + 1 < argc)
            strcat(*text, "\n");
    }
    for (const char *p = *text; *p; p++)
        n += *p == '\n';
    if (!(words = calloc(n + 1, sizeof *words)))
        return NULL;
    n = 0;
    for (char *line = *text, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        words[n++] = line;
    }
    return words;
}

/** Size an old runner's suites, which `argv` names, from tests/registry. */
static int suites_claim(struct launch *l, char *const *argv, int argc)
{
    size_t size, lines = 1, n;
    char *text, *joined = NULL, **words;
    struct suite *suites;
    struct selection s = {0};
    int rc = 2;
    if (access("tests/registry", R_OK) || !(text = slurp("tests/registry", &size)))
        return refuse(l, "cannot read the suite registry");
    for (size_t i = 0; i < size; i++)
        lines += text[i] == '\n';
    suites = calloc(lines, sizeof *suites);
    words = words_of(argv, argc, &joined);
    n = suites ? registry(text, suites) : 0;
    if (!suites || !words || select_suites(suites, n, words, &s))
        refuse(l, "cannot select suites");
    else
        rc = selection_claim(l, &s);
    free(s.memory);
    free(words);
    free(joined);
    free(suites);
    free(text);
    return rc;
}

/**
 * Size a legacy command. `tests/run`, alone or through a shell, is sized
 * from the suites it names, and `tests/run inventory` as a build; any other
 * command claims what a run that fills the guest pool claims.
 */
static int legacy_claim(struct launch *l)
{
    char *const *argv = l->command;
    int argc = l->ncommand;
    char *build[] = {"true", NULL};
    if (shell(argv[0])) {
        argv++;
        argc--;
    }
    if (!argc || (strcmp(argv[0], "tests/run") && !ends(argv[0], "/tests/run")))
        return run_share(l, l->pool);
    argv++;
    argc--;
    if (argc && !strcmp(argv[0], "inventory"))
        return build_resources(l, build);
    return suites_claim(l, argv, argc);
}

/** Read RACH2_WORKERS and RACH2_MEMORY_MIB, 8 and 16384 by default. */
static int pools(struct launch *l)
{
    if (positive(l, setting("RACH2_WORKERS", "8"), "RACH2_WORKERS", &l->workers))
        return 2;
    if (l->workers > 1024)
        return refuse(l, "invalid RACH2_WORKERS");
    return positive(l, setting("RACH2_MEMORY_MIB", "16384"), "RACH2_MEMORY_MIB", &l->pool);
}

/** Say whether the directory `dir` has an executable make. */
static int has_make(const char *dir)
{
    char path[4300];
    snprintf(path, sizeof path, "%s/make", dir);
    if (!access(path, X_OK))
        return 1;
#ifdef _WIN32
    snprintf(path, sizeof path, "%s/make.exe", dir);
    return !access(path, X_OK);
#else
    return 0;
#endif
}

/**
 * Give the command the environment of an admitted build: the real make as
 * ETUDE_MAKE_REAL, the installed make wrapper, `etude-bin/make` beside the
 * launcher, first on PATH, and the job count as ETUDE_BUILD_JOBS and JOBS.
 */
static int build_environment(struct launch *l)
{
    const char *launcher = field(l, LAUNCHER), *path = getenv("PATH");
    char make[4096], real[4096], wrapper[4200], *search;
    size_t size;
    if (launch_find("make", make, sizeof make))
        return refuse(l, "make is unavailable");
    snprintf(real, sizeof real, "%s",
             ends(make, "/etude-bin/make") || ends(make, "/etude-bin/make.exe")
                 ? (getenv("ETUDE_MAKE_REAL") ? getenv("ETUDE_MAKE_REAL") : "")
                 : make);
    if (!launch_absolute(real))
        return refuse(l, "make is not an absolute path");
    snprintf(wrapper, sizeof wrapper, "%.*s/etude-bin", (int)(strrchr(launcher, '/') - launcher),
             launcher);
    if (!has_make(wrapper))
        return refuse(l, "installed make wrapper is unavailable");
    size = strlen(wrapper) + strlen(path ? path : "") + 2;
    if (!(search = malloc(size)))
        return refuse(l, "cannot set PATH");
    snprintf(search, size, "%s%s%s", wrapper, LAUNCH_SEPARATOR, path ? path : "");
    int rc = export(l, "ETUDE_MAKE_REAL", real) || export(l, "PATH", search) ||
             export_number(l, "ETUDE_BUILD_JOBS", l->jobs ? l->jobs : l->workers) ||
             export_number(l, "JOBS", l->jobs ? l->jobs : l->workers);
    free(search);
    return rc ? 2 : 0;
}

/** Name the account that runs etude. */
static int account(char *out, size_t size)
{
#ifdef _WIN32
    DWORD n = (DWORD)size;
    return GetUserNameA(out, &n) && *out ? 0 : -1;
#else
    struct passwd *pw = getpwuid(geteuid());
    if (!pw || !*pw->pw_name || strlen(pw->pw_name) >= size)
        return -1;
    strcpy(out, pw->pw_name);
    return 0;
#endif
}

/**
 * Run the command through `etude admit` of the policy's etude, as one claim
 * of the policy's ledger: the whole capacity when it takes the host alone,
 * and otherwise its own processors, memory and 40 GiB of disk, which must
 * fit the capacity. A build of make or gmake gets make's own job count.
 */
static int admit(struct launch *l)
{
    char resources[80], nonce[65], id[48], env[4096], name[256], jobs[32];
    char **args = calloc((size_t)l->ncommand + 24, sizeof *args);
    int n = 0, rc;
    if (!args)
        return refuse(l, "cannot make the command");
    snprintf(resources, sizeof resources, "%llu,%llu,%llu", (unsigned long long)l->processors,
             (unsigned long long)l->memory, (unsigned long long)BUILD_DISK);
    if (!l->alone && (l->processors > l->capacity[0] || l->memory > l->capacity[1] ||
                      BUILD_DISK > l->capacity[2])) {
        free(args);
        return refuse(l, "launch exceeds site capacity");
    }
    rc = export(l, "ETUDE_ADMITTED", l->site) ||
         export(l, "ETUDE_SITE", getenv("ETUDE_ADMITTED")) ||
         export_number(l, "RACH2_WORKERS", l->workers) ||
         export_number(l, "RACH2_MEMORY_MIB", l->pool);
    if (!rc && authority_nonce(nonce))
        rc = refuse(l, "cannot make an attempt identity");
    if (!rc && launch_find("env", env, sizeof env))
        rc = refuse(l, "env is unavailable");
    if (!rc && account(name, sizeof name))
        rc = refuse(l, "cannot name the account");
    if (rc) {
        free(args);
        return 2;
    }
    snprintf(id, sizeof id, "launch:%.32s", nonce);
    const char *fixed[] = {field(l, ETUDE), "admit", "-i",          "-p", "-d",
                           field(l, LOCKS), "-w",    field(l, WAIT)};
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        args[n++] = (char *)fixed[i];
    if (l->result) {
        args[n++] = "-r";
        args[n++] = (char *)l->result;
    }
    const char *claim[] = {field(l, LEDGER),
                           id,
                           name,
                           l->alone ? "alone" : "shared",
                           l->alone ? field(l, CAPACITY) : resources,
                           field(l, CAPACITY),
                           field(l, SECONDS),
                           env};
    for (size_t i = 0; i < sizeof claim / sizeof *claim; i++)
        args[n++] = (char *)claim[i];
    for (int i = 0; i < l->ncommand; i++)
        args[n++] = l->command[i];
    snprintf(jobs, sizeof jobs, "-j%llu", (unsigned long long)l->make_jobs);
    if (!strcmp(l->kind, "build") &&
        (!strcmp(base(l->command[0]), "make") || !strcmp(base(l->command[0]), "gmake")))
        args[n++] = jobs;
    rc = launch_replace("etude-launch", args, 0);
    free(args);
    return rc;
}

/** Size the claim of the launch's kind; `alone` takes the host alone. */
static int claim(struct launch *l)
{
    if (!strcmp(l->kind, "build"))
        return build_claim(l);
    if (!strcmp(l->kind, "legacy"))
        return legacy_claim(l);
    l->alone = 1;
    return 0;
}

/** Launch `l` under the site policy ETUDE_SITE names, or the installed one. */
static int launch(struct launch *l)
{
    const char *given = getenv("ETUDE_SITE"), *admitted = getenv("ETUDE_ADMITTED");
    struct stat st;
    l->site = given && *given ? given : LAUNCH_SITE;
    if (!given && lstat(l->site, &st))
        return launch_replace("etude-launch", l->command, 1);
    if (site_fields(l) || site_check(l))
        return 2;
    if (!strcmp(l->kind, "legacy") && !port_offsets())
        return refuse(l, "the checkout does not support reserved port offsets");
    if (admitted && *admitted)
        return nested(l);
    if (getenv("RACH2_PORT_OFFSET") || getenv("SSH_TEST_PORT"))
        return refuse(l, "port overrides bypass admission");
    if (pools(l) || claim(l) || build_environment(l))
        return 2;
    return admit(l);
}

/**
 * `etude launch [-r FILE] build|legacy|alone COMMAND [ARGS...]`: admit a
 * build, an old runner's command or a command that needs the host alone.
 * With no policy installed and ETUDE_SITE unset, it runs the command as it
 * is. -r names the file that gets the result `etude admit -r` writes, or
 * `refused`.
 */
int etude_launch_main(int argc, char **argv)
{
    struct launch l;
    int rc;
    memset(&l, 0, sizeof l);
    argc--;
    argv++;
    if (argc >= 3 && !strcmp(argv[0], "-r")) {
        l.result = argv[1];
        argc -= 2;
        argv += 2;
    }
    if (argc < 2 || !strcmp(argv[0], "-r")) {
        fputs("usage: etude launch [-r FILE] build|legacy|alone COMMAND [ARGS...]\n", stderr);
        return 2;
    }
    l.kind = argv[0];
    l.command = argv + 1;
    l.ncommand = argc - 1;
    if (strcmp(l.kind, "build") && strcmp(l.kind, "legacy") && strcmp(l.kind, "alone"))
        return refuse(&l, "unknown launch kind %s", l.kind);
    rc = launch(&l);
    free(l.text);
    return rc;
}
