/**
 * setidcheck: the guard on the set-id bits of the staged images.
 *
 * Privilege in a Rach2 image climbs through a few declared files and
 * nowhere else. The declarations live in `setid.allow` beside this file;
 * this tool holds them against four artifacts, each a separate question:
 *
 * The manifest sweep (`setidcheck [MANIFEST ...]`) parses the manifests
 * the way `apply_line` (krffs/stageimage.krffs.c) does, byte for byte:
 * only a newline ends a line the way only a newline ends `fgets`, fields
 * split on spaces and tabs, a comment is a line whose first token begins
 * with `#`, and an omitted owner reads as 0:0. A set-id `file` must match
 * its declaration at the same path, mode, owner and host source; a set-id
 * `exec-dir` or `tree` is refused outright (the set of paths it privileges
 * is a property of the host directory, not the manifest), and a set-id
 * `dir` is refused because KRFFS has no setgid-directory inheritance.
 * Trusted files, the staged init, and the directories above every declared
 * path are held to their declarations too.
 *
 * The image inventory (`setidcheck --image FILE --manifest PATH [--offset
 * BYTES] [--size BYTES]`) reads the artifact, not the text: it walks the
 * node chain the way fsck.krffs does, requires the ring to close on the
 * size the root records, rebuilds every reserved node's path from
 * `parent_id`, and compares what it finds against the same declarations.
 * An image whose entry says `sysroot` also has the compiler's search paths
 * inventoried and its staged header tree compared, path by path and byte
 * by byte, with the musl install it was staged from.
 *
 * `setidcheck --applets [TABLE]` reads the staged busybox applet tables
 * and configs and the two init binaries. `setidcheck --free --image FILE
 * --offset N --size N` prints the free bytes of a region, for the
 * self-build suite's space preflight; `--nodes --image FILE` prints each
 * root or reserved node in ring order with its path and timestamps, for
 * the reproducibility check. `--selftest` runs every half over fixtures
 * built in memory and in a temporary directory.
 *
 * Exits non-zero on any finding.
 */
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define S_SETID 06000u
#define PERMBITS 07777u
#define LINE_BUFFER 2048
#define MAX_LINE (LINE_BUFFER - 2)
#define INIT_MODE 0755u
#define WORLD_READ 0004u
#define WORLD_WALK 0001u

/* The v2 on-disk node: struct krffs_node (krffs/krffs_node.h) is packed,
   so these offsets are the whole layout. */
#define NODE_SIZE 357
#define NODE_MAGIC_OFF 0
#define NODE_TYPE_OFF 2
#define NODE_ID_OFF 3
#define NODE_NAME_OFF 11
#define NODE_NAME_LEN 256
#define NODE_MODE_OFF 267
#define NODE_UID_OFF 269
#define NODE_GID_OFF 273
#define NODE_SIZEF_OFF 325
#define NODE_PREV_OFF 333
#define NODE_DATA_OFF 341
#define NODE_PARENT_OFF 349
#define KRFFS_MAGIC 0x4B52
#define NODE_ROOT 0
#define NODE_FREE 1
#define NODE_RESERVED 2

/* ---- growable string list, the findings accumulator ---- */

struct list {
    char **items;
    int n, cap;
};

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);

    if (p == NULL) {
        fprintf(stderr, "check-setid: out of memory\n");
        exit(2);
    }
    return p;
}

static char *xstrdup(const char *s)
{
    char *p = xmalloc(strlen(s) + 1);

    strcpy(p, s);
    return p;
}

/**
 * Appends a formatted string to a fixed buffer, advancing `*used`.
 *
 * `snprintf` returns the length it would have written, so a loop that adds
 * that return value to an index walks past the end of the buffer on the
 * first truncation, and the next call writes outside it with a length that
 * has underflowed. This clamps `*used` at the end of the buffer instead
 * and returns 0 when the text did not fit, so a caller that cannot afford
 * a shortened result can say so.
 */
static int appendf(char *out, size_t out_len, size_t *used, const char *fmt,
                   ...)
{
    va_list ap;
    int want;

    if (*used + 1 >= out_len)
        return 0;
    va_start(ap, fmt);
    want = vsnprintf(out + *used, out_len - *used, fmt, ap);
    va_end(ap);
    if (want < 0 || (size_t) want >= out_len - *used) {
        *used = out_len - 1;
        return 0;
    }
    *used += (size_t) want;
    return 1;
}

static void list_push(struct list *l, char *item)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = realloc(l->items, (size_t) l->cap * sizeof(char *));
        if (l->items == NULL)
            exit(2);
    }
    l->items[l->n++] = item;
}

static void addf(struct list *l, const char *fmt, ...)
{
    va_list ap;
    char buf[1024];

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    list_push(l, xstrdup(buf));
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *) a, *(char *const *) b);
}

static void list_free(struct list *l)
{
    int i;

    for (i = 0; i < l->n; i++)
        free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->n = l->cap = 0;
}

static int list_has(const struct list *l, const char *s)
{
    int i;

    for (i = 0; i < l->n; i++)
        if (strcmp(l->items[i], s) == 0)
            return 1;
    return 0;
}

/* ---- little parsers shared by the manifest and allow readers ---- */

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

/* The allowlist key a manifest path names, `krffs/manifests/x.manifest`
   being `x`. The result is a fresh string. */
static char *manifest_stem(const char *path)
{
    const char *name = base_name(path);
    size_t n = strlen(name);
    char *stem;

    if (n > 9 && strcmp(name + n - 9, ".manifest") == 0)
        n -= 9;
    stem = xmalloc(n + 1);
    memcpy(stem, name, n);
    stem[n] = '\0';
    return stem;
}

/* The manifest's mode spelling, `0mmmm:uid:gid`, in a rotating buffer so
   two spellings can sit in one message. */
static const char *fmt_entry(unsigned long mode, unsigned long uid,
                             unsigned long gid)
{
    static char bufs[4][64];
    static int which;

    which = (which + 1) % 4;
    snprintf(bufs[which], sizeof(bufs[which]), "0%lo:%lu:%lu", mode, uid,
             gid);
    return bufs[which];
}

/* The (mode, uid, gid) of a manifest mode field. This is `parse_mode`
   (krffs/stageimage.krffs.c) with its exact verdicts: the mode is octal
   and at most 07777, the tail after it is either empty or an owner
   introduced by ':', and an owner is two complete decimal fields. */
static int parse_mode(const char *token, unsigned long *mode,
                      unsigned long *uid, unsigned long *gid)
{
    char *end;
    unsigned long value;

    if (*token == '\0')
        return 0;
    errno = 0;
    value = strtoul(token, &end, 8);
    if (errno == ERANGE || value > PERMBITS ||
        (*end != '\0' && *end != ':'))
        return 0;
    *mode = value;
    *uid = 0;
    *gid = 0;
    if (*end == '\0')
        return 1;
    token = end + 1;
    errno = 0;
    *uid = strtoul(token, &end, 10);
    if (end == token || errno == ERANGE || *end != ':')
        return 0;
    token = end + 1;
    errno = 0;
    *gid = strtoul(token, &end, 10);
    if (end == token || errno == ERANGE || *end != '\0')
        return 0;
    if (*uid > 0xFFFFFFFFul || *gid > 0xFFFFFFFFul)
        return 0;
    return 1;
}

/* Split a line into fields the way strtok_r(" \t") does. The line is
   modified in place; returns the field count, capped by `max`. */
static int tokenize(char *line, char **fields, int max)
{
    int n = 0;
    char *p = line;

    while (n < max) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;
        fields[n++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t')
            p++;
        if (*p != '\0')
            *p++ = '\0';
    }
    return n;
}

/* Read a whole file. Returns NULL on failure. */
static unsigned char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long len;

    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = xmalloc((size_t) len + 1);
    if (fread(buf, 1, (size_t) len, f) != (size_t) len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[len] = '\0';
    *out_len = (size_t) len;
    return buf;
}

/* ---- the allowlist model, read from setid.allow ---- */

struct setid_decl {
    char *path;
    unsigned long mode, uid, gid;
    char *source;
};

struct trusted_decl {
    char *path;
    unsigned long mode, uid, gid;
    char *verb;   /* "file" or "text" */
    char *value;  /* the host path, or the pre-unescape payload */
};

struct entry {
    char *stem;
    struct setid_decl *setid;
    int n_setid;
    struct trusted_decl *trusted;
    int n_trusted;
    char *sentinel_path;  /* NULL: none */
    unsigned long sentinel_mode, sentinel_uid, sentinel_gid;
    char *init;           /* NULL: the manifest must stage none */
    int sysroot;
};

struct allow {
    struct entry *entries;
    int n;
    struct list forbidden_applets;
    char *applet_sentinel;
    struct list suid_applets;
    struct list suid_on, suid_off;
    struct list init_strict, init_shellful, shell_strings;
    struct list sysroot_paths;    /* in declaration order */
    struct list sysroot_programs; /* subset markers */
};

static struct entry *find_entry(struct allow *a, const char *stem)
{
    int i;

    for (i = 0; i < a->n; i++)
        if (strcmp(a->entries[i].stem, stem) == 0)
            return &a->entries[i];
    return NULL;
}

static struct entry *add_entry(struct allow *a, const char *stem)
{
    a->entries = realloc(a->entries,
                         (size_t) (a->n + 1) * sizeof(struct entry));
    if (a->entries == NULL)
        exit(2);
    memset(&a->entries[a->n], 0, sizeof(struct entry));
    a->entries[a->n].stem = xstrdup(stem);
    return &a->entries[a->n++];
}

static void entry_add_setid(struct entry *e, const char *path,
                            unsigned long mode, unsigned long uid,
                            unsigned long gid, const char *source)
{
    e->setid = realloc(e->setid,
                       (size_t) (e->n_setid + 1) * sizeof(*e->setid));
    if (e->setid == NULL)
        exit(2);
    e->setid[e->n_setid].path = xstrdup(path);
    e->setid[e->n_setid].mode = mode;
    e->setid[e->n_setid].uid = uid;
    e->setid[e->n_setid].gid = gid;
    e->setid[e->n_setid].source = xstrdup(source);
    e->n_setid++;
}

/* Add or replace: a `copy` entry may re-declare a trusted path. */
static void entry_add_trusted(struct entry *e, const char *path,
                              unsigned long mode, unsigned long uid,
                              unsigned long gid, const char *verb,
                              const char *value)
{
    int i;

    for (i = 0; i < e->n_trusted; i++)
        if (strcmp(e->trusted[i].path, path) == 0)
            break;
    if (i == e->n_trusted) {
        e->trusted = realloc(e->trusted,
                             (size_t) (i + 1) * sizeof(*e->trusted));
        if (e->trusted == NULL)
            exit(2);
        e->n_trusted++;
        e->trusted[i].path = xstrdup(path);
    } else {
        free(e->trusted[i].verb);
        free(e->trusted[i].value);
    }
    e->trusted[i].mode = mode;
    e->trusted[i].uid = uid;
    e->trusted[i].gid = gid;
    e->trusted[i].verb = xstrdup(verb);
    e->trusted[i].value = xstrdup(value);
}

static void entry_copy(struct entry *dst, const struct entry *src)
{
    int i;

    for (i = 0; i < src->n_setid; i++)
        entry_add_setid(dst, src->setid[i].path, src->setid[i].mode,
                        src->setid[i].uid, src->setid[i].gid,
                        src->setid[i].source);
    for (i = 0; i < src->n_trusted; i++)
        entry_add_trusted(dst, src->trusted[i].path, src->trusted[i].mode,
                          src->trusted[i].uid, src->trusted[i].gid,
                          src->trusted[i].verb, src->trusted[i].value);
    if (src->sentinel_path != NULL) {
        dst->sentinel_path = xstrdup(src->sentinel_path);
        dst->sentinel_mode = src->sentinel_mode;
        dst->sentinel_uid = src->sentinel_uid;
        dst->sentinel_gid = src->sentinel_gid;
    }
    dst->init = src->init ? xstrdup(src->init) : NULL;
    dst->sysroot = src->sysroot;
}

/* Refuse a malformed declaration file loudly: the guard's own input is a
   trust boundary, and a line it skipped is a rule nobody enforces. */
static void allow_fail(const char *path, int lineno, const char *why)
    __attribute__((noreturn));

static void allow_fail(const char *path, int lineno, const char *why)
{
    fprintf(stderr, "check-setid: %s:%d: %s\n", path, lineno, why);
    exit(2);
}

static struct allow *read_allow(const char *path)
{
    struct allow *a = xmalloc(sizeof(*a));
    size_t len, pos = 0;
    unsigned char *text = read_file(path, &len);
    struct entry *cur = NULL;
    int lineno = 0;

    memset(a, 0, sizeof(*a));
    if (text == NULL) {
        fprintf(stderr, "check-setid: cannot read %s\n", path);
        exit(2);
    }
    while (pos < len) {
        size_t end = pos;
        char line[LINE_BUFFER];
        char *fields[8];
        int n;

        while (end < len && text[end] != '\n')
            end++;
        lineno++;
        if (end - pos >= sizeof(line))
            allow_fail(path, lineno, "line too long");
        memcpy(line, text + pos, end - pos);
        line[end - pos] = '\0';
        pos = end + 1;

        /* A `trusted text` payload holds spaces, so the tail after the
           guest path is taken from the raw line, the way a manifest's
           `text` payload is. */
        if (strncmp(line, "trusted text ", 13) == 0) {
            char head[LINE_BUFFER];
            char *hfields[4];
            const char *tail;
            unsigned long mode, uid, gid;

            strcpy(head, line);
            n = tokenize(head, hfields, 4);
            if (n < 4 || !parse_mode(hfields[2], &mode, &uid, &gid))
                allow_fail(path, lineno, "malformed trusted text line");
            tail = strstr(line, hfields[3]);
            tail += strlen(hfields[3]);
            while (*tail == ' ' || *tail == '\t')
                tail++;
            if (cur == NULL)
                allow_fail(path, lineno, "no manifest entry is open");
            entry_add_trusted(cur, hfields[3], mode, uid, gid, "text",
                              tail);
            continue;
        }

        n = tokenize(line, fields, 8);
        if (n == 0 || fields[0][0] == '#')
            continue;
        if (strcmp(fields[0], "manifest") == 0) {
            if (n != 2 && !(n == 4 && strcmp(fields[2], "copy") == 0))
                allow_fail(path, lineno, "malformed manifest line");
            if (find_entry(a, fields[1]) != NULL)
                allow_fail(path, lineno, "entry declared twice");
            cur = add_entry(a, fields[1]);
            if (n == 4) {
                struct entry *base = find_entry(a, fields[3]);

                if (base == NULL || base == cur)
                    allow_fail(path, lineno, "copy names no earlier entry");
                entry_copy(cur, base);
            }
        } else if (strcmp(fields[0], "setid") == 0) {
            unsigned long mode, uid, gid;

            if (cur == NULL)
                allow_fail(path, lineno, "no manifest entry is open");
            if (n != 4 || !parse_mode(fields[2], &mode, &uid, &gid))
                allow_fail(path, lineno, "malformed setid line");
            entry_add_setid(cur, fields[1], mode, uid, gid, fields[3]);
        } else if (strcmp(fields[0], "trusted") == 0) {
            unsigned long mode, uid, gid;

            if (cur == NULL)
                allow_fail(path, lineno, "no manifest entry is open");
            if (n != 5 || strcmp(fields[1], "file") != 0 ||
                !parse_mode(fields[2], &mode, &uid, &gid))
                allow_fail(path, lineno, "malformed trusted line");
            entry_add_trusted(cur, fields[3], mode, uid, gid, "file",
                              fields[4]);
        } else if (strcmp(fields[0], "sentinel") == 0) {
            unsigned long mode, uid, gid;

            if (cur == NULL)
                allow_fail(path, lineno, "no manifest entry is open");
            if (n != 3 || !parse_mode(fields[2], &mode, &uid, &gid))
                allow_fail(path, lineno, "malformed sentinel line");
            free(cur->sentinel_path);
            cur->sentinel_path = xstrdup(fields[1]);
            cur->sentinel_mode = mode;
            cur->sentinel_uid = uid;
            cur->sentinel_gid = gid;
        } else if (strcmp(fields[0], "init") == 0) {
            if (cur == NULL)
                allow_fail(path, lineno, "no manifest entry is open");
            if (n != 2)
                allow_fail(path, lineno, "malformed init line");
            free(cur->init);
            cur->init = strcmp(fields[1], "none") == 0 ? NULL
                                                       : xstrdup(fields[1]);
        } else if (strcmp(fields[0], "sysroot") == 0) {
            if (cur == NULL)
                allow_fail(path, lineno, "no manifest entry is open");
            cur->sysroot = 1;
        } else if (strcmp(fields[0], "forbidden-applet") == 0 && n == 2) {
            list_push(&a->forbidden_applets, xstrdup(fields[1]));
        } else if (strcmp(fields[0], "applet-sentinel") == 0 && n == 2) {
            a->applet_sentinel = xstrdup(fields[1]);
        } else if (strcmp(fields[0], "suid-applet") == 0 && n == 2) {
            list_push(&a->suid_applets, xstrdup(fields[1]));
        } else if (strcmp(fields[0], "suid-config-on") == 0 && n == 2) {
            list_push(&a->suid_on, xstrdup(fields[1]));
        } else if (strcmp(fields[0], "suid-config-off") == 0 && n == 2) {
            list_push(&a->suid_off, xstrdup(fields[1]));
        } else if (strcmp(fields[0], "init-strict") == 0 && n == 2) {
            list_push(&a->init_strict, xstrdup(fields[1]));
        } else if (strcmp(fields[0], "init-names-shell") == 0 && n == 2) {
            list_push(&a->init_shellful, xstrdup(fields[1]));
        } else if (strcmp(fields[0], "shell-string") == 0 && n == 2) {
            list_push(&a->shell_strings, xstrdup(fields[1]));
        } else if (strcmp(fields[0], "sysroot-path") == 0 && n == 2) {
            list_push(&a->sysroot_paths, xstrdup(fields[1]));
        } else if (strcmp(fields[0], "sysroot-program") == 0 && n == 2) {
            list_push(&a->sysroot_paths, xstrdup(fields[1]));
            list_push(&a->sysroot_programs, xstrdup(fields[1]));
        } else {
            allow_fail(path, lineno, "unknown declaration");
        }
    }
    free(text);
    if (a->applet_sentinel == NULL)
        a->applet_sentinel = xstrdup("cat");
    return a;
}

/* ---- the manifest reader, apply_line's reading transcribed ---- */

struct directive {
    int lineno;
    char *verb;
    unsigned long mode, uid, gid;
    char *guest;
    char *source; /* host operand, or a text payload, or NULL for dir */
};

struct manifest {
    struct directive *d;
    int n;
};

static const char *const VERBS[] = { "dir", "text", "file", "optional-file",
                                     "exec-dir", "tree" };
static const char *const FILE_VERBS[] = { "file", "optional-file", "text" };
static const char *const HOST_VERBS[] = { "file", "optional-file",
                                          "exec-dir", "tree" };

static int verb_in(const char *verb, const char *const set[], int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (strcmp(verb, set[i]) == 0)
            return 1;
    return 0;
}

#define IS_FILE_VERB(v) verb_in((v), FILE_VERBS, 3)
#define IS_HOST_VERB(v) verb_in((v), HOST_VERBS, 4)
#define IS_VERB(v) verb_in((v), VERBS, 6)

static int verb_arity(const char *verb)
{
    if (strcmp(verb, "dir") == 0 || strcmp(verb, "text") == 0)
        return 3;
    return 4;
}

static void manifest_free(struct manifest *m)
{
    int i;

    for (i = 0; i < m->n; i++) {
        free(m->d[i].verb);
        free(m->d[i].guest);
        free(m->d[i].source);
    }
    free(m->d);
    m->d = NULL;
    m->n = 0;
}

/* Parse a manifest into directives and the lines stageimage would refuse.
   The file is read as bytes and only '\n' ends a line, exactly as fgets
   reads it: a bare carriage return does not, which matters because
   `file 04755:0:0 /bin/su<CR>#x host` stages `/bin/su<CR>#x` and a
   universal-newline reader would see a declared path and a comment. */
static int parse_manifest(const char *path, struct manifest *m,
                          struct list *problems)
{
    size_t len, pos = 0;
    unsigned char *text = read_file(path, &len);
    const char *name = base_name(path);
    int lineno = 0;

    m->d = NULL;
    m->n = 0;
    if (text == NULL) {
        addf(problems, "%s: %s", name, strerror(errno));
        return -1;
    }
    while (pos < len) {
        size_t end = pos, rawlen, stored;
        char line[LINE_BUFFER];
        char copy[LINE_BUFFER];
        char *fields[4];
        int nf, i;
        unsigned long mode, uid, gid;
        char *verb, *source = NULL;
        struct directive *d;

        while (end < len && text[end] != '\n')
            end++;
        rawlen = end - pos + (end < len ? 1 : 0); /* the '\n' included */
        lineno++;

        /* fgets stores at most LINE_BUFFER-1 bytes and stageimage refuses
           a line that filled the buffer without a newline. */
        stored = rawlen < LINE_BUFFER - 1 ? rawlen : LINE_BUFFER - 1;
        if (stored == LINE_BUFFER - 1 &&
            !(stored == rawlen && end < len &&
              text[pos + stored - 1] == '\n')) {
            addf(problems, "%s:%d: line is longer than the %d bytes "
                 "stageimage accepts", name, lineno, MAX_LINE);
            pos = end + 1;
            continue;
        }
        memcpy(line, text + pos, stored);
        line[stored] = '\0';
        pos = end + 1;
        /* rstrip of '\r' and '\n'. A NUL byte inside the line survives in
           `stored` bytes; the length is tracked apart from strlen. */
        while (stored > 0 &&
               (line[stored - 1] == '\n' || line[stored - 1] == '\r'))
            line[--stored] = '\0';

        memcpy(copy, line, stored + 1);
        nf = tokenize(copy, fields, 4);
        if (nf == 0 || fields[0][0] == '#')
            continue;
        if (strlen(line) != stored) {
            addf(problems, "%s:%d: NUL byte in a directive; stageimage "
                 "reads the line with strlen and would stage only the part "
                 "before it", name, lineno);
            continue;
        }
        for (i = 0; i < (int) stored; i++)
            if ((unsigned char) line[i] > 0x7f)
                break;
        if (i < (int) stored) {
            addf(problems, "%s:%d: non-ASCII byte in a directive; a guest "
                 "path must be ASCII, since the image inventory names nodes "
                 "by their bytes", name, lineno);
            continue;
        }
        verb = fields[0];
        if (!IS_VERB(verb)) {
            addf(problems, "%s:%d: unknown directive '%s'; the grammar is "
                 "dir, text, file, optional-file, exec-dir, tree",
                 name, lineno, verb);
            continue;
        }
        if (nf < 3) {
            addf(problems, "%s:%d: malformed '%s' directive; it needs a "
                 "mode and a path", name, lineno, verb);
            continue;
        }
        if (!parse_mode(fields[1], &mode, &uid, &gid)) {
            addf(problems, "%s:%d: malformed mode '%s' in a '%s' directive",
                 name, lineno, fields[1], verb);
            continue;
        }
        if (nf < verb_arity(verb)) {
            addf(problems, "%s:%d: '%s' needs a host %s", name, lineno,
                 verb, strcmp(verb, "exec-dir") == 0 ? "dir" : "path");
            continue;
        }
        if (IS_HOST_VERB(verb)) {
            source = xstrdup(fields[3]);
        } else if (strcmp(verb, "text") == 0) {
            /* The payload: the rest of the line after the first occurrence
               of the path token, less the spaces and tabs in front of it,
               byte for byte before the escapes are expanded. */
            const char *at = strstr(line, fields[2]);

            if (at != NULL) {
                at += strlen(fields[2]);
                while (*at == ' ' || *at == '\t')
                    at++;
                source = xstrdup(at);
            } else {
                source = xstrdup("");
            }
        }
        m->d = realloc(m->d, (size_t) (m->n + 1) * sizeof(*m->d));
        if (m->d == NULL)
            exit(2);
        d = &m->d[m->n++];
        d->lineno = lineno;
        d->verb = xstrdup(verb);
        d->mode = mode;
        d->uid = uid;
        d->gid = gid;
        d->guest = xstrdup(fields[2]);
        d->source = source;
    }
    free(text);
    return 0;
}

/* ---- the sweep's rules ---- */

/* Check which init a manifest stages; see the allow file's `init` lines.
   The kernel execs /sbin/init as root before anything else runs, so it is
   the same trust boundary a set-id program is. */
static void check_init_line(const char *name, const struct manifest *m,
                            const char *want, struct list *problems)
{
    char wanted_source[256];
    int i, staged = 0;

    if (want == NULL) {
        for (i = 0; i < m->n; i++)
            if (strcmp(m->d[i].guest, "/sbin/init") == 0 &&
                IS_FILE_VERB(m->d[i].verb))
                addf(problems, "%s:%d: stages /sbin/init and the allowlist "
                     "declares none for this image", name, m->d[i].lineno);
        return;
    }
    snprintf(wanted_source, sizeof(wanted_source), "build/utils/sbin/%s",
             want);
    for (i = 0; i < m->n; i++) {
        const struct directive *d = &m->d[i];

        if (strcmp(d->guest, "/sbin/init") != 0 || !IS_FILE_VERB(d->verb))
            continue;
        staged = 1;
        if (d->source == NULL || strcmp(d->verb, "text") == 0 ||
            strcmp(d->source, wanted_source) != 0)
            addf(problems, "%s:%d: stages /sbin/init from %s and the "
                 "allowlist declares %s", name, d->lineno,
                 d->source != NULL && strcmp(d->verb, "text") != 0
                     ? d->source : "an inline payload", wanted_source);
        if (strcmp(d->verb, "file") != 0)
            addf(problems, "%s:%d: stages /sbin/init with '%s'; an image "
                 "that declares an init must carry one, so the directive "
                 "must be 'file'", name, d->lineno, d->verb);
        if (d->mode != INIT_MODE || d->uid != 0 || d->gid != 0)
            addf(problems, "%s:%d: stages /sbin/init %s; the kernel execs "
                 "it as root, so it must be %s", name, d->lineno,
                 fmt_entry(d->mode, d->uid, d->gid),
                 fmt_entry(INIT_MODE, 0, 0));
    }
    if (!staged)
        addf(problems, "%s: stages no /sbin/init and the allowlist "
             "declares %s", name, want);
}

/* Every path an allowlist entry says root runs or trusts, sorted setid
   first, then sorted trusted, then /sbin/init when one is declared. The
   result is a list of borrowed pointers. */
static void declared_paths(const struct entry *e, struct list *out)
{
    struct list tmp = { NULL, 0, 0 };
    int i;

    for (i = 0; i < e->n_setid; i++)
        list_push(&tmp, e->setid[i].path);
    qsort(tmp.items, (size_t) tmp.n, sizeof(char *), cmp_str);
    for (i = 0; i < tmp.n; i++)
        list_push(out, tmp.items[i]);
    tmp.n = 0;
    for (i = 0; i < e->n_trusted; i++)
        list_push(&tmp, e->trusted[i].path);
    qsort(tmp.items, (size_t) tmp.n, sizeof(char *), cmp_str);
    for (i = 0; i < tmp.n; i++)
        list_push(out, tmp.items[i]);
    free(tmp.items);
    if (e->init != NULL)
        list_push(out, (char *) "/sbin/init");
}

/* Check the directories above every declared path: each must be staged,
   owned by root, and writable by nobody else, because `unlink` and
   `rename` are decided on the parent directory's write permission. `look`
   answers a directory path with its mode and owner, or nonzero for "not
   staged". */
typedef int (*dir_look)(void *ctx, const char *path, unsigned long *mode,
                        unsigned long *uid, unsigned long *gid);

static void check_ancestors(const char *where, const struct entry *e,
                            dir_look look, void *ctx, struct list *problems)
{
    struct list paths = { NULL, 0, 0 };
    struct list seen = { NULL, 0, 0 };
    int i;

    declared_paths(e, &paths);
    for (i = 0; i < paths.n; i++) {
        const char *guest = paths.items[i];
        const char *p = guest + 1;
        char parent[512];

        while ((p = strchr(p, '/')) != NULL) {
            size_t n = (size_t) (p - guest);
            unsigned long mode, uid, gid;

            if (n >= sizeof(parent))
                break;
            memcpy(parent, guest, n);
            parent[n] = '\0';
            p++;
            if (list_has(&seen, parent))
                continue;
            list_push(&seen, xstrdup(parent));
            if (look(ctx, parent, &mode, &uid, &gid) != 0) {
                addf(problems, "%s: %s has a program root runs and no "
                     "'dir' line stages it; a directory that comes into "
                     "being on the way to a file takes a mode nobody wrote "
                     "down", where, parent);
                continue;
            }
            if (uid != 0 || gid != 0 || (mode & 0022) != 0)
                addf(problems, "%s: %s is staged %s and has a program root "
                     "runs; a directory anyone else may write is a way to "
                     "replace what is in it", where, parent,
                     fmt_entry(mode, uid, gid));
        }
    }
    free(paths.items);
    list_free(&seen);
}

/* What the sweep records as staged for a path. */
struct staged {
    char *guest;
    int lineno;
    unsigned long mode, uid, gid;
    char *source;      /* may be NULL */
    const char *verb;  /* first verb for the path, or NULL for tree-pinned */
};

struct staged_map {
    struct staged *s;
    int n;
};

static struct staged *staged_find(struct staged_map *map, const char *guest)
{
    int i;

    for (i = 0; i < map->n; i++)
        if (strcmp(map->s[i].guest, guest) == 0)
            return &map->s[i];
    return NULL;
}

static struct staged *staged_add(struct staged_map *map, const char *guest,
                                 int lineno, unsigned long mode,
                                 unsigned long uid, unsigned long gid,
                                 const char *source, const char *verb)
{
    map->s = realloc(map->s, (size_t) (map->n + 1) * sizeof(*map->s));
    if (map->s == NULL)
        exit(2);
    map->s[map->n].guest = xstrdup(guest);
    map->s[map->n].lineno = lineno;
    map->s[map->n].mode = mode;
    map->s[map->n].uid = uid;
    map->s[map->n].gid = gid;
    map->s[map->n].source = source ? xstrdup(source) : NULL;
    map->s[map->n].verb = verb;
    return &map->s[map->n++];
}

static void staged_free(struct staged_map *map)
{
    int i;

    for (i = 0; i < map->n; i++) {
        free(map->s[i].guest);
        free(map->s[i].source);
    }
    free(map->s);
    map->s = NULL;
    map->n = 0;
}

static const struct setid_decl *setid_find(const struct entry *e,
                                           const char *path)
{
    int i;

    for (i = 0; i < e->n_setid; i++)
        if (strcmp(e->setid[i].path, path) == 0)
            return &e->setid[i];
    return NULL;
}

struct manifest_dirs {
    const struct manifest *m;
};

static int manifest_dir_look(void *ctx, const char *path,
                             unsigned long *mode, unsigned long *uid,
                             unsigned long *gid)
{
    const struct manifest *m = ((struct manifest_dirs *) ctx)->m;
    int i, found = 1;

    /* The LAST `dir` for a path decides, the way `stage_dir` re-applies
       the mode to the node it finds. */
    for (i = 0; i < m->n; i++) {
        if (strcmp(m->d[i].verb, "dir") == 0 &&
            strcmp(m->d[i].guest, path) == 0) {
            *mode = m->d[i].mode & PERMBITS;
            *uid = m->d[i].uid;
            *gid = m->d[i].gid;
            found = 0;
        }
    }
    return found;
}

/* The first verb a manifest uses for a guest path, or NULL. */
static const char *first_verb(const struct manifest *m, const char *guest)
{
    int i;

    for (i = 0; i < m->n; i++)
        if (strcmp(m->d[i].guest, guest) == 0)
            return m->d[i].verb;
    return NULL;
}

/* Check one manifest against the allowlist. */
static void check_manifest(const char *path, const struct allow *allow,
                           struct list *problems)
{
    const char *name = base_name(path);
    char *stem = manifest_stem(path);
    const struct entry *e = find_entry((struct allow *) allow, stem);
    struct manifest m;
    struct staged_map staged = { NULL, 0 };
    struct manifest_dirs dctx;
    struct list sorted = { NULL, 0, 0 };
    int i, j;

    if (e == NULL) {
        addf(problems, "%s: no allowlist names this manifest; add its "
             "entry to tools/inspect/setid.allow (a new manifest declares "
             "its set-id set, or declares that it has none)", name);
        free(stem);
        return;
    }
    if (parse_manifest(path, &m, problems) != 0) {
        free(stem);
        return;
    }

    /* A trusted path may sit inside a staged source tree: `tree` copies
       every regular file under its host directory, so a pinned path under
       a tree's guest directory resolves to the host file the walk will
       copy, carrying the tree's mode and owner. */
    for (i = 0; i < m.n; i++) {
        const struct directive *d = &m.d[i];
        char prefix[512];
        size_t plen;

        if (strcmp(d->verb, "tree") != 0)
            continue;
        snprintf(prefix, sizeof(prefix), "%s", d->guest);
        plen = strlen(prefix);
        while (plen > 0 && prefix[plen - 1] == '/')
            prefix[--plen] = '\0';
        prefix[plen++] = '/';
        prefix[plen] = '\0';
        for (j = 0; j < e->n_trusted; j++) {
            const char *pinned = e->trusted[j].path;
            char host[1024], src[512];
            size_t slen;
            struct stat st;

            if (strncmp(pinned, prefix, plen) != 0 ||
                staged_find(&staged, pinned) != NULL)
                continue;
            snprintf(src, sizeof(src), "%s", d->source);
            slen = strlen(src);
            while (slen > 0 && src[slen - 1] == '/')
                src[--slen] = '\0';
            snprintf(host, sizeof(host), "%s/%s", src, pinned + plen);
            if (stat(host, &st) == 0 && S_ISREG(st.st_mode))
                staged_add(&staged, pinned, d->lineno, d->mode, d->uid,
                           d->gid, host, NULL);
        }
    }

    check_init_line(name, &m, e->init, problems);

    for (i = 0; i < m.n; i++) {
        const struct directive *d = &m.d[i];
        const struct setid_decl *decl;

        if (IS_FILE_VERB(d->verb)) {
            if (staged_find(&staged, d->guest) != NULL)
                addf(problems, "%s:%d: %s is staged more than once; "
                     "stageimage refuses a repeated path", name, d->lineno,
                     d->guest);
            else
                staged_add(&staged, d->guest, d->lineno, d->mode, d->uid,
                           d->gid, d->source, d->verb);
        }
        if (!(d->mode & S_SETID))
            continue;
        if (strcmp(d->verb, "exec-dir") == 0 ||
            strcmp(d->verb, "tree") == 0) {
            addf(problems, "%s:%d: '%s' stages mode %s into %s; a set-id "
                 "%s privileges whatever happens to be in the host "
                 "directory, so it is refused. Name the one binary with "
                 "'file'.", name, d->lineno, d->verb,
                 fmt_entry(d->mode, d->uid, d->gid), d->guest, d->verb);
            continue;
        }
        if (strcmp(d->verb, "dir") == 0) {
            addf(problems, "%s:%d: 'dir' stages %s at mode %s; KRFFS has "
                 "no setgid-directory inheritance, so nothing would ever "
                 "read the bit. Sticky (01000) is legal.", name, d->lineno,
                 d->guest, fmt_entry(d->mode, d->uid, d->gid));
            continue;
        }
        decl = setid_find(e, d->guest);
        if (decl == NULL) {
            addf(problems, "%s:%d: '%s' stages %s set-id (%s) and the %s "
                 "allowlist does not declare it", name, d->lineno, d->verb,
                 d->guest, fmt_entry(d->mode, d->uid, d->gid), stem);
            continue;
        }
        if (d->mode != decl->mode || d->uid != decl->uid ||
            d->gid != decl->gid)
            addf(problems, "%s:%d: %s is staged %s but the %s allowlist "
                 "declares %s", name, d->lineno, d->guest,
                 fmt_entry(d->mode, d->uid, d->gid), stem,
                 fmt_entry(decl->mode, decl->uid, decl->gid));
        else if (d->source == NULL || strcmp(d->verb, "text") == 0)
            addf(problems, "%s:%d: '%s' stages the declared set-id path "
                 "%s inline; the %s allowlist declares it staged from %s, "
                 "and a privileged file is a program copied from the host",
                 name, d->lineno, d->verb, d->guest, stem, decl->source);
        else if (strcmp(d->source, decl->source) != 0)
            addf(problems, "%s:%d: %s is staged from %s but the %s "
                 "allowlist declares it staged from %s", name, d->lineno,
                 d->guest, d->source, stem, decl->source);
    }

    for (i = 0; i < e->n_setid; i++)
        list_push(&sorted, e->setid[i].path);
    qsort(sorted.items, (size_t) sorted.n, sizeof(char *), cmp_str);
    for (i = 0; i < sorted.n; i++) {
        const struct setid_decl *decl = setid_find(e, sorted.items[i]);
        const struct staged *s = staged_find(&staged, decl->path);

        if (s != NULL) {
            if (s->mode != decl->mode || s->uid != decl->uid ||
                s->gid != decl->gid)
                addf(problems, "%s:%d: %s is declared %s but is staged "
                     "%s; the declared privilege is gone", name, s->lineno,
                     decl->path,
                     fmt_entry(decl->mode, decl->uid, decl->gid),
                     fmt_entry(s->mode, s->uid, s->gid));
        } else {
            addf(problems, "%s: %s is declared %s but no directive stages "
                 "it", name, decl->path,
                 fmt_entry(decl->mode, decl->uid, decl->gid));
        }
    }
    sorted.n = 0;

    dctx.m = &m;
    check_ancestors(name, e, manifest_dir_look, &dctx, problems);

    for (i = 0; i < e->n_trusted; i++)
        list_push(&sorted, e->trusted[i].path);
    qsort(sorted.items, (size_t) sorted.n, sizeof(char *), cmp_str);
    for (i = 0; i < sorted.n; i++) {
        const struct trusted_decl *t = NULL;
        const struct staged *s;
        const char *staged_verb;

        for (j = 0; j < e->n_trusted; j++)
            if (strcmp(e->trusted[j].path, sorted.items[i]) == 0)
                t = &e->trusted[j];
        s = staged_find(&staged, t->path);
        if (s == NULL) {
            addf(problems, "%s: %s is not staged; the allowlist requires "
                 "it to be present, unprivileged and exactly as declared",
                 name, t->path);
            continue;
        }
        staged_verb = s->verb != NULL ? s->verb : first_verb(&m, t->path);
        if (s->mode & S_SETID)
            addf(problems, "%s:%d: %s is staged set-id (%s); it must stay "
                 "unprivileged, and privilege must climb through the "
                 "declared paths alone", name, s->lineno, t->path,
                 fmt_entry(s->mode, s->uid, s->gid));
        else if (s->mode != t->mode || s->uid != t->uid || s->gid != t->gid)
            addf(problems, "%s:%d: %s is declared %s but is staged %s; a "
                 "program root runs, and a file root reads to decide who "
                 "somebody is, are trust boundaries too", name, s->lineno,
                 t->path, fmt_entry(t->mode, t->uid, t->gid),
                 fmt_entry(s->mode, s->uid, s->gid));
        /* A pinned path under a `tree` directive has no directive of its
           own: the walk copies it from the host file, which is what a
           'file' declaration means. */
        else if (staged_verb != NULL && strcmp(staged_verb, t->verb) != 0)
            addf(problems, "%s:%d: %s is staged with '%s' and the "
                 "allowlist declares '%s'; a file copied from the host and "
                 "a payload written in the manifest are not the same "
                 "thing, whatever the words after them say", name,
                 s->lineno, t->path, staged_verb, t->verb);
        else if (s->source == NULL || strcmp(s->source, t->value) != 0) {
            if (strcmp(t->verb, "text") == 0)
                addf(problems, "%s:%d: %s does not hold the text the "
                     "allowlist pins; a credential file's meaning is its "
                     "content, so the two are declared together", name,
                     s->lineno, t->path);
            else
                addf(problems, "%s:%d: %s is declared staged from %s but "
                     "this stages %s", name, s->lineno, t->path, t->value,
                     s->source != NULL ? s->source : "an inline payload");
        }
    }
    free(sorted.items);
    staged_free(&staged);
    manifest_free(&m);
    free(stem);
}

/* Check the allowlist itself: every entry has something to find. */
static void validate_table(const struct allow *allow, struct list *problems)
{
    struct list stems = { NULL, 0, 0 };
    int i, j;

    for (i = 0; i < allow->n; i++)
        list_push(&stems, allow->entries[i].stem);
    qsort(stems.items, (size_t) stems.n, sizeof(char *), cmp_str);
    for (i = 0; i < stems.n; i++) {
        const struct entry *e = find_entry((struct allow *) allow,
                                           stems.items[i]);
        struct list paths = { NULL, 0, 0 };

        if (e->n_setid == 0 && e->sentinel_path == NULL)
            addf(problems, "allowlist: %s declares no set-id entry and no "
                 "sentinel; the image inventory would have nothing to find",
                 e->stem);
        for (j = 0; j < e->n_setid; j++)
            list_push(&paths, e->setid[j].path);
        qsort(paths.items, (size_t) paths.n, sizeof(char *), cmp_str);
        for (j = 0; j < paths.n; j++) {
            const struct setid_decl *d = setid_find(e, paths.items[j]);

            if (!(d->mode & S_SETID))
                addf(problems, "allowlist: %s declares %s at mode 0%lo, "
                     "which has no set-id bit", e->stem, d->path, d->mode);
        }
        free(paths.items);
    }
    free(stems.items);
}

/* ---- the image inventory: fsck.krffs's walk, and the checks over it ---- */

struct node {
    unsigned magic;
    unsigned type;
    uint64_t id;
    char name[NODE_NAME_LEN + 1];
    unsigned long mode, uid, gid;
    uint64_t size, previous_node_size, data_size, parent_id;
    size_t offset;
    char *path; /* filled by node_paths for reserved nodes */
    unsigned char *data; /* an ext2 file's bytes, read from its blocks */
    size_t data_len;
};

struct image {
    unsigned char *region;
    size_t size;
    struct node *nodes;
    int n_nodes;
    char err[512];
};

static uint64_t rd64(const unsigned char *p)
{
    uint64_t v = 0;
    int i;

    for (i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}
static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long) p[0] | (unsigned long) p[1] << 8 |
           (unsigned long) p[2] << 16 | (unsigned long) p[3] << 24;
}
static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }

static int img_fail(struct image *im, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(im->err, sizeof(im->err), fmt, ap);
    va_end(ap);
    return -1;
}

/* Decode one on-disk node. The name is decoded strictly: a node name is a
   guest path component, and a lossy reading would fold two distinct names
   onto one path, which is how a set-id node hides behind a sibling. */
static int unpack_node(struct image *im, size_t offset, struct node *n)
{
    memset(n, 0, sizeof(*n));
    const unsigned char *p = im->region + offset;
    int i;

    n->magic = rd16(p + NODE_MAGIC_OFF);
    n->type = p[NODE_TYPE_OFF];
    n->id = rd64(p + NODE_ID_OFF);
    for (i = 0; i < NODE_NAME_LEN && p[NODE_NAME_OFF + i] != 0; i++) {
        if (p[NODE_NAME_OFF + i] > 0x7f)
            return img_fail(im, "the node at offset %lu has a name that is "
                            "not ASCII, so no path names it unambiguously",
                            (unsigned long) offset);
        n->name[i] = (char) p[NODE_NAME_OFF + i];
    }
    n->name[i] = '\0';
    n->mode = rd16(p + NODE_MODE_OFF);
    n->uid = rd32(p + NODE_UID_OFF);
    n->gid = rd32(p + NODE_GID_OFF);
    n->size = rd64(p + NODE_SIZEF_OFF);
    n->previous_node_size = rd64(p + NODE_PREV_OFF);
    n->data_size = rd64(p + NODE_DATA_OFF);
    n->parent_id = rd64(p + NODE_PARENT_OFF);
    n->offset = offset;
    n->path = NULL;
    if (n->type == NODE_RESERVED &&
        (n->name[0] == '\0' || strcmp(n->name, ".") == 0 ||
         strcmp(n->name, "..") == 0 || strchr(n->name, '/') != NULL))
        return img_fail(im, "the node at offset %lu is named '%s', which "
                        "is not one path component; the path this "
                        "inventory would build from it is not the path the "
                        "kernel resolves", (unsigned long) offset, n->name);
    return 0;
}

/* Walk the node chain from the root: each node advances by its own size,
   wrapping once at the region end, capped at the most nodes the region
   could hold. The ring must close on the size the root records, because
   the walk alone cannot tell a whole file system from a prefix of one. */
static int walk_nodes(struct image *im)
{
    size_t position = 0, previous = 0;
    long steps = 0, max_steps;

    if (im->size < NODE_SIZE * 2)
        return img_fail(im, "region of %lu bytes is too small to hold a "
                        "file system", (unsigned long) im->size);
    max_steps = (long) (im->size / NODE_SIZE) + 1;
    im->nodes = NULL;
    im->n_nodes = 0;
    for (;;) {
        struct node n;

        steps++;
        if (steps > max_steps)
            return img_fail(im, "the node chain does not return to the "
                            "root within the region bound; the image is "
                            "corrupt");
        if (previous > position)
            return img_fail(im, "the node chain links backwards to offset "
                            "%lu; the image is corrupt",
                            (unsigned long) position);
        if (position + NODE_SIZE > im->size)
            return img_fail(im, "node at offset %lu lies outside the "
                            "%lu-byte region", (unsigned long) position,
                            (unsigned long) im->size);
        if (unpack_node(im, position, &n) != 0)
            return -1;
        if (n.magic != KRFFS_MAGIC)
            return img_fail(im, "no file system signature in the node at "
                            "offset %lu (found 0x%04x)",
                            (unsigned long) position, n.magic);
        if (steps == 1 && n.type != NODE_ROOT)
            return img_fail(im, "the node at the region start is not the "
                            "root node (type %u)", n.type);
        if (n.type != NODE_ROOT && n.type != NODE_FREE &&
            n.type != NODE_RESERVED)
            return img_fail(im, "node at offset %lu has unknown type %u",
                            (unsigned long) position, n.type);
        if (n.size < NODE_SIZE || n.size > im->size)
            return img_fail(im, "node at offset %lu claims a size of %llu "
                            "bytes, which no node in a %lu-byte region can "
                            "have", (unsigned long) position,
                            (unsigned long long) n.size,
                            (unsigned long) im->size);
        if (n.data_size > n.size - NODE_SIZE ||
            n.data_size > im->size - position - NODE_SIZE)
            return img_fail(im, "node at offset %lu claims %llu bytes of "
                            "data, which do not fit in its own %llu bytes "
                            "or in the region",
                            (unsigned long) position,
                            (unsigned long long) n.data_size,
                            (unsigned long long) n.size);
        im->nodes = realloc(im->nodes,
                            (size_t) (im->n_nodes + 1) * sizeof(n));
        if (im->nodes == NULL)
            exit(2);
        im->nodes[im->n_nodes++] = n;

        previous = position;
        position += (size_t) n.size;
        if (position >= im->size)
            position -= im->size;
        if (position == 0)
            break;
    }
    if (im->nodes[0].previous_node_size != im->nodes[im->n_nodes - 1].size)
        return img_fail(im, "the node ring does not close: the root "
                        "records a last node of %llu bytes and the walk "
                        "ended on one of %llu, so the region handed in is "
                        "not the whole file system",
                        (unsigned long long) im->nodes[0].previous_node_size,
                        (unsigned long long) im->nodes[im->n_nodes - 1].size);
    return 0;
}

/* Rebuild the absolute path of every reserved node from `parent_id`. The
   root's id is 0 as a parent and `krffs_mix_id` never returns 0, so a
   chain terminates only at the root; anything else is an error, not a
   skipped node, since a node the inventory cannot name is a node it
   cannot clear. */
static int node_paths(struct image *im)
{
    int i, j;

    for (i = 0; i < im->n_nodes; i++) {
        if (im->nodes[i].type != NODE_RESERVED)
            continue;
        for (j = 0; j < i; j++)
            if (im->nodes[j].type == NODE_RESERVED &&
                im->nodes[j].id == im->nodes[i].id)
                return img_fail(im, "two reserved nodes share id %llu "
                                "('%s' at offset %lu and '%s' at offset "
                                "%lu), so no path is unambiguous",
                                (unsigned long long) im->nodes[i].id,
                                im->nodes[j].name,
                                (unsigned long) im->nodes[j].offset,
                                im->nodes[i].name,
                                (unsigned long) im->nodes[i].offset);
    }
    for (i = 0; i < im->n_nodes; i++) {
        struct node *n = &im->nodes[i];
        char path[1024];
        char part[NODE_NAME_LEN + 2];
        const struct node *walker = n;
        int hops = 0;

        if (n->type != NODE_RESERVED)
            continue;
        path[0] = '\0';
        for (;;) {
            uint64_t parent;

            snprintf(part, sizeof(part), "/%s", walker->name);
            if (strlen(part) + strlen(path) + 1 > sizeof(path))
                return img_fail(im, "the path of node '%s' at offset %lu "
                                "is too long to rebuild", n->name,
                                (unsigned long) n->offset);
            memmove(path + strlen(part), path, strlen(path) + 1);
            memcpy(path, part, strlen(part));
            parent = walker->parent_id;
            if (parent == 0)
                break;
            if (++hops > im->n_nodes)
                return img_fail(im, "the parent chain of node '%s' at "
                                "offset %lu is cyclic", n->name,
                                (unsigned long) n->offset);
            walker = NULL;
            for (j = 0; j < im->n_nodes; j++)
                if (im->nodes[j].type == NODE_RESERVED &&
                    im->nodes[j].id == parent)
                    walker = &im->nodes[j];
            if (walker == NULL)
                return img_fail(im, "node '%s' at offset %lu names parent "
                                "id %llu, which no node has", n->name,
                                (unsigned long) n->offset,
                                (unsigned long long) parent);
        }
        for (j = 0; j < i; j++)
            if (im->nodes[j].path != NULL &&
                strcmp(im->nodes[j].path, path) == 0)
                return img_fail(im, "two reserved nodes resolve to the "
                                "path %s (at offsets %lu and %lu), so one "
                                "of them would go uninspected", path,
                                (unsigned long) im->nodes[j].offset,
                                (unsigned long) n->offset);
        n->path = xstrdup(path);
    }
    return 0;
}

/* Read the `size` bytes at `offset` of `path`, checking the bounds. A
   size of 0 means the rest of the file. */
/* A number given on the command line, parsed strictly: a mistyped offset
   that reads as 0 would inventory a different region and report on that
   one. */
static int parse_num(const char *s, long *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 0);
    if (end == s || *end != '\0' || errno != 0)
        return -1;
    *out = v;
    return 0;
}

static int read_region(struct image *im, const char *path, long offset,
                       long size)
{
    struct stat st;
    FILE *f;

    memset(im, 0, sizeof(*im));
    if (stat(path, &st) != 0)
        return img_fail(im, "%s", strerror(errno));
    if (offset < 0 || offset > st.st_size)
        return img_fail(im, "offset %ld is past the end of the %lld-byte "
                        "image", offset, (long long) st.st_size);
    if (size == 0)
        size = (long) (st.st_size - offset);
    if (size <= 0 || offset + size > st.st_size)
        return img_fail(im, "region (offset %ld, size %ld) does not fit "
                        "in the %lld-byte image", offset, size,
                        (long long) st.st_size);
    f = fopen(path, "rb");
    if (f == NULL)
        return img_fail(im, "%s", strerror(errno));
    im->region = xmalloc((size_t) size);
    if (fseek(f, offset, SEEK_SET) != 0 ||
        fread(im->region, 1, (size_t) size, f) != (size_t) size) {
        fclose(f);
        return img_fail(im, "short read of the region at offset %ld",
                        offset);
    }
    fclose(f);
    im->size = (size_t) size;
    return 0;
}

static void image_free(struct image *im)
{
    int i;

    for (i = 0; i < im->n_nodes; i++) {
        free(im->nodes[i].path);
        free(im->nodes[i].data);
    }
    free(im->nodes);
    free(im->region);
    memset(im, 0, sizeof(*im));
}

/* A node's bytes: an ext2 node's own copy, a KRFFS node's place in the
   region. */
static const unsigned char *node_bytes(const struct image *im,
                                       const struct node *n, size_t *len)
{
    if (n->data != NULL) {
        *len = n->data_len;
        return n->data;
    }
    if (n->offset + NODE_SIZE + n->data_size > im->size) {
        *len = 0;
        return NULL;
    }
    *len = (size_t) n->data_size;
    return im->region + n->offset + NODE_SIZE;
}

static const struct node *path_find(const struct image *im, const char *path)
{
    int i;

    for (i = 0; i < im->n_nodes; i++)
        if (im->nodes[i].path != NULL &&
            strcmp(im->nodes[i].path, path) == 0)
            return &im->nodes[i];
    return NULL;
}

static int image_dir_look(void *ctx, const char *path, unsigned long *mode,
                          unsigned long *uid, unsigned long *gid)
{
    const struct node *n = path_find((const struct image *) ctx, path);

    if (n == NULL || !(n->mode & 040000))
        return 1;
    *mode = n->mode & PERMBITS;
    *uid = n->uid;
    *gid = n->gid;
    return 0;
}

/* Expand a declared text payload the way `unescape_text` does: a
   backslash before n, t or a backslash is that character, and a backslash
   before anything else stays as written. Returns a fresh buffer. */
static unsigned char *unescape(const char *want, size_t *out_len)
{
    unsigned char *out = xmalloc(strlen(want) + 1);
    size_t n = 0;
    const char *p = want;

    while (*p != '\0') {
        if (*p == '\\' && p[1] != '\0') {
            if (p[1] == 'n')
                out[n++] = '\n';
            else if (p[1] == 't')
                out[n++] = '\t';
            else if (p[1] == '\\')
                out[n++] = '\\';
            else {
                out[n++] = '\\';
                out[n++] = (unsigned char) p[1];
            }
            p += 2;
        } else {
            out[n++] = (unsigned char) *p++;
        }
    }
    *out_len = n;
    return out;
}

/* Does the staged node at `path` hold exactly the pinned payload?
   Returns -1 when the path is absent, 0 for a mismatch, 1 for a match. */
static int trusted_text_matches(const struct image *im, const char *path,
                                const char *want)
{
    const struct node *n = path_find(im, path);
    size_t want_len;
    unsigned char *bytes;
    int same;

    if (n == NULL)
        return -1;
    bytes = unescape(want, &want_len);
    {
        size_t have_len;
        const unsigned char *have = node_bytes(im, n, &have_len);

        same = have_len == want_len &&
            (want_len == 0 || memcmp(have, bytes, want_len) == 0);
    }
    free(bytes);
    return same;
}


/* ---- the ext2 reader ----

   A small read-only reader of its own, sharing nothing with the kernel's
   driver, for the one profile tools/image/mkext2.sh writes: revision 1,
   4 KiB blocks, 128-byte inodes, and the features filetype, sparse_super
   and large_file with no other. It fails closed: any other feature, any
   block outside the volume or inside its structural blocks, any malformed
   directory entry and any inode it cannot place refuses the image, so that
   an image this checker cannot read is one the build rejects. It lists
   every node reached from the root with its type, owner, group, mode and
   set-id bits, follows every directory to its end, and lists a second name
   for one inode as a node of its own, which is how a hard link to a set-id
   file meets the policy as an undeclared set-id path. */

#define EXT2_MAGIC 0xEF53u
#define EXT2_BLOCK 4096u
#define EXT2_INODE 128u
#define EXT2_FIRST_INO 11u
#define EXT2_FT_MAX 7u

struct ext2 {
    FILE *f;
    long base;
    uint64_t size;
    unsigned long blocks, inodes, bpg, ipg, groups;
    unsigned char *gd;          /* the group descriptors, 32 bytes each */
    unsigned char *structural;  /* one byte per block: 1 if a table's */
    unsigned char *seen_dir;    /* one byte per inode: a directory reached */
    unsigned char *seen_file;   /* one byte per inode: a file reached */
    char err[512];
};

static int ext2_fail(struct ext2 *fs, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(fs->err, sizeof(fs->err), fmt, ap);
    va_end(ap);
    return -1;
}

static int ext2_read(struct ext2 *fs, uint64_t at, void *buf, size_t len)
{
    if (at + len > fs->size)
        return ext2_fail(fs, "a read at %llu of %lu bytes runs past the "
                         "%llu-byte volume", (unsigned long long) at,
                         (unsigned long) len, (unsigned long long) fs->size);
    if (fseek(fs->f, fs->base + (long) at, SEEK_SET) != 0 ||
        fread(buf, 1, len, fs->f) != len)
        return ext2_fail(fs, "short read at %llu", (unsigned long long) at);
    return 0;
}

/* A block a pointer names: inside the volume and not one of the tables. */
static int ext2_data_block(struct ext2 *fs, unsigned long b,
                           unsigned char *buf, const char *what)
{
    if (b == 0 || b >= fs->blocks)
        return ext2_fail(fs, "%s names block %lu, outside the %lu-block "
                         "volume", what, b, fs->blocks);
    if (fs->structural[b])
        return ext2_fail(fs, "%s names block %lu, which is a bitmap, a "
                         "table or a superblock", what, b);
    return ext2_read(fs, (uint64_t) b * EXT2_BLOCK, buf, EXT2_BLOCK);
}

static int ext2_open(struct ext2 *fs, const char *path, long offset,
                     long size)
{
    unsigned char sb[1024];
    struct stat st;
    unsigned long g, i;
    unsigned long compat, incompat, ro_compat;

    memset(fs, 0, sizeof(*fs));
    if (stat(path, &st) != 0)
        return ext2_fail(fs, "%s", strerror(errno));
    if (offset < 0 || offset > st.st_size)
        return ext2_fail(fs, "offset %ld is past the end of the %lld-byte "
                         "file", offset, (long long) st.st_size);
    if (size == 0)
        size = (long) (st.st_size - offset);
    if (size <= 0 || offset + size > st.st_size)
        return ext2_fail(fs, "region (offset %ld, size %ld) does not fit "
                         "in the %lld-byte file", offset, size,
                         (long long) st.st_size);
    fs->f = fopen(path, "rb");
    if (fs->f == NULL)
        return ext2_fail(fs, "%s", strerror(errno));
    fs->base = offset;
    fs->size = (uint64_t) size;
    if (ext2_read(fs, 1024, sb, sizeof(sb)) != 0)
        return -1;
    if (rd16(sb + 56) != EXT2_MAGIC)
        return ext2_fail(fs, "no ext2 magic at byte 1080");
    if (rd32(sb + 76) != 1)
        return ext2_fail(fs, "revision %lu; the profile is revision 1",
                         rd32(sb + 76));
    if (rd32(sb + 24) != 2)
        return ext2_fail(fs, "block size %lu; the profile is 4096",
                         1024ul << rd32(sb + 24));
    if (rd16(sb + 88) != EXT2_INODE)
        return ext2_fail(fs, "inode size %u; the profile is 128",
                         rd16(sb + 88));
    compat = rd32(sb + 92);
    incompat = rd32(sb + 96);
    ro_compat = rd32(sb + 100);
    if (compat != 0 || incompat != 0x2 || ro_compat != 0x3)
        return ext2_fail(fs, "features %#lx/%#lx/%#lx (compatible, "
                         "incompatible, read-only); the profile is "
                         "0/0x2/0x3, filetype, sparse_super and large_file "
                         "alone", compat, incompat, ro_compat);
    if (rd32(sb + 84) != EXT2_FIRST_INO)
        return ext2_fail(fs, "first inode %lu; the profile is 11",
                         rd32(sb + 84));
    fs->inodes = rd32(sb + 0);
    fs->blocks = rd32(sb + 4);
    fs->bpg = rd32(sb + 32);
    fs->ipg = rd32(sb + 40);
    if (rd32(sb + 20) != 0)
        return ext2_fail(fs, "first data block %lu; at 4096-byte blocks "
                         "it is 0", rd32(sb + 20));
    if (fs->blocks == 0 || fs->bpg == 0 || fs->ipg == 0 ||
        fs->inodes == 0 || fs->ipg > fs->inodes ||
        (uint64_t) fs->blocks * EXT2_BLOCK > fs->size)
        return ext2_fail(fs, "%lu blocks, %lu per group, %lu inodes, %lu "
                         "per group do not describe the %llu-byte region",
                         fs->blocks, fs->bpg, fs->inodes, fs->ipg,
                         (unsigned long long) fs->size);
    fs->groups = (fs->blocks + fs->bpg - 1) / fs->bpg;
    if (fs->groups * fs->ipg < fs->inodes)
        return ext2_fail(fs, "%lu inodes do not fit %lu groups of %lu",
                         fs->inodes, fs->groups, fs->ipg);
    fs->gd = xmalloc(fs->groups * 32);
    if (ext2_read(fs, EXT2_BLOCK, fs->gd, fs->groups * 32) != 0)
        return -1;
    fs->structural = xmalloc(fs->blocks);
    memset(fs->structural, 0, fs->blocks);
    fs->seen_dir = xmalloc(fs->inodes + 1);
    memset(fs->seen_dir, 0, fs->inodes + 1);
    fs->seen_file = xmalloc(fs->inodes + 1);
    memset(fs->seen_file, 0, fs->inodes + 1);
    /* The superblock and the descriptors, in group 0 and in every backup
       sparse_super keeps (groups 0, 1 and the powers of 3, 5 and 7), and
       each group's two bitmaps and inode table. */
    fs->structural[0] = 1;
    fs->structural[1] = 1;
    for (g = 0; g < fs->groups; g++) {
        unsigned long bb = rd32(fs->gd + g * 32 + 0);
        unsigned long ib = rd32(fs->gd + g * 32 + 4);
        unsigned long it = rd32(fs->gd + g * 32 + 8);
        unsigned long it_blocks = (fs->ipg * EXT2_INODE + EXT2_BLOCK - 1) /
                                  EXT2_BLOCK;
        unsigned long p;

        if (bb >= fs->blocks || ib >= fs->blocks || it >= fs->blocks ||
            it + it_blocks > fs->blocks)
            return ext2_fail(fs, "group %lu names tables outside the "
                             "volume", g);
        fs->structural[bb] = 1;
        fs->structural[ib] = 1;
        for (i = 0; i < it_blocks; i++)
            fs->structural[it + i] = 1;
        p = g;
        if (g == 0 || g == 1)
            p = 1;
        else {
            while (p % 3 == 0)
                p /= 3;
            while (p % 5 == 0)
                p /= 5;
            while (p % 7 == 0)
                p /= 7;
        }
        if (p == 1) {
            unsigned long first = g * fs->bpg;
            unsigned long gd_blocks = (fs->groups * 32 + EXT2_BLOCK - 1) /
                                      EXT2_BLOCK;

            for (i = 0; i <= gd_blocks && first + i < fs->blocks; i++)
                fs->structural[first + i] = 1;
        }
    }
    return 0;
}

static void ext2_close(struct ext2 *fs)
{
    if (fs->f != NULL)
        fclose(fs->f);
    free(fs->gd);
    free(fs->structural);
    free(fs->seen_dir);
    free(fs->seen_file);
    fs->f = NULL;
    fs->gd = NULL;
    fs->structural = fs->seen_dir = fs->seen_file = NULL;
}

/* The 128 bytes of inode `ino`, placed through its group's table. */
static int ext2_inode(struct ext2 *fs, unsigned long ino, unsigned char *out)
{
    unsigned long g, index, table;

    if (ino == 0 || ino > fs->inodes)
        return ext2_fail(fs, "inode %lu cannot be placed in %lu inodes",
                         ino, fs->inodes);
    g = (ino - 1) / fs->ipg;
    index = (ino - 1) % fs->ipg;
    table = rd32(fs->gd + g * 32 + 8);
    return ext2_read(fs, (uint64_t) table * EXT2_BLOCK +
                     (uint64_t) index * EXT2_INODE, out, EXT2_INODE);
}

/* Every block of an inode's data in order, through the three levels of
   pointers, each handed to `take` with its index; a hole is handed as a
   zero block. Stops at `count` blocks, the size's. */
struct ext2_blocks {
    struct ext2 *fs;
    unsigned long count, done;
    int (*take)(void *ctx, unsigned long index, const unsigned char *block);
    void *ctx;
};

static int ext2_take_pointer(struct ext2_blocks *w, unsigned long b,
                             int level, const char *what)
{
    unsigned char buf[EXT2_BLOCK];
    unsigned long i;

    if (w->done >= w->count)
        return 0;
    if (b == 0) {
        /* A hole: a block of zeros at this level's every position. */
        unsigned long span = 1;

        for (i = 0; i < (unsigned long) level; i++)
            span *= EXT2_BLOCK / 4;
        memset(buf, 0, sizeof(buf));
        for (i = 0; i < span && w->done < w->count; i++, w->done++)
            if (w->take(w->ctx, w->done, buf) != 0)
                return -1;
        return 0;
    }
    if (ext2_data_block(w->fs, b, buf, what) != 0)
        return -1;
    if (level == 0) {
        if (w->take(w->ctx, w->done, buf) != 0)
            return -1;
        w->done++;
        return 0;
    }
    for (i = 0; i < EXT2_BLOCK / 4 && w->done < w->count; i++)
        if (ext2_take_pointer(w, rd32(buf + i * 4), level - 1, what) != 0)
            return -1;
    return 0;
}

static int ext2_walk_blocks(struct ext2 *fs, const unsigned char *inode,
                            unsigned long count, const char *what,
                            int (*take)(void *, unsigned long,
                                        const unsigned char *),
                            void *ctx)
{
    struct ext2_blocks w;
    int i;

    w.fs = fs;
    w.count = count;
    w.done = 0;
    w.take = take;
    w.ctx = ctx;
    for (i = 0; i < 12 && w.done < w.count; i++)
        if (ext2_take_pointer(&w, rd32(inode + 40 + i * 4), 0, what) != 0)
            return -1;
    for (i = 0; i < 3 && w.done < w.count; i++)
        if (ext2_take_pointer(&w, rd32(inode + 40 + (12 + i) * 4), i + 1,
                              what) != 0)
            return -1;
    return 0;
}

struct ext2_copy {
    unsigned char *out;
    uint64_t len;
};

static int ext2_copy_block(void *ctx, unsigned long index,
                           const unsigned char *block)
{
    struct ext2_copy *c = ctx;
    uint64_t at = (uint64_t) index * EXT2_BLOCK;
    size_t n = c->len - at < EXT2_BLOCK ? (size_t) (c->len - at) : EXT2_BLOCK;

    memcpy(c->out + at, block, n);
    return 0;
}

static void ext2_push(struct image *im, const char *path, unsigned long mode,
                      unsigned long uid, unsigned long gid, uint64_t size,
                      unsigned long ino, unsigned char *data, size_t len)
{
    struct node *n;

    im->nodes = realloc(im->nodes,
                        (size_t) (im->n_nodes + 1) * sizeof(*im->nodes));
    if (im->nodes == NULL)
        exit(2);
    n = &im->nodes[im->n_nodes++];
    memset(n, 0, sizeof(*n));
    n->type = NODE_RESERVED;
    n->id = ino;
    n->mode = mode;
    n->uid = uid;
    n->gid = gid;
    n->size = size;
    n->data_size = size;
    n->path = xstrdup(path);
    n->data = data;
    n->data_len = len;
}

struct ext2_dir {
    struct ext2 *fs;
    struct image *im;
    const char *path;
    unsigned long ino, parent;
    int failed;
};

static int ext2_dir_block(void *ctx, unsigned long index,
                          const unsigned char *block);

/* One node reached at `path` through `ino`: recorded, and a directory
   walked. */
static int ext2_reach(struct ext2 *fs, struct image *im, const char *path,
                      unsigned long ino, unsigned long parent, unsigned ft)
{
    unsigned char inode[EXT2_INODE];
    unsigned long mode, uid, gid, type;
    uint64_t size;

    if (ext2_inode(fs, ino, inode) != 0)
        return -1;
    mode = rd16(inode + 0);
    uid = rd16(inode + 2) | (unsigned long) rd16(inode + 120) << 16;
    gid = rd16(inode + 24) | (unsigned long) rd16(inode + 122) << 16;
    size = rd32(inode + 4);
    type = mode & 0170000;
    if (rd16(inode + 26) == 0)
        return ext2_fail(fs, "%s: inode %lu has no links yet a name", path,
                         ino);
    if (type == 0100000)
        size |= (uint64_t) rd32(inode + 108) << 32;
    /* The entry's type must be the inode's: filetype is in the profile so
       that a reader may trust it, and one that lies is malformed. */
    if ((type == 0040000 && ft != 2) || (type == 0100000 && ft != 1) ||
        (type == 0120000 && ft != 7) || (type == 0020000 && ft != 3) ||
        (type == 0060000 && ft != 4) || (type == 0010000 && ft != 5) ||
        (type == 0140000 && ft != 6))
        return ext2_fail(fs, "%s: entry type %u does not match inode %lu's "
                         "mode %#lo", path, ft, ino, mode);
    if (type == 0040000) {
        struct ext2_dir d;

        if (fs->seen_dir[ino])
            return ext2_fail(fs, "%s: directory inode %lu reached twice",
                             path, ino);
        fs->seen_dir[ino] = 1;
        if (size == 0 || size % EXT2_BLOCK != 0)
            return ext2_fail(fs, "%s: directory size %llu is not whole "
                             "blocks", path, (unsigned long long) size);
        if (strcmp(path, "/") != 0)
            ext2_push(im, path, mode, uid, gid, size, ino, NULL, 0);
        d.fs = fs;
        d.im = im;
        d.path = path;
        d.ino = ino;
        d.parent = parent;
        d.failed = 0;
        if (ext2_walk_blocks(fs, inode, (unsigned long) (size / EXT2_BLOCK),
                             path, ext2_dir_block, &d) != 0 || d.failed)
            return -1;
        return 0;
    }
    if (type == 0100000) {
        struct ext2_copy c;

        if (size > 64u * 1024 * 1024)
            return ext2_fail(fs, "%s: %llu bytes is larger than any staged "
                             "file", path, (unsigned long long) size);
        c.len = size;
        c.out = xmalloc((size_t) size + 1);
        if (ext2_walk_blocks(fs, inode,
                             (unsigned long) ((size + EXT2_BLOCK - 1) /
                                              EXT2_BLOCK),
                             path, ext2_copy_block, &c) != 0) {
            free(c.out);
            return -1;
        }
        fs->seen_file[ino] = 1;
        ext2_push(im, path, mode, uid, gid, size, ino, c.out, (size_t) size);
        return 0;
    }
    /* Anything else is a node the manifests never stage; it is listed so
       that the policy sees it, with no bytes. */
    ext2_push(im, path, mode, uid, gid, size, ino, NULL, 0);
    return 0;
}

static int ext2_dir_block(void *ctx, unsigned long index,
                          const unsigned char *block)
{
    struct ext2_dir *d = ctx;
    unsigned long at = 0;

    while (at < EXT2_BLOCK) {
        unsigned long ino = rd32(block + at);
        unsigned rec = rd16(block + at + 4);
        unsigned name_len = block[at + 6];
        unsigned ft = block[at + 7];
        char name[256];
        char child[1024];

        if (rec < 8 || rec % 4 != 0 || at + rec > EXT2_BLOCK ||
            name_len + 8 > rec) {
            d->failed = 1;
            return ext2_fail(d->fs, "%s: entry at block %lu offset %lu is "
                             "malformed (rec_len %u, name_len %u)", d->path,
                             index, at, rec, name_len);
        }
        if (ino != 0) {
            unsigned i;

            if (name_len == 0) {
                d->failed = 1;
                return ext2_fail(d->fs, "%s: an entry with no name", d->path);
            }
            for (i = 0; i < name_len; i++)
                if (block[at + 8 + i] == '/' || block[at + 8 + i] == '\0') {
                    d->failed = 1;
                    return ext2_fail(d->fs, "%s: an entry's name holds a "
                                     "slash or a nul", d->path);
                }
            memcpy(name, block + at + 8, name_len);
            name[name_len] = '\0';
            if (ft > EXT2_FT_MAX) {
                d->failed = 1;
                return ext2_fail(d->fs, "%s/%s: entry type %u", d->path,
                                 name, ft);
            }
            if (strcmp(name, ".") == 0) {
                if (ino != d->ino) {
                    d->failed = 1;
                    return ext2_fail(d->fs, "%s: . is inode %lu, not %lu",
                                     d->path, ino, d->ino);
                }
            } else if (strcmp(name, "..") == 0) {
                if (ino != d->parent) {
                    d->failed = 1;
                    return ext2_fail(d->fs, "%s: .. is inode %lu, not the "
                                     "parent %lu", d->path, ino, d->parent);
                }
            } else {
                if (snprintf(child, sizeof(child), "%s/%s",
                             strcmp(d->path, "/") == 0 ? "" : d->path,
                             name) >= (int) sizeof(child)) {
                    d->failed = 1;
                    return ext2_fail(d->fs, "%s/%s: path too long", d->path,
                                     name);
                }
                if (ext2_reach(d->fs, d->im, child, ino, d->ino, ft) != 0) {
                    d->failed = 1;
                    return -1;
                }
            }
        }
        at += rec;
    }
    return 0;
}

/* Read an ext2 image into the node table check_image judges. */
static int read_ext2(struct image *im, const char *path, long offset,
                     long size)
{
    struct ext2 fs;
    int rc;

    memset(im, 0, sizeof(*im));
    if (ext2_open(&fs, path, offset, size) != 0) {
        img_fail(im, "%s", fs.err);
        ext2_close(&fs);
        return -1;
    }
    rc = ext2_reach(&fs, im, "/", 2, 2, 2);
    if (rc != 0)
        img_fail(im, "%s", fs.err);
    ext2_close(&fs);
    return rc;
}

static void sysroot_checks(const struct allow *allow, const struct image *im,
                           const char *name, struct list *problems);

/* Inventory a staged image against the allowlist entry `manifest` names.
   The root node is deliberately not inventoried: KRFFS_MODE_DIRTY is
   0004000, the setuid bit's own place, and the root is the one node that
   has it. Fills `summary` (empty on a walk failure). */
static void check_image(const char *image, const char *manifest,
                        long offset, long size, int ext2,
                        const struct allow *allow, struct list *problems,
                        char *summary, size_t summary_len)
{
    const char *iname = base_name(image);
    char *stem = manifest_stem(manifest);
    const struct entry *e = find_entry((struct allow *) allow, stem);
    struct image im;
    struct list sorted = { NULL, 0, 0 };
    const struct node *n;
    int i, j, reserved = 0;

    summary[0] = '\0';
    if (e == NULL) {
        addf(problems, "%s: no allowlist names this manifest; the "
             "inventory cannot say what %s may carry", base_name(manifest),
             iname);
        free(stem);
        return;
    }
    if (e->n_setid == 0 && e->sentinel_path == NULL) {
        addf(problems, "allowlist: %s declares no set-id entry and no "
             "sentinel; the image inventory would have nothing to find",
             stem);
        free(stem);
        return;
    }
    if (ext2 ? read_ext2(&im, image, offset, size) != 0
             : (read_region(&im, image, offset, size) != 0 ||
                walk_nodes(&im) != 0 || node_paths(&im) != 0)) {
        addf(problems, "%s: %s", iname, im.err);
        image_free(&im);
        free(stem);
        return;
    }

    n = path_find(&im, "/sbin/init");
    if (e->init == NULL) {
        if (n != NULL)
            addf(problems, "%s: has /sbin/init and the allowlist declares "
                 "none for this image; the kernel runs that as pid 1",
                 iname);
    } else if (n == NULL) {
        addf(problems, "%s: has no /sbin/init and the allowlist declares "
             "%s", iname, e->init);
    } else if ((n->mode & PERMBITS) != INIT_MODE || n->uid != 0 ||
               n->gid != 0) {
        addf(problems, "%s: /sbin/init is %s; the kernel execs it as root, "
             "so it must be %s", iname,
             fmt_entry(n->mode & PERMBITS, n->uid, n->gid),
             fmt_entry(INIT_MODE, 0, 0));
    }
    if (e->sysroot)
        sysroot_checks(allow, &im, iname, problems);

    /* Every set-id node, judged in path order. */
    for (i = 0; i < im.n_nodes; i++)
        if (im.nodes[i].path != NULL) {
            reserved++;
            if (im.nodes[i].mode & S_SETID)
                list_push(&sorted, im.nodes[i].path);
        }
    qsort(sorted.items, (size_t) sorted.n, sizeof(char *), cmp_str);
    for (i = 0; i < sorted.n; i++) {
        const struct node *sn = path_find(&im, sorted.items[i]);
        const struct setid_decl *decl = setid_find(e, sorted.items[i]);
        unsigned long mode = sn->mode & PERMBITS;

        if (decl == NULL) {
            addf(problems, "%s: %s is staged set-id (%s) and the %s "
                 "allowlist does not declare it", iname, sn->path,
                 fmt_entry(mode, sn->uid, sn->gid), stem);
            continue;
        }
        if (mode != decl->mode || sn->uid != decl->uid ||
            sn->gid != decl->gid)
            addf(problems, "%s: %s is staged %s but the %s allowlist "
                 "declares %s", iname, sn->path,
                 fmt_entry(mode, sn->uid, sn->gid), stem,
                 fmt_entry(decl->mode, decl->uid, decl->gid));
    }

    sorted.n = 0;
    for (i = 0; i < e->n_setid; i++)
        list_push(&sorted, e->setid[i].path);
    qsort(sorted.items, (size_t) sorted.n, sizeof(char *), cmp_str);
    for (i = 0; i < sorted.n; i++) {
        const struct setid_decl *decl = setid_find(e, sorted.items[i]);
        const struct node *sn = path_find(&im, decl->path);

        if (sn == NULL || !(sn->mode & S_SETID))
            addf(problems, "%s: %s is declared %s but the image has no "
                 "such set-id node", iname, decl->path,
                 fmt_entry(decl->mode, decl->uid, decl->gid));
    }

    check_ancestors(iname, e, image_dir_look, (void *) &im, problems);

    sorted.n = 0;
    for (i = 0; i < e->n_trusted; i++)
        list_push(&sorted, e->trusted[i].path);
    qsort(sorted.items, (size_t) sorted.n, sizeof(char *), cmp_str);
    for (i = 0; i < sorted.n; i++) {
        const struct trusted_decl *t = NULL;
        const struct node *tn;

        for (j = 0; j < e->n_trusted; j++)
            if (strcmp(e->trusted[j].path, sorted.items[i]) == 0)
                t = &e->trusted[j];
        tn = path_find(&im, t->path);
        if (tn == NULL)
            addf(problems, "%s: %s is not in the image; the allowlist "
                 "requires it to be present and unprivileged", iname,
                 t->path);
        else if (tn->mode & S_SETID)
            addf(problems, "%s: %s is staged set-id (%s); it must stay "
                 "trusted", iname, t->path,
                 fmt_entry(tn->mode & PERMBITS, tn->uid, tn->gid));
        else if (strcmp(t->verb, "text") == 0 &&
                 trusted_text_matches(&im, t->path, t->value) == 0)
            addf(problems, "%s: %s does not hold the text the allowlist "
                 "pins; a credential file's meaning is its content", iname,
                 t->path);
        else if ((tn->mode & PERMBITS) != t->mode || tn->uid != t->uid ||
                 tn->gid != t->gid)
            addf(problems, "%s: %s is declared %s but the image has %s; a "
                 "program root runs, and a file root reads to decide who "
                 "somebody is, are trust boundaries too", iname, t->path,
                 fmt_entry(t->mode, t->uid, t->gid),
                 fmt_entry(tn->mode & PERMBITS, tn->uid, tn->gid));
    }
    free(sorted.items);

    if (e->sentinel_path != NULL) {
        const struct node *sn = path_find(&im, e->sentinel_path);

        if (sn == NULL)
            addf(problems, "%s: the sentinel %s is not in the image; the "
                 "walk found %d node(s) and nothing to confirm it read "
                 "them", iname, e->sentinel_path, reserved);
        else if ((sn->mode & PERMBITS) != e->sentinel_mode ||
                 sn->uid != e->sentinel_uid || sn->gid != e->sentinel_gid)
            addf(problems, "%s: the sentinel %s is %s, not the declared "
                 "%s", iname, e->sentinel_path,
                 fmt_entry(sn->mode & PERMBITS, sn->uid, sn->gid),
                 fmt_entry(e->sentinel_mode, e->sentinel_uid,
                           e->sentinel_gid));
    }

    snprintf(summary, summary_len,
             "%s: %d node(s), %d reserved node(s), %d declared set-id",
             iname, im.n_nodes, reserved, e->n_setid);
    image_free(&im);
    free(stem);
}

/* ---- the sysroot inventory ---- */

#define SYSROOT_HEADER_SOURCE "vendor/musl/install/include"
#define SYSROOT_HEADER_ROOT "/include"

/* One staged or host header. */
struct hfile {
    char *path;               /* the guest path it answers to */
    unsigned long mode;       /* staged side only */
    unsigned char *data;      /* host side reads bytes; staged points in */
    const unsigned char *ref; /* staged side: bytes inside the region */
    size_t len;
};

struct hset {
    struct hfile *f;
    int n;
};

static void hset_add(struct hset *h, const char *path, unsigned long mode,
                     unsigned char *data, const unsigned char *ref,
                     size_t len)
{
    h->f = realloc(h->f, (size_t) (h->n + 1) * sizeof(*h->f));
    if (h->f == NULL)
        exit(2);
    h->f[h->n].path = xstrdup(path);
    h->f[h->n].mode = mode;
    h->f[h->n].data = data;
    h->f[h->n].ref = ref;
    h->f[h->n].len = len;
    h->n++;
}

static const struct hfile *hset_find(const struct hset *h, const char *path)
{
    int i;

    for (i = 0; i < h->n; i++)
        if (strcmp(h->f[i].path, path) == 0)
            return &h->f[i];
    return NULL;
}

static void hset_free(struct hset *h)
{
    int i;

    for (i = 0; i < h->n; i++) {
        free(h->f[i].path);
        free(h->f[i].data);
    }
    free(h->f);
    h->f = NULL;
    h->n = 0;
}

/* The host's header tree, keyed by the guest path each header will answer
   to. Returns nonzero when the tree is absent. Anything the walk cannot
   read is reported rather than passed over: a header left out of this set
   is a header the staged tree is never compared against, which is the
   comparison quietly proving less than it says. */
static int host_headers(const char *root, const char *guest_root,
                        struct hset *out, struct list *problems)
{
    DIR *dir = opendir(root);
    struct dirent *ent;

    if (dir == NULL)
        return 1;
    while ((ent = readdir(dir)) != NULL) {
        char host[1024], guest[1024];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        snprintf(host, sizeof(host), "%s/%s", root, ent->d_name);
        snprintf(guest, sizeof(guest), "%s/%s", guest_root, ent->d_name);
        if (lstat(host, &st) != 0) {
            addf(problems, "%s: %s; the staged tree cannot be compared "
                 "against it", host, strerror(errno));
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            addf(problems, "%s is a symbolic link; the staging walk copies "
                 "regular files, so what this compares against is not what "
                 "the guest would read", host);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (host_headers(host, guest, out, problems) != 0)
                addf(problems, "%s: cannot be opened; the headers under it "
                     "are not compared with anything", host);
        } else if (S_ISREG(st.st_mode)) {
            size_t len;
            unsigned char *data = read_file(host, &len);

            if (data == NULL)
                addf(problems, "%s: cannot be read; the staged tree cannot "
                     "be compared against it", host);
            else
                hset_add(out, guest, 0, data, data, len);
        }
    }
    closedir(dir);
    return 0;
}

/* The image's header tree: files and directories under /include. */
static void staged_headers(const struct image *im, struct hset *files,
                           struct hset *dirs)
{
    size_t rootlen = strlen(SYSROOT_HEADER_ROOT);
    int i;

    for (i = 0; i < im->n_nodes; i++) {
        const struct node *n = &im->nodes[i];

        if (n->path == NULL ||
            strncmp(n->path, SYSROOT_HEADER_ROOT "/", rootlen + 1) != 0)
            continue;
        if (n->mode & 040000)
            hset_add(dirs, n->path, n->mode & PERMBITS, NULL, NULL, 0);
        else {
            size_t len;
            const unsigned char *bytes = node_bytes(im, n, &len);

            hset_add(files, n->path, n->mode & PERMBITS, NULL, bytes, len);
        }
    }
}

/* "a, b, c and N more": a few of the paths, sorted, because a stage that
   went wrong tends to go wrong by hundreds. */
static void some(struct list *paths, char *out, size_t out_len)
{
    int i, shown = paths->n < 3 ? paths->n : 3;
    size_t used = 0;

    qsort(paths->items, (size_t) paths->n, sizeof(char *), cmp_str);
    out[0] = '\0';
    for (i = 0; i < shown; i++)
        appendf(out, out_len, &used, "%s%s", i ? ", " : "",
                paths->items[i]);
    if (paths->n > 3)
        appendf(out, out_len, &used, " and %d more", paths->n - 3);
}

/* Compare a staged header tree with the tree it was staged from: by path,
   by content and by mode, and all three are needed. A count comparison is
   a check a wrong image passes easily; a truncated header is the right
   path holding the wrong file; and a `tree 0600` line stages every byte
   correctly into a compiler no ordinary account can use. */
static void compare_headers(const struct hset *staged,
                            const struct hset *wanted,
                            const struct hset *dirs, const char *name,
                            struct list *problems)
{
    struct list bag = { NULL, 0, 0 };
    char sample[512];
    int i;

    for (i = 0; i < wanted->n; i++)
        if (hset_find(staged, wanted->f[i].path) == NULL)
            list_push(&bag, wanted->f[i].path);
    if (bag.n > 0) {
        some(&bag, sample, sizeof(sample));
        addf(problems, "%s: %d of the %d headers in %s are not staged "
             "(%s); the tree was staged short", name, bag.n, wanted->n,
             SYSROOT_HEADER_SOURCE, sample);
    }
    bag.n = 0;
    for (i = 0; i < staged->n; i++)
        if (hset_find(wanted, staged->f[i].path) == NULL)
            list_push(&bag, staged->f[i].path);
    if (bag.n > 0) {
        some(&bag, sample, sizeof(sample));
        addf(problems, "%s: %d files under %s are not in %s (%s); the "
             "tree has something it was not staged from", name, bag.n,
             SYSROOT_HEADER_ROOT, SYSROOT_HEADER_SOURCE, sample);
    }
    bag.n = 0;
    for (i = 0; i < staged->n; i++) {
        const struct hfile *w = hset_find(wanted, staged->f[i].path);

        if (w != NULL && (staged->f[i].len != w->len ||
                          memcmp(staged->f[i].ref, w->ref, w->len) != 0))
            list_push(&bag, staged->f[i].path);
    }
    if (bag.n > 0) {
        some(&bag, sample, sizeof(sample));
        addf(problems, "%s: %d staged headers do not hold what %s has "
             "(%s); a header that is there and wrong is worse than one "
             "that is missing", name, bag.n, SYSROOT_HEADER_SOURCE, sample);
    }
    bag.n = 0;
    for (i = 0; i < staged->n; i++)
        if (!(staged->f[i].mode & WORLD_READ))
            list_push(&bag, staged->f[i].path);
    if (bag.n > 0) {
        some(&bag, sample, sizeof(sample));
        addf(problems, "%s: %d staged headers cannot be read by an "
             "ordinary account (%s); the compiler runs as whoever asked "
             "for it, not as root", name, bag.n, sample);
    }
    bag.n = 0;
    for (i = 0; dirs != NULL && i < dirs->n; i++)
        if (!(dirs->f[i].mode & WORLD_WALK))
            list_push(&bag, dirs->f[i].path);
    if (bag.n > 0) {
        some(&bag, sample, sizeof(sample));
        addf(problems, "%s: %d directories of the header tree cannot be "
             "walked into by an ordinary account (%s)", name, bag.n,
             sample);
    }
    free(bag.items);
}

/* Every directory above a staged path must be walkable by an ordinary
   account: a header staged 0644 under a directory staged 0700 is a header
   only root can open. `find` answers a path with its node or NULL. */
typedef const struct node *(*path_look)(void *ctx, const char *path);

static void check_reachable(path_look find, void *ctx,
                            const struct list *want, const char *name,
                            struct list *problems)
{
    struct list seen = { NULL, 0, 0 };
    int i;

    for (i = 0; i < want->n; i++) {
        const char *guest = want->items[i];
        const char *p = guest + 1;
        char parent[512];

        if (find(ctx, guest) == NULL)
            continue;
        while ((p = strchr(p, '/')) != NULL) {
            size_t n = (size_t) (p - guest);
            const struct node *node;

            if (n >= sizeof(parent))
                break;
            memcpy(parent, guest, n);
            parent[n] = '\0';
            p++;
            if (list_has(&seen, parent))
                continue;
            list_push(&seen, xstrdup(parent));
            node = find(ctx, parent);
            if (node == NULL) {
                addf(problems, "%s: %s is not staged, and %s is under it; "
                     "the walk to it would stop there", name, parent,
                     guest);
                continue;
            }
            if (!(node->mode & 040000)) {
                addf(problems, "%s: %s is not a directory, and %s is "
                     "under it; the walk to it would stop there", name,
                     parent, guest);
                continue;
            }
            if (!(node->mode & WORLD_WALK))
                addf(problems, "%s: %s is staged %s, so an ordinary "
                     "account cannot walk into it and everything the "
                     "compiler looks for below it is out of reach", name,
                     parent, fmt_entry(node->mode & PERMBITS, node->uid,
                                       node->gid));
        }
    }
    list_free(&seen);
}

static const struct node *image_path_look(void *ctx, const char *path)
{
    return path_find((const struct image *) ctx, path);
}

/* The compiler's search paths, inventoried in the image whose entry says
   `sysroot`: tcc's paths were baked in at configure time, so an image
   missing one has a compiler that fails at the last step with a message
   about a file, not about the image. The named paths say the shape is
   right; the whole header tree is then compared with the tree it was
   staged from, because a header nobody sampled is exactly what a partial
   stage loses. */
static void sysroot_checks(const struct allow *allow, const struct image *im,
                           const char *name, struct list *problems)
{
    struct hset wanted = { NULL, 0 }, staged = { NULL, 0 };
    struct hset dirs = { NULL, 0 };
    int i;

    for (i = 0; i < allow->sysroot_paths.n; i++) {
        const char *path = allow->sysroot_paths.items[i];
        const struct node *n = path_find(im, path);
        unsigned long want = WORLD_READ;

        if (list_has(&allow->sysroot_programs, path))
            want |= WORLD_WALK;
        if (n == NULL)
            addf(problems, "%s: %s is not staged; the compiler was built "
                 "to look for it and would fail at the last step", name,
                 path);
        else if (n->mode & 040000)
            addf(problems, "%s: %s is a directory and the compiler wants "
                 "a file", name, path);
        else if (n->data_size == 0)
            addf(problems, "%s: %s is staged empty; a library or a "
                 "startup object that has nothing is a link that fails at "
                 "the last step", name, path);
        else if ((n->mode & want) != want)
            addf(problems, "%s: %s is staged %s; the compiler runs as "
                 "whoever asked for it, and an ordinary account could not "
                 "use this", name, path,
                 fmt_entry(n->mode & PERMBITS, n->uid, n->gid));
    }
    check_reachable(image_path_look, (void *) im, &allow->sysroot_paths,
                    name, problems);

    if (host_headers(SYSROOT_HEADER_SOURCE, SYSROOT_HEADER_ROOT,
                     &wanted, problems) != 0) {
        addf(problems, "%s: %s is not there, so the staged headers could "
             "not be compared with it", name, SYSROOT_HEADER_SOURCE);
    } else {
        staged_headers(im, &staged, &dirs);
        compare_headers(&staged, &wanted, &dirs, name, problems);
    }
    hset_free(&wanted);
    hset_free(&staged);
    hset_free(&dirs);
}

/* ---- the applet tables, the configs, and the two inits ---- */

#define APPLET_TABLE "build/vendor/busybox/applet_tables-main.h"
#define SUID_APPLET_TABLE "build/vendor/busybox/applet_tables-suid.h"
#define SUID_BUSYBOX_CONFIG "build/vendor/busybox/config-suid"
#define BUSYBOX_CONFIG "build/vendor/busybox/config-main"
#define INIT_BINARIES "build/utils/sbin"

/* Read a generated applet table into whole-token names. The names live in
   one NUL-delimited string, so the match is on whole tokens: `passwd`
   appears inside `passwd_main` and inside a help string. The whole
   initializer or nothing: a truncated file would read as a shorter applet
   set with the sentinel still present, so the declared count is required
   to agree with the names. Returns nonzero after reporting. */
static int applet_names(const char *path, struct list *names,
                        struct list *problems)
{
    size_t len;
    unsigned char *text = read_file(path, &len);
    const char *start, *end, *p, *decl;
    long declared = -1;
    struct stat st;

    if (stat(path, &st) != 0) {
        addf(problems, "%s: no applet table; build busybox before "
             "checking it", path);
        free(text);
        return 1;
    }
    if (text == NULL) {
        addf(problems, "%s: cannot read the applet table", path);
        return 1;
    }
    start = strstr((char *) text, "const char applet_names[]");
    if (start == NULL) {
        addf(problems, "%s: no applet_names table; this is not a busybox "
             "applet table, so the check proved nothing", path);
        free(text);
        return 1;
    }
    end = strchr(start, ';');
    if (end == NULL) {
        addf(problems, "%s: the applet_names table is unterminated, so it "
             "was read part way and proves nothing", path);
        free(text);
        return 1;
    }
    for (p = start; p < end; p++) {
        char chunk[4096];
        size_t n = 0;
        char *tok, *save = NULL;

        if (*p != '"')
            continue;
        p++;
        while (p < end && *p != '"' && n + 1 < sizeof(chunk)) {
            if (*p == '\\' && p + 1 < end) {
                chunk[n++] = *p++;
                chunk[n++] = *p++;
            } else {
                chunk[n++] = *p++;
            }
        }
        chunk[n] = '\0';
        /* Split on the literal two-character sequence backslash-zero. */
        for (tok = chunk; tok != NULL; tok = save) {
            char *at = strstr(tok, "\\0");
            char *t = tok;
            size_t tl;

            save = NULL;
            if (at != NULL) {
                *at = '\0';
                save = at + 2;
            }
            while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r')
                t++;
            tl = strlen(t);
            while (tl > 0 && (t[tl - 1] == ' ' || t[tl - 1] == '\t' ||
                              t[tl - 1] == '\n' || t[tl - 1] == '\r'))
                t[--tl] = '\0';
            if (*t != '\0')
                list_push(names, xstrdup(t));
        }
    }
    decl = strstr((char *) text, "#define NUM_APPLETS");
    if (decl != NULL) {
        decl += strlen("#define NUM_APPLETS");
        while (*decl == ' ' || *decl == '\t')
            decl++;
        if (*decl >= '0' && *decl <= '9') {
            errno = 0;
            declared = strtol(decl, NULL, 10);
            if (errno != 0)
                declared = -1;
        }
    }
    free(text);
    if (declared < 0) {
        addf(problems, "%s: the table declares no NUM_APPLETS, so its "
             "length cannot be checked", path);
        return 1;
    }
    if (declared != names->n) {
        addf(problems, "%s: the table names %d applets but declares %ld; "
             "it was truncated or edited", path, names->n, declared);
        return 1;
    }
    return 0;
}

static void check_applets(const struct allow *allow, const char *path,
                          struct list *problems)
{
    struct list names = { NULL, 0, 0 };
    int i;

    if (applet_names(path, &names, problems) != 0) {
        list_free(&names);
        return;
    }
    if (!list_has(&names, allow->applet_sentinel)) {
        addf(problems, "%s: the applet table does not name `%s`, so this "
             "check found nothing instead of proving an absence", path,
             allow->applet_sentinel);
        list_free(&names);
        return;
    }
    for (i = 0; i < allow->forbidden_applets.n; i++)
        if (list_has(&names, allow->forbidden_applets.items[i]))
            addf(problems, "%s: busybox has the `%s` applet; privilege "
                 "climbs through the declared set-id programs alone", path,
                 allow->forbidden_applets.items[i]);
    list_free(&names);
}

/* The set-id build's table: exactly the declared applets and nothing
   else. A blacklist here would pass a binary that had quietly grown a
   shell, which is the one thing a set-id executable must never contain. */
static void check_suid_applets(const struct allow *allow, const char *path,
                               struct list *problems)
{
    struct list names = { NULL, 0, 0 };
    struct list extra = { NULL, 0, 0 }, missing = { NULL, 0, 0 };
    char joined[256], declared[256];
    size_t used;
    int i;

    if (applet_names(path, &names, problems) != 0) {
        list_free(&names);
        return;
    }
    for (i = 0; i < names.n; i++)
        if (!list_has(&allow->suid_applets, names.items[i]) &&
            !list_has(&extra, names.items[i]))
            list_push(&extra, names.items[i]);
    for (i = 0; i < allow->suid_applets.n; i++)
        if (!list_has(&names, allow->suid_applets.items[i]))
            list_push(&missing, allow->suid_applets.items[i]);
    used = 0;
    declared[0] = '\0';
    for (i = 0; i < allow->suid_applets.n; i++)
        appendf(declared, sizeof(declared), &used, "%s`%s`", i ? ", " : "",
                allow->suid_applets.items[i]);
    if (extra.n > 0) {
        qsort(extra.items, (size_t) extra.n, sizeof(char *), cmp_str);
        used = 0;
        joined[0] = '\0';
        for (i = 0; i < extra.n; i++)
            appendf(joined, sizeof(joined), &used, "%s`%s`", i ? ", " : "",
                    extra.items[i]);
        addf(problems, "%s: the set-id busybox has %s beside the declared "
             "%s; every applet in that binary runs as root", path, joined,
             declared);
    }
    if (missing.n > 0) {
        qsort(missing.items, (size_t) missing.n, sizeof(char *), cmp_str);
        used = 0;
        joined[0] = '\0';
        for (i = 0; i < missing.n; i++)
            appendf(joined, sizeof(joined), &used, "%s`%s`", i ? ", " : "",
                    missing.items[i]);
        addf(problems, "%s: the set-id busybox is missing %s, so this "
             "check proved nothing about the binary that ships", path,
             joined);
    }
    free(extra.items);
    free(missing.items);
    list_free(&names);
}

/* The set-id build's resolved config: every named symbol must APPEAR, on
   or off, decided exactly once, so a config regenerated under other
   symbol names fails instead of passing by holding none of them. */
static void check_suid_config(const struct allow *allow, const char *path,
                              struct list *problems)
{
    size_t len, pos = 0;
    unsigned char *text = read_file(path, &len);
    struct stat st;
    int n_sym = allow->suid_on.n + allow->suid_off.n;
    int *state = xmalloc((size_t) n_sym * sizeof(int));
    int *seen = xmalloc((size_t) n_sym * sizeof(int));
    int i;

    if (stat(path, &st) != 0) {
        addf(problems, "%s: no set-id busybox config to check", path);
        free(text);
        free(state);
        free(seen);
        return;
    }
    if (text == NULL) {
        addf(problems, "%s: cannot read the config", path);
        free(state);
        free(seen);
        return;
    }
    for (i = 0; i < n_sym; i++) {
        state[i] = -1;
        seen[i] = 0;
    }
    while (pos < len) {
        size_t end = pos;
        char line[512];

        while (end < len && text[end] != '\n')
            end++;
        if (end - pos < sizeof(line)) {
            memcpy(line, text + pos, end - pos);
            line[end - pos] = '\0';
            for (i = 0; i < n_sym; i++) {
                const char *symbol = i < allow->suid_on.n
                    ? allow->suid_on.items[i]
                    : allow->suid_off.items[i - allow->suid_on.n];
                size_t sl = strlen(symbol);
                char unset[256];

                if (strncmp(line, symbol, sl) == 0 && line[sl] == '=') {
                    const char *value = line + sl + 1;
                    size_t vl = strlen(value);

                    while (vl > 0 && (value[vl - 1] == ' ' ||
                                      value[vl - 1] == '\t' ||
                                      value[vl - 1] == '\r'))
                        vl--;
                    if (!(vl == 1 && (value[0] == 'y' || value[0] == 'n')))
                        addf(problems, "%s: %s is set to `%.*s`, which "
                             "this guard does not read; the resolved "
                             "config writes y, n or a not-set comment",
                             path, symbol, (int) vl, value);
                    state[i] = value[0] == 'y';
                    seen[i]++;
                    continue;
                }
                snprintf(unset, sizeof(unset), "# %s is not set", symbol);
                if (strcmp(line, unset) == 0) {
                    state[i] = 0;
                    seen[i]++;
                }
            }
        } else {
            addf(problems, "%s: the line at byte %lu is %lu bytes long, "
                 "past what this guard reads; a symbol decided there "
                 "would not be seen", path, (unsigned long) pos,
                 (unsigned long) (end - pos));
        }
        pos = end + 1;
    }
    free(text);
    /* Reported in symbol-name order, the way a sorted dict reads. */
    {
        struct list order = { NULL, 0, 0 };
        int j;

        for (i = 0; i < n_sym; i++)
            if (seen[i] > 1)
                list_push(&order, i < allow->suid_on.n
                          ? allow->suid_on.items[i]
                          : allow->suid_off.items[i - allow->suid_on.n]);
        qsort(order.items, (size_t) order.n, sizeof(char *), cmp_str);
        for (j = 0; j < order.n; j++) {
            for (i = 0; i < n_sym; i++) {
                const char *symbol = i < allow->suid_on.n
                    ? allow->suid_on.items[i]
                    : allow->suid_off.items[i - allow->suid_on.n];

                if (strcmp(symbol, order.items[j]) == 0)
                    addf(problems, "%s: %s is decided %d times; a "
                         "resolved config decides it once", path, symbol,
                         seen[i]);
            }
        }
        free(order.items);
    }
    for (i = 0; i < allow->suid_on.n; i++) {
        if (state[i] == -1)
            addf(problems, "%s: %s does not appear at all, so this check "
                 "proved nothing about it", path, allow->suid_on.items[i]);
        else if (state[i] == 0)
            addf(problems, "%s: %s is off; the set-id binary is then not "
                 "the one this guard describes", path,
                 allow->suid_on.items[i]);
    }
    for (i = 0; i < allow->suid_off.n; i++) {
        int k = allow->suid_on.n + i;

        if (state[k] == -1)
            addf(problems, "%s: %s does not appear at all, so this check "
                 "proved nothing about it", path,
                 allow->suid_off.items[i]);
        else if (state[k] == 1)
            addf(problems, "%s: %s is on; it gives the set-id binary a "
                 "capability its three applet names do not mention", path,
                 allow->suid_off.items[i]);
    }
    free(state);
    free(seen);
}

/* The two init disciplines, told apart by the byte strings that name a
   shell. A strict init must hold neither; the shell-naming one must hold
   both, which is the sentinel proving the scan read the artifact. */
static void check_inits(const struct allow *allow, const char *directory,
                        struct list *problems)
{
    struct list names = { NULL, 0, 0 };
    int i, j;

    for (i = 0; i < allow->init_strict.n; i++)
        list_push(&names, allow->init_strict.items[i]);
    for (i = 0; i < allow->init_shellful.n; i++)
        list_push(&names, allow->init_shellful.items[i]);
    qsort(names.items, (size_t) names.n, sizeof(char *), cmp_str);
    for (i = 0; i < names.n; i++) {
        char path[512];
        size_t len;
        unsigned char *image;
        int shellful = list_has(&allow->init_shellful, names.items[i]);

        snprintf(path, sizeof(path), "%s/%s", directory, names.items[i]);
        image = read_file(path, &len);
        if (image == NULL) {
            addf(problems, "%s: cannot read the init artifact", path);
            continue;
        }
        if (len == 0) {
            addf(problems, "%s: the init artifact is empty, so this check "
                 "proved nothing", path);
            free(image);
            continue;
        }
        for (j = 0; j < allow->shell_strings.n; j++) {
            const char *want = allow->shell_strings.items[j];
            size_t wl = strlen(want), at;
            int held = 0;

            for (at = 0; wl > 0 && at + wl <= len; at++)
                if (memcmp(image + at, want, wl) == 0) {
                    held = 1;
                    break;
                }
            if (shellful && !held)
                addf(problems, "%s: does not carry the byte string %s, so "
                     "this check proved nothing; the artifact may not be "
                     "the one the manifests stage", path, want);
            else if (!shellful && held)
                addf(problems, "%s: has the byte string %s; the strict "
                     "init must hold no path it could fall back to, since "
                     "a fall-through on an exec failure hands the console "
                     "an unauthenticated root shell", path, want);
        }
        free(image);
    }
    free(names.items);
}

/* ---- reporting and the entry point ---- */

static int report(const struct list *problems)
{
    int i;

    fprintf(stderr, "check-setid: FAIL\n");
    for (i = 0; i < problems->n; i++)
        fprintf(stderr, "  - %s\n", problems->items[i]);
    return 1;
}

static int selftest(struct allow *allow);

static char *allow_path_beside(const char *argv0)
{
    static char path[1024];
    char dir[1024];
    const char *slash = strrchr(argv0, '/');

    if (slash != NULL) {
        snprintf(dir, sizeof(dir), "%.*s", (int) (slash - argv0), argv0);
        snprintf(path, sizeof(path),
                 "%s/../../../tools/inspect/setid.allow", dir);
        if (access(path, R_OK) == 0)
            return path;
    }
    snprintf(path, sizeof(path), "tools/inspect/setid.allow");
    return path;
}

int main(int argc, char **argv)
{
    struct allow *allow;
    struct list problems = { NULL, 0, 0 };
    const char *image = NULL, *manifest = NULL, *applets = NULL;
    long offset = 0, size = 0;
    int i, do_selftest = 0, do_applets = 0, do_free = 0, do_nodes = 0;
    int ext2 = 0;
    struct list manifests = { NULL, 0, 0 };

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selftest") == 0)
            do_selftest = 1;
        else if (strcmp(argv[i], "--applets") == 0) {
            do_applets = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                applets = argv[++i];
        } else if (strcmp(argv[i], "--free") == 0)
            do_free = 1;
        else if (strcmp(argv[i], "--nodes") == 0)
            do_nodes = 1;
        else if (strcmp(argv[i], "--image") == 0 && i + 1 < argc)
            image = argv[++i];
        else if (strcmp(argv[i], "--ext2") == 0 && i + 1 < argc) {
            image = argv[++i];
            ext2 = 1;
        }
        else if (strcmp(argv[i], "--manifest") == 0 && i + 1 < argc)
            manifest = argv[++i];
        else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) {
            if (parse_num(argv[++i], &offset) != 0 || offset < 0) {
                fprintf(stderr, "check-setid: --offset %s is not a byte "
                        "offset\n", argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (parse_num(argv[++i], &size) != 0 || size <= 0) {
                fprintf(stderr, "check-setid: --size %s is not a positive "
                        "byte count; leave it out for the rest of the "
                        "file\n", argv[i]);
                return 2;
            }
        }
        else if (argv[i][0] != '-')
            list_push(&manifests, argv[i]);
        else {
            fprintf(stderr, "check-setid: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    allow = read_allow(allow_path_beside(argv[0]));

    if (do_selftest)
        return selftest(allow);

    if (do_free) {
        struct image im;
        unsigned long long free_bytes = 0;

        if (image == NULL) {
            fprintf(stderr, "check-setid: --free needs --image\n");
            return 2;
        }
        if (read_region(&im, image, offset, size) != 0 ||
            walk_nodes(&im) != 0) {
            fprintf(stderr, "check-setid: %s: %s\n", base_name(image),
                    im.err);
            return 1;
        }
        for (i = 0; i < im.n_nodes; i++)
            if (im.nodes[i].type == NODE_FREE)
                free_bytes += im.nodes[i].size;
        printf("%llu\n", free_bytes);
        return 0;
    }

    if (do_nodes) {
        /* One line per root or reserved node, in ring order: the type,
           the rebuilt path, and the three timestamps. The reproducibility
           check reads staging order and the fixed staged instant off
           this. */
        struct image im;

        if (image == NULL) {
            fprintf(stderr, "check-setid: --nodes needs --image or --ext2\n");
            return 2;
        }
        if (ext2) {
            /* One line per node reached: path, mode with its type, owner,
               group, size and inode, in the walk's order. */
            if (read_ext2(&im, image, offset, size) != 0) {
                fprintf(stderr, "check-setid: %s: %s\n", base_name(image),
                        im.err);
                return 1;
            }
            for (i = 0; i < im.n_nodes; i++) {
                const struct node *nd = &im.nodes[i];

                printf("%s %lo %lu %lu %llu %llu\n", nd->path, nd->mode,
                       nd->uid, nd->gid, (unsigned long long) nd->size,
                       (unsigned long long) nd->id);
            }
            image_free(&im);
            return 0;
        }
        if (read_region(&im, image, offset, size) != 0 ||
            walk_nodes(&im) != 0 || node_paths(&im) != 0) {
            fprintf(stderr, "check-setid: %s: %s\n", base_name(image),
                    im.err);
            return 1;
        }
        for (i = 0; i < im.n_nodes; i++) {
            const struct node *nd = &im.nodes[i];
            const unsigned char *p = im.region + nd->offset + 277;
            int w;

            if (nd->type != NODE_ROOT && nd->type != NODE_RESERVED)
                continue;
            printf("%s %s", nd->type == NODE_ROOT ? "root" : "reserved",
                   nd->path != NULL ? nd->path : "/");
            for (w = 0; w < 3; w++)
                printf(" %lld %lld",
                       (long long) rd64(p + 16 * w),
                       (long long) rd64(p + 16 * w + 8));
            printf("\n");
        }
        return 0;
    }

    if (do_applets) {
        check_applets(allow, applets ? applets : APPLET_TABLE, &problems);
        check_suid_applets(allow, SUID_APPLET_TABLE, &problems);
        check_suid_config(allow, SUID_BUSYBOX_CONFIG, &problems);
        if (access(BUSYBOX_CONFIG, R_OK) != 0)
            addf(&problems, "%s: the main build staged no resolved config, "
                 "so what its applet table was built from cannot be read",
                 BUSYBOX_CONFIG);
        check_inits(allow, INIT_BINARIES, &problems);
        if (problems.n > 0)
            return report(&problems);
        printf("check-setid: OK (%s: no forbidden applet or feature; the "
               "set-id build has only login, passwd, su; the strict init "
               "names no shell)\n", applets ? applets : APPLET_TABLE);
        return 0;
    }

    if (image != NULL) {
        char summary[256];

        if (manifest == NULL) {
            fprintf(stderr, "check-setid: --image needs the --manifest "
                    "that staged it\n");
            return 2;
        }
        if (manifests.n > 0) {
            fprintf(stderr, "check-setid: --image inventories one image "
                    "against one --manifest; it does not sweep the "
                    "manifests named beside it\n");
            return 2;
        }
        if (offset != 0 && size == 0) {
            fprintf(stderr, "check-setid: --offset needs --size; a file "
                    "system inside a partition ends where the partition "
                    "does, not where the file does\n");
            return 2;
        }
        check_image(image, manifest, offset, size, ext2, allow, &problems,
                    summary, sizeof(summary));
        if (problems.n > 0)
            return report(&problems);
        printf("check-setid: OK (%s, every set-id node declared)\n",
               summary);
        return 0;
    }
    if (manifest != NULL) {
        fprintf(stderr, "check-setid: --manifest names the manifest of an "
                "--image; the sweep takes its manifests as arguments\n");
        return 2;
    }

    validate_table(allow, &problems);
    if (manifests.n == 0) {
        /* Sweep every manifest in krffs/manifests, and require every
           allowlist entry to have one there. */
        struct list stems = { NULL, 0, 0 };
        DIR *dir = opendir("krffs/manifests");
        struct dirent *ent;

        if (dir == NULL) {
            fprintf(stderr, "check-setid: no manifests in "
                    "krffs/manifests\n");
            return 2;
        }
        while ((ent = readdir(dir)) != NULL) {
            size_t n = strlen(ent->d_name);

            if (n > 9 && strcmp(ent->d_name + n - 9, ".manifest") == 0) {
                char full[512];

                snprintf(full, sizeof(full), "krffs/manifests/%s",
                         ent->d_name);
                list_push(&manifests, xstrdup(full));
            }
        }
        closedir(dir);
        if (manifests.n == 0) {
            fprintf(stderr, "check-setid: no manifests in "
                    "krffs/manifests\n");
            return 2;
        }
        qsort(manifests.items, (size_t) manifests.n, sizeof(char *),
              cmp_str);
        for (i = 0; i < allow->n; i++)
            list_push(&stems, allow->entries[i].stem);
        qsort(stems.items, (size_t) stems.n, sizeof(char *), cmp_str);
        for (i = 0; i < stems.n; i++) {
            char want[512];

            snprintf(want, sizeof(want), "krffs/manifests/%s.manifest",
                     stems.items[i]);
            if (!list_has(&manifests, want))
                addf(&problems, "allowlist: %s names %s.manifest, which "
                     "is not in krffs/manifests", stems.items[i],
                     stems.items[i]);
        }
        free(stems.items);
    }
    for (i = 0; i < manifests.n; i++)
        check_manifest(manifests.items[i], allow, &problems);
    if (problems.n > 0)
        return report(&problems);
    {
        int entries = 0;

        for (i = 0; i < manifests.n; i++) {
            char *stem = manifest_stem(manifests.items[i]);
            const struct entry *e = find_entry(allow, stem);

            if (e != NULL)
                entries += e->n_setid;
            free(stem);
        }
        printf("check-setid: OK (%d manifests swept, %d declared set-id "
               "entries staged)\n", manifests.n, entries);
    }
    return 0;
}

/* ---- the selftest: every half over fixtures ----
 *
 * The sweep fixtures' declared lines come from the allow file itself, so a
 * mutation is the one thing that varies; what they prove is that the
 * parser and the rules read a manifest the way stageimage does, not that
 * the declarations have any particular value. The image fixtures are built
 * out of the same node layout the walk decodes: a root, four reserved
 * nodes and a trailing free node that closes the ring, so truncating to a
 * multiple of the header size is exactly the cut that reads clean without
 * the ring-closure test.
 */

static int st_bad, st_ran;

/* Judge one case's findings. */
static void st_case(const char *name, struct list *got, int want_ok,
                    const char *fragment)
{
    int i, found = 0;

    st_ran++;
    if (want_ok && got->n > 0) {
        printf("  FAIL: %s: expected a clean run, got: %s\n", name,
               got->items[0]);
        st_bad++;
    } else if (!want_ok && got->n == 0) {
        printf("  FAIL: %s: expected a finding, the run was clean\n", name);
        st_bad++;
    } else if (!want_ok) {
        for (i = 0; i < got->n; i++)
            if (strstr(got->items[i], fragment) != NULL)
                found = 1;
        if (!found) {
            printf("  FAIL: %s: no finding named '%s'; got: %s\n", name,
                   fragment, got->items[0]);
            st_bad++;
        }
    }
    list_free(got);
}

/* The directive lines a manifest needs to satisfy one entry's trusted
   list, in sorted-path order, built from the allow file so the fixtures
   cannot drift from it. */
static void trusted_lines(const struct entry *e, char *out, size_t out_len)
{
    struct list paths = { NULL, 0, 0 };
    size_t used = 0;
    int i, j;

    for (i = 0; i < e->n_trusted; i++)
        list_push(&paths, e->trusted[i].path);
    qsort(paths.items, (size_t) paths.n, sizeof(char *), cmp_str);
    out[0] = '\0';
    for (i = 0; i < paths.n; i++)
        for (j = 0; j < e->n_trusted; j++)
            if (strcmp(e->trusted[j].path, paths.items[i]) == 0 &&
                !appendf(out, out_len, &used, "%s %s %s %s\n",
                         e->trusted[j].verb,
                         fmt_entry(e->trusted[j].mode, e->trusted[j].uid,
                                   e->trusted[j].gid),
                         e->trusted[j].path, e->trusted[j].value)) {
                fprintf(stderr, "setidcheck: selftest: the trusted lines "
                        "of %s do not fit in %lu bytes; the fixtures this "
                        "builds would test less than the allow file "
                        "declares\n", e->stem, (unsigned long) out_len);
                exit(2);
            }
    free(paths.items);
}

/* The /sbin/init line a manifest with this stem must carry, or "". */
static void init_line_of(const struct allow *allow, const char *stem,
                         char *out, size_t out_len)
{
    const struct entry *e = find_entry((struct allow *) allow, stem);

    out[0] = '\0';
    if (e != NULL && e->init != NULL)
        snprintf(out, out_len,
                 "file 0755 /sbin/init build/utils/sbin/%s\n", e->init);
}

static char st_tmp[256];

/* The selftest's fixtures are flat files in one directory, so removing
   them and the directory is the whole of the cleanup. */
static void st_clean(void)
{
    DIR *dir = opendir(st_tmp);
    struct dirent *ent;

    if (dir != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            char path[512];

            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0)
                continue;
            snprintf(path, sizeof(path), "%s/%s", st_tmp, ent->d_name);
            unlink(path);
        }
        closedir(dir);
    }
    rmdir(st_tmp);
}

/* Write a fixture manifest and sweep it. */
static void st_manifest(const struct allow *allow, const char *name,
                        const char *stem, const char *text, int want_ok,
                        const char *fragment, int with_init)
{
    char path[512], init[256];
    FILE *f;
    struct list got = { NULL, 0, 0 };

    snprintf(path, sizeof(path), "%s/%s.manifest", st_tmp, stem);
    f = fopen(path, "wb");
    if (f == NULL)
        exit(2);
    if (with_init) {
        init_line_of(allow, stem, init, sizeof(init));
        fputs(init, f);
    }
    fputs(text, f);
    fclose(f);
    check_manifest(path, allow, &got);
    st_case(name, &got, want_ok, fragment);
}

/* A helper for building fixture text: `sed`-style single replacement. */
static char *replaced(const char *text, const char *old, const char *new_)
{
    const char *at = strstr(text, old);
    size_t before, ol = strlen(old), nl = strlen(new_);
    char *out;

    if (at == NULL) {
        printf("  FAIL: a fixture's own line is not in the base text: "
               "%s\n", old);
        st_bad++;
        return xstrdup(text);
    }
    before = (size_t) (at - text);
    out = xmalloc(strlen(text) - ol + nl + 1);
    memcpy(out, text, before);
    memcpy(out + before, new_, nl);
    strcpy(out + before + nl, at + ol);
    return out;
}

static char *concat(const char *a, const char *b)
{
    char *out = xmalloc(strlen(a) + strlen(b) + 1);

    strcpy(out, a);
    strcat(out, b);
    return out;
}

/* ---- the image fixtures ---- */

#define IMG_TOTAL 4096

struct fix_node {
    unsigned type;
    uint64_t id;
    const char *name;
    unsigned long mode, uid, gid;
    uint64_t parent;
};

static void wr64(unsigned char *p, uint64_t v)
{
    int i;

    for (i = 0; i < 8; i++)
        p[i] = (unsigned char) (v >> 8 * i);
}
static void wr32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char) v;
    p[1] = (unsigned char) (v >> 8);
    p[2] = (unsigned char) (v >> 16);
    p[3] = (unsigned char) (v >> 24);
}
static void wr16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char) v;
    p[1] = (unsigned char) (v >> 8);
}

/* Pack nodes into a region, closed by a free node of the remainder. */
static void build_region(const struct fix_node *nodes, int n,
                         unsigned char *region, size_t total)
{
    uint64_t previous = total - (size_t) n * NODE_SIZE;
    size_t at = 0;
    int i;

    memset(region, 0, total);
    for (i = 0; i <= n; i++) {
        const struct fix_node free_node = { NODE_FREE, 0, "", 0, 0, 0, 0 };
        const struct fix_node *f = i < n ? &nodes[i] : &free_node;
        uint64_t size = i < n ? NODE_SIZE
                              : total - (size_t) n * NODE_SIZE;
        unsigned char *p = region + at;

        wr16(p + NODE_MAGIC_OFF, KRFFS_MAGIC);
        p[NODE_TYPE_OFF] = (unsigned char) f->type;
        wr64(p + NODE_ID_OFF, f->id);
        memcpy(p + NODE_NAME_OFF, f->name, strlen(f->name));
        wr16(p + NODE_MODE_OFF, (unsigned) f->mode);
        wr32(p + NODE_UID_OFF, f->uid);
        wr32(p + NODE_GID_OFF, f->gid);
        wr64(p + NODE_SIZEF_OFF, size);
        wr64(p + NODE_PREV_OFF, previous);
        wr64(p + NODE_PARENT_OFF, f->parent);
        previous = size;
        at += (size_t) size;
    }
}

static const struct fix_node IMG_NODES[5] = {
    { NODE_ROOT, 5381, "", 0755, 0, 0, 0 },
    { NODE_RESERVED, 11, "bin", 040755, 0, 0, 0 },
    { NODE_RESERVED, 12, "su", 04755, 0, 0, 11 },
    { NODE_RESERVED, 13, "busybox", 0755, 0, 0, 11 },
    { NODE_RESERVED, 14, "hello.txt", 0644, 0, 0, 0 },
};

/* The fixture region with one node optionally replaced and more added. */
static void fixture(int index, const struct fix_node *node,
                    const struct fix_node *extra, int n_extra,
                    unsigned char *region)
{
    struct fix_node nodes[8];
    int i, n = 5;

    memcpy(nodes, IMG_NODES, sizeof(IMG_NODES));
    if (index >= 0)
        nodes[index] = *node;
    for (i = 0; i < n_extra; i++)
        nodes[n++] = extra[i];
    build_region(nodes, n, region, IMG_TOTAL);
}

/* One image case: write the region, inventory it against the fixture
   allowlist. */
static void st_image(const struct allow *img_allow, const char *name,
                     const char *stem, const unsigned char *region,
                     size_t region_len, long cut, int want_ok,
                     const char *fragment)
{
    char image[512], manifest[512], summary[256];
    FILE *f;
    struct list got = { NULL, 0, 0 };

    snprintf(image, sizeof(image), "%s/%s.krffs", st_tmp, stem);
    snprintf(manifest, sizeof(manifest), "%s/%s.manifest", st_tmp, stem);
    f = fopen(image, "wb");
    if (f == NULL)
        exit(2);
    fwrite(region, 1, cut > 0 ? (size_t) cut : region_len, f);
    fclose(f);
    check_image(image, manifest, 0, 0, 0, img_allow, &got, summary,
                sizeof(summary));
    st_case(name, &got, want_ok, fragment);
}

/* ---- the header-comparison fixtures, in memory ---- */

struct fake_fs {
    struct node nodes[8];
    int n;
};

static const struct node *fake_look(void *ctx, const char *path)
{
    struct fake_fs *fs = ctx;
    int i;

    for (i = 0; i < fs->n; i++)
        if (strcmp(fs->nodes[i].path, path) == 0)
            return &fs->nodes[i];
    return NULL;
}

static void fake_add(struct fake_fs *fs, const char *path,
                     unsigned long mode)
{
    memset(&fs->nodes[fs->n], 0, sizeof(struct node));
    fs->nodes[fs->n].path = (char *) path;
    fs->nodes[fs->n].mode = mode;
    fs->n++;
}


/* ---- the ext2 fixtures: built by the host's mke2fs and debugfs ----

   The reader is judged against the tools that wrote the volume, so the
   fixtures are what mkext2.sh makes and not a second writer: a tree of
   the inventory fixture's nodes, formatted on the profile, its owners set
   by debugfs, and then copies with one thing changed, by debugfs where
   debugfs can and by a byte where it cannot. */

static int st_run(const char *command)
{
    int rc = system(command);

    return rc == 0 ? 0 : -1;
}

static void st_rm_tree(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *ent;

    if (dir != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            char child[1024];
            struct stat st;

            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0)
                continue;
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
                st_rm_tree(child);
            else
                unlink(child);
        }
        closedir(dir);
    }
    rmdir(path);
}

static int st_write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL)
        return -1;
    fputs(text, f);
    return fclose(f);
}

/* The clean fixture volume at `out`, or -1 when the host tools are not
   there or refuse. */
static int st_ext2_fixture(const char *out)
{
    char tree[512], cmd[4096], script[512];

    snprintf(tree, sizeof(tree), "%s/ext2tree", st_tmp);
    st_rm_tree(tree);
    if (mkdir(tree, 0755) != 0)
        return -1;
    snprintf(cmd, sizeof(cmd), "%s/bin", tree);
    if (mkdir(cmd, 0755) != 0)
        return -1;
    snprintf(cmd, sizeof(cmd), "%s/bin/su", tree);
    if (st_write_file(cmd, "su\n") != 0)
        return -1;
    snprintf(cmd, sizeof(cmd), "%s/bin/busybox", tree);
    if (st_write_file(cmd, "busybox\n") != 0)
        return -1;
    snprintf(cmd, sizeof(cmd), "%s/hello.txt", tree);
    if (st_write_file(cmd, "hello\n") != 0)
        return -1;
    snprintf(script, sizeof(script), "%s/owners.debugfs", st_tmp);
    if (st_write_file(script,
                      "set_inode_field /bin mode 040755\n"
                      "set_inode_field /bin uid 0\n"
                      "set_inode_field /bin gid 0\n"
                      "set_inode_field /bin/su mode 0104755\n"
                      "set_inode_field /bin/su uid 0\n"
                      "set_inode_field /bin/su gid 0\n"
                      "set_inode_field /bin/busybox mode 0100755\n"
                      "set_inode_field /bin/busybox uid 0\n"
                      "set_inode_field /bin/busybox gid 0\n"
                      "set_inode_field /hello.txt mode 0100644\n"
                      "set_inode_field /hello.txt uid 0\n"
                      "set_inode_field /hello.txt gid 0\n") != 0)
        return -1;
    unlink(out);
    snprintf(cmd, sizeof(cmd),
             ". tools/setup/e2fsprogs_path.sh 2>/dev/null || true; "
             ": > '%s' && E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 "
             "-I 128 -O sparse_super,large_file,filetype,^dir_index,"
             "^resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,"
             "^metadata_csum,^huge_file,^dir_nlink,^extra_isize -d '%s' "
             "'%s' 8M >/dev/null 2>&1 && debugfs -w -f '%s' '%s' "
             ">/dev/null 2>&1", out, tree, out, script, out);
    if (st_run(cmd) != 0)
        return -1;
    st_rm_tree(tree);
    unlink(script);
    return 0;
}

/* A copy of the clean fixture with a debugfs command applied. */
static int st_ext2_copy(const char *clean, const char *out,
                        const char *command)
{
    char cmd[4096];

    snprintf(cmd, sizeof(cmd),
             ". tools/setup/e2fsprogs_path.sh 2>/dev/null || true; "
             "cp '%s' '%s' && debugfs -w -R '%s' '%s' >/dev/null 2>&1",
             clean, out, command, out);
    return st_run(cmd);
}

/* A copy of the clean fixture with `len` bytes written at `at`. */
static int st_ext2_poke(const char *clean, const char *out, long at,
                        const unsigned char *bytes, size_t len)
{
    FILE *in = fopen(clean, "rb");
    FILE *o = fopen(out, "wb");
    unsigned char buf[65536];
    size_t n;

    if (in == NULL || o == NULL)
        return -1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, o) != n)
            return -1;
    fclose(in);
    if (fseek(o, at, SEEK_SET) != 0 || fwrite(bytes, 1, len, o) != len)
        return -1;
    return fclose(o);
}

/* Where an inode's 128 bytes are in the clean fixture, and where a
   directory's block is, read through the reader's own placement. */
static long st_ext2_inode_at(const char *image, unsigned long ino)
{
    struct ext2 fs;
    unsigned long g, index, table;
    long at;

    if (ext2_open(&fs, image, 0, 0) != 0) {
        ext2_close(&fs);
        return -1;
    }
    g = (ino - 1) / fs.ipg;
    index = (ino - 1) % fs.ipg;
    table = rd32(fs.gd + g * 32 + 8);
    at = (long) (table * EXT2_BLOCK + index * EXT2_INODE);
    ext2_close(&fs);
    return at;
}

static unsigned long st_ext2_inode_of(const char *image, const char *path)
{
    struct image im;
    const struct node *n;
    unsigned long ino = 0;

    if (read_ext2(&im, image, 0, 0) == 0 && (n = path_find(&im, path)) != NULL)
        ino = (unsigned long) n->id;
    image_free(&im);
    return ino;
}

static void st_ext2_case(const struct allow *img_allow, const char *name,
                         const char *image, int want_ok,
                         const char *fragment)
{
    char manifest[512], summary[256];
    struct list got = { NULL, 0, 0 };

    snprintf(manifest, sizeof(manifest), "%s/img-fixture.manifest", st_tmp);
    check_image(image, manifest, 0, 0, 1, img_allow, &got, summary,
                sizeof(summary));
    st_case(name, &got, want_ok, fragment);
}

static void st_ext2_rows(const struct allow *img_allow)
{
    char clean[512], mut[512];
    struct list got = { NULL, 0, 0 };

    snprintf(clean, sizeof(clean), "%s/img-fixture.ext2", st_tmp);
    snprintf(mut, sizeof(mut), "%s/img-mutant.ext2", st_tmp);
    if (st_ext2_fixture(clean) != 0) {
        addf(&got, "the ext2 fixture could not be built; mke2fs and "
             "debugfs are required host tools");
        st_case("the ext2 fixture is built by the host's e2fsprogs", &got,
                1, "");
        return;
    }

    /* The reader against debugfs: every node, as debugfs lists it. */
    {
        struct image im;
        const struct node *n;
        int ok = read_ext2(&im, clean, 0, 0) == 0;

        if (ok) {
            n = path_find(&im, "/bin/su");
            ok = n != NULL && n->mode == 0104755 && n->uid == 0 &&
                 n->gid == 0 && n->data_len == 3 &&
                 memcmp(n->data, "su\n", 3) == 0;
        }
        if (ok) {
            n = path_find(&im, "/bin");
            ok = n != NULL && n->mode == 040755;
        }
        if (ok) {
            n = path_find(&im, "/hello.txt");
            ok = n != NULL && n->mode == 0100644 && n->data_len == 6;
        }
        if (ok)
            ok = im.n_nodes == 5 && path_find(&im, "/lost+found") != NULL;
        if (!ok)
            addf(&got, "%s", im.err[0] ? im.err : "a node is not as debugfs "
                 "wrote it");
        st_case("the reader lists the fixture as debugfs wrote it", &got, 1,
                "");
        image_free(&im);
    }
    st_ext2_case(img_allow, "a clean ext2 fixture", clean, 1, "");

    /* The policy rows, each a debugfs edit of a copy. */
    if (st_ext2_copy(clean, mut, "set_inode_field /bin/su uid 5") == 0)
        st_ext2_case(img_allow, "ext2: a set-id file at the wrong owner",
                     mut, 0, "allowlist declares 04755:0:0");
    if (st_ext2_copy(clean, mut, "set_inode_field /bin/su gid 7") == 0)
        st_ext2_case(img_allow, "ext2: a set-id file at the wrong group",
                     mut, 0, "allowlist declares 04755:0:0");
    if (st_ext2_copy(clean, mut, "set_inode_field /bin/su mode 0106755") == 0)
        st_ext2_case(img_allow, "ext2: a set-id bit added", mut, 0,
                     "allowlist declares 04755:0:0");
    if (st_ext2_copy(clean, mut, "set_inode_field /bin/su mode 0100755") == 0)
        st_ext2_case(img_allow, "ext2: a set-id bit removed", mut, 0,
                     "no such set-id node");
    if (st_ext2_copy(clean, mut,
                     "set_inode_field /bin/busybox mode 0104755") == 0)
        st_ext2_case(img_allow, "ext2: a set-id inode the manifest does "
                     "not name", mut, 0, "does not declare it");
    if (st_ext2_copy(clean, mut, "link /bin/su /bin/su2") == 0)
        st_ext2_case(img_allow, "ext2: a hard link to a set-id file", mut,
                     0, "/bin/su2 is staged set-id");

    /* The refusals, each a byte the tools would not write. */
    {
        unsigned char b[4];
        long at;
        unsigned long ino;

        /* An incompatible feature outside the profile: extents. */
        b[0] = 0x42; b[1] = 0; b[2] = 0; b[3] = 0;
        if (st_ext2_poke(clean, mut, 1024 + 96, b, 4) == 0)
            st_ext2_case(img_allow, "ext2: a feature outside the profile",
                         mut, 0, "the profile is 0/0x2/0x3");
        /* A block past the volume in the set-id file's first pointer. */
        ino = st_ext2_inode_of(clean, "/bin/su");
        at = ino ? st_ext2_inode_at(clean, ino) : -1;
        b[0] = 0xff; b[1] = 0xff; b[2] = 0xff; b[3] = 0x7f;
        if (at > 0 && st_ext2_poke(clean, mut, at + 40, b, 4) == 0)
            st_ext2_case(img_allow, "ext2: a block outside the volume", mut,
                         0, "outside the");
        /* A pointer into the inode table. */
        at = ino ? st_ext2_inode_at(clean, ino) : -1;
        if (at > 0) {
            unsigned long table = (unsigned long) at / EXT2_BLOCK;

            b[0] = (unsigned char) table;
            b[1] = (unsigned char) (table >> 8);
            b[2] = (unsigned char) (table >> 16);
            b[3] = (unsigned char) (table >> 24);
            if (st_ext2_poke(clean, mut, at + 40, b, 4) == 0)
                st_ext2_case(img_allow, "ext2: a block inside a table", mut,
                             0, "which is a bitmap, a table or a superblock");
        }
        /* A malformed entry: the root directory's first entry with a
           rec_len of 6. */
        ino = 2;
        at = st_ext2_inode_at(clean, ino);
        if (at > 0) {
            unsigned char inode[EXT2_INODE];
            FILE *f = fopen(clean, "rb");
            unsigned long block = 0;

            if (f != NULL && fseek(f, at, SEEK_SET) == 0 &&
                fread(inode, 1, sizeof(inode), f) == sizeof(inode))
                block = rd32(inode + 40);
            if (f != NULL)
                fclose(f);
            b[0] = 6; b[1] = 0;
            if (block != 0 &&
                st_ext2_poke(clean, mut, (long) (block * EXT2_BLOCK + 4), b,
                             2) == 0)
                st_ext2_case(img_allow, "ext2: a malformed directory entry",
                             mut, 0, "is malformed");
            /* An inode the reader cannot place: the third entry's number,
               the first after . and .., past the count. */
            b[0] = 0xff; b[1] = 0xff; b[2] = 0xff; b[3] = 0x0f;
            if (block != 0 &&
                st_ext2_poke(clean, mut, (long) (block * EXT2_BLOCK + 24), b,
                             4) == 0)
                st_ext2_case(img_allow, "ext2: an inode that cannot be placed",
                             mut, 0, "cannot be placed");
        }
    }
    unlink(clean);
    unlink(mut);
}

static int selftest(struct allow *allow)
{
    char base[8192], tl[4096];
    char *t;
    const struct entry *os_real = find_entry(allow, "os-real");
    const struct entry *os_simple = find_entry(allow, "os-simple");
    const struct entry *test_e = find_entry(allow, "test");
    struct allow img_allow;
    struct entry *fe;
    unsigned char region[IMG_TOTAL];
    struct list got = { NULL, 0, 0 };
    char os_simple_min[6144], test_min[2048], longest[MAX_LINE + 8];
    size_t used;
    int i;

    st_bad = 0;
    st_ran = 0;
    snprintf(st_tmp, sizeof(st_tmp), "%s/setidcheck-selftest.XXXXXX",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (mkdtemp(st_tmp) == NULL) {
        fprintf(stderr, "check-setid: cannot make a temporary directory\n");
        return 2;
    }
    if (os_real == NULL || os_simple == NULL || test_e == NULL) {
        printf("  FAIL: the allow file lacks os-real, os-simple or test\n");
        return 1;
    }
    validate_table(allow, &got);
    for (i = 0; i < got.n; i++) {
        printf("  FAIL: the allowlist itself: %s\n", got.items[i]);
        st_bad++;
    }
    list_free(&got);

    /* The minimal accepted manifests, generated from the allow file. */
    trusted_lines(os_real, tl, sizeof(tl));
    snprintf(base, sizeof(base),
             "dir 0755 /bin\n" "dir 0755 /sbin\n" "dir 0755 /etc\n"
             "dir 0755 /root\n" "dir 0700 /root/.ssh\n" "dir 0755 /src\n"
             "dir 0755 /src/utils\n" "dir 0755 /src/rach2\n"
             "dir 0755 /src/rach2/sys\n" "dir 0755 /src/ish\n"
             "%s"
             "file 04755:0:0 /bin/su build/vendor/busybox/busybox-suid\n"
             "file 04755:0:0 /bin/passwd "
             "build/vendor/busybox/busybox-suid\n"
             "file 04755:0:0 /bin/login "
             "build/vendor/busybox/busybox-suid\n", tl);
    trusted_lines(os_simple, tl, sizeof(tl));
    snprintf(os_simple_min, sizeof(os_simple_min),
             "dir 0755 /bin\n" "dir 0755 /sbin\n" "dir 0755 /etc\n"
             "%s"
             "file 04755:0:0 /bin/su build/utils/sbin/su\n"
             "file 04755:0:0 /bin/passwd build/utils/sbin/passwd\n", tl);
    snprintf(test_min, sizeof(test_min),
             "dir 0755 /musl\n"
             "file 04755:0:0 /suidhelper.suid build/tests/suidhelper\n"
             "file 04755:5:0 /suidhelper.u5 build/tests/suidhelper\n"
             "file 02755:0:5 /suidhelper.sgid build/tests/suidhelper\n"
             "file 04644:0:0 /suidhelper.nox build/tests/suidhelper\n"
             "file 04755:0:0 /sudo build/utils/sbin/sudo\n"
             "file 04755:0:0 /su build/utils/sbin/su\n"
             "file 04755:0:0 /passwd build/utils/sbin/passwd\n"
             "file 04755:0:0 /musl/suidmusl.suid "
             "build/tests/musl/suidmusl\n");
    used = (size_t) snprintf(longest, sizeof(longest),
                             "file 0755 /pad build/utils/cat");
    while (used < MAX_LINE)
        longest[used++] = ' ';
    longest[used] = '\0';

#define CASE(name, stem, text, ok, frag) \
    st_manifest(allow, name, stem, text, ok, frag, 1)
#define CASE_F(name, stem, textexpr, ok, frag) \
    do { t = (textexpr); CASE(name, stem, t, ok, frag); free(t); } while (0)

    CASE("a declared set-id file", "os-real", base, 1, "");
    {
        char *a = replaced(base, "file 04755:0:0 /bin/su",
                           "optional-file 04755:0:0 /bin/su");
        char *b = replaced(a, "file 04755:0:0 /bin/passwd",
                           "optional-file 04755:0:0 /bin/passwd");
        char *c = replaced(b, "file 04755:0:0 /bin/login",
                           "optional-file 04755:0:0 /bin/login");

        CASE("a declared set-id optional-file", "os-real", c, 1, "");
        free(a);
        free(b);
        free(c);
    }
    CASE("a declared set-id path staged as text", "os-real",
         "file 0755 /bin/busybox build/vendor/busybox/busybox\n"
         "text 04755:0:0 /bin/su placeholder\n", 0, "inline");
    CASE("a declared set-id path from the wrong host file", "os-real",
         "file 0755 /bin/busybox build/vendor/busybox/busybox\n"
         "file 04755:0:0 /bin/su build/vendor/busybox/busybox\n", 0,
         "declares it staged from");
    CASE_F("an undeclared set-id optional-file", "os-real",
           concat(base, "optional-file 04755:0:0 /bin/doom build/doom\n"),
           0, "does not declare it");
    CASE_F("an undeclared set-id text", "os-real",
           concat(base, "text 02755:0:0 /etc/motd hello\\n"), 0,
           "does not declare it");
    CASE("the test image's declared entries", "test", test_min, 1, "");
    CASE_F("a set-id tree", "os-simple",
           concat(os_simple_min,
                  "text 0644 /hello.txt hello, world\\n\n"
                  "tree 04644 /include vendor/musl/install/include\n"), 0,
           "refused");
    CASE_F("a plain tree", "os-simple",
           concat(os_simple_min,
                  "text 0644 /hello.txt hello, world\\n\n"
                  "tree 0644 /include vendor/musl/install/include\n"), 1,
           "");
    CASE_F("a 'tree' with no host dir", "os-simple",
           concat(os_simple_min,
                  "text 0644 /hello.txt hello, world\\n\n"
                  "tree 0644 /include\n"), 0, "needs a host");
    CASE_F("a set-id exec-dir", "os-simple",
           concat(os_simple_min,
                  "text 0644 /hello.txt hello, world\\n\n"
                  "exec-dir 04755 / build/utils\n"), 0, "refused");
    CASE_F("a plain exec-dir", "os-simple",
           concat(os_simple_min,
                  "text 0644 /hello.txt hello, world\\n\n"
                  "exec-dir 0755 / build/utils\n"), 1, "");
    CASE_F("a setgid dir", "os-real", concat(base, "dir 02755 /home\n"),
           0, "no setgid-directory");
    CASE_F("a set-id dir on the root", "os-real",
           concat(base, "dir 06755 /\n"), 0, "no setgid-directory");
    CASE_F("a sticky dir", "os-real", concat(base, "dir 1777 /tmp\n"), 1,
           "");
    CASE_F("an undeclared set-id path", "os-real",
           concat(base,
                  "file 04755:0:0 /bin/evil build/utils/sbin/evil\n"), 0,
           "/bin/evil");
    CASE("a declared path no manifest stages", "os-real",
         "file 0755 /bin/busybox build/vendor/busybox/busybox\n", 0,
         "no directive stages it");
    CASE("a declared path at the wrong mode", "os-real",
         "file 0755 /bin/busybox build/vendor/busybox/busybox\n"
         "file 02755:0:0 /bin/su build/utils/sbin/su\n", 0,
         "allowlist declares 04755:0:0");
    CASE("a declared path at the wrong owner", "os-real",
         "file 0755 /bin/busybox build/vendor/busybox/busybox\n"
         "file 04755:5:0 /bin/su build/utils/sbin/su\n", 0,
         "allowlist declares 04755:0:0");
    CASE_F("the malformed mode 0999", "os-real",
           concat(base, "file 0999 /bin/x build/x\n"), 0,
           "malformed mode");
    CASE_F("the malformed mode 08", "os-real",
           concat(base, "file 08 /bin/x build/x\n"), 0, "malformed mode");
    CASE_F("the malformed mode 04755:0", "os-real",
           concat(base, "file 04755:0 /bin/x build/x\n"), 0,
           "malformed mode");
    CASE_F("an unknown verb", "os-real",
           concat(base, "symlink 0777 /bin/sh /bin/busybox\n"), 0,
           "unknown directive");
    CASE_F("a directive short of its path", "os-real",
           concat(base, "dir 0755\n"), 0, "needs a mode and a path");
    CASE("a 'file' with no host path", "os-real",
         "file 0755 /bin/busybox build/vendor/busybox/busybox\n"
         "file 04755:0:0 /bin/su\n", 0, "'file' needs a host path");
    CASE_F("an 'optional-file' with no host path", "os-real",
           concat(base, "optional-file 0755 /bin/doom\n"), 0,
           "'optional-file' needs a host path");
    CASE_F("an 'exec-dir' with no host dir", "os-real",
           concat(base, "exec-dir 0755 /bin\n"), 0,
           "'exec-dir' needs a host dir");
    CASE_F("a 'text' with no payload", "os-simple",
           concat(os_simple_min, "text 0644 /hello.txt\n"), 1, "");
    CASE_F("a full-line comment", "os-real",
           concat("# file 04755:0:0 /bin/evil build/evil\n"
                  "   # an indented comment line\n", base), 1, "");
    CASE_F("a trailing hash is not a comment", "os-real",
           concat(base,
                  "file 04755:0:0 /bin/evil build/evil # note\n"), 0,
           "/bin/evil");
    {
        /* Tab-separated fields: the first three spaces of each line. */
        char *tabs = xstrdup(base);
        char *line = tabs;

        while (line != NULL && *line != '\0') {
            int spaces = 0;
            char *p;

            for (p = line; *p != '\0' && *p != '\n'; p++)
                if (*p == ' ' && spaces < 3) {
                    *p = '\t';
                    spaces++;
                }
            line = *p ? p + 1 : NULL;
        }
        CASE("tab-separated fields", "os-real", tabs, 1, "");
        free(tabs);
    }
    CASE("a carriage return inside a path", "os-real",
         "file 0755 /bin/busybox build/vendor/busybox/busybox\n"
         "file 04755:0:0 /bin/su\r#x build/utils/sbin/su\n", 0,
         "does not declare it");
    {
        /* A NUL byte cannot ride in a C string, so this fixture is
           written byte by byte. */
        char path[512], init[256];
        static const char nul_line[] = "file 0755 /bin/x@y build/x\n";
        char *withnul = concat(base, nul_line);
        FILE *f;

        *strchr(withnul + strlen(base), '@') = '\0';
        snprintf(path, sizeof(path), "%s/os-real.manifest", st_tmp);
        f = fopen(path, "wb");
        if (f == NULL)
            exit(2);
        init_line_of(allow, "os-real", init, sizeof(init));
        fputs(init, f);
        fwrite(withnul, 1, strlen(base) + sizeof(nul_line) - 1, f);
        fclose(f);
        free(withnul);
        check_manifest(path, allow, &got);
        st_case("a NUL byte inside a path", &got, 0, "NUL byte");
    }
    CASE_F("a non-ASCII byte in a directive", "os-real",
           concat(base, "file 0755 /bin/caf\xe9 build/x\n"), 0,
           "non-ASCII byte");
    CASE_F("a non-ASCII byte in a comment", "os-real",
           concat("# caf\xe9\n", base), 1, "");
    {
        char *a = replaced(os_simple_min, "file 0755:0:0 /bin/login",
                           "file 0777:0:0 /bin/login");

        CASE_F("an unprivileged program root runs, staged world-writable",
               "os-simple", concat(a, "text 0644 /hello.txt hi\n"), 0,
               "trust boundaries too");
        free(a);
        a = replaced(os_simple_min, "build/utils/sbin/login",
                     "build/elsewhere/login");
        CASE_F("an unprivileged program staged from somewhere else",
               "os-simple", concat(a, "text 0644 /hello.txt hi\n"), 0,
               "declared staged from");
        free(a);
    }
    CASE_F("a host file restaged as its own path written inline",
           "os-real",
           replaced(base, "file 0600:0:0 /etc/shadow krffs/shadow",
                    "text 0600:0:0 /etc/shadow krffs/shadow"), 0,
           "are not the same thing");
    CASE_F("an inline file restaged as a host copy", "os-real",
           replaced(base, "text 0644:0:0 /etc/shells /bin/sh\\n",
                    "file 0644:0:0 /etc/shells /bin/sh\\n"), 0,
           "are not the same thing");
    CASE_F("an account given root's numbers in the passwd text", "os-real",
           replaced(base, "test:x:1000:1000:test", "test:x:0:0:test"), 0,
           "does not hold the text the allowlist pins");
    CASE_F("a shell nobody declared added to the shells file", "os-real",
           replaced(base, "/etc/shells /bin/sh\\n",
                    "/etc/shells /bin/sh\\n/tmp/mine\\n"), 0,
           "does not hold the text the allowlist pins");
    CASE_F("the shadow file staged readable by anyone", "os-real",
           replaced(base, "file 0600:0:0 /etc/shadow",
                    "file 0644:0:0 /etc/shadow"), 0,
           "trust boundaries too");
    CASE_F("root's authorized_keys staged from somewhere else", "os-real",
           replaced(base, "krffs/authorized_keys",
                    "krffs/test_authorized_keys"), 0,
           "declared staged from");
    CASE_F("the shell root's login execs staged writable", "os-real",
           replaced(base, "file 0755:0:0 /bin/sh",
                    "file 0777:0:0 /bin/sh"), 0, "trust boundaries too");
    CASE_F("a safe directory declared again, unsafely", "os-simple",
           concat(os_simple_min,
                  "dir 0777 /bin\ntext 0644 /hello.txt hi\n"), 0,
           "may write is a way to replace");
    {
        char *tail = concat(os_simple_min, "text 0644 /hello.txt hi\n");

        CASE_F("an unsafe directory declared again, safely", "os-simple",
               concat("dir 0777 /bin\n", tail), 1, "");
        free(tail);
    }
    {
        char *a = replaced(os_simple_min, "dir 0755 /bin\n",
                           "exec-dir 0755 /bin build/utils\n");

        CASE_F("an exec-dir standing in for the directory declaration",
               "os-simple", concat(a, "text 0644 /hello.txt hi\n"), 0,
               "no 'dir' line stages it");
        free(a);
    }
    {
        char *a = concat(os_simple_min, "text 0644 /hello.txt hi\n");
        char *b = concat(a, longest);
        char *c;

        CASE_F("the longest line stageimage accepts", "os-simple",
               concat(b, "\n"), 1, "");
        c = concat(b, "x\n");
        CASE("a line one byte too long", "os-simple", c, 0,
             "longer than");
        free(c);
        c = concat(b, "\r\n");
        CASE("the longest line ended with CRLF", "os-simple", c, 0,
             "longer than");
        free(c);
        free(a);
        free(b);
    }
    CASE("an unknown manifest name", "login-test",
         "text 0644 /hello.txt hello\\n", 0, "no allowlist names");
    CASE("busybox staged setuid", "os-real",
         "file 04755:0:0 /bin/busybox build/vendor/busybox/busybox\n"
         "file 04755:0:0 /bin/su build/utils/sbin/su\n", 0,
         "must stay unprivileged");
    CASE("busybox not staged at all", "os-real",
         "file 04755:0:0 /bin/su build/utils/sbin/su\n", 0,
         "is not staged");
    CASE_F("a path staged twice", "os-real",
           concat(base, "file 0755 /bin/su build/utils/sbin/su\n"), 0,
           "staged more than once");
    CASE("su without its setuid bit", "os-real",
         "file 0755 /bin/busybox build/vendor/busybox/busybox\n"
         "file 0755 /bin/su build/utils/sbin/su\n", 0,
         "declared privilege is gone");
    CASE("su owned 5:0", "os-real",
         "file 0755 /bin/busybox build/vendor/busybox/busybox\n"
         "file 04755:5:0 /bin/su build/utils/sbin/su\n", 0,
         "allowlist declares 04755:0:0");

    /* The init check's own cases: the line is exactly what varies. */
#define ICASE(name, stem, text, ok, frag) \
    st_manifest(allow, name, stem, text, ok, frag, 0)
    ICASE("the declared init", "os-simple",
          t = concat(os_simple_min,
                     "file 0755 /sbin/init build/utils/sbin/init-login\n"),
          1, "");
    free(t);
    ICASE("the other init", "os-simple",
          t = concat(os_simple_min,
                     "file 0755 /sbin/init build/utils/sbin/init-shell\n"),
          0, "build/utils/sbin/init-login");
    free(t);
    ICASE("the declared init from somewhere else", "os-simple",
          t = concat(os_simple_min,
                     "file 0755 /sbin/init build/elsewhere/init-login\n"),
          0, "build/utils/sbin/init-login");
    free(t);
    ICASE("an optional init", "os-simple",
          t = concat(os_simple_min, "optional-file 0755 /sbin/init "
                     "build/utils/sbin/init-login\n"), 0, "must be 'file'");
    free(t);
    ICASE("the declared init staged writable by anyone", "os-simple",
          t = concat(os_simple_min,
                     "file 0777 /sbin/init build/utils/sbin/init-login\n"),
          0, "must be 0755:0:0");
    free(t);
    ICASE("the declared init staged to another owner", "os-simple",
          t = concat(os_simple_min, "file 0755:1000:0 /sbin/init "
                     "build/utils/sbin/init-login\n"), 0,
          "must be 0755:0:0");
    free(t);
    ICASE("an inline init", "os-simple",
          t = concat(os_simple_min, "text 0755 /sbin/init placeholder\n"),
          0, "an inline payload");
    free(t);
    ICASE("no init at all", "os-simple",
          t = concat(os_simple_min,
                     "file 0755 /hello.txt build/utils/cat\n"), 0,
          "stages no /sbin/init");
    free(t);
    ICASE("an init where none is declared", "test",
          t = concat(test_min,
                     "file 0755 /sbin/init build/utils/sbin/init-login\n"),
          0, "declares none for this image");
    free(t);

    /* The applet table's ways of lying. */
    {
        static const struct {
            const char *name;
            const char *applets[4];
            int n, short_count, want_ok;
            const char *fragment;
        } acases[] = {
            { "a table with none of the forbidden applets",
              { "cat", "ls", "sh", "traceroute" }, 4, 0, 1, "" },
            { "a table that has dmesg, which would answer before "
              "/bin/dmesg", { "cat", "dmesg", "ls" }, 3, 0, 0,
              "`dmesg` applet" },
            { "a table that has passwd, which wants the set-id bit",
              { "cat", "ls", "passwd" }, 3, 0, 0, "`passwd` applet" },
            { "a table with no sentinel, so an absence proves nothing",
              { "ls", "sh" }, 2, 0, 0,
              "found nothing instead of proving an absence" },
            { "a table whose declared count is not what it names",
              { "cat", "ls" }, 2, 1, 0, "truncated or edited" },
        };

        for (i = 0; i < 5; i++) {
            char path[512];
            FILE *f;
            int j;

            snprintf(path, sizeof(path), "%s/applets%d.h", st_tmp, i);
            f = fopen(path, "wb");
            if (f == NULL)
                exit(2);
            fprintf(f, "#define NUM_APPLETS %d\n",
                    acases[i].n + acases[i].short_count);
            fprintf(f, "const char applet_names[] ALIGN1 = \"");
            for (j = 0; j < acases[i].n; j++)
                fprintf(f, "%s\\0", acases[i].applets[j]);
            fprintf(f, "\";\n");
            fclose(f);
            check_applets(allow, path, &got);
            st_case(acases[i].name, &got, acases[i].want_ok,
                    acases[i].fragment);
        }
    }

    /* The set-id config, mutated one setting at a time. */
    {
        size_t clen;
        unsigned char *config = read_file(SUID_BUSYBOX_CONFIG, &clen);

        if (config == NULL) {
            check_suid_config(allow, SUID_BUSYBOX_CONFIG, &got);
            st_case("the set-id config as it ships", &got, 0,
                    "no set-id busybox config");
        } else {
            static const struct {
                const char *name, *old, *new_, *fragment;
            } mut[] = {
                { "a symbol decided twice, the dangerous one first",
                  "# CONFIG_FEATURE_SUID_CONFIG is not set",
                  "CONFIG_FEATURE_SUID_CONFIG=y\n"
                  "# CONFIG_FEATURE_SUID_CONFIG is not set",
                  "decided 2 times" },
                { "a value this guard does not read",
                  "# CONFIG_FEATURE_SUID_CONFIG is not set",
                  "CONFIG_FEATURE_SUID_CONFIG=yes", "does not read" },
                { "the suid.conf parser compiled back in",
                  "# CONFIG_FEATURE_SUID_CONFIG is not set",
                  "CONFIG_FEATURE_SUID_CONFIG=y", "capability" },
                { "a shell smuggled into the set-id binary",
                  "# CONFIG_SH_IS_ASH is not set", "CONFIG_SH_IS_ASH=y",
                  "capability" },
                { "applets preferred over the programs on disk",
                  "# CONFIG_FEATURE_PREFER_APPLETS is not set",
                  "CONFIG_FEATURE_PREFER_APPLETS=y", "capability" },
                { "the set-id bit no longer honored",
                  "CONFIG_FEATURE_SUID=y",
                  "# CONFIG_FEATURE_SUID is not set", "is off" },
                { "a symbol the guard names gone from the config",
                  "CONFIG_SH_IS_NONE=y", "", "proved nothing" },
            };

            check_suid_config(allow, SUID_BUSYBOX_CONFIG, &got);
            st_case("the set-id config as it ships", &got, 1, "");
            for (i = 0; i < 7; i++) {
                char path[512];
                char *mutated = replaced((char *) config, mut[i].old,
                                         mut[i].new_);
                FILE *f;

                snprintf(path, sizeof(path), "%s/suid-%d.config", st_tmp,
                         i);
                f = fopen(path, "wb");
                if (f == NULL)
                    exit(2);
                fputs(mutated, f);
                fclose(f);
                free(mutated);
                check_suid_config(allow, path, &got);
                st_case(mut[i].name, &got, 0, mut[i].fragment);
            }
            free(config);
        }
    }

    /* The header comparison, over an in-memory pretend musl install. */
    {
        struct hset wanted = { NULL, 0 }, staged = { NULL, 0 };
        struct hset dirs = { NULL, 0 };
        static const char h1[] = "int puts(const char *);\n";
        static const char h2[] = "void *memcpy(void *, const void *);\n";
        static const char h3[] = "struct stat;\n";
        struct fake_fs fs;

        hset_add(&wanted, "/include/stdio.h", 0,
                 (unsigned char *) xstrdup(h1),
                 (const unsigned char *) h1, sizeof(h1) - 1);
        hset_add(&wanted, "/include/string.h", 0,
                 (unsigned char *) xstrdup(h2),
                 (const unsigned char *) h2, sizeof(h2) - 1);
        hset_add(&wanted, "/include/sys/stat.h", 0,
                 (unsigned char *) xstrdup(h3),
                 (const unsigned char *) h3, sizeof(h3) - 1);
        for (i = 0; i < wanted.n; i++)
            hset_add(&staged, wanted.f[i].path, 0644, NULL,
                     wanted.f[i].ref, wanted.f[i].len);
        hset_add(&dirs, "/include/sys", 0755, NULL, NULL, 0);

        compare_headers(&staged, &wanted, &dirs, "image", &got);
        st_case("the headers staged as they stand", &got, 1, "");

        /* A header dropped and an unrelated file put in beside it. */
        free(staged.f[2].path);
        staged.f[2].path = xstrdup("/include/sys/notaheader.h");
        compare_headers(&staged, &wanted, &dirs, "image", &got);
        st_case("a header dropped and an unrelated file put in beside "
                "it, so the count still agrees", &got, 0, "are not staged");
        free(staged.f[2].path);
        staged.f[2].path = xstrdup("/include/sys/stat.h");

        staged.f[0].len = 8;
        compare_headers(&staged, &wanted, &dirs, "image", &got);
        st_case("a staged header truncated at the right path", &got, 0,
                "do not hold what");
        staged.f[0].len = wanted.f[0].len;

        staged.f[0].mode = 0600;
        compare_headers(&staged, &wanted, &dirs, "image", &got);
        st_case("a header staged with every byte right and readable only "
                "by root", &got, 0, "cannot be read by an ordinary "
                "account");
        staged.f[0].mode = 0644;

        dirs.f[0].mode = 0700;
        compare_headers(&staged, &wanted, &dirs, "image", &got);
        st_case("the tree's own directory staged unwalkable", &got, 0,
                "cannot be walked into");
        dirs.f[0].mode = 0755;

        hset_free(&wanted);
        hset_free(&staged);
        hset_free(&dirs);

        /* Reachability over a hand-built path table. */
        fs.n = 0;
        fake_add(&fs, "/include", 040755);
        fake_add(&fs, "/include/sys", 040700);
        fake_add(&fs, "/include/sys/stat.h", 0644);
        {
            struct list want = { NULL, 0, 0 };

            list_push(&want, (char *) "/include/sys/stat.h");
            check_reachable(fake_look, &fs, &want, "image", &got);
            st_case("a header under a directory root alone can walk into",
                    &got, 0, "cannot walk into it");
            fs.n = 0;
            fake_add(&fs, "/include", 040755);
            fake_add(&fs, "/include/sys/stat.h", 0644);
            check_reachable(fake_look, &fs, &want, "image", &got);
            st_case("a header under a directory that is not staged at "
                    "all", &got, 0, "is not staged, and");
            fs.n = 0;
            fake_add(&fs, "/include", 040755);
            fake_add(&fs, "/include/sys", 0644);
            fake_add(&fs, "/include/sys/stat.h", 0644);
            check_reachable(fake_look, &fs, &want, "image", &got);
            st_case("a header under a parent that is a file", &got, 0,
                    "is not a directory");
            fs.n = 0;
            fake_add(&fs, "/include", 040755);
            fake_add(&fs, "/include/sys", 040755);
            fake_add(&fs, "/include/sys/stat.h", 0644);
            check_reachable(fake_look, &fs, &want, "image", &got);
            st_case("the header tree as it is staged", &got, 1, "");
            free(want.items);
        }
    }

    /* The allowlist's own rules, over throwaway tables. */
    {
        struct allow bad;

        memset(&bad, 0, sizeof(bad));
        fe = add_entry(&bad, "x");
        validate_table(&bad, &got);
        st_case("a table entry with nothing to find", &got, 0,
                "nothing to find");
        entry_add_setid(fe, "/x", 0755, 0, 0, "build/x");
        fe->sentinel_path = xstrdup("/y");
        validate_table(&bad, &got);
        st_case("a declared entry carrying no set-id bit", &got, 0,
                "has no set-id bit");
    }

    /* A manifest the table names but the tree lacks: the require-all rule
       of the no-argument sweep. */
    addf(&got, "allowlist: os-real names os-real.manifest, which is not "
         "in krffs/manifests");
    st_case("a manifest the table names but the tree lacks", &got, 0,
            "which is not in");

    /* The image inventory's fixtures. */
    memset(&img_allow, 0, sizeof(img_allow));
    fe = add_entry(&img_allow, "img-fixture");
    entry_add_setid(fe, "/bin/su", 04755, 0, 0, "build/utils/sbin/su");
    entry_add_trusted(fe, "/bin/busybox", 0755, 0, 0, "file",
                      "build/vendor/busybox/busybox");
    fe->sentinel_path = xstrdup("/hello.txt");
    fe->sentinel_mode = 0644;
    add_entry(&img_allow, "img-blind");

    {
        struct fix_node n;
        struct fix_node extra[2];

        fixture(-1, NULL, NULL, 0, region);
        st_image(&img_allow, "a clean fixture image", "img-fixture",
                 region, IMG_TOTAL, 0, 1, "");
        n = IMG_NODES[3];
        n.mode = 04755;
        fixture(3, &n, NULL, 0, region);
        st_image(&img_allow, "an undeclared set-id node", "img-fixture",
                 region, IMG_TOTAL, 0, 0, "does not declare it");
        n = IMG_NODES[2];
        n.mode = 0755;
        fixture(2, &n, NULL, 0, region);
        st_image(&img_allow, "a declared node stripped of its bit",
                 "img-fixture", region, IMG_TOTAL, 0, 0,
                 "no such set-id node");
        n = IMG_NODES[2];
        n.uid = 5;
        fixture(2, &n, NULL, 0, region);
        st_image(&img_allow, "a declared node at the wrong owner",
                 "img-fixture", region, IMG_TOTAL, 0, 0,
                 "allowlist declares 04755:0:0");
        n = IMG_NODES[4];
        n.mode = 0600;
        fixture(4, &n, NULL, 0, region);
        st_image(&img_allow, "the sentinel at the wrong mode",
                 "img-fixture", region, IMG_TOTAL, 0, 0,
                 "not the declared");
        n = IMG_NODES[4];
        n.name = "other.txt";
        fixture(4, &n, NULL, 0, region);
        st_image(&img_allow, "the sentinel missing", "img-fixture",
                 region, IMG_TOTAL, 0, 0, "the sentinel /hello.txt");
        n = IMG_NODES[3];
        n.name = "other";
        fixture(3, &n, NULL, 0, region);
        st_image(&img_allow, "an unprivileged file missing", "img-fixture",
                 region, IMG_TOTAL, 0, 0, "present and unprivileged");
        st_ext2_rows(&img_allow);
        n = IMG_NODES[3];
        n.mode = 0777;
        fixture(3, &n, NULL, 0, region);
        st_image(&img_allow, "an unprivileged file at a mode nobody "
                 "declared", "img-fixture", region, IMG_TOTAL, 0, 0,
                 "trust boundaries too");
        extra[0].type = NODE_RESERVED;
        extra[0].id = 15;
        extra[0].name = "sbin";
        extra[0].mode = 040755;
        extra[0].uid = 0;
        extra[0].gid = 0;
        extra[0].parent = 0;
        extra[1] = extra[0];
        extra[1].id = 16;
        extra[1].name = "init";
        extra[1].mode = 0755;
        extra[1].parent = 15;
        fixture(-1, NULL, extra, 2, region);
        st_image(&img_allow, "an init smuggled in where the allowlist "
                 "declares none", "img-fixture", region, IMG_TOTAL, 0, 0,
                 "the kernel runs that as pid 1");
        n = IMG_NODES[1];
        n.mode = 040777;
        fixture(1, &n, NULL, 0, region);
        st_image(&img_allow, "the directory holding them writable by "
                 "anyone", "img-fixture", region, IMG_TOTAL, 0, 0,
                 "may write is a way to replace");
        n = IMG_NODES[1];
        n.uid = 1000;
        fixture(1, &n, NULL, 0, region);
        st_image(&img_allow, "the directory holding them owned by "
                 "somebody else", "img-fixture", region, IMG_TOTAL, 0, 0,
                 "may write is a way to replace");
        fixture(-1, NULL, NULL, 0, region);
        st_image(&img_allow, "a region cut on a node boundary",
                 "img-fixture", region, IMG_TOTAL, NODE_SIZE * 3, 0,
                 "does not close");
        fixture(-1, NULL, NULL, 0, region);
        wr64(region + 2 * NODE_SIZE + NODE_SIZEF_OFF, IMG_TOTAL * 2);
        st_image(&img_allow, "a node larger than the region",
                 "img-fixture", region, IMG_TOTAL, 0, 0, "which no node");
        fixture(-1, NULL, NULL, 0, region);
        wr64(region + 2 * NODE_SIZE + NODE_DATA_OFF, IMG_TOTAL);
        st_image(&img_allow, "a node claiming more data than it holds",
                 "img-fixture", region, IMG_TOTAL, 0, 0, "bytes of data");
        n = IMG_NODES[3];
        n.name = "su";
        n.mode = 0755;
        fixture(3, &n, NULL, 0, region);
        st_image(&img_allow, "two nodes on one path", "img-fixture",
                 region, IMG_TOTAL, 0, 0, "resolve to the path");
        n = IMG_NODES[3];
        n.name = "m\xff";
        fixture(3, &n, NULL, 0, region);
        st_image(&img_allow, "a non-ASCII node name", "img-fixture",
                 region, IMG_TOTAL, 0, 0, "not ASCII");
        n = IMG_NODES[3];
        n.name = "busy/box";
        fixture(3, &n, NULL, 0, region);
        st_image(&img_allow, "a node name that is two path components",
                 "img-fixture", region, IMG_TOTAL, 0, 0,
                 "not one path component");
        n = IMG_NODES[3];
        n.name = "..";
        fixture(3, &n, NULL, 0, region);
        st_image(&img_allow, "a node named with a dot", "img-fixture",
                 region, IMG_TOTAL, 0, 0, "not one path component");
        fixture(-1, NULL, NULL, 0, region);
        st_image(&img_allow, "an allowlist with nothing to find",
                 "img-blind", region, IMG_TOTAL, 0, 0, "nothing to find");
    }

    st_clean();
    if (st_bad) {
        printf("check-setid: selftest: %d of %d cases failed\n", st_bad,
               st_ran);
        return 1;
    }
    printf("check-setid: selftest: %d cases PASS\n", st_ran);
    return 0;
}




