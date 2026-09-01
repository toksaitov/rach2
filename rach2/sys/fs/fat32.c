/*
 * fat32.c: the `vfat` type, read-only. The contract is in fat32.h; what is
 * here is the volume check, the FAT walk, the directory reader with its
 * long names, and the node pool that gives the VFS something to hold.
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
#include "bcache.h"

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
    struct fat_node nodes[FAT32_NODES];
};

static struct fat_mount fat_mounts[FAT32_MOUNTS];

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

        if (!n->used || n->is_dir != is_dir) {
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

static void fat_put(struct vfs_mount *mnt, void *node)
{
    struct fat_node *n = node;

    (void)mnt;
    if (n->refs > 0) {
        n->refs--;
    }
    if (n->refs == 0) {
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
    char name[VFS_NAME_MAX + 1];
    char alias[13];    /* the short name as shown, always */
    int name_too_long; /* a long name past the component limit */
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
        out->name_too_long = 0;
        short_to_name(e.raw, out->alias);
        if (lfn_ok && lfn_expect == 0 && lfn_count > 0 && short_checksum(e.raw) == lfn_sum) {
            int rc = units_to_utf8(units, (uint32_t)lfn_count * 13, out->name, sizeof(out->name));

            if (rc != 0) {
                out->name_too_long = rc == -ENAMETOOLONG;
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
        unsigned char x = (unsigned char)*a++;
        unsigned char y = (unsigned char)*b++;

        if (x >= 'A' && x <= 'Z') {
            x = (unsigned char)(x + 32);
        }
        if (y >= 'A' && y <= 'Z') {
            y = (unsigned char)(y + 32);
        }
        if (x != y) {
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
    n->ctenth = sc->ent.raw[13];
    n->ctime = (uint16_t)le16(sc->ent.raw + 14);
    n->cdate = (uint16_t)le16(sc->ent.raw + 16);
    n->adate = (uint16_t)le16(sc->ent.raw + 18);
    n->time = (uint16_t)le16(sc->ent.raw + 22);
    n->date = (uint16_t)le16(sc->ent.raw + 24);
    return n;
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

            if (sc.name_too_long) {
                return -ENAMETOOLONG;
            }
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
    uint32_t y;
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
    for (y = 1970; y < year; y++) {
        days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
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
    st->uid = fm->uid;
    st->gid = fm->gid;
    st->nlink = 1;
    st->size = n->size;
    st->ino = fat_ino(n->is_dir, n->first, n->dir_cluster, n->index);
    st->mtime_sec = fat_epoch(n->date, n->time);
    st->ctime_sec = fat_epoch(n->cdate, n->ctime) + n->ctenth / 100;
    st->ctime_nsec = (int64_t)(n->ctenth % 100) * 10000000;
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
        int rc = count_free(fm);

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

static int fat_prepare_unmount(struct vfs_mount *mnt)
{
    (void)mnt;
    return 0; /* nothing is written on a read-only mount */
}

static void fat_unmount(struct vfs_mount *mnt)
{
    struct fat_mount *fm = fm_of(mnt);

    fm->used = 0;
    mnt->priv = 0;
    mnt->root = 0;
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

    if (!(flags & VFS_MS_RDONLY)) {
        return -EROFS; /* nothing writes a volume yet */
    }
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

    root = node_take(fm);
    root->is_dir = 1;
    root->first = fm->root_cluster;
    fm->used = 1;
    mnt->priv = fm;
    mnt->root = root;
    return 0;
}

static const struct vfs_ops vfat_ops = {
    fat_lookup,
    0, /* lookup_rest */
    fat_hold,
    fat_put,
    fat_getattr,
    0, /* setattr */
    fat_open,
    fat_release,
    fat_read,
    0, /* write */
    0, /* truncate */
    0, /* fsync */
    fat_readdir,
    0, /* create */
    0, /* unlink */
    0, /* mkdir */
    0, /* rmdir */
    0, /* rename */
    0, /* link */
    0, /* readlink */
    fat_statfs,
    0, /* sync */
    fat_prepare_unmount,
    fat_unmount,
};

const struct vfs_type vfs_vfat_type = {"vfat", fat_mount, &vfat_ops};
