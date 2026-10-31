/*
 * native_prepare.c: `etude prepare native`, which prepares the immutable
 * payload of the local Rach2 collection that tests/native/Readme.md
 * describes, and runs none of its cases.
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "native_prepare.h"
#include "sha256.h"

extern char **environ;

/* The directories etude-guest resolves staged files in: `stage` and its
 * numbered subdirectories, for programs that share a final name. */
#define LANES 8

/* A sorted set of paths relative to the tree. */
struct paths {
    char **v;
    size_t n;
};

static void add(struct paths *p, const char *path)
{
    for (size_t i = 0; i < p->n; i++)
        if (!strcmp(p->v[i], path))
            return;
    char **v = realloc(p->v, (p->n + 1) * sizeof(*v));
    if (!v || !(v[p->n] = strdup(path)))
        exit(1);
    p->v = v;
    p->n++;
}

static void paths_free(struct paths *p)
{
    for (size_t i = 0; i < p->n; i++)
        free(p->v[i]);
    free(p->v);
    memset(p, 0, sizeof(*p));
}

/* Order paths component by component, so that a directory's files come
 * before a longer name that only begins with the directory's. */
static int path_order(const void *a, const void *b)
{
    const unsigned char *x = *(const unsigned char *const *)a,
                        *y = *(const unsigned char *const *)b;
    for (; *x && *x == *y; x++, y++)
        ;
    unsigned cx = *x == '/' ? 1 : *x ? *x + 1u : 0, cy = *y == '/' ? 1 : *y ? *y + 1u : 0;
    return cx < cy ? -1 : cx > cy;
}

static void paths_sort(struct paths *p)
{
    qsort(p->v, p->n, sizeof(*p->v), path_order);
}

/** Copy `text`, or end the program when memory is exhausted. */
char *np_strdup(const char *text)
{
    char *copy = strdup(text);
    if (copy == NULL) {
        fputs("etude prepare native: out of memory\n", stderr);
        exit(1);
    }
    return copy;
}

/**
 * Format a path into `path`, a buffer of `size` bytes, as snprintf does.
 *
 * Return 0, or -1 after a diagnostic when the path does not fit.
 */
int np_path(char *path, size_t size, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(path, size, format, ap);
    va_end(ap);
    if (n >= 0 && (size_t)n < size)
        return 0;
    fprintf(stderr, "etude prepare native: path too long: %.64s...\n", path);
    return -1;
}

int np_file_hash(const char *path, char hex[65])
{
    struct sha256 hash;
    unsigned char bytes[1 << 16], sum[32];
    size_t n;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    sha256_init(&hash);
    while ((n = fread(bytes, 1, sizeof(bytes), f)))
        sha256_update(&hash, bytes, n);
    int bad = ferror(f);
    fclose(f);
    if (bad)
        return -1;
    sha256_final(&hash, sum);
    sha256_hex(sum, hex);
    return 0;
}

/** Remove `path` and, for a directory, everything below it. */
int np_remove(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
    DIR *d = opendir(path);
    if (d == NULL)
        return -1;
    int bad = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char sub[4096];
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name) >= (int)sizeof(sub) ||
            np_remove(sub))
            bad = -1;
    }
    closedir(d);
    return rmdir(path) == 0 || errno == ENOENT ? bad : -1;
}

static int make_parents(const char *path)
{
    char copy[4096];
    if (snprintf(copy, sizeof(copy), "%s", path) >= (int)sizeof(copy))
        return -1;
    for (char *p = copy + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(copy, 0777) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    return 0;
}

static int copy_file(const char *from, const char *to, int executable)
{
    struct stat st;
    char bytes[1 << 16];
    size_t n;
    FILE *in = fopen(from, "rb"), *out = NULL;
    int bad = in == NULL || fstat(fileno(in), &st) != 0 || !S_ISREG(st.st_mode) ||
              make_parents(to) || (out = fopen(to, "wb")) == NULL;
    while (!bad && (n = fread(bytes, 1, sizeof(bytes), in)))
        bad = fwrite(bytes, 1, n, out) != n;
    if (in) {
        bad = bad || ferror(in);
        fclose(in);
    }
    if (out && fclose(out))
        bad = 1;
    if (!bad)
        bad = chmod(to, (st.st_mode & 07777) | (executable ? 0111 : 0)) != 0;
    if (bad)
        fprintf(stderr, "etude prepare native: prepared input is missing: %s\n", from);
    return bad ? -1 : 0;
}

/* ---- The inventory. ---- */

static void push(char ***v, size_t *n, const char *word)
{
    char **grown = realloc(*v, (*n + 2) * sizeof(**v));
    if (!grown || !(grown[*n] = strdup(word)))
        exit(1);
    grown[++*n] = NULL;
    *v = grown;
}

static void cases_free(struct np_case *k, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(k[i].name);
        for (size_t j = 0; j < k[i].nfixtures; j++)
            free(k[i].fixtures[j]);
        for (size_t j = 0; j < k[i].nargv; j++)
            free(k[i].argv[j]);
        free(k[i].fixtures);
        free(k[i].argv);
    }
    free(k);
}

/* Read one case line: `case`, the name, the fixtures, `--`, and the command. */
static int case_line(char *line, struct np_case **cases, size_t *n, const char *file)
{
    char *save = NULL, *w;
    struct np_case k;
    int command = 0;

    memset(&k, 0, sizeof(k));
    w = strtok_r(line, " \t\r", &save);
    if (w == NULL)
        return 0;
    if (strcmp(w, "case") || (w = strtok_r(NULL, " \t\r", &save)) == NULL) {
        fprintf(stderr, "etude prepare native: tests/native/%s: invalid case\n", file);
        return -1;
    }
    k.name = np_strdup(w);
    for (size_t i = 0; i < *n; i++)
        if (!strcmp((*cases)[i].name, w)) {
            fprintf(stderr, "etude prepare native: tests/native/%s: duplicate case %s\n", file, w);
            free(k.name);
            return -1;
        }
    while ((w = strtok_r(NULL, " \t\r", &save)) != NULL) {
        if (!command && !strcmp(w, "--"))
            command = 1;
        else if (command)
            push(&k.argv, &k.nargv, w);
        else
            push(&k.fixtures, &k.nfixtures, w);
    }
    struct np_case *v = realloc(*cases, (*n + 1) * sizeof(*v));
    if (!v)
        exit(1);
    *cases = v;
    (*cases)[(*n)++] = k;
    if (!command || k.nargv == 0) {
        fprintf(stderr, "etude prepare native: tests/native/%s: incomplete case %s\n", file,
                k.name);
        return -1;
    }
    return 0;
}

/**
 * Read the cases of probes.cases, musl.cases and rows.cases, in order. A
 * `#` begins a comment, and a line that ends in `\` continues on the next.
 */
static int read_cases(const struct np_context *c, struct np_case **cases, size_t *n)
{
    static const char *const files[] = {"probes.cases", "musl.cases", "rows.cases"};
    int bad = 0;

    for (size_t f = 0; !bad && f < 3; f++) {
        char path[8192], line[8192];
        struct jbuf pending = {0};
        snprintf(path, sizeof(path), "%s/tests/native/%s", c->tree, files[f]);
        FILE *in = fopen(path, "r");
        if (in == NULL) {
            fprintf(stderr, "etude prepare native: %s: %s\n", path, strerror(errno));
            return -1;
        }
        while (!bad && fgets(line, sizeof(line), in)) {
            size_t len = strcspn(line, "#\n");
            if (line[len] == '\0' && !feof(in)) {
                fprintf(stderr, "etude prepare native: %s: line too long\n", path);
                bad = 1;
                break;
            }
            while (len && strchr(" \t\r", line[len - 1]))
                len--;
            line[len] = 0;
            if (len && line[len - 1] == '\\') {
                line[len - 1] = ' ';
                jbuf_puts(&pending, line);
                continue;
            }
            jbuf_puts(&pending, line);
            if (pending.len == 0)
                continue;
            bad = case_line(pending.data, cases, n, files[f]);
            pending.len = 0;
            pending.data[0] = 0;
        }
        if (!bad && pending.len) {
            fprintf(stderr, "etude prepare native: %s: continued line has no end\n", path);
            bad = 1;
        }
        fclose(in);
        jbuf_free(&pending);
    }
    return bad ? -1 : 0;
}

/* ---- The tree's inputs. ---- */

static void program_path(const struct np_context *c, const char *name, char path[4096])
{
    if (!strncmp(name, "native/", 7) || !strncmp(name, "system/", 7))
        snprintf(path, 4096, "tests/%s", name);
    else
        snprintf(path, 4096, "%s/tests/%s", c->build, name);
}

static int has_suffix(const char *name, const char *suffix)
{
    size_t n = strlen(name), m = strlen(suffix);
    return n >= m && !strcmp(name + n - m, suffix);
}

/* Add the files below `dir` of the tree whose names end in one of
 * `suffixes`, a list ended by NULL; a directory is searched when `deep`. */
static int add_files(const struct np_context *c, struct paths *p, const char *dir,
                     const char *const *suffixes, int deep)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s/%s", c->tree, dir);
    DIR *d = opendir(path);
    if (d == NULL)
        return -1;
    int bad = 0;
    struct dirent *e;
    while (!bad && (e = readdir(d)) != NULL) {
        char relative[4096];
        struct stat st;
        if (e->d_name[0] == '.' && (!e->d_name[1] || !strcmp(e->d_name, "..")))
            continue;
        if (snprintf(relative, sizeof(relative), "%s/%s", dir, e->d_name) >=
            (int)sizeof(relative)) {
            bad = 1;
            break;
        }
        snprintf(path, sizeof(path), "%s/%s", c->tree, relative);
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (deep)
                bad = add_files(c, p, relative, suffixes, deep);
            continue;
        }
        for (size_t i = 0; S_ISREG(st.st_mode) && suffixes[i]; i++)
            if (has_suffix(e->d_name, suffixes[i])) {
                add(p, relative);
                break;
            }
    }
    closedir(d);
    return bad ? -1 : 0;
}

/**
 * Copy the collection's inputs into `tree`, the payload's copy of the
 * source tree: each case's program and staged files, the driver and
 * etude-guest, the inventory, and the sources of a local TinyCC build of the
 * driver. Programs are made executable.
 *
 * @return 0 with the copies that etude-guest must find by name, the
 *         sources aside, in `staged`, or -1.
 */
static int copy_inputs(const struct np_context *c, const char *tree, const struct np_case *cases,
                       size_t n, struct paths *staged)
{
    struct paths selected = {0}, programs = {0};
    char path[4096];
    static const char *const fixed[] = {"tests/native/tccbuild.sh",
                                        "tests/native/bounds.json",
                                        "tools/etude/schemas/history.schema.json",
                                        "tests/native/probes.cases",
                                        "tests/native/musl.cases",
                                        "tests/native/rows.cases",
                                        "tests/native/maps",
                                        "tests/native/Readme.md"};

    for (size_t i = 0; i < n; i++) {
        program_path(c, cases[i].argv[0], path);
        add(&programs, path);
        add(&selected, path);
        for (size_t j = 0; j < cases[i].nfixtures; j++) {
            const char *f = cases[i].fixtures[j];
            if (!strncmp(f, "stage:", 6)) {
                char *list = np_strdup(f + 6), *save = NULL;
                for (char *e = strtok_r(list, ",", &save); e; e = strtok_r(NULL, ",", &save)) {
                    program_path(c, e, path);
                    add(&selected, path);
                }
                free(list);
            } else if (!strncmp(f, "file:", 5))
                add(&selected, f + 5);
            else if (!strncmp(f, "pty:", 4) && f[4]) {
                snprintf(path, sizeof(path), "tests/native/%s", f + 4);
                add(&selected, path);
            }
        }
    }
    snprintf(path, sizeof(path), "%s/tests/native/etude", c->build);
    add(&selected, path);
    snprintf(path, sizeof(path), "%s/tools/etude/guest/etude-guest", c->build);
    add(&selected, path);
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
        add(&selected, fixed[i]);
    for (size_t i = 0; i < selected.n; i++)
        add(staged, selected.v[i]);
    static const char *const sources[] = {".c", ".h", ".mk", NULL};
    static const char *const scripts[] = {".sh", NULL};
    int bad = add_files(c, &selected, "tests/native", sources, 1) ||
              add_files(c, &selected, "tools/etude", sources, 1) ||
              add_files(c, &selected, "vendor/gnu/agents/packages", scripts, 0);
    add(&selected, "utils/sha256.c");
    add(&selected, "utils/sha256.h");
    paths_sort(&selected);
    paths_sort(staged);
    for (size_t i = 0; !bad && i < selected.n; i++) {
        char from[8192], to[8192];
        int executable = 0;
        for (size_t j = 0; j < programs.n; j++)
            executable = executable || !strcmp(programs.v[j], selected.v[i]);
        snprintf(from, sizeof(from), "%s/%s", c->tree, selected.v[i]);
        snprintf(to, sizeof(to), "%s/%s", tree, selected.v[i]);
        bad = copy_file(from, to, executable);
    }
    paths_free(&selected);
    paths_free(&programs);
    return bad ? -1 : 0;
}

/**
 * Link each staged input into the first of the stage directories that has
 * no file of its name, or that has the same file already. Different files
 * that share a final name thus occupy different directories.
 */
static int stage_inputs(const char *stage, const char *tree, const struct paths *staged,
                        const char *images, const struct paths *image_files)
{
    int bad = mkdir(stage, 0777) != 0;
    for (int lane = 1; !bad && lane < LANES; lane++) {
        char dir[4096];
        bad = np_path(dir, sizeof(dir), "%s/%d", stage, lane) || mkdir(dir, 0777) != 0;
    }
    for (size_t i = 0; !bad && i < staged->n + image_files->n; i++) {
        char file[8192], wanted[65], target[8192], have[65];
        const char *relative = i < staged->n ? staged->v[i] : image_files->v[i - staged->n];
        if (np_path(file, sizeof(file), "%s/%s", i < staged->n ? tree : images, relative))
            return -1;
        const char *name = strrchr(file, '/') + 1;
        int placed = 0;
        bad = np_file_hash(file, wanted);
        for (int lane = 0; !bad && !placed && lane < LANES; lane++) {
            if (lane ? np_path(target, sizeof(target), "%s/%d/%s", stage, lane, name)
                     : np_path(target, sizeof(target), "%s/%s", stage, name))
                return -1;
            if (access(target, F_OK) != 0) {
                bad = link(file, target) != 0;
                placed = 1;
            } else if (np_file_hash(target, have) == 0 && !strcmp(have, wanted))
                placed = 1;
        }
        if (!bad && !placed) {
            fprintf(stderr, "etude prepare native: more than %d staged inputs named %s\n", LANES,
                    name);
            bad = 1;
        }
    }
    return bad ? -1 : 0;
}

/* ---- The records. ---- */

static int list_files(const char *root, const char *dir, struct paths *p)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s%s%s", root, *dir ? "/" : "", dir);
    DIR *d = opendir(path);
    if (d == NULL)
        return -1;
    int bad = 0;
    struct dirent *e;
    while (!bad && (e = readdir(d)) != NULL) {
        char relative[4096];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(relative, sizeof(relative), "%s%s%s", dir, *dir ? "/" : "", e->d_name);
        snprintf(path, sizeof(path), "%s/%s", root, relative);
        if (lstat(path, &st) != 0)
            bad = 1;
        else if (S_ISDIR(st.st_mode))
            bad = list_files(root, relative, p);
        else if (S_ISREG(st.st_mode))
            add(p, relative);
    }
    closedir(d);
    return bad ? -1 : 0;
}

static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    int bad = fputs(text, f) == EOF;
    if (fclose(f))
        bad = 1;
    return bad ? -1 : 0;
}

static int write_json(const char *path, const struct json *value)
{
    struct jbuf text = {0};
    json_canonical(value, &text);
    jbuf_puts(&text, "\n");
    int bad = write_file(path, text.data);
    jbuf_free(&text);
    return bad;
}

static struct json *value(enum json_type type)
{
    struct json *v = json_alloc(sizeof(*v));
    memset(v, 0, sizeof(*v));
    v->type = type;
    return v;
}

static void member(struct json *object, const char *name, struct json *v)
{
    struct json **items = realloc(object->item, (object->count + 1) * sizeof(*items));
    if (!items || !(v->name = strdup(name)))
        exit(1);
    object->item = items;
    object->item[object->count++] = v;
}

static struct json *text_value(const char *text)
{
    struct json *v = value(JSON_STRING);
    if (!(v->string = strdup(text)))
        exit(1);
    return v;
}

/* Record the commit of a clean source tree in native-source.json. */
static int source_record(struct np_context *c, const char *tree)
{
    struct jbuf status = {0}, head = {0};
    char path[8192];
    char *git_status[] = {c->tools[NP_GIT], "status", "--porcelain", "--untracked-files=all", NULL};
    char *git_head[] = {c->tools[NP_GIT], "rev-parse", "HEAD", NULL};
    int bad = np_run(c, git_status, &status);
    if (!bad && status.len) {
        fprintf(stderr, "etude prepare native: native source must be clean before preparation\n");
        bad = 1;
    }
    bad = bad || np_run(c, git_head, &head);
    if (!bad) {
        head.data[strcspn(head.data, "\n")] = 0;
        if (strlen(head.data) != 40 || strspn(head.data, "0123456789abcdef") != 40) {
            fprintf(stderr, "etude prepare native: native source has no exact commit\n");
            bad = 1;
        }
    }
    if (!bad) {
        struct json *record = value(JSON_OBJECT);
        member(record, "commit", text_value(head.data));
        snprintf(path, sizeof(path), "%s/native-source.json", tree);
        bad = write_json(path, record);
        json_free(record);
    }
    jbuf_free(&status);
    jbuf_free(&head);
    return bad ? -1 : 0;
}

/* Write assets.sha256, the digest of every file of the payload's tree. */
static int asset_list(const char *tree)
{
    struct paths files = {0};
    struct jbuf text = {0};
    char path[8192], hex[65];
    int bad = list_files(tree, "", &files);
    paths_sort(&files);
    for (size_t i = 0; !bad && i < files.n; i++) {
        snprintf(path, sizeof(path), "%s/%s", tree, files.v[i]);
        bad = np_file_hash(path, hex);
        jbuf_puts(&text, hex);
        jbuf_puts(&text, "  ");
        jbuf_puts(&text, files.v[i]);
        jbuf_puts(&text, "\n");
    }
    snprintf(path, sizeof(path), "%s/assets.sha256", tree);
    bad = bad || write_file(path, text.data ? text.data : "");
    paths_free(&files);
    jbuf_free(&text);
    return bad ? -1 : 0;
}

/* ---- The entry. ---- */

/* Find `name` on PATH, or in the other directories of `extra`, ended by
 * NULL; a name with a slash is taken as it is, relative to the tree. */
static char *program(const struct np_context *c, const char *name, const char *const *extra)
{
    char path[8192];
    if (strchr(name, '/')) {
        if (np_path(path, sizeof(path), "%s%s%s", name[0] == '/' ? "" : c->tree,
                    name[0] == '/' ? "" : "/", name))
            return NULL;
        return access(path, X_OK) == 0 ? np_strdup(path) : NULL;
    }
    char *search = np_strdup(getenv("PATH") ? getenv("PATH") : ""), *save = NULL;
    char *found = NULL;
    for (char *dir = strtok_r(search, ":", &save); dir && !found; dir = strtok_r(NULL, ":", &save))
        if (snprintf(path, sizeof(path), "%s/%s", dir, name) < (int)sizeof(path) &&
            access(path, X_OK) == 0)
            found = np_strdup(path);
    for (size_t i = 0; extra && extra[i] && !found; i++)
        if (snprintf(path, sizeof(path), "%s/%s", extra[i], name) < (int)sizeof(path) &&
            access(path, X_OK) == 0)
            found = np_strdup(path);
    free(search);
    return found;
}

/* Choose the tools: one directory that has mke2fs, debugfs and e2fsck, as
 * tools/setup/e2fsprogs_path.sh chooses, unless an option names each. */
static int choose_tools(struct np_context *c, char *const given[NP_TOOLS])
{
    static const char *const names[NP_TOOLS] = {"mke2fs",
                                                "debugfs",
                                                "e2fsck",
                                                "mformat",
                                                "build/krffs/mkfs.krffs",
                                                "build/krffs/stageimage.krffs",
                                                "build/tests/host/mkexfatfixture",
                                                "git",
                                                "gzip"};
    static const char *const sbin[] = {"/opt/homebrew/opt/e2fsprogs/sbin",
                                       "/usr/local/opt/e2fsprogs/sbin", "/sbin", "/usr/sbin", NULL};
    struct paths dirs = {0};
    char *path = np_strdup(getenv("PATH") ? getenv("PATH") : ""), *save = NULL, tool[8192];
    const char *e2fs = NULL;
    for (char *d = strtok_r(path, ":", &save); d; d = strtok_r(NULL, ":", &save))
        add(&dirs, d);
    for (size_t i = 0; sbin[i]; i++)
        add(&dirs, sbin[i]);
    free(path);
    for (size_t d = 0; d < dirs.n && !e2fs; d++) {
        int all = 1;
        for (size_t i = NP_MKE2FS; i <= NP_E2FSCK; i++)
            all = all &&
                  snprintf(tool, sizeof(tool), "%s/%s", dirs.v[d], names[i]) < (int)sizeof(tool) &&
                  access(tool, X_OK) == 0;
        if (all)
            e2fs = dirs.v[d];
    }
    int bad = 0;
    for (size_t i = 0; i < NP_TOOLS; i++) {
        const char *name = given[i] ? given[i] : names[i];
        if (!given[i] && i <= NP_E2FSCK && e2fs) {
            snprintf(tool, sizeof(tool), "%s/%s", e2fs, names[i]);
            name = tool;
        }
        c->tools[i] = program(c, name, sbin);
        if (!c->tools[i]) {
            fprintf(stderr, "etude prepare native: requires %s\n", name);
            bad = 1;
        }
    }
    paths_free(&dirs);
    return bad ? -1 : 0;
}

/* The tools' environment: the caller's, with e2fsprogs' times fixed and the
 * stager's map of build paths for x86-64. */
static char **environment(const struct np_context *c)
{
    size_t n = 0, k = 0;
    while (environ[n])
        n++;
    char **env = calloc(n + 3, sizeof(*env)), map[8192];
    if (!env)
        exit(1);
    for (size_t i = 0; i < n; i++)
        if (strncmp(environ[i], "E2FSPROGS_FAKE_TIME=", 20) &&
            strncmp(environ[i], "RACH2_PATH_MAP=", 15))
            env[k++] = np_strdup(environ[i]);
    env[k++] = np_strdup("E2FSPROGS_FAKE_TIME=1");
    if (!strcmp(c->arch, "i686"))
        snprintf(map, sizeof(map), "RACH2_PATH_MAP=");
    else
        snprintf(map, sizeof(map),
                 "RACH2_PATH_MAP=build/man/=build/man/ build/tests/system/=build/tests/system/ "
                 "build/=%s/ vendor/musl/install/=%s/vendor-work/musl/install/",
                 c->build, c->build);
    env[k++] = np_strdup(map);
    return env;
}

static void usage(void)
{
    fputs("usage: etude prepare native --architecture i686|x86_64 --output DIR\n"
          "           [--mke2fs PATH] [--debugfs PATH] [--e2fsck PATH] [--mformat PATH]\n",
          stderr);
}

/* Say that the output is refused; return -1. */
static int refuse_output(void)
{
    fprintf(stderr, "etude prepare native: output must be below this tree's build directory\n");
    return -1;
}

/**
 * Give in `out` the absolute path of the output `given`, which must be below
 * the tree's build directory and have no `.` or `..` component.
 *
 * The longest part of the path that exists must also resolve, through any
 * symbolic links, to the build directory or below it, since preparation
 * makes the rest and removes an earlier preparation at the path. Only a
 * tree with no build directory yet may leave the tree itself as that part.
 *
 * @return 0, or -1 after a message.
 */
int np_output_path(const struct np_context *c, const char *given, char out[4096])
{
    char build[4096], prefix[4096], real_build[4096], real[4096];
    struct stat st;

    if (snprintf(out, 4096, "%s%s%s", given[0] == '/' ? "" : c->tree, given[0] == '/' ? "" : "/",
                 given) >= 4096)
        return -1;
    size_t n = strlen(c->tree);
    if (strncmp(out, c->tree, n) || strncmp(out + n, "/build/", 7) || strstr(out, "/./") ||
        strstr(out, "/../") || has_suffix(out, "/.") || has_suffix(out, "/..") ||
        has_suffix(out, "/"))
        return refuse_output();
    if (snprintf(build, sizeof(build), "%s/build", c->tree) >= (int)sizeof(build))
        return refuse_output();
    strcpy(prefix, out);
    while (lstat(prefix, &st) != 0) {
        if (errno != ENOENT)
            return refuse_output();
        *strrchr(prefix, '/') = 0;
    }
    if (strlen(prefix) < strlen(build))
        return 0;
    if (!realpath(build, real_build) || !realpath(prefix, real))
        return refuse_output();
    size_t b = strlen(real_build);
    if (strncmp(real, real_build, b) || (real[b] != 0 && real[b] != '/'))
        return refuse_output();
    return 0;
}

/* Prepare the payload in the work directory, then replace `destination`,
 * its archive and the archive's digest with it. */
/* Copy the inputs, make the images and stage the lanes under `payload`. */
static int build_payload(struct np_context *c, const char *payload, const struct np_case *cases,
                         size_t n)
{
    char tree[4096], stage[4096], images[8192];
    struct paths staged = {0}, image_files = {0};

    if (np_path(tree, sizeof(tree), "%s/tree", payload) ||
        np_path(stage, sizeof(stage), "%s/stage", payload) ||
        np_path(images, sizeof(images), "%s/%s/tests/native/images", tree, c->build))
        return -1;
    int bad = copy_inputs(c, tree, cases, n, &staged) || make_parents(images) ||
              np_images(c, images, cases, n) || list_files(images, "", &image_files);
    if (!bad) {
        for (size_t i = 0; i < image_files.n; i++)
            if (!strcmp(image_files.v[i], "roots")) {
                free(image_files.v[i]);
                image_files.v[i] = image_files.v[--image_files.n];
            }
        paths_sort(&image_files);
        bad = stage_inputs(stage, tree, &staged, images, &image_files) || source_record(c, tree) ||
              asset_list(tree);
    }
    paths_free(&staged);
    paths_free(&image_files);
    return bad ? -1 : 0;
}

/*
 * Write the payload's prepared.json, archive the payload as `archive`, and
 * write the record again with the archive's SHA-256, which is left in `hex`.
 */
static int write_record(struct np_context *c, const char *payload, const char *archive,
                        const struct np_case *cases, size_t n, char hex[66])
{
    char path[8192];
    struct json *record = value(JSON_OBJECT), *names = value(JSON_ARRAY),
                *version = value(JSON_INTEGER);

    version->integer = 1;
    for (size_t i = 0; i < n; i++) {
        struct json **items = realloc(names->item, (names->count + 1) * sizeof(*items));
        if (!items)
            exit(1);
        names->item = items;
        names->item[names->count++] = text_value(cases[i].name);
    }
    snprintf(path, sizeof(path), "%s/tree/tests/native/maps", payload);
    int bad = np_file_hash(path, hex);
    member(record, "schema_version", version);
    member(record, "architecture", text_value(c->arch));
    member(record, "cases", names);
    member(record, "account", text_value("maps"));
    member(record, "maps_sha256", text_value(hex));
    snprintf(path, sizeof(path), "tree/%s/tests/native/images/roots", c->build);
    member(record, "roots", text_value(path));
    snprintf(path, sizeof(path), "%s/prepared.json", payload);
    bad = bad || write_json(path, record) || (c->stop && *c->stop) ||
          np_archive(c, payload, archive, hex);
    if (!bad) {
        member(record, "archive_sha256", text_value(hex));
        bad = write_json(path, record);
    }
    json_free(record);
    return bad ? -1 : 0;
}

/*
 * Replace `destination`, which must be absent or an earlier preparation,
 * with `payload`, and put the archive and its digest file beside it.
 */
static int publish(const char *payload, const char *archive, const char *destination, char hex[66])
{
    char path[8192];
    struct stat st;

    snprintf(path, sizeof(path), "%s/prepared.json", destination);
    if (lstat(destination, &st) == 0 && stat(path, &st) != 0) {
        fprintf(stderr, "etude prepare native: %s: existing output is not a native preparation\n",
                destination);
        return -1;
    }
    if (np_remove(destination) || rename(payload, destination))
        return -1;
    snprintf(path, sizeof(path), "%s.tar.gz", destination);
    if (rename(archive, path))
        return -1;
    snprintf(path, sizeof(path), "%s.tar.gz.sha256", destination);
    strcat(hex, "\n");
    return write_file(path, hex);
}

static int prepare(struct np_context *c, const char *destination)
{
    char payload[4096], archive[4096], hex[66];
    struct np_case *cases = NULL;
    size_t n = 0;

    if (np_path(payload, sizeof(payload), "%s/payload", c->work) ||
        np_path(archive, sizeof(archive), "%s/prepared.tar.gz", c->work))
        return -1;
    int bad = mkdir(payload, 0777) || read_cases(c, &cases, &n) ||
              build_payload(c, payload, cases, n) ||
              write_record(c, payload, archive, cases, n, hex) ||
              publish(payload, archive, destination, hex);
    if (!bad)
        printf("etude prepare native: %zu cases in %s\n", n, destination);
    else if (c->stop && *c->stop)
        fprintf(stderr, "etude prepare native: stopped\n");
    cases_free(cases, n);
    return bad ? -1 : 0;
}

/**
 * Run `etude prepare native --architecture ARCH --output DIR`: prepare the
 * payload of the local collection for ARCH in DIR, with DIR.tar.gz and
 * DIR.tar.gz.sha256 beside it. Run from the root of a clean tree that has
 * built ARCH's guest programs, the driver and the KRFFS tools.
 *
 * @return 0, 1 when preparation failed or was stopped, or 2 for a usage
 *         error.
 */
/* Read the options into `c`, `given` and `output`; return 0, or -1 after
 * the usage message. */
static int arguments(int argc, char **argv, struct np_context *c, char *given[NP_TOOLS],
                     const char **output)
{
    static const char *const options[] = {"--mke2fs", "--debugfs", "--e2fsck", "--mformat"};

    for (int i = 1; i < argc; i++) {
        int known = 0;
        if (i + 1 < argc && !strcmp(argv[i], "--architecture"))
            c->arch = argv[++i], known = 1;
        else if (i + 1 < argc && !strcmp(argv[i], "--output"))
            *output = argv[++i], known = 1;
        for (size_t t = 0; !known && t < 4; t++)
            if (i + 1 < argc && !strcmp(argv[i], options[t]))
                given[t] = argv[++i], known = 1;
        if (!known) {
            usage();
            return -1;
        }
    }
    if (!c->arch || !*output || (strcmp(c->arch, "i686") && strcmp(c->arch, "x86_64"))) {
        usage();
        return -1;
    }
    return 0;
}

int etude_native_prepare_main(int argc, char **argv, volatile sig_atomic_t *stop)
{
    struct np_context c;
    char *given[NP_TOOLS] = {0}, destination[4096], parent[4096];
    const char *output = NULL;

    memset(&c, 0, sizeof(c));
    c.stop = stop;
    if (arguments(argc, argv, &c, given, &output))
        return 2;
    c.build = !strcmp(c.arch, "x86_64") ? "build/x86_64" : "build";
    if (!getcwd(c.tree, sizeof(c.tree)) || np_output_path(&c, output, destination) ||
        choose_tools(&c, given))
        return 1;
    strcpy(parent, destination);
    *strrchr(parent, '/') = 0;
    if (snprintf(c.work, sizeof(c.work), "%s/rach2-native-prepare.XXXXXX", parent) >=
            (int)sizeof(c.work) ||
        make_parents(c.work) || !mkdtemp(c.work)) {
        fprintf(stderr, "etude prepare native: %s: no work directory\n", parent);
        return 1;
    }
    c.environment = environment(&c);
    int bad = prepare(&c, destination);
    if (np_remove(c.work)) {
        fprintf(stderr, "etude prepare native: %s: not removed\n", c.work);
        bad = 1;
    }
    for (size_t i = 0; c.environment[i]; i++)
        free(c.environment[i]);
    free(c.environment);
    for (size_t i = 0; i < NP_TOOLS; i++)
        free(c.tools[i]);
    return bad ? 1 : 0;
}
