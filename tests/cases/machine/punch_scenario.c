/*
 * punch_scenario.c: the scenario of machine.punch, which frees large files on
 * an ext2 volume while other processes allocate, write and sync on a machine
 * of four processors, and judges the volume after it without that kernel:
 * e2fsck -fn, and each kept file hashed from the disk image by the reader
 * here, which shares no code with the driver.
 */
#ifdef _WIN32
#include "../../../tools/etude/win/files.h"
#endif
#include "../../../tools/image/host_io.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* The disk and its one ext2 volume, as the host's tools take them. */
static const char disk[] = "punch";
#define PART_OFFSET 1048576ull
#define OFFSET_TEXT "1048576"
#define VOLUME_SIZE "2048M"
#define PART_RANGE  "2048:4194304:83"

/* The rounds of punchmusl on the writeback mount. */
#define ROUNDS "6"

/* The features of the volume, as machine.large's ext2 volume has them. */
static char features[] = "sparse_super,large_file,filetype,^dir_index,resize_inode,"
                         "^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,"
                         "^huge_file,^dir_nlink,^extra_isize";

#define KEEPS_MAX 2048
#define BLOCK_MAX 65536u
#define FNV_BASIS 14695981039346656037ull
#define FNV_PRIME 1099511628211ull

/* A file punchmusl says it kept. */
struct keep {
    char name[32];
    uint64_t size;
    uint64_t hash;
};

/* The volume on the disk image, for the reader. */
struct volume {
    FILE *file;
    uint32_t block;      /* bytes in a block */
    uint32_t per_group;  /* inodes in a group */
    uint32_t inode_size; /* bytes in an inode */
    uint32_t first_data; /* the block of the first group */
    uint32_t per_blocks; /* blocks in a group */
};

/* An inode's fields that the reader needs. */
struct inode {
    uint64_t size;
    uint32_t pointers[15];
};

/* A walk of a file's blocks: `leaf` is called for each pointer that is not 0,
 * in the order of the file, with the block's index and number. */
struct walk {
    struct volume *v;
    int (*leaf)(struct walk *w, uint64_t index, uint32_t number);
    uint64_t size;   /* the file's length */
    uint64_t hash;   /* a hash so far */
    uint32_t first;  /* the first block with bytes, or 0 */
    const char *key; /* the name a directory walk looks for */
    uint32_t found;  /* its inode, or 0 */
};

/** The little-endian 32-bit word at `p`. */
static uint32_t word_at(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** Read `n` bytes at byte `at` of the volume; 1 when they came. */
static int read_at(const struct volume *v, uint64_t at, void *buf, size_t n)
{
    return host_image_seek(v->file, (int64_t)(PART_OFFSET + at), SEEK_SET) == 0 &&
           fread(buf, 1, n, v->file) == n;
}

/** Open the volume of the disk at `path`, read and written when `mode` says
 * so; 1 when its superblock is one this reader reads. */
static int volume_open(struct volume *v, const char *path, const char *mode)
{
    unsigned char sb[256];

    v->file = host_image_open(path, mode, NULL);
    if (v->file == NULL)
        return 0;
    if (!read_at(v, 1024, sb, sizeof(sb)) || sb[56] != 0x53 || sb[57] != 0xEF ||
        word_at(sb + 24) > 6) {
        fclose(v->file);
        v->file = NULL;
        return 0;
    }
    v->block = 1024u << word_at(sb + 24);
    v->per_blocks = word_at(sb + 32);
    v->per_group = word_at(sb + 40);
    v->first_data = word_at(sb + 20);
    v->inode_size = (uint32_t)sb[88] | ((uint32_t)sb[89] << 8);
    return v->block <= BLOCK_MAX && v->per_group != 0 && v->inode_size != 0;
}

/** The byte of the volume where the descriptor of group `g` begins. */
static uint64_t descriptor_at(const struct volume *v, uint32_t g)
{
    return ((uint64_t)v->first_data + 1) * v->block + (uint64_t)g * 32;
}

/** Read inode `ino`; 1 when it came. */
static int inode_read(const struct volume *v, uint32_t ino, struct inode *out)
{
    uint32_t g = (ino - 1) / v->per_group, i;
    uint64_t at = (uint64_t)((ino - 1) % v->per_group) * v->inode_size;
    unsigned char gd[32], raw[128];

    if (!read_at(v, descriptor_at(v, g), gd, sizeof(gd)) ||
        !read_at(v, (uint64_t)word_at(gd + 8) * v->block + at, raw, sizeof(raw)))
        return 0;
    out->size = (uint64_t)word_at(raw + 4) | ((uint64_t)word_at(raw + 108) << 32);
    for (i = 0; i < 15; i++)
        out->pointers[i] = word_at(raw + 40 + i * 4);
    return 1;
}

/**
 * Walk the tree under pointer block `number`, of `level` (1 for one that
 * names data), whose first data block is `index`. @return 1, or 0 when a
 * read failed or `leaf` said to stop.
 */
static int walk_tree(struct walk *w, uint32_t number, int level, uint64_t index)
{
    struct volume *v = w->v;
    uint32_t per = v->block / 4, i;
    uint64_t span = 1;
    unsigned char *slots;
    int l, ok = 1;

    for (l = 1; l < level; l++)
        span *= per;
    slots = malloc(v->block);
    if (slots == NULL || !read_at(v, (uint64_t)number * v->block, slots, v->block)) {
        free(slots);
        return 0;
    }
    for (i = 0; ok && i < per; i++) {
        uint32_t child = word_at(slots + i * 4);
        uint64_t at = index + i * span;

        if (child != 0)
            ok = level == 1 ? w->leaf(w, at, child) : walk_tree(w, child, level - 1, at);
    }
    free(slots);
    return ok;
}

/** Walk every block of the file `n`. @return 1, or 0 as `walk_tree`. */
static int walk_file(struct walk *w, const struct inode *n)
{
    uint64_t per = w->v->block / 4, index = 12;
    int i, level;

    for (i = 0; i < 12; i++)
        if (n->pointers[i] != 0 && !w->leaf(w, (uint64_t)i, n->pointers[i]))
            return 0;
    for (level = 1; level <= 3; level++) {
        uint64_t span = per, l;

        if (n->pointers[11 + level] != 0 && !walk_tree(w, n->pointers[11 + level], level, index))
            return 0;
        for (l = 1; l < (uint64_t)level; l++)
            span *= per;
        index += span;
    }
    return 1;
}

/** Add `n` bytes at `p` to the hash `h`. */
static uint64_t hash_bytes(uint64_t h, const unsigned char *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * FNV_PRIME;
    return h;
}

/** Add block `index`, of `n` bytes at `data`, to a file's hash: its number
 * in eight bytes and its bytes, or nothing when they are all zeros. */
static uint64_t hash_block(uint64_t h, uint64_t index, const unsigned char *data, size_t n)
{
    unsigned char number[8];
    size_t i;
    int b;

    for (i = 0; i < n && data[i] == 0; i++)
        ;
    if (i == n)
        return h;
    for (b = 0; b < 8; b++)
        number[b] = (unsigned char)(index >> (b * 8));
    return hash_bytes(hash_bytes(h, number, 8), data, n);
}

/** A walk's leaf: add the block to the hash. A block wholly past the end of
 * the file is a fault of the tree, and stops the walk. */
static int hash_leaf(struct walk *w, uint64_t index, uint32_t number)
{
    uint64_t at = index * w->v->block;
    unsigned char *data;
    size_t n;
    int ok;

    if (at >= w->size)
        return 0;
    n = w->size - at < w->v->block ? (size_t)(w->size - at) : w->v->block;
    data = malloc(w->v->block);
    ok = data != NULL && read_at(w->v, (uint64_t)number * w->v->block, data, n);
    if (ok) {
        w->hash = hash_block(w->hash, index, data, n);
        if (w->first == 0)
            w->first = number;
    }
    free(data);
    return ok;
}

/** A walk's leaf: look for `key` in the directory block `number`. */
static int entry_leaf(struct walk *w, uint64_t index, uint32_t number)
{
    size_t len = strlen(w->key);
    unsigned char *data = malloc(w->v->block);
    uint32_t at = 0;
    int ok;

    (void)index;
    ok = data != NULL && read_at(w->v, (uint64_t)number * w->v->block, data, w->v->block);
    while (ok && at + 8 <= w->v->block) {
        uint32_t ino = word_at(data + at);
        uint32_t size = (uint32_t)data[at + 4] | ((uint32_t)data[at + 5] << 8);

        if (size < 8 || at + size > w->v->block) {
            ok = 0;
        } else if (ino != 0 && data[at + 6] == len && memcmp(data + at + 8, w->key, len) == 0) {
            w->found = ino;
        }
        at += size;
    }
    free(data);
    return ok;
}

/** The inode of `name` in the directory inode `dir`, or 0. */
static uint32_t lookup(struct volume *v, uint32_t dir, const char *name)
{
    struct walk w;
    struct inode n;

    memset(&w, 0, sizeof(w));
    w.v = v;
    w.leaf = entry_leaf;
    w.key = name;
    if (!inode_read(v, dir, &n) || !walk_file(&w, &n))
        return 0;
    return w.found;
}

/**
 * Hash the kept file `k` of directory /d from the image, and give the number
 * of its first block with bytes in `*first` when that is not NULL.
 *
 * @return 1 when its length and hash are the ones punchmusl printed.
 */
static int hash_keep(struct volume *v, const struct keep *k, uint32_t *first)
{
    uint32_t dir = lookup(v, 2, "d");
    uint32_t ino = dir != 0 ? lookup(v, dir, k->name) : 0;
    struct inode n;
    struct walk w;

    if (ino == 0 || !inode_read(v, ino, &n) || n.size != k->size)
        return 0;
    memset(&w, 0, sizeof(w));
    w.v = v;
    w.leaf = hash_leaf;
    w.size = n.size;
    w.hash = FNV_BASIS;
    if (!walk_file(&w, &n))
        return 0;
    if (first != NULL)
        *first = w.first;
    return w.hash == k->hash;
}

/**
 * Read each `PUNCH KEEP NAME SIZE HASH` line of `w` into `keeps`, as many as
 * KEEPS_MAX.
 *
 * @return how many, or -1 when `PUNCH KEPT N` does not say that many.
 */
static int parse_keeps(const char *w, struct keep *keeps)
{
    const char *p = w, *said = strstr(w, "PUNCH KEPT ");
    int n = 0;

    while ((p = strstr(p, "PUNCH KEEP ")) != NULL && n < KEEPS_MAX) {
        unsigned long long size, hash;

        if (sscanf(p, "PUNCH KEEP %31s %llu %llx", keeps[n].name, &size, &hash) != 3)
            return -1;
        keeps[n].size = size;
        keeps[n].hash = hash;
        n++;
        p += 11;
    }
    return said != NULL && atoi(said + 11) == n ? n : -1;
}

/** Give the path of the attempt's disk, or NULL, after which the scenario
 * cannot go on. */
static const char *disk_path(struct lc *l)
{
    const char *copy = etude_private_copy(l->c, disk);

    if (copy == NULL)
        lc_halt(l, ETUDE_REFUSED, "the empty disk");
    return copy;
}

/** Run host tool `argv[0]`, keeping its output as evidence `output`.
 *
 * @return the status it exited with, -1 for any other end, or -2 when it did
 *         not run, after which the scenario cannot go on. */
static int tool(struct lc *l, char *argv[], const char *output)
{
    struct etude_termination end;
    enum etude_status st;

    if (!lc_ok(l))
        return -2;
    st = etude_host(l->c, argv, output, &end);
    if (st != ETUDE_OK) {
        lc_halt(l, st, argv[0]);
        return -2;
    }
    return end.end == ETUDE_END_EXITED ? end.value : -1;
}

/** Make the disk's table and the ext2 volume in it. Assertion `volume`. */
static void make_volume(struct lc *l, char *path)
{
    char *table[] = {"mkmbr", "--lba", "--id", "1a26e002", path, PART_RANGE, NULL};
    char *make[] = {"mke2fs",
                    "-q",
                    "-F",
                    "-t",
                    "ext2",
                    "-b",
                    "4096",
                    "-I",
                    "128",
                    "-O",
                    features,
                    "-E",
                    "offset=" OFFSET_TEXT,
                    path,
                    VOLUME_SIZE,
                    NULL};
    int made;

    if (!lc_step(l, "volume"))
        return;
    made = tool(l, table, "mkmbr") == 0 && tool(l, make, "mke2fs") == 0;
    if (!l->stopped)
        lc_assert(l, "volume", made, "mkmbr or mke2fs did not exit 0");
    if (!made)
        lc_halt(l, ETUDE_FAILED, "the volume");
}

/**
 * Wait after `*at` for the line that hands the debug shell back, and report
 * each `PUNCH ROUND` line before it as one more unit of the witness `round`,
 * so that the watchdog's gap bounds a round of punchmusl instead of both
 * phases. Leaves `*at` at the start of the line that hands the shell back.
 *
 * @return 1, or 0 when a wait or a read of the console failed, after which
 *         the scenario cannot go on.
 */
static int wait_rounds(struct lc *l, struct etude_case_machine *m, uint64_t *at)
{
    uint64_t start, seen = 0;
    char *line;
    int back;

    do {
        start = *at;
        if (!lc_wait(l, m, "\n", at, NULL) || (line = lc_console(l, "stress", start, *at)) == NULL)
            return 0;
        if (strstr(line, "PUNCH ROUND ") != NULL)
            etude_progress(l->c, "round", ++seen);
        back = strstr(line, ", returning to kernel shell") != NULL;
        free(line);
    } while (!back);
    *at = start;
    return 1;
}

/**
 * Boot the machine, run punchmusl and end the machine. Assertions `exit` and
 * `said-ok`, and the lines it printed for a person.
 *
 * @return the window, which the caller frees; or NULL when the scenario
 *         cannot go on.
 */
static char *stress(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at, from;
    const char *failed, *total;
    char why[200], *w;

    if ((m = lc_shell(l, "stress", "stress", &at)) == NULL)
        return NULL;
    from = at;
    if (!lc_type(l, m, "exec /musl/punchmusl /dev/disk0p1 " ROUNDS "\r") ||
        !wait_rounds(l, m, &at)) {
        if (!l->stopped)
            lc_assert(l, "exit", 0, l->why);
        return NULL;
    }
    w = lc_handback(l, m, "stress", from, &at, "exit");
    if (w == NULL)
        return NULL;
    failed = strstr(w, "PUNCH FAIL");
    snprintf(why, sizeof(why), "%.*s", failed ? (int)strcspn(failed, "\r\n") : 40,
             failed ? failed : "punchmusl never says PUNCH OK");
    lc_assert(l, "said-ok", failed == NULL && strstr(w, "PUNCH OK") != NULL, why);
    if ((total = strstr(w, "PUNCH TOTAL")) != NULL)
        printf("punch: %.*s\n", (int)strcspn(total, "\r\n"), total);
    if (!lc_quit(l, m, NULL)) {
        free(w);
        return NULL;
    }
    return w;
}

/** Run e2fsck -fn on the volume, its output kept as evidence `output`.
 *
 * @return its status, or as `tool`. */
static int fsck(struct lc *l, char *at_offset, const char *output)
{
    char *argv[] = {"e2fsck", "-fn", at_offset, NULL};

    return tool(l, argv, output);
}

/** Hash every file of `keeps`, `n` of them. Assertion `hash`. */
static void hash_all(struct lc *l, const char *path, const struct keep *keeps, int n)
{
    struct volume v;
    char why[200] = "the volume did not open";
    int i, differ = 0;

    if (!lc_step(l, "hash"))
        return;
    if (n > 0 && volume_open(&v, path, "rb")) {
        for (i = 0; i < n; i++) {
            if (!hash_keep(&v, &keeps[i], NULL)) {
                printf("punch: %s differs from what the machine printed\n", keeps[i].name);
                differ++;
            }
        }
        fclose(v.file);
        snprintf(why, sizeof(why), "%d of %d kept files differ", differ, n);
        printf("punch: %d kept files hashed from the image, %d differ\n", n, differ);
    } else {
        differ = 1;
    }
    lc_assert(l, "hash", differ == 0, why);
}

/** Say whether `k` has a size debugfs's dump brings over the console. */
static int small_keep(const struct keep *k)
{
    return k->size > 0 && k->size <= 300000;
}

/** Read debugfs's dump of the kept file `k` from the volume, in the evidence
 * `name`. Assertion `debugfs`: its hash is the one printed, so the reader
 * here and debugfs read the same bytes. */
static void debugfs_one(struct lc *l, char *at_offset, const struct keep *k)
{
    char request[2200], name[] = "debugfs", r[] = "-R";
    char *argv[] = {name, r, request, at_offset, NULL};
    const char *named;
    char out[1100];
    unsigned char *data = malloc(k->size + 1);
    size_t i, got = 0;
    uint64_t h = FNV_BASIS;
    FILE *f;

    if (!lc_step(l, "debugfs") || data == NULL ||
        (named = etude_evidence(l->c, "keep.dump")) == NULL || strlen(named) >= sizeof(out)) {
        free(data);
        return;
    }
    snprintf(out, sizeof(out), "%s", named);
    snprintf(request, sizeof(request), "dump \"/d/%s\" \"%s\"", k->name, out);
    if (tool(l, argv, "dump") == -2) {
        free(data);
        return;
    }
    if ((f = fopen(out, "rb")) != NULL) {
        got = fread(data, 1, k->size + 1, f);
        fclose(f);
    }
    for (i = 0; got == k->size && i < got; i += 4096)
        h = hash_block(h, i / 4096, data + i, got - i < 4096 ? got - i : 4096);
    lc_assert(l, "debugfs", got == k->size && h == k->hash, "debugfs reads other bytes");
    free(data);
}

/** Set byte `at` of the volume to its complement; 1 when it was written. */
static int flip_byte(const struct volume *v, uint64_t at)
{
    int c;

    if (host_image_seek(v->file, (int64_t)(PART_OFFSET + at), SEEK_SET) != 0 ||
        (c = fgetc(v->file)) == EOF ||
        host_image_seek(v->file, (int64_t)(PART_OFFSET + at), SEEK_SET) != 0)
        return 0;
    return fputc(~c & 0xFF, v->file) != EOF && fflush(v->file) == 0;
}

/**
 * Show that the two judges can fail, on the disk they have passed, which
 * nothing reads after this: a byte of a kept file's data flipped makes its
 * hash differ, assertion `hash-catches`, and that block's bit in the bitmap
 * flipped makes e2fsck -fn exit other than 0, assertion `fsck-catches`.
 */
static void catches(struct lc *l, const char *path, char *at_offset, const struct keep *k)
{
    unsigned char gd[32];
    struct volume v;
    uint32_t first = 0, g, bit;
    int caught = 0, status;

    if (!lc_step(l, "catches") || !volume_open(&v, path, "r+b"))
        return;
    if (hash_keep(&v, k, &first) && first != 0 && flip_byte(&v, (uint64_t)first * v.block) &&
        !hash_keep(&v, k, NULL))
        caught = 1;
    lc_assert(l, "hash-catches", caught, "a flipped block does not change the hash");
    g = (first - v.first_data) / v.per_blocks;
    bit = (first - v.first_data) % v.per_blocks;
    caught = read_at(&v, descriptor_at(&v, g), gd, sizeof(gd)) &&
             flip_byte(&v, (uint64_t)word_at(gd) * v.block + bit / 8);
    fclose(v.file);
    status = caught ? fsck(l, at_offset, "fsck.flipped") : -2;
    if (status != -2 || !caught)
        lc_assert(l, "fsck-catches", caught && status != 0, "e2fsck passes a flipped bitmap");
}

/** The first of `n` kept files that debugfs can bring over, or `keeps`. */
static const struct keep *pick_small(const struct keep *keeps, int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (small_keep(&keeps[i]))
            return &keeps[i];
    return keeps;
}

/** Judge the volume after the machine, from what `w` printed. */
static void judge(struct lc *l, const char *path, const char *w)
{
    static struct keep keeps[KEEPS_MAX];
    char at_offset[4200];
    int n = parse_keeps(w, keeps), status;

    snprintf(at_offset, sizeof(at_offset), "%s?offset=" OFFSET_TEXT, path);
    if (!lc_step(l, "fsck"))
        return;
    status = fsck(l, at_offset, "fsck");
    if (status == -2)
        return;
    lc_assert(l, "fsck", status == 0,
              "e2fsck -fn does not exit 0; its output is the evidence fsck");
    lc_assert(l, "kept", n > 0, "the machine's PUNCH KEEP lines do not match PUNCH KEPT");
    if (n <= 0)
        return;
    hash_all(l, path, keeps, n);
    debugfs_one(l, at_offset, pick_small(keeps, n));
    catches(l, path, at_offset, pick_small(keeps, n));
}

int etude_scenario(struct etude_case *c)
{
    static const char *const ids[] = {"volume", "exit",    "said-ok",      "fsck",         "kept",
                                      "hash",   "debugfs", "hash-catches", "fsck-catches", NULL};
    const char *path;
    struct lc l;
    char *w;

    lc_begin(&l, c, ids);
    path = disk_path(&l);
    if (path != NULL) {
        make_volume(&l, (char *)path);
        if ((w = lc_ok(&l) ? stress(&l) : NULL) != NULL) {
            judge(&l, path, w);
            free(w);
        }
    }
    return lc_finish(&l);
}
