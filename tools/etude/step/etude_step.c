/*
 * etude_step.c: etude-step, which runs one step of the build, such as a
 * vendored project's build script, through the host's store of step outputs.
 * tools/etude/Readme.md gives its usage and the store's layout.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sha256.h"

/* The longest path the program makes, and the most operands of one kind. */
#define PATH_LEN 4096
#define LIST_MAX 512

/* The seconds after which a directory left in the store's tmp/ is taken to
 * be one whose maker died, and removed. */
#define STALE_SECONDS (24L * 60 * 60)

/* A growing string. */
struct text {
    char *s;
    size_t len;
    size_t cap;
};

/* The operands of one kind, in the order given. */
struct list {
    const char *v[LIST_MAX];
    int n;
};

/* The step: its options, and what the program works out from them. */
struct step {
    const char *store; /* the store's directory; NULL or "" for none */
    const char *name;  /* the step's name in the record and the messages */
    const char *stamp; /* the stamp the step writes, with its key */
    const char *arch;  /* the architecture, or NULL */
    const char *root;  /* link mode: the step's root in the copy; else NULL */
    const char *days;  /* -e: the days unused after which an entry may go */
    int print;         /* -p: print the record and the key, and run nothing */
    struct list cc;    /* compilers whose identity counts, and ccache wraps */
    struct list wrap;  /* compilers ccache wraps whose identity does not count */
    struct list in;    /* input files and directories */
    struct list dep;   /* stamps of the steps this one uses */
    struct list use;   /* link mode: outputs of those steps, under root */
    struct list out;   /* outputs, under root in link mode */
    struct list skip;  /* names left out of a copied output */
    char **argv;       /* the command */

    struct text record; /* the record, whose SHA-256 is the key */
    char key[65];
    int keyed;             /* every stamp of -d had a key */
    char here[PATH_LEN];   /* the copy's root: the working directory */
    char shelf[PATH_LEN];  /* the store, its links resolved */
    char ccache[PATH_LEN]; /* ccache, or "" when none is used */
    char shims[PATH_LEN];  /* the directory of ccache's compiler names */
};

/* The signal that asked the program to stop, or 0. */
static volatile sig_atomic_t stop_signal;

static void on_signal(int sig)
{
    stop_signal = sig;
}

static int usage(void)
{
    fprintf(stderr, "usage: etude-step [-p] [-s STORE] -n NAME -t STAMP [-a ARCH] [-c CC]... "
                    "[-w CC]...\n"
                    "                  [-i FILE]... [-d STAMP]... [-r ROOT [-u PATH]...] "
                    "[-x NAME]...\n"
                    "                  -o PATH... -- COMMAND [ARG...]\n"
                    "       etude-step [-p] -s STORE -e DAYS\n");
    return 2;
}

static void say(const struct step *s, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "etude-step: %s: ", s->name);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/** Append the formatted text to `t`; 0, or -1 when memory runs out. */
static int text_add(struct text *t, const char *fmt, ...)
{
    va_list ap;
    int n;

    for (;;) {
        size_t room = t->cap - t->len;

        va_start(ap, fmt);
        n = vsnprintf(t->s ? t->s + t->len : NULL, t->s ? room : 0, fmt, ap);
        va_end(ap);
        if (n < 0)
            return -1;
        if (t->s && (size_t)n < room) {
            t->len += (size_t)n;
            return 0;
        }
        {
            size_t cap = t->cap ? t->cap * 2 : 4096;
            char *p;

            while (cap - t->len <= (size_t)n)
                cap *= 2;
            if ((p = realloc(t->s, cap)) == NULL)
                return -1;
            t->s = p;
            t->cap = cap;
        }
    }
}

/** Format a path into `out`; 0, or -1 when it does not fit. */
static int path_fmt(char *out, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(out, PATH_LEN, fmt, ap);
    va_end(ap);
    return n < 0 || n >= PATH_LEN ? -1 : 0;
}

static int by_string(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/** A path the store may name: relative, without `.` or `..` parts, and
 * without an empty one. */
static int clean_path(const char *p)
{
    const char *c = p;

    if (*p == '\0' || *p == '/')
        return 0;
    while (*c) {
        const char *e = strchr(c, '/');
        size_t n = e ? (size_t)(e - c) : strlen(c);

        if (n == 0 || (n == 1 && c[0] == '.') || (n == 2 && c[0] == '.' && c[1] == '.'))
            return 0;
        c += n;
        if (*c == '/')
            c++;
    }
    return p[strlen(p) - 1] != '/';
}

/** 1 when `a` is `b` or a directory above it. */
static int covers(const char *a, const char *b)
{
    size_t n = strlen(a);

    return strncmp(a, b, n) == 0 && (b[n] == '\0' || b[n] == '/');
}

/** Make each directory above `path`. */
static int make_parents(const char *path)
{
    char p[PATH_LEN];
    char *c;

    if (path_fmt(p, "%s", path) != 0)
        return -1;
    for (c = p + 1; *c; c++) {
        if (*c != '/')
            continue;
        *c = '\0';
        if (mkdir(p, 0777) != 0 && errno != EEXIST)
            return -1;
        *c = '/';
    }
    return 0;
}

/** The SHA-256 of the file `fd` as hex in `hex`; 0, or -1 on a read error. */
static int hash_fd(int fd, char hex[65])
{
    struct sha256 h;
    unsigned char buf[65536];
    unsigned char d[32];
    ssize_t n;

    sha256_init(&h);
    while ((n = read(fd, buf, sizeof(buf))) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sha256_update(&h, buf, (size_t)n);
    }
    sha256_final(&h, d);
    sha256_hex(d, hex);
    return 0;
}

static void hash_text(const char *p, size_t n, char hex[65])
{
    struct sha256 h;
    unsigned char d[32];

    sha256_init(&h);
    sha256_update(&h, (const unsigned char *)p, n);
    sha256_final(&h, d);
    sha256_hex(d, hex);
}

/* The lines of the record's inputs, sorted before they are added. */
struct lines {
    char **v;
    size_t n;
    size_t cap;
};

static int lines_add(struct lines *l, const char *fmt, ...)
{
    va_list ap;
    char buf[PATH_LEN + 200];
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(buf))
        return -1;
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 256;
        char **v = realloc(l->v, cap * sizeof(*v));

        if (v == NULL)
            return -1;
        l->v = v;
        l->cap = cap;
    }
    if ((l->v[l->n] = malloc((size_t)n + 1)) == NULL)
        return -1;
    memcpy(l->v[l->n++], buf, (size_t)n + 1);
    return 0;
}

static void lines_free(struct lines *l)
{
    size_t i;

    for (i = 0; i < l->n; i++)
        free(l->v[i]);
    free(l->v);
}

/**
 * Add the line of each file under the input `path` to `l`, as `PATH input
 * SHA` so that a sort orders them by path; a directory is read whole, and a
 * symbolic link inside one counts by the path it names.
 */
static int hash_input(const struct step *s, struct lines *l, const char *path, int top)
{
    struct stat st;
    char hex[65];

    if ((top ? stat(path, &st) : lstat(path, &st)) != 0) {
        say(s, "input %s: %s", path, strerror(errno));
        return -1;
    }
    if (S_ISLNK(st.st_mode)) {
        char to[PATH_LEN];
        ssize_t n = readlink(path, to, sizeof(to) - 1);

        if (n < 0) {
            say(s, "input %s: %s", path, strerror(errno));
            return -1;
        }
        to[n] = '\0';
        hash_text(to, (size_t)n, hex);
        return lines_add(l, "%s\tlink %s", path, hex);
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        int rc = 0;

        if (d == NULL) {
            say(s, "input %s: %s", path, strerror(errno));
            return -1;
        }
        while (rc == 0 && (e = readdir(d)) != NULL) {
            char sub[PATH_LEN];

            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            if (path_fmt(sub, "%s/%s", path, e->d_name) != 0)
                rc = -1;
            else
                rc = hash_input(s, l, sub, 0);
        }
        closedir(d);
        return rc;
    }
    if (S_ISREG(st.st_mode)) {
        int fd = open(path, O_RDONLY);
        int rc;

        if (fd < 0) {
            say(s, "input %s: %s", path, strerror(errno));
            return -1;
        }
        rc = hash_fd(fd, hex);
        close(fd);
        if (rc != 0) {
            say(s, "input %s: %s", path, strerror(errno));
            return -1;
        }
        return lines_add(l, "%s\tinput %s", path, hex);
    }
    say(s, "input %s is not a file or a directory", path);
    return -1;
}

/**
 * Read the key from the stamp `path` into `key`: 1 when the stamp is one this
 * program wrote, `etude-step 1 KEY ORIGIN`; 0 when it is not; -1 when it
 * cannot be read.
 */
static int stamp_key(const char *path, char key[65], char origin[16])
{
    char buf[256];
    FILE *f = fopen(path, "r");
    char k[65], o[16];

    if (f == NULL)
        return -1;
    if (fgets(buf, sizeof(buf), f) == NULL)
        buf[0] = '\0';
    fclose(f);
    if (sscanf(buf, "etude-step 1 %64[0-9a-f] %15[a-z]", k, o) != 2 || strlen(k) != 64)
        return 0;
    memcpy(key, k, 65);
    if (origin)
        memcpy(origin, o, 16);
    return 1;
}

/**
 * The identity of the compiler `cc`: the SHA-256 of what `cc -v` prints, less
 * the line that says by which name it was run.
 */
static int compiler_id(const struct step *s, const char *cc, char hex[65])
{
    struct text cmd = {0}, said = {0};
    char line[4096];
    FILE *p;
    int status;

    if (text_add(&cmd, "%s -v 2>&1", cc) != 0 || (p = popen(cmd.s, "r")) == NULL) {
        free(cmd.s);
        say(s, "compiler %s cannot be run", cc);
        return -1;
    }
    free(cmd.s);
    while (fgets(line, sizeof(line), p) != NULL)
        if (strncmp(line, "COLLECT_GCC=", 12) != 0 && text_add(&said, "%s", line) != 0)
            break;
    status = pclose(p);
    if (status != 0 || said.len == 0) {
        free(said.s);
        say(s, "compiler %s: `%s -v` failed", cc, cc);
        return -1;
    }
    hash_text(said.s, said.len, hex);
    free(said.s);
    return 0;
}

/** Add the lines `l` to `t` sorted, each once. A line `SORT\tTEXT` is sorted
 * by SORT and written as `TEXT SORT`. */
static void add_sorted(struct text *t, struct lines *l)
{
    size_t i;

    if (l->n)
        qsort(l->v, l->n, sizeof(*l->v), by_string);
    for (i = 0; i < l->n; i++) {
        char *tab = strchr(l->v[i], '\t');

        if (i > 0 && strcmp(l->v[i], l->v[i - 1]) == 0)
            continue;
        if (tab == NULL) {
            text_add(t, "%s\n", l->v[i]);
            continue;
        }
        *tab = '\0';
        text_add(t, "%s %s\n", tab + 1, l->v[i]);
        *tab = '\t';
    }
}

/** Add to `l` a line for the key of each stamp of -d, or `none` for a stamp
 * with no key, which keeps the step out of the store. */
static int add_depends(struct step *s, struct lines *l)
{
    int k;

    for (k = 0; k < s->dep.n; k++) {
        char key[65];
        int got = stamp_key(s->dep.v[k], key, NULL);

        if (got < 0) {
            say(s, "stamp %s: %s", s->dep.v[k], strerror(errno));
            return -1;
        }
        if (got == 0) {
            s->keyed = 0;
            memcpy(key, "none", 5);
        }
        if (lines_add(l, "%s\tdepends %s", s->dep.v[k], key) != 0)
            return -1;
    }
    return 0;
}

/** Add to `t` a line `WHAT NAME` for each name of `list`, sorted, each once. */
static void add_names(struct text *t, const char *what, const struct list *list)
{
    const char *v[LIST_MAX];
    int k, n = list->n;

    memcpy(v, list->v, (size_t)n * sizeof(*v));
    if (n)
        qsort(v, (size_t)n, sizeof(*v), by_string);
    for (k = 0; k < n; k++)
        if (k == 0 || strcmp(v[k], v[k - 1]) != 0)
            text_add(t, "%s %s\n", what, v[k]);
}

/**
 * Write the record of the step, from which its key comes. Its lines are, in
 * this order: the format, the step's name, the architecture, the platform,
 * each compiler's identity, each input file by path, each step it uses by
 * key, and the root, uses, outputs and names left out, each sorted. A stamp
 * of -d with no key makes `keyed` 0.
 */
static int make_record(struct step *s)
{
    struct utsname u;
    struct lines l = {0}, c = {0};
    int k, rc = -1;

    s->keyed = 1;
    if (uname(&u) != 0) {
        say(s, "uname: %s", strerror(errno));
        return -1;
    }
    text_add(&s->record, "etude-step 1\nstep %s\n", s->name);
    if (s->arch)
        text_add(&s->record, "arch %s\n", s->arch);
    text_add(&s->record, "platform %s %s\n", u.sysname, u.machine);
    for (k = 0; k < s->cc.n; k++) {
        char hex[65];

        if (compiler_id(s, s->cc.v[k], hex) != 0 || lines_add(&c, "compiler %s", hex) != 0)
            goto out;
    }
    add_sorted(&s->record, &c);
    for (k = 0; k < s->in.n; k++)
        if (hash_input(s, &l, s->in.v[k], 1) != 0)
            goto out;
    if (add_depends(s, &l) != 0)
        goto out;
    add_sorted(&s->record, &l);
    if (s->root)
        text_add(&s->record, "root %s\n", s->root);
    add_names(&s->record, "use", &s->use);
    add_names(&s->record, "output", &s->out);
    add_names(&s->record, "skip", &s->skip);
    text_add(&s->record, "mode %s\n", s->root ? "link" : "copy");
    if (s->record.s == NULL) {
        say(s, "out of memory");
        goto out;
    }
    hash_text(s->record.s, s->record.len, s->key);
    rc = 0;
out:
    lines_free(&l);
    lines_free(&c);
    return rc;
}

/** Write the stamp: `etude-step 1 KEY ORIGIN`, ORIGIN `here` when the
 * outputs in the copy were built in it and `store` when they are the
 * store's. */
static int write_stamp(const struct step *s, const char *origin)
{
    char tmp[PATH_LEN];
    FILE *f;

    if (path_fmt(tmp, "%s.%ld", s->stamp, (long)getpid()) != 0 || make_parents(s->stamp) != 0 ||
        (f = fopen(tmp, "w")) == NULL) {
        say(s, "stamp %s cannot be written: %s", s->stamp, strerror(errno));
        return -1;
    }
    fprintf(f, "etude-step 1 %s %s\n", s->key, origin);
    if (fclose(f) != 0 || rename(tmp, s->stamp) != 0) {
        say(s, "stamp %s cannot be written: %s", s->stamp, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

/**
 * Remove `path` and everything under it. A directory of the caller's that it
 * may not write is made writable first, so that a sealed entry of its own
 * can go. 0, or -1 when something stayed.
 */
static int remove_tree(const char *path)
{
    struct stat st;
    int rc = 0;

    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d;
        struct dirent *e;

        if ((st.st_mode & S_IRWXU) != S_IRWXU)
            chmod(path, (st.st_mode & 07777) | S_IRWXU);
        if ((d = opendir(path)) == NULL)
            return -1;
        while ((e = readdir(d)) != NULL) {
            char sub[PATH_LEN];

            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            if (path_fmt(sub, "%s/%s", path, e->d_name) != 0 || remove_tree(sub) != 0)
                rc = -1;
        }
        closedir(d);
        return rmdir(path) == 0 ? rc : -1;
    }
    return unlink(path) == 0 ? 0 : -1;
}

/** 1 when the name `name` matches a pattern of -x. */
static int skipped(const struct step *s, const char *name)
{
    int k;

    for (k = 0; k < s->skip.n; k++)
        if (fnmatch(s->skip.v[k], name, 0) == 0)
            return 1;
    return 0;
}

/** Write the `n` bytes at `buf` to `fd`; -1 on an error. */
static int write_all(int fd, const unsigned char *buf, size_t n)
{
    size_t off = 0;

    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);

        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0)
            return -1;
        off += (size_t)w;
    }
    return 0;
}

/** Copy the regular file `src`, whose mode is `mode`, to the new file `dst`,
 * with its permissions less the set-id bits and with the owner's write bit. */
static int copy_file(const char *src, const char *dst, mode_t mode)
{
    static unsigned char buf[65536];
    int in, out, rc = 0;
    ssize_t n;

    if ((in = open(src, O_RDONLY)) < 0)
        return -1;
    if ((out = open(dst, O_WRONLY | O_CREAT | O_EXCL, (mode & 0777) | S_IWUSR)) < 0) {
        close(in);
        return -1;
    }
    while (rc == 0 && (n = read(in, buf, sizeof(buf))) != 0) {
        if (n < 0 && errno == EINTR)
            continue;
        rc = n < 0 ? -1 : write_all(out, buf, (size_t)n);
    }
    close(in);
    if (close(out) != 0)
        rc = -1;
    return rc;
}

static int copy_tree(const struct step *s, const char *src, const char *dst);

/** Copy the directory `src` to the new directory `dst`, less the names of
 * -x. */
static int copy_dir(const struct step *s, const char *src, const char *dst)
{
    DIR *d;
    struct dirent *e;
    int rc = 0;

    if (mkdir(dst, 0777) != 0 || (d = opendir(src)) == NULL) {
        say(s, "cannot copy %s to %s: %s", src, dst, strerror(errno));
        return -1;
    }
    while (rc == 0 && (e = readdir(d)) != NULL) {
        char a[PATH_LEN], b[PATH_LEN];

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 || skipped(s, e->d_name))
            continue;
        if (path_fmt(a, "%s/%s", src, e->d_name) != 0 || path_fmt(b, "%s/%s", dst, e->d_name) != 0)
            rc = -1;
        else
            rc = copy_tree(s, a, b);
    }
    closedir(d);
    return rc;
}

/**
 * Copy `src` to `dst`, which must not exist: a directory whole, less the
 * names of -x below it, a file with its bytes and its permissions less the
 * set-id bits and with the owner's write bit, and a symbolic link as a link.
 * The copy's times are the time it is made.
 */
static int copy_tree(const struct step *s, const char *src, const char *dst)
{
    struct stat st;
    char to[PATH_LEN];
    ssize_t n;

    if (stop_signal)
        return -1;
    if (lstat(src, &st) != 0)
        goto fail;
    if (S_ISDIR(st.st_mode))
        return copy_dir(s, src, dst);
    if (S_ISLNK(st.st_mode)) {
        if ((n = readlink(src, to, sizeof(to) - 1)) < 0)
            goto fail;
        to[n] = '\0';
        if (symlink(to, dst) != 0)
            goto fail;
        return 0;
    }
    if (S_ISREG(st.st_mode)) {
        if (copy_file(src, dst, st.st_mode) != 0)
            goto fail;
        return 0;
    }
    say(s, "%s is not a file, a directory or a link", src);
    return -1;
fail:
    if (!stop_signal)
        say(s, "cannot copy %s to %s: %s", src, dst, strerror(errno));
    return -1;
}

/**
 * Make the tree `path` read-only: every file and directory readable by all
 * and writable by none, and no file set-id. Links are left as they are.
 */
static int seal_tree(const char *path)
{
    struct stat st;
    mode_t m;

    if (lstat(path, &st) != 0)
        return -1;
    if (S_ISLNK(st.st_mode))
        return 0;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        int rc = 0;

        if (d == NULL)
            return -1;
        while (rc == 0 && (e = readdir(d)) != NULL) {
            char sub[PATH_LEN];

            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            rc = path_fmt(sub, "%s/%s", path, e->d_name) != 0 ? -1 : seal_tree(sub);
        }
        closedir(d);
        return rc == 0 && chmod(path, 0555) == 0 ? 0 : -1;
    }
    m = (st.st_mode & 0111) ? 0555 : 0444;
    return chmod(path, m) == 0 ? 0 : -1;
}

/** Find `needle` in the regular file `path` where it does not begin
 * `allow`: 1 when it is there, 0 when not, -1 on an error. */
static int find_in_file(const char *path, const char *needle, const char *allow)
{
    static unsigned char buf[65536 + PATH_LEN];
    size_t nl = strlen(needle), al = strlen(allow), m = nl > al ? nl : al;
    size_t have = 0, limit, i;
    ssize_t n;
    int fd, rc = 0;

    if (nl == 0 || m >= PATH_LEN || (fd = open(path, O_RDONLY)) < 0)
        return -1;
    for (;;) {
        if ((n = read(fd, buf + have, 65536)) < 0) {
            if (errno == EINTR)
                continue;
            rc = -1;
            break;
        }
        have += (size_t)n;
        limit = n == 0 ? have : have >= m ? have - m + 1 : 0;
        for (i = 0; i < limit && rc == 0; i++)
            if (i + nl <= have && memcmp(buf + i, needle, nl) == 0 &&
                !(al && i + al <= have && memcmp(buf + i, allow, al) == 0))
                rc = 1;
        if (n == 0 || rc)
            break;
        memmove(buf, buf + limit, have - limit);
        have -= limit;
    }
    close(fd);
    return rc;
}

/**
 * Find `needle` in the files under `path`, where it does not begin `allow`:
 * 1 with the first file's path in `where`, 0 when no file has it, -1 on an
 * error.
 */
static int find_text(const char *path, const char *needle, const char *allow, char *where)
{
    struct stat st;
    DIR *d;
    struct dirent *e;
    int rc = 0;

    if (lstat(path, &st) != 0)
        return -1;
    if (S_ISREG(st.st_mode)) {
        rc = find_in_file(path, needle, allow);
        if (rc == 1)
            path_fmt(where, "%s", path);
        return rc;
    }
    if (!S_ISDIR(st.st_mode))
        return 0;
    if ((d = opendir(path)) == NULL)
        return -1;
    while (rc == 0 && (e = readdir(d)) != NULL) {
        char sub[PATH_LEN];

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        rc = path_fmt(sub, "%s/%s", path, e->d_name) != 0 ? -1
                                                          : find_text(sub, needle, allow, where);
    }
    closedir(d);
    return rc;
}

/** Look for the copy's own path in the outputs under `base`, which may not be
 * shared when they name it, though they may name the store's; 0 when none
 * does. */
static int check_paths(const struct step *s, const char *base)
{
    char needle[PATH_LEN], allow[PATH_LEN], p[PATH_LEN], where[PATH_LEN];
    int k;

    if (path_fmt(needle, "%s/", s->here) != 0 || path_fmt(allow, "%s/", s->shelf) != 0)
        return -1;
    for (k = 0; k < s->out.n; k++) {
        int got;

        if (path_fmt(p, "%s/%s", base, s->out.v[k]) != 0)
            return -1;
        if ((got = find_text(p, needle, allow, where)) != 0) {
            if (got > 0)
                say(s, "%s has the path of this copy, %s, and is not shared",
                    where + strlen(base) + 1, s->here);
            else
                say(s, "cannot read %s: %s", p, strerror(errno));
            return -1;
        }
    }
    return 0;
}

/** Find ccache: RACH2_CCACHE, empty for none, or the first on the path. */
static void find_ccache(struct step *s)
{
    const char *env = getenv("RACH2_CCACHE");
    const char *path = getenv("PATH");

    s->ccache[0] = '\0';
    if (env) {
        if (*env && access(env, X_OK) == 0)
            path_fmt(s->ccache, "%s", env);
        return;
    }
    while (path && *path) {
        const char *e = strchr(path, ':');
        size_t n = e ? (size_t)(e - path) : strlen(path);
        char p[PATH_LEN];

        if (n > 0 && n < PATH_LEN - 8) {
            memcpy(p, path, n);
            memcpy(p + n, "/ccache", 8);
            if (access(p, X_OK) == 0) {
                path_fmt(s->ccache, "%s", p);
                return;
            }
        }
        path = e ? e + 1 : NULL;
    }
}

/** The last part of the first word of `cc`. */
static void compiler_name(const char *cc, char *out)
{
    const char *end = cc + strcspn(cc, " \t");
    const char *start = end;

    while (start > cc && start[-1] != '/')
        start--;
    snprintf(out, 256, "%.*s", (int)(end - start), start);
}

/**
 * Make a directory of links to ccache, one named for each compiler the step
 * runs, for the front of the command's path: ccache, run by a compiler's
 * name, runs the compiler of that name that comes after it on the path.
 */
static int make_shims(struct step *s)
{
    const char *tmp = getenv("TMPDIR");
    const char *fixed[] = {"cc", "gcc", "c++", "g++"};
    char name[256], p[PATH_LEN];
    int k;

    if (path_fmt(s->shims, "%s/etude-step.XXXXXX", tmp && *tmp ? tmp : "/tmp") != 0 ||
        mkdtemp(s->shims) == NULL) {
        say(s, "cannot make a directory for ccache's names: %s", strerror(errno));
        s->shims[0] = '\0';
        return -1;
    }
    for (k = 0; k < 4 + s->cc.n + s->wrap.n; k++) {
        if (k < 4)
            snprintf(name, sizeof(name), "%s", fixed[k]);
        else
            compiler_name(k < 4 + s->cc.n ? s->cc.v[k - 4] : s->wrap.v[k - 4 - s->cc.n], name);
        if (name[0] == '\0' || strcmp(name, "ccache") == 0 ||
            path_fmt(p, "%s/%s", s->shims, name) != 0)
            continue;
        if (symlink(s->ccache, p) != 0 && errno != EEXIST) {
            say(s, "cannot link %s: %s", p, strerror(errno));
            return -1;
        }
    }
    return 0;
}

/**
 * Run the command with `root` as ETUDE_STEP_ROOT, or none, and ccache in
 * front of its compilers when the step uses it; with `shared`, files it
 * makes are writable by the group, so that a partial entry another account
 * left can be removed. The command's status, as wait gives it.
 */
static int run_command(struct step *s, const char *root, const char *basedir, int shared)
{
    pid_t pid;
    int status;

    fflush(NULL);
    if ((pid = fork()) < 0) {
        say(s, "fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        if (root)
            setenv("ETUDE_STEP_ROOT", root, 1);
        else
            unsetenv("ETUDE_STEP_ROOT");
        if (s->shims[0]) {
            struct text path = {0};
            const char *old = getenv("PATH");

            text_add(&path, "%s:%s", s->shims, old ? old : "/usr/bin:/bin");
            setenv("PATH", path.s, 1);
            setenv("ETUDE_STEP_SHIMS", s->shims, 1);
            setenv("ETUDE_STEP_CCACHE", s->ccache, 1);
            setenv("CCACHE_BASEDIR", basedir, 1);
            setenv("CCACHE_NOHASHDIR", "true", 1);
        } else {
            unsetenv("ETUDE_STEP_SHIMS");
            unsetenv("ETUDE_STEP_CCACHE");
        }
        if (shared)
            umask(002);
        execvp(s->argv[0], s->argv);
        fprintf(stderr, "etude-step: %s: %s: %s\n", s->name, s->argv[0], strerror(errno));
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) {
            say(s, "wait: %s", strerror(errno));
            return -1;
        }
    return status;
}

/** Check that each output is under `base` once the command has run. */
static int check_outputs(const struct step *s, const char *base)
{
    struct stat st;
    char p[PATH_LEN];
    int k;

    for (k = 0; k < s->out.n; k++)
        if (path_fmt(p, "%s/%s", base, s->out.v[k]) != 0 || lstat(p, &st) != 0) {
            say(s, "the step made no %s", s->out.v[k]);
            return -1;
        }
    return 0;
}

/**
 * Remove from the link-mode tree `dir`, whose path under the tree is `rel`,
 * everything that is not an output or a use or above one: the sources and
 * build directories the command left there.
 */
static int prune(const struct step *s, const char *dir, const char *rel)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int rc = 0;

    if (d == NULL)
        return -1;
    while ((e = readdir(d)) != NULL) {
        char sub[PATH_LEN], subrel[PATH_LEN];
        int k, keep = 0, above = 0;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (path_fmt(sub, "%s/%s", dir, e->d_name) != 0 ||
            (*rel ? path_fmt(subrel, "%s/%s", rel, e->d_name)
                  : path_fmt(subrel, "%s", e->d_name)) != 0) {
            rc = -1;
            continue;
        }
        for (k = 0; k < s->out.n + s->use.n; k++) {
            const char *o = k < s->out.n ? s->out.v[k] : s->use.v[k - s->out.n];

            if (strcmp(o, subrel) == 0)
                keep = 1;
            else if (covers(subrel, o))
                above = 1;
        }
        if (keep)
            continue;
        if (above) {
            if (prune(s, sub, subrel) != 0)
                rc = -1;
        } else if (remove_tree(sub) != 0) {
            say(s, "cannot remove %s", sub);
            rc = -1;
        }
    }
    closedir(d);
    return rc;
}

/** Write the record of a finished entry, `DIR/record`, with its key. */
static int write_record(const struct step *s, const char *dir)
{
    char p[PATH_LEN], tmp[PATH_LEN];
    FILE *f;

    if (path_fmt(p, "%s/record", dir) != 0 || path_fmt(tmp, "%s/record.tmp", dir) != 0 ||
        (f = fopen(tmp, "w")) == NULL)
        return -1;
    fwrite(s->record.s, 1, s->record.len, f);
    fprintf(f, "key %s\n", s->key);
    if (fclose(f) != 0 || chmod(tmp, 0444) != 0 || rename(tmp, p) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/** Make DIR, writable by the store's group, where it is missing. */
static int store_dir(const char *dir)
{
    if (mkdir(dir, 0775) == 0) {
        chmod(dir, 02775);
        return 0;
    }
    return errno == EEXIST ? 0 : -1;
}

/** Remove what was left in the store's tmp/ a day ago or earlier. */
static void clear_stale(const struct step *s)
{
    char dir[PATH_LEN], p[PATH_LEN];
    DIR *d;
    struct dirent *e;
    time_t now = time(NULL);

    if (path_fmt(dir, "%s/tmp", s->shelf) != 0 || (d = opendir(dir)) == NULL)
        return;
    while ((e = readdir(d)) != NULL) {
        struct stat st;

        if (e->d_name[0] == '.' || path_fmt(p, "%s/%s", dir, e->d_name) != 0)
            continue;
        if (lstat(p, &st) == 0 && now - st.st_mtime > STALE_SECONDS)
            remove_tree(p);
    }
    closedir(d);
}

/**
 * Move a partial entry, one whose command did not finish, out of the way and
 * remove it. A part that another account made and keeps from this one stays
 * in tmp/ for its owner, or for clear_stale.
 */
static int discard(const struct step *s, const char *entry)
{
    char aside[PATH_LEN];

    if (path_fmt(aside, "%s/tmp/%s.left.%ld", s->shelf, s->key, (long)getpid()) != 0)
        return -1;
    say(s, "removing the partial entry %s that an earlier build left", entry);
    chmod(entry, 02775);
    if (rename(entry, aside) != 0) {
        say(s, "cannot move %s: %s", entry, strerror(errno));
        return -1;
    }
    if (remove_tree(aside) != 0)
        say(s, "some of %s is not this account's and stays", aside);
    return 0;
}

/** Put the outputs of the entry tree `tree` in the copy: copies in copy
 * mode, and links in link mode. */
static int place(const struct step *s, const char *tree)
{
    char from[PATH_LEN], to[PATH_LEN];
    int k;

    for (k = 0; k < s->out.n; k++) {
        if (path_fmt(from, "%s/%s", tree, s->out.v[k]) != 0 ||
            (s->root ? path_fmt(to, "%s/%s", s->root, s->out.v[k])
                     : path_fmt(to, "%s", s->out.v[k])) != 0)
            return -1;
        if (remove_tree(to) != 0) {
            say(s, "cannot remove %s: %s", to, strerror(errno));
            return -1;
        }
        if (make_parents(to) != 0) {
            say(s, "cannot make the directories of %s: %s", to, strerror(errno));
            return -1;
        }
        if (s->root) {
            if (symlink(from, to) != 0) {
                say(s, "cannot link %s: %s", to, strerror(errno));
                return -1;
            }
        } else if (copy_tree(s, from, to) != 0) {
            return -1;
        }
    }
    return 0;
}

/**
 * Add the outputs a copy-mode step made in the copy to the store as the
 * entry `entry`: copied to a directory of tmp/, sealed, given the record and
 * renamed into place. Nothing is added when an output names the copy.
 */
static int add_copies(const struct step *s, const char *entry)
{
    char dir[PATH_LEN], tree[PATH_LEN], p[PATH_LEN];
    int k;

    if (path_fmt(dir, "%s/tmp/%s.XXXXXX", s->shelf, s->key) != 0 || mkdtemp(dir) == NULL) {
        say(s, "cannot make a directory in %s/tmp: %s", s->shelf, strerror(errno));
        return -1;
    }
    if (path_fmt(tree, "%s/tree", dir) != 0 || mkdir(tree, 0777) != 0)
        goto fail;
    for (k = 0; k < s->out.n; k++)
        if (path_fmt(p, "%s/%s", tree, s->out.v[k]) != 0 || make_parents(p) != 0 ||
            copy_tree(s, s->out.v[k], p) != 0)
            goto fail;
    if (check_paths(s, tree) != 0)
        goto fail;
    if (seal_tree(tree) != 0 || write_record(s, dir) != 0) {
        say(s, "cannot seal %s: %s", dir, strerror(errno));
        goto fail;
    }
    if (rename(dir, entry) != 0) {
        if (errno != EEXIST && errno != ENOTEMPTY)
            say(s, "cannot add %s: %s", entry, strerror(errno));
        goto fail;
    }
    chmod(entry, 0555);
    return 0;
fail:
    remove_tree(dir);
    return -1;
}

/** Make the links of the uses in the link-mode tree `tree`; -1 when a use
 * is not in the store, and the step is not shared. */
static int link_uses(const struct step *s, const char *tree, int make)
{
    char p[PATH_LEN], real[PATH_LEN], entries[PATH_LEN], to[PATH_LEN];
    int k;

    if (path_fmt(entries, "%s/entries/", s->shelf) != 0)
        return -1;
    for (k = 0; k < s->use.n; k++) {
        if (path_fmt(p, "%s/%s", s->root, s->use.v[k]) != 0 || realpath(p, real) == NULL)
            return -1;
        if (strncmp(real, entries, strlen(entries)) != 0)
            return -1;
        if (!make)
            continue;
        if (path_fmt(to, "%s/%s", tree, s->use.v[k]) != 0 || make_parents(to) != 0 ||
            symlink(real, to) != 0) {
            say(s, "cannot link %s: %s", to, strerror(errno));
            return -2;
        }
    }
    return 0;
}

/** The exit status for the command's status `status`: its own, or the
 * signal that ended it raised again in this process. */
static int command_exit(int status)
{
    if (status < 0)
        return 1;
    if (WIFSIGNALED(status)) {
        signal(WTERMSIG(status), SIG_DFL);
        raise(WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return WEXITSTATUS(status);
}

/**
 * Run the step in the copy, as a build without the store does: in link mode
 * at the root in the copy, after removing outputs there that are links into
 * the store.
 */
static int run_here(struct step *s)
{
    char root[PATH_LEN];
    int status, k;

    if (s->root) {
        if (make_parents(s->root) != 0 || (mkdir(s->root, 0777) != 0 && errno != EEXIST) ||
            realpath(s->root, root) == NULL) {
            say(s, "cannot make %s: %s", s->root, strerror(errno));
            return 1;
        }
        for (k = 0; k < s->out.n; k++) {
            struct stat st;
            char p[PATH_LEN];

            if (path_fmt(p, "%s/%s", s->root, s->out.v[k]) == 0 && lstat(p, &st) == 0 &&
                S_ISLNK(st.st_mode))
                unlink(p);
        }
    }
    status = run_command(s, s->root ? root : NULL, s->here, 0);
    if (status != 0)
        return command_exit(status);
    if (check_outputs(s, s->root ? s->root : ".") != 0 || write_stamp(s, "here") != 0)
        return 1;
    printf("etude-step: %s: result: built here, %.12s\n", s->name, s->key);
    return 0;
}

/** Build a link-mode step in the store, at the entry's own path, so that
 * every path the build writes into its outputs is the store's. */
static int build_in_store(struct step *s, const char *entry)
{
    char tree[PATH_LEN];
    int status;

    if (path_fmt(tree, "%s/tree", entry) != 0 || store_dir(entry) != 0 || mkdir(tree, 02775) != 0) {
        say(s, "cannot make %s: %s", entry, strerror(errno));
        return 1;
    }
    chmod(tree, 02775);
    if (link_uses(s, tree, 1) != 0)
        return 1;
    printf("etude-step: %s: building in the store, %s\n", s->name, entry);
    status = run_command(s, tree, tree, 1);
    if (status == 0 && stop_signal)
        return 128 + stop_signal;
    if (status != 0) {
        if (!stop_signal)
            say(s, "the step failed; its tree stays in %s until the step is built again", entry);
        return command_exit(status);
    }
    if (check_outputs(s, tree) != 0 || prune(s, tree, "") != 0 || check_paths(s, tree) != 0)
        return 1;
    if (seal_tree(tree) != 0 || write_record(s, entry) != 0 || chmod(entry, 0555) != 0) {
        say(s, "cannot seal %s: %s", entry, strerror(errno));
        return 1;
    }
    if (place(s, tree) != 0 || write_stamp(s, "store") != 0)
        return 1;
    printf("etude-step: %s: result: built in the store, %.12s\n", s->name, s->key);
    return 0;
}

/**
 * Open the lock of `key` and take it, waiting for another process that holds
 * it when `wait` is 1: its descriptor; -2 when `wait` is 0 and the lock is
 * held; -1 on an error or a stop. A lock file that `etude-step -e` removed
 * while this process waited for it is opened again, so that every holder
 * holds the file the path names. `made` is 1 when this process made the file.
 */
static int lock_key(const struct step *s, const char *key, int wait, int *made)
{
    char lock[PATH_LEN];
    struct stat a, b;
    int fd;

    if (path_fmt(lock, "%s/locks/%s", s->shelf, key) != 0)
        return -1;
    for (;;) {
        *made = 0;
        if ((fd = open(lock, O_RDWR)) < 0 && errno == ENOENT) {
            if ((fd = open(lock, O_RDWR | O_CREAT | O_EXCL, 0664)) < 0 && errno == EEXIST)
                continue;
            if (fd >= 0) {
                *made = 1;
                fchmod(fd, 0664);
            }
        }
        if (fd < 0) {
            say(s, "cannot open %s: %s", lock, strerror(errno));
            return -1;
        }
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            if (!wait) {
                close(fd);
                return -2;
            }
            printf("etude-step: %s: waiting for another build of %.12s\n", s->name, key);
            fflush(stdout);
            while (flock(fd, LOCK_EX) != 0)
                if (errno != EINTR || stop_signal) {
                    close(fd);
                    return -1;
                }
        }
        if (fstat(fd, &a) == 0 && stat(lock, &b) == 0 && a.st_dev == b.st_dev &&
            a.st_ino == b.st_ino)
            return fd;
        close(fd);
    }
}

/**
 * Open and hold the lock of the step's key, waiting for another build of the
 * key to end, and date the lock now, which records that a step used the key:
 * its descriptor, or -1 with the exit status in `rc`.
 */
static int hold_key(const struct step *s, int *rc)
{
    int made, fd = lock_key(s, s->key, 1, &made);

    if (fd < 0) {
        *rc = stop_signal ? 128 + stop_signal : 1;
        return -1;
    }
    futimens(fd, NULL);
    return fd;
}

/**
 * Record in the store's stamps/ that the step's stamp names its key: a link
 * `stamps/KEY.HASH` to the stamp's absolute path, HASH being the SHA-256 of
 * the path. `etude-step -e` keeps each entry whose key a stamp so recorded
 * still names. A failure is reported and does not fail the step.
 */
static void record_stamp(const struct step *s)
{
    char path[PATH_LEN], dir[PATH_LEN], link[PATH_LEN], tmp[PATH_LEN], to[PATH_LEN], hex[65];
    ssize_t n;

    if ((s->stamp[0] == '/' ? path_fmt(path, "%s", s->stamp)
                            : path_fmt(path, "%s/%s", s->here, s->stamp)) != 0)
        return;
    hash_text(path, strlen(path), hex);
    if (path_fmt(dir, "%s/stamps", s->shelf) != 0 ||
        path_fmt(link, "%s/%s.%s", dir, s->key, hex) != 0 ||
        path_fmt(tmp, "%s/.%s.%ld", dir, hex, (long)getpid()) != 0)
        return;
    n = readlink(link, to, sizeof(to) - 1);
    if (n >= 0 && (size_t)n == strlen(path) && memcmp(to, path, (size_t)n) == 0)
        return;
    if (store_dir(dir) != 0 || symlink(path, tmp) != 0 || rename(tmp, link) != 0) {
        say(s, "cannot record the stamp in %s: %s", dir, strerror(errno));
        unlink(tmp);
    }
}

/** Build a copy-mode step in the copy, and add its outputs to the store as
 * `entry` when they may be shared. */
static int build_and_add(struct step *s, const char *entry)
{
    int status = run_command(s, NULL, s->here, 0);

    if (status != 0)
        return command_exit(status);
    if (check_outputs(s, ".") != 0 || write_stamp(s, "here") != 0)
        return 1;
    if (!stop_signal && add_copies(s, entry) == 0)
        printf("etude-step: %s: result: built and stored, %.12s\n", s->name, s->key);
    else
        printf("etude-step: %s: result: built here, %.12s\n", s->name, s->key);
    return 0;
}

/** The step with a store: take its outputs, or build them and add them,
 * holding the key's lock so that one build of a key runs at a time. */
static int run_shared(struct step *s)
{
    char entry[PATH_LEN], record[PATH_LEN], tree[PATH_LEN], p[PATH_LEN];
    struct stat st;
    int fd, rc;

    if (path_fmt(p, "%s/entries", s->shelf) != 0 || store_dir(p) != 0 ||
        path_fmt(p, "%s/locks", s->shelf) != 0 || store_dir(p) != 0 ||
        path_fmt(p, "%s/tmp", s->shelf) != 0 || store_dir(p) != 0) {
        say(s, "cannot make the directories of the store %s: %s", s->shelf, strerror(errno));
        return 1;
    }
    clear_stale(s);
    if (path_fmt(entry, "%s/entries/%s", s->shelf, s->key) != 0 ||
        path_fmt(record, "%s/record", entry) != 0 || path_fmt(tree, "%s/tree", entry) != 0)
        return 1;
    if ((fd = hold_key(s, &rc)) < 0)
        return rc;
    if (stat(record, &st) == 0) {
        rc = place(s, tree) == 0 && write_stamp(s, "store") == 0 ? 0 : 1;
        if (rc == 0)
            printf("etude-step: %s: result: taken from the store, %.12s\n", s->name, s->key);
    } else if (lstat(entry, &st) == 0 && discard(s, entry) != 0) {
        rc = 1;
    } else if (s->root) {
        rc = build_in_store(s, entry);
    } else {
        rc = build_and_add(s, entry);
    }
    if (rc == 0)
        record_stamp(s);
    close(fd);
    return rc;
}

/** 1 when `name` begins with a key, 64 hexadecimal digits in lowercase. */
static int keyed_name(const char *name)
{
    return strspn(name, "0123456789abcdef") >= 64;
}

/** 1 when `l` has the key `key`. */
static int has_key(const struct lines *l, const char *key)
{
    size_t i;

    for (i = 0; i < l->n; i++)
        if (strncmp(l->v[i], key, 64) == 0)
            return 1;
    return 0;
}

/** Add the key at the start of `key` to `l` when it is not there. */
static int keep_key(struct lines *l, const char *key)
{
    return has_key(l, key) ? 0 : lines_add(l, "%.64s", key);
}

/** The key the stamp `path` names, as stamp_key gives it, reading nothing but
 * a regular file. */
static int named_key(const char *path, char key[65])
{
    struct stat st;

    if (stat(path, &st) != 0)
        return -1;
    return S_ISREG(st.st_mode) ? stamp_key(path, key, NULL) : 0;
}

/**
 * Add to `live` the key of each link of the store's stamps/ whose stamp still
 * names it, and remove each link whose stamp is gone or names another key,
 * counting them in `dropped`. A stamp that cannot be read for another reason,
 * such as a home directory this account may not search, keeps its key.
 */
static int read_stamps(const struct step *s, struct lines *live, int *dropped)
{
    char dir[PATH_LEN], link[PATH_LEN], to[PATH_LEN], key[65];
    DIR *d;
    struct dirent *e;
    int rc = 0;

    if (path_fmt(dir, "%s/stamps", s->shelf) != 0)
        return -1;
    if ((d = opendir(dir)) == NULL)
        return errno == ENOENT ? 0 : -1;
    while (rc == 0 && (e = readdir(d)) != NULL) {
        ssize_t n;
        int got;

        if (!keyed_name(e->d_name) || e->d_name[64] != '.' ||
            path_fmt(link, "%s/%s", dir, e->d_name) != 0 ||
            (n = readlink(link, to, sizeof(to) - 1)) < 0)
            continue;
        to[n] = '\0';
        got = named_key(to, key);
        if ((got == 1 && strncmp(key, e->d_name, 64) == 0) ||
            (got < 0 && errno != ENOENT && errno != ENOTDIR)) {
            rc = keep_key(live, e->d_name);
        } else if (s->print || unlink(link) == 0) {
            (*dropped)++;
        }
    }
    closedir(d);
    return rc;
}

/** Add to `live` each key that the record of an entry in `live` uses, and
 * so on, since a link-mode entry has links to the entries it uses. */
static int add_uses(const struct step *s, struct lines *live)
{
    char p[PATH_LEN], line[PATH_LEN + 200];
    size_t i;

    for (i = 0; i < live->n; i++) {
        FILE *f;

        if (path_fmt(p, "%s/entries/%s/record", s->shelf, live->v[i]) != 0 ||
            (f = fopen(p, "r")) == NULL)
            continue;
        while (fgets(line, sizeof(line), f) != NULL)
            if (strncmp(line, "depends ", 8) == 0 && keyed_name(line + 8) &&
                keep_key(live, line + 8) != 0) {
                fclose(f);
                return -1;
            }
        fclose(f);
    }
    return 0;
}

/** The bytes the tree `path` takes on its disk. */
static double tree_bytes(const char *path)
{
    struct stat st;
    double n;
    DIR *d;
    struct dirent *e;

    if (lstat(path, &st) != 0)
        return 0;
    n = (double)st.st_blocks * 512;
    if (!S_ISDIR(st.st_mode) || (d = opendir(path)) == NULL)
        return n;
    while ((e = readdir(d)) != NULL) {
        char sub[PATH_LEN];

        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0 &&
            path_fmt(sub, "%s/%s", path, e->d_name) == 0)
            n += tree_bytes(sub);
    }
    closedir(d);
    return n;
}

/** The step and the architecture that the record of `entry` gives, in
 * `what`, or `a partial entry` when it has no record. */
static void entry_what(const char *entry, char *what, size_t size)
{
    char p[PATH_LEN], line[PATH_LEN + 200], step[256] = "", arch[64] = "";
    FILE *f;

    if (path_fmt(p, "%s/record", entry) != 0 || (f = fopen(p, "r")) == NULL) {
        snprintf(what, size, "a partial entry");
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, "step ", 5) == 0)
            snprintf(step, sizeof(step), "%.200s", line + 5);
        else if (strncmp(line, "arch ", 5) == 0)
            snprintf(arch, sizeof(arch), " for %.40s", line + 5);
    }
    fclose(f);
    snprintf(what, size, "%s%s", step, arch);
}

/* An entry that `etude-step -e` may remove: its key, the time a step last
 * used it, and the descriptor of its lock while this process holds it. */
struct doomed {
    char key[65];
    time_t used;
    int fd;
};

/** The time a step last used the entry `key`: the later of its lock's date
 * and the entry's; (time_t)-1 when the entry is gone. */
static time_t entry_used(const struct step *s, const char *key)
{
    char p[PATH_LEN];
    struct stat st;
    time_t used;

    if (path_fmt(p, "%s/entries/%.64s", s->shelf, key) != 0 || lstat(p, &st) != 0)
        return (time_t)-1;
    used = st.st_mtime;
    if (path_fmt(p, "%s/locks/%.64s", s->shelf, key) == 0 && stat(p, &st) == 0 &&
        st.st_mtime > used)
        used = st.st_mtime;
    return used;
}

/**
 * Remove the entry `d`, whose lock this process holds, and its lock file,
 * adding its size to `freed`; with -p only report it. 0, or -1 when the entry
 * cannot be moved out of entries/.
 */
static int drop_entry(const struct step *s, const struct doomed *d, double *freed)
{
    char entry[PATH_LEN], lock[PATH_LEN], aside[PATH_LEN], what[300];
    struct stat st;
    double bytes;

    if (path_fmt(entry, "%s/entries/%s", s->shelf, d->key) != 0 ||
        path_fmt(lock, "%s/locks/%s", s->shelf, d->key) != 0 ||
        path_fmt(aside, "%s/tmp/%s.gone.%ld", s->shelf, d->key, (long)getpid()) != 0 ||
        lstat(entry, &st) != 0)
        return -1;
    bytes = tree_bytes(entry);
    entry_what(entry, what, sizeof(what));
    if (!s->print) {
        chmod(entry, (st.st_mode & 07777) | S_IRWXU);
        if (rename(entry, aside) != 0) {
            say(s, "cannot remove %s: %s", entry, strerror(errno));
            chmod(entry, st.st_mode & 07777);
            return -1;
        }
        if (remove_tree(aside) != 0)
            say(s, "some of %s is not this account's and stays", aside);
        unlink(lock);
    }
    *freed += bytes;
    printf("etude-step: %s: %s %.12s, %s, %.0f MiB, unused for %ld days\n", s->name,
           s->print ? "would remove" : "removed", d->key, what, bytes / 1048576,
           (long)((time(NULL) - d->used) / 86400));
    return 0;
}

/** Remove each lock file of the store that has no entry and that no step
 * has used since `before`. */
static void drop_locks(const struct step *s, time_t before)
{
    char dir[PATH_LEN], lock[PATH_LEN], entry[PATH_LEN];
    DIR *d;
    struct dirent *e;

    if (path_fmt(dir, "%s/locks", s->shelf) != 0 || (d = opendir(dir)) == NULL)
        return;
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        int fd, made;

        if (!keyed_name(e->d_name) || e->d_name[64] != '\0' ||
            path_fmt(lock, "%s/%s", dir, e->d_name) != 0 ||
            path_fmt(entry, "%s/entries/%s", s->shelf, e->d_name) != 0 || stat(lock, &st) != 0 ||
            st.st_mtime > before || lstat(entry, &st) == 0)
            continue;
        if ((fd = lock_key(s, e->d_name, 0, &made)) < 0)
            continue;
        if (made || (fstat(fd, &st) == 0 && st.st_mtime <= before && lstat(entry, &st) != 0))
            unlink(lock);
        close(fd);
    }
    closedir(d);
}

/* What `etude-step -e` works out: the keys it keeps, and the entries it may
 * remove. */
struct sweep {
    struct lines live; /* the keys kept: named, recent, or used by one kept */
    struct doomed *v;  /* the entries that may go */
    size_t n, cap;
    time_t before; /* an entry used after this time stays */
    int entries;   /* the entries the store had */
};

/** Read the store's entries/ into `w`: each entry not in `w->live` that no
 * step has used since `w->before` is a candidate, and the rest are kept. */
static int find_doomed(const struct step *s, struct sweep *w)
{
    char dir[PATH_LEN];
    DIR *d;
    struct dirent *e;
    int rc = 0;

    if (path_fmt(dir, "%s/entries", s->shelf) != 0)
        return -1;
    if ((d = opendir(dir)) == NULL)
        return errno == ENOENT ? 0 : -1;
    while (rc == 0 && (e = readdir(d)) != NULL) {
        time_t used;

        if (!keyed_name(e->d_name) || e->d_name[64] != '\0')
            continue;
        w->entries++;
        if (has_key(&w->live, e->d_name) || (used = entry_used(s, e->d_name)) == (time_t)-1)
            continue;
        if (used > w->before) {
            rc = keep_key(&w->live, e->d_name);
            continue;
        }
        if (w->n == w->cap) {
            size_t cap = w->cap ? w->cap * 2 : 64;
            struct doomed *v = realloc(w->v, cap * sizeof(*v));

            if (v == NULL) {
                rc = -1;
                break;
            }
            w->v = v;
            w->cap = cap;
        }
        memcpy(w->v[w->n].key, e->d_name, 65);
        w->v[w->n].used = used;
        w->v[w->n++].fd = -1;
    }
    closedir(d);
    return rc;
}

/** Lock each candidate of `w` not yet kept, without waiting, and keep one
 * whose lock is held or that a step used since it was read. 0, or 1 when a
 * lock cannot be opened. */
static int lock_doomed(const struct step *s, struct sweep *w)
{
    size_t i;
    int rc = 0;

    for (i = 0; i < w->n; i++) {
        struct doomed *d = &w->v[i];
        struct stat st;
        int made;

        if (has_key(&w->live, d->key))
            continue;
        d->fd = lock_key(s, d->key, 0, &made);
        if (d->fd >= 0 && !made && fstat(d->fd, &st) == 0 && st.st_mtime > d->used)
            d->used = st.st_mtime;
        if (d->fd == -1)
            rc = 1;
        if (d->fd < 0 || d->used > w->before)
            keep_key(&w->live, d->key);
    }
    return rc;
}

/**
 * Remove from the store each entry that no recorded stamp names and that no
 * step has used for `s->days` days, with its lock file; the links of stamps
 * that are gone; the lock files of keys with no entry; and what tmp/ has kept
 * for a day. An entry that stays keeps each entry it uses, since a link-mode
 * entry has links to them. Each entry that may go is locked, without
 * waiting, and its date read again before any is removed, so that a step that
 * takes or builds one meanwhile keeps it and what it uses. With -p, list the
 * entries that would go and remove nothing. The exit status: 0, or 1 when
 * something cannot be read or removed.
 */
static int expire(struct step *s)
{
    struct sweep w;
    size_t i;
    double freed = 0;
    int dropped = 0, removed = 0, rc;

    memset(&w, 0, sizeof(w));
    w.before = time(NULL) - atol(s->days) * 86400L;
    if (read_stamps(s, &w.live, &dropped) != 0 || find_doomed(s, &w) != 0 ||
        add_uses(s, &w.live) != 0) {
        say(s, "cannot read %s: %s", s->shelf, strerror(errno));
        free(w.v);
        lines_free(&w.live);
        return 1;
    }
    rc = lock_doomed(s, &w);
    add_uses(s, &w.live);
    for (i = 0; i < w.n; i++) {
        if (w.v[i].fd < 0)
            continue;
        if (!has_key(&w.live, w.v[i].key) && drop_entry(s, &w.v[i], &freed) != 0)
            rc = 1;
        else if (!has_key(&w.live, w.v[i].key))
            removed++;
        close(w.v[i].fd);
    }
    if (!s->print) {
        drop_locks(s, w.before);
        clear_stale(s);
    }
    printf("etude-step: %s: result: %s %d entries, %.1f GiB, and %d links of stamps that are "
           "gone; kept %d entries\n",
           s->name, s->print ? "would remove" : "removed", removed, freed / 1073741824.0, dropped,
           w.entries - removed);
    free(w.v);
    lines_free(&w.live);
    return rc;
}

static int list_add(struct list *l, const char *v)
{
    if (l->n == LIST_MAX)
        return -1;
    l->v[l->n++] = v;
    return 0;
}

/** The member of `s` that the option -c of one value sets, or NULL. */
static const char **value_of(struct step *s, int c)
{
    switch (c) {
    case 's':
        return &s->store;
    case 'n':
        return &s->name;
    case 't':
        return &s->stamp;
    case 'a':
        return &s->arch;
    case 'r':
        return &s->root;
    case 'e':
        return &s->days;
    }
    return NULL;
}

/** The list of `s` that the option -c adds to, or NULL. */
static struct list *list_of(struct step *s, int c)
{
    switch (c) {
    case 'c':
        return &s->cc;
    case 'w':
        return &s->wrap;
    case 'i':
        return &s->in;
    case 'd':
        return &s->dep;
    case 'u':
        return &s->use;
    case 'o':
        return &s->out;
    case 'x':
        return &s->skip;
    }
    return NULL;
}

/** The `k`th of the outputs and then the uses of `s`. */
static const char *placed(const struct step *s, int k)
{
    return k < s->out.n ? s->out.v[k] : s->use.v[k - s->out.n];
}

/** Check the paths of the outputs and the uses: each clean, and none inside
 * another; 0, or -1. */
static int check_placed(const struct step *s)
{
    int j, k, n = s->out.n + s->use.n;

    for (k = 0; k < n; k++) {
        if (!clean_path(placed(s, k)))
            return -1;
        for (j = 0; j < n; j++)
            if (j != k && covers(placed(s, k), placed(s, j)))
                return -1;
    }
    return 0;
}

/** 1 when -e has a store, a number of days and no option of a step. */
static int expire_usage(const struct step *s)
{
    size_t n = strlen(s->days);

    if (s->store == NULL || *s->store == '\0' || n == 0 || n > 5 ||
        strspn(s->days, "0123456789") != n)
        return 0;
    return !s->name && !s->stamp && !s->arch && !s->root &&
           s->cc.n + s->wrap.n + s->in.n + s->dep.n + s->use.n + s->out.n + s->skip.n == 0;
}

/** Read the options into `s`; 0, or -1 on a usage error. */
static int parse(struct step *s, int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char **value;
        struct list *l;

        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "-p") == 0) {
            s->print = 1;
            continue;
        }
        if (a[0] != '-' || a[1] == '\0' || a[2] != '\0' || i + 1 == argc)
            return -1;
        if ((value = value_of(s, a[1])) != NULL)
            *value = argv[++i];
        else if ((l = list_of(s, a[1])) == NULL || list_add(l, argv[++i]) != 0)
            return -1;
    }
    if (s->days)
        return i == argc && expire_usage(s) ? 0 : -1;
    if (i >= argc && !s->print)
        return -1;
    s->argv = argv + i;
    if (s->name == NULL || s->stamp == NULL || s->out.n == 0 || (s->use.n && !s->root))
        return -1;
    if (s->root && !clean_path(s->root))
        return -1;
    return check_placed(s);
}

int main(int argc, char **argv)
{
    static struct step step;
    struct step *s = &step;
    struct sigaction sa;
    int rc;

    memset(s, 0, sizeof(*s));
    if (parse(s, argc, argv) != 0)
        return usage();
    if (s->days) {
        struct stat st;

        s->name = "store";
        if (stat(s->store, &st) != 0 || !S_ISDIR(st.st_mode) ||
            realpath(s->store, s->shelf) == NULL) {
            say(s, "the store %s is not a directory", s->store);
            return 1;
        }
        return expire(s);
    }
    if (getcwd(s->here, sizeof(s->here)) == NULL) {
        say(s, "getcwd: %s", strerror(errno));
        return 1;
    }
    if (make_record(s) != 0)
        return 1;
    if (s->print) {
        fwrite(s->record.s, 1, s->record.len, stdout);
        printf("key %s\n", s->key);
        return 0;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);

    if (s->store == NULL || s->store[0] == '\0') {
        rc = run_here(s);
    } else {
        struct stat st;

        if (stat(s->store, &st) != 0 || !S_ISDIR(st.st_mode) ||
            realpath(s->store, s->shelf) == NULL) {
            say(s, "the store %s is not a directory", s->store);
            return 1;
        }
        find_ccache(s);
        if (s->ccache[0] && make_shims(s) != 0)
            s->shims[0] = '\0';
        if (!s->keyed) {
            say(s, "a step it uses has a stamp with no key; built without the store");
            rc = run_here(s);
        } else if (s->root && link_uses(s, NULL, 0) != 0) {
            say(s, "a step it uses was built in this copy; built without the store");
            rc = run_here(s);
        } else {
            rc = run_shared(s);
        }
        if (s->shims[0])
            remove_tree(s->shims);
    }
    if (stop_signal) {
        signal(stop_signal, SIG_DFL);
        raise(stop_signal);
    }
    return rc;
}
