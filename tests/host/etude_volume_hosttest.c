/**
 * Check the tooling machine's volumes as etude-volume makes and reads them
 * (tools/etude/volume/volume.c): the FAT16 seed volume, and the tar archive
 * of the job volume in both directions.
 *
 * host-tools: tar
 * host-deps: tests/host/compat/command_host.c tools/etude/capture.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-deps: tools/etude/posix/process.c
 * host-cflags: -I tools/etude/include
 * host-deps: tools/etude/volume/volume.c
 * host-cflags: -I tools/etude/volume -Wall -Wextra -Werror
 *
 * The seed is read back by a FAT16 reader of this test's own: the boot
 * sector, the label, the count of clusters that makes the volume FAT16, the
 * two copies of the FAT, each long name with its checksum, and each file's
 * chain and bytes. A name FAT cannot hold, a missing file and files that do
 * not fit are refused.
 *
 * The job volume's archive is extracted by the host's tar(1), long names,
 * subdirectories, an empty directory and modes included, and read back by
 * etude_volume_read. The reader then reads archives that the host's tar
 * makes in two formats, GNU's and pax with GNU tar, gnutar and pax with
 * bsdtar, with bytes after their end as the input's leftovers are, each
 * entry placed by the first pair that takes it; and it refuses archives made
 * by hand here: a header whose checksum is wrong, data cut short, an archive
 * with no end, a lone zero block, a name with `..` or a leading slash, a
 * symbolic link, an entry that no pair takes and a pair that takes no entry.
 * The writer refuses files that do not fit and a FIFO.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "compat/fixdir_host.c"
#include "command_host.h"
#include "volume.h"

static int fails;

/** Count a failure when `cond` is false, and print its line and row. */
#define CHECK(cond, row)                                                                          \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            printf("etude-volume-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), #cond); \
            fails++;                                                                              \
        }                                                                                         \
    } while (0)

/* Two components of more than 100 bytes, so that a path through both needs
 * a long name in every format. */
#define LONG_DIR                                                                               \
    "a-directory-whose-name-is-longer-than-the-hundred-bytes-of-a-ustar-name-field-so-that-a-" \
    "long-name-is-needed"
#define LONG_FILE                                                                              \
    "a-file-whose-name-is-also-longer-than-one-hundred-bytes-and-is-below-the-long-directory-" \
    "in-a-subdirectory.txt"

/** Write `n` bytes of `data` to `path`; the test ends when it cannot. */
static void put_file(const char *path, const char *data, size_t n)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL || fwrite(data, 1, n, f) != n || fclose(f) != 0) {
        printf("etude-volume-test: %s cannot be written\n", path);
        exit(2);
    }
}

/** Give a file's bytes with a NUL after them, or NULL. */
static char *get_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    char *d;
    long size;

    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0 ||
        (d = malloc((size_t)size + 1)) == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(d, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(d);
        return NULL;
    }
    fclose(f);
    d[size] = '\0';
    *n = (size_t)size;
    return d;
}

/** Say whether file `path` has exactly the bytes of `want`. */
static int has(const char *path, const char *want)
{
    size_t n;
    char *d = get_file(path, &n);
    int ok = d != NULL && n == strlen(want) && memcmp(d, want, n) == 0;

    free(d);
    return ok;
}

/** Make directory `path`; the test ends when it cannot. */
static void make(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        printf("etude-volume-test: %s cannot be made\n", path);
        exit(2);
    }
}

/** Say whether `path` is a directory. */
static int is_dir(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ---- The seed volume. ---- */

/** Read a little-endian number of `n` bytes at `p`. */
static unsigned long le(const unsigned char *p, int n)
{
    unsigned long v = 0;

    while (n-- > 0)
        v = v << 8 | p[n];
    return v;
}

/* A file of the seed: its name and its bytes. */
struct seed_file {
    const char *name;
    char *data;
    size_t size;
};

/* The layout of a seed volume, as its boot sector gives it. */
struct seed_layout {
    unsigned long root, data, clusters; /* sectors, and the count of clusters */
    const unsigned char *fat1;
};

/** Check the boot sector and the FATs of seed volume `v` of `size` bytes. */
static void seed_boot(const unsigned char *v, size_t size, struct seed_layout *l)
{
    unsigned long bps = le(v + 11, 2), spc = v[13], reserved = le(v + 14, 2), fats = v[16];
    unsigned long roots = le(v + 17, 2), total = le(v + 19, 2), fat = le(v + 22, 2);

    l->root = reserved + fats * fat;
    l->data = l->root + roots * 32 / 512;
    l->clusters = spc > 0 ? (total - l->data) / spc : 0;
    l->fat1 = v + reserved * 512;
    CHECK(v[510] == 0x55 && v[511] == 0xaa, "seed: the boot sector's signature");
    CHECK(v[0] == 0xeb && v[2] == 0x90, "seed: the boot sector's jump");
    CHECK(bps == 512 && spc == 1 && reserved >= 1 && fats == 2 && v[21] == 0xf8,
          "seed: the boot sector's geometry");
    CHECK(total * 512 == size && total == ETUDE_VOLUME_SEED_SECTORS, "seed: the size");
    CHECK(l->clusters >= 4085 && l->clusters < 65525, "seed: the count of clusters makes it FAT16");
    CHECK(fat * 512 / 2 >= l->clusters + 2, "seed: the FAT covers every cluster");
    CHECK(v[38] == 0x29 && memcmp(v + 43, "CIDATA     ", 11) == 0 &&
              memcmp(v + 54, "FAT16   ", 8) == 0,
          "seed: the label and type of the boot sector");
    CHECK(memcmp(l->fat1, l->fat1 + fat * 512, fat * 512) == 0, "seed: the two FATs are equal");
    CHECK(le(l->fat1, 2) == 0xfff8 && le(l->fat1 + 2, 2) == 0xffff,
          "seed: the FAT's first entries");
}

/**
 * Take the part of a long name in directory entry `e` into `name`, of 256
 * bytes; `*parts` and `*sum` are the name's count of entries and checksum,
 * which its last part, the first entry, sets.
 */
static void seed_long(const unsigned char *e, char *name, size_t *parts, unsigned *sum)
{
    static const int at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    unsigned seq = e[0] & 0x3f;
    size_t k;
    int i;

    if (e[0] & 0x40) {
        *parts = seq;
        *sum = e[13];
        memset(name, 0, 256);
    }
    CHECK(*parts > 0 && seq >= 1 && seq <= *parts && e[13] == *sum,
          "seed: a long name's entries are in order with one checksum");
    for (i = 0; i < 13 && seq >= 1; i++) {
        unsigned long u = le(e + at[i], 2);

        k = 13 * (seq - 1) + (size_t)i;
        if (u != 0 && u != 0xffff && k < 255)
            name[k] = (char)u;
    }
}

/** Require file `f` to have its bytes in the chain from `cluster`. */
static void seed_chain(const unsigned char *v, const struct seed_layout *l, unsigned long cluster,
                       const struct seed_file *f)
{
    unsigned long left, c;

    CHECK((f->size == 0) == (cluster == 0), "seed: an empty file has no cluster");
    for (left = f->size, c = cluster; left > 0;) {
        size_t n = left < 512 ? left : 512;

        if (c < 2 || c >= l->clusters + 2) {
            CHECK(c >= 2 && c < l->clusters + 2, f->name);
            break;
        }
        CHECK(memcmp(v + (l->data + c - 2) * 512, f->data + (f->size - left), n) == 0, f->name);
        left -= n;
        c = le(l->fat1 + 2 * c, 2);
        CHECK(left > 0 ? c < 0xfff8 : c >= 0xfff8, "seed: a chain ends at the file's end");
    }
}

/**
 * Read seed volume `v` of `size` bytes as FAT16 and require it to have the
 * label CIDATA and exactly the files `want`, in that order.
 */
static void read_seed(const unsigned char *v, size_t size, const struct seed_file *want,
                      size_t nwant)
{
    struct seed_layout l;
    const unsigned char *e;
    char name[256];
    size_t found = 0, parts = 0;
    unsigned sum = 0;
    int i;

    seed_boot(v, size, &l);
    if (fails > 0)
        return;
    e = v + l.root * 512;
    CHECK(memcmp(e, "CIDATA     ", 11) == 0 && e[11] == 0x08, "seed: the label entry");
    for (e += 32; e < v + l.data * 512 && e[0] != 0; e += 32) {
        unsigned char s = 0;

        if (e[11] == 0x0f) {
            seed_long(e, name, &parts, &sum);
            continue;
        }
        for (i = 0; i < 11; i++)
            s = (unsigned char)(((s & 1) << 7) + (s >> 1) + e[i]);
        CHECK(parts > 0 && s == sum, "seed: the long name's checksum is its short name's");
        CHECK(e[11] == 0x20, "seed: a file's attributes");
        if (found >= nwant) {
            CHECK(found < nwant, "seed: more files than were written");
            return;
        }
        CHECK(strcmp(name, want[found].name) == 0, want[found].name);
        CHECK(le(e + 28, 4) == want[found].size, want[found].name);
        seed_chain(v, &l, le(e + 26, 2), &want[found]);
        parts = 0;
        found++;
    }
    CHECK(found == nwant, "seed: every file is in the root directory");
}

/** The seed row: a volume of the recipe's names, and three refusals. */
static void row_seed(void)
{
    static const char *const names[] = {
        "user-data",     "meta-data",         "provision.sh", "empty",
        "thirteen-char", "etude-job.service", "tools",        NULL};
    struct seed_file want[7];
    char dir[512], path[600], why[512], img[512], *big;
    const char *bad[] = {"a/b", NULL}, *missing[] = {"user-data", "absent", NULL};
    unsigned char *v;
    size_t i, j, n;

    fix_format(dir, sizeof(dir), "%s/seed", fix_dir());
    fix_format(img, sizeof(img), "%s/seed.img", fix_dir());
    make(dir);
    for (i = 0; names[i] != NULL; i++) {
        want[i].name = names[i];
        want[i].size = strcmp(names[i], "empty") == 0          ? 0
                       : strcmp(names[i], "provision.sh") == 0 ? 3000
                                                               : 20 + i;
        want[i].data = malloc(want[i].size + 1);
        for (j = 0; j < want[i].size; j++)
            want[i].data[j] = (char)('a' + (j * 7 + i) % 26);
        fix_format(path, sizeof(path), "%s/%s", dir, names[i]);
        put_file(path, want[i].data, want[i].size);
    }
    CHECK(etude_volume_seed(img, "CIDATA", dir, names, why, sizeof(why)) == 0, why);
    v = (unsigned char *)get_file(img, &n);
    CHECK(v != NULL, "seed: the volume is written");
    if (v != NULL)
        read_seed(v, n, want, 7);
    free(v);
    for (i = 0; i < 7; i++)
        free(want[i].data);

    why[0] = '\0';
    CHECK(etude_volume_seed(img, "CIDATA", dir, bad, why, sizeof(why)) != 0 &&
              strstr(why, "cannot be a name") != NULL,
          "seed: a name with a slash is refused");
    why[0] = '\0';
    CHECK(etude_volume_seed(img, "CIDATA", dir, missing, why, sizeof(why)) != 0 &&
              strstr(why, "absent cannot be read") != NULL,
          "seed: a missing file is refused");
    n = (size_t)9 << 20;
    big = calloc(1, n);
    fix_format(path, sizeof(path), "%s/user-data", dir);
    put_file(path, big, n);
    free(big);
    why[0] = '\0';
    CHECK(etude_volume_seed(img, "CIDATA", dir, names, why, sizeof(why)) != 0 &&
              strstr(why, "does not fit") != NULL,
          "seed: files that do not fit are refused");
}

/* ---- The job volume. ---- */

/** Make the fixtures' tree under `dir`: long names, an empty directory. */
static void make_tree(const char *dir)
{
    char path[1024];

    make(dir);
    fix_format(path, sizeof(path), "%s/%s", dir, LONG_DIR);
    make(path);
    fix_format(path, sizeof(path), "%s/%s/sub", dir, LONG_DIR);
    make(path);
    fix_format(path, sizeof(path), "%s/%s/sub/%s", dir, LONG_DIR, LONG_FILE);
    put_file(path, "deep\n", 5);
    fix_format(path, sizeof(path), "%s/empty", dir);
    put_file(path, "", 0);
    fix_format(path, sizeof(path), "%s/emptydir", dir);
    make(path);
    fix_format(path, sizeof(path), "%s/run.sh", dir);
    put_file(path, "echo run\n", 9);
    chmod(path, 0755);
}

/**
 * Require `dir` to have the tree make_tree makes; with `modes`, files that
 * give their owner execution as they did in the tree.
 */
static void has_tree(const char *dir, int modes, const char *row)
{
    char path[1024];
    struct stat st;

    fix_format(path, sizeof(path), "%s/%s/sub/%s", dir, LONG_DIR, LONG_FILE);
    CHECK(has(path, "deep\n"), row);
    CHECK(stat(path, &st) == 0 && (!modes || !(st.st_mode & S_IXUSR)), row);
    fix_format(path, sizeof(path), "%s/empty", dir);
    CHECK(has(path, ""), row);
    fix_format(path, sizeof(path), "%s/emptydir", dir);
    CHECK(is_dir(path), row);
    fix_format(path, sizeof(path), "%s/run.sh", dir);
    CHECK(has(path, "echo run\n"), row);
    CHECK(stat(path, &st) == 0 && (!modes || (st.st_mode & S_IXUSR)), row);
}

/* The pairs that place a job's results, as `etude provision tooling` gives
 * them: the job's status and output in `dir`, out/tree in `tree`, and the
 * rest of out/ in `outdir`. */
struct results {
    char status[600], out[600], err[600], tree[600], outdir[600];
    const char *map[13];
};

/** Make the pairs of `r` for the directories `dir`, `tree` and `outdir`. */
static const char *const *results(struct results *r, const char *dir, const char *tree,
                                  const char *outdir)
{
    fix_format(r->status, sizeof(r->status), "%s/status", dir);
    fix_format(r->out, sizeof(r->out), "%s/stdout", dir);
    fix_format(r->err, sizeof(r->err), "%s/stderr", dir);
    fix_format(r->tree, sizeof(r->tree), "%s", tree);
    fix_format(r->outdir, sizeof(r->outdir), "%s", outdir);
    r->map[0] = "status";
    r->map[1] = r->status;
    r->map[2] = "stdout";
    r->map[3] = r->out;
    r->map[4] = "stderr";
    r->map[5] = r->err;
    r->map[6] = "out/tree";
    r->map[7] = r->tree;
    r->map[8] = "out";
    r->map[9] = r->outdir;
    r->map[10] = NULL;
    return r->map;
}

/** The writer's row: the host's tar extracts the volume, and the reader too. */
static void row_writer(void)
{
    char tree[512], img[512], path[1024], why[512], job[1024], outputs[1024], in[1024];
    const char *map[7];
    struct etude_volume *v;
    struct stat st;

    fix_format(tree, sizeof(tree), "%s/intree", fix_dir());
    fix_format(img, sizeof(img), "%s/job.img", fix_dir());
    make_tree(tree);
    fix_format(path, sizeof(path), "%s/job.sh", fix_dir());
    put_file(path, "cat note\n", 9);
    v = etude_volume_create(img, (uint64_t)4 << 20, why, sizeof(why));
    CHECK(v != NULL, why);
    if (v == NULL)
        return;
    etude_volume_add(v, "job.sh", path);
    etude_volume_text(v, "outputs", "result.txt\ntree\n", 16);
    etude_volume_dir(v, "in");
    etude_volume_add(v, "in/tree", tree);
    CHECK(etude_volume_close(v) == 0, why);
    CHECK(stat(img, &st) == 0 && st.st_size == (off_t)4 << 20, "writer: the disk has its size");

    fix_format(path, sizeof(path), "%s/x", fix_dir());
    make(path);
    {
        const char *argv[] = {host_tool_path("tar"), "-x", "-f", img, "-C", path, NULL};
        CHECK(host_command(argv, NULL, NULL, NULL, 0) == 0,
              "writer: the host's tar extracts the volume");
    }
    fix_format(path, sizeof(path), "%s/x/job.sh", fix_dir());
    CHECK(has(path, "cat note\n"), "writer: job.sh");
    fix_format(path, sizeof(path), "%s/x/outputs", fix_dir());
    CHECK(has(path, "result.txt\ntree\n"), "writer: outputs");
    fix_format(path, sizeof(path), "%s/x/in/tree", fix_dir());
    has_tree(path, 1, "writer: the tree, by the host's tar");

    fix_format(job, sizeof(job), "%s/r/job.sh", fix_dir());
    fix_format(outputs, sizeof(outputs), "%s/r/outputs", fix_dir());
    fix_format(in, sizeof(in), "%s/r/in", fix_dir());
    map[0] = "job.sh";
    map[1] = job;
    map[2] = "outputs";
    map[3] = outputs;
    map[4] = "in";
    map[5] = in;
    map[6] = NULL;
    CHECK(etude_volume_read(img, map, why, sizeof(why)) == 0, why);
    CHECK(has(job, "cat note\n"), "writer: job.sh, read back");
    fix_format(path, sizeof(path), "%s/tree", in);
    has_tree(path, 0, "writer: the tree, read back");

    why[0] = '\0';
    if ((v = etude_volume_create(img, 4096, why, sizeof(why))) != NULL) {
        etude_volume_add(v, "in/tree", tree);
        CHECK(etude_volume_close(v) != 0 && strstr(why, "do not fit") != NULL,
              "writer: files that do not fit are refused");
    }
    fix_format(path, sizeof(path), "%s/fifo", fix_dir());
    remove(path);
    CHECK(mkfifo(path, 0600) == 0, "writer: a FIFO is made");
    why[0] = '\0';
    if ((v = etude_volume_create(img, (uint64_t)1 << 20, why, sizeof(why))) != NULL) {
        etude_volume_add(v, "in/fifo", path);
        CHECK(etude_volume_close(v) != 0 && strstr(why, "neither a file nor a directory") != NULL,
              "writer: a FIFO is refused");
    }
}

/** Read the results in `stage` archived by the host's tar with `format`. */
static void read_format(const char *stage, const char *format)
{
    char arch[512], img[512], out[512], dir[512], tree[512], path[1024], why[512];
    char row[128], *a;
    struct results r;
    size_t n;
    FILE *f;

    snprintf(row, sizeof(row), "reader: tar %s", format);
    fix_format(arch, sizeof(arch), "%s/results.tar", fix_dir());
    fix_format(img, sizeof(img), "%s/results.img", fix_dir());
    {
        const char *argv[] = {host_tool_path("tar"),
                              "-c",
                              "-f",
                              arch,
                              format,
                              "-C",
                              stage,
                              "out",
                              "stdout",
                              "stderr",
                              "status",
                              NULL};
        CHECK(host_command(argv, NULL, NULL, NULL, 0) == 0, row);
    }
    a = get_file(arch, &n);
    CHECK(a != NULL, row);
    if (a == NULL)
        return;
    f = fopen(img, "wb");
    CHECK(f != NULL, row);
    if (f != NULL) {
        fwrite(a, 1, n, f);
        for (n = 0; n < 8192; n++)
            fputc('X', f);
        fclose(f);
    }
    free(a);
    fix_format(dir, sizeof(dir), "%s/rd", fix_dir());
    CHECK(fix_remove_tree(dir) == 0, row);
    make(dir);
    fix_format(out, sizeof(out), "%s/rd/out", fix_dir());
    make(out);
    fix_format(tree, sizeof(tree), "%s/rd/elsewhere/tree", fix_dir());
    why[0] = '\0';
    CHECK(etude_volume_read(img, results(&r, dir, tree, out), why, sizeof(why)) == 0, why);
    has_tree(tree, 0, row);
    fix_format(path, sizeof(path), "%s/tree", out);
    CHECK(!is_dir(path), "reader: an entry goes by the first pair that takes it");
    fix_format(path, sizeof(path), "%s/result.txt", out);
    CHECK(has(path, "made\n"), row);
    fix_format(path, sizeof(path), "%s/stdout", dir);
    CHECK(has(path, "the output\n"), row);
    fix_format(path, sizeof(path), "%s/stderr", dir);
    CHECK(has(path, ""), row);
    fix_format(path, sizeof(path), "%s/status", dir);
    CHECK(has(path, "3\n"), row);
}

/** The reader's row: archives in the host's tar's formats. */
static void row_reader(void)
{
    static const char *const gnu[] = {"--format=gnu", "--format=pax", NULL};
    static const char *const bsd[] = {"--format=gnutar", "--format=pax", NULL};
    const char *const *formats;
    char stage[512], path[1024];

    fix_format(stage, sizeof(stage), "%s/stage", fix_dir());
    make(stage);
    fix_format(path, sizeof(path), "%s/out", stage);
    make(path);
    fix_format(path, sizeof(path), "%s/out/tree", stage);
    make_tree(path);
    fix_format(path, sizeof(path), "%s/out/result.txt", stage);
    put_file(path, "made\n", 5);
    fix_format(path, sizeof(path), "%s/stdout", stage);
    put_file(path, "the output\n", 11);
    fix_format(path, sizeof(path), "%s/stderr", stage);
    put_file(path, "", 0);
    fix_format(path, sizeof(path), "%s/status", stage);
    put_file(path, "3\n", 2);
    {
        char version[4096];
        const char *argv[] = {host_tool_path("tar"), "--version", NULL};
        int status = host_command(argv, NULL, NULL, version, sizeof version);
        CHECK(status == 0, "reader: the prepared tar identifies its formats");
        formats = status == 0 && strstr(version, "GNU tar") != NULL ? gnu : bsd;
    }
    for (; *formats != NULL; formats++)
        read_format(stage, *formats);
}

/* An archive made by hand, block by block. */
struct craft {
    unsigned char b[64 * 512];
    size_t n;
};

/** Append a ustar header of `name`, `type` and `size`, with its checksum. */
static void craft_header(struct craft *c, const char *name, char type, size_t size)
{
    unsigned char *h = c->b + c->n;
    unsigned long sum = 0;
    size_t i;

    memset(h, 0, 512);
    memcpy(h, name, strlen(name));
    snprintf((char *)h + 100, 8, "%07o", 0644);
    snprintf((char *)h + 108, 8, "%07o", 0);
    snprintf((char *)h + 116, 8, "%07o", 0);
    snprintf((char *)h + 124, 12, "%011lo", (unsigned long)size);
    snprintf((char *)h + 136, 12, "%011o", 0);
    h[156] = (unsigned char)type;
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    memset(h + 148, ' ', 8);
    for (i = 0; i < 512; i++)
        sum += h[i];
    snprintf((char *)h + 148, 8, "%06lo", sum);
    c->n += 512;
}

/** Append `size` bytes of data, padded to a block. */
static void craft_data(struct craft *c, size_t size)
{
    memset(c->b + c->n, 'd', size);
    c->n += (size + 511) / 512 * 512;
}

/** Append the end of an archive, two zero blocks. */
static void craft_end(struct craft *c)
{
    memset(c->b + c->n, 0, 1024);
    c->n += 1024;
}

/**
 * Write the first `n` bytes of `c` as a volume, read it with `pairs`, or,
 * when that is NULL, with pairs that place status, stdout and out/, and
 * require the reader to refuse it with a reason that has `reason`.
 */
static void refused(const struct craft *c, size_t n, const char *const *pairs, const char *reason,
                    const char *row)
{
    char img[512], dir[512], out[512], status[600], so[600], why[512];
    const char *map[7];

    fix_format(img, sizeof(img), "%s/crafted.img", fix_dir());
    fix_format(dir, sizeof(dir), "%s/cr", fix_dir());
    fix_format(out, sizeof(out), "%s/cr/out", fix_dir());
    make(dir);
    make(out);
    put_file(img, (const char *)c->b, n);
    fix_format(status, sizeof(status), "%s/status", dir);
    fix_format(so, sizeof(so), "%s/stdout", dir);
    map[0] = "status";
    map[1] = status;
    map[2] = "stdout";
    map[3] = so;
    map[4] = "out";
    map[5] = out;
    map[6] = NULL;
    why[0] = '\0';
    CHECK(etude_volume_read(img, pairs != NULL ? pairs : map, why, sizeof(why)) != 0, row);
    if (strstr(why, reason) == NULL) {
        printf("etude-volume-test: fail: %s: the reason is \"%s\", not \"%s\"\n", row, why, reason);
        fails++;
    }
}

/** The refusals' row: archives made by hand that the reader must refuse. */
static void row_refusals(void)
{
    static struct craft c;
    char img[512], so[600], status[600], why[512];
    const char *map[5];
    size_t at;

    c.n = 0;
    craft_header(&c, "stdout", '0', 10);
    craft_data(&c, 10);
    craft_end(&c);
    fix_format(img, sizeof(img), "%s/crafted.img", fix_dir());
    fix_format(so, sizeof(so), "%s/cr/stdout", fix_dir());
    fix_format(status, sizeof(status), "%s/cr/status", fix_dir());
    put_file(img, (const char *)c.b, c.n);
    map[0] = "stdout";
    map[1] = so;
    map[2] = NULL;
    CHECK(etude_volume_read(img, map, why, sizeof(why)) == 0, why);
    CHECK(has(so, "dddddddddd"), "refusals: the crafted archive is read");

    c.n = 0;
    craft_header(&c, "std", '0', 2);
    craft_data(&c, 2);
    craft_header(&c, "stdout", '0', 10);
    craft_data(&c, 10);
    craft_end(&c);
    put_file(img, (const char *)c.b, c.n);
    remove(so);
    map[0] = "std";
    map[1] = status;
    map[2] = "stdout";
    map[3] = so;
    map[4] = NULL;
    CHECK(etude_volume_read(img, map, why, sizeof(why)) == 0 && has(so, "dddddddddd"),
          "refusals: a pair takes a name only up to a slash");

    c.n = 0;
    craft_header(&c, "stdout", '0', 10);
    craft_data(&c, 10);
    craft_end(&c);
    put_file(img, (const char *)c.b, c.n);
    map[0] = "stdout";
    map[1] = so;
    map[2] = "status";
    map[3] = status;
    map[4] = NULL;
    refused(&c, c.n, map, "the volume has no status", "refusals: a pair that takes no entry");

    c.b[0] = 'S';
    refused(&c, c.n, NULL, "fails its checksum", "refusals: a damaged header");

    c.n = 0;
    craft_header(&c, "status", '0', 2);
    craft_data(&c, 2);
    craft_header(&c, "stdout", '0', 2000);
    at = c.n;
    craft_data(&c, 2000);
    craft_end(&c);
    refused(&c, at + 700, NULL, "inside stdout: it is truncated", "refusals: data cut short");
    refused(&c, at + 2048, NULL, "before its archive does: it is truncated",
            "refusals: an archive with no end");

    c.n = 0;
    craft_header(&c, "status", '0', 2);
    craft_data(&c, 2);
    memset(c.b + c.n, 0, 512);
    c.n += 512;
    craft_header(&c, "stdout", '0', 2);
    craft_data(&c, 2);
    craft_end(&c);
    refused(&c, c.n, NULL, "lone zero block", "refusals: a lone zero block");

    c.n = 0;
    craft_header(&c, "out/../../escaped", '0', 2);
    craft_data(&c, 2);
    craft_end(&c);
    refused(&c, c.n, NULL, "is not inside it", "refusals: a name with ..");

    c.n = 0;
    craft_header(&c, "/etc/escaped", '0', 2);
    craft_data(&c, 2);
    craft_end(&c);
    refused(&c, c.n, NULL, "is not inside it", "refusals: an absolute name");

    c.n = 0;
    craft_header(&c, "out/link", '2', 0);
    craft_end(&c);
    refused(&c, c.n, NULL, "out/link is a link", "refusals: a symbolic link");

    c.n = 0;
    craft_header(&c, "elsewhere", '0', 2);
    craft_data(&c, 2);
    craft_end(&c);
    refused(&c, c.n, NULL, "should not: elsewhere", "refusals: an entry that no pair takes");
}

/* Extract one named file while checking the rest of the archive. */
static void row_selected(void)
{
    char why[256];
    size_t n = 0;
    char *data;
    struct craft c;
    c.n = 0;
    craft_header(&c, "other", '0', 4);
    craft_data(&c, 4);
    craft_header(&c, "ranges.txt", '0', 6);
    craft_data(&c, 6);
    craft_end(&c);
    put_file(fix_path("selected.tar"), (const char *)c.b, c.n);
    CHECK(etude_volume_file(fix_path("selected.tar"), "ranges.txt", fix_path("ranges"), why, sizeof(why)) == 0,
          "one regular selected entry");
    data = get_file(fix_path("ranges"), &n);
    CHECK(data != NULL && n == 6 && access(fix_path("other"), F_OK) != 0,
          "other archive entries are not extracted");
    free(data);
    CHECK(etude_volume_file(fix_path("selected.tar"), "absent", fix_path("missing"), why, sizeof(why)) != 0,
          "a missing selected entry fails");
    c.n -= 1024;
    craft_header(&c, "ranges.txt", '0', 1);
    craft_data(&c, 1);
    craft_end(&c);
    put_file(fix_path("selected.tar"), (const char *)c.b, c.n);
    CHECK(etude_volume_file(fix_path("selected.tar"), "ranges.txt", fix_path("ranges"), why, sizeof(why)) != 0,
          "a repeated selected entry fails");
}

int main(void)
{
    row_seed();
    row_writer();
    row_reader();
    row_refusals();
    row_selected();
    if (fails > 0) {
        printf("etude-volume-test: result: fail: %d checks\n", fails);
        return 1;
    }
    fix_passed();
    printf("etude-volume-test: result: pass\n");
    return 0;
}
