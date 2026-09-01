/*
 * fat32.c: the `vfat` type, read and written. The contract is in fat32.h
 * and fat32.Readme.md; what is here is the volume check, the FAT walk, the
 * directory reader with its long names, the node pool that gives the VFS
 * something to hold, the writer with its order of writes, and the check
 * of a volume that was not unmounted cleanly.
 *
 * Everything on the volume is reached through the buffer cache in 4 KiB
 * blocks, one buffer held at a time, so the cache's hold rules are met
 * without thought. A cluster is at most 32 KiB and a directory entry is 32
 * bytes, so a read of either is a run of block copies.
 */

#include "fat32.h"

#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../panic.h"
#include "../proc/process.h"
#include "../proc/wait_queue.h"
#include "../rtc.h"
#include "bcache.h"
#include "fat32name.h"

#define FAT_S_IFDIR 0040000u
#define FAT_S_IFREG 0100000u

#define FAT_EOC_MIN      0x0FFFFFF8u
#define FAT_BAD          0x0FFFFFF7u
#define FAT_MASK         0x0FFFFFFFu
#define FAT_MAX_CLUSTERS 0x0FFFFFEEu /* so no cluster number reaches the reserved 0x0FFFFFF0 */

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME    0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_LFN       0x0F

#define LFN_MAX_ENTRIES 20 /* 20 * 13 = 260 code units, past the 255 limit */

struct fat_node {
    int used;
    int refs;
    int is_dir;
    uint32_t dir_cluster; /* the directory holding the entry; 0 for the root */
    uint32_t index;       /* entry index in that directory */
    uint32_t first; /* first cluster; 0 for an empty file or the root */
    uint32_t size;
    uint16_t date, time, cdate, ctime, adate;
    uint8_t ctenth; /* creation time, hundredths of a second, 0 to 199 */
    uint8_t attr;
    uint8_t name11[11]; /* the short entry's name, for rewriting the entry */
    uint8_t lower;      /* its case bits */
    /* The entry is gone but a description or a directory still holds the
     * node: the chain is freed at the last put, and no lookup finds it. */
    int unlinked;
    /* The last cluster reached by a read, so a sequential read walks the
     * chain once rather than from the start on every call. */
    uint32_t hint_index;
    uint32_t hint_cluster;
};

struct fat_mount {
    int used;
    block_device_t *dev;
    uint32_t spc;
    uint32_t reserved;
    uint32_t nfats;
    uint32_t total;
    uint32_t fatsz;
    uint32_t root_cluster;
    uint32_t data_start;
    uint32_t cluster_count;
    uint32_t active_fat;
    uint32_t uid, gid, umask;
    int free_known;
    uint32_t free_clusters;
    uint32_t fsinfo_sector; /* 0 when the volume has none */
    int mirrored;           /* every FAT write goes to both copies */
    int writable;
    int dirty_set;          /* the dirty flag has been written for this mount */
    uint32_t alloc_hint;
    struct vfs_mount *mnt;
    struct fat_node nodes[FAT32_NODES];
};

static struct fat_mount fat_mounts[FAT32_MOUNTS];

static int count_free(struct fat_mount *fm);
struct fat_scan;
static int dir_scan(struct fat_mount *fm, struct fat_node *dir, uint32_t first, uint32_t *cursor,
                    struct fat_scan *out);
static int names_match(const char *a, const char *b);

/* ---- little-endian fields ---- */

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- bytes off the volume, through the cache ---- */

/**
 * Copy `len` bytes at byte offset `off` of the volume into `dst`, one cache
 * block at a time. The offset is in sectors times 512 and the volume is
 * bounded to 2 TiB by the constructor, so 64-bit arithmetic here is only
 * multiplication and addition, which the kernel has.
 */
static int fat_read_bytes(struct fat_mount *fm, uint64_t off, void *dst, uint32_t len)
{
    uint8_t *out = dst;

    while (len > 0) {
        uint64_t blk = off / BCACHE_BLOCK_BYTES;
        uint32_t in = (uint32_t)(off % BCACHE_BLOCK_BYTES);
        uint32_t take = BCACHE_BLOCK_BYTES - in;
        struct bbuf *b;

        if (take > len) {
            take = len;
        }
        b = bcache_get(fm->dev, blk);
        if (!b) {
            return -EIO;
        }
        if (in + take > b->bytes) {
            bcache_release(b);
            return -EIO; /* past the device's short last block */
        }
        k_memcpy(out, b->data + in, take);
        bcache_release(b);
        out += take;
        off += take;
        len -= take;
    }
    return 0;
}

static uint64_t cluster_offset(const struct fat_mount *fm, uint32_t cluster)
{
    return ((uint64_t)fm->data_start + (uint64_t)(cluster - 2) * fm->spc) * BLOCK_SECTOR_SIZE;
}

static uint32_t cluster_bytes(const struct fat_mount *fm)
{
    return fm->spc * BLOCK_SECTOR_SIZE;
}

/** The next cluster after `cluster`: the FAT entry masked to 28 bits, 0 at
 * the end of the chain, or a negative errno for a value no chain may hold. */
static int fat_next(struct fat_mount *fm, uint32_t cluster, uint32_t *next)
{
    uint8_t raw[4];
    uint64_t off;
    uint32_t v;
    int rc;

    off = ((uint64_t)fm->reserved + (uint64_t)fm->active_fat * fm->fatsz) * BLOCK_SECTOR_SIZE +
          (uint64_t)cluster * 4;
    rc = fat_read_bytes(fm, off, raw, 4);
    if (rc != 0) {
        return rc;
    }
    v = le32(raw) & FAT_MASK;
    if (v >= FAT_EOC_MIN) {
        *next = 0;
        return 0;
    }
    if (v == FAT_BAD || v < 2 || v >= fm->cluster_count + 2) {
        return -EIO; /* free, reserved, bad, or outside the data region */
    }
    *next = v;
    return 0;
}

/**
 * The cluster `index` clusters into the chain starting at `first`, walked
 * with the cluster count as the bound so a loop ends in EIO; a chain that
 * ends before the index answers ENXIO, which a directory reads as its end
 * and a file as damage. `node` may keep a hint from the last walk, which
 * is used when it is behind the target and updated on the way out.
 */
static int chain_at(struct fat_mount *fm, struct fat_node *node, uint32_t first, uint32_t index,
                    uint32_t *cluster)
{
    uint32_t c = first;
    uint32_t i = 0;
    uint32_t steps = 0;

    if (index >= fm->cluster_count) {
        return -EIO; /* a chain longer than the volume is a loop */
    }
    if (node && node->hint_cluster && node->hint_index <= index) {
        c = node->hint_cluster;
        i = node->hint_index;
    }
    while (i < index) {
        uint32_t n;
        int rc = fat_next(fm, c, &n);

        if (rc != 0) {
            return rc;
        }
        if (n == 0) {
            return -ENXIO; /* the chain ends before the index */
        }
        if (++steps > fm->cluster_count) {
            return -EIO; /* a loop */
        }
        c = n;
        i++;
    }
    if (node) {
        node->hint_index = index;
        node->hint_cluster = c;
    }
    *cluster = c;
    return 0;
}

/* ---- the node pool ---- */

static struct fat_node *node_take(struct fat_mount *fm)
{
    int i;

    for (i = 0; i < FAT32_NODES; i++) {
        if (!fm->nodes[i].used) {
            k_memset(&fm->nodes[i], 0, sizeof(fm->nodes[i]));
            fm->nodes[i].used = 1;
            fm->nodes[i].refs = 1;
            return &fm->nodes[i];
        }
    }
    return 0;
}

/* A node already in the pool for this identity, held once more, or NULL. */
static struct fat_node *node_find(struct fat_mount *fm, int is_dir, uint32_t dir_cluster,
                                  uint32_t index, uint32_t first)
{
    int i;

    for (i = 0; i < FAT32_NODES; i++) {
        struct fat_node *n = &fm->nodes[i];

        if (!n->used || n->is_dir != is_dir || n->unlinked) {
            continue;
        }
        if (is_dir ? n->first == first : (n->dir_cluster == dir_cluster && n->index == index)) {
            n->refs++;
            return n;
        }
    }
    return 0;
}

static void fat_hold(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    ((struct fat_node *)node)->refs++;
}

static int free_chain(struct fat_mount *fm, uint32_t first);

static void fat_put(struct vfs_mount *mnt, void *node)
{
    struct fat_node *n = node;

    if (n->refs > 0) {
        n->refs--;
    }
    if (n->refs == 0) {
        /* An unlinked file's clusters go when its last holder does; the
         * entry went at the unlink, so a crash here leaks them. */
        if (n->unlinked && n->first != 0) {
            struct fat_mount *fm = mnt->priv;

            vfs_ns_lock(mnt);
            free_chain(fm, n->first);
            vfs_ns_unlock(mnt);
        }
        n->used = 0;
    }
}

/* ---- directory entries ---- */

struct fat_dirent {
    uint8_t raw[32];
};

/**
 * The entry `index` of the directory whose chain starts at `first` (the
 * root's chain for `first` zero). 1 with the entry copied, 0 past the end
 * of the chain, or a negative errno.
 */
static int dir_entry(struct fat_mount *fm, struct fat_node *dir, uint32_t first, uint32_t index,
                     struct fat_dirent *ent)
{
    uint32_t per_cluster = cluster_bytes(fm) / 32;
    uint32_t cluster;
    int rc;

    if (first == 0) {
        first = fm->root_cluster;
    }
    rc = chain_at(fm, dir, first, index / per_cluster, &cluster);
    if (rc == -ENXIO) {
        return 0; /* past the chain's end is the end of the directory */
    }
    if (rc != 0) {
        return rc;
    }
    return fat_read_bytes(fm, cluster_offset(fm, cluster) + (uint64_t)(index % per_cluster) * 32,
                          ent->raw, 32) == 0
               ? 1
               : -EIO;
}

static uint8_t short_checksum(const uint8_t *name11)
{
    uint8_t sum = 0;
    int i;

    for (i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name11[i]);
    }
    return sum;
}

/* One UTF-16 code unit sequence into UTF-8; returns bytes written, 0 when
 * `cap` cannot hold it. Surrogate pairs arrive as two units. */
static int put_utf8(uint32_t cp, char *out, uint32_t cap)
{
    if (cp < 0x80) {
        if (cap < 1) {
            return 0;
        }
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        if (cap < 2) {
            return 0;
        }
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        if (cap < 3) {
            return 0;
        }
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cap < 4) {
        return 0;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* The 13 code units of one long-name entry, in order. */
static void lfn_units(const uint8_t *raw, uint16_t *units)
{
    static const int at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    int i;

    for (i = 0; i < 13; i++) {
        units[i] = (uint16_t)le16(raw + at[i]);
    }
}

/**
 * Render UTF-16 `units` (terminated by 0 or by `count`) as UTF-8 into
 * `out`; 0 on success, -ENAMETOOLONG when it does not fit the component,
 * -EINVAL for a name no writer produces: an unpaired surrogate, or a unit
 * after the terminator that is not the 0xFFFF padding.
 */
static int units_to_utf8(const uint16_t *units, uint32_t count, char *out, uint32_t cap)
{
    uint32_t i = 0;
    uint32_t n = 0;

    while (i < count && units[i] != 0) {
        uint32_t cp = units[i++];
        int w;

        if (cp >= 0xD800 && cp < 0xDC00) {
            if (i >= count || units[i] < 0xDC00 || units[i] >= 0xE000) {
                return -EINVAL;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i] - 0xDC00);
            i++;
        } else if (cp >= 0xDC00 && cp < 0xE000) {
            return -EINVAL;
        }
        w = put_utf8(cp, out + n, cap - 1 - n);
        if (w == 0) {
            return -ENAMETOOLONG;
        }
        n += (uint32_t)w;
    }
    for (i++; i < count; i++) {
        if (units[i] != 0xFFFF) {
            return -EINVAL;
        }
    }
    out[n] = '\0';
    return 0;
}

/* The short name as shown: the base and the extension, lower-cased where
 * the reserved byte's case bits say so, a byte above 0x7F shown as `?`. */
static void short_to_name(const uint8_t *raw, char *out)
{
    uint8_t lower = raw[12];
    int n = 0;
    int i, end;

    for (end = 8; end > 0 && raw[end - 1] == ' '; end--) {
    }
    for (i = 0; i < end; i++) {
        uint8_t c = raw[i];

        if (i == 0 && c == 0x05) {
            c = 0xE5;
        }
        if (c > 0x7F) {
            c = '?';
        } else if ((lower & 0x08) && c >= 'A' && c <= 'Z') {
            c = (uint8_t)(c + 32);
        }
        out[n++] = (char)c;
    }
    for (end = 11; end > 8 && raw[end - 1] == ' '; end--) {
    }
    if (end > 8) {
        out[n++] = '.';
        for (i = 8; i < end; i++) {
            uint8_t c = raw[i];

            if (c > 0x7F) {
                c = '?';
            } else if ((lower & 0x10) && c >= 'A' && c <= 'Z') {
                c = (uint8_t)(c + 32);
            }
            out[n++] = (char)c;
        }
    }
    out[n] = '\0';
}

/* The record a directory scan yields: one real entry with its name. */
struct fat_scan {
    struct fat_dirent ent; /* the short entry */
    uint32_t index;        /* its index in the directory */
    uint32_t start;        /* the index of the name's first entry: a long one, or `index` */
    char name[VFS_NAME_MAX + 1];
    char alias[13];    /* the short name as shown, always */
};

/**
 * The next real entry of a directory at or after entry `*cursor`, with the
 * long name assembled when it checks and the short name otherwise. The
 * cursor moves past the entry returned. 1 with `out` filled, 0 at the end,
 * or a negative errno.
 */
static int dir_scan(struct fat_mount *fm, struct fat_node *dir, uint32_t first, uint32_t *cursor,
                    struct fat_scan *out)
{
    uint16_t units[LFN_MAX_ENTRIES * 13];
    int lfn_count = 0;  /* entries collected so far */
    int lfn_expect = 0; /* the ordinal the next entry must carry */
    uint8_t lfn_sum = 0;
    int lfn_ok = 0;
    uint32_t lfn_start = 0;

    for (;;) {
        struct fat_dirent e;
        uint32_t idx = *cursor;
        int got = dir_entry(fm, dir, first, idx, &e);
        uint8_t attr;

        if (got <= 0) {
            return got;
        }
        (*cursor)++;
        if (e.raw[0] == 0) {
            return 0; /* the end of the directory */
        }
        if (e.raw[0] == 0xE5) {
            lfn_ok = 0;
            continue; /* deleted */
        }
        attr = e.raw[11];
        if (attr == ATTR_LFN) {
            uint8_t ord = e.raw[0];
            int seq = ord & 0x1F;

            /* The sequence starts with the last entry, flagged 0x40, and
             * counts down to 1; every entry names the same checksum, has
             * type 0 and a zero first-cluster field. Anything else ends
             * the sequence and the short name that follows is shown. */
            if (ord & 0x40) {
                lfn_count = 0;
                lfn_expect = seq;
                lfn_sum = e.raw[13];
                lfn_start = idx;
                lfn_ok = (seq >= 1 && seq <= LFN_MAX_ENTRIES && (ord & 0xA0) == 0);
            } else if (!lfn_ok || seq != lfn_expect || (ord & 0xE0) != 0) {
                lfn_ok = 0;
            }
            if (lfn_ok && (e.raw[12] != 0 || le16(e.raw + 26) != 0 || e.raw[13] != lfn_sum)) {
                lfn_ok = 0;
            }
            if (lfn_ok) {
                lfn_units(e.raw, units + (uint32_t)(seq - 1) * 13);
                lfn_count++;
                lfn_expect = seq - 1;
            }
            continue;
        }
        if (attr & ATTR_VOLUME) {
            lfn_ok = 0;
            continue; /* the volume label */
        }
        out->ent = e;
        out->index = idx;
        out->start = idx;
        short_to_name(e.raw, out->alias);
        if (lfn_ok && lfn_expect == 0 && lfn_count > 0 && short_checksum(e.raw) == lfn_sum) {
            int rc;

            out->start = lfn_start;
            rc = units_to_utf8(units, (uint32_t)lfn_count * 13, out->name, sizeof(out->name));
            if (rc != 0) {
                /* A long name past the kernel's component limit is listed
                 * and opened by its alias, which is then its name. */
                short_to_name(e.raw, out->name);
            }
        } else {
            short_to_name(e.raw, out->name);
        }
        return 1;
    }
}

/* Names compare with ASCII case folded, as FAT does; other bytes exactly. */
static int names_match(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;

        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca + 32);
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb + 32);
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == *b;
}

static uint32_t entry_first(const uint8_t *raw)
{
    return (le16(raw + 20) << 16) | le16(raw + 26);
}

/* A node for the scanned entry, from the pool or already in it. */
static struct fat_node *node_for(struct fat_mount *fm, uint32_t dir_cluster,
                                 const struct fat_scan *sc)
{
    int is_dir = (sc->ent.raw[11] & ATTR_DIRECTORY) ? 1 : 0;
    uint32_t first = entry_first(sc->ent.raw);
    struct fat_node *n;

    if (is_dir && first == 0) {
        first = fm->root_cluster; /* `..` naming the root */
    }
    n = node_find(fm, is_dir, dir_cluster, sc->index, first);
    if (n) {
        return n;
    }
    n = node_take(fm);
    if (!n) {
        return 0;
    }
    n->is_dir = is_dir;
    n->dir_cluster = dir_cluster;
    n->index = sc->index;
    n->first = first;
    n->size = is_dir ? 0 : le32(sc->ent.raw + 28);
    n->attr = sc->ent.raw[11];
    k_memcpy(n->name11, sc->ent.raw, 11);
    n->lower = sc->ent.raw[12];
    n->ctenth = sc->ent.raw[13];
    n->ctime = (uint16_t)le16(sc->ent.raw + 14);
    n->cdate = (uint16_t)le16(sc->ent.raw + 16);
    n->adate = (uint16_t)le16(sc->ent.raw + 18);
    n->time = (uint16_t)le16(sc->ent.raw + 22);
    n->date = (uint16_t)le16(sc->ent.raw + 24);
    return n;
}


/* ---- writing ----
 *
 * Metadata goes through `bcache_write` and reaches the disk before the call
 * returns, in call order; data goes through `bcache_dirty` and is written
 * back. The order of writes for each operation is the one fat32.Readme.md
 * gives: a crash between two of them may leak clusters and never cross-links
 * them, and an entry never names a cluster that is free.
 */

static int raw_eq(const uint8_t *a, const uint8_t *b)
{
    int i;

    for (i = 0; i < 11; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* `len` bytes of `src` at volume offset `off`, written through (`sync`) or
 * dirtied for writeback, one cache block at a time. */
static int fat_put_bytes(struct fat_mount *fm, uint64_t off, const void *src, uint32_t len,
                         int sync)
{
    const uint8_t *in = src;

    while (len > 0) {
        uint64_t blk = off / BCACHE_BLOCK_BYTES;
        uint32_t at = (uint32_t)(off % BCACHE_BLOCK_BYTES);
        uint32_t take = BCACHE_BLOCK_BYTES - at;
        struct bbuf *b;
        int rc = 0;

        if (take > len) {
            take = len;
        }
        b = bcache_get(fm->dev, blk);
        if (!b) {
            return -EIO;
        }
        if (at + take > b->bytes) {
            bcache_release(b);
            return -EIO;
        }
        if (in) {
            k_memcpy(b->data + at, in, take);
            in += take;
        } else {
            k_memset(b->data + at, 0, take);
        }
        if (sync) {
            rc = bcache_write(b);
        } else {
            bcache_dirty(b);
        }
        bcache_release(b);
        if (rc != 0) {
            return rc;
        }
        off += take;
        len -= take;
    }
    return 0;
}

static uint64_t fat_offset(const struct fat_mount *fm, uint32_t copy, uint32_t cluster)
{
    return ((uint64_t)fm->reserved + (uint64_t)copy * fm->fatsz) * BLOCK_SECTOR_SIZE +
           (uint64_t)cluster * 4;
}

/* One FAT entry, its reserved high nibble kept, written through to the
 * active FAT first and then to the mirror when mirroring is on. */
static int fat_set(struct fat_mount *fm, uint32_t cluster, uint32_t value)
{
    uint32_t copies[2];
    int ncopies = 0;
    int i;

    copies[ncopies++] = fm->active_fat;
    if (fm->mirrored && fm->nfats == 2) {
        copies[ncopies++] = fm->active_fat == 0 ? 1 : 0;
    }
    for (i = 0; i < ncopies; i++) {
        uint8_t raw[4];
        int rc = fat_read_bytes(fm, fat_offset(fm, copies[i], cluster), raw, 4);

        if (rc != 0) {
            return rc;
        }
        put32(raw, (le32(raw) & ~FAT_MASK) | (value & FAT_MASK));
        rc = fat_put_bytes(fm, fat_offset(fm, copies[i], cluster), raw, 4, 1);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* The dirty flag, in both places, written before the first metadata change
 * of a mount and cleared at a clean unmount. A cleared bit 27 of entry 1 is
 * dirty; a set bit 0 of byte 65 of the boot sector is dirty. */
static int fat_flag(struct fat_mount *fm, int dirty)
{
    uint8_t raw[4];
    uint8_t byte;
    int rc;

    rc = fat_read_bytes(fm, fat_offset(fm, fm->active_fat, 1), raw, 4);
    if (rc != 0) {
        return rc;
    }
    put32(raw, dirty ? (le32(raw) & ~0x08000000u) : (le32(raw) | 0x08000000u));
    rc = fat_put_bytes(fm, fat_offset(fm, fm->active_fat, 1), raw, 4, 1);
    if (rc == 0 && fm->mirrored && fm->nfats == 2) {
        rc = fat_put_bytes(fm, fat_offset(fm, fm->active_fat == 0 ? 1 : 0, 1), raw, 4, 1);
    }
    if (rc != 0) {
        return rc;
    }
    rc = fat_read_bytes(fm, 65, &byte, 1);
    if (rc != 0) {
        return rc;
    }
    byte = dirty ? (uint8_t)(byte | 1) : (uint8_t)(byte & ~1);
    return fat_put_bytes(fm, 65, &byte, 1, 1);
}

static int mark_dirty(struct fat_mount *fm)
{
    int rc;

    if (fm->dirty_set) {
        return 0;
    }
    rc = fat_flag(fm, 1);
    if (rc == 0) {
        fm->dirty_set = 1;
    }
    return rc;
}

/* FSInfo, when the volume has one: the count and the hint this driver
 * keeps, which no reader trusts and every writer keeps current. */
static int write_fsinfo(struct fat_mount *fm)
{
    uint8_t raw[8];

    if (fm->fsinfo_sector == 0 || !fm->free_known) {
        return 0;
    }
    put32(raw, fm->free_clusters);
    put32(raw + 4, fm->alloc_hint);
    return fat_put_bytes(fm, (uint64_t)fm->fsinfo_sector * BLOCK_SECTOR_SIZE + 488, raw, 8, 1);
}

/* One free cluster, first-fit from the hint and wrapping once, marked as
 * the end of a chain. ENOSPC is decided before anything is written. */
static int alloc_cluster(struct fat_mount *fm, uint32_t *out)
{
    uint32_t start, c;
    uint32_t tried = 0;
    int rc;

    if (!fm->free_known) {
        rc = count_free(fm);
        if (rc != 0) {
            return rc;
        }
    }
    if (fm->free_clusters == 0) {
        return -ENOSPC;
    }
    start = fm->alloc_hint >= 2 && fm->alloc_hint < fm->cluster_count + 2 ? fm->alloc_hint : 2;
    c = start;
    while (tried < fm->cluster_count) {
        uint32_t v;

        rc = fat_next(fm, c, &v);
        if (rc == -EIO) {
            uint8_t raw[4];

            rc = fat_read_bytes(fm, fat_offset(fm, fm->active_fat, c), raw, 4);
            v = rc == 0 ? (le32(raw) & FAT_MASK) : 1;
            if (rc != 0) {
                return rc;
            }
            if (v == 0) {
                rc = fat_set(fm, c, FAT_EOC_MIN);
                if (rc != 0) {
                    return rc;
                }
                fm->free_clusters--;
                fm->alloc_hint = c + 1 < fm->cluster_count + 2 ? c + 1 : 2;
                *out = c;
                return 0;
            }
        } else if (rc != 0) {
            return rc;
        }
        tried++;
        c = c + 1 < fm->cluster_count + 2 ? c + 1 : 2;
    }
    return -ENOSPC;
}

/* A run of `n` clusters chained together and ended, disconnected from
 * everything; `*first` and `*last` name its ends. `zero` 1 fills them with
 * zeros for writeback, which a file's data sync makes stable before the
 * run is joined; `zero` 2 writes the zeros through, for a directory
 * cluster, which is joined without a sync of its own. A failure frees what
 * was taken. */
static int free_chain(struct fat_mount *fm, uint32_t first);

static int alloc_run(struct fat_mount *fm, uint32_t n, int zero, uint32_t *first, uint32_t *last)
{
    uint32_t prev = 0;
    uint32_t i;
    int rc = 0;

    *first = 0;
    *last = 0;
    if (!fm->free_known) {
        rc = count_free(fm);
        if (rc != 0) {
            return rc;
        }
    }
    if (fm->free_clusters < n) {
        return -ENOSPC; /* decided before any FAT sector is written */
    }
    for (i = 0; i < n && rc == 0; i++) {
        uint32_t c;

        rc = alloc_cluster(fm, &c);
        if (rc != 0) {
            break;
        }
        if (zero) {
            rc = fat_put_bytes(fm, cluster_offset(fm, c), 0, cluster_bytes(fm), zero == 2);
        }
        if (rc == 0 && prev != 0) {
            rc = fat_set(fm, prev, c);
        }
        if (rc != 0) {
            fat_set(fm, c, 0);
            fm->free_clusters++;
            break;
        }
        if (prev == 0) {
            *first = c;
        }
        prev = c;
    }
    if (rc != 0 && *first != 0) {
        free_chain(fm, *first); /* what was taken goes back */
        *first = 0;
    }
    *last = prev;
    return rc;
}

/* Free the chain at `first` to its end; a chain that leaves the volume or
 * loops is freed as far as it was sound. */
static int free_chain(struct fat_mount *fm, uint32_t first)
{
    uint32_t c = first;
    uint32_t steps = 0;

    while (c >= 2 && c < fm->cluster_count + 2 && steps < fm->cluster_count) {
        uint32_t next;
        int rc = fat_next(fm, c, &next);

        if (rc != 0) {
            next = 0;
        }
        rc = fat_set(fm, c, 0);
        if (rc != 0) {
            return rc;
        }
        if (fm->free_known) {
            fm->free_clusters++;
        }
        if (c < fm->alloc_hint) {
            fm->alloc_hint = c;
        }
        c = next;
        steps++;
    }
    return 0;
}

/* The clock as a FAT stamp: local time read as UTC, two-second resolution
 * in `time`, hundredths of the odd second in `tenth`. */
static void fat_now(uint16_t *date, uint16_t *time, uint8_t *tenth)
{
    int64_t sec, nsec;
    uint32_t days, rem, year, month, day;
    static const uint32_t length[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    rtc_realtime(&sec, &nsec);
    if (sec < 315532800) {
        sec = 315532800; /* before 1980: the stamp floors at the epoch FAT has */
    }
    if (sec > 4294967295LL) {
        sec = 4294967295LL; /* the year 2106, past FAT's own 2107 */
    }
    days = (uint32_t)sec / 86400u; /* 32-bit: the kernel has no 64-bit division */
    rem = (uint32_t)sec % 86400u;
    for (year = 1970;; year++) {
        uint32_t in_year = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;

        if (days < in_year) {
            break;
        }
        days -= in_year;
    }
    for (month = 1; month <= 12; month++) {
        uint32_t in_month = length[month - 1];

        if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) {
            in_month = 29;
        }
        if (days < in_month) {
            break;
        }
        days -= in_month;
    }
    day = days + 1;
    if (year > 2107) {
        year = 2107;
    }
    *date = (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
    *time = (uint16_t)(((rem / 3600) << 11) | (((rem / 60) % 60) << 5) | ((rem % 60) / 2));
    if (nsec < 0 || nsec >= 1000000000) {
        nsec = 0;
    }
    *tenth = (uint8_t)(((rem % 60) % 2) * 100 + (uint32_t)nsec / 10000000u);
}

/* The modification stamp: the date and the two-second time; the creation
 * hundredths belong to the creation time and are not touched. */
static void stamp_mtime(struct fat_node *n)
{
    uint8_t tenth;

    fat_now(&n->date, &n->time, &tenth);
}

/* The 32 bytes of a node's entry, from its fields. */
static void entry_of(const struct fat_node *n, uint8_t *raw)
{
    k_memset(raw, 0, 32);
    k_memcpy(raw, n->name11, 11);
    raw[11] = n->attr;
    raw[12] = n->lower;
    raw[13] = n->ctenth;
    put16(raw + 14, n->ctime);
    put16(raw + 16, n->cdate);
    put16(raw + 18, n->adate);
    put16(raw + 20, n->first >> 16);
    put16(raw + 22, n->time);
    put16(raw + 24, n->date);
    put16(raw + 26, n->first & 0xFFFFu);
    put32(raw + 28, n->size);
}

/* The volume offset of entry `index` of the directory at `first` (the root's
 * chain for zero), or a negative errno. */
static int entry_offset(struct fat_mount *fm, uint32_t first, uint32_t index, uint64_t *off)
{
    uint32_t per_cluster = cluster_bytes(fm) / 32;
    uint32_t cluster;
    int rc;

    if (first == 0) {
        first = fm->root_cluster;
    }
    rc = chain_at(fm, 0, first, index / per_cluster, &cluster);
    if (rc != 0) {
        return rc == -ENXIO ? -EIO : rc;
    }
    *off = cluster_offset(fm, cluster) + (uint64_t)(index % per_cluster) * 32;
    return 0;
}

static int write_entry_raw(struct fat_mount *fm, uint32_t dir_first, uint32_t index,
                           const uint8_t *raw)
{
    uint64_t off;
    int rc = entry_offset(fm, dir_first, index, &off);

    if (rc != 0) {
        return rc;
    }
    return fat_put_bytes(fm, off, raw, 32, 1);
}

/* A node's entry rewritten from its fields: the one write that publishes a
 * size, a first cluster or a time. An unlinked node has no entry; its
 * fields stay in the pool for the descriptions that still use it. */
static int write_node_entry(struct fat_mount *fm, struct fat_node *n)
{
    uint8_t raw[32];

    if (n->unlinked) {
        return 0;
    }
    entry_of(n, raw);
    return write_entry_raw(fm, n->dir_cluster, n->index, raw);
}

/* The index of `count` free entries in a row in the directory at `first`,
 * growing the directory by zeroed clusters, each chained after the old
 * tail, when the entries are not there. */
static int free_slots(struct fat_mount *fm, struct fat_node *dir, uint32_t first, uint32_t count,
                      uint32_t *index)
{
    uint32_t per_cluster = cluster_bytes(fm) / 32;
    uint32_t i = 0, run = 0, run_start = 0, total;
    uint32_t chain_first = first == 0 ? fm->root_cluster : first;

    for (;;) {
        struct fat_dirent e;
        int got = dir_entry(fm, dir, first, i, &e);

        if (got < 0) {
            return got;
        }
        if (got == 0) {
            break; /* the chain ended: grow it */
        }
        if (e.raw[0] == 0 || e.raw[0] == 0xE5) {
            if (run == 0) {
                run_start = i;
            }
            run++;
            if (run == count) {
                *index = run_start;
                return 0;
            }
        } else {
            run = 0;
        }
        i++;
    }
    total = i;
    if (run == 0) {
        run_start = i;
    }
    while (total - run_start < count) {
        uint32_t tail, newc, unused;
        int rc;

        rc = chain_at(fm, 0, chain_first, (total - 1) / per_cluster, &tail);
        if (rc != 0) {
            return rc == -ENXIO ? -EIO : rc;
        }
        rc = alloc_run(fm, 1, 2, &newc, &unused);
        if (rc != 0) {
            return rc;
        }
        rc = fat_set(fm, tail, newc);
        if (rc != 0) {
            free_chain(fm, newc);
            return rc;
        }
        if (dir) {
            dir->hint_cluster = 0;
        }
        total += per_cluster;
    }
    *index = run_start;
    return 0;
}

/* A name prepared for the directory at `first`: checked, and either a short
 * entry alone or long entries in front of an alias no entry of the
 * directory uses. */
struct new_name {
    uint8_t name11[11];
    uint8_t lower;
    uint16_t units[FAT32_NAME_UNITS];
    uint32_t count;   /* UTF-16 units, 0 for a short-only name */
    uint32_t entries; /* long entries, 0 for a short-only name */
};

static int alias_taken(struct fat_mount *fm, struct fat_node *dir, uint32_t first,
                       const uint8_t *name11, int *taken)
{
    uint32_t i;

    *taken = 0;
    for (i = 0;; i++) {
        struct fat_dirent e;
        int got = dir_entry(fm, dir, first, i, &e);

        if (got < 0) {
            return got;
        }
        if (got == 0 || e.raw[0] == 0) {
            return 0;
        }
        if (e.raw[0] == 0xE5 || e.raw[11] == ATTR_LFN || (e.raw[11] & ATTR_VOLUME)) {
            continue;
        }
        if (raw_eq(e.raw, name11)) {
            *taken = 1;
            return 0;
        }
    }
}

static int prepare_name(struct fat_mount *fm, struct fat_node *dir, uint32_t first,
                        const char *name, struct new_name *nn)
{
    uint32_t n;
    int rc = fat32_name_units(name, nn->units, &nn->count);

    if (rc != 0) {
        return rc;
    }
    if (fat32_name_short(name, nn->name11, &nn->lower)) {
        nn->count = 0;
        nn->entries = 0;
        return 0;
    }
    nn->lower = 0;
    nn->entries = fat32_long_entries(nn->count);
    fat32_alias(name, nn->name11);
    for (n = 1; n <= 999999; n++) {
        int taken;

        fat32_alias_number(nn->name11, n);
        rc = alias_taken(fm, dir, first, nn->name11, &taken);
        if (rc != 0) {
            return rc;
        }
        if (!taken) {
            return 0;
        }
    }
    return -ENOSPC; /* a million names on one alias */
}

/* The entries of a name written in order: the long ones, highest ordinal
 * first, then the short entry last, so no reader trusts them before the
 * short entry is there. `raw` is the short entry. */
static int write_name_entries(struct fat_mount *fm, uint32_t dir_first, uint32_t index,
                              const struct new_name *nn, const uint8_t *raw)
{
    uint32_t ord;
    uint8_t sum = fat32_short_sum(nn->name11);
    int rc;

    for (ord = nn->entries; ord >= 1; ord--) {
        uint8_t lraw[32];

        fat32_long_entry(lraw, ord, nn->entries, sum, nn->units, nn->count);
        rc = write_entry_raw(fm, dir_first, index + (nn->entries - ord), lraw);
        if (rc != 0) {
            return rc;
        }
    }
    return write_entry_raw(fm, dir_first, index + nn->entries, raw);
}

/* The entry `name` names in the directory at `first`: 1 with the scan
 * filled, 0 when absent, or a negative errno. */
static int find_entry(struct fat_mount *fm, struct fat_node *dir, uint32_t first, const char *name,
                      struct fat_scan *sc)
{
    uint32_t cursor = 0;

    for (;;) {
        int got = dir_scan(fm, dir, first, &cursor, sc);

        if (got <= 0) {
            return got;
        }
        if (names_match(sc->name, name) || names_match(sc->alias, name)) {
            return 1;
        }
    }
}

/* The node in the pool for the entry at (`dir_cluster`, `index`), whatever
 * holds it, or NULL. */
static struct fat_node *node_at(struct fat_mount *fm, uint32_t dir_cluster, uint32_t index)
{
    int i;

    for (i = 0; i < FAT32_NODES; i++) {
        struct fat_node *n = &fm->nodes[i];

        if (n->used && !n->unlinked && !n->is_dir && n->dir_cluster == dir_cluster &&
            n->index == index) {
            return n;
        }
    }
    return 0;
}

static struct fat_node *dir_node_at(struct fat_mount *fm, uint32_t first)
{
    int i;

    for (i = 0; i < FAT32_NODES; i++) {
        struct fat_node *n = &fm->nodes[i];

        if (n->used && n->is_dir && n->first == first) {
            return n;
        }
    }
    return 0;
}

/* Whether the directory at `first` holds anything beyond `.` and `..`. */
static int dir_has_entries(struct fat_mount *fm, uint32_t first, int *has)
{
    uint32_t i;

    *has = 0;
    for (i = 0;; i++) {
        struct fat_dirent e;
        int got = dir_entry(fm, 0, first, i, &e);

        if (got < 0) {
            return got;
        }
        if (got == 0 || e.raw[0] == 0) {
            return 0;
        }
        if (e.raw[0] == 0xE5) {
            continue;
        }
        if (raw_eq(e.raw, (const uint8_t *)".          ") ||
            raw_eq(e.raw, (const uint8_t *)"..         ")) {
            continue;
        }
        *has = 1;
        return 0;
    }
}

/* ---- the operations ---- */

static struct fat_mount *fm_of(struct vfs_mount *mnt)
{
    return mnt->priv;
}

static int fat_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    struct fat_mount *fm = fm_of(mnt);
    struct fat_node *d = dir ? dir : mnt->root;
    uint32_t dir_cluster = d->first == fm->root_cluster ? 0 : d->first;
    uint32_t cursor = 0;


    if (name[0] == '.' && name[1] == '\0') {
        d->refs++;
        *node = d;
        return 0;
    }
    for (;;) {
        struct fat_scan sc;
        int got = dir_scan(fm, d, d->first, &cursor, &sc);

        if (got < 0) {
            return got;
        }
        if (got == 0) {
            return -ENOENT;
        }
        /* The long name or the short alias, as vfat on Linux matches. */
        if (names_match(sc.name, name) || names_match(sc.alias, name)) {
            struct fat_node *n;

            /* `..` names the parent by its first cluster, zero for the
             * root; `.` and `..` are not files in a directory. */
            n = node_for(fm, dir_cluster, &sc);
            if (!n) {
                klog(KLOG_WARN, "vfat", "node pool full");
                return -ENOMEM;
            }
            *node = n;
            return 0;
        }
    }
}

/* The inode number: a directory is its first cluster, below 2^28; a file is
 * its directory's cluster (0 for the root) plus one in the high word and
 * its entry index plus one in the low, so no two entries share a number. */
static uint64_t fat_ino(int is_dir, uint32_t first, uint32_t dir_cluster, uint32_t index)
{
    if (is_dir) {
        return first;
    }
    return ((uint64_t)(dir_cluster + 1) << 32) | (uint64_t)(index + 1);
}

/* FAT keeps local time with no zone; it is read as UTC. Days count from
 * 1980-01-01 through the Gregorian rule, in 32-bit arithmetic. A field out
 * of its range reads as 1980-01-01, as an unset stamp does. */
static int64_t fat_epoch(uint16_t date, uint16_t time)
{
    uint32_t year = 1980 + (date >> 9);
    uint32_t month = (date >> 5) & 0xF;
    uint32_t day = date & 0x1F;
    uint32_t days = 0;
    uint32_t yr;
    static const uint32_t before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    static const uint32_t length[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap;

    if (month < 1 || month > 12 || day < 1) {
        return 315532800; /* 1980-01-01: an unset or broken stamp */
    }
    leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > length[month - 1] + (month == 2 && leap ? 1u : 0u) || (time >> 11) > 23 ||
        ((time >> 5) & 0x3F) > 59 || (time & 0x1F) > 29) {
        return 315532800;
    }
    for (yr = 1970; yr < year; yr++) {
        days += (yr % 4 == 0 && (yr % 100 != 0 || yr % 400 == 0)) ? 366 : 365;
    }
    days += before[month - 1] + day - 1;
    if (month > 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) {
        days++;
    }
    return (int64_t)days * 86400 + (int64_t)(time >> 11) * 3600 +
           (int64_t)((time >> 5) & 0x3F) * 60 + (int64_t)(time & 0x1F) * 2;
}

static int fat_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    struct fat_mount *fm = fm_of(mnt);
    struct fat_node *n = node;

    k_memset(st, 0, sizeof(*st));
    /* Every entry reports 0777 less the mount's umask, as vfat on Linux
     * does: FAT has no execute bit, and a program on a volume runs unless
     * the mount says noexec. */
    st->mode = (n->is_dir ? FAT_S_IFDIR : FAT_S_IFREG) | (0777u & ~fm->umask);
    if (n->attr & ATTR_READ_ONLY) {
        st->mode &= ~0222u; /* the one attribute a mode can hold */
    }
    st->uid = fm->uid;
    st->gid = fm->gid;
    st->nlink = 1;
    st->size = n->size;
    st->ino = fat_ino(n->is_dir, n->first, n->dir_cluster, n->index);
    st->mtime_sec = fat_epoch(n->date, n->time);
    {
        uint32_t tenth = n->ctenth > 199 ? 0 : n->ctenth; /* 0 to 199 by the format */

        st->ctime_sec = fat_epoch(n->cdate, n->ctime) + tenth / 100;
        st->ctime_nsec = (int64_t)(tenth % 100) * 10000000;
    }
    st->atime_sec = fat_epoch(n->adate, 0);
    return 0;
}

static int fat_open(struct vfs_mount *mnt, void *node, int flags, void **handle)
{
    (void)mnt;
    (void)flags;
    ((struct fat_node *)node)->refs++;
    *handle = node;
    return 0;
}

static void fat_release(struct vfs_mount *mnt, void *handle)
{
    fat_put(mnt, handle);
}

static int32_t fat_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count,
                        uint64_t offset)
{
    struct fat_mount *fm = fm_of(mnt);
    struct fat_node *n = handle;
    uint32_t cbytes = cluster_bytes(fm);
    uint8_t *out = buf;
    uint32_t done = 0;

    if (n->is_dir) {
        return -EISDIR;
    }
    if (offset >= n->size) {
        return 0;
    }
    if (count > n->size - offset) {
        count = (uint32_t)(n->size - offset);
    }
    while (done < count) {
        uint32_t at = (uint32_t)offset + done;
        uint32_t in = at % cbytes;
        uint32_t take = cbytes - in;
        uint32_t cluster;
        int rc;

        if (take > count - done) {
            take = count - done;
        }
        if (n->first == 0) {
            return -EIO; /* a size with no clusters */
        }
        rc = chain_at(fm, n, n->first, at / cbytes, &cluster);
        if (rc == -ENXIO) {
            rc = -EIO; /* the chain is shorter than the file claims */
        }
        if (rc != 0) {
            return done > 0 ? (int32_t)done : rc;
        }
        rc = fat_read_bytes(fm, cluster_offset(fm, cluster) + in, out + done, take);
        if (rc != 0) {
            return done > 0 ? (int32_t)done : rc;
        }
        done += take;
    }
    return (int32_t)done;
}

static int fat_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor,
                       struct vfs_dirent *ent)
{
    struct fat_mount *fm = fm_of(mnt);
    struct fat_node *d = handle;
    struct fat_scan sc;
    int got;

    if (!d->is_dir) {
        return -ENOTDIR;
    }
    got = dir_scan(fm, d, d->first, cursor, &sc);
    if (got <= 0) {
        return got;
    }
    k_strncpy(ent->name, sc.name, sizeof(ent->name));
    ent->name[sizeof(ent->name) - 1] = '\0';
    ent->is_dir = (sc.ent.raw[11] & ATTR_DIRECTORY) ? 1 : 0;
    {
        uint32_t first = entry_first(sc.ent.raw);

        if (ent->is_dir && first == 0) {
            first = fm->root_cluster; /* `.` of the root, or `..` naming it */
        }
        ent->ino = fat_ino(ent->is_dir, first,
                           d->first == fm->root_cluster ? 0 : d->first, sc.index);
    }
    return 1;
}

/* Free clusters counted from the active FAT on the first statfs and kept:
 * a read-only volume cannot change. */
static int count_free(struct fat_mount *fm)
{
    uint64_t off =
        ((uint64_t)fm->reserved + (uint64_t)fm->active_fat * fm->fatsz) * BLOCK_SECTOR_SIZE;
    uint32_t c = 2;
    uint32_t free_count = 0;

    while (c < fm->cluster_count + 2) {
        uint8_t raw[512];
        uint32_t i;
        int rc = fat_read_bytes(fm, off + (uint64_t)c * 4, raw, sizeof(raw));

        if (rc != 0) {
            return rc;
        }
        for (i = 0; i < 128 && c < fm->cluster_count + 2; i++, c++) {
            if ((le32(raw + i * 4) & FAT_MASK) == 0) {
                free_count++;
            }
        }
    }
    fm->free_clusters = free_count;
    fm->free_known = 1;
    return 0;
}

static int fat_statfs(struct vfs_mount *mnt, struct vfs_statfs *st)
{
    struct fat_mount *fm = fm_of(mnt);

    if (!fm->free_known) {
        int rc;

        vfs_ns_lock(mnt); /* the count and the first allocation exclude */
        rc = count_free(fm);
        vfs_ns_unlock(mnt);
        if (rc != 0) {
            return rc;
        }
    }
    st->magic = 0x4d44u; /* MSDOS_SUPER_MAGIC, what Linux reports for vfat */
    st->bsize = cluster_bytes(fm);
    st->blocks = fm->cluster_count;
    st->bfree = fm->free_clusters;
    st->namelen = VFS_NAME_MAX;
    return 0;
}


/* ---- the write operations, under the mount's namespace lock ---- */

/* Every mutation of the volume runs under the mount's namespace lock, and
 * says so: the lock is what keeps two writers, or a writer and a sync, from
 * interleaving across the sleeps the writes make. A caller without it is a
 * defect, found on the first mutation and not by a race. */
static void assert_locked(struct vfs_mount *mnt)
{
    int me = current_proc ? (int)current_proc->pid : 0;

    if (mnt->ns_holder != me || mnt->ns_depth == 0) {
        k_panic("vfat: a mutation outside the mount's lock");
    }
}

/* Whether `name` is in `d`, by long name or alias as the lookup matches:
 * 1, 0, or a negative error. Decided under the lock, so a create that
 * waited behind another's sees what it made. */
static int dir_has_name(struct fat_mount *fm, struct fat_node *d, const char *name)
{
    uint32_t cursor = 0;

    for (;;) {
        struct fat_scan sc;
        int got = dir_scan(fm, d, d->first, &cursor, &sc);

        if (got <= 0) {
            return got;
        }
        if (names_match(sc.name, name) || names_match(sc.alias, name)) {
            return 1;
        }
    }
}

/* The mount's lock for an operation the common layer does not lock: write,
 * truncate, fsync and sync. The namespace operations arrive locked. */
static int begin_write(struct vfs_mount *mnt, struct fat_mount *fm)
{
    vfs_ns_lock(mnt);
    /* Decided under the lock: a writer queued behind the one whose write
     * failed finds the mount read-only, not the flags it saw before. */
    if (!fm->writable || (mnt->flags & VFS_MS_RDONLY)) {
        vfs_ns_unlock(mnt);
        return -EROFS;
    }
    return 0;
}

/* Copy `count` bytes of `buf` (or zeros) into the file's clusters at file
 * offset `off`, through the chain at `first`; the clusters must exist. */
static int put_file_bytes(struct fat_mount *fm, struct fat_node *n, uint32_t first, uint64_t off,
                          const uint8_t *buf, uint32_t count)
{
    uint32_t cbytes = cluster_bytes(fm);
    uint32_t done = 0;

    while (done < count) {
        uint32_t at = (uint32_t)(off + done);
        uint32_t in = at % cbytes;
        uint32_t take = cbytes - in;
        uint32_t cluster;
        int rc;

        if (take > count - done) {
            take = count - done;
        }
        rc = chain_at(fm, n, first, at / cbytes, &cluster);
        if (rc != 0) {
            return rc == -ENXIO ? -EIO : rc;
        }
        rc = fat_put_bytes(fm, cluster_offset(fm, cluster) + in, buf ? buf + done : 0, take, 0);
        if (rc != 0) {
            return rc;
        }
        done += take;
    }
    return 0;
}

/* Extend the file's chain so that it covers `needed` clusters: the new run
 * is allocated disconnected and zero-filled, and joined to the old tail (or
 * published as the first cluster by the caller's entry write) only after
 * the caller's data is on the disk. Returns the run's first cluster. */
static int extend_chain(struct fat_mount *fm, struct fat_node *n, uint32_t have, uint32_t needed,
                        uint32_t *run_first)
{
    uint32_t run_last;
    int rc = alloc_run(fm, needed - have, 1, run_first, &run_last);

    (void)n;
    return rc;
}

static int32_t fat_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                         uint64_t offset)
{
    struct fat_mount *fm = fm_of(mnt);
    struct fat_node *n = handle;
    uint32_t cbytes = cluster_bytes(fm);
    uint32_t have, needed, run_first = 0;
    uint64_t end;
    int rc;

    if (n->is_dir) {
        return -EISDIR;
    }
    if (count == 0) {
        return 0;
    }
    if (offset >= 0xFFFFFFFFu || count > 0xFFFFFFFFu - offset) {
        return -EFBIG;
    }
    rc = begin_write(mnt, fm);
    if (rc != 0) {
        return rc;
    }
    assert_locked(mnt);
    rc = mark_dirty(fm);
    if (rc != 0) {
        goto out;
    }
    end = offset + count;
    have = n->size == 0 ? 0 : (n->size - 1) / cbytes + 1;
    needed = (uint32_t)(end - 1) / cbytes + 1; /* end fits 32 bits, checked above */
    if (needed > have) {
        rc = extend_chain(fm, n, have, needed, &run_first);
        if (rc != 0) {
            goto out;
        }
    }
    /* The bytes: the old clusters take the part that lands in them, the
     * new run the rest; a gap between the old size and the write is
     * zeroed, in old clusters here and in the run by its zero fill. */
    if (offset > n->size && have > 0) {
        uint64_t gap_end = (uint64_t)have * cbytes < offset ? (uint64_t)have * cbytes : offset;

        if (gap_end > n->size) {
            rc = put_file_bytes(fm, n, n->first, n->size, 0, (uint32_t)(gap_end - n->size));
            if (rc != 0) {
                goto undo;
            }
        }
    }
    if (offset < (uint64_t)have * cbytes) {
        uint32_t part = (uint32_t)((uint64_t)have * cbytes - offset);

        if (part > count) {
            part = count;
        }
        rc = put_file_bytes(fm, n, n->first, offset, buf, part);
        if (rc != 0) {
            goto undo;
        }
        if (part < count) {
            rc = put_file_bytes(fm, 0, run_first, offset + part - (uint64_t)have * cbytes,
                                (const uint8_t *)buf + part, count - part);
        }
    } else {
        rc = put_file_bytes(fm, 0, run_first, offset - (uint64_t)have * cbytes, buf, count);
    }
    if (rc != 0) {
        goto undo;
    }
    rc = bcache_sync(fm->dev); /* the data reaches the disk before the chain names it */
    if (rc != 0) {
        goto undo;
    }
    if (run_first != 0 && have > 0) {
        uint32_t tail;

        rc = chain_at(fm, n, n->first, have - 1, &tail);
        if (rc == 0) {
            rc = fat_set(fm, tail, run_first);
        }
        if (rc != 0) {
            goto undo;
        }
    } else if (run_first != 0) {
        n->first = run_first; /* published with the size, in the one entry write below */
    }
    if (end > n->size) {
        n->size = (uint32_t)end;
    }
    stamp_mtime(n);
    rc = write_node_entry(fm, n);
    if (rc != 0) {
        goto out; /* the chain is joined; the size is the walk's surplus */
    }
    n->hint_cluster = 0;
    vfs_ns_unlock(mnt);
    return (int32_t)count;
undo:
    if (run_first != 0) {
        free_chain(fm, run_first);
    }
out:
    vfs_ns_unlock(mnt);
    return rc;
}

static int fat_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    struct fat_mount *fm = fm_of(mnt);
    struct fat_node *n = handle;
    uint32_t cbytes = cluster_bytes(fm);
    uint32_t have, keep;
    int rc;

    if (n->is_dir) {
        return -EISDIR;
    }
    if (size > 0xFFFFFFFFu) {
        return -EFBIG;
    }
    rc = begin_write(mnt, fm);
    if (rc != 0) {
        return rc;
    }
    assert_locked(mnt);
    rc = mark_dirty(fm);
    if (rc != 0) {
        goto out;
    }
    have = n->size == 0 ? 0 : (n->size - 1) / cbytes + 1;
    keep = size == 0 ? 0 : (uint32_t)(size - 1) / cbytes + 1;
    if (size > n->size) {
        /* A grow is a write of zeros: new clusters, zeroed and joined,
         * then the size. */
        uint32_t run_first = 0;

        if (keep > have) {
            rc = extend_chain(fm, n, have, keep, &run_first);
            if (rc != 0) {
                goto out;
            }
        }
        if (have > 0 && n->size < (uint64_t)have * cbytes) {
            uint64_t gap_end = (uint64_t)have * cbytes < size ? (uint64_t)have * cbytes : size;

            rc = put_file_bytes(fm, n, n->first, n->size, 0, (uint32_t)(gap_end - n->size));
            if (rc == 0) {
                rc = bcache_sync(fm->dev);
            }
            if (rc != 0) {
                if (run_first) {
                    free_chain(fm, run_first);
                }
                goto out;
            }
        }
        if (run_first != 0) {
            /* The run's zeros are on the disk before the link that
             * publishes them, whether or not a gap was filled above. */
            rc = bcache_sync(fm->dev);
            if (rc != 0) {
                free_chain(fm, run_first);
                goto out;
            }
        }
        if (run_first != 0 && have > 0) {
            uint32_t tail;

            rc = chain_at(fm, n, n->first, have - 1, &tail);
            if (rc == 0) {
                rc = fat_set(fm, tail, run_first);
            }
            if (rc != 0) {
                free_chain(fm, run_first);
                goto out;
            }
        } else if (run_first != 0) {
            n->first = run_first;
        }
        n->size = (uint32_t)size;
        stamp_mtime(n);
        rc = write_node_entry(fm, n);
        n->hint_cluster = 0;
        goto out;
    }
    /* A shrink: the size first (and the first cluster to zero with it),
     * the retained tail ended, the detached tail freed last. */
    {
        uint32_t old_first = n->first;
        uint32_t cut = 0;

        n->size = (uint32_t)size;
        if (keep == 0) {
            n->first = 0;
        }
        stamp_mtime(n);
        rc = write_node_entry(fm, n);
        if (rc != 0) {
            goto out;
        }
        if (keep > 0 && keep < have) {
            uint32_t last;

            rc = chain_at(fm, n, old_first, keep - 1, &last);
            if (rc == 0) {
                rc = fat_next(fm, last, &cut);
            }
            if (rc == 0) {
                rc = fat_set(fm, last, FAT_EOC_MIN);
            }
            if (rc != 0) {
                goto out;
            }
        } else if (keep == 0) {
            cut = old_first;
        }
        if (cut != 0) {
            rc = free_chain(fm, cut);
        }
        n->hint_cluster = 0;
    }
out:
    vfs_ns_unlock(mnt);
    return rc;
}

/* A FAT stamp from an epoch, the inverse of `fat_epoch`, floored at 1980
 * and capped at 2107. */
static void fat_stamp(int64_t sec, uint16_t *date, uint16_t *time)
{
    uint32_t days, rem, year, month, day;
    static const uint32_t length[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (sec < 315532800) {
        sec = 315532800;
    }
    if (sec > 4294967295LL) {
        sec = 4294967295LL;
    }
    days = (uint32_t)sec / 86400u;
    rem = (uint32_t)sec % 86400u;
    for (year = 1970;; year++) {
        uint32_t in_year = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;

        if (days < in_year) {
            break;
        }
        days -= in_year;
    }
    for (month = 1; month <= 12; month++) {
        uint32_t in_month = length[month - 1];

        if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) {
            in_month = 29;
        }
        if (days < in_month) {
            break;
        }
        days -= in_month;
    }
    day = days + 1;
    if (year > 2107) {
        year = 2107;
    }
    *date = (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
    *time = (uint16_t)(((rem / 3600) << 11) | (((rem / 60) % 60) << 5) | ((rem % 60) / 2));
}

/* The attributes the volume can store: the access and modification times,
 * and the write bits as the read-only attribute. A mode change outside the
 * write bits, an owner or a group is EPERM, as Linux's vfat answers. */
static int fat_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st)
{
    struct fat_mount *fm = fm_of(mnt);
    struct fat_node *n = node;
    int rc;

    if (what & ~(VFS_SET_MODE | VFS_SET_ATIME | VFS_SET_MTIME)) {
        return -EPERM; /* the owner, the group, or a field this call does not store */
    }
    if (what & VFS_SET_MODE) {
        uint32_t base = 0777u & ~fm->umask;
        uint32_t want = st->mode & 07777u;

        if ((want & ~0222u) != (base & ~0222u)) {
            return -EPERM;
        }
    }
    if (n->is_dir && n->first == fm->root_cluster) {
        return 0; /* the root has no entry of its own */
    }
    rc = begin_write(mnt, fm);
    if (rc != 0) {
        return rc;
    }
    assert_locked(mnt);
    rc = mark_dirty(fm);
    if (rc == 0) {
        if (what & VFS_SET_MODE) {
            if (st->mode & 0222u) {
                n->attr = (uint8_t)(n->attr & ~ATTR_READ_ONLY);
            } else {
                n->attr |= ATTR_READ_ONLY;
            }
        }
        if (what & VFS_SET_ATIME) {
            uint16_t unused;

            fat_stamp(st->atime_sec, &n->adate, &unused);
        }
        if (what & VFS_SET_MTIME) {
            fat_stamp(st->mtime_sec, &n->date, &n->time);
        }
        rc = write_node_entry(fm, n);
    }
    vfs_ns_unlock(mnt);
    return rc;
}

static int fat_fsync(struct vfs_mount *mnt, void *handle)
{
    struct fat_mount *fm = fm_of(mnt);
    int rc;

    (void)handle;
    vfs_ns_lock(mnt);
    rc = bcache_sync(fm->dev);
    vfs_ns_unlock(mnt);
    return rc;
}

static int fat_sync(struct vfs_mount *mnt)
{
    struct fat_mount *fm = fm_of(mnt);
    int rc;

    if (!fm->writable) {
        return 0;
    }
    vfs_ns_lock(mnt); /* a first mutation's flag writes may be in flight */
    if (!fm->dirty_set) {
        vfs_ns_unlock(mnt);
        return 0;
    }
    rc = bcache_sync(fm->dev);
    if (rc == 0) {
        rc = write_fsinfo(fm);
    }
    vfs_ns_unlock(mnt);
    return rc;
}

/* A new file: its entry, free of clusters, in a free slot of `dir`. */
static int fat_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode,
                      void **node)
{
    struct fat_mount *fm = fm_of(mnt);
    struct fat_node *d = dir ? dir : mnt->root;
    uint32_t dir_cluster = d->first == fm->root_cluster ? 0 : d->first;
    struct fat_node *n;
    static struct new_name nn; /* one create at a time, under the lock */
    uint8_t raw[32];
    uint32_t index;
    int rc;

    (void)mode;
    if (!fm->writable || (mnt->flags & VFS_MS_RDONLY)) {
        return -EROFS;
    }
    assert_locked(mnt);
    rc = dir_has_name(fm, d, name);
    if (rc != 0) {
        return rc > 0 ? -EEXIST : rc;
    }
    rc = prepare_name(fm, d, d->first, name, &nn);
    if (rc != 0) {
        return rc;
    }
    n = node_take(fm);
    if (!n) {
        return -ENOMEM;
    }
    rc = mark_dirty(fm);
    if (rc == 0) {
        rc = free_slots(fm, d, d->first, nn.entries + 1, &index);
    }
    if (rc != 0) {
        n->used = 0;
        return rc;
    }
    k_memcpy(n->name11, nn.name11, 11);
    n->lower = nn.lower;
    n->is_dir = 0;
    n->dir_cluster = dir_cluster;
    n->index = index + nn.entries;
    n->first = 0;
    n->size = 0;
    n->attr = 0x20;
    fat_now(&n->cdate, &n->ctime, &n->ctenth);
    n->date = n->cdate;
    n->time = n->ctime;
    n->adate = n->cdate;
    entry_of(n, raw);
    rc = write_name_entries(fm, d->first, index, &nn, raw);
    if (rc != 0) {
        n->used = 0;
        return rc;
    }
    *node = n;
    return 0;
}

/* Free an entry: 0xE5 in its first byte, the rest kept. */
static int free_entry(struct fat_mount *fm, uint32_t dir_first, uint32_t index)
{
    uint64_t off;
    uint8_t e5 = 0xE5;
    int rc = entry_offset(fm, dir_first, index, &off);

    if (rc != 0) {
        return rc;
    }
    return fat_put_bytes(fm, off, &e5, 1, 1);
}

/* The entries of a name freed: the short entry first, then its long entries
 * from the one nearest it back to the first, so a reader never trusts the
 * long ones without the short one. */
static int free_name_entries(struct fat_mount *fm, uint32_t dir_first, const struct fat_scan *sc)
{
    uint32_t i;
    int rc = free_entry(fm, dir_first, sc->index);

    if (rc != 0) {
        return rc;
    }
    for (i = sc->index; i > sc->start; i--) {
        rc = free_entry(fm, dir_first, i - 1);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* The chain of an entry that is gone: freed now, or left to the node's last
 * holder. */
static int retire_chain(struct vfs_mount *mnt, struct fat_mount *fm, uint32_t dir_cluster,
                        uint32_t index, uint32_t first)
{
    struct fat_node *n = node_at(fm, dir_cluster, index);

    (void)mnt;
    if (n && n->refs > 0) {
        n->unlinked = 1; /* the chain goes at the last put */
        return 0;
    }
    if (n) {
        n->used = 0;
    }
    return first ? free_chain(fm, first) : 0;
}

static int fat_unlink(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct fat_mount *fm = fm_of(mnt);
    struct fat_node *d = dir ? dir : mnt->root;
    uint32_t dir_cluster = d->first == fm->root_cluster ? 0 : d->first;
    struct fat_scan sc;
    int got, rc;

    if (!fm->writable || (mnt->flags & VFS_MS_RDONLY)) {
        return -EROFS;
    }
    assert_locked(mnt);
    got = find_entry(fm, d, d->first, name, &sc);
    if (got < 0) {
        return got;
    }
    if (got == 0) {
        return -ENOENT;
    }
    if (sc.ent.raw[11] & ATTR_DIRECTORY) {
        return -EISDIR;
    }
    rc = mark_dirty(fm);
    if (rc == 0) {
        rc = free_name_entries(fm, d->first, &sc);
    }
    if (rc != 0) {
        return rc;
    }
    return retire_chain(mnt, fm, dir_cluster, sc.index, entry_first(sc.ent.raw));
}

static int fat_mkdir(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode)
{
    struct fat_mount *fm = fm_of(mnt);
    struct fat_node *d = dir ? dir : mnt->root;
    uint32_t parent_cluster = d->first == fm->root_cluster ? 0 : d->first;
    struct fat_node n;
    static struct new_name nn;
    uint32_t c, unused, index;
    uint8_t raw[64];
    int rc;

    (void)mode;
    if (!fm->writable || (mnt->flags & VFS_MS_RDONLY)) {
        return -EROFS;
    }
    assert_locked(mnt);
    rc = dir_has_name(fm, d, name);
    if (rc != 0) {
        return rc > 0 ? -EEXIST : rc;
    }
    k_memset(&n, 0, sizeof(n));
    rc = prepare_name(fm, d, d->first, name, &nn);
    if (rc != 0) {
        return rc;
    }
    k_memcpy(n.name11, nn.name11, 11);
    n.lower = nn.lower;
    rc = mark_dirty(fm);
    if (rc != 0) {
        return rc;
    }
    /* The cluster, zeroed and ended, with `.` and `..`, then the parent's
     * entry last; a crash before it leaks the cluster. */
    rc = alloc_run(fm, 1, 2, &c, &unused);
    if (rc != 0) {
        return rc;
    }
    n.is_dir = 1;
    n.attr = ATTR_DIRECTORY;
    n.first = c;
    fat_now(&n.cdate, &n.ctime, &n.ctenth);
    n.date = n.cdate;
    n.time = n.ctime;
    n.adate = n.cdate;
    entry_of(&n, raw);
    k_memcpy(raw, ".          ", 11);
    raw[12] = 0;
    entry_of(&n, raw + 32);
    k_memcpy(raw + 32, "..         ", 11);
    raw[32 + 12] = 0;
    put16(raw + 32 + 20, parent_cluster >> 16);
    put16(raw + 32 + 26, parent_cluster & 0xFFFFu);
    rc = fat_put_bytes(fm, cluster_offset(fm, c), raw, 64, 1);
    if (rc == 0) {
        rc = free_slots(fm, d, d->first, nn.entries + 1, &index);
    }
    if (rc == 0) {
        n.dir_cluster = parent_cluster;
        n.index = index + nn.entries;
        entry_of(&n, raw);
        rc = write_name_entries(fm, d->first, index, &nn, raw);
    }
    if (rc != 0) {
        free_chain(fm, c);
        return rc;
    }
    return 0;
}

static int fat_rmdir(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct fat_mount *fm = fm_of(mnt);
    struct fat_node *d = dir ? dir : mnt->root;
    struct fat_scan sc;
    struct fat_node *held;
    uint32_t first;
    int got, has, rc;

    if (!fm->writable || (mnt->flags & VFS_MS_RDONLY)) {
        return -EROFS;
    }
    assert_locked(mnt);
    got = find_entry(fm, d, d->first, name, &sc);
    if (got < 0) {
        return got;
    }
    if (got == 0) {
        return -ENOENT;
    }
    if (!(sc.ent.raw[11] & ATTR_DIRECTORY)) {
        return -ENOTDIR;
    }
    first = entry_first(sc.ent.raw);
    rc = dir_has_entries(fm, first, &has);
    if (rc != 0) {
        return rc;
    }
    if (has) {
        return -ENOTEMPTY;
    }
    held = dir_node_at(fm, first);
    if (held && held->refs > 0) {
        return -EBUSY; /* a description or a working directory is in it */
    }
    rc = mark_dirty(fm);
    if (rc == 0) {
        rc = free_name_entries(fm, d->first, &sc);
    }
    if (rc != 0) {
        return rc;
    }
    if (held) {
        held->used = 0;
    }
    return free_chain(fm, first);
}

/* Whether the directory at cluster `dir` (0 for the root) is `anc` or lies
 * under it, by the `..` chain up to the root. 1, 0, or a negative error. */
static int is_under(struct fat_mount *fm, uint32_t dir, uint32_t anc)
{
    uint32_t steps = 0;

    while (dir != 0 && steps++ < fm->cluster_count) {
        struct fat_dirent e;

        if (dir == anc) {
            return 1;
        }
        if (dir_entry(fm, 0, dir, 1, &e) != 1 || !raw_eq(e.raw, (const uint8_t *)"..         ")) {
            return -EIO;
        }
        dir = entry_first(e.raw);
        if (dir == fm->root_cluster) {
            dir = 0;
        }
    }
    return 0;
}

/* Rename within one mount. In one directory the entry is rewritten in
 * place; otherwise the destination is freed if it exists, the source entry
 * is freed, a moved directory's `..` is rewritten, and the new entry is
 * written last, so a crash between the two leaves a nameless chain for
 * the walk and never two names on one chain. */
static int fat_rename(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
                      const char *newname)
{
    struct fat_mount *fm = fm_of(mnt);
    struct fat_node *od = olddir ? olddir : mnt->root;
    struct fat_node *nd = newdir ? newdir : mnt->root;
    uint32_t od_cluster = od->first == fm->root_cluster ? 0 : od->first;
    uint32_t nd_cluster = nd->first == fm->root_cluster ? 0 : nd->first;
    struct fat_scan src, dst;
    struct fat_node *moved;
    static struct new_name nn;
    uint32_t index;
    int got, rc, is_dir, dst_exists;

    if (!fm->writable || (mnt->flags & VFS_MS_RDONLY)) {
        return -EROFS;
    }
    assert_locked(mnt);
    got = find_entry(fm, od, od->first, oldname, &src);
    if (got < 0) {
        return got;
    }
    if (got == 0) {
        return -ENOENT;
    }
    is_dir = (src.ent.raw[11] & ATTR_DIRECTORY) != 0;
    if (is_dir && (raw_eq(src.ent.raw, (const uint8_t *)".          ") ||
                   raw_eq(src.ent.raw, (const uint8_t *)"..         "))) {
        return -EINVAL;
    }
    got = find_entry(fm, nd, nd->first, newname, &dst);
    if (got < 0) {
        return got;
    }
    dst_exists = got == 1;
    if (dst_exists) {
        int dst_dir = (dst.ent.raw[11] & ATTR_DIRECTORY) != 0;

        if (od_cluster == nd_cluster && dst.index == src.index) {
            return 0; /* the same entry under a spelling of its own name */
        }
        if (is_dir && !dst_dir) {
            return -ENOTDIR;
        }
        if (!is_dir && dst_dir) {
            return -EISDIR;
        }
        if (dst_dir) {
            int has;

            rc = dir_has_entries(fm, entry_first(dst.ent.raw), &has);
            if (rc != 0) {
                return rc;
            }
            if (has) {
                return -ENOTEMPTY;
            }
            if (dir_node_at(fm, entry_first(dst.ent.raw))) {
                return -EBUSY;
            }
        }
    }
    if (is_dir && od_cluster != nd_cluster) {
        rc = is_under(fm, nd_cluster, entry_first(src.ent.raw));
        if (rc < 0) {
            return rc;
        }
        if (rc) {
            return -EINVAL; /* into itself or under itself */
        }
    }
    rc = mark_dirty(fm);
    if (rc != 0) {
        return rc;
    }
    /* The name and its room are decided with the destination still in
     * place, so ENOSPC destroys nothing; the alias avoids the
     * destination's, and its freed slots are simply free. */
    rc = prepare_name(fm, nd, nd->first, newname, &nn);
    if (rc != 0) {
        return rc;
    }
    moved = is_dir ? dir_node_at(fm, entry_first(src.ent.raw)) : node_at(fm, od_cluster, src.index);
    if (od_cluster == nd_cluster && !dst_exists && src.index - src.start == nn.entries) {
        /* In place: the same entries rewritten under the new name, long
         * ones first and the short one last. */
        k_memcpy(src.ent.raw, nn.name11, 11);
        src.ent.raw[12] = nn.lower;
        rc = write_name_entries(fm, od->first, src.start, &nn, src.ent.raw);
        if (rc == 0 && moved) {
            k_memcpy(moved->name11, nn.name11, 11);
            moved->lower = nn.lower;
        }
        return rc;
    }
    rc = free_slots(fm, nd, nd->first, nn.entries + 1, &index);
    if (rc != 0) {
        return rc;
    }
    if (dst_exists) {
        rc = free_name_entries(fm, nd->first, &dst);
        if (rc == 0) {
            if (dst.ent.raw[11] & ATTR_DIRECTORY) {
                rc = free_chain(fm, entry_first(dst.ent.raw));
            } else {
                rc = retire_chain(mnt, fm, nd_cluster, dst.index, entry_first(dst.ent.raw));
            }
        }
        if (rc != 0) {
            return rc;
        }
    }
    rc = free_name_entries(fm, od->first, &src);
    if (rc != 0) {
        return rc;
    }
    if (is_dir && od_cluster != nd_cluster) {
        uint8_t dotdot[32];
        uint32_t c = entry_first(src.ent.raw);

        rc = fat_read_bytes(fm, cluster_offset(fm, c) + 32, dotdot, 32);
        if (rc == 0) {
            put16(dotdot + 20, nd_cluster >> 16);
            put16(dotdot + 26, nd_cluster & 0xFFFFu);
            rc = fat_put_bytes(fm, cluster_offset(fm, c) + 32, dotdot, 32, 1);
        }
        if (rc != 0) {
            return rc;
        }
    }
    k_memcpy(src.ent.raw, nn.name11, 11);
    src.ent.raw[12] = nn.lower;
    rc = write_name_entries(fm, nd->first, index, &nn, src.ent.raw);
    if (rc != 0) {
        return rc;
    }
    if (moved) {
        k_memcpy(moved->name11, nn.name11, 11);
        moved->lower = nn.lower;
        moved->dir_cluster = nd_cluster;
        moved->index = index + nn.entries;
    }
    return 0;
}

/* The clean unmount: every dirty buffer written, FSInfo, then the dirty
 * flag cleared in both places. A failure leaves the volume marked dirty
 * and the mount where it was; nothing here is irreversible. */
static int fat_prepare_unmount(struct vfs_mount *mnt)
{
    struct fat_mount *fm = fm_of(mnt);
    int rc;

    if (!fm->writable) {
        return 0;
    }
    rc = bcache_sync(fm->dev);
    if (rc == 0 && !fm->free_known && fm->fsinfo_sector != 0) {
        rc = count_free(fm); /* so the count written is exact */
    }
    if (rc == 0) {
        rc = write_fsinfo(fm);
    }
    if (rc == 0 && fm->dirty_set) {
        rc = fat_flag(fm, 0);
    }
    if (rc != 0) {
        if (fm->dirty_set) {
            fat_flag(fm, 1); /* whatever was cleared goes back to dirty */
        }
        return rc;
    }
    fm->dirty_set = 0;
    return 0;
}

static void fat_unmount(struct vfs_mount *mnt)
{
    struct fat_mount *fm = fm_of(mnt);

    fm->used = 0;
    mnt->priv = 0;
    mnt->root = 0;
}


static int k_memcmp_bytes(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 1;
        }
    }
    return 0;
}

static int free_entry(struct fat_mount *fm, uint32_t dir_first, uint32_t index);

/* ---- the check of a dirty volume at mount ----
 *
 * A volume whose dirty flag is set was written by something that did not
 * unmount it. Before the mount is published the directory tree is walked
 * under one bitmap of 2^20 clusters, shared by every mount and used by one
 * walk at a time. The four states this driver's own order leaves behind a
 * crash are found and three of them repaired, each repair a metadata write
 * in an order a second crash cannot make worse: a surplus tail is ended at
 * the size's last cluster and then freed, orphan long-name entries are
 * freed, and a lagging mirror is rewritten from the active FAT. A lost
 * cluster is reported and left. Anything else (a cross-link, a loop, a
 * chain outside the volume or shorter than its size, a wrong `.` or `..`,
 * a name twice in one directory) is a state this driver never leaves, and
 * the volume is mounted read-only with a record naming it.
 */

#define WALK_CLUSTERS (1u << 20)
#define WALK_DEPTH 32
#define WALK_DIR_ENTRIES 4096 /* entries compared for a duplicate name */

static uint8_t walk_bits[WALK_CLUSTERS / 8];
static int walk_busy;
static wait_queue_t walk_wq;

static int walk_idle(void *arg)
{
    (void)arg;
    return !walk_busy;
}

static int bit_test_set(uint32_t c)
{
    uint8_t m = (uint8_t)(1u << (c & 7));

    if (walk_bits[c >> 3] & m) {
        return 1;
    }
    walk_bits[c >> 3] |= m;
    return 0;
}

#define WALK_TAILS 16

struct walk_state {
    struct fat_mount *fm;
    int repaired;  /* repairs written */
    uint32_t lost; /* clusters marked used that nothing reaches */
    const char *why;
    /* Surplus tails found by the walk, freed only after the whole tree is
     * walked: a tail that a later entry's chain enters is a cross-link,
     * and freeing it first would destroy that entry's clusters. */
    int ntails;
    uint32_t tail_prev[WALK_TAILS];
    uint32_t tail_first[WALK_TAILS];
};

/* Walk the chain at `first` for the entry `name11`, marking clusters;
 * `expect` clusters for a file (-1 for a directory). Returns the count, or
 * -1 for a bad chain. A surplus tail is walked and marked like the rest of
 * the chain and recorded for the repair that follows the walk. */
static long walk_chain(struct walk_state *w, uint32_t first, long expect)
{
    struct fat_mount *fm = w->fm;
    uint32_t c = first;
    uint32_t prev = 0;
    long count = 0;

    while (c != 0) {
        uint32_t next;
        int rc;

        if (c < 2 || c >= fm->cluster_count + 2) {
            w->why = "a chain outside the volume";
            return -1;
        }
        if (bit_test_set(c)) {
            w->why = "a cluster reached twice";
            return -1;
        }
        count++;
        if (expect >= 0 && count == expect + 1) {
            /* The surplus tail this driver's extend leaves behind a crash. */
            if (w->ntails == WALK_TAILS) {
                w->why = "more surplus tails than the walk repairs";
                return -1;
            }
            w->tail_prev[w->ntails] = prev;
            w->tail_first[w->ntails] = c;
            w->ntails++;
        }
        rc = fat_next(fm, c, &next);
        if (rc != 0) {
            w->why = "a chain ending in a bad or free entry";
            return -1;
        }
        prev = c;
        c = next;
    }
    if (expect >= 0 && count < expect) {
        w->why = "a chain shorter than its size";
        return -1;
    }
    return count;
}

/* One directory: its entries checked, its files' chains walked, its
 * subdirectories returned one at a time through `sub` with `*resume` at
 * the entry after them, so the walk keeps its own small stack. */
static int walk_dir(struct walk_state *w, uint32_t first, uint32_t parent, uint32_t *resume,
                    uint32_t *sub)
{
    struct fat_mount *fm = w->fm;
    uint32_t i = *resume;
    uint32_t lfn_start = 0;
    int lfn_ok = 0, lfn_expect = 0, lfn_pending = 0;
    uint8_t lfn_sum = 0;

    *sub = 0;
    for (;; i++) {
        struct fat_dirent e;
        int got = dir_entry(fm, 0, first, i, &e);
        uint32_t fc;
        int is_dir;

        if (got < 0) {
            w->why = "an unreadable directory";
            return -1;
        }
        if (got == 0 || e.raw[0] == 0 || e.raw[0] == 0xE5) {
            if (lfn_pending) {
                /* Long-name entries with no short entry after them: what a
                 * crash in the middle of a name leaves; freed, last first. */
                uint32_t k;

                for (k = i; k > lfn_start; k--) {
                    if (free_entry(fm, first, k - 1) != 0) {
                        w->why = "a repair write failed";
                        return -1;
                    }
                }
                w->repaired++;
                klog(KLOG_WARN, "vfat", "check: orphan long-name entries freed");
            }
            lfn_ok = 0;
            lfn_pending = 0;
            if (got == 0 || e.raw[0] == 0) {
                *resume = i;
                return 0;
            }
            continue;
        }
        if (e.raw[11] == ATTR_LFN) {
            int seq = e.raw[0] & 0x1F;

            if (!lfn_pending) {
                lfn_start = i;
            }
            lfn_pending++;
            if (e.raw[0] & 0x40) {
                lfn_ok = seq >= 1 && (e.raw[0] & 0xA0) == 0;
                lfn_expect = seq;
                lfn_sum = e.raw[13];
            } else if (!lfn_ok || seq != lfn_expect || (e.raw[0] & 0xE0) != 0 ||
                       e.raw[13] != lfn_sum) {
                lfn_ok = 0;
            }
            if (lfn_ok) {
                lfn_expect = seq - 1;
            }
            continue;
        }
        if (e.raw[11] & ATTR_VOLUME) {
            lfn_ok = 0;
            lfn_pending = 0;
            continue;
        }
        if (lfn_pending && !(lfn_ok && lfn_expect == 0 && short_checksum(e.raw) == lfn_sum)) {
            uint32_t k;

            for (k = i; k > lfn_start; k--) {
                if (free_entry(fm, first, k - 1) != 0) {
                    w->why = "a repair write failed";
                    return -1;
                }
            }
            w->repaired++;
            klog(KLOG_WARN, "vfat", "check: orphan long-name entries freed");
        }
        lfn_ok = 0;
        lfn_pending = 0;
        fc = entry_first(e.raw);
        is_dir = (e.raw[11] & ATTR_DIRECTORY) != 0;
        if (raw_eq(e.raw, (const uint8_t *)".          ")) {
            if (fc != first) {
                w->why = "a directory whose . is not itself";
                return -1;
            }
            continue;
        }
        if (raw_eq(e.raw, (const uint8_t *)"..         ")) {
            if (fc != parent) {
                w->why = "a directory whose .. is not its parent";
                return -1;
            }
            continue;
        }
        {
            /* The same name twice in one directory. */
            uint32_t k;

            if (i >= WALK_DIR_ENTRIES) {
                w->why = "a directory larger than the walk compares";
                return -1;
            }
            for (k = 0; k < i; k++) {
                struct fat_dirent o;

                if (dir_entry(fm, 0, first, k, &o) != 1) {
                    break;
                }
                if (o.raw[0] != 0xE5 && o.raw[11] != ATTR_LFN && !(o.raw[11] & ATTR_VOLUME) &&
                    raw_eq(o.raw, e.raw)) {
                    w->why = "a name twice in one directory";
                    return -1;
                }
            }
        }
        if (is_dir) {
            if (fc == 0) {
                w->why = "a directory with no cluster";
                return -1;
            }
            if (walk_chain(w, fc, -1) < 0) {
                return -1;
            }
            *resume = i + 1;
            *sub = fc;
            return 1;
        }
        if (fc != 0) {
            uint32_t cb = cluster_bytes(fm);
            /* A size of zero with a cluster expects none: all surplus. */
            long expect = (long)(le32(e.raw + 28) == 0 ? 0 : (le32(e.raw + 28) - 1) / cb + 1);

            if (walk_chain(w, fc, expect) < 0) {
                return -1;
            }
        } else if (le32(e.raw + 28) != 0) {
            w->why = "a size with no chain";
            return -1;
        }
    }
}

/* The mirror rewritten from the active FAT, one cache block at a time. */
static int rewrite_mirror(struct fat_mount *fm)
{
    uint32_t other = fm->active_fat == 0 ? 1 : 0;
    uint64_t bytes = (uint64_t)fm->fatsz * BLOCK_SECTOR_SIZE;
    uint64_t at = 0;
    int differ = 0;

    while (at < bytes) {
        uint8_t a[512], b[512];
        uint32_t take = bytes - at < 512 ? (uint32_t)(bytes - at) : 512;
        int rc = fat_read_bytes(fm, fat_offset(fm, fm->active_fat, 0) + at, a, take);

        if (rc == 0) {
            rc = fat_read_bytes(fm, fat_offset(fm, other, 0) + at, b, take);
        }
        if (rc != 0) {
            return rc;
        }
        if (k_memcmp_bytes(a, b, take) != 0) {
            differ = 1;
            rc = fat_put_bytes(fm, fat_offset(fm, other, 0) + at, a, take, 1);
            if (rc != 0) {
                return rc;
            }
        }
        at += take;
    }
    return differ;
}

/* The check itself: the walk, the lost count, the mirror; 0 with the
 * volume consistent or repaired, 1 with it bad. */
static int fat_check_volume(struct fat_mount *fm)
{
    struct walk_state w;
    uint32_t stack_first[WALK_DEPTH], stack_parent[WALK_DEPTH], stack_resume[WALK_DEPTH];
    int depth = 0;
    int i;
    uint32_t c;

    k_memset(&w, 0, sizeof(w));
    w.fm = fm;
    if (fm->cluster_count + 2 > WALK_CLUSTERS) {
        klog(KLOG_WARN, "vfat", "check: %d clusters is more than the walk covers; read-only",
             (int)fm->cluster_count);
        return 1;
    }
    while (walk_busy) {
        wq_wait_event_nointr(&walk_wq, walk_idle, 0);
    }
    walk_busy = 1;
    k_memset(walk_bits, 0, (fm->cluster_count + 2 + 7) / 8);
    klog(KLOG_INFO, "vfat", "check: the volume was not unmounted cleanly; checking");
    if (walk_chain(&w, fm->root_cluster, -1) < 0) {
        goto done;
    }
    stack_first[0] = fm->root_cluster;
    stack_parent[0] = 0;
    stack_resume[0] = 0;
    depth = 1;
    while (depth > 0) {
        uint32_t sub;
        int got = walk_dir(&w, stack_first[depth - 1], stack_parent[depth - 1],
                           &stack_resume[depth - 1], &sub);

        if (got < 0) {
            goto done;
        }
        if (got == 0) {
            depth--;
            continue;
        }
        if (depth == WALK_DEPTH) {
            w.why = "a directory tree deeper than the walk";
            goto done;
        }
        stack_first[depth] = sub;
        stack_parent[depth] = stack_first[depth - 1] == fm->root_cluster ? 0 : stack_first[depth - 1];
        stack_resume[depth] = 0;
        depth++;
    }
    /* The surplus tails, now that the walk has shown nothing else reaches
     * them: the size's last cluster is ended first, then the tail freed. */
    for (i = 0; i < w.ntails; i++) {
        int rc = fat_set(fm, w.tail_prev[i], FAT_EOC_MIN);

        if (rc == 0) {
            rc = free_chain(fm, w.tail_first[i]);
        }
        if (rc != 0) {
            w.why = "a repair write failed";
            goto done;
        }
        w.repaired++;
        klog(KLOG_WARN, "vfat", "check: surplus tail freed");
    }
    {
        uint32_t freecount = 0;

        for (c = 2; c < fm->cluster_count + 2; c++) {
            uint32_t v;
            uint8_t raw[4];

            if (fat_read_bytes(fm, fat_offset(fm, fm->active_fat, c), raw, 4) != 0) {
                w.why = "an unreadable FAT";
                goto done;
            }
            v = le32(raw) & FAT_MASK;
            /* A marked cluster can be free: a surplus tail is marked by the
             * walk and then freed by its repair. */
            if (v == 0) {
                freecount++;
            } else if (v != FAT_BAD && !(walk_bits[c >> 3] & (1u << (c & 7)))) {
                w.lost++;
            }
        }
        /* The count the walk found is the count FSInfo says from now on;
         * a crash leaves the sector's count where the last sync put it. */
        fm->free_clusters = freecount;
        fm->free_known = 1;
        if (write_fsinfo(fm) != 0) {
            w.why = "a repair write failed";
            goto done;
        }
    }
    if (w.lost) {
        klog(KLOG_WARN, "vfat", "check: %d lost cluster(s) left for a host tool", (int)w.lost);
    }
    if (fm->mirrored && fm->nfats == 2) {
        int rc = rewrite_mirror(fm);

        if (rc < 0) {
            w.why = "a repair write failed";
            goto done;
        }
        if (rc == 1) {
            w.repaired++;
            klog(KLOG_WARN, "vfat", "check: the mirror FAT rewritten from the active one");
        }
    }
done:
    walk_busy = 0;
    wq_wake_all(&walk_wq);
    if (w.why) {
        klog(KLOG_WARN, "vfat", "check: %s; mounted read-only", w.why);
        return 1;
    }
    klog(KLOG_INFO, "vfat", "check: consistent, %d repair(s)", w.repaired);
    return 0;
}

/* ---- the constructor ---- */

static int key_is(const char *key, const char *end, const char *word)
{
    while (key < end && *word && *key == *word) {
        key++;
        word++;
    }
    return key == end && *word == '\0';
}

/* `umask=`, `uid=` and `gid=` are the options Linux's vfat takes and
 * BusyBox passes through; `ro` is the flag given as data. Anything else
 * is refused. */
static int parse_options(struct fat_mount *fm, const char *options)
{
    const char *p = options ? options : "";

    fm->umask = 022;
    fm->uid = 0;
    fm->gid = 0;
    while (*p) {
        const char *key = p;
        const char *keyend;
        uint32_t value = 0;
        int base = 10;
        int digits = 0;

        int has_value = 0;

        while (*p && *p != ',' && *p != '=') {
            p++;
        }
        keyend = p;
        if (*p == '=') {
            p++;
            has_value = 1;
            if (key_is(key, keyend, "umask")) {
                base = 8;
            }
            while (*p >= '0' && *p < '0' + base) {
                uint32_t digit = (uint32_t)(*p - '0');

                if (value > (0xFFFFFFFFu - digit) / (uint32_t)base) {
                    return -EINVAL; /* past 32 bits */
                }
                value = value * (uint32_t)base + digit;
                p++;
                digits++;
            }
        }
        if (*p != '\0' && *p != ',') {
            return -EINVAL;
        }
        if (key_is(key, keyend, "ro") && !has_value) {
            /* the mount flag given as data */
        } else if (key_is(key, keyend, "umask") && digits) {
            fm->umask = value & 0777u;
        } else if (key_is(key, keyend, "uid") && digits) {
            fm->uid = value;
        } else if (key_is(key, keyend, "gid") && digits) {
            fm->gid = value;
        } else if (p == key) {
            /* an empty item, as in a trailing comma */
        } else {
            return -EINVAL;
        }
        if (*p == ',') {
            p++;
        }
    }
    return 0;
}

static int refuse(const char *field)
{
    klog(KLOG_WARN, "vfat", "refused: %s", field);
    return -EINVAL;
}

/**
 * The volume check, field by field, before anything is published.
 */
static int fat_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                     const char *options)
{
    struct fat_mount *fm = 0;
    uint8_t bpb[512];
    uint32_t bytes_per_sector, total, fatsz16, fatsz, root_entries;
    uint32_t ext_flags, version;
    struct fat_node *root;
    int i;
    int rc;

    for (i = 0; i < FAT32_MOUNTS; i++) {
        if (!fat_mounts[i].used) {
            fm = &fat_mounts[i];
            break;
        }
    }
    if (!fm) {
        return -ENOMEM;
    }
    k_memset(fm, 0, sizeof(*fm));
    fm->dev = dev;
    rc = parse_options(fm, options);
    if (rc != 0) {
        return rc;
    }
    rc = fat_read_bytes(fm, 0, bpb, sizeof(bpb));
    if (rc != 0) {
        return rc;
    }
    if (bpb[510] != 0x55 || bpb[511] != 0xAA) {
        return refuse("boot-sector signature");
    }
    bytes_per_sector = le16(bpb + 11);
    fm->spc = bpb[13];
    fm->reserved = le16(bpb + 14);
    fm->nfats = bpb[16];
    root_entries = le16(bpb + 17);
    total = le16(bpb + 19);
    if (total == 0) {
        total = le32(bpb + 32);
    }
    fatsz16 = le16(bpb + 22);
    fatsz = le32(bpb + 36);
    ext_flags = le16(bpb + 40);
    version = le16(bpb + 42);
    fm->root_cluster = le32(bpb + 44);

    /* The recognition rule Linux uses: FAT32 is what says it is, whatever
     * its cluster count; the 65525 threshold is a formatting rule. */
    if (fatsz16 != 0 || fatsz == 0 || root_entries != 0 || version != 0) {
        return refuse("not a FAT32 volume");
    }
    if (bytes_per_sector != 512) {
        return refuse("sector size");
    }
    if (fm->spc == 0 || fm->spc > 64 || (fm->spc & (fm->spc - 1)) != 0) {
        return refuse("sectors per cluster");
    }
    if (fm->reserved == 0) {
        return refuse("reserved sectors");
    }
    if (fm->nfats != 1 && fm->nfats != 2) {
        return refuse("FAT count");
    }
    if (total == 0 || (uint64_t)total > dev->sector_count) {
        return refuse("total sectors");
    }
    if (fm->reserved >= total || fatsz > (total - fm->reserved) / fm->nfats) {
        return refuse("FAT regions");
    }
    fm->total = total;
    fm->fatsz = fatsz;
    fm->data_start = fm->reserved + fm->nfats * fatsz;
    fm->cluster_count = (total - fm->data_start) / fm->spc;
    if (fm->cluster_count == 0) {
        return refuse("data region");
    }
    if (fm->cluster_count > FAT_MAX_CLUSTERS) {
        return refuse("cluster count");
    }
    if ((uint64_t)fatsz * 128 < (uint64_t)fm->cluster_count + 2) {
        return refuse("FAT size");
    }
    if (fm->root_cluster < 2 || fm->root_cluster >= fm->cluster_count + 2) {
        return refuse("root cluster");
    }
    /* With mirroring on the first FAT is read; with it off, the one the
     * flags name. */
    fm->active_fat = (ext_flags & 0x80) ? (ext_flags & 0xF) : 0;
    if (fm->active_fat >= fm->nfats) {
        return refuse("active FAT");
    }
    fm->mirrored = (ext_flags & 0x80) == 0;
    fm->fsinfo_sector = le16(bpb + 48);
    if (fm->fsinfo_sector >= fm->reserved) {
        fm->fsinfo_sector = 0; /* not inside the reserved region: none */
    }
    if (fm->fsinfo_sector != 0) {
        uint8_t fi[512];

        rc = fat_read_bytes(fm, (uint64_t)fm->fsinfo_sector * BLOCK_SECTOR_SIZE, fi, sizeof(fi));
        if (rc != 0 || le32(fi) != 0x41615252u || le32(fi + 484) != 0x61417272u) {
            fm->fsinfo_sector = 0; /* no signature: nothing there to keep */
        } else {
            uint32_t hint = le32(fi + 492);

            fm->alloc_hint = hint >= 2 && hint < fm->cluster_count + 2 ? hint : 2;
        }
    }
    if (fm->alloc_hint < 2) {
        fm->alloc_hint = 2;
    }
    fm->writable = (flags & VFS_MS_RDONLY) == 0;
    fm->mnt = mnt;
    {
        uint8_t raw[4];
        uint8_t byte;
        int dirty;

        rc = fat_read_bytes(fm, fat_offset(fm, fm->active_fat, 1), raw, 4);
        if (rc == 0) {
            rc = fat_read_bytes(fm, 65, &byte, 1);
        }
        if (rc != 0) {
            return rc;
        }
        dirty = !(le32(raw) & 0x08000000u) || (byte & 1);
        if (dirty && fm->writable && fat_check_volume(fm) != 0) {
            fm->writable = 0;
            mnt->flags |= VFS_MS_RDONLY;
        } else if (dirty && fm->writable) {
            fm->dirty_set = 1; /* already on the disk; the clean unmount clears it */
        }
    }

    root = node_take(fm);
    root->is_dir = 1;
    root->first = fm->root_cluster;
    fm->used = 1;
    mnt->priv = fm;
    mnt->root = root;
    return 0;
}

/* The path of a directory node, rebuilt from the volume: the parent by the
 * `..` entry, the name by the entry in the parent whose first cluster is
 * the child's, up to the root. What a directory descriptor's `*at` calls
 * resolve against, so they follow a rename of the directory. */
static int fat_path(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct fat_mount *fm = fm_of(mnt);
    struct fat_node *n = node;
    uint32_t c = n->first;
    uint32_t len = 0;
    int depth;

    if (!n->is_dir) {
        return -ENOTDIR;
    }
    buf[0] = '\0';
    for (depth = 0; c != fm->root_cluster && depth < WALK_DEPTH; depth++) {
        struct fat_dirent dotdot;
        uint32_t parent, cursor = 0;
        struct fat_scan sc;
        int found = 0;
        int rc = fat_read_bytes(fm, cluster_offset(fm, c) + 32, dotdot.raw, 32);

        if (rc != 0) {
            return rc;
        }
        parent = entry_first(dotdot.raw);
        if (parent == 0) {
            parent = fm->root_cluster;
        }
        for (;;) {
            int got = dir_scan(fm, 0, parent == fm->root_cluster ? 0 : parent, &cursor, &sc);

            if (got < 0) {
                return got;
            }
            if (got == 0) {
                break;
            }
            if ((sc.ent.raw[11] & ATTR_DIRECTORY) && entry_first(sc.ent.raw) == c &&
                !raw_eq(sc.ent.raw, (const uint8_t *)".          ") &&
                !raw_eq(sc.ent.raw, (const uint8_t *)"..         ")) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return -ENOENT;
        }
        {
            uint32_t nl = k_strlen(sc.name);

            if (len + nl + 1 >= cap) {
                return -ENAMETOOLONG;
            }
            k_memmove(buf + nl + 1, buf, len + 1);
            buf[0] = '/';
            k_memcpy(buf + 1, sc.name, nl);
            len += nl + 1;
        }
        c = parent;
    }
    if (depth == WALK_DEPTH) {
        return -ENAMETOOLONG;
    }
    if (len == 0) {
        buf[0] = '/';
        buf[1] = '\0';
    }
    return 0;
}

static const struct vfs_ops vfat_ops = {
    fat_lookup,
    0, /* lookup_rest */
    fat_hold,
    fat_put,
    fat_getattr,
    fat_setattr,
    fat_open,
    fat_release,
    fat_read,
    fat_write,
    fat_truncate,
    fat_fsync,
    fat_readdir,
    fat_create,
    fat_unlink,
    fat_mkdir,
    fat_rmdir,
    fat_rename,
    0, /* link */
    0, /* readlink */
    fat_statfs,
    fat_sync,
    fat_prepare_unmount,
    fat_unmount,
    fat_path,
};

const struct vfs_type vfs_vfat_type = {"vfat", fat_mount, &vfat_ops};
