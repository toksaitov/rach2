/*
 * proof.c: the catalogue of a mutation proof, read and checked, and the edits
 * of its faults, in plain C. posix/proof.c runs the proof.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "etude/proof.h"
#include "json.h"
#include "proof_file.h"
#include "schema.h"

/* The placeholders a build's or a test's arguments may use. */
static const char *const PLACEHOLDERS[] = {"${TREE}", "${SRC}", "${WORK}", "${PHASE}"};

/* The catalogue being read: its path, where findings go, and their count. */
struct reader {
    const char *path;
    FILE *log;
    int bad;
};

int proof_slurp(const char *path, char **data, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    size_t n = 0, cap = 0, got;
    int e;

    if (f == NULL)
        return -1;
    do {
        if (n == cap) {
            char *more;

            cap = cap ? cap * 2 : 8192;
            if (cap > PROOF_FILE_MAX + 1)
                cap = PROOF_FILE_MAX + 1;
            more = realloc(buf, cap + 1);
            if (more == NULL || n == cap) {
                e = more == NULL ? ENOMEM : EFBIG;
                free(more != NULL ? more : buf);
                fclose(f);
                errno = e;
                return -1;
            }
            buf = more;
        }
        got = fread(buf + n, 1, cap - n, f);
        n += got;
    } while (got > 0);
    e = ferror(f) ? EIO : 0;
    fclose(f);
    if (e != 0 || n > PROOF_FILE_MAX) {
        free(buf);
        errno = e != 0 ? e : EFBIG;
        return -1;
    }
    buf[n] = '\0';
    *data = buf;
    *len = n;
    return 0;
}

/** Report one finding of the catalogue at `line`. */
static void finding(struct reader *r, int line, const char *fmt, ...)
{
    va_list ap;

    fprintf(r->log, "etude-proof: %s:%d: ", r->path, line);
    va_start(ap, fmt);
    vfprintf(r->log, fmt, ap);
    va_end(ap);
    fputc('\n', r->log);
    r->bad = 1;
}

/** Report one fault of the schema check as a finding. */
static void schema_fault(void *ctx, int line, const char *path, const char *what)
{
    finding(ctx, line, "%s: %s", path, what);
}

/** Give a copy of `s` from malloc, or NULL. */
static char *copy(const char *s)
{
    size_t n = strlen(s) + 1;
    char *c = malloc(n);

    if (c != NULL)
        memcpy(c, s, n);
    return c;
}

/** Free a list ended by NULL. */
static void list_free(char **list)
{
    size_t i;

    for (i = 0; list != NULL && list[i] != NULL; i++)
        free(list[i]);
    free(list);
}

/** Copy the strings of array `a` into a list ended by NULL, or give NULL. */
static char **list_of(const struct json *a, size_t *count)
{
    char **list = calloc(a->count + 1, sizeof(*list));
    size_t i;

    for (i = 0; list != NULL && i < a->count; i++)
        if ((list[i] = copy(a->item[i]->string)) == NULL) {
            list_free(list);
            return NULL;
        }
    if (count != NULL)
        *count = a->count;
    return list;
}

/**
 * Report each `${NAME}` of string `v` whose NAME is capitals and underscores
 * and that is not one of PLACEHOLDERS. Other text, a shell's `${CC:-cc}`
 * among it, is passed on unchanged.
 */
static void check_placeholders(struct reader *r, const struct json *v)
{
    const char *s = v->string, *at;
    size_t i, n;

    while ((at = strstr(s, "${")) != NULL) {
        n = strspn(at + 2, "ABCDEFGHIJKLMNOPQRSTUVWXYZ_");
        s = at + 2;
        if (n == 0 || at[2 + n] != '}')
            continue;
        for (i = 0; i < sizeof(PLACEHOLDERS) / sizeof(PLACEHOLDERS[0]); i++)
            if (strncmp(at, PLACEHOLDERS[i], strlen(PLACEHOLDERS[i])) == 0)
                break;
        if (i == sizeof(PLACEHOLDERS) / sizeof(PLACEHOLDERS[0])) {
            finding(r, v->line, "an unknown placeholder: %.*s", (int)n + 3, at);
            return;
        }
    }
}

/** Report the unknown placeholders of each string of array `a`. */
static void check_list(struct reader *r, const struct json *a)
{
    size_t i;

    for (i = 0; i < a->count; i++)
        check_placeholders(r, a->item[i]);
}

/** Say whether array `a` has the string `s`. */
static int has(const struct json *a, const char *s)
{
    size_t i;

    for (i = 0; i < a->count; i++)
        if (strcmp(a->item[i]->string, s) == 0)
            return 1;
    return 0;
}

/**
 * Check the rules a schema cannot state: fault names given once, edits of
 * the proof's own sources, known placeholders, and a prefix for `only`.
 */
static void check_rules(struct reader *r, const struct json *doc)
{
    const struct json *sources = json_member(doc, "sources");
    const struct json *faults = json_member(doc, "faults");
    const struct json *test = json_member(doc, "test");
    size_t i, j;

    check_list(r, json_member(doc, "artifacts"));
    check_list(r, json_member(json_member(doc, "build"), "argv"));
    check_list(r, json_member(test, "argv"));
    for (i = 0; i < faults->count; i++) {
        const struct json *f = faults->item[i];
        const struct json *edits = json_member(f, "edits");
        const struct json *only = json_member(json_member(f, "fails"), "only");
        const char *name = json_text(f, "name");

        for (j = 0; j < i; j++)
            if (strcmp(json_text(faults->item[j], "name"), name) == 0)
                finding(r, f->line, "fault %s is given twice", name);
        for (j = 0; j < edits->count; j++)
            if (!has(sources, json_text(edits->item[j], "file")))
                finding(r, edits->item[j]->line, "fault %s edits %s, which is not a source", name,
                        json_text(edits->item[j], "file"));
        if (only != NULL && only->integer && json_member(test, "fail_prefix") == NULL)
            finding(r, f->line, "fault %s is `only` and the test has no fail_prefix", name);
    }
}

/** Fill fault `f` from its member of the catalogue; 0, or -1 for no memory. */
static int fault_of(const struct json *v, struct etude_fault *f)
{
    const struct json *edits = json_member(v, "edits");
    const struct json *fails = json_member(v, "fails");
    const struct json *status = json_member(fails, "status");
    const struct json *only = json_member(fails, "only");
    size_t i;

    f->name = copy(json_text(v, "name"));
    f->summary = copy(json_text(v, "summary"));
    f->edit = calloc(edits->count, sizeof(*f->edit));
    f->lines = list_of(json_member(fails, "lines"), &f->nlines);
    f->status = status != NULL ? (int)status->integer : 0;
    f->only = only != NULL && only->integer;
    if (f->name == NULL || f->summary == NULL || f->edit == NULL || f->lines == NULL)
        return -1;
    f->nedits = edits->count;
    for (i = 0; i < edits->count; i++) {
        struct etude_edit *e = &f->edit[i];

        e->file = copy(json_text(edits->item[i], "file"));
        e->find = copy(json_text(edits->item[i], "find"));
        e->replace = copy(json_text(edits->item[i], "replace"));
        if (e->file == NULL || e->find == NULL || e->replace == NULL)
            return -1;
    }
    return 0;
}

/** Make the proof of a checked catalogue, or give NULL for no memory. */
static struct etude_proof *proof_of(const struct json *doc)
{
    const struct json *build = json_member(doc, "build");
    const struct json *test = json_member(doc, "test");
    const struct json *faults = json_member(doc, "faults");
    const char *prefix = json_text(test, "fail_prefix");
    struct etude_proof *p = calloc(1, sizeof(*p));
    size_t i;

    if (p == NULL)
        return NULL;
    p->name = copy(json_text(doc, "proof"));
    p->summary = copy(json_text(doc, "summary"));
    p->shared = strcmp(json_text(doc, "tree"), "shared") == 0;
    p->sources = list_of(json_member(doc, "sources"), &p->nsources);
    p->artifacts = list_of(json_member(doc, "artifacts"), &p->nartifacts);
    p->build = list_of(json_member(build, "argv"), NULL);
    p->build_bound_us = (uint64_t)json_member(build, "bound_s")->integer * 1000000u;
    p->test = list_of(json_member(test, "argv"), NULL);
    p->test_bound_us = (uint64_t)json_member(test, "bound_s")->integer * 1000000u;
    p->fail_prefix = prefix != NULL ? copy(prefix) : NULL;
    p->faults = calloc(faults->count, sizeof(*p->faults));
    if (p->faults != NULL)
        p->nfaults = faults->count;
    for (i = 0; p->faults != NULL && i < faults->count; i++)
        if (fault_of(faults->item[i], &p->faults[i]) != 0)
            break;
    if (p->name == NULL || p->summary == NULL || p->sources == NULL || p->artifacts == NULL ||
        p->build == NULL || p->test == NULL || (prefix != NULL && p->fail_prefix == NULL) ||
        p->faults == NULL || i < faults->count) {
        etude_proof_free(p);
        return NULL;
    }
    return p;
}

/** Read and parse the JSON file at `path`; 0, 1 for a finding, or 2. */
static int read_json(struct reader *r, const char *path, struct json **doc)
{
    char *text, err[200];
    size_t len;
    int status = 0;

    if (proof_slurp(path, &text, &len) != 0) {
        fprintf(r->log, "etude-proof: %s: %s\n", path, strerror(errno));
        return 2;
    }
    if (json_parse(text, len, 1, doc, err, sizeof err) != 0) {
        fprintf(r->log, "etude-proof: %s: %s\n", path, err);
        status = 1;
    }
    free(text);
    return status;
}

int etude_proof_read(const char *path, const char *schemas, FILE *log, struct etude_proof **out)
{
    struct reader r;
    struct json *schema = NULL, *doc = NULL;
    char spath[4096];
    int status;

    *out = NULL;
    r.path = path;
    r.log = log;
    r.bad = 0;
    snprintf(spath, sizeof spath, "%s/proof.schema.json", schemas);
    status = read_json(&r, spath, &schema);
    if (status != 0)
        return 2;
    status = read_json(&r, path, &doc);
    if (status == 0 && schema_check(schema, doc, schema_fault, &r) == 0)
        check_rules(&r, doc);
    if (status == 0 && r.bad)
        status = 1;
    if (status == 0 && (*out = proof_of(doc)) == NULL) {
        fprintf(log, "etude-proof: %s: %s\n", path, strerror(ENOMEM));
        status = 2;
    }
    json_free(doc);
    json_free(schema);
    return status;
}

void etude_proof_free(struct etude_proof *p)
{
    size_t i, j;

    if (p == NULL)
        return;
    for (i = 0; p->faults != NULL && i < p->nfaults; i++) {
        struct etude_fault *f = &p->faults[i];

        for (j = 0; f->edit != NULL && j < f->nedits; j++) {
            free(f->edit[j].file);
            free(f->edit[j].find);
            free(f->edit[j].replace);
        }
        free(f->edit);
        free(f->name);
        free(f->summary);
        list_free(f->lines);
    }
    free(p->faults);
    free(p->name);
    free(p->summary);
    list_free(p->sources);
    list_free(p->artifacts);
    list_free(p->build);
    list_free(p->test);
    free(p->fail_prefix);
    free(p);
}

/** Find `find` in `text` of `len` bytes from `from`, or give NULL. */
static const char *find_in(const char *text, size_t len, size_t from, const char *find)
{
    size_t n = strlen(find), i;

    for (i = from; n <= len && i <= len - n; i++)
        if (memcmp(text + i, find, n) == 0)
            return text + i;
    return NULL;
}

size_t etude_edit_apply(const char *text, size_t len, const struct etude_edit *e, char **out,
                        size_t *outlen)
{
    const char *at = find_in(text, len, 0, e->find), *next;
    size_t count = 0, flen = strlen(e->find), rlen = strlen(e->replace), head;

    *out = NULL;
    for (next = at; next != NULL; count++)
        next = find_in(text, len, (size_t)(next - text) + 1, e->find);
    if (count != 1)
        return count;
    head = (size_t)(at - text);
    *outlen = len - flen + rlen;
    *out = malloc(*outlen + 1);
    if (*out == NULL)
        return 0;
    memcpy(*out, text, head);
    memcpy(*out + head, e->replace, rlen);
    memcpy(*out + head + rlen, at + flen, len - head - flen);
    (*out)[*outlen] = '\0';
    return 1;
}

/** Check one fault's edits of source `file`, whose text is `text`; 0 or 1. */
static int texts_of(const struct etude_proof *p, const struct etude_fault *f, const char *file,
                    char *text, size_t len, FILE *log)
{
    size_t i, count, n;
    char *next;

    for (i = 0; i < f->nedits; i++) {
        if (strcmp(f->edit[i].file, file) != 0)
            continue;
        count = etude_edit_apply(text, len, &f->edit[i], &next, &n);
        if (count != 1 || next == NULL) {
            fprintf(log, "etude-proof: %s: fault %s: edit %zu finds its text %zu times in %s\n",
                    p->name, f->name, i + 1, count, file);
            free(text);
            return 1;
        }
        free(text);
        text = next;
        len = n;
    }
    free(text);
    return 0;
}

int etude_proof_texts(const struct etude_proof *p, const char *tree, FILE *log)
{
    char path[4096], *text;
    size_t i, j, len;
    int status = 0;

    for (i = 0; i < p->nsources; i++) {
        snprintf(path, sizeof path, "%s/%s", tree, p->sources[i]);
        for (j = 0; j < p->nfaults; j++) {
            if (proof_slurp(path, &text, &len) != 0) {
                fprintf(log, "etude-proof: %s: %s: %s\n", p->name, path, strerror(errno));
                return 2;
            }
            if (texts_of(p, &p->faults[j], p->sources[i], text, len, log) != 0)
                status = 1;
        }
    }
    return status;
}
