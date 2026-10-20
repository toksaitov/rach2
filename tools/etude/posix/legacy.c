/*
 * legacy.c: the preparation and suite adapters on POSIX systems. The first
 * runs the preparation of tests/run one step at a time through the
 * executor, finds each suite's artifacts in make's database, checks that
 * they exist, and records them; the second runs one prepared suite under
 * runutil's supervision and judges how it ended.
 */
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#if defined(__APPLE__) && defined(__x86_64__)
#include <sys/sysctl.h>
#endif

#include "etude/exec.h"
#include "etude/legacy.h"
#include "posix.h"
#include "sha256.h"

extern char **environ;

/* The time from a step's termination signal to its kill. make removes the
 * target it was writing when it is stopped, and tests/run gave it as long. */
#define GRACE_US 5000000u

/* The bytes kept of a step's standard output and of its standard error
 * through the executor: the path, the preparation's tools, and whatever the
 * shell printed before its output went to the log. */
#define OUTPUT_MAX 65536

/* The lines of a failed step's log printed under its result, and the bytes
 * of the log's end read to find them. */
#define TAIL_LINES 20
#define TAIL_BYTES 8192

/* The first line of a record. */
#define RECORD_HEAD "etude-legacy 1"

/* The most commands one entry of a suite's tools may name. */
#define TOOL_MAX 255

/* A build step's output, and its errors, go to the log $1. */
static const char script_log[] = "exec >\"$1\" 2>&1; shift; exec \"$@\"";

/* make's database goes to the file $2, and its errors to the log. */
static const char script_file[] = "exec >\"$2\" 2>\"$1\"; shift 2; exec \"$@\"";

/* Standard output goes to the executor, which keeps it: a short answer, or
 * a suite's `child` records; standard error goes to the log $1. */
static const char script_answer[] = "exec 2>\"$1\"; shift; exec \"$@\"";

/* The path the steps search: the caller's, with e2fsprogs's directories
 * added where a package manager keeps them off, as tests/run adds them. */
static const char script_path[] = "exec 2>\"$1\"; [ ! -f tools/setup/e2fsprogs_path.sh ] || "
                                  ". tools/setup/e2fsprogs_path.sh; printf '%s\\n' \"$PATH\"";

/* The variables of make that a step does not inherit: a jobserver's
 * descriptors do not reach it, so each step's make is a make of its own. */
static const char *const dropped_env[] = {
    "MAKEFLAGS=", "MFLAGS=", "MAKELEVEL=", "LC_ALL=", "PATH="};

/* A list of strings from malloc. */
struct list {
    char **v;
    size_t n, cap;
};

/* A suite of the registry. */
struct suite {
    char *name;
    char *tools;  /* the commands it needs, `-` for none, empty when unknown */
    char *images; /* the globs of what it writes under the images, or empty */
    char *arches; /* the architectures it runs on, empty for both */
};

/* A file or target of make's database. */
struct node {
    char *name;
    size_t *deps; /* the indices of its prerequisites */
    size_t ndeps, cap;
    unsigned char target; /* a rule of the makefiles makes it */
    unsigned char phony;
};

/* make's database: its nodes, and a table of their indices by name. */
struct db {
    struct node *n;
    size_t len, cap;
    size_t *slot; /* each an index plus one, or 0 when empty */
    size_t nslot; /* a power of two, more than twice `len` */
};

/* The state of a reading of the database. */
struct parse {
    long cur;       /* the target whose recipe the tab lines are, or -1 */
    int not_target; /* the next entry is a file no rule makes */
    int in_define;  /* inside a multi-line variable's value */
};

/* One step: a command run under sh(1) from the tree's root. */
struct step {
    const char *name;   /* names its log, WORK/N-NAME.log */
    const char *script; /* where its output goes */
    const char *file;   /* the file of script_file, or NULL */
    char **cmd;         /* the command, ended by NULL */
    int capture;        /* nonzero: its standard output is its answer */
    char *answer;       /* that answer, its first line, once it passed */
};

/* One preparation under way. */
struct prep {
    const struct etude_legacy_opts *o;
    FILE *out;
    char *tree;
    const char *arch_build; /* the architecture's build tree */
    char *arch_var;         /* ARCH=..., given to every make of the suites */
    char *kernel;           /* the kernel the suites boot */
    char *kernel_var;       /* BOOT_KERNEL=..., or NULL for the default */
    char *work;
    char **env; /* the steps' environment */
    struct etude_exec *ex;
    int posix_only; /* the executor uses POSIX interfaces alone */
    unsigned steps; /* the steps begun, which number their logs */
    struct suite *reg;
    size_t nreg;
    const struct suite **chosen; /* the suites, in the order named */
    char **targets;              /* their targets, in that order */
    char *why;                   /* what failed, for the result line */
    struct db db;
    struct list goals;    /* the artifacts the last step builds */
    struct list required; /* every artifact the suites read */
    size_t files;         /* the regular files of `required` */
};

/* ---- Strings and lists. ---- */

/** Give a string from malloc made as printf makes it, or NULL. */
static char *format(const char *fmt, ...)
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

/** Add a copy of `s` to list `l`; 0, or -1 when memory ran out. */
static int list_add(struct list *l, const char *s)
{
    char **more;

    if (l->n + 1 >= l->cap) {
        more = realloc(l->v, (l->cap * 2 + 16) * sizeof(*more));
        if (more == NULL)
            return -1;
        l->v = more;
        l->cap = l->cap * 2 + 16;
    }
    if ((l->v[l->n] = strdup(s)) == NULL)
        return -1;
    l->v[++l->n] = NULL;
    return 0;
}

/** Free the strings of list `l` and the list. */
static void list_free(struct list *l)
{
    size_t i;

    for (i = 0; i < l->n; i++)
        free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof(*l));
}

/** Order two strings for qsort(3). */
static int by_text(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/** Sort list `l` and remove the strings it has twice. */
static void list_unique(struct list *l)
{
    size_t i, k = 0;

    if (l->n == 0)
        return;
    qsort(l->v, l->n, sizeof(*l->v), by_text);
    for (i = 1; i < l->n; i++) {
        if (strcmp(l->v[i], l->v[k]) == 0)
            free(l->v[i]);
        else
            l->v[++k] = l->v[i];
    }
    l->n = k + 1;
    l->v[l->n] = NULL;
}

/** Say whether `word` is one of the comma-separated words of `list`. */
static int listed(const char *list, const char *word)
{
    size_t n = strlen(word), len;

    while (*list != '\0') {
        len = strcspn(list, ",");
        if (len == n && strncmp(list, word, n) == 0)
            return 1;
        list += len + (list[len] == ',');
    }
    return 0;
}

/** Say whether `s` is letters, digits, `_` and `-`, and begins with neither. */
static int plain_name(const char *s)
{
    static const char allowed[] = "abcdefghijklmnopqrstuvwxyz"
                                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";

    return *s != '\0' && *s != '-' && *s != '_' && s[strspn(s, allowed)] == '\0';
}

/* ---- Results. ---- */

/** Print the result line of a request refused before anything was built. */
static int refuse(struct prep *p, const char *fmt, ...)
{
    va_list ap;

    fprintf(p->out, "etude-legacy: result: refused: ");
    va_start(ap, fmt);
    vfprintf(p->out, fmt, ap);
    va_end(ap);
    fprintf(p->out, "\n");
    return 2;
}

/** Note what failed, for the result line, and give the status 1. */
static int failed(struct prep *p, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (p->why != NULL)
        return 1;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n >= 0 && (p->why = malloc((size_t)n + 1)) != NULL) {
        va_start(ap, fmt);
        vsnprintf(p->why, (size_t)n + 1, fmt, ap);
        va_end(ap);
    }
    return 1;
}

/** Report an error of the system and give the status 2. */
static int sys_error(struct prep *p, const char *what, int err)
{
    fprintf(p->out, "etude-legacy: result: error: %s: %s\n", what, strerror(err));
    return 2;
}

/** Say whether a stop signal has come. */
static int stopped(const struct prep *p)
{
    return p->o->stop != NULL && *p->o->stop != 0;
}

/* ---- The registry and the tools. ---- */

/** Take one line of the registry as a suite; 0, or -1 when memory ran out. */
static int take_suite(struct prep *p, char *line)
{
    char *field[7] = {NULL, "", "", "", "", "", ""};
    struct suite *more, *s;
    int i;

    field[0] = line;
    for (i = 1; i < 7 && (line = strchr(line, '|')) != NULL; i++) {
        *line++ = '\0';
        field[i] = line;
    }
    if (i == 1)
        return 0;
    more = realloc(p->reg, (p->nreg + 1) * sizeof(*more));
    if (more == NULL)
        return -1;
    p->reg = more;
    s = &p->reg[p->nreg++];
    s->name = strdup(field[0]);
    s->tools = strdup(field[1]);
    s->images = strdup(field[3]);
    s->arches = strdup(field[5]);
    return s->name == NULL || s->tools == NULL || s->images == NULL || s->arches == NULL ? -1 : 0;
}

/** Read the tree's registry, tests/registry; 0, or 2 after a refusal. */
static int read_registry(struct prep *p)
{
    char *path = format("%s/tests/registry", p->tree), *line = NULL;
    size_t cap = 0;
    ssize_t len;
    FILE *f = path != NULL ? fopen(path, "r") : NULL;
    int bad = 0;

    if (f == NULL) {
        free(path);
        return refuse(p, "tests/registry cannot be read: %s", strerror(errno));
    }
    while (!bad && (len = getline(&line, &cap, f)) >= 0) {
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        if (line[0] != '#' && take_suite(p, line) != 0)
            bad = 1;
    }
    bad = bad || ferror(f);
    free(line);
    free(path);
    fclose(f);
    return bad ? refuse(p, "tests/registry cannot be read") : 0;
}

/** Give the suite of the registry named `name`, or NULL for none. */
static const struct suite *find_suite(const struct prep *p, const char *name)
{
    size_t i;

    for (i = 0; i < p->nreg; i++) {
        if (strcmp(p->reg[i].name, name) == 0 && p->reg[i].tools[0] != '\0')
            return &p->reg[i];
    }
    return NULL;
}

/** Give the first executable file `tool` in a directory of `path`, from
 * malloc, or NULL when there is none. */
static char *path_of(const char *tool, const char *path)
{
    struct stat st;
    char *file;
    size_t len;

    for (;;) {
        len = strcspn(path, ":");
        file = len == 0 ? format("./%s", tool) : format("%.*s/%s", (int)len, path, tool);
        if (file != NULL && stat(file, &st) == 0 && S_ISREG(st.st_mode) && access(file, X_OK) == 0)
            return file;
        free(file);
        if (path[len] == '\0')
            return NULL;
        path += len + 1;
    }
}

/** Say whether `tool` is an executable file in a directory of `path`. */
static int on_path(const char *tool, const char *path)
{
    char *file = path_of(tool, path);

    free(file);
    return file != NULL;
}

/** Say whether every command of `choice`, `len` bytes of names joined by
 * `+`, is on `path`; `qemu` stands for the architecture's emulator. */
static int choice_met(const char *choice, size_t len, const char *qemu, const char *path)
{
    char name[TOOL_MAX + 1];
    size_t n;

    while (len > 0) {
        n = strcspn(choice, "+");
        n = n < len ? n : len;
        if (n == 0 || n > TOOL_MAX)
            return 0;
        memcpy(name, choice, n);
        name[n] = '\0';
        if (!on_path(strcmp(name, "qemu") == 0 ? qemu : name, path))
            return 0;
        choice += n;
        len -= n;
        if (len > 0) {
            choice++;
            len--;
        }
    }
    return 1;
}

/**
 * Give the first entry of `tools`, the registry's form, that `path` does
 * not meet, from malloc, or NULL when every entry is met. An entry of
 * choices, `a+b/c+d`, is met when every command of one choice is there.
 */
static char *tools_missing(const char *tools, const char *qemu, const char *path)
{
    const char *choice;
    size_t len, n;
    int met;

    if (strcmp(tools, "-") == 0)
        return NULL;
    while (*tools != '\0') {
        len = strcspn(tools, ",");
        met = 0;
        for (choice = tools; !met && choice < tools + len; choice += n + 1) {
            n = strcspn(choice, "/,");
            met = choice_met(choice, n, qemu, path);
        }
        if (!met)
            return format("%.*s", (int)len, tools);
        tools += len + (tools[len] == ',');
    }
    return NULL;
}

/* ---- Admission. ---- */

/**
 * Say whether the tree's run lock is held for this process: the lock's
 * owner file names the process RACH2_TREE_HELD names, and that process runs.
 */
static int tree_held(const struct prep *p)
{
    const char *held = getenv("RACH2_TREE_HELD");
    char *path = format("%s/build/.run.lock/owner", p->tree), *line = NULL;
    long pid = held != NULL ? strtol(held, NULL, 10) : 0;
    size_t cap = 0;
    FILE *f = pid > 0 && path != NULL ? fopen(path, "r") : NULL;
    int ok = 0;

    while (f != NULL && !ok && getline(&line, &cap, f) >= 0)
        ok = strncmp(line, "pid ", 4) == 0 && strtol(line + 4, NULL, 10) == pid;
    if (f != NULL)
        fclose(f);
    free(line);
    free(path);
    return ok && (kill((pid_t)pid, 0) == 0 || errno == EPERM);
}

/** Take the architecture and the kernel; 0, or 2 after a refusal. */
static int admit_machine(struct prep *p)
{
    const struct etude_legacy_opts *o = p->o;
    const char *arch = o->arch != NULL ? o->arch : "i686";

    if (strcmp(arch, "i686") == 0)
        p->arch_build = "build";
    else if (strcmp(arch, "x86_64") == 0)
        p->arch_build = "build/x86_64";
    else
        return refuse(p, "%s: the architectures are i686 and x86_64", arch);
    if (o->kernel != NULL && !plain_name(o->kernel))
        return refuse(p, "%s: a kernel's name is letters, digits, '_' and '-'", o->kernel);
    p->arch_var = format("ARCH=%s", arch);
    if (o->kernel != NULL) {
        p->kernel = format("%s/rach2-%s/kernel", p->arch_build, o->kernel);
        p->kernel_var = p->kernel != NULL ? format("BOOT_KERNEL=%s", p->kernel) : NULL;
    } else {
        p->kernel = format("%s/rach2/kernel", p->arch_build);
    }
    if (p->arch_var == NULL || p->kernel == NULL || (o->kernel != NULL && p->kernel_var == NULL))
        return sys_error(p, "the request", ENOMEM);
    return 0;
}

/** Give the target whose closure is suite `name`'s artifacts: an e1000
 * suite's target runs its plain twin's with the second card. */
static char *target_of(const char *name)
{
    size_t n = strlen(name);

    if (n > 6 && strcmp(name + n - 6, "-e1000") == 0)
        return format("run-%.*s", (int)(n - 6), name);
    return format("run-%s", name);
}

/** Say whether `s` names a goal inside the tree: a relative path of
 * letters, digits, `.`, `_`, `+` and `-`, whose parts are not `.` or `..`. */
static int goal_name(const char *s)
{
    const char *part;
    size_t n;

    if (s[0] == '\0' || s[0] == '-' ||
        s[strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTU"
                    "VWXYZ0123456789._+-/")] != '\0')
        return 0;
    for (part = s; *part != '\0'; part += n + (part[n] == '/')) {
        n = strcspn(part, "/");
        if (n == 0 || (n == 1 && part[0] == '.') || (n == 2 && strncmp(part, "..", 2) == 0))
            return 0;
    }
    return 1;
}

/** Take the suites and the goals named; 0, or 2 after a refusal. */
static int admit_suites(struct prep *p)
{
    const struct etude_legacy_opts *o = p->o;
    const char *arch = p->arch_var + 5;
    size_t i, j;

    if (o->nsuites == 0 && o->ngoals == 0)
        return refuse(p, "no suite named");
    for (i = 0; i < o->ngoals; i++)
        if (!goal_name(o->goals[i]))
            return refuse(p, "%s: a goal is a relative path inside the tree", o->goals[i]);
    p->chosen = calloc(o->nsuites + 1, sizeof(*p->chosen));
    p->targets = calloc(o->nsuites + 1, sizeof(*p->targets));
    if (p->chosen == NULL || p->targets == NULL)
        return sys_error(p, "the request", ENOMEM);
    for (i = 0; i < o->nsuites; i++) {
        const struct suite *s = find_suite(p, o->suites[i]);

        if (s == NULL)
            return refuse(p, "unknown suite '%s'", o->suites[i]);
        for (j = 0; j < i; j++) {
            if (p->chosen[j] == s)
                return refuse(p, "%s is named twice", s->name);
        }
        if (s->arches[0] != '\0' && !listed(s->arches, arch))
            return refuse(p, "%s runs on %s alone, and the architecture is %s", s->name, s->arches,
                          arch);
        p->chosen[i] = s;
        if ((p->targets[i] = target_of(s->name)) == NULL)
            return sys_error(p, "the request", ENOMEM);
    }
    return 0;
}

/** Check the request before anything runs; 0, or 2 after a refusal. */
static int admit(struct prep *p)
{
    int status = admit_machine(p);

    if (status != 0)
        return status;
    p->tree = realpath(p->o->tree != NULL ? p->o->tree : ".", NULL);
    if (p->tree == NULL)
        return refuse(p, "%s: %s", p->o->tree != NULL ? p->o->tree : ".", strerror(errno));
    if ((status = read_registry(p)) != 0 || (status = admit_suites(p)) != 0)
        return status;
    if (!tree_held(p))
        return refuse(p, "the tree is not held: RACH2_TREE_HELD must name the running process "
                         "that build/.run.lock/owner names");
    return 0;
}

/* ---- The work directory and the steps' environment. ---- */

/** Say whether `var` is one of the inherited variables a step does not get. */
static int dropped(const char *var)
{
    size_t i;

    for (i = 0; i < sizeof(dropped_env) / sizeof(dropped_env[0]); i++) {
        if (strncmp(var, dropped_env[i], strlen(dropped_env[i])) == 0)
            return 1;
    }
    return 0;
}

/**
 * Make the steps' environment: this process's without the dropped
 * variables, with LC_ALL=C and the PATH `path`; 0, or -1 when memory ran
 * out.
 */
static int make_env(struct prep *p, const char *path)
{
    size_t n = 0, k = 0, i;
    char **env;

    while (environ[n] != NULL)
        n++;
    if ((env = calloc(n + 3, sizeof(*env))) == NULL)
        return -1;
    for (i = 0; i < n; i++) {
        if (!dropped(environ[i]) && (env[k++] = strdup(environ[i])) == NULL)
            break;
    }
    env[k] = i == n ? strdup("LC_ALL=C") : NULL;
    env[k + 1] = env[k] != NULL ? format("PATH=%s", path) : NULL;
    for (i = 0; p->env != NULL && p->env[i] != NULL; i++)
        free(p->env[i]);
    free(p->env);
    p->env = env;
    return env[k + 1] != NULL ? 0 : -1;
}

/** Make the work directory and the executor; 0, or 2 after an error. */
static int open_work(struct prep *p)
{
    const char *tmp = getenv("TMPDIR");
    const char *path = getenv("PATH");

    if (tmp == NULL || *tmp == '\0')
        tmp = "/tmp";
    if (p->o->work != NULL) {
        if (mkdir(p->o->work, 0700) != 0)
            return sys_error(p, p->o->work, errno);
        p->work = realpath(p->o->work, NULL);
    } else if ((p->work = format("%s/rach2-legacy.XXXXXX", tmp)) != NULL &&
               mkdtemp(p->work) == NULL) {
        int e = errno;

        free(p->work);
        p->work = NULL;
        return sys_error(p, "the work directory", e);
    }
    if (p->work == NULL)
        return sys_error(p, "the work directory", errno != 0 ? errno : ENOMEM);
    if (make_env(p, path != NULL ? path : "/usr/bin:/bin") != 0 ||
        (p->ex = etude_exec_new()) == NULL)
        return sys_error(p, "the steps", ENOMEM);
    return 0;
}

/**
 * Remove a tree, not following links; 0 once it is gone. A directory is
 * made writable and searchable before it is read.
 */
static int remove_tree(const char *path)
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
        if ((sub = format("%s/%s", path, e->d_name)) == NULL || remove_tree(sub) != 0)
            bad = -1;
        free(sub);
    }
    closedir(d);
    return rmdir(path) == 0 || errno == ENOENT ? bad : -1;
}

/* ---- Steps. ---- */

/** Print the last lines of a log, indented, so a failure reads without it. */
static void print_tail(FILE *out, const char *path)
{
    FILE *f = fopen(path, "r");
    char buf[TAIL_BYTES + 1];
    size_t n, start, lines = 0;
    long size;

    if (f == NULL)
        return;
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > TAIL_BYTES)
        fseek(f, size - TAIL_BYTES, SEEK_SET);
    else
        rewind(f);
    n = fread(buf, 1, TAIL_BYTES, f);
    fclose(f);
    buf[n] = '\0';
    if (n > 0 && buf[n - 1] == '\n')
        buf[--n] = '\0';
    for (start = n; start > 0; start--) {
        if (buf[start - 1] == '\n' && ++lines == TAIL_LINES)
            break;
    }
    while (start < n) {
        size_t len = strcspn(buf + start, "\n");

        fprintf(out, "    | %.*s\n", (int)len, buf + start);
        start += len + 1;
    }
}

/** Give the arguments that run step `s` under sh(1) with its log `log`. */
static char **step_argv(const struct step *s, const char *log)
{
    size_t n = 0, k = 0, i;
    char **argv;

    while (s->cmd[n] != NULL)
        n++;
    if ((argv = calloc(n + 7, sizeof(*argv))) == NULL)
        return NULL;
    argv[k++] = "/bin/sh";
    argv[k++] = "-c";
    argv[k++] = (char *)s->script;
    argv[k++] = "etude-legacy";
    argv[k++] = (char *)log;
    if (s->file != NULL)
        argv[k++] = (char *)s->file;
    for (i = 0; i < n; i++)
        argv[k++] = s->cmd[i];
    return argv;
}

/* How a step ended besides by itself. */
enum ended { ENDED_ITSELF, ENDED_BOUND, ENDED_STOP };

/* A log that is copied to the output as it grows. */
struct follow {
    const char *path;
    int fd; /* -1 until the log exists */
};

/** Copy what the log `f` has gained to the output. */
static void follow_log(struct prep *p, struct follow *f)
{
    char buf[8192];
    ssize_t n;

    if (f->fd < 0 && (f->fd = open(f->path, O_RDONLY)) < 0)
        return;
    while ((n = read(f->fd, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, (size_t)n, p->out);
    fflush(p->out);
}

/** Wait until the step `proc` finishes, cancelling it at its bound or at a
 * stop signal, and say which ended it; with `f`, copy that log to the
 * output meanwhile and once more at the end. */
static enum ended await_step(struct prep *p, struct etude_proc *proc, struct follow *f)
{
    uint64_t began = posix_now_us(), bound = (uint64_t)p->o->seconds * 1000000u, now;
    enum ended ended = ENDED_ITSELF;
    int wait_ms, done;

    if (bound == 0)
        bound = (uint64_t)ETUDE_LEGACY_SECONDS * 1000000u;
    for (;;) {
        now = posix_now_us();
        wait_ms = f != NULL ? 200 : 1000;
        if (ended == ENDED_ITSELF && stopped(p)) {
            ended = ENDED_STOP;
            etude_exec_cancel(proc);
        } else if (ended == ENDED_ITSELF && now - began >= bound) {
            ended = ENDED_BOUND;
            etude_exec_cancel(proc);
        } else if (ended == ENDED_ITSELF && began + bound - now < (uint64_t)wait_ms * 1000u) {
            wait_ms = (int)((began + bound - now) / 1000u) + 1;
        }
        done = etude_exec_wait(p->ex, wait_ms) == proc;
        if (f != NULL)
            follow_log(p, f);
        if (done)
            return ended;
    }
}

/** Describe how a step that did not pass ended, into `buf`. */
static void describe(const struct prep *p, const struct etude_exec_result *r, enum ended ended,
                     char *buf, size_t n)
{
    unsigned bound = p->o->seconds != 0 ? p->o->seconds : ETUDE_LEGACY_SECONDS;

    if (ended == ENDED_BOUND)
        snprintf(buf, n, "it ran past its bound of %u s", bound);
    else if (r->end == ETUDE_END_SIGNALED)
        snprintf(buf, n, "it was killed by signal %d", r->value);
    else if (r->end == ETUDE_END_EXITED && r->value != 0)
        snprintf(buf, n, "it exited %d", r->value);
    else if (r->end == ETUDE_END_EXITED && r->cleanup != ETUDE_CLEANUP_CONFIRMED)
        snprintf(buf, n, "its processes were not all seen to end");
    else if (r->end == ETUDE_END_EXITED)
        snprintf(buf, n, "its output passed %d bytes", OUTPUT_MAX);
    else
        snprintf(buf, n, "its end was not seen");
}

/** Judge a step that has finished; 0, 1 after a failure, or 128 plus the
 * signal after a stop. */
static int judge(struct prep *p, struct step *s, const struct etude_exec_result *r,
                 enum ended ended, const char *log)
{
    char why[128];
    int ok = ended == ENDED_ITSELF && r->end == ETUDE_END_EXITED && r->value == 0 &&
             r->cleanup == ETUDE_CLEANUP_CONFIRMED && r->out.total == r->out.kept;

    if (ended == ENDED_STOP) {
        fprintf(p->out, "etude-legacy: step: %s: stopped by signal %d\n", s->name,
                (int)*p->o->stop);
        return 128 + (int)*p->o->stop;
    }
    if (ok && s->capture && (s->answer = strdup(r->out.data != NULL ? r->out.data : "")) == NULL)
        return failed(p, "%s: memory ran out", s->name);
    if (ok && s->capture)
        s->answer[strcspn(s->answer, "\n")] = '\0';
    if (ok)
        return 0;
    describe(p, r, ended, why, sizeof(why));
    fprintf(p->out, "etude-legacy: step: %s: failed: %s; see %s\n", s->name, why, log);
    if (r->err.kept > 0)
        fprintf(p->out, "    | %s%s", r->err.data,
                r->err.data[r->err.kept - 1] == '\n' ? "" : "\n");
    print_tail(p->out, log);
    return failed(p, "%s: %s", s->name, why);
}

/**
 * Run step `s` through the executor from the tree's root, with its output
 * as its script directs and its log WORK/N-NAME.log.
 *
 * @return 0, 1 after a failure, or 128 plus the signal after a stop.
 */
static int run_step(struct prep *p, struct step *s)
{
    char *log = format("%s/%u-%s.log", p->work, ++p->steps, s->name);
    char **argv = log != NULL ? step_argv(s, log) : NULL;
    struct etude_exec_spec spec;
    struct etude_proc *proc = NULL;
    enum ended ended;
    int status;

    if (argv == NULL) {
        free(log);
        return failed(p, "%s: memory ran out", s->name);
    }
    if (stopped(p)) {
        free(argv);
        free(log);
        return 128 + (int)*p->o->stop;
    }
    fprintf(p->out, "etude-legacy: step: %s\n", s->name);
    fflush(p->out);
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = s->name;
    spec.argv = argv;
    spec.envp = p->env;
    spec.cwd = p->tree;
    spec.output_max = OUTPUT_MAX;
    spec.grace_us = GRACE_US;
    spec.posix_only = p->posix_only;
    if (etude_exec_start(p->ex, &spec, &proc) != 0) {
        status = failed(p, "%s: it did not start: %s", s->name, strerror(errno));
    } else {
        ended = await_step(p, proc, NULL);
        status = judge(p, s, etude_proc_result(proc), ended, log);
        etude_proc_free(proc);
    }
    free(argv);
    free(log);
    return status;
}

/**
 * Run `make -C TREE --no-print-directory ARCH=A [BOOT_KERNEL=K]` with the
 * `n` further arguments `args` as step `name` with `script`.
 */
static int run_make(struct prep *p, struct step *s, char *const *args, size_t n)
{
    size_t k = 0, i;
    int status;

    if ((s->cmd = calloc(n + 7, sizeof(*s->cmd))) == NULL)
        return failed(p, "%s: memory ran out", s->name);
    s->cmd[k++] = "make";
    s->cmd[k++] = "-C";
    s->cmd[k++] = p->tree;
    s->cmd[k++] = "--no-print-directory";
    s->cmd[k++] = p->arch_var;
    if (p->kernel_var != NULL)
        s->cmd[k++] = p->kernel_var;
    for (i = 0; i < n; i++)
        s->cmd[k++] = args[i];
    status = run_step(p, s);
    free(s->cmd);
    s->cmd = NULL;
    return status;
}

/** Run make with the goals `a` and `b`, NULL for none, as a build step. */
static int build_step(struct prep *p, const char *name, const char *a, const char *b)
{
    struct step s;
    char *args[2];
    size_t n = 0;

    memset(&s, 0, sizeof(s));
    s.name = name;
    s.script = script_log;
    if (a != NULL)
        args[n++] = (char *)a;
    if (b != NULL)
        args[n++] = (char *)b;
    return run_make(p, &s, args, n);
}

/* ---- The host's tools. ---- */

/** Find the path the steps search, and set it in their environment. */
static int find_path(struct prep *p)
{
    struct step s;
    char *cmd[] = {NULL};
    int status;

    memset(&s, 0, sizeof(s));
    s.name = "path";
    s.script = script_path;
    s.cmd = cmd;
    s.capture = 1;
    if ((status = run_step(p, &s)) != 0)
        return status;
    status = make_env(p, s.answer) == 0 ? 0 : failed(p, "path: memory ran out");
    free(s.answer);
    return status;
}

/** Give the PATH of the steps' environment. */
static const char *env_path(const struct prep *p)
{
    size_t i;

    for (i = 0; p->env[i] != NULL; i++) {
        if (strncmp(p->env[i], "PATH=", 5) == 0)
            return p->env[i] + 5;
    }
    return "";
}

/**
 * Check the tools of every suite and, for more than one, those of
 * `prepare-all`, which `make prepare-tools` prints; 0, 1 after a failed
 * step, 2 after a refusal, or 128 plus a stop signal.
 */
static int check_tools(struct prep *p)
{
    const char *qemu =
        strcmp(p->arch_build, "build") == 0 ? "qemu-system-i386" : "qemu-system-x86_64";
    char *args[] = {"-s", "prepare-tools"}, *missing;
    struct step s;
    size_t i;
    int status;

    for (i = 0; i < p->o->nsuites; i++) {
        if ((missing = tools_missing(p->chosen[i]->tools, qemu, env_path(p))) != NULL) {
            status = refuse(p, "%s needs %s, which this host does not have", p->chosen[i]->name,
                            missing);
            free(missing);
            return status;
        }
    }
    if (p->o->nsuites < 2)
        return 0;
    memset(&s, 0, sizeof(s));
    s.name = "prepare-tools";
    s.script = script_answer;
    s.capture = 1;
    if ((status = run_make(p, &s, args, 2)) != 0)
        return status;
    missing = tools_missing(s.answer, qemu, env_path(p));
    free(s.answer);
    if (missing == NULL)
        return 0;
    status = refuse(p,
                    "a set prepares every suite's artifacts, and needs %s, which this host "
                    "does not have",
                    missing);
    free(missing);
    return status;
}

/* ---- make's database. ---- */

/** Give the 64-bit FNV-1a hash of `s`. */
static uint64_t hash_of(const char *s)
{
    uint64_t h = 14695981039346656037u;

    while (*s != '\0')
        h = (h ^ (unsigned char)*s++) * 1099511628211u;
    return h;
}

/** Give the index of the node named `s`, or -1 for none. */
static long db_find(const struct db *d, const char *s)
{
    size_t i;

    if (d->nslot == 0)
        return -1;
    for (i = hash_of(s) & (d->nslot - 1); d->slot[i] != 0; i = (i + 1) & (d->nslot - 1)) {
        if (strcmp(d->n[d->slot[i] - 1].name, s) == 0)
            return (long)(d->slot[i] - 1);
    }
    return -1;
}

/** Give the first free entry on the path that `name` probes in the table
 * `slot` of `nslot` entries, a power of two. */
static size_t free_slot(const size_t *slot, size_t nslot, const char *name)
{
    size_t i = hash_of(name) & (nslot - 1);

    while (slot[i] != 0)
        i = (i + 1) & (nslot - 1);
    return i;
}

/** Make the table twice as large; 0, or -1 when memory ran out. */
static int db_grow(struct db *d)
{
    size_t nslot = d->nslot != 0 ? d->nslot * 2 : 1024, i;
    size_t *slot = calloc(nslot, sizeof(*slot));

    if (slot == NULL)
        return -1;
    for (i = 0; i < d->len; i++)
        slot[free_slot(slot, nslot, d->n[i].name)] = i + 1;
    free(d->slot);
    d->slot = slot;
    d->nslot = nslot;
    return 0;
}

/** Give the index of the node named `s`, added when it is new, or -1. */
static long db_intern(struct db *d, const char *s)
{
    long found = db_find(d, s);
    struct node *more;

    if (found >= 0)
        return found;
    if ((d->len + 1) * 2 >= d->nslot && db_grow(d) != 0)
        return -1;
    if (d->len == d->cap) {
        more = realloc(d->n, (d->cap * 2 + 1024) * sizeof(*more));
        if (more == NULL)
            return -1;
        d->n = more;
        d->cap = d->cap * 2 + 1024;
    }
    memset(&d->n[d->len], 0, sizeof(d->n[d->len]));
    if ((d->n[d->len].name = strdup(s)) == NULL)
        return -1;
    d->slot[free_slot(d->slot, d->nslot, s)] = d->len + 1;
    return (long)d->len++;
}

/** Add node `dep` to node `t`'s prerequisites; 0, or -1. */
static int db_dep(struct db *d, size_t t, size_t dep)
{
    struct node *n = &d->n[t];
    size_t *more;

    if (n->ndeps == n->cap) {
        more = realloc(n->deps, (n->cap * 2 + 8) * sizeof(*more));
        if (more == NULL)
            return -1;
        n->deps = more;
        n->cap = n->cap * 2 + 8;
    }
    n->deps[n->ndeps++] = dep;
    return 0;
}

/** Free the database. */
static void db_free(struct db *d)
{
    size_t i;

    for (i = 0; i < d->len; i++) {
        free(d->n[i].name);
        free(d->n[i].deps);
    }
    free(d->n);
    free(d->slot);
    memset(d, 0, sizeof(*d));
}

/**
 * Give the length of the target's name when `line` is a rule of the
 * database, `NAME: PREREQUISITES` or `NAME:: PREREQUISITES`, with the
 * prerequisites in `*rest`; 0 for any other line. A target's own variable,
 * `NAME: VAR := VALUE`, is not a rule.
 */
static size_t rule_name(char *line, char **rest)
{
    size_t n = strcspn(line, " \t:=");

    if (n == 0 || line[n] != ':')
        return 0;
    *rest = line + n + 1 + (line[n + 1] == ':');
    return strchr(*rest, '=') == NULL ? n : 0;
}

/** Add each word of `words` as a prerequisite of node `t`, all of them or,
 * with `prefix`, those that begin with it; 0, or -1. */
static int db_words(struct db *d, size_t t, char *words, const char *prefix)
{
    char *w;
    size_t n;
    long dep;

    for (w = words + strspn(words, " \t"); *w != '\0'; w += strspn(w, " \t")) {
        n = strcspn(w, " \t");
        if (w[n] != '\0')
            w[n++] = '\0';
        if (strcmp(w, "|") != 0 && (prefix == NULL || strncmp(w, prefix, strlen(prefix)) == 0)) {
            if ((dep = db_intern(d, w)) < 0 || db_dep(d, t, (size_t)dep) != 0)
                return -1;
        }
        w += n;
    }
    return 0;
}

/** Mark each prerequisite of `.PHONY` phony. */
static void db_phony(struct db *d)
{
    long t = db_find(d, ".PHONY");
    size_t i;

    for (i = 0; t >= 0 && i < d->n[t].ndeps; i++)
        d->n[d->n[t].deps[i]].phony = 1;
}

/**
 * Take one line of the database. The recipe of `prepare-all` makes its two
 * halves with a make of their own, so the words of its recipe that begin
 * with `prepare-` are its prerequisites too, as tests/run read them.
 *
 * @return 0, or -1 when memory ran out.
 */
static int db_line(struct db *d, struct parse *ps, char *line)
{
    char *rest;
    size_t n;
    long t;

    if (ps->in_define) {
        ps->in_define = strcmp(line, "endef") != 0;
        return 0;
    }
    if (line[0] == '\t')
        return ps->cur >= 0 && strcmp(d->n[ps->cur].name, "prepare-all") == 0
                   ? db_words(d, (size_t)ps->cur, line, "prepare-")
                   : 0;
    if (line[0] == '#') {
        ps->not_target = ps->not_target || strcmp(line, "# Not a target:") == 0;
        return 0;
    }
    ps->cur = -1;
    ps->in_define = strncmp(line, "define ", 7) == 0 || strncmp(line, "override define ", 16) == 0;
    if ((n = rule_name(line, &rest)) == 0) {
        ps->not_target = 0;
        return 0;
    }
    line[n] = '\0';
    if ((t = db_intern(d, line)) < 0)
        return -1;
    d->n[t].target = d->n[t].target || !ps->not_target;
    ps->not_target = 0;
    ps->cur = t;
    return db_words(d, (size_t)t, rest, NULL);
}

/** Read make's database, as `make -p` prints it, from `path`; 0, or -1. */
static int db_read(struct db *d, const char *path)
{
    struct parse ps = {-1, 0, 0};
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    FILE *f = fopen(path, "r");
    int bad = 0;

    if (f == NULL)
        return -1;
    while (!bad && (len = getline(&line, &cap, f)) >= 0) {
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        bad = db_line(d, &ps, line) != 0;
    }
    bad = bad || ferror(f);
    free(line);
    fclose(f);
    db_phony(d);
    return bad ? -1 : 0;
}

/**
 * Mark in `mark` every node reachable from the node named `root`, itself
 * included.
 *
 * @return 0, or -1 with errno ENOENT for a root the database lacks or
 *         ENOMEM.
 */
static int db_reach(const struct db *d, const char *root, unsigned char *mark)
{
    long r = db_find(d, root);
    size_t *queue, head = 0, tail = 0, i;
    const struct node *n;

    if (r < 0) {
        errno = ENOENT;
        return -1;
    }
    if ((queue = malloc(d->len * sizeof(*queue))) == NULL)
        return -1;
    memset(mark, 0, d->len);
    mark[r] = 1;
    queue[tail++] = (size_t)r;
    while (head < tail) {
        n = &d->n[queue[head++]];
        for (i = 0; i < n->ndeps; i++) {
            if (!mark[n->deps[i]]) {
                mark[n->deps[i]] = 1;
                queue[tail++] = n->deps[i];
            }
        }
    }
    free(queue);
    return 0;
}

/** Give the name `name` from the tree's root `tree` when it is an absolute
 * path inside the tree, as make's database may name one file both ways;
 * otherwise `name`. */
static const char *in_tree(const char *tree, const char *name)
{
    size_t n = strlen(tree);

    return strncmp(name, tree, n) == 0 && name[n] == '/' ? name + n + 1 : name;
}

/** Say whether node `n`, whose name from the tree's root is `name`, is an
 * artifact: a file under build/, or a target with a directory in its name;
 * a phony target is not. */
static int artifact(const struct node *n, const char *name)
{
    if (n->phony)
        return 0;
    return strncmp(name, "build/", 6) == 0 || (n->target && strchr(name, '/') != NULL);
}

/* ---- The artifacts. ---- */

/** Dump make's database into WORK/database and read it. */
static int read_database(struct prep *p)
{
    char *args[] = {"-pn", "prepare-tools"};
    char *file = format("%s/database", p->work);
    struct step s;
    int status;

    if (file == NULL)
        return failed(p, "database: memory ran out");
    memset(&s, 0, sizeof(s));
    s.name = "database";
    s.script = script_file;
    s.file = file;
    status = run_make(p, &s, args, 2);
    if (status == 0 && db_read(&p->db, file) != 0)
        status = failed(p, "database: %s cannot be read: %s", file, strerror(errno));
    free(file);
    return status;
}

/**
 * Add suite `i`'s artifacts to the ones required, by their names from the
 * tree's root, and those that `prepared` does not mark, NULL for none, to the
 * goals of the last step, by the names make knows them by; 0, or 1.
 */
static int take_artifacts(struct prep *p, size_t i, unsigned char *mark,
                          const unsigned char *prepared)
{
    const char *name;
    size_t k;
    int bad = 0;

    if (db_reach(&p->db, p->targets[i], mark) != 0)
        return errno == ENOENT ? failed(p, "%s: make's database has no target %s",
                                        p->chosen[i]->name, p->targets[i])
                               : failed(p, "artifacts: memory ran out");
    for (k = 0; k < p->db.len && !bad; k++) {
        name = in_tree(p->tree, p->db.n[k].name);
        if (!mark[k] || !artifact(&p->db.n[k], name))
            continue;
        bad = list_add(&p->required, name) != 0;
        if (!bad && (prepared == NULL || !prepared[k]))
            bad = list_add(&p->goals, p->db.n[k].name) != 0;
    }
    return bad ? failed(p, "artifacts: memory ran out") : 0;
}

/**
 * Find the artifacts every suite reads and those no step has built yet:
 * for one suite, all of its own, which its target would otherwise build as
 * it starts; for a set, those outside `prepare-all`'s, such as the GNU
 * system's images. The goals named are artifacts as well, and built.
 */
static int find_artifacts(struct prep *p)
{
    unsigned char *mark = malloc(p->db.len + 1), *prepared = NULL;
    size_t i;
    int status = mark == NULL ? failed(p, "artifacts: memory ran out") : 0;

    if (status == 0 && p->o->nsuites > 1) {
        prepared = malloc(p->db.len + 1);
        if (prepared == NULL)
            status = failed(p, "artifacts: memory ran out");
        else if (db_reach(&p->db, "prepare-all", prepared) != 0)
            status = failed(p, "make's database has no target prepare-all");
    }
    for (i = 0; status == 0 && i < p->o->nsuites; i++)
        status = take_artifacts(p, i, mark, prepared);
    for (i = 0; status == 0 && i < p->o->ngoals; i++)
        if (list_add(&p->required, p->o->goals[i]) != 0 || list_add(&p->goals, p->o->goals[i]) != 0)
            status = failed(p, "artifacts: memory ran out");
    if (status == 0 && list_add(&p->required, p->kernel) != 0)
        status = failed(p, "artifacts: memory ran out");
    list_unique(&p->goals);
    list_unique(&p->required);
    free(mark);
    free(prepared);
    return status;
}

/** Build the goals that no step has built, in one make. */
static int build_goals(struct prep *p)
{
    struct step s;
    char **args;
    int status;

    if (p->goals.n == 0)
        return 0;
    if ((args = calloc(p->goals.n + 1, sizeof(*args))) == NULL)
        return failed(p, "artifacts: memory ran out");
    args[0] = "-j";
    memcpy(args + 1, p->goals.v, p->goals.n * sizeof(*args));
    memset(&s, 0, sizeof(s));
    s.name = "artifacts";
    s.script = script_log;
    status = run_make(p, &s, args, p->goals.n + 1);
    free(args);
    return status;
}

/** Give the modification time of `st` in nanoseconds. */
static long long mtime_ns(const struct stat *st)
{
#if defined(__APPLE__)
    return (long long)st->st_mtimespec.tv_sec * 1000000000LL + st->st_mtimespec.tv_nsec;
#else
    return (long long)st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
#endif
}

/**
 * Hash the regular file at `path` into `hex`, with its status in `*st` as
 * it was opened.
 *
 * @return 0, or -1 with errno set, EISDIR for a directory and EINVAL for
 *         anything else but a regular file.
 */
static int hash_file(const char *path, struct stat *st, char hex[65])
{
    static unsigned char buf[65536];
    unsigned char sum[32];
    struct sha256 h;
    ssize_t n;
    int fd = open(path, O_RDONLY), e;

    if (fd < 0)
        return -1;
    if (fstat(fd, st) != 0) {
        e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    if (!S_ISREG(st->st_mode)) {
        close(fd);
        errno = S_ISDIR(st->st_mode) ? EISDIR : EINVAL;
        return -1;
    }
    sha256_init(&h);
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        sha256_update(&h, buf, (size_t)n);
    e = errno;
    close(fd);
    if (n < 0) {
        errno = e;
        return -1;
    }
    sha256_final(&h, sum);
    sha256_hex(sum, hex);
    return 0;
}

/** Give the path of artifact `name`, from malloc: a name of the tree is
 * taken from its root. */
static char *artifact_path(const char *tree, const char *name)
{
    return name[0] == '/' ? strdup(name) : format("%s/%s", tree, name);
}

/** Write the record's head to `f`. */
static void record_head(const struct prep *p, FILE *f)
{
    size_t i;

    fprintf(f, "%s\ntree %s\narch %s\nkernel %s\n", RECORD_HEAD, p->tree, p->arch_var + 5,
            p->kernel);
    for (i = 0; i < p->o->nsuites; i++)
        fprintf(f, "suite %s %s\n", p->chosen[i]->name, p->targets[i]);
}

/**
 * Check that every artifact required exists, and write the record of each
 * regular file to `f`: `file DIGEST SIZE INODE MTIME PATH`, its time in
 * nanoseconds. A directory is accepted and not recorded.
 */
static int hash_artifacts(struct prep *p, FILE *f)
{
    struct stat st;
    char hex[65], *path;
    size_t i, missing = 0;

    for (i = 0; i < p->required.n && !stopped(p); i++) {
        if ((path = artifact_path(p->tree, p->required.v[i])) == NULL)
            return failed(p, "artifacts: memory ran out");
        if (hash_file(path, &st, hex) == 0) {
            p->files++;
            fprintf(f, "file %s %llu %llu %lld %s\n", hex, (unsigned long long)st.st_size,
                    (unsigned long long)st.st_ino, mtime_ns(&st), p->required.v[i]);
        } else if (errno != EISDIR) {
            fprintf(p->out, "etude-legacy: missing: %s: %s\n", p->required.v[i],
                    errno == EINVAL ? "not a regular file" : strerror(errno));
            missing++;
        }
        free(path);
    }
    if (stopped(p)) {
        fprintf(p->out, "etude-legacy: artifacts: stopped by signal %d\n", (int)*p->o->stop);
        return 128 + (int)*p->o->stop;
    }
    return missing == 0 ? 0
                        : failed(p, "%lu of %lu artifacts are missing", (unsigned long)missing,
                                 (unsigned long)p->required.n);
}

/**
 * Check the artifacts and write the record: to a new file beside the
 * caller's, renamed to it once whole, so that a reader finds the record
 * whole or not at all; to WORK/record when the caller asked for none.
 */
static int write_record(struct prep *p)
{
    char *dest = p->o->record != NULL ? strdup(p->o->record) : format("%s/record", p->work);
    char *tmp = dest != NULL ? format("%s.XXXXXX", dest) : NULL;
    int fd = tmp != NULL ? mkstemp(tmp) : -1, status;
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;

    if (f == NULL) {
        status = failed(p, "record: %s cannot be written: %s", dest != NULL ? dest : "",
                        strerror(errno != 0 ? errno : ENOMEM));
        if (fd >= 0) {
            close(fd);
            unlink(tmp);
        }
        free(tmp);
        free(dest);
        return status;
    }
    record_head(p, f);
    status = hash_artifacts(p, f);
    if ((fflush(f) != 0 || fsync(fd) != 0) && status == 0)
        status = failed(p, "record: %s cannot be written: %s", dest, strerror(errno));
    fclose(f);
    if (status == 0 && rename(tmp, dest) != 0)
        status = failed(p, "record: %s cannot be written: %s", dest, strerror(errno));
    if (status != 0)
        unlink(tmp);
    free(tmp);
    free(dest);
    return status;
}

/* ---- The preparation. ---- */

/** Make the recovery key when the tree has a recovery system and none, and
 * give the committed demo keys the mode ssh(1) requires. */
static int keys(struct prep *p)
{
    static const char *const demo[] = {"krffs/rach2_demo_ed25519", "krffs/rach2_test_ed25519"};
    char *manifest = format("%s/krffs/manifests/rach2-recovery.manifest", p->tree);
    char *key = format("%s/krffs/recovery_authorized_keys", p->tree), *path;
    char *goal[] = {"recovery-key"};
    struct step s;
    size_t i;
    int status = 0;

    memset(&s, 0, sizeof(s));
    s.name = "recovery-key";
    s.script = script_log;
    if (manifest == NULL || key == NULL)
        status = failed(p, "recovery-key: memory ran out");
    else if (access(manifest, F_OK) == 0 && access(key, F_OK) != 0)
        status = run_make(p, &s, goal, 1);
    for (i = 0; status == 0 && i < sizeof(demo) / sizeof(demo[0]); i++) {
        if ((path = format("%s/%s", p->tree, demo[i])) != NULL && access(path, F_OK) == 0 &&
            chmod(path, 0600) != 0)
            status = failed(p, "%s: chmod: %s", demo[i], strerror(errno));
        free(path);
    }
    free(manifest);
    free(key);
    return status;
}

/** Run the steps, find and build the artifacts, and record them. */
static int prepare(struct prep *p)
{
    char *kernel_goal;
    int status = find_path(p);

    if (status == 0)
        status = check_tools(p);
    if (status == 0)
        status = keys(p);
    if (status == 0)
        status = build_step(p, "components", "-j", "components");
    if (status == 0 && p->o->kernel != NULL) {
        kernel_goal = format("%s-kernel", p->o->kernel);
        status = kernel_goal != NULL ? build_step(p, "kernel", kernel_goal, NULL)
                                     : failed(p, "kernel: memory ran out");
        free(kernel_goal);
    }
    if (status == 0 && p->o->nsuites > 1)
        status = build_step(p, "prepare-all", "-j", "prepare-all");
    if (status == 0)
        status = read_database(p);
    if (status == 0)
        status = find_artifacts(p);
    if (status == 0)
        status = build_goals(p);
    if (status == 0)
        status = write_record(p);
    return status;
}

/** End the executor, keep the work and name it, or remove it, and free the
 * rest. */
static void release(struct prep *p, int keep)
{
    size_t i;

    if (p->ex != NULL)
        etude_exec_free(p->ex);
    if (p->work != NULL && keep)
        fprintf(p->out, "etude-legacy: evidence: %s\n", p->work);
    else if (p->work != NULL && remove_tree(p->work) != 0)
        fprintf(p->out, "etude-legacy: %s: not removed\n", p->work);
    fflush(p->out);
    for (i = 0; p->env != NULL && p->env[i] != NULL; i++)
        free(p->env[i]);
    for (i = 0; i < p->nreg; i++) {
        free(p->reg[i].name);
        free(p->reg[i].tools);
        free(p->reg[i].images);
        free(p->reg[i].arches);
    }
    for (i = 0; p->targets != NULL && p->targets[i] != NULL; i++)
        free(p->targets[i]);
    free(p->env);
    free(p->reg);
    free(p->targets);
    free((void *)p->chosen);
}

/** Print the result line, remove or keep the work, and free the rest. */
static void conclude(struct prep *p, int status)
{
    if (status == 0)
        fprintf(p->out, "etude-legacy: result: prepared: %lu suites, %lu files\n",
                (unsigned long)p->o->nsuites, (unsigned long)p->files);
    else if (status == 1)
        fprintf(p->out, "etude-legacy: result: failed: %s\n",
                p->why != NULL ? p->why : "memory ran out");
    else if (status > 128)
        fprintf(p->out, "etude-legacy: result: stopped by signal %d\n", status - 128);
    release(p, status == 1);
}

int etude_legacy_prepare(const struct etude_legacy_opts *o)
{
    struct prep p;
    int status;

    memset(&p, 0, sizeof(p));
    p.o = o;
    p.out = o->out != NULL ? o->out : stdout;
    status = admit(&p);
    if (status == 0) {
        fprintf(p.out, "etude-legacy: preparing %lu suites for %s in %s\n",
                (unsigned long)o->nsuites, p.arch_var + 5, p.tree);
        status = open_work(&p);
    }
    if (status == 0)
        status = prepare(&p);
    conclude(&p, status);
    db_free(&p.db);
    list_free(&p.goals);
    list_free(&p.required);
    free(p.tree);
    free(p.arch_var);
    free(p.kernel);
    free(p.kernel_var);
    free(p.work);
    free(p.why);
    return status;
}

/* ---- The check of a record. ---- */

/* One artifact of a record. */
struct entry {
    char digest[65];
    unsigned long long size, inode;
    long long mtime;
    const char *path;
};

/** Take a record's line `file DIGEST SIZE INODE MTIME PATH`; 0, or -1. */
static int take_entry(char *line, struct entry *e)
{
    int at = -1;

    if (sscanf(line, "file %64s %llu %llu %lld %n", e->digest, &e->size, &e->inode, &e->mtime,
               &at) != 4 ||
        at < 0 || line[at] == '\0' || strlen(e->digest) != 64 ||
        e->digest[strspn(e->digest, "0123456789abcdef")] != '\0')
        return -1;
    e->path = line + at;
    return 0;
}

/**
 * Compare artifact `e` of the tree `tree` with the file there now.
 *
 * @return 0 when it is unchanged, 1 when it changed or is missing.
 */
static int compare(const char *tree, const struct entry *e, int digests, FILE *out)
{
    char *path = artifact_path(tree, e->path), hex[65];
    struct stat st;
    int r = path == NULL ? -1 : digests ? hash_file(path, &st, hex) : stat(path, &st);
    int changed;

    free(path);
    if (r != 0) {
        fprintf(out, "etude-legacy: missing: %s: %s\n", e->path,
                errno == EINVAL || errno == EISDIR ? "not a regular file" : strerror(errno));
        return 1;
    }
    changed = !S_ISREG(st.st_mode) || (unsigned long long)st.st_size != e->size;
    if (digests)
        changed = changed || strcmp(hex, e->digest) != 0;
    else
        changed = changed || (unsigned long long)st.st_ino != e->inode || mtime_ns(&st) != e->mtime;
    if (changed)
        fprintf(out, "etude-legacy: changed: %s\n", e->path);
    return changed;
}

/* A reading of a record. */
struct reading {
    char *tree;        /* the tree's root, from the `tree` line */
    char *arch;        /* the `arch` line's architecture */
    char *kernel;      /* the `kernel` line's path */
    const char *suite; /* a suite to look for, or NULL */
    int listed;        /* a `suite` line names it */
    size_t files;      /* the artifacts compared */
    size_t changed;    /* those changed or missing */
    int digests;
    FILE *out;
};

/** Take line `lineno` of a record; NULL, or why the record is malformed. */
static const char *check_line(struct reading *rd, char *line, size_t lineno)
{
    struct entry e;

    if (lineno == 1)
        return strcmp(line, RECORD_HEAD) == 0 ? NULL : "not a record of etude-legacy";
    if (strncmp(line, "tree ", 5) == 0 && rd->tree == NULL)
        return (rd->tree = strdup(line + 5)) == NULL ? "memory ran out" : NULL;
    if (strncmp(line, "file ", 5) == 0) {
        if (rd->tree == NULL || take_entry(line, &e) != 0)
            return "a malformed file line";
        rd->changed += (size_t)compare(rd->tree, &e, rd->digests, rd->out);
        rd->files++;
        return NULL;
    }
    if (strncmp(line, "arch ", 5) == 0 && rd->arch == NULL)
        return (rd->arch = strdup(line + 5)) == NULL ? "memory ran out" : NULL;
    if (strncmp(line, "kernel ", 7) == 0 && rd->kernel == NULL)
        return (rd->kernel = strdup(line + 7)) == NULL ? "memory ran out" : NULL;
    if (strncmp(line, "suite ", 6) == 0 && rd->suite != NULL &&
        strncmp(line + 6, rd->suite, strlen(rd->suite)) == 0 && line[6 + strlen(rd->suite)] == ' ')
        rd->listed = 1;
    if (strncmp(line, "arch ", 5) == 0 || strncmp(line, "kernel ", 7) == 0 ||
        strncmp(line, "suite ", 6) == 0)
        return NULL;
    return "an unknown line";
}

/** Read the record `f` and compare each artifact; 0, 1, or 2 for a
 * malformed record. */
static int check_lines(FILE *f, const char *record, struct reading *rd)
{
    char *line = NULL;
    size_t cap = 0, lineno = 0;
    ssize_t len;
    const char *why = NULL;

    while (why == NULL && (len = getline(&line, &cap, f)) >= 0) {
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        why = check_line(rd, line, ++lineno);
    }
    if (why == NULL && (ferror(f) || rd->tree == NULL))
        why = "the record cannot be read whole";
    free(line);
    if (why != NULL) {
        fprintf(rd->out, "etude-legacy: %s:%lu: %s\n", record, (unsigned long)lineno, why);
        return 2;
    }
    return rd->changed != 0;
}

int etude_legacy_check(const char *record, int digests, FILE *out)
{
    struct reading rd;
    FILE *f = fopen(record, "r");
    int status;

    memset(&rd, 0, sizeof(rd));
    rd.digests = digests;
    rd.out = out != NULL ? out : stdout;
    if (f == NULL) {
        fprintf(rd.out, "etude-legacy: %s: %s\n", record, strerror(errno));
        return 2;
    }
    status = check_lines(f, record, &rd);
    fclose(f);
    free(rd.tree);
    free(rd.arch);
    free(rd.kernel);
    if (status == 0)
        fprintf(rd.out, "etude-legacy: result: unchanged: %lu files\n", (unsigned long)rd.files);
    else if (status == 1)
        fprintf(rd.out, "etude-legacy: result: changed: %lu of %lu files\n",
                (unsigned long)rd.changed, (unsigned long)rd.files);
    return status;
}

/* ---- The run of a suite. ---- */

/* The time from the suite's termination signal to its kill. runutil then
 * stops what the suite started, which takes up to twenty seconds when a
 * machine outlives SIGKILL, and writes its record of the suite's end. */
#define SUITE_GRACE_US 30000000u

/* The first line of runutil's record of a suite's end. */
#define END_HEAD "runutil-record 1"

/* The emulators that get a shim, as tests/run gives them one. */
static const char *const emulators[] = {"qemu-system-i386", "qemu-system-x86_64"};

/* The variables a suite takes from the adapter alone. */
static const char *const suite_vars[] = {"PATH=",
                                         "RACH2_QEMU=",
                                         "RACH2_QEMU_ACCEL=",
                                         "RACH2_CPUS=",
                                         "RACH2_RUN_UTIL=",
                                         "RACH2_QEMU_LEDGER=",
                                         "RACH2_QEMU_SHIMS=",
                                         "RACH2_PROGRESS_FD=",
                                         "RACH2_ETUDE_CHILDREN="};

/* What a run of a suite came to. */
enum verdict { VERDICT_INCOMPLETE, VERDICT_FAILED, VERDICT_PASSED };

/* runutil's record of a suite's end. */
struct suite_end {
    int whole;        /* the record was read whole */
    int status;       /* the target's exit status, or 128 plus its signal */
    int idle;         /* it was stopped as hung */
    int stopped;      /* the signal runutil received, or 0 */
    int leaked;       /* a process outlived the suite */
    int survivor;     /* a machine outlived its SIGTERM */
    struct list left; /* `PID GROUP COMMAND` of each machine or relay left */
    struct list cut;  /* `MACHINE RELAY` of each console cut */
};

/* One run of a suite under way, beside the state it shares with a
 * preparation. */
struct run {
    const struct etude_legacy_run_opts *ro;
    char *tree, *arch, *kernel; /* the record's */
    const char *accel;          /* QEMU's accelerator */
    const char *cpus;           /* the processors of each machine */
    char idle[16];              /* the idle limit in seconds */
    char *target;               /* run-NAME */
    char *bin;                  /* WORK/bin, the shims */
    char *runutil;              /* WORK/runutil */
    char *ledger;               /* WORK/machines, the machines' ledger */
    char *end;                  /* WORK/end, runutil's record */
    char *log;                  /* WORK/suite.log, the suite's output */
    long long stamp;            /* the time of WORK/start, made as the suite
                                   starts, or 0 before */
    enum verdict verdict;
    struct suite_end e;
};

/**
 * Read the run's record, which must list the suite and whose artifacts must
 * be as it recorded them, and take its tree, architecture and kernel; 0, or
 * 2 after a refusal.
 */
static int run_record(struct prep *p, struct run *r)
{
    const char *record = r->ro->record;
    FILE *f = fopen(record, "r");
    struct reading rd;
    int status;

    if (f == NULL)
        return refuse(p, "%s: %s", record, strerror(errno));
    memset(&rd, 0, sizeof(rd));
    rd.suite = r->ro->suite;
    rd.out = p->out;
    status = check_lines(f, record, &rd);
    fclose(f);
    r->tree = rd.tree;
    r->arch = rd.arch;
    r->kernel = rd.kernel;
    if (status == 2)
        return refuse(p, "%s cannot be read as a record of a preparation", record);
    if (r->arch == NULL || r->kernel == NULL)
        return refuse(p, "%s names no architecture or no kernel", record);
    if (!rd.listed)
        return refuse(p, "%s is no suite of the preparation %s records", r->ro->suite, record);
    if (status != 0)
        return refuse(p,
                      "the preparation changed: %lu of %lu files differ from the record; "
                      "prepare the suite again",
                      (unsigned long)rd.changed, (unsigned long)rd.files);
    return 0;
}

/** The accelerator where there is no KVM: HVF on an x86 Mac whose kernel
 * offers the hypervisor, and the emulator otherwise. */
static const char *accel_without_kvm(void)
{
#if defined(__APPLE__) && defined(__x86_64__)
    int on = 0;
    size_t len = sizeof(on);

    if (sysctlbyname("kern.hv_support", &on, &len, NULL, 0) == 0 && on == 1)
        return "hvf";
#endif
    return "tcg";
}

/**
 * Take QEMU's accelerator and the machines' processors as tests/run takes
 * them: RACH2_QEMU_ACCEL when it is set; else KVM when its device,
 * RACH2_KVM_DEVICE or /dev/kvm, can be read and written, HVF on an x86 Mac
 * that offers it, and the emulator otherwise; and RACH2_CPUS, 2 when it is
 * unset. A KVM device this user cannot open is refused, since the suites'
 * bounds were set under KVM. 0, or 2 after a refusal.
 */
static int run_machine(struct prep *p, struct run *r)
{
    const char *accel = getenv("RACH2_QEMU_ACCEL"), *kvm = getenv("RACH2_KVM_DEVICE");
    const char *cpus = getenv("RACH2_CPUS");

    if (kvm == NULL || *kvm == '\0')
        kvm = "/dev/kvm";
    if (accel != NULL && *accel != '\0') {
        if (strcmp(accel, "kvm") != 0 && strcmp(accel, "hvf") != 0 && strcmp(accel, "tcg") != 0)
            return refuse(p, "RACH2_QEMU_ACCEL must be kvm, hvf or tcg");
        r->accel = accel;
    } else if (access(kvm, R_OK | W_OK) == 0) {
        r->accel = "kvm";
    } else if (access(kvm, F_OK) == 0) {
        return refuse(p,
                      "%s exists but this user cannot read and write it; add the user to its "
                      "group, or set RACH2_QEMU_ACCEL=tcg to run under the emulator",
                      kvm);
    } else {
        r->accel = accel_without_kvm();
    }
    if (cpus == NULL || *cpus == '\0')
        cpus = "2";
    if (cpus[0] < '1' || cpus[0] > '9' || strlen(cpus) > 2 ||
        cpus[strspn(cpus, "0123456789")] != '\0' || atoi(cpus) > 64)
        return refuse(p, "RACH2_CPUS must be a whole number from 1 to 64");
    r->cpus = cpus;
    return 0;
}

/**
 * Check the run's request before anything runs: the record, the suite in the
 * tree's registry, the held tree, the port block and the machines; 0, or 2
 * after a refusal.
 */
static int run_admit(struct prep *p, struct run *r, struct etude_legacy_opts *o)
{
    const char *offset = getenv("RACH2_PORT_OFFSET");
    int status = run_record(p, r);

    if (status != 0)
        return status;
    o->tree = r->tree;
    o->arch = r->arch;
    o->suites = (char *const *)&r->ro->suite;
    o->nsuites = 1;
    if ((status = admit(p)) != 0)
        return status;
    if (strcmp(r->kernel, p->kernel) != 0) {
        free(p->kernel);
        p->kernel = strdup(r->kernel);
        p->kernel_var = p->kernel != NULL ? format("BOOT_KERNEL=%s", p->kernel) : NULL;
        if (p->kernel_var == NULL)
            return sys_error(p, "the request", ENOMEM);
    }
    if (offset == NULL || *offset == '\0' || offset[strspn(offset, "0123456789")] != '\0')
        return refuse(p, "RACH2_PORT_OFFSET must name the port block the caller holds, as "
                         "etude-bridge -p gives it");
    if ((status = run_machine(p, r)) != 0)
        return status;
    snprintf(r->idle, sizeof(r->idle), "%u", r->ro->idle != 0 ? r->ro->idle : ETUDE_LEGACY_IDLE);
    r->target = format("run-%s", r->ro->suite);
    return r->target == NULL ? sys_error(p, "the request", ENOMEM) : 0;
}

/** Name the files of the work directory; 0, or 2 when memory ran out. */
static int run_files(struct prep *p, struct run *r)
{
    r->bin = format("%s/bin", p->work);
    r->runutil = format("%s/runutil", p->work);
    r->ledger = format("%s/machines", p->work);
    r->end = format("%s/end", p->work);
    r->log = format("%s/suite.log", p->work);
    if (r->bin == NULL || r->runutil == NULL || r->ledger == NULL || r->end == NULL ||
        r->log == NULL)
        return sys_error(p, "the work directory", ENOMEM);
    return 0;
}

/** Refuse a suite whose commands are not on the steps' path; 0, or 2. */
static int run_tools(struct prep *p)
{
    const char *qemu =
        strcmp(p->arch_build, "build") == 0 ? "qemu-system-i386" : "qemu-system-x86_64";
    char *missing = tools_missing(p->chosen[0]->tools, qemu, env_path(p));
    int status;

    if (missing == NULL)
        return 0;
    status = refuse(p, "%s needs %s, which this host does not have", p->chosen[0]->name, missing);
    free(missing);
    return status;
}

/** Compile runutil into the work directory from the tree's source, as
 * tests/run does, so that no build of the tree changes it while it runs. */
static int run_runutil(struct prep *p, struct run *r)
{
    char *src = format("%s/tests/system/runner/runutil.c", p->tree);
    char *cmd[] = {"cc", "-O2", "-Wall", "-Wextra", "-o", r->runutil, src, NULL};
    struct step s;
    int status;

    if (src == NULL)
        return failed(p, "runutil: memory ran out");
    memset(&s, 0, sizeof(s));
    s.name = "runutil";
    s.script = script_log;
    s.cmd = cmd;
    status = run_step(p, &s);
    free(src);
    return status;
}

/**
 * Write a shim into WORK/bin for each emulator on the steps' path, as
 * tests/run writes them: it adds the accelerator, and the processors when
 * RACH2_CPUS is not 1, and execs the emulator through `runutil machine`,
 * which records the machine in the suite's ledger and relays its console.
 * An emulator whose path a shim cannot quote is refused, since the suite
 * would reach it unrecorded. 0, 1 after an error, or 2 after a refusal.
 */
static int run_shims(struct prep *p, struct run *r)
{
    char *real, *shim;
    FILE *f;
    size_t i;
    int bad;

    if (mkdir(r->bin, 0700) != 0)
        return failed(p, "%s: %s", r->bin, strerror(errno));
    for (i = 0; i < sizeof(emulators) / sizeof(emulators[0]); i++) {
        if ((real = path_of(emulators[i], env_path(p))) == NULL)
            continue;
        if (real[0] != '/' || real[strcspn(real, "\"\\$`")] != '\0') {
            bad = refuse(p, "%s: a shim cannot name this emulator", real);
            free(real);
            return bad;
        }
        shim = format("%s/%s", r->bin, emulators[i]);
        f = shim != NULL ? fopen(shim, "w") : NULL;
        bad = f == NULL;
        if (f != NULL) {
            fprintf(f,
                    "#!/bin/sh\nsmp=\n[ \"${RACH2_CPUS:-1}\" = 1 ] || smp=\"-smp $RACH2_CPUS\"\n"
                    "exec \"$RACH2_RUN_UTIL\" machine \"${RACH2_QEMU_LEDGER:?}\" \"%s\" "
                    "-accel \"${RACH2_QEMU_ACCEL:?}\" $smp \"$@\"\n",
                    real);
            bad = fclose(f) != 0 || chmod(shim, 0755) != 0;
        }
        free(real);
        free(shim);
        if (bad)
            return failed(p, "%s: its shim cannot be written: %s", emulators[i],
                          strerror(errno != 0 ? errno : ENOMEM));
    }
    return 0;
}

/** Say whether `var` is a variable the suite takes from the adapter. */
static int suite_var(const char *var)
{
    size_t i;

    for (i = 0; i < sizeof(suite_vars) / sizeof(suite_vars[0]); i++) {
        if (strncmp(var, suite_vars[i], strlen(suite_vars[i])) == 0)
            return 1;
    }
    return 0;
}

/**
 * Make the suite's environment from the steps': the shims before the steps'
 * path, the emulator, the accelerator, the processors, runutil and the
 * machines' ledger, as tests/run gives them; 0, or 2 when memory ran out.
 */
static int run_env(struct prep *p, struct run *r)
{
    const char *qemu =
        strcmp(p->arch_build, "build") == 0 ? "qemu-system-i386" : "qemu-system-x86_64";
    char *add[7];
    size_t n = 0, k = 0, i;
    char **env;
    int bad = 0;

    add[0] = format("PATH=%s:%s", r->bin, env_path(p));
    add[1] = format("RACH2_QEMU=%s", qemu);
    add[2] = format("RACH2_QEMU_ACCEL=%s", r->accel);
    add[3] = format("RACH2_CPUS=%s", r->cpus);
    add[4] = format("RACH2_RUN_UTIL=%s", r->runutil);
    add[5] = format("RACH2_QEMU_LEDGER=%s", r->ledger);
    add[6] = format("RACH2_QEMU_SHIMS=%s", r->bin);
    while (p->env[n] != NULL)
        n++;
    env = calloc(n + 8, sizeof(*env));
    for (i = 0; i < 7; i++)
        bad = bad || add[i] == NULL;
    if (env == NULL || bad) {
        for (i = 0; i < 7; i++)
            free(add[i]);
        free(env);
        return sys_error(p, "the suite's environment", ENOMEM);
    }
    for (i = 0; i < n; i++) {
        if (suite_var(p->env[i]))
            free(p->env[i]);
        else
            env[k++] = p->env[i];
    }
    for (i = 0; i < 7; i++)
        env[k++] = add[i];
    free(p->env);
    p->env = env;
    return 0;
}

/** Take the decimal number after `name` in `line` into `*v`; 1, or 0 when
 * the line is not that field. */
static int end_field(const char *line, const char *name, int *v)
{
    size_t n = strlen(name);
    char *rest;
    long x;

    if (strncmp(line, name, n) != 0 || line[n] != ' ' || line[n + 1] < '0' || line[n + 1] > '9')
        return 0;
    x = strtol(line + n + 1, &rest, 10);
    if (*rest != '\0' || x > 255)
        return 0;
    *v = (int)x;
    return 1;
}

/**
 * Read runutil's record of the suite's end, as `finish_record` in runutil.c
 * writes it, into `e`: whole only when it has its first line and a status,
 * every line is known and ends with a newline, and `end` is last.
 */
static void read_end(const char *path, struct suite_end *e)
{
    FILE *f = fopen(path, "r");
    char *line = NULL;
    size_t cap = 0, lineno = 0;
    ssize_t len;
    int bad = 0, done = 0;

    e->status = -1;
    if (f == NULL)
        return;
    while (!bad && (len = getline(&line, &cap, f)) >= 0) {
        if (done || len == 0 || line[len - 1] != '\n') {
            bad = 1;
            break;
        }
        line[len - 1] = '\0';
        if (++lineno == 1)
            bad = strcmp(line, END_HEAD) != 0;
        else if (strncmp(line, "left ", 5) == 0)
            bad = list_add(&e->left, line + 5) != 0;
        else if (strncmp(line, "cut ", 4) == 0)
            bad = list_add(&e->cut, line + 4) != 0;
        else if (strcmp(line, "end") == 0)
            done = 1;
        else
            bad = !end_field(line, "status", &e->status) && !end_field(line, "idle", &e->idle) &&
                  !end_field(line, "stopped", &e->stopped) &&
                  !end_field(line, "leaked", &e->leaked) &&
                  !end_field(line, "survivor", &e->survivor);
    }
    e->whole = done && !bad && !ferror(f) && e->status >= 0;
    free(line);
    fclose(f);
}

/** Write a `left` entry of runutil's record, `PID GROUP COMMAND`, as
 * prose into `buf`. */
static void left_text(const char *entry, char *buf, size_t n)
{
    long pid = 0, group = 0;
    int at = -1;

    sscanf(entry, "%ld %ld %n", &pid, &group, &at);
    snprintf(buf, n, "pid %ld, group %ld, %s", pid, group, at > 0 ? entry + at : "?");
}

/**
 * Judge the suite from how its case ended, `ended` and `x`, and runutil's
 * record `r->e`, and give the reason in `why`. A run that did not finish is
 * incomplete whatever else it shows; one that finished fails for a machine
 * left running, a process that outlived it, or a target that did not exit 0.
 */
static enum verdict judge_suite(const struct prep *p, const struct run *r, enum ended ended,
                                const struct etude_exec_result *x, char *why, size_t n)
{
    const struct suite_end *e = &r->e;
    unsigned bound = p->o->seconds != 0 ? p->o->seconds : ETUDE_LEGACY_SECONDS;
    int seen = x->end == ETUDE_END_EXITED || x->end == ETUDE_END_SIGNALED;
    char left[160];

    if (ended == ENDED_BOUND)
        snprintf(why, n, "it ran past its bound of %u s", bound);
    else if (!seen)
        snprintf(why, n, "its end was not seen");
    else if (!e->whole && x->end == ETUDE_END_SIGNALED)
        snprintf(why, n, "runutil left no record of its end: it was killed by signal %d", x->value);
    else if (!e->whole)
        snprintf(why, n, "runutil left no record of its end: it exited %d", x->value);
    else if (e->idle)
        snprintf(why, n, "no output, console or processor time for %s s", r->idle);
    else if (e->stopped)
        snprintf(why, n, "runutil was stopped by signal %d", e->stopped);
    else if (e->cut.n > 0)
        snprintf(why, n, "the console of machine %.*s was cut", (int)strcspn(e->cut.v[0], " "),
                 e->cut.v[0]);
    else if (x->cleanup != ETUDE_CLEANUP_CONFIRMED)
        snprintf(why, n, "its processes were not all seen to end");
    else
        why[0] = '\0';
    if (why[0] != '\0')
        return VERDICT_INCOMPLETE;
    if (e->left.n > 0) {
        left_text(e->left.v[0], left, sizeof(left));
        snprintf(why, n, "a machine was left running: %s", left);
    } else if (e->survivor) {
        snprintf(why, n, "a machine outlived its SIGTERM");
    } else if (e->leaked) {
        snprintf(why, n, "a process outlived the suite");
    } else if (e->status > 128) {
        snprintf(why, n, "it was killed by signal %d", e->status - 128);
    } else if (e->status != 0) {
        snprintf(why, n, "it exited %d", e->status);
    } else {
        return VERDICT_PASSED;
    }
    return VERDICT_FAILED;
}

/** Print a line for each machine left running and each console cut. */
static void print_findings(const struct prep *p, const struct run *r)
{
    char text[160];
    size_t i;

    for (i = 0; i < r->e.left.n; i++) {
        left_text(r->e.left.v[i], text, sizeof(text));
        fprintf(p->out, "etude-legacy: %s: left running: %s\n", r->ro->suite, text);
    }
    for (i = 0; i < r->e.cut.n; i++) {
        long machine = 0, relay = 0;

        sscanf(r->e.cut.v[i], "%ld %ld", &machine, &relay);
        fprintf(p->out, "etude-legacy: %s: console cut: machine %ld, relay %ld\n", r->ro->suite,
                machine, relay);
    }
}

/** Make WORK/start, whose time the logs the suite writes are newer than;
 * 0, or 1 after an error. */
static int run_stamp(struct prep *p, struct run *r)
{
    char *path = format("%s/start", p->work);
    int fd = path != NULL ? open(path, O_WRONLY | O_CREAT | O_EXCL, 0600) : -1;
    struct stat st;
    int ok = fd >= 0 && fstat(fd, &st) == 0;

    if (fd >= 0)
        close(fd);
    free(path);
    if (!ok)
        return failed(p, "%s/start cannot be made: %s", p->work, strerror(errno));
    r->stamp = mtime_ns(&st);
    return 0;
}

/**
 * Run the suite's target through the executor under `runutil record`, with
 * the suite's output in WORK/suite.log copied to the output as it grows, and
 * judge it; 0 when it passed, 1 when it did not, or 128 plus the signal
 * after a stop.
 */
static int run_suite(struct prep *p, struct run *r)
{
    char *argv[20], why[256];
    static const char *const word[] = {"INCOMPLETE", "FAIL", "PASS"};
    const struct etude_exec_result *x;
    struct etude_exec_spec spec;
    struct etude_proc *proc;
    struct follow f;
    enum ended ended;
    uint64_t began;
    size_t k = 0;
    int status;

    argv[k++] = "/bin/sh";
    argv[k++] = "-c";
    argv[k++] = (char *)script_answer;
    argv[k++] = "etude-legacy";
    argv[k++] = r->log;
    argv[k++] = r->runutil;
    argv[k++] = "record";
    argv[k++] = (char *)r->ro->suite;
    argv[k++] = r->idle;
    argv[k++] = r->ledger;
    argv[k++] = r->end;
    argv[k++] = "make";
    argv[k++] = "-C";
    argv[k++] = p->tree;
    argv[k++] = "--no-print-directory";
    argv[k++] = p->arch_var;
    if (p->kernel_var != NULL)
        argv[k++] = p->kernel_var;
    argv[k++] = r->target;
    argv[k] = NULL;
    if (stopped(p))
        return 128 + (int)*p->o->stop;
    if ((status = run_stamp(p, r)) != 0)
        return status;
    fprintf(p->out, "run: %s: starting (idle limit %s s)\n", r->ro->suite, r->idle);
    fflush(p->out);
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = r->ro->suite;
    spec.argv = argv;
    spec.envp = p->env;
    spec.cwd = p->tree;
    spec.output_max = OUTPUT_MAX;
    spec.grace_us = SUITE_GRACE_US;
    spec.posix_only = p->posix_only;
    began = posix_now_us();
    if (etude_exec_start(p->ex, &spec, &proc) != 0)
        return failed(p, "it did not start: %s", strerror(errno));
    f.path = r->log;
    f.fd = -1;
    ended = await_step(p, proc, &f);
    if (f.fd >= 0)
        close(f.fd);
    x = etude_proc_result(proc);
    if (ended == ENDED_STOP) {
        fprintf(p->out, "etude-legacy: %s: stopped by signal %d\n", r->ro->suite, (int)*p->o->stop);
        etude_proc_free(proc);
        return 128 + (int)*p->o->stop;
    }
    read_end(r->end, &r->e);
    r->verdict = judge_suite(p, r, ended, x, why, sizeof(why));
    fprintf(p->out, "run: %s: %s in %llus\n", r->ro->suite, word[r->verdict],
            (unsigned long long)((posix_now_us() - began) / 1000000u));
    print_findings(p, r);
    if (r->verdict != VERDICT_PASSED && x->err.kept > 0)
        fprintf(p->out, "    | %s%s", x->err.data,
                x->err.data[x->err.kept - 1] == '\n' ? "" : "\n");
    etude_proc_free(proc);
    return r->verdict == VERDICT_PASSED ? 0 : failed(p, "%s", why);
}

/** Make the directories of `path` above its last name, from the length
 * `from` on, which exists already; 0, or -1. */
static int make_parents(char *path, size_t from)
{
    char *slash;

    for (slash = strchr(path + from, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        if (mkdir(path, 0700) != 0 && errno != EEXIST) {
            *slash = '/';
            return -1;
        }
        *slash = '/';
    }
    return 0;
}

/** Copy the file `from` to `to`; 0, or -1. */
static int copy_file(const char *from, const char *to)
{
    char buf[65536];
    ssize_t n = 0;
    int in = open(from, O_RDONLY);
    int out = in >= 0 ? open(to, O_WRONLY | O_CREAT | O_TRUNC, 0600) : -1, bad = out < 0;

    while (!bad && (n = read(in, buf, sizeof(buf))) > 0)
        bad = write(out, buf, (size_t)n) != n;
    if (in >= 0)
        close(in);
    if (out >= 0 && close(out) != 0)
        bad = 1;
    return bad || n < 0 ? -1 : 0;
}

/** Give the globs of the files suite `s` writes under the images, as
 * tests/run reads them, into `g`; 0, or -1 when memory ran out. */
static int image_globs(const struct suite *s, struct list *g)
{
    static const char *const twin[] = {"%.*s*-e1000.*", "%.*s*-e1000-*", "%.*s*-e1000/*"};
    static const char *const own[] = {"%.*s-test*", "%.*s.*", "%.*s-*", "%.*s/*"};
    const char *images = s->images;
    size_t n = strlen(s->name), len, i;
    char *glob;
    int e1000 = n > 6 && strcmp(s->name + n - 6, "-e1000") == 0;

    while (*images != '\0') {
        len = strcspn(images, ",");
        if ((glob = format("%.*s", (int)len, images)) == NULL || list_add(g, glob) != 0) {
            free(glob);
            return -1;
        }
        free(glob);
        images += len + (images[len] == ',');
    }
    if (g->n != 0)
        return 0;
    for (i = 0; i < (e1000 ? 3u : 4u); i++) {
        glob = format(e1000 ? twin[i] : own[i], (int)(e1000 ? n - 6 : n), s->name);
        if (glob == NULL || list_add(g, glob) != 0) {
            free(glob);
            return -1;
        }
        free(glob);
    }
    return 0;
}

/** Say whether the file `rel` of the images is the suite's, by the globs
 * `g`; a plain suite owns no file of its e1000 twin's. */
static int owns(const char *rel, const struct list *g, int e1000)
{
    static const char *const twin[] = {"*-e1000.*", "*-e1000-*", "*-e1000/*"};
    size_t i;

    for (i = 0; !e1000 && i < sizeof(twin) / sizeof(twin[0]); i++) {
        if (fnmatch(twin[i], rel, 0) == 0)
            return 0;
    }
    for (i = 0; i < g->n; i++) {
        if (fnmatch(g->v[i], rel, 0) == 0)
            return 1;
    }
    return 0;
}

/**
 * Copy each log under `dir`, `rel` below the images, that the suite owns by
 * the globs `g` and that changed since it started, into WORK/images.
 */
static void keep_dir(struct prep *p, struct run *r, const char *dir, const char *rel,
                     const struct list *g, int e1000)
{
    DIR *d = opendir(dir);
    struct dirent *ent;
    struct stat st;
    char *path, *sub, *to;
    size_t n;

    while (d != NULL && (ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        path = format("%s/%s", dir, ent->d_name);
        sub = rel[0] != '\0' ? format("%s/%s", rel, ent->d_name) : strdup(ent->d_name);
        n = sub != NULL ? strlen(sub) : 0;
        if (path != NULL && sub != NULL && lstat(path, &st) == 0 && S_ISDIR(st.st_mode))
            keep_dir(p, r, path, sub, g, e1000);
        else if (path != NULL && sub != NULL && lstat(path, &st) == 0 && S_ISREG(st.st_mode) &&
                 n > 4 && strcmp(sub + n - 4, ".log") == 0 && mtime_ns(&st) >= r->stamp &&
                 owns(sub, g, e1000)) {
            to = format("%s/images/%s", p->work, sub);
            if (to == NULL || make_parents(to, strlen(p->work) + 1) != 0 ||
                copy_file(path, to) != 0)
                fprintf(p->out, "etude-legacy: %s: not kept: %s\n", sub, strerror(errno));
            free(to);
        }
        free(path);
        free(sub);
    }
    if (d != NULL)
        closedir(d);
}

/** Keep the logs the suite wrote under the architecture's images, as
 * tests/run's keep_logs keeps them, in WORK/images. */
static void keep_images(struct prep *p, struct run *r)
{
    const struct suite *s = p->chosen[0];
    char *dir = format("%s/%s/images", p->tree, p->arch_build);
    size_t n = strlen(s->name);
    struct list g;

    memset(&g, 0, sizeof(g));
    if (dir != NULL && image_globs(s, &g) == 0)
        keep_dir(p, r, dir, "", &g, n > 6 && strcmp(s->name + n - 6, "-e1000") == 0);
    list_free(&g);
    free(dir);
}

/** Print the run's result line, keep its evidence or remove its work, and
 * free the rest. */
static void run_conclude(struct prep *p, struct run *r, int status)
{
    if (status == 0)
        fprintf(p->out, "etude-legacy: result: passed: %s\n", r->ro->suite);
    else if (status == 1)
        fprintf(p->out, "etude-legacy: result: %s: %s: %s\n",
                r->verdict == VERDICT_FAILED ? "failed" : "incomplete", r->ro->suite,
                p->why != NULL ? p->why : "memory ran out");
    else if (status > 128)
        fprintf(p->out, "etude-legacy: result: stopped by signal %d\n", status - 128);
    if (status == 1 && r->stamp != 0)
        keep_images(p, r);
    release(p, status == 1);
    list_free(&r->e.left);
    list_free(&r->e.cut);
    free(r->tree);
    free(r->arch);
    free(r->kernel);
    free(r->target);
    free(r->bin);
    free(r->runutil);
    free(r->ledger);
    free(r->end);
    free(r->log);
}

int etude_legacy_run(const struct etude_legacy_run_opts *ro)
{
    struct etude_legacy_opts o;
    struct prep p;
    struct run r;
    int status;

    memset(&o, 0, sizeof(o));
    memset(&p, 0, sizeof(p));
    memset(&r, 0, sizeof(r));
    o.work = ro->work;
    o.seconds = ro->seconds;
    o.out = ro->out;
    o.stop = ro->stop;
    p.o = &o;
    p.out = ro->out != NULL ? ro->out : stdout;
    p.posix_only = ro->posix_only;
    r.ro = ro;
    status = ro->record != NULL && ro->suite != NULL ? run_admit(&p, &r, &o)
                                                     : refuse(&p, "no record or no suite named");
    if (status == 0) {
        fprintf(p.out, "etude-legacy: running %s for %s in %s\n", ro->suite, p.arch_var + 5,
                p.tree);
        status = open_work(&p);
    }
    if (status == 0)
        status = run_files(&p, &r);
    if (status == 0)
        status = find_path(&p);
    if (status == 0)
        status = run_tools(&p);
    if (status == 0)
        status = run_runutil(&p, &r);
    if (status == 0)
        status = run_shims(&p, &r);
    if (status == 0)
        status = run_env(&p, &r);
    if (status == 0)
        status = run_suite(&p, &r);
    run_conclude(&p, &r, status);
    db_free(&p.db);
    list_free(&p.goals);
    list_free(&p.required);
    free(p.tree);
    free(p.arch_var);
    free(p.kernel);
    free(p.kernel_var);
    free(p.work);
    free(p.why);
    return status;
}
