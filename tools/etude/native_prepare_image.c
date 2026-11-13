/*
 * native_prepare_image.c: the volume images and private roots of the local
 * Rach2 collection, made with the build's KRFFS tools and one e2fsprogs.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "etude/exec.h"
#include "native_prepare.h"
#include "sha256.h"

/* The one ext2 profile of Rach2's volumes, as tools/setup/ext2_profile.sh. */
#define EXT2_PROFILE                                                                           \
    "sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent," \
    "^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize"

/* The greatest output kept of one preparation command. */
#define OUTPUT_MAX (64u << 20)

static void print_command(char *const argv[])
{
    fputs("etude prepare native:", stdout);
    for (size_t i = 0; argv[i]; i++)
        printf(" %s", argv[i]);
    putchar('\n');
    fflush(stdout);
}

/**
 * Run one preparation command in the tree with the tools' environment.
 *
 * Its standard output and then its standard error are appended to `output`
 * when that is not NULL, and printed when the command fails. A stop request
 * cancels the command.
 *
 * @return 0 when the command exited 0 and its cleanup was confirmed, or -1.
 */
int np_run(struct np_context *c, char *const argv[], struct jbuf *output)
{
    struct etude_exec *ex = etude_exec_new();
    struct etude_proc *p = NULL;
    struct etude_exec_spec spec;
    int result = -1, cancelled = 0;

    print_command(argv);
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = "native-preparation";
    spec.argv = argv;
    spec.envp = c->environment;
    spec.cwd = c->tree;
    spec.output_max = OUTPUT_MAX;
    spec.grace_us = 5000000;
    if (ex == NULL || etude_exec_start(ex, &spec, &p) != 0) {
        fprintf(stderr, "etude prepare native: %s: %s\n", argv[0], strerror(errno));
        if (ex)
            etude_exec_free(ex);
        return -1;
    }
    /* interval: observe preparation stop requests every 100 ms */
    while (etude_exec_wait(ex, 100) != p) {
        if (c->stop && *c->stop && !cancelled) {
            etude_exec_cancel(p);
            cancelled = 1;
        }
    }
    const struct etude_exec_result *r = etude_proc_result(p);
    int complete = r->out.total == r->out.kept && r->err.total == r->err.kept;
    if (r->end == ETUDE_END_EXITED && r->value == 0 && r->cleanup == ETUDE_CLEANUP_CONFIRMED &&
        complete)
        result = 0;
    if (result == 0 && output) {
        jbuf_put(output, r->out.data ? r->out.data : "", r->out.kept);
        jbuf_put(output, r->err.data ? r->err.data : "", r->err.kept);
    } else if (result != 0) {
        fwrite(r->out.data ? r->out.data : "", 1, r->out.kept, stderr);
        fwrite(r->err.data ? r->err.data : "", 1, r->err.kept, stderr);
        fprintf(stderr, "etude prepare native: %s: %s\n", argv[0],
                cancelled                    ? "stopped"
                : !complete                  ? "output too long"
                : r->end != ETUDE_END_EXITED ? "did not exit"
                : r->value                   ? "failed"
                                             : "cleanup unconfirmed");
    }
    etude_proc_free(p);
    etude_exec_free(ex);
    return result;
}

/** Make `path` an empty file of `mib` MiB, sparse where the system allows. */
int np_blank(const char *path, long mib)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return -1;
    int bad = ftruncate(fd, (off_t)mib * 1048576);
    if (close(fd))
        bad = -1;
    return bad ? -1 : 0;
}

static uint64_t le(const unsigned char *b, size_t n)
{
    uint64_t v = 0;
    while (n--)
        v = v << 8 | b[n];
    return v;
}

/* The fault of the KRFFS node header `h` at `offset` of a root of `length`
 * bytes, its back link aside, or NULL when it has none. */
static const char *node_fault(const unsigned char *h, uint64_t offset, uint64_t length,
                              int reject_padding)
{
    unsigned kind = h[2];
    uint64_t size = le(h + 325, 8), data = le(h + 341, 8), parent = le(h + 349, 8);

    if (le(h, 2) != 0x4b52 || kind > 2 || size < NP_KRFFS_HEADER || size > length - offset ||
        (kind == 0) != (offset == 0))
        return "invalid KRFFS root chain";
    if (reject_padding && kind == 2 && parent == 0 &&
        strncmp((const char *)h + 11, &NP_ROOT_PADDING[1], 256) == 0)
        return "native root reserves " NP_ROOT_PADDING;
    if (kind == 1)
        return offset + size != length ? "native KRFFS root is not packed" : NULL;
    if (data > size - NP_KRFFS_HEADER)
        return "invalid KRFFS root data";
    return NULL;
}

/**
 * Read the free tail of a packed KRFFS root, its last node, which must be
 * the only free one.
 *
 * Every node's header must be whole and its size, chain and data length
 * valid; only the first is the root, and the root's back link names the
 * free tail. With `reject_padding`, a node at the top named as
 * NP_ROOT_PADDING is refused, since preparation adds that file itself.
 *
 * @return 0 with the tail's offset in `*used` and its size in `*free_bytes`,
 *         or -1 after a diagnostic.
 */
int np_krffs_free_tail(const char *path, int reject_padding, uint64_t *used, uint64_t *free_bytes)
{
    unsigned char h[NP_KRFFS_HEADER];
    struct stat st;
    uint64_t offset = 0, previous = 0, root_previous = 0;
    int have_previous = 0, tail = 0;
    FILE *image = fopen(path, "rb");
    const char *why = NULL;

    if (image == NULL || fstat(fileno(image), &st) != 0) {
        fprintf(stderr, "etude prepare native: %s: %s\n", path, strerror(errno));
        if (image)
            fclose(image);
        return -1;
    }
    uint64_t length = (uint64_t)st.st_size;
    while (!why && offset < length) {
        if (fseeko(image, (off_t)offset, SEEK_SET) != 0 ||
            fread(h, 1, sizeof(h), image) != sizeof(h)) {
            why = "short KRFFS root header";
            break;
        }
        uint64_t size = le(h + 325, 8), back = le(h + 333, 8);
        why = have_previous && back != previous ? "invalid KRFFS root chain"
                                                : node_fault(h, offset, length, reject_padding);
        if (offset == 0)
            root_previous = back;
        if (h[2] == 1) {
            *used = offset;
            *free_bytes = size;
            tail = 1;
        }
        previous = size;
        have_previous = 1;
        offset += size;
    }
    fclose(image);
    if (!why && (!tail || root_previous != previous))
        why = "native KRFFS root has no complete free tail";
    if (why) {
        fprintf(stderr, "etude prepare native: %s: %s\n", path, why);
        return -1;
    }
    return 0;
}

static int write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    int bad = fputs(text, f) == EOF;
    if (fclose(f))
        bad = 1;
    return bad ? -1 : 0;
}

static int work_path(const struct np_context *c, char path[4096], const char *name)
{
    int n = snprintf(path, 4096, "%s/%s", c->work, name);
    return n < 0 || n >= 4096 ? -1 : 0;
}

/**
 * Stage a file of zeros at NP_ROOT_PADDING that brings the root's free
 * extent to a block boundary, so that the files a case makes there start on
 * one.
 *
 * @return 0 with the free extent's size in `*free_bytes`, or -1.
 */
static int align_root(struct np_context *c, const char *out, uint64_t *free_bytes)
{
    char padding[4096], recipe[4096], line[8192];
    uint64_t used;

    if (np_krffs_free_tail(out, 1, &used, free_bytes) ||
        work_path(c, padding, "root-padding.bin") || work_path(c, recipe, "root-padding.manifest"))
        return -1;
    uint64_t pad = (NP_KRFFS_BLOCK - (used + NP_KRFFS_HEADER) % NP_KRFFS_BLOCK) % NP_KRFFS_BLOCK;
    if (np_blank(padding, 0) || truncate(padding, (off_t)pad) ||
        snprintf(line, sizeof(line), "file 0444:0:0 %s %s\n", NP_ROOT_PADDING, padding) >=
            (int)sizeof(line) ||
        write_text(recipe, line))
        return -1;
    char *argv[] = {c->tools[NP_STAGE], recipe, (char *)out, NULL};
    if (np_run(c, argv, NULL) || np_krffs_free_tail(out, 0, &used, free_bytes))
        return -1;
    if (used % NP_KRFFS_BLOCK) {
        fprintf(stderr, "etude prepare native: %s: free extent is not block aligned\n", out);
        return -1;
    }
    return 0;
}

/* Set each staged node's mode and owner, which mke2fs -d takes from the
 * staging tree, to those stageimage's tree mode reports. */
static int ext2_owners(struct np_context *c, const char *out, const char *nodes)
{
    char commands[4096];
    struct jbuf edits = {0}, said = {0};
    int bad = work_path(c, commands, "debugfs");

    for (const char *line = nodes; !bad && *line;) {
        size_t n = strcspn(line, "\n");
        char *copy = strndup(line, n), *field[5] = {0}, *save = NULL;
        size_t count = 0;
        for (char *w = strtok_r(copy, " \t\r", &save); w && count < 5;
             w = strtok_r(NULL, " \t\r", &save))
            field[count++] = w;
        if (count == 5 && !strcmp(field[0], "node")) {
            static const char *const names[] = {"mode", "uid", "gid"};
            for (size_t i = 0; i < 3; i++) {
                jbuf_puts(&edits, "set_inode_field ");
                jbuf_puts(&edits, field[4]);
                jbuf_puts(&edits, " ");
                jbuf_puts(&edits, names[i]);
                jbuf_puts(&edits, " ");
                jbuf_puts(&edits, field[i + 1]);
                jbuf_puts(&edits, "\n");
            }
        }
        free(copy);
        line += n + (line[n] == '\n');
    }
    bad = bad || write_text(commands, edits.data ? edits.data : "\n");
    char *argv[] = {c->tools[NP_DEBUGFS], "-w", "-f", commands, (char *)out, NULL};
    bad = bad || np_run(c, argv, &said);
    for (const char *line = said.data ? said.data : ""; !bad && *line;) {
        size_t n = strcspn(line, "\n");
        if (n && strncmp(line, "debugfs", 7)) {
            fprintf(stderr, "etude prepare native: %s: debugfs: %.*s\n", out, (int)n, line);
            bad = 1;
        }
        line += n + (line[n] == '\n');
    }
    jbuf_free(&edits);
    jbuf_free(&said);
    return bad ? -1 : 0;
}

/**
 * Make the private root `out` of `kind`, krffs, ext2 or ext2-dir, from the
 * stager manifest `text`, with `mib` mebibytes free beyond its files.
 *
 * A KRFFS root is staged into a fresh volume of `mib` plus 10 MiB, made
 * again once, larger by the shortfall, when its files leave less than `mib`
 * free, and padded so that its free extent starts on a block; an ext2 root
 * is staged as a tree, formatted over it, given the manifest's owners and
 * modes, and checked by e2fsck.
 *
 * @return 0, or -1 after a diagnostic.
 */
int np_root_image(struct np_context *c, const char *out, const char *kind, long mib,
                  const char *text)
{
    char manifest[4096], staged[4096];
    struct jbuf nodes = {0};

    if (work_path(c, manifest, "root.manifest") || write_text(manifest, text))
        return -1;
    if (!strcmp(kind, "krffs")) {
        uint64_t free_bytes = 0, want = (uint64_t)mib * 1048576;
        char *mkfs[] = {c->tools[NP_MKFS], (char *)out, NULL};
        char *stage[] = {c->tools[NP_STAGE], manifest, (char *)out, NULL};
        long size = mib + 10;
        for (int pass = 0; pass < 2 && free_bytes < want; pass++) {
            /* A second pass grows the volume by what the files took beyond
             * the first pass's 10 MiB allowance. */
            if (pass)
                size += (long)((want - free_bytes + 1048575) / 1048576) + 1;
            if (np_blank(out, size) || np_run(c, mkfs, NULL) || np_run(c, stage, NULL) ||
                align_root(c, out, &free_bytes))
                return -1;
        }
        if (free_bytes < want) {
            fprintf(stderr, "etude prepare native: root %s has insufficient free space: %llu\n",
                    out, (unsigned long long)free_bytes);
            return -1;
        }
        return 0;
    }
    if (strcmp(kind, "ext2") && strcmp(kind, "ext2-dir")) {
        fprintf(stderr, "etude prepare native: unknown root kind: %s\n", kind);
        return -1;
    }
    if (work_path(c, staged, "staged") || np_remove(staged))
        return -1;
    char *stage[] = {c->tools[NP_STAGE], manifest, "--tree", staged, NULL};
    char *mke2fs[] = {
        c->tools[NP_MKE2FS], "-q", "-F",    "-t", "ext2", "-b",        "4096", "-I", "128", "-O",
        EXT2_PROFILE,        "-L", "rach2", "-d", staged, (char *)out, NULL};
    char *e2fsck[] = {c->tools[NP_E2FSCK], "-fn", (char *)out, NULL};
    int bad = np_run(c, stage, &nodes) || np_blank(out, !strcmp(kind, "ext2") ? mib + 16 : 32) ||
              np_run(c, mke2fs, NULL) || ext2_owners(c, out, nodes.data ? nodes.data : "") ||
              np_run(c, e2fsck, NULL);
    jbuf_free(&nodes);
    return bad ? -1 : 0;
}

/* ---- The id map and the roots' manifests. ---- */

struct idmap {
    char **old, **new;
    size_t n;
};

static char *slurp(const struct np_context *c, const char *name)
{
    char path[4096];
    struct jbuf text = {0};
    char bytes[65536];
    size_t n;

    if (snprintf(path, sizeof(path), "%s/%s", c->tree, name) >= (int)sizeof(path))
        return NULL;
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "etude prepare native: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    while ((n = fread(bytes, 1, sizeof(bytes), f)))
        jbuf_put(&text, bytes, n);
    int bad = ferror(f);
    fclose(f);
    if (bad) {
        jbuf_free(&text);
        return NULL;
    }
    return text.data ? text.data : np_strdup("");
}

/* Split `line` into at most `max` words, the last of which keeps the rest of
 * the line after its leading blanks, as Python's str.split(None, max - 1). */
static size_t words(char *line, char **word, size_t max)
{
    size_t n = 0;
    char *p = line;
    while (n < max) {
        p += strspn(p, " \t\r");
        if (!*p)
            break;
        word[n++] = p;
        if (n == max)
            break;
        p += strcspn(p, " \t\r");
        if (*p)
            *p++ = 0;
    }
    return n;
}

static void idmap_free(struct idmap *m)
{
    for (size_t i = 0; i < m->n; i++) {
        free(m->old[i]);
        free(m->new[i]);
    }
    free(m->old);
    free(m->new);
}

static const char *mapped(const struct idmap *m, const char *id, size_t n)
{
    for (size_t i = 0; i < m->n; i++)
        if (strlen(m->old[i]) == n && !strncmp(m->old[i], id, n))
            return m->new[i];
    return NULL;
}

static int idmap_read(const struct np_context *c, struct idmap *m)
{
    char *text = slurp(c, "tests/native/maps"), *save = NULL;
    int bad = text == NULL;

    memset(m, 0, sizeof(*m));
    for (char *line = bad ? NULL : strtok_r(text, "\n", &save); !bad && line;
         line = strtok_r(NULL, "\n", &save)) {
        char *w[64];
        size_t n = words(line, w, 64);
        if (n == 64)
            bad = 1;
        if (bad || n == 0 || strcmp(w[0], "idmap"))
            continue;
        for (size_t i = 1; !bad && i < n; i++) {
            char *colon = strchr(w[i], ':');
            if (!colon || strchr(colon + 1, ':')) {
                bad = 1;
                break;
            }
            *colon = 0;
            for (size_t j = 0; j < m->n; j++)
                if (!strcmp(m->old[j], w[i]) || !strcmp(m->new[j], colon + 1))
                    bad = 1;
            char **old = realloc(m->old, (m->n + 1) * sizeof(*old));
            char **new = old ? realloc(m->new, (m->n + 1) * sizeof(*new)) : NULL;
            if (old)
                m->old = old;
            if (!new)
                exit(1);
            m->new = new;
            m->old[m->n] = np_strdup(w[i]);
            m->new[m->n++] = np_strdup(colon + 1);
        }
    }
    if (bad && text)
        fprintf(stderr, "etude prepare native: tests/native/maps: invalid or duplicate id map\n");
    free(text);
    if (bad)
        idmap_free(m);
    return bad ? -1 : 0;
}

static void put_id(struct jbuf *b, const struct idmap *m, const char *id, size_t n)
{
    const char *native = mapped(m, id, n);
    if (native)
        jbuf_puts(b, native);
    else
        jbuf_put(b, id, n);
}

/* Map the ids of one `text` line's contents, whose items are separated by a
 * written `\n`: the uid and gid of a passwd entry, the gid of a group entry,
 * and a sudoers line that is an id alone. */
static void map_text(struct jbuf *b, const struct idmap *m, const char *file, const char *text)
{
    int passwd = !strcmp(file, "/etc/passwd"), group = !strcmp(file, "/etc/group");
    int sudoers = !strcmp(file, "/etc/sudoers");
    for (const char *item = text;;) {
        const char *end = strstr(item, "\\n");
        size_t n = end ? (size_t)(end - item) : strlen(item);
        if (sudoers)
            put_id(b, m, item, n);
        else if (passwd || group) {
            size_t parts = 1;
            for (size_t i = 0; i < n; i++)
                parts += item[i] == ':';
            size_t part = 0;
            for (size_t i = 0; i <= n;) {
                size_t k = i;
                while (k < n && item[k] != ':')
                    k++;
                if (parts >= 3 && (part == 2 || (passwd && part == 3 && parts >= 4)))
                    put_id(b, m, item + i, k - i);
                else
                    jbuf_put(b, item + i, k - i);
                if (k < n)
                    jbuf_put(b, ":", 1);
                part++;
                i = k + 1;
            }
        } else
            jbuf_put(b, item, n);
        if (!end)
            break;
        jbuf_puts(b, "\\n");
        item = end + 2;
    }
}

static int is_file(const struct np_context *c, const char *relative, int executable)
{
    char path[8192];
    struct stat st;
    if (np_path(path, sizeof(path), "%s/%s", c->tree, relative))
        return 0;
    if (executable)
        return access(path, X_OK) == 0;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Append the manifest line `line` to `out`, its owners and a text file's
 * owner names mapped by `m`. */
static void map_line(struct jbuf *out, const struct idmap *m, const char *line)
{
    char *copy = np_strdup(line), *w[4];
    size_t n = words(copy, w, 4);
    if (n < 3 || (strcmp(w[0], "file") && strcmp(w[0], "dir") && strcmp(w[0], "text") &&
                  strcmp(w[0], "exec-dir"))) {
        jbuf_puts(out, line);
        jbuf_puts(out, "\n");
        free(copy);
        return;
    }
    jbuf_puts(out, w[0]);
    jbuf_puts(out, " ");
    char *first = strchr(w[1], ':'), *second = first ? strchr(first + 1, ':') : NULL;
    if (second && !strchr(second + 1, ':')) {
        jbuf_put(out, w[1], (size_t)(first + 1 - w[1]));
        put_id(out, m, first + 1, (size_t)(second - first - 1));
        jbuf_puts(out, ":");
        put_id(out, m, second + 1, strlen(second + 1));
    } else
        jbuf_puts(out, w[1]);
    jbuf_puts(out, " ");
    jbuf_puts(out, w[2]);
    if (n == 4) {
        jbuf_puts(out, " ");
        if (!strcmp(w[0], "text"))
            map_text(out, m, w[2], w[3]);
        else
            jbuf_puts(out, w[3]);
    }
    jbuf_puts(out, "\n");
    free(copy);
}

/* Whether the manifest `out` already has a file or text line for `/name`. */
static int stages(const struct jbuf *out, const char *name)
{
    int staged = 0;
    char *lines = np_strdup(out->data ? out->data : ""), *keep = NULL;
    for (char *line = strtok_r(lines, "\n", &keep); line && !staged;
         line = strtok_r(NULL, "\n", &keep)) {
        char *w[4];
        size_t n = words(line, w, 4);
        staged = n >= 3 && (!strcmp(w[0], "file") || !strcmp(w[0], "text")) && w[2][0] == '/' &&
                 !strncmp(w[2] + 1, name, strlen(name)) &&
                 (w[2][1 + strlen(name)] == 0 || strchr(" \t\r", w[2][1 + strlen(name)]));
    }
    free(lines);
    return staged;
}

/**
 * Make a root's stager manifest: the test image's, with its ids mapped, and
 * a `file` line at `/` for each of `extras`, the case's staged files, that
 * neither the build's `tests` directory nor the manifest has at that name.
 */
static int root_manifest(const struct np_context *c, const struct idmap *m, char **extras,
                         size_t nextras, struct jbuf *out)
{
    char *text = slurp(c, "krffs/manifests/test.manifest"), *next;
    if (text == NULL)
        return -1;
    for (char *line = text; *line; line = next) {
        next = line + strcspn(line, "\n");
        if (*next)
            *next++ = 0;
        map_line(out, m, line);
    }
    free(text);
    for (size_t i = 0; i < nextras; i++) {
        const char *name = strrchr(extras[i], '/') ? strrchr(extras[i], '/') + 1 : extras[i];
        char path[8192], source[8192];
        int native = !strncmp(extras[i], "native/", 7);
        snprintf(path, sizeof(path), "%s/tests/%s", c->build, name);
        if (is_file(c, path, 0) || stages(out, name))
            continue;
        snprintf(source, sizeof(source), "%s%s", native ? "tests/" : "build/tests/", extras[i]);
        snprintf(path, sizeof(path), "%s/tests/%s", native ? "." : c->build, extras[i]);
        jbuf_puts(out, "file ");
        jbuf_puts(out, is_file(c, path, 1) ? "0755" : "0644");
        jbuf_puts(out, " /");
        jbuf_puts(out, name);
        jbuf_puts(out, " ");
        jbuf_puts(out, source);
        jbuf_puts(out, "\n");
    }
    return 0;
}

/* ---- The volume images of `maps`. ---- */

static int copy_range(const char *from, const char *to, uint64_t start, uint64_t length, int whole)
{
    struct stat st;
    char bytes[1 << 16];
    FILE *in = fopen(from, "rb"), *out = NULL;
    int bad = in == NULL || fstat(fileno(in), &st) != 0;

    if (!bad && whole)
        length = (uint64_t)st.st_size;
    else if (!bad && start + length > (uint64_t)st.st_size) {
        fprintf(stderr, "etude prepare native: image range exceeds %s\n", from);
        bad = 1;
    }
    bad = bad || fseeko(in, (off_t)start, SEEK_SET) != 0 || (out = fopen(to, "wb")) == NULL;
    while (!bad && length) {
        size_t want = length < sizeof(bytes) ? (size_t)length : sizeof(bytes);
        size_t n = fread(bytes, 1, want, in);
        if (n == 0) {
            fprintf(stderr, "etude prepare native: short source image: %s\n", from);
            bad = 1;
        }
        bad = bad || fwrite(bytes, 1, n, out) != n;
        length -= n;
    }
    if (in)
        fclose(in);
    if (out && fclose(out))
        bad = 1;
    if (!bad && whole)
        bad = chmod(to, st.st_mode & 07777) != 0;
    return bad ? -1 : 0;
}

/* Copy an image of `maps` from the build, or the MIB mebibytes at its
 * `@BYTEOFFSET` in a disk the build made. */
static int image_from(const struct np_context *c, const char *origin, const char *to, long mib)
{
    char source[8192], path[8192];
    const char *at = strrchr(origin, '@');
    size_t n = at ? (size_t)(at - origin) : strlen(origin);

    if (n >= 4096)
        return -1;
    if (strcmp(c->build, "build") && !strncmp(origin, "build/", 6) &&
        strncmp(origin, "build/tests/system/", 19))
        snprintf(source, sizeof(source), "%s%.*s", c->build, (int)n - 5, origin + 5);
    else
        snprintf(source, sizeof(source), "%.*s", (int)n, origin);
    if (np_path(path, sizeof(path), "%s/%s", c->tree, source))
        return -1;
    if (!at)
        return copy_range(path, to, 0, 0, 1);
    if (!at[1] || strspn(at + 1, "0123456789") != strlen(at + 1)) {
        fprintf(stderr, "etude prepare native: invalid image byte offset: %s\n", origin);
        return -1;
    }
    return copy_range(path, to, strtoull(at + 1, NULL, 10), (uint64_t)mib * 1048576, 0);
}

static int maps_image(struct np_context *c, const char *output, char **w, size_t n)
{
    char path[8192], sectors[32];
    char *end;

    if (n != 4 && (n != 6 || strcmp(w[4], "from")))
        return -1;
    long mib = strtol(w[3], &end, 10);
    if (*end || mib <= 0 || strchr(w[1], '/') ||
        snprintf(path, sizeof(path), "%s/%s", output, w[1]) >= (int)sizeof(path))
        return -1;
    if (n == 6)
        return image_from(c, w[5], path, mib);
    if (!strcmp(w[2], "exfat")) {
        snprintf(sectors, sizeof(sectors), "%ld", mib * 2048);
        char *argv[] = {c->tools[NP_EXFAT], "--make", path, "--sectors", sectors, NULL};
        return np_run(c, argv, NULL);
    }
    if (np_blank(path, mib))
        return -1;
    if (!strcmp(w[2], "krffs")) {
        char *argv[] = {c->tools[NP_MKFS], path, NULL};
        return np_run(c, argv, NULL);
    }
    if (!strcmp(w[2], "ext2")) {
        char *argv[] = {
            c->tools[NP_MKE2FS], "-q", "-F", "-t", "ext2", "-b", "4096", "-I", "128", "-O",
            EXT2_PROFILE,        path, NULL};
        return np_run(c, argv, NULL);
    }
    if (!strcmp(w[2], "vfat")) {
        char *argv[] = {c->tools[NP_MFORMAT], "-i", path, "-F", "::", NULL};
        return np_run(c, argv, NULL);
    }
    fprintf(stderr, "etude prepare native: unknown image kind: %s\n", w[2]);
    return -1;
}

/* Make the root of one case that names one, unless an earlier case made the
 * same root, and give the case's line of the index `roots`. */
static int case_root(struct np_context *c, const char *output, const struct idmap *m,
                     const struct np_case *k, struct jbuf *made, struct jbuf *roots)
{
    const char *kind = NULL;
    char kinds[64], **extras = NULL;
    long mib = 64;
    size_t nextras = 0;
    int bad = 0;

    for (size_t i = 0; i < k->nfixtures; i++) {
        const char *f = k->fixtures[i];
        if (!strncmp(f, "root:", 5)) {
            const char *colon = strchr(f + 5, ':');
            size_t n = colon ? (size_t)(colon - f - 5) : strlen(f + 5);
            if (n >= sizeof(kinds) || (colon && strchr(colon + 1, ':')))
                return -1;
            memcpy(kinds, f + 5, n);
            kinds[n] = 0;
            kind = kinds;
            if (colon) {
                char *end;
                mib = strtol(colon + 1, &end, 10);
                if (*end || end == colon + 1)
                    return -1;
            }
        } else if (!strncmp(f, "stage:", 6)) {
            char *list = np_strdup(f + 6), *save = NULL;
            for (char *e = strtok_r(list, ",", &save); e; e = strtok_r(NULL, ",", &save)) {
                char **v = realloc(extras, (nextras + 1) * sizeof(*v));
                if (!v)
                    exit(1);
                extras = v;
                extras[nextras++] = np_strdup(e);
            }
            free(list);
        }
    }
    if (kind) {
        struct jbuf text = {0}, key = {0};
        struct sha256 hash;
        unsigned char sum[32];
        char hex[65], name[32], path[8192], line[64];
        bad = root_manifest(c, m, extras, nextras, &text);
        snprintf(line, sizeof(line), "%.40s %ld\n", kind, mib);
        jbuf_puts(&key, line);
        jbuf_puts(&key, text.data ? text.data : "");
        sha256_init(&hash);
        sha256_update(&hash, (const unsigned char *)key.data, key.len);
        sha256_final(&hash, sum);
        sha256_hex(sum, hex);
        snprintf(name, sizeof(name), "r-%.12s.img", hex);
        snprintf(line, sizeof(line), " %s\n", name);
        if (!bad && (!made->data || !strstr(made->data, line))) {
            snprintf(path, sizeof(path), "%s/%s", output, name);
            bad = np_root_image(c, path, kind, mib, text.data ? text.data : "");
            jbuf_puts(made, line);
        }
        jbuf_puts(roots, k->name);
        jbuf_puts(roots, line);
        jbuf_free(&text);
        jbuf_free(&key);
    }
    for (size_t i = 0; i < nextras; i++)
        free(extras[i]);
    free(extras);
    return bad ? -1 : 0;
}

/**
 * Make the images directory `output`: each image `maps` declares, the
 * private root of each case that names one, and the index `roots`, a line
 * `CASE FILE` for each such case.
 *
 * @return 0, or -1 after a diagnostic.
 */
int np_images(struct np_context *c, const char *output, const struct np_case *cases, size_t n)
{
    struct idmap m;
    struct jbuf made = {0}, roots = {0};
    char *text, *save = NULL, path[8192];

    if (mkdir(output, 0777) != 0 || (text = slurp(c, "tests/native/maps")) == NULL)
        return -1;
    int bad = 0;
    for (char *line = strtok_r(text, "\n", &save); !bad && line;
         line = strtok_r(NULL, "\n", &save)) {
        char *w[8], *copy = np_strdup(line);
        size_t count = words(copy, w, 8);
        if (count && !strcmp(w[0], "image") && maps_image(c, output, w, count)) {
            fprintf(stderr, "etude prepare native: image not made: %s\n", line);
            bad = 1;
        }
        free(copy);
        bad = bad || (c->stop && *c->stop);
    }
    free(text);
    if (bad || idmap_read(c, &m))
        return -1;
    for (size_t i = 0; !bad && i < n; i++) {
        bad = case_root(c, output, &m, &cases[i], &made, &roots);
        if (bad)
            fprintf(stderr, "etude prepare native: %s: root not made\n", cases[i].name);
        bad = bad || (c->stop && *c->stop);
    }
    idmap_free(&m);
    if (!bad)
        bad = snprintf(path, sizeof(path), "%s/roots", output) >= (int)sizeof(path) ||
              write_text(path, roots.data ? roots.data : "");
    jbuf_free(&made);
    jbuf_free(&roots);
    return bad ? -1 : 0;
}
