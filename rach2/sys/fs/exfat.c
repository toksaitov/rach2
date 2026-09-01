/*
 * exfat.c: the `exfat` type, read. The contract is in exfat.h and
 * exfat.Readme.md; what is here is the volume check over both boot
 * regions, the root's objects, the up-case table in its three forms, the
 * entry sets and their invariants, chains with and without the FAT,
 * names, timestamps, and the node pool that gives the VFS something to
 * hold. The writer is phase 4's.
 */

#include "exfat.h"

#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "bcache.h"
#include "fatcommon.h"

#define EX_S_IFDIR 0040000u
#define EX_S_IFREG 0100000u

#define ATTR_READ_ONLY 0x0001u
#define ATTR_DIRECTORY 0x0010u

#define FLAG_ALLOC_POSSIBLE 0x01u
#define FLAG_NO_FAT_CHAIN   0x02u

#define EX_EOC 0xFFFFFFFFu
#define EX_BAD 0xFFFFFFF7u

#define UPCASE_HELD 16384u /* a stored table up to this size is kept per mount */
#define SET_MAX     19     /* the file entry, the stream, and up to 17 name entries */

struct ex_node {
    int used;
    int refs;
    int is_dir;
    uint32_t dir_cluster; /* the directory holding the set; 0 for the root */
    uint32_t index;       /* the set's file entry, an index in that directory */
    uint32_t parent;      /* a directory's parent directory, 0 for the root */
    uint32_t first;       /* first cluster; 0 for no clusters or the root */
    uint64_t length;      /* DataLength */
    uint64_t valid;       /* ValidDataLength; bytes past it read as zeros */
    int nofat;            /* the chain is contiguous and the FAT is not read */
    uint16_t attr;
    uint32_t ctime, mtime, atime; /* the timestamp fields as stored */
    uint8_t c10, m10, coff, moff, aoff;
    int unlinked;
    uint32_t hint_index;
    uint32_t hint_cluster;
};

struct ex_mount {
    int used;
    block_device_t *dev;
    struct vfs_mount *mnt;
    uint32_t spc_shift;
    uint32_t fat_offset, fat_length, heap_offset, clusters, root, active_fat;
    uint64_t volume_length;
    uint32_t uid, gid, umask;
    int writable;
    uint32_t bitmap_first;
    uint64_t bitmap_length;
    uint32_t upcase_first;
    uint64_t upcase_length;
    /* 0: the stored form held here and walked; 1: uncompressed, indexed
     * through the cache; 2: compressed and larger than held, streamed
     * through the cache from its beginning for every unit. */
    int upcase_form;
    uint8_t upcase_stored[UPCASE_HELD];
    int free_known;
    uint32_t free_clusters;
    struct ex_node nodes[EXFAT_NODES];
};

static struct ex_mount ex_mounts[EXFAT_MOUNTS];

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static struct ex_mount *em_of(struct vfs_mount *mnt)
{
    return mnt->priv;
}

static uint32_t cluster_bytes(const struct ex_mount *em)
{
    return BLOCK_SECTOR_SIZE << em->spc_shift;
}

static uint64_t cluster_offset(const struct ex_mount *em, uint32_t c)
{
    return ((uint64_t)em->heap_offset + ((uint64_t)(c - 2) << em->spc_shift)) * BLOCK_SECTOR_SIZE;
}

/* The clusters `bytes` take, by shift: the kernel has no 64-bit division. */
static uint64_t clusters_of(const struct ex_mount *em, uint64_t bytes)
{
    uint32_t shift = 9 + em->spc_shift;

    return (bytes + (((uint64_t)1 << shift) - 1)) >> shift;
}

static int in_heap(const struct ex_mount *em, uint32_t c)
{
    return c >= 2 && c < em->clusters + 2;
}

/* ---- the cache ---- */

static int ex_read_bytes(struct ex_mount *em, uint64_t off, void *dst, uint32_t len)
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
        b = bcache_get(em->dev, blk);
        if (!b) {
            return -EIO;
        }
        if (in + take > b->bytes) {
            bcache_release(b);
            return -EIO;
        }
        k_memcpy(out, b->data + in, take);
        bcache_release(b);
        out += take;
        off += take;
        len -= take;
    }
    return 0;
}

/* The FAT entry of `c`, from the active FAT. */
static int fat_next(struct ex_mount *em, uint32_t c, uint32_t *next)
{
    uint8_t raw[4];
    uint64_t off =
        ((uint64_t)em->fat_offset + (uint64_t)em->active_fat * em->fat_length) * BLOCK_SECTOR_SIZE +
        (uint64_t)c * 4;
    int rc = ex_read_bytes(em, off, raw, 4);

    if (rc != 0) {
        return rc;
    }
    *next = le32(raw);
    return 0;
}

/* Cluster `index` of a chain: `first + index` for a contiguous one, and
 * through the FAT otherwise, from the node's last position when it helps.
 * -ENXIO past the chain's end, -EIO for a chain that leaves the heap, a
 * bad or free entry, or a loop. */
static int chain_at(struct ex_mount *em, struct ex_node *n, uint32_t first, int nofat,
                    uint64_t length, uint32_t index, uint32_t *cluster)
{
    uint32_t c = first;
    uint32_t i = 0;
    uint32_t steps = 0;

    if (index >= em->clusters) {
        return -EIO;
    }
    if (nofat) {
        uint64_t have = clusters_of(em, length);

        if (index >= have) {
            return -ENXIO;
        }
        if (!in_heap(em, first + index)) {
            return -EIO;
        }
        *cluster = first + index;
        return 0;
    }
    if (n && n->hint_cluster && n->hint_index <= index) {
        c = n->hint_cluster;
        i = n->hint_index;
    }
    while (i < index) {
        uint32_t next;
        int rc = fat_next(em, c, &next);

        if (rc != 0) {
            return rc;
        }
        if (next == EX_EOC) {
            return -ENXIO;
        }
        if (next == EX_BAD || !in_heap(em, next)) {
            return -EIO;
        }
        if (++steps > em->clusters) {
            return -EIO;
        }
        c = next;
        i++;
    }
    if (n) {
        n->hint_index = index;
        n->hint_cluster = c;
    }
    *cluster = c;
    return 0;
}

/* ---- the node pool ---- */

static struct ex_node *node_take(struct ex_mount *em)
{
    int i;

    for (i = 0; i < EXFAT_NODES; i++) {
        if (!em->nodes[i].used) {
            k_memset(&em->nodes[i], 0, sizeof(em->nodes[i]));
            em->nodes[i].used = 1;
            em->nodes[i].refs = 1;
            return &em->nodes[i];
        }
    }
    return 0;
}

static struct ex_node *node_find(struct ex_mount *em, int is_dir, uint32_t dir_cluster,
                                 uint32_t index, uint32_t first)
{
    int i;

    for (i = 0; i < EXFAT_NODES; i++) {
        struct ex_node *n = &em->nodes[i];

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

static void ex_hold(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    ((struct ex_node *)node)->refs++;
}

static void ex_put(struct vfs_mount *mnt, void *node)
{
    struct ex_node *n = node;

    (void)mnt;
    if (n->refs > 0) {
        n->refs--;
    }
    if (n->refs == 0) {
        n->used = 0;
    }
}

/* ---- the up-case table ---- */

/* The stored table read two bytes at a time, from the mount's held copy
 * or through the cache. 1 with a unit, 0 at the end, or an error. */
struct upcase_reader {
    struct ex_mount *em;
    uint64_t at; /* the chunk's offset in the table */
    uint32_t have, pos;
    uint8_t chunk[512];
};

static int upcase_next(struct upcase_reader *r, uint16_t *w)
{
    if (r->pos + 2 > r->have) {
        uint64_t next = r->at + r->have;
        uint32_t take;

        if (next + 2 > r->em->upcase_length) {
            return 0;
        }
        take = r->em->upcase_length - next < sizeof(r->chunk)
                   ? (uint32_t)(r->em->upcase_length - next)
                   : (uint32_t)sizeof(r->chunk);
        if (r->em->upcase_form == 0) {
            k_memcpy(r->chunk, r->em->upcase_stored + next, take);
        } else {
            int rc = ex_read_bytes(r->em, cluster_offset(r->em, r->em->upcase_first) + next,
                                   r->chunk, take);

            if (rc != 0) {
                return rc;
            }
        }
        r->at = next;
        r->have = take;
        r->pos = 0;
    }
    *w = le16(r->chunk + r->pos);
    r->pos += 2;
    return 1;
}

/* The stored form's runs walked to `unit`: a 0xFFFF followed by a count is
 * that many identity mappings, anything else maps the next unit. */
static int upcase_walk(struct ex_mount *em, uint16_t unit, uint16_t *out)
{
    struct upcase_reader r;
    uint32_t u = 0;

    k_memset(&r, 0, sizeof(r));
    r.em = em;
    for (;;) {
        uint16_t w;
        int rc = upcase_next(&r, &w);

        if (rc <= 0) {
            break;
        }
        if (w == 0xFFFF) {
            uint16_t run;

            rc = upcase_next(&r, &run);
            if (rc <= 0) {
                break;
            }
            if (unit < u + run) {
                *out = unit;
                return 0;
            }
            u += run;
        } else {
            if (u == unit) {
                *out = w;
                return 0;
            }
            u++;
        }
    }
    *out = unit; /* past the table, which the check has excluded: identity */
    return 0;
}

static int ex_upcase(struct ex_mount *em, uint16_t unit, uint16_t *out)
{
    if (em->upcase_form == 1) {
        uint8_t raw[2];
        int rc =
            ex_read_bytes(em, cluster_offset(em, em->upcase_first) + (uint64_t)unit * 2, raw, 2);

        if (rc != 0) {
            return rc;
        }
        *out = le16(raw);
        return 0;
    }
    return upcase_walk(em, unit, out);
}

/* The name hash of `n` units, up-cased, as the specification computes it. */
static int name_hash(struct ex_mount *em, const uint16_t *units, uint32_t n, uint16_t *hash)
{
    uint16_t sum = 0;
    uint32_t i;

    for (i = 0; i < n; i++) {
        uint16_t u;
        int rc = ex_upcase(em, units[i], &u);

        if (rc != 0) {
            return rc;
        }
        sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + (u & 0xFF));
        sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + (u >> 8));
    }
    *hash = sum;
    return 0;
}

/* ---- directories and sets ---- */

/* Entry `index` of the directory at `first` (the root for 0), read through
 * its chain. 1 with `raw` filled, 0 past the chain's end, or an error. */
static int dir_entry(struct ex_mount *em, struct ex_node *d, uint32_t index, uint8_t *raw)
{
    uint32_t per = cluster_bytes(em) / 32;
    uint32_t c;
    int rc;

    if (d->first == 0) {
        return 0;
    }
    rc = chain_at(em, d, d->first, d->nofat, d->length, index / per, &c);
    if (rc == -ENXIO) {
        return 0;
    }
    if (rc != 0) {
        return rc;
    }
    rc = ex_read_bytes(em, cluster_offset(em, c) + (uint64_t)(index % per) * 32, raw, 32);
    return rc == 0 ? 1 : rc;
}

struct ex_set {
    uint32_t index;   /* of the file entry */
    uint32_t entries; /* the whole set */
    uint8_t file[32];
    uint8_t stream[32];
    uint16_t units[255];
    uint32_t count;
};

static uint16_t set_checksum(const uint8_t *entries, uint32_t count)
{
    uint16_t sum = 0;
    uint32_t i;

    for (i = 0; i < count * 32; i++) {
        if (i == 2 || i == 3) {
            continue;
        }
        sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + entries[i]);
    }
    return sum;
}

/* The set whose file entry is at `index`: 1 with `set` filled when every
 * invariant holds, 0 when the set is skipped with a record naming why, or
 * an error. `raw` is the file entry already read. */
static int set_read(struct ex_mount *em, struct ex_node *d, uint32_t index, const uint8_t *raw,
                    struct ex_set *set)
{
    uint8_t all[SET_MAX * 32];
    uint32_t secondaries = raw[1];
    uint32_t k, got = 0, names = 0;
    uint16_t hash;
    const char *why = 0;
    int rc;

    k_memset(all, 0, sizeof(all));
    if (secondaries < 2 || secondaries > SET_MAX - 1) {
        why = "SecondaryCount";
    } else {
        k_memcpy(all, raw, 32);
        for (k = 1; k <= secondaries && !why; k++) {
            rc = dir_entry(em, d, index + k, all + k * 32);
            if (rc < 0) {
                return rc;
            }
            if (rc == 0) {
                why = "a set past the directory's end";
            }
        }
    }
    if (!why && all[32] != 0xC0) {
        why = "no stream extension second";
    }
    if (!why && set_checksum(all, secondaries + 1) != le16(all + 2)) {
        why = "the set checksum";
    }
    if (!why) {
        const uint8_t *st = all + 32;
        uint32_t nlen = st[3];
        uint64_t length = le64(st + 24), valid = le64(st + 8);
        uint32_t first = le32(st + 20);

        for (k = 2; k <= secondaries; k++) {
            const uint8_t *ne = all + k * 32;
            uint32_t m;

            if (ne[0] != 0xC1) {
                continue;
            }
            names++;
            for (m = 0; m < 15; m++) {
                uint16_t u = le16(ne + 2 + m * 2);

                if (got < nlen) {
                    set->units[got++] = u;
                } else if (u != 0) {
                    why = "units past NameLength";
                }
            }
        }
        if (!why && (nlen == 0 || got != nlen || names != (nlen + 14) / 15)) {
            why = "NameLength";
        }
        if (!why && valid > length) {
            why = "ValidDataLength past DataLength";
        }
        if (!why && !(st[1] & FLAG_ALLOC_POSSIBLE)) {
            why = "AllocationPossible clear";
        }
        if (!why && length != 0 && !in_heap(em, first)) {
            why = "a first cluster outside the heap";
        }
        if (!why && length == 0 && first != 0) {
            why = "a cluster with no length";
        }
        if (!why && (st[1] & FLAG_NO_FAT_CHAIN) && length != 0 &&
            (uint64_t)first - 2 + clusters_of(em, length) > em->clusters) {
            why = "a contiguous run past the heap";
        }
        if (!why) {
            rc = name_hash(em, set->units, nlen, &hash);
            if (rc != 0) {
                return rc;
            }
            if (hash != le16(st + 4)) {
                why = "the name hash";
            }
        }
        if (!why) {
            set->index = index;
            set->entries = secondaries + 1;
            k_memcpy(set->file, all, 32);
            k_memcpy(set->stream, st, 32);
            set->count = nlen;
            return 1;
        }
    }
    klog(KLOG_WARN, "exfat", "set skipped: %s (entry %d of directory cluster %d)", why, (int)index,
         (int)d->first);
    return 0;
}

/* The next valid set at or after `*cursor`: 1 with the set and the cursor
 * past it, 0 at the directory's end, or an error. Entries not in use,
 * the root's system entries, and a set that fails its invariants are
 * passed over; a set is not split across a call. */
static int dir_scan(struct ex_mount *em, struct ex_node *d, uint32_t *cursor, struct ex_set *set)
{
    for (;;) {
        uint8_t raw[32];
        int rc = dir_entry(em, d, *cursor, raw);

        if (rc <= 0) {
            return rc;
        }
        if (raw[0] == 0x00) {
            return 0;
        }
        if (raw[0] != 0x85) {
            (*cursor)++;
            continue;
        }
        rc = set_read(em, d, *cursor, raw, set);
        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            (*cursor)++;
            continue;
        }
        *cursor += set->entries;
        return 1;
    }
}

static struct ex_node *node_for(struct ex_mount *em, struct ex_node *d, const struct ex_set *set)
{
    const uint8_t *st = set->stream;
    int is_dir = (le16(set->file + 4) & ATTR_DIRECTORY) != 0;
    uint32_t first = le32(st + 20);
    uint32_t dir_cluster = d->first == em->root ? 0 : d->first;
    struct ex_node *n = node_find(em, is_dir, dir_cluster, set->index, first);

    if (n) {
        return n;
    }
    n = node_take(em);
    if (!n) {
        return 0;
    }
    n->is_dir = is_dir;
    n->dir_cluster = dir_cluster;
    n->index = set->index;
    n->parent = dir_cluster;
    n->first = first;
    n->length = le64(st + 24);
    n->valid = le64(st + 8);
    n->nofat = (st[1] & FLAG_NO_FAT_CHAIN) != 0;
    n->attr = le16(set->file + 4);
    n->ctime = le32(set->file + 8);
    n->mtime = le32(set->file + 12);
    n->atime = le32(set->file + 16);
    n->c10 = set->file[20];
    n->m10 = set->file[21];
    n->coff = set->file[22];
    n->moff = set->file[23];
    n->aoff = set->file[24];
    return n;
}

/* ---- the operations ---- */

/* The directory whose first cluster is `first`, found by walking the tree
 * from the root, for a parent whose node has been put: a bounded search,
 * rare, that gives the node back held. */
static int find_dir(struct ex_mount *em, struct ex_node *root, uint32_t first, struct ex_node **out)
{
    uint32_t stack_first[32], stack_cursor[32];
    int depth = 1;

    stack_first[0] = root->first;
    stack_cursor[0] = 0;
    while (depth > 0) {
        struct ex_node d;
        struct ex_set set;
        int rc;

        k_memset(&d, 0, sizeof(d));
        d.is_dir = 1;
        d.first = stack_first[depth - 1];
        rc = dir_scan(em, &d, &stack_cursor[depth - 1], &set);
        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            depth--;
            continue;
        }
        if (le16(set.file + 4) & ATTR_DIRECTORY) {
            uint32_t sub = le32(set.stream + 20);

            if (sub == first) {
                struct ex_node *n = node_for(em, &d, &set);

                if (!n) {
                    return -ENOMEM;
                }
                *out = n;
                return 0;
            }
            if (depth < 32 && sub != 0) {
                stack_first[depth] = sub;
                stack_cursor[depth] = 0;
                depth++;
            }
        }
    }
    return -ENOENT;
}

static int units_match(struct ex_mount *em, const uint16_t *a, const uint16_t *b, uint32_t n,
                       int *same)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        uint16_t x, y;
        int rc = ex_upcase(em, a[i], &x);

        if (rc == 0) {
            rc = ex_upcase(em, b[i], &y);
        }
        if (rc != 0) {
            return rc;
        }
        if (x != y) {
            *same = 0;
            return 0;
        }
    }
    *same = 1;
    return 0;
}

static int ex_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    struct ex_mount *em = em_of(mnt);
    struct ex_node *d = dir ? dir : mnt->root;
    uint16_t units[FAT_NAME_UNITS];
    uint32_t count, cursor = 0;
    uint16_t hash;
    int rc;

    if (name[0] == '.' && name[1] == '\0') {
        d->refs++;
        *node = d;
        return 0;
    }
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        /* exFAT directories have no dot entries: the parent is the
         * directory this one was found in, which its node remembers. */
        struct ex_node *p;

        if (d->first == em->root || d->parent == 0) {
            ((struct ex_node *)mnt->root)->refs++;
            *node = mnt->root;
            return 0;
        }
        p = node_find(em, 1, 0, 0, d->parent);
        if (!p) {
            rc = find_dir(em, mnt->root, d->parent, &p);
            if (rc != 0) {
                return rc;
            }
        }
        *node = p;
        return 0;
    }
    rc = fat_name_units(name, units, &count);
    if (rc != 0) {
        return rc == -EINVAL ? -ENOENT : rc;
    }
    rc = name_hash(em, units, count, &hash);
    if (rc != 0) {
        return rc;
    }
    for (;;) {
        struct ex_set set;
        int same;

        rc = dir_scan(em, d, &cursor, &set);
        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            return -ENOENT;
        }
        if (set.count != count || le16(set.stream + 4) != hash) {
            continue;
        }
        rc = units_match(em, set.units, units, count, &same);
        if (rc != 0) {
            return rc;
        }
        if (same) {
            struct ex_node *n = node_for(em, d, &set);

            if (!n) {
                klog(KLOG_WARN, "exfat", "node pool full");
                return -ENOMEM;
            }
            *node = n;
            return 0;
        }
    }
}

/* The epoch second of an exFAT timestamp: the same bit fields as a DOS
 * date and time, the 10 ms increment, and the UTC offset when its valid
 * bit is set; without one the field is reported as if it were UTC. */
static int64_t ex_epoch(uint32_t ts, uint8_t inc, uint8_t off, int64_t *nsec)
{
    int64_t sec = fat_epoch((uint16_t)(ts >> 16), (uint16_t)(ts & 0xFFFF));
    uint32_t tenth = inc > 199 ? 0 : inc;

    sec += tenth / 100;
    *nsec = (int64_t)(tenth % 100) * 10000000;
    if (off & 0x80) {
        int units = off & 0x7F;

        if (units >= 64) {
            units -= 128; /* a signed seven-bit count of quarter hours */
        }
        sec -= (int64_t)units * 15 * 60;
    }
    return sec;
}

static uint64_t ex_ino(const struct ex_node *n)
{
    if (n->is_dir) {
        return n->first;
    }
    return ((uint64_t)(n->dir_cluster + 1) << 32) | (uint64_t)(n->index + 1);
}

static int ex_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    struct ex_mount *em = em_of(mnt);
    struct ex_node *n = node;

    k_memset(st, 0, sizeof(*st));
    st->mode = (n->is_dir ? EX_S_IFDIR : EX_S_IFREG) | (0777u & ~em->umask);
    if (n->attr & ATTR_READ_ONLY) {
        st->mode &= ~0222u;
    }
    st->uid = em->uid;
    st->gid = em->gid;
    st->nlink = 1;
    st->size = n->length;
    st->ino = ex_ino(n);
    st->mtime_sec = ex_epoch(n->mtime, n->m10, n->moff, &st->mtime_nsec);
    st->ctime_sec = ex_epoch(n->ctime, n->c10, n->coff, &st->ctime_nsec);
    st->atime_sec = ex_epoch(n->atime, 0, n->aoff, &st->atime_nsec);
    return 0;
}

static int ex_open(struct vfs_mount *mnt, void *node, int flags, void **handle)
{
    (void)mnt;
    (void)flags;
    ((struct ex_node *)node)->refs++;
    *handle = node;
    return 0;
}

static void ex_release(struct vfs_mount *mnt, void *handle)
{
    ex_put(mnt, handle);
}

/* Bytes within ValidDataLength come from the chain; bytes past it and up
 * to DataLength read as zeros, which is what the specification says of a
 * file whose tail was never written. */
static int32_t ex_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count,
                       uint64_t offset)
{
    struct ex_mount *em = em_of(mnt);
    struct ex_node *n = handle;
    uint32_t cbytes = cluster_bytes(em);
    uint8_t *out = buf;
    uint32_t done = 0;
    uint64_t size = n->length;

    if (n->is_dir) {
        return -EISDIR;
    }
    if (offset >= size) {
        return 0;
    }
    if (count > size - offset) {
        count = (uint32_t)(size - offset);
    }
    while (done < count) {
        uint64_t at = offset + done;
        uint32_t in = (uint32_t)(at & (cbytes - 1));
        uint32_t take = cbytes - in;
        uint32_t c;
        int rc;

        if (take > count - done) {
            take = count - done;
        }
        if (at >= n->valid) {
            k_memset(out + done, 0, take); /* never written: zeros, whatever the disk holds */
            done += take;
            continue;
        }
        if (at + take > n->valid) {
            take = (uint32_t)(n->valid - at);
        }
        rc = chain_at(em, n, n->first, n->nofat, n->length, (uint32_t)(at >> (9 + em->spc_shift)),
                      &c);
        if (rc != 0) {
            return done ? (int32_t)done : -EIO;
        }
        rc = ex_read_bytes(em, cluster_offset(em, c) + in, out + done, take);
        if (rc != 0) {
            return done ? (int32_t)done : rc;
        }
        done += take;
    }
    return (int32_t)done;
}

static int ex_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor, struct vfs_dirent *ent)
{
    struct ex_mount *em = em_of(mnt);
    struct ex_node *d = handle;
    struct ex_set set;
    int rc;

    if (!d->is_dir) {
        return -ENOTDIR;
    }
    rc = dir_scan(em, d, cursor, &set);
    if (rc <= 0) {
        return rc;
    }
    rc = fat_units_utf8(set.units, set.count, ent->name, sizeof(ent->name));
    if (rc == -ENAMETOOLONG) {
        /* A name past the kernel's component: shown by its first bytes, and
         * unreachable by path, as the Readme says. */
        set.count = 60;
        (void)fat_units_utf8(set.units, set.count, ent->name, sizeof(ent->name));
    } else if (rc != 0) {
        k_strncpy(ent->name, "?", sizeof(ent->name));
    }
    ent->is_dir = (le16(set.file + 4) & ATTR_DIRECTORY) != 0;
    {
        struct ex_node probe;

        k_memset(&probe, 0, sizeof(probe));
        probe.is_dir = ent->is_dir;
        probe.first = le32(set.stream + 20);
        probe.dir_cluster = d->first == em->root ? 0 : d->first;
        probe.index = set.index;
        ent->ino = ex_ino(&probe);
    }
    return 1;
}

/* Free clusters are the bitmap's zero bits, counted once and kept. */
static int count_free(struct ex_mount *em)
{
    uint64_t off = cluster_offset(em, em->bitmap_first);
    uint32_t c = 0, free_count = 0;

    while (c < em->clusters) {
        uint8_t raw[512];
        uint32_t take = (em->clusters - c + 7) / 8;
        uint32_t i;
        int rc;

        if (take > sizeof(raw)) {
            take = sizeof(raw);
        }
        rc = ex_read_bytes(em, off + c / 8, raw, take);
        if (rc != 0) {
            return rc;
        }
        for (i = 0; i < take * 8 && c < em->clusters; i++, c++) {
            if (!(raw[i / 8] & (1u << (i % 8)))) {
                free_count++;
            }
        }
    }
    em->free_clusters = free_count;
    em->free_known = 1;
    return 0;
}

static int ex_statfs(struct vfs_mount *mnt, struct vfs_statfs *st)
{
    struct ex_mount *em = em_of(mnt);

    if (!em->free_known) {
        int rc = count_free(em);

        if (rc != 0) {
            return rc;
        }
    }
    st->magic = 0x2011BAB0u; /* EXFAT_SUPER_MAGIC, what Linux reports */
    st->bsize = cluster_bytes(em);
    st->blocks = em->clusters;
    st->bfree = em->free_clusters;
    st->namelen = VFS_NAME_MAX;
    return 0;
}

static void ex_unmount(struct vfs_mount *mnt)
{
    struct ex_mount *em = em_of(mnt);

    em->used = 0;
    mnt->priv = 0;
    mnt->root = 0;
}

/* ---- the constructor ---- */

static int refused(const char *field)
{
    klog(KLOG_WARN, "exfat", "refused: %s", field);
    return -EINVAL;
}

/* The boot checksum of the region at sector `first`: the rotate-and-add
 * sum over its eleven sectors, skipping VolumeFlags and PercentInUse. */
static int boot_checksum(struct ex_mount *em, uint64_t first, uint32_t *sum)
{
    uint32_t s = 0;
    uint32_t sec, i;

    for (sec = 0; sec < 11; sec++) {
        uint8_t raw[512];
        int rc = ex_read_bytes(em, (first + sec) * BLOCK_SECTOR_SIZE, raw, sizeof(raw));

        if (rc != 0) {
            return rc;
        }
        for (i = 0; i < sizeof(raw); i++) {
            if (sec == 0 && (i == 106 || i == 107 || i == 112)) {
                continue;
            }
            s = ((s & 1) ? 0x80000000u : 0) + (s >> 1) + raw[i];
        }
    }
    *sum = s;
    return 0;
}

/* The up-case table checked from its stored bytes: the checksum over them,
 * and the runs expanding to exactly 65536 mappings with no zero-length,
 * truncated or overlong run. Fills the held form when it fits. 0 with the
 * checksum in `sum`, -1 for a table that breaks a rule, or -EIO. */
static int upcase_check(struct ex_mount *em, uint32_t *sum)
{
    uint64_t at = 0;
    uint32_t s = 0, u = 0;
    int have_pending = 0;
    uint64_t off = cluster_offset(em, em->upcase_first);

    if (em->upcase_length < 2 || em->upcase_length > 131072 || (em->upcase_length & 1)) {
        return -1;
    }
    while (at < em->upcase_length) {
        uint8_t chunk[512];
        uint32_t take = em->upcase_length - at < sizeof(chunk) ? (uint32_t)(em->upcase_length - at)
                                                               : (uint32_t)sizeof(chunk);
        uint32_t i;
        int rc = ex_read_bytes(em, off + at, chunk, take);

        if (rc != 0) {
            return rc;
        }
        if (em->upcase_length <= UPCASE_HELD) {
            k_memcpy(em->upcase_stored + at, chunk, take);
        }
        for (i = 0; i < take; i++) {
            s = ((s & 1) ? 0x80000000u : 0) + (s >> 1) + chunk[i];
        }
        for (i = 0; i + 1 < take; i += 2) {
            uint16_t w = le16(chunk + i);

            if (have_pending) {
                if (w == 0 || u + w > 65536) {
                    return -1; /* a zero-length run, or one past the end */
                }
                u += w;
                have_pending = 0;
            } else if (w == 0xFFFF) {
                have_pending = 1;
            } else {
                if (u >= 65536) {
                    return -1;
                }
                u++;
            }
        }
        at += take;
    }
    if (have_pending) {
        /* A 0xFFFF as the table's last unit is the mapping of U+FFFF to
         * itself, which the recommended table ends with; a marker needs
         * a count after it. */
        u++;
    }
    if (u != 65536) {
        return -1; /* a truncated run, or a table short or long of the units */
    }
    em->upcase_form = em->upcase_length <= UPCASE_HELD ? 0 : (em->upcase_length == 131072 ? 1 : 2);
    *sum = s;
    return 0;
}

static int ex_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags, const char *options)
{
    struct ex_mount *em = 0;
    uint8_t b[512], b2[512];
    uint32_t main_sum, backup_sum, stored_main, stored_backup;
    uint32_t rev;
    uint32_t i, per;
    uint64_t heap_end;
    int main_ok, backup_ok, agree = 1;
    const char *readonly_why = 0;
    struct ex_node *root;
    uint32_t c, steps = 0;
    uint32_t upcase_sum_stored = 0;
    int have_bitmap = 0, have_upcase = 0, second_bitmap = 0;
    int rc;

    for (i = 0; i < EXFAT_MOUNTS; i++) {
        if (!ex_mounts[i].used) {
            em = &ex_mounts[i];
            break;
        }
    }
    if (!em) {
        return -ENOMEM;
    }
    k_memset(em, 0, sizeof(*em));
    em->dev = dev;
    rc = fat_parse_options(options, &em->uid, &em->gid, &em->umask);
    if (rc != 0) {
        return rc;
    }
    rc = ex_read_bytes(em, 0, b, sizeof(b));
    if (rc != 0) {
        return rc;
    }
    for (i = 0; i < 8; i++) {
        if (b[3 + i] != (uint8_t)"EXFAT   "[i]) {
            return refused("FileSystemName");
        }
    }
    for (i = 11; i < 64; i++) {
        if (b[i] != 0) {
            return refused("MustBeZero");
        }
    }
    if (le16(b + 510) != 0xAA55) {
        return refused("BootSignature");
    }
    if (b[108] != 9) {
        return refused("BytesPerSectorShift"); /* a valid exFAT this kernel does not read */
    }
    em->spc_shift = b[109];
    if (em->spc_shift > 16) {
        return refused("SectorsPerClusterShift"); /* a cluster past 32 MiB */
    }
    if (b[110] != 1 && b[110] != 2) {
        return refused("NumberOfFats");
    }
    rev = le16(b + 104);
    if (rev != 0x0100) {
        return refused("FileSystemRevision");
    }
    em->volume_length = le64(b + 72);
    em->fat_offset = le32(b + 80);
    em->fat_length = le32(b + 84);
    em->heap_offset = le32(b + 88);
    em->clusters = le32(b + 92);
    em->root = le32(b + 96);
    em->active_fat = le16(b + 106) & 1;
    if (em->volume_length > dev->sector_count || em->volume_length < 24 + 1) {
        return refused("VolumeLength");
    }
    if (em->fat_offset < 24 || em->fat_offset >= em->volume_length) {
        return refused("FatOffset");
    }
    if (em->clusters == 0 || em->clusters > 0xFFFFFFF5u) {
        return refused("ClusterCount");
    }
    if ((uint64_t)em->fat_length * BLOCK_SECTOR_SIZE < ((uint64_t)em->clusters + 2) * 4) {
        return refused("FatLength");
    }
    if ((uint64_t)em->fat_offset + (uint64_t)em->fat_length * b[110] > em->heap_offset) {
        return refused("ClusterHeapOffset");
    }
    heap_end = (uint64_t)em->heap_offset + ((uint64_t)em->clusters << em->spc_shift);
    if (heap_end > em->volume_length) {
        return refused("ClusterCount");
    }
    if (!in_heap(em, em->root)) {
        return refused("FirstClusterOfRootDirectory");
    }
    if (em->active_fat >= b[110]) {
        return refused("VolumeFlags");
    }
    /* Both boot regions, always. */
    rc = boot_checksum(em, 0, &main_sum);
    if (rc == 0) {
        rc = boot_checksum(em, 12, &backup_sum);
    }
    if (rc == 0) {
        rc = ex_read_bytes(em, 11 * BLOCK_SECTOR_SIZE, b2, 4);
        stored_main = le32(b2);
    }
    if (rc == 0) {
        rc = ex_read_bytes(em, 23 * BLOCK_SECTOR_SIZE, b2, 4);
        stored_backup = le32(b2);
    }
    if (rc == 0) {
        rc = ex_read_bytes(em, 12 * BLOCK_SECTOR_SIZE, b2, sizeof(b2));
    }
    if (rc != 0) {
        return rc;
    }
    main_ok = main_sum == stored_main;
    backup_ok = backup_sum == stored_backup;
    for (i = 0; i < 512; i++) {
        if (i == 106 || i == 107 || i == 112) {
            continue;
        }
        if (b[i] != b2[i]) {
            agree = 0;
            break;
        }
    }
    if (!main_ok && !backup_ok) {
        return refused("BootChecksum");
    }
    if (!main_ok) {
        readonly_why = "the main boot region fails its checksum; read from the backup";
        if (!agree) {
            /* The backup describes the volume from here on. */
            k_memcpy(b, b2, sizeof(b));
            em->fat_offset = le32(b + 80);
            em->fat_length = le32(b + 84);
            em->heap_offset = le32(b + 88);
            em->clusters = le32(b + 92);
            em->root = le32(b + 96);
        }
    } else if (!backup_ok) {
        readonly_why = "the backup boot region fails its checksum";
    } else if (!agree) {
        readonly_why = "the two boot regions disagree";
    } else if (b[110] == 2) {
        readonly_why = "two FATs (TexFAT)";
    } else if (le16(b + 106) & 4) {
        readonly_why = "MediaFailure is set";
    }
    /* The root's objects: one bitmap, one up-case table. */
    per = cluster_bytes(em) / 32;
    c = em->root;
    while (in_heap(em, c) && steps++ < em->clusters) {
        uint32_t k;
        uint32_t next;
        int end = 0;

        for (k = 0; k < per && !end; k++) {
            uint8_t e[32];

            rc = ex_read_bytes(em, cluster_offset(em, c) + (uint64_t)k * 32, e, 32);
            if (rc != 0) {
                return rc;
            }
            if (e[0] == 0x00) {
                end = 1;
            } else if (e[0] == 0x81) {
                if (have_bitmap || (e[1] & 1)) {
                    second_bitmap = 1;
                } else {
                    have_bitmap = 1;
                    em->bitmap_first = le32(e + 20);
                    em->bitmap_length = le64(e + 24);
                }
            } else if (e[0] == 0x82 && !have_upcase) {
                have_upcase = 1;
                em->upcase_first = le32(e + 20);
                em->upcase_length = le64(e + 24);
                upcase_sum_stored = le32(e + 4);
            }
        }
        if (end) {
            break;
        }
        rc = fat_next(em, c, &next);
        if (rc != 0) {
            return rc;
        }
        if (next == EX_EOC) {
            break;
        }
        c = next;
    }
    if (!have_bitmap) {
        return refused("the root has no allocation bitmap");
    }
    if (!have_upcase) {
        return refused("the root has no up-case table");
    }
    if (!in_heap(em, em->bitmap_first) || em->bitmap_length < ((uint64_t)em->clusters + 7) / 8 ||
        (uint64_t)em->bitmap_first - 2 + clusters_of(em, em->bitmap_length) > em->clusters) {
        return refused("the allocation bitmap's extent");
    }
    if (!in_heap(em, em->upcase_first) ||
        (uint64_t)em->upcase_first - 2 + clusters_of(em, em->upcase_length) > em->clusters) {
        return refused("the up-case table's extent");
    }
    {
        uint32_t sum;

        rc = upcase_check(em, &sum);
        if (rc == -EIO) {
            return -EIO;
        }
        if (rc != 0) {
            return refused("the up-case table's runs");
        }
        if (sum != upcase_sum_stored) {
            return refused("the up-case table's checksum");
        }
    }
    if (second_bitmap && !readonly_why) {
        readonly_why = "a second allocation bitmap";
    }
    em->writable = (flags & VFS_MS_RDONLY) == 0;
    if (readonly_why) {
        klog(KLOG_WARN, "exfat", "mounted read-only: %s", readonly_why);
        em->writable = 0;
        mnt->flags |= VFS_MS_RDONLY;
    }
    klog(KLOG_INFO, "exfat", "up-case table: %s",
         em->upcase_form == 0   ? "held"
         : em->upcase_form == 1 ? "uncompressed, indexed through the cache"
                                : "streamed through the cache");
    root = node_take(em);
    root->is_dir = 1;
    root->first = em->root;
    root->length = 0; /* the root's chain is followed to its end */
    em->used = 1;
    em->mnt = mnt;
    mnt->priv = em;
    mnt->root = root;
    return 0;
}

static const struct vfs_ops exfat_ops = {
    ex_lookup,  0,                         /* lookup_rest */
    ex_hold,    ex_put,     ex_getattr, 0, /* setattr */
    ex_open,    ex_release, ex_read,    0, /* write */
    0,                                     /* truncate */
    0,                                     /* fsync */
    ex_readdir, 0,                         /* create */
    0,                                     /* unlink */
    0,                                     /* mkdir */
    0,                                     /* rmdir */
    0,                                     /* rename */
    0,                                     /* link */
    0,                                     /* readlink */
    ex_statfs,  0,                         /* sync */
    0,                                     /* prepare_unmount */
    ex_unmount, 0,                         /* path */
};

const struct vfs_type vfs_exfat_type = {"exfat", ex_mount, &exfat_ops};
