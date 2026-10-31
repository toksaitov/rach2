/* preparation_record.c: validate prepared artifacts without build tools. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "etude/artifact.h"
#include "etude/legacy.h"
#include "preparation_record.h"

static char *record_path(const char *tree, const char *name)
{
    int n = snprintf(NULL, 0, "%s/%s", tree, name);
    char *path = n < 0 ? NULL : malloc((size_t)n + 1);
    if (path != NULL)
        snprintf(path, (size_t)n + 1, "%s/%s", tree, name);
    return path;
}

/** Give the modification time of `st` in nanoseconds. */
long long etude_record_mtime_ns(const struct stat *st)
{
#if defined(__APPLE__)
    return (long long)st->st_mtimespec.tv_sec * 1000000000LL + st->st_mtimespec.tv_nsec;
#else
    return (long long)st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
#endif
}

/** Give the path of the digests file of the tree `tree`, from malloc. */
char *etude_record_digests_path(const char *tree)
{
    return record_path(tree, ETUDE_LEGACY_DIGESTS);
}

/** Give the path of artifact `name`, from malloc: a name of the tree is
 * taken from its root. */
char *etude_record_artifact_path(const char *tree, const char *name)
{
#ifdef _WIN32
    int absolute = etude_win_absolute(name);
#else
    int absolute = name[0] == '/';
#endif
    return absolute ? strdup(name) : record_path(tree, name);
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

/* A reading of a record. */
struct reading {
    char *tree;              /* the tree's root, from the `tree` line */
    char *arch;              /* the `arch` line's architecture */
    char *kernel;            /* the `kernel` line's path */
    size_t files;            /* the artifacts compared */
    size_t changed;          /* those changed or missing */
    int digests;             /* compare by digest */
    struct etude_digests *d; /* the tree's digests, once one is compared */
    const char *override_tree;
    FILE *out;
};

/**
 * Compare artifact `e` of the tree `tree` with the file there now, by its
 * digest when `rd` says so, taken from the tree's digests file when it keeps
 * one, which `rd` reads once.
 *
 * @return 0 when it is unchanged, 1 when it changed or is missing.
 */
static int compare(struct reading *rd, const char *tree, const struct entry *e)
{
    char *path = etude_record_artifact_path(tree, e->path), *cache, hex[65];
    struct stat st;
    int r, changed;

    if (rd->digests && rd->d == NULL && (cache = etude_record_digests_path(tree)) != NULL) {
        rd->d = etude_digests_open(cache);
        free(cache);
    }
    r = path == NULL || (rd->digests && rd->d == NULL) ? -1
        : rd->digests                                  ? etude_digests_file(rd->d, path, &st, hex)
                                                       : stat(path, &st);
    free(path);
    if (r != 0) {
        fprintf(rd->out, "etude check: missing: %s: %s\n", e->path,
                errno == EINVAL || errno == EISDIR ? "not a regular file" : strerror(errno));
        return 1;
    }
    changed = !S_ISREG(st.st_mode) || (unsigned long long)st.st_size != e->size;
    if (rd->digests)
        changed = changed || strcmp(hex, e->digest) != 0;
    else
        changed = changed || (unsigned long long)st.st_ino != e->inode ||
                  etude_record_mtime_ns(&st) != e->mtime;
    if (changed)
        fprintf(rd->out, "etude check: changed: %s\n", e->path);
    return changed;
}

/** Take line `lineno` of a record; NULL, or why the record is malformed. */
static const char *check_line(struct reading *rd, char *line, size_t lineno)
{
    struct entry e;

    if (lineno == 1)
        return strcmp(line, ETUDE_PREPARATION_HEAD) == 0 ? NULL : "not a record of etude prepare";
    if (strncmp(line, "tree ", 5) == 0 && rd->tree == NULL)
        return (rd->tree = strdup(line + 5)) == NULL ? "memory ran out" : NULL;
    if (strncmp(line, "file ", 5) == 0) {
        if (rd->tree == NULL || take_entry(line, &e) != 0)
            return "a malformed file line";
        rd->changed +=
            (size_t)compare(rd, rd->override_tree != NULL ? rd->override_tree : rd->tree, &e);
        rd->files++;
        return NULL;
    }
    if (strncmp(line, "arch ", 5) == 0 && rd->arch == NULL)
        return (rd->arch = strdup(line + 5)) == NULL ? "memory ran out" : NULL;
    if (strncmp(line, "kernel ", 7) == 0 && rd->kernel == NULL)
        return (rd->kernel = strdup(line + 7)) == NULL ? "memory ran out" : NULL;
    if (strncmp(line, "arch ", 5) == 0 || strncmp(line, "kernel ", 7) == 0)
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
        fprintf(rd->out, "etude check: %s:%lu: %s\n", record, (unsigned long)lineno, why);
        return 2;
    }
    return rd->changed != 0;
}

int etude_legacy_check_tree(const char *record, const char *tree, int digests, FILE *out)
{
    struct reading rd;
    FILE *f = fopen(record, "r");
    int status;

    memset(&rd, 0, sizeof(rd));
    rd.digests = digests;
    rd.override_tree = tree;
    rd.out = out != NULL ? out : stdout;
    if (f == NULL) {
        fprintf(rd.out, "etude check: %s: %s\n", record, strerror(errno));
        return 2;
    }
    status = check_lines(f, record, &rd);
    fclose(f);
    etude_digests_close(rd.d);
    free(rd.tree);
    free(rd.arch);
    free(rd.kernel);
    if (status == 0)
        fprintf(rd.out, "etude check: result: unchanged: %lu files\n", (unsigned long)rd.files);
    else if (status == 1)
        fprintf(rd.out, "etude check: result: changed: %lu of %lu files\n",
                (unsigned long)rd.changed, (unsigned long)rd.files);
    return status;
}

int etude_legacy_check(const char *record, int digests, FILE *out)
{
    return etude_legacy_check_tree(record, NULL, digests, out);
}
