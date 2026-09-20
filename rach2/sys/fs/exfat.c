/*
 * exfat.c: the `exfat` type, read. The contract is in exfat.h and
 * exfat.Readme.md; what is here is the volume check over both boot
 * regions, the root's objects, the up-case table in its three forms, the
 * entry sets and their invariants, chains with and without the FAT,
 * names, timestamps, and the node pool that gives the VFS something to
 * hold. The writer is phase 4's.
 */

#include "exfat.h"

#include "../mm/kheap.h"

#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "bcache.h"
#include "fatcommon.h"
#include "../panic.h"
#include "../proc/process.h"

#define EX_S_IFDIR 0040000u
#define EX_S_IFREG 0100000u

#define ATTR_READ_ONLY 0x0001u
#define ATTR_DIRECTORY 0x0010u

#define FLAG_ALLOC_POSSIBLE 0x01u
#define FLAG_NO_FAT_CHAIN   0x02u

#define EX_EOC 0xFFFFFFFFu
#define EX_BAD 0xFFFFFFF7u

/* The largest offset a 64-bit seek can hand a write; the format allows it. */
#define EX_FILE_LIMIT (((uint64_t)0x7FFFFFFFu << 32) | 0xFFFFFFFFu)

#define UPCASE_HELD 16384u /* a stored table up to this size is kept per mount */
#define UPCASE_LOW  2048u  /* code points whose mapping is kept, walked once */
#define SET_MAX     19     /* the file entry, the stream, and up to 17 name entries */

struct ex_node {
    struct ex_node *next; /* the mount's list of nodes in memory */
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
    int unlinked; /* the set is gone; the clusters go at the last put */
    int scanned;  /* a directory read to its end for a critical entry it cannot hold */
    uint32_t hint_index;
    uint32_t hint_cluster;
};

#define EXFAT_FROZEN 16 /* directories found holding an unknown critical entry */

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
    struct ex_node bitmap_walk; /* the hints of the last walk of the bitmap's chain */
    uint32_t upcase_first;
    uint64_t upcase_length;
    /* 0: the stored form held here and walked; 1: uncompressed, indexed
     * through the cache; 2: compressed and larger than held, streamed
     * through the cache from its beginning for every unit. */
    int upcase_form;
    uint8_t upcase_stored[UPCASE_HELD];
    /* The mappings of the first UPCASE_LOW code points, read once at mount
     * by one walk of the table, since a name is hashed at every set a
     * lookup passes and a walk per character is the cost of that scan. */
    int upcase_low_held;
    uint16_t upcase_low[UPCASE_LOW];
    int free_known;
    uint32_t free_clusters;
    uint32_t alloc_hint;
    int dirty_set; /* VolumeDirty is on the disk for this mount */
    uint32_t frozen[EXFAT_FROZEN];
    int nfrozen;
    uint8_t setbuf[256 * 32]; /* one set, other writers' up to 255 secondaries */
    struct ex_node *nodes;    /* the nodes in memory, from the heap */
    /* The last cache block a directory entry was read from, kept so a scan
     * of a directory costs a cache get per block and not per 32 bytes; a
     * write of any byte of the block empties it. */
    int dir_block_held;
    uint64_t dir_block;
    uint8_t dir_bytes[BCACHE_BLOCK_BYTES];
};

/* The way a node leaves the mount's list and the heap. */
static void node_drop(struct ex_mount *em, struct ex_node *n)
{
    struct ex_node **link = &em->nodes;

    while (*link && *link != n) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = n->next;
    }
    kfree(n);
}

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

    return (bytes >> shift) + ((bytes & (((uint64_t)1 << shift) - 1)) != 0 ? 1 : 0);
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
 * bad or free entry, or a cluster that names itself. A longer loop within
 * a file's length repeats its clusters; the dirty-volume check is what
 * finds it, as on FAT32. */
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
        if (next == EX_BAD || !in_heap(em, next) || next == c) {
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
    struct ex_node *n = kzalloc(sizeof(*n));

    if (n) {
        n->used = 1;
        n->refs = 1;
        n->next = em->nodes;
        em->nodes = n;
    }
    return n;
}

static struct ex_node *node_find(struct ex_mount *em, int is_dir, uint32_t dir_cluster,
                                 uint32_t index, uint32_t first)
{
    struct ex_node *n;

    for (n = em->nodes; n; n = n->next) {
        if (n->is_dir != is_dir || n->unlinked) {
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

static int free_chain(struct ex_mount *em, uint32_t first, int nofat, uint64_t length);
static int write_boot_state(struct ex_mount *em, int dirty);
static int ex_check_volume(struct ex_mount *em);

static void ex_put(struct vfs_mount *mnt, void *node)
{
    struct ex_node *n = node;

    if (n->refs > 0) {
        n->refs--;
    }
    if (n->refs == 0) {
        /* An unlinked file's clusters go when its last holder does; the
         * set went at the unlink, so a crash here leaks them. */
        if (n->unlinked && n->first != 0) {
            struct ex_mount *em = mnt->priv;

            vfs_ns_lock(mnt);
            free_chain(em, n->first, n->nofat, n->length);
            vfs_ns_unlock(mnt);
        }
        node_drop((struct ex_mount *)mnt->priv, n);
    }
}

/* ---- the system objects ---- */

/* `len` bytes at `off` of a system object whose clusters the FAT chains:
 * the bitmap and the up-case table, whose entries have no flags and so no
 * NoFatChain. */
static int object_read(struct ex_mount *em, uint32_t first, struct ex_node *walk, uint64_t off,
                       void *dst, uint32_t len)
{
    uint8_t *out = dst;
    uint32_t cbytes = cluster_bytes(em);

    while (len > 0) {
        uint32_t in = (uint32_t)(off & (cbytes - 1));
        uint32_t take = cbytes - in;
        uint32_t c;
        int rc;

        if (take > len) {
            take = len;
        }
        rc = chain_at(em, walk, first, 0, 0, (uint32_t)(off >> (9 + em->spc_shift)), &c);
        if (rc != 0) {
            return rc == -ENXIO ? -EIO : rc;
        }
        rc = ex_read_bytes(em, cluster_offset(em, c) + in, out, take);
        if (rc != 0) {
            return rc;
        }
        out += take;
        off += take;
        len -= take;
    }
    return 0;
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
            int rc = object_read(r->em, r->em->upcase_first, 0, next, r->chunk, take);

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

/**
 * The mappings of the code points below `UPCASE_LOW` into the mount's
 * record, by one walk of the stored table from its start; a table that
 * ends before them maps the rest to themselves.
 */
static int upcase_fill_low(struct ex_mount *em)
{
    struct upcase_reader r;
    uint32_t u = 0;

    k_memset(&r, 0, sizeof(r));
    r.em = em;
    while (u < UPCASE_LOW) {
        uint16_t w;
        int rc = upcase_next(&r, &w);

        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            break;
        }
        if (w == 0xFFFF && em->upcase_form != 1) {
            uint16_t run;

            rc = upcase_next(&r, &run);
            if (rc < 0) {
                return rc;
            }
            if (rc == 0) {
                break;
            }
            while (run-- > 0 && u < UPCASE_LOW) {
                em->upcase_low[u] = (uint16_t)u;
                u++;
            }
        } else {
            em->upcase_low[u] = w;
            u++;
        }
    }
    while (u < UPCASE_LOW) {
        em->upcase_low[u] = (uint16_t)u;
        u++;
    }
    em->upcase_low_held = 1;
    return 0;
}

static int ex_upcase(struct ex_mount *em, uint16_t unit, uint16_t *out)
{
    if (em->upcase_low_held && unit < UPCASE_LOW) {
        *out = em->upcase_low[unit];
        return 0;
    }
    if (em->upcase_form == 1) {
        uint8_t raw[2];
        int rc = object_read(em, em->upcase_first, 0, (uint64_t)unit * 2, raw, 2);

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
 * its chain from the mount's copy of the entry's cache block, which a scan
 * reads once per block. 1 with `raw` filled, 0 past the chain's end, or an
 * error. */
static int dir_entry(struct ex_mount *em, struct ex_node *d, uint32_t index, uint8_t *raw)
{
    uint32_t per = cluster_bytes(em) / 32;
    uint32_t c;
    uint64_t off, blk;
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
    off = cluster_offset(em, c) + (uint64_t)(index % per) * 32;
    blk = off / BCACHE_BLOCK_BYTES;
    if (!em->dir_block_held || em->dir_block != blk) {
        struct bbuf *b = bcache_get(em->dev, blk);

        if (!b) {
            return -EIO;
        }
        k_memcpy(em->dir_bytes, b->data, b->bytes);
        bcache_release(b);
        em->dir_block = blk;
        em->dir_block_held = 1;
    }
    k_memcpy(raw, em->dir_bytes + off % BCACHE_BLOCK_BYTES, 32);
    return 1;
}

struct ex_set {
    uint32_t index;   /* of the file entry */
    uint32_t entries; /* the whole set */
    const char *why;  /* why the set was passed over, or NULL */
    int checksum_only;
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
 * invariant holds, 0 when the set is passed over with `set->why` naming
 * the first fault (and `set->checksum_only` when the checksum is the only
 * one, which is what a stop inside a straddling change leaves and the
 * dirty-volume check repairs), or an error. `raw` is the file entry
 * already read. The set is streamed entry by entry, so a set with the
 * format's 255 secondaries costs no more memory than one with two. */
static int set_read(struct ex_mount *em, struct ex_node *d, uint32_t index, const uint8_t *raw,
                    struct ex_set *set)
{
    uint32_t secondaries = raw[1];
    uint32_t k, got = 0, names = 0, nlen = 0, want_names = 0;
    uint16_t sum = 0, stored_sum = le16(raw + 2);
    uint8_t stream[32];
    uint16_t hash;
    const char *why = 0;
    int have_stream = 0;
    int rc;

    set->why = 0;
    set->checksum_only = 0;
    set->index = index;
    set->entries = secondaries + 1;
    set->count = 0;
    if (secondaries < 2) {
        why = "SecondaryCount";
    }
    for (k = 0; k <= secondaries && !why; k++) {
        uint8_t e[32];
        uint32_t b;

        if (k == 0) {
            k_memcpy(e, raw, 32);
        } else {
            rc = dir_entry(em, d, index + k, e);
            if (rc < 0) {
                return rc;
            }
            if (rc == 0) {
                why = "a set past the directory's end";
                break;
            }
        }
        for (b = 0; b < 32; b++) {
            if (k == 0 && (b == 2 || b == 3)) {
                continue;
            }
            sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + e[b]);
        }
        if (k == 0) {
            continue;
        }
        /* Every entry of a set after its primary is an in-use secondary. */
        if ((e[0] & 0xC0) != 0xC0) {
            why = "an entry of the set that is not an in-use secondary";
            break;
        }
        if (k == 1) {
            if (e[0] != 0xC0) {
                why = "no stream extension second";
                break;
            }
            k_memcpy(stream, e, 32);
            have_stream = 1;
            nlen = e[3];
            want_names = (nlen + 14) / 15;
            if (nlen == 0 || nlen > FAT_NAME_UNITS) {
                why = "NameLength";
                break;
            }
            if (e[1] & 0xFC) {
                why = "reserved GeneralSecondaryFlags bits";
                break;
            }
            continue;
        }
        /* The name entries: exactly ceil(NameLength / 15), consecutive,
         * right after the stream; then benign secondaries only. */
        if (names < want_names) {
            uint32_t m;

            if (e[0] != 0xC1) {
                why = "name entries not consecutive after the stream";
                break;
            }
            names++;
            for (m = 0; m < 15; m++) {
                uint16_t u = le16(e + 2 + m * 2);

                if (got < nlen) {
                    set->units[got++] = u;
                } else if (u != 0) {
                    why = "units past NameLength";
                    break;
                }
            }
        } else if (!(e[0] & 0x20)) {
            why = "a critical secondary after the name";
            break;
        }
    }
    if (!why && (!have_stream || got != nlen)) {
        why = "NameLength";
    }
    if (!why) {
        uint64_t length = le64(stream + 24), valid = le64(stream + 8);
        uint32_t first = le32(stream + 20);

        if (valid > length) {
            why = "ValidDataLength past DataLength";
        } else if ((le16(raw + 4) & ATTR_DIRECTORY) && (valid != length || length > 0x10000000u)) {
            why = "a directory's lengths"; /* equal, and at most 256 MiB */
        } else if ((stream[1] & FLAG_NO_FAT_CHAIN) && length == 0) {
            why = "NoFatChain with no length";
        } else if (!(stream[1] & FLAG_ALLOC_POSSIBLE)) {
            why = "AllocationPossible clear";
        } else if (length != 0 && !in_heap(em, first)) {
            why = "a first cluster outside the heap";
        } else if (length == 0 && first != 0) {
            why = "a cluster with no length";
        } else if ((stream[1] & FLAG_NO_FAT_CHAIN) && length != 0 &&
                   (uint64_t)first - 2 + clusters_of(em, length) > em->clusters) {
            why = "a contiguous run past the heap";
        }
        if (!why) {
            rc = name_hash(em, set->units, nlen, &hash);
            if (rc != 0) {
                return rc;
            }
            if (hash != le16(stream + 4)) {
                why = "the name hash";
            }
        }
    }
    if (!why && sum != stored_sum) {
        why = "the set checksum";
        set->checksum_only = 1;
    }
    if (have_stream) {
        k_memcpy(set->file, raw, 32);
        k_memcpy(set->stream, stream, 32);
        set->count = nlen;
    }
    if (!why) {
        return 1;
    }
    set->why = why;
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
            /* The rejected set is passed over as one unit, so nothing
             * inside it is read as a primary entry of its own. */
            *cursor += raw[1] >= 2 ? (uint32_t)raw[1] + 1 : 1;
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
    uint64_t stack_length[32];
    uint8_t stack_nofat[32];
    int depth = 1;

    stack_first[0] = root->first;
    stack_cursor[0] = 0;
    stack_length[0] = 0;
    stack_nofat[0] = 0;
    while (depth > 0) {
        struct ex_node d;
        struct ex_set set;
        int rc;

        k_memset(&d, 0, sizeof(d));
        d.is_dir = 1;
        d.first = stack_first[depth - 1];
        d.length = stack_length[depth - 1];
        d.nofat = stack_nofat[depth - 1];
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
                stack_length[depth] = le64(set.stream + 24);
                stack_nofat[depth] = (set.stream[1] & FLAG_NO_FAT_CHAIN) != 0;
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
        uint16_t ua = 0, ub = 0;
        int rc = ex_upcase(em, a[i], &ua);

        if (rc == 0) {
            rc = ex_upcase(em, b[i], &ub);
        }
        if (rc != 0) {
            return rc;
        }
        if (ua != ub) {
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
    st->ino = fat_inode(n->is_dir, n->first, n->dir_cluster, n->index);
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
        /* A name past the kernel's component: shown by as many of its
         * first units as fit, whole characters only, and unreachable by
         * path, as the Readme says. */
        uint32_t keep = 60;

        if (set.units[keep - 1] >= 0xD800 && set.units[keep - 1] < 0xDC00) {
            keep--;
        }
        rc = fat_units_utf8(set.units, keep, ent->name, sizeof(ent->name));
    }
    if (rc != 0) {
        k_strncpy(ent->name, "?", sizeof(ent->name));
        ent->name[sizeof(ent->name) - 1] = '\0';
    }
    ent->is_dir = (le16(set.file + 4) & ATTR_DIRECTORY) != 0;
    {
        struct ex_node probe;

        k_memset(&probe, 0, sizeof(probe));
        probe.is_dir = ent->is_dir;
        probe.first = le32(set.stream + 20);
        probe.dir_cluster = d->first == em->root ? 0 : d->first;
        probe.index = set.index;
        ent->ino = fat_inode(probe.is_dir, probe.first, probe.dir_cluster, probe.index);
    }
    return 1;
}

/* Free clusters are the bitmap's zero bits, counted once and kept. */
K_NOINLINE static int count_free(struct ex_mount *em)
{
    uint32_t c = 0, free_count = 0;

    while (c < em->clusters) {
        uint8_t raw[512];
        uint32_t take = (em->clusters - c + 7) / 8;
        uint32_t i;
        int rc;

        if (take > sizeof(raw)) {
            take = sizeof(raw);
        }
        rc = object_read(em, em->bitmap_first, &em->bitmap_walk, c / 8, raw, take);
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

    while (em->nodes) {
        node_drop(em, em->nodes);
    }
    mnt->priv = 0;
    mnt->root = 0;
    kfree(em);
}

/* ---- the constructor ---- */

static int refused(const char *field)
{
    klog(KLOG_WARN, "exfat", "refused: %s", field);
    return -EINVAL;
}

/*
 * The volume is not exFAT at all, which is an answer and not a fault.
 *
 * A mount is how a caller asks a file system whether a volume is its own, and
 * the installer asks every partition of every removable disk that way to find
 * the one holding its image. A driver that warned on the console each time it
 * was asked about a partition that was never its own would put
 * `exfat: refused: FileSystemName` in front of a person installing a machine,
 * which is what it did until 2026-09-14. The fields that only say "not this
 * file system" answer quietly; a volume that claims to be exFAT and is broken
 * still says so through `refused`.
 */
static int not_exfat(void)
{
    return -EINVAL;
}

/* The boot checksum of the region at sector `first`: the rotate-and-add
 * sum over its eleven sectors, skipping VolumeFlags and PercentInUse. */
K_NOINLINE static int boot_checksum(struct ex_mount *em, uint64_t first, uint32_t *sum)
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

/* The stored boot checksum at sector `n`: every 32-bit word of the sector
 * repeats it, or the sector is not a checksum sector. */
K_NOINLINE static int checksum_sector(struct ex_mount *em, uint64_t n, uint32_t *sum)
{
    uint8_t raw[512];
    uint32_t i;
    int rc = ex_read_bytes(em, n * BLOCK_SECTOR_SIZE, raw, sizeof(raw));

    if (rc != 0) {
        return rc;
    }
    *sum = le32(raw);
    for (i = 4; i < sizeof(raw); i += 4) {
        if (le32(raw + i) != *sum) {
            *sum ^= 0x80000000u; /* words that differ match nothing */
            return 0;
        }
    }
    return 0;
}

/* The up-case table checked from its stored bytes: the checksum over them,
 * and the runs expanding to exactly 65536 mappings with no zero-length,
 * truncated or overlong run. Fills the held form when it fits. 0 with the
 * checksum in `sum`, -1 for a table that breaks a rule, or -EIO. */
K_NOINLINE static int upcase_check(struct ex_mount *em, uint32_t *sum)
{
    uint64_t at = 0;
    uint32_t s = 0, u = 0, markers = 0, m;
    int have_pending = 0;

    if (em->upcase_length < 2 || em->upcase_length > 131072 || (em->upcase_length & 1)) {
        return -1;
    }
    while (at < em->upcase_length) {
        uint8_t chunk[512];
        uint32_t take = em->upcase_length - at < sizeof(chunk) ? (uint32_t)(em->upcase_length - at)
                                                               : (uint32_t)sizeof(chunk);
        uint32_t i;
        int rc = object_read(em, em->upcase_first, 0, at, chunk, take);

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
                markers++;
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
    if (have_pending) {
        markers--; /* the last unit was a mapping after all */
    }
    /* Uncompressed is a table of 65536 explicit units and no run marker;
     * a compressed table of the same size is walked, not indexed. */
    em->upcase_form = em->upcase_length <= UPCASE_HELD
                          ? 0
                          : (em->upcase_length == 131072 && markers == 0 ? 1 : 2);
    if (upcase_fill_low(em) != 0) {
        return -1;
    }
    /* The mandatory mappings: the first 128 code points are themselves,
     * but a to z are A to Z. */
    for (m = 0; m < 128; m++) {
        uint16_t got;

        if (ex_upcase(em, (uint16_t)m, &got) != 0 ||
            got != (m >= 'a' && m <= 'z' ? (uint16_t)(m - 32) : (uint16_t)m)) {
            return -1;
        }
    }
    *sum = s;
    return 0;
}

/* The constructor proper, on a slot already claimed: it sleeps in the
 * cache and in the dirty-volume check, so the slot is taken before it
 * runs and given back when it fails. */
static int ex_mount_on(struct ex_mount *em, struct vfs_mount *mnt, block_device_t *dev,
                       uint32_t flags, const char *options)
{
    uint8_t b[512], b2[512];
    uint32_t main_sum = 0, backup_sum = 0, stored_main = 0, stored_backup = 0;
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

    em->dev = dev;
    rc = fat_parse_options(options, &em->uid, &em->gid, &em->umask);
    if (rc != 0) {
        return rc;
    }
    rc = ex_read_bytes(em, 0, b, sizeof(b));
    if (rc == 0) {
        rc = ex_read_bytes(em, 12 * BLOCK_SECTOR_SIZE, b2, sizeof(b2));
    }
    if (rc == 0) {
        rc = boot_checksum(em, 0, &main_sum);
    }
    if (rc == 0) {
        rc = boot_checksum(em, 12, &backup_sum);
    }
    if (rc == 0) {
        rc = checksum_sector(em, 11, &stored_main);
    }
    if (rc == 0) {
        rc = checksum_sector(em, 23, &stored_backup);
    }
    if (rc != 0) {
        return rc;
    }
    /* Both boot regions, always, before any field is trusted. */
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
        /* The backup describes the volume from here on, but the flags
         * are the main's: the backup's are stale by the specification. */
        readonly_why = "the main boot region fails its checksum; read from the backup";
        for (i = 0; i < 512; i++) {
            if (i != 106 && i != 107 && i != 112) {
                b[i] = b2[i];
            }
        }
    } else if (!backup_ok) {
        readonly_why = "the backup boot region fails its checksum";
    } else if (!agree) {
        readonly_why = "the two boot regions disagree";
    }
    for (i = 0; i < 8; i++) {
        if (b[3 + i] != (uint8_t)"EXFAT   "[i]) {
            return not_exfat();
        }
    }
    if (b[0] != 0xEB || b[1] != 0x76 || b[2] != 0x90) {
        return not_exfat();
    }
    for (i = 11; i < 64; i++) {
        if (b[i] != 0) {
            return refused("MustBeZero");
        }
    }
    if (le16(b + 510) != 0xAA55) {
        return not_exfat();
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
    if (em->volume_length > dev->sector_count || em->volume_length < 2048) {
        return refused("VolumeLength"); /* at least 1 MiB by the specification */
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
    if (!readonly_why && b[110] == 2) {
        readonly_why = "two FATs (TexFAT)";
    } else if (!readonly_why && (le16(b + 106) & 4)) {
        readonly_why = "MediaFailure is set";
    }
    /* The root's objects: the bitmap of the active FAT, one up-case table. */
    per = cluster_bytes(em) / 32;
    c = em->root;
    for (;;) {
        uint32_t k;
        uint32_t next;
        int end = 0;

        if (!in_heap(em, c) || steps++ >= em->clusters) {
            return refused("the root's chain");
        }
        for (k = 0; k < per && !end; k++) {
            uint8_t e[32];

            rc = ex_read_bytes(em, cluster_offset(em, c) + (uint64_t)k * 32, e, 32);
            if (rc != 0) {
                return rc;
            }
            if (e[0] == 0x00) {
                end = 1;
            } else if (e[0] == 0x81) {
                if ((e[1] & 1) != em->active_fat) {
                    second_bitmap = 1; /* the other FAT's, which only TexFAT has */
                } else if (have_bitmap) {
                    return refused("a second allocation bitmap of the active FAT");
                } else {
                    have_bitmap = 1;
                    em->bitmap_first = le32(e + 20);
                    em->bitmap_length = le64(e + 24);
                }
            } else if (e[0] == 0x82) {
                if (have_upcase) {
                    return refused("a second up-case table");
                }
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
    em->mnt = mnt;
    em->alloc_hint = 2;
    if (em->writable && (le16(b + 106) & 2)) {
        /* Not unmounted cleanly: checked and repaired before the mount is
         * published, or read-only with the fault named. */
        rc = ex_check_volume(em);
        if (rc < 0) {
            return rc;
        }
        if (rc > 0) {
            em->writable = 0;
            mnt->flags |= VFS_MS_RDONLY;
        }
    }
    if (em->writable) {
        /* VolumeDirty on the disk for the writable life of the mount, as
         * Linux does; the clean unmount clears it last. */
        rc = write_boot_state(em, 1);
        if (rc != 0) {
            return rc;
        }
        em->dirty_set = 1;
    }
    root = node_take(em);
    root->is_dir = 1;
    root->first = em->root;
    root->length = 0; /* the root's chain is followed to its end */
    mnt->priv = em;
    mnt->root = root;
    return 0;
}

static int ex_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags, const char *options)
{
    struct ex_mount *em = kzalloc(sizeof(*em));
    int rc;

    if (!em) {
        return -ENOMEM;
    }
    em->used = 1;
    rc = ex_mount_on(em, mnt, dev, flags, options);
    if (rc != 0) {
        while (em->nodes) {
            node_drop(em, em->nodes);
        }
        kfree(em);
    }
    return rc;
}

/* ---- writing ----
 *
 * The order of writes is exfat.Readme.md's: a cluster's bitmap bit is set
 * and its chain written before any entry refers to it; data is written
 * and synced before a length is published; a set is unpublished before its
 * clusters are freed, and the clusters of a node a description holds go
 * at its last put. A set is made or unmade by the in-use bit of its file
 * entry, written last with the checksum over the whole; a change to the
 * file and stream entries is one sector write when they share a sector,
 * and every set this driver makes has them in one. Every metadata write
 * is `bcache_write`.
 */

static void assert_locked(struct vfs_mount *mnt)
{
    int me = current_proc ? (int)current_proc->pid : 0;

    if (mnt->ns_holder != me || mnt->ns_depth == 0) {
        k_panic("exfat: a mutation outside the mount's lock");
    }
}

/* The mount's lock for an operation the common layer does not lock, and
 * the read-only decision under it. */
static int begin_write(struct vfs_mount *mnt, struct ex_mount *em)
{
    vfs_ns_lock(mnt);
    if (!em->writable || (mnt->flags & VFS_MS_RDONLY)) {
        vfs_ns_unlock(mnt);
        return -EROFS;
    }
    return 0;
}

/**
 * Write back everything dirty on the volume's device and then tell the
 * device to put it on the medium: the point in the order of writes below
 * where what came before must be on the disk before what comes after. A
 * disk that writes through, as this kernel's ATA and AHCI drivers do,
 * has nothing to flush; a stick with a cache of its own is told.
 */
static int sync_disk(struct ex_mount *em)
{
    int rc = bcache_sync(em->dev);

    return rc != 0 ? rc : block_flush(em->dev);
}

static int ex_write_bytes(struct ex_mount *em, uint64_t off, const void *src, uint32_t len,
                          int through)
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
        /* A whole block is not read first: its bytes are about to be
           replaced, and the zeros of a blank buffer are the zero fill. */
        b = take == BCACHE_BLOCK_BYTES ? bcache_get_blank(em->dev, blk) : bcache_get(em->dev, blk);
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
        if (em->dir_block_held && em->dir_block == blk) {
            em->dir_block_held = 0;
        }
        if (through) {
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

/* The FAT entry of `c`, written through, or, for one of a run whose
 * caller syncs before anything refers to it, set in the cache. */
static int fat_set(struct ex_mount *em, uint32_t c, uint32_t value, int through)
{
    uint8_t raw[4];
    uint64_t off =
        ((uint64_t)em->fat_offset + (uint64_t)em->active_fat * em->fat_length) * BLOCK_SECTOR_SIZE +
        (uint64_t)c * 4;

    raw[0] = (uint8_t)value;
    raw[1] = (uint8_t)(value >> 8);
    raw[2] = (uint8_t)(value >> 16);
    raw[3] = (uint8_t)(value >> 24);
    return ex_write_bytes(em, off, raw, 4, through);
}

/* The bitmap bit of `c`, by way of the bitmap's chain, written through or
 * set in the cache as `fat_set` is. */
static int bit_set(struct ex_mount *em, uint32_t c, int on, int through)
{
    uint8_t byte;
    uint32_t cl;
    uint64_t at = (uint64_t)(c - 2) / 8;
    int rc = chain_at(em, &em->bitmap_walk, em->bitmap_first, 0, 0,
                      (uint32_t)(at >> (9 + em->spc_shift)), &cl);

    if (rc != 0) {
        return rc == -ENXIO ? -EIO : rc;
    }
    rc = ex_read_bytes(em, cluster_offset(em, cl) + (at & (cluster_bytes(em) - 1)), &byte, 1);
    if (rc != 0) {
        return rc;
    }
    if (on) {
        byte |= (uint8_t)(1u << ((c - 2) % 8));
    } else {
        byte &= (uint8_t)~(1u << ((c - 2) % 8));
    }
    return ex_write_bytes(em, cluster_offset(em, cl) + (at & (cluster_bytes(em) - 1)), &byte, 1,
                          through);
}

/* One free cluster, first-fit from the hint with one wrap, its bit set. */
static int alloc_cluster(struct ex_mount *em, uint32_t *out)
{
    uint32_t start, c, tried = 0;
    int rc;

    if (!em->free_known) {
        rc = count_free(em);
        if (rc != 0) {
            return rc;
        }
    }
    if (em->free_clusters == 0) {
        return -ENOSPC;
    }
    start = fat_hint_start(em->alloc_hint, em->clusters);
    c = start;
    while (tried < em->clusters) {
        uint8_t byte;

        rc = object_read(em, em->bitmap_first, &em->bitmap_walk, (uint64_t)(c - 2) / 8, &byte, 1);
        if (rc != 0) {
            return rc;
        }
        if (!(byte & (1u << ((c - 2) % 8)))) {
            rc = bit_set(em, c, 1, 0);
            if (rc != 0) {
                return rc;
            }
            em->free_clusters--;
            em->alloc_hint = fat_hint_next(c, em->clusters);
            *out = c;
            return 0;
        }
        tried++;
        c = fat_hint_next(c, em->clusters);
    }
    return -ENOSPC;
}

static int free_chain(struct ex_mount *em, uint32_t first, int nofat, uint64_t length)
{
    uint32_t c = first;
    uint32_t steps = 0;
    uint64_t have = nofat ? clusters_of(em, length) : 0;
    int rc;

    /* What unpublished the chain went through the cache to the device;
     * a stick with a cache of its own puts it on the medium here, before
     * a cluster is freed. */
    rc = block_flush(em->dev);
    if (rc != 0) {
        return rc;
    }
    while (in_heap(em, c) && steps < em->clusters) {
        uint32_t next = 0;

        if (nofat) {
            if (steps >= have) {
                break;
            }
            next = c + 1;
        } else {
            rc = fat_next(em, c, &next);
            if (rc != 0) {
                return rc;
            }
        }
        rc = bit_set(em, c, 0, 0);
        if (rc == 0 && !nofat) {
            rc = fat_set(em, c, 0, 0);
        }
        if (rc != 0) {
            return rc;
        }
        if (em->free_known) {
            em->free_clusters++;
        }
        if (c < em->alloc_hint) {
            em->alloc_hint = c;
        }
        steps++;
        if (!nofat && (next == EX_EOC || next == c)) {
            break;
        }
        c = next;
    }
    /* The bits and entries of the whole chain, set in the cache above,
     * reach the disk in runs here rather than one write each. */
    return steps > 0 ? sync_disk(em) : 0;
}

/* Whether `n` clusters are free, counted once and kept: ENOSPC is decided
 * before any write, the conversion of a contiguous chain included. */
static int room_for(struct ex_mount *em, uint32_t n)
{
    if (!em->free_known) {
        int rc = count_free(em);

        if (rc != 0) {
            return rc;
        }
    }
    return em->free_clusters < n ? -ENOSPC : 0;
}

/* A run of `n` clusters chained through the FAT and ended, disconnected
 * from everything; `zero` 1 fills them with zeros for writeback, 2 writes
 * the zeros through, 0 leaves their bytes. ENOSPC is decided before any
 * write; a failure gives back what was taken. */
static int alloc_run(struct ex_mount *em, uint32_t n, int zero, uint32_t *first, uint32_t *last)
{
    uint32_t prev = 0, i;
    int rc = 0;

    *first = 0;
    *last = 0;
    if (!em->free_known) {
        rc = count_free(em);
        if (rc != 0) {
            return rc;
        }
    }
    if (em->free_clusters < n) {
        return -ENOSPC;
    }
    for (i = 0; i < n; i++) {
        uint32_t c;

        rc = alloc_cluster(em, &c);
        if (rc != 0) {
            break;
        }
        if (zero) {
            rc = ex_write_bytes(em, cluster_offset(em, c), 0, cluster_bytes(em), zero == 2);
        }
        if (rc == 0) {
            rc = fat_set(em, c, EX_EOC, 0);
        }
        if (rc == 0 && prev != 0) {
            rc = fat_set(em, prev, c, 0);
        }
        if (rc != 0) {
            bit_set(em, c, 0, 0);
            em->free_clusters++;
            break;
        }
        if (prev == 0) {
            *first = c;
        }
        prev = c;
    }
    if (rc != 0 && *first != 0) {
        free_chain(em, *first, 0, 0);
        *first = 0;
    }
    /* The run's bits and entries are set in the cache: a caller that
     * writes its data first syncs before it joins the run, and a run
     * written through, a directory's, is joined at once, so its sync is
     * here. */
    if (rc == 0 && zero == 2) {
        rc = sync_disk(em);
        if (rc != 0) {
            free_chain(em, *first, 0, 0);
            *first = 0;
        }
    }
    *last = prev;
    return rc;
}

/* A contiguous chain written into the FAT, so that the flag can be
 * cleared and the chain grown: the run's entries, ended. */
static int chain_into_fat(struct ex_mount *em, uint32_t first, uint64_t length)
{
    uint64_t n = clusters_of(em, length), i;

    for (i = 0; i < n; i++) {
        uint32_t next = i + 1 < n ? (uint32_t)(first + i + 1) : EX_EOC;
        int rc = fat_set(em, (uint32_t)(first + i), next, 0);

        if (rc != 0) {
            return rc;
        }
    }
    return sync_disk(em); /* the chain is on the disk before the flag is cleared */
}

/* ---- directory entries and sets ---- */

static int dir_write(struct ex_mount *em, struct ex_node *d, uint32_t index, const uint8_t *raw,
                     uint32_t count)
{
    uint32_t per = cluster_bytes(em) / 32;

    while (count > 0) {
        uint32_t c;
        uint32_t take = per - index % per;
        int rc = chain_at(em, d, d->first, d->nofat, d->length, index / per, &c);

        if (rc != 0) {
            return rc == -ENXIO ? -EIO : rc;
        }
        if (take > count) {
            take = count;
        }
        rc = ex_write_bytes(em, cluster_offset(em, c) + (uint64_t)(index % per) * 32, raw,
                            take * 32, 1);
        if (rc != 0) {
            return rc;
        }
        raw += take * 32;
        index += take;
        count -= take;
    }
    return 0;
}

static uint32_t ex_stamp_now(uint8_t *tenth)
{
    uint16_t date, time;

    fat_now(&date, &time, tenth);
    return ((uint32_t)date << 16) | time;
}

static int node_publish(struct ex_mount *em, struct ex_node *n);

/* The set at `index` of `d` loaded into the mount's buffer: its entry
 * count, or an error; the count is SecondaryCount + 1 as stored. */
static int set_load(struct ex_mount *em, struct ex_node *d, uint32_t index, uint32_t *count)
{
    uint32_t k, n;
    int rc = dir_entry(em, d, index, em->setbuf);

    if (rc <= 0) {
        return rc == 0 ? -EIO : rc;
    }
    n = (uint32_t)em->setbuf[1] + 1;
    if (n > 256) {
        return -EIO;
    }
    for (k = 1; k < n; k++) {
        rc = dir_entry(em, d, index + k, em->setbuf + k * 32);
        if (rc <= 0) {
            return rc == 0 ? -EIO : rc;
        }
    }
    *count = n;
    return 0;
}

/* The set in the buffer written to `index` of `d`: the entries outside
 * the file entry's sector first, then that sector, whose file entry
 * carries the checksum and, when `publish`, the in-use bit. The file and
 * stream entries share a sector in every set this driver makes; on one
 * that straddles, the stream's sector goes first and the file entry's
 * last, the state between being a checksum the check repairs. */
static int set_store(struct ex_mount *em, struct ex_node *d, uint32_t index, uint32_t count,
                     int publish)
{
    uint32_t per_sector = BLOCK_SECTOR_SIZE / 32;
    uint32_t file_sector = index / per_sector;
    uint32_t k;
    int rc;

    /* The checksum is over the published set, so an inactive file entry
     * written now matches once its in-use bit is set later. */
    em->setbuf[0] |= 0x80;
    {
        uint16_t sum = set_checksum(em->setbuf, count);

        em->setbuf[2] = (uint8_t)sum;
        em->setbuf[3] = (uint8_t)(sum >> 8);
    }
    if (!publish) {
        em->setbuf[0] &= 0x7F;
    }
    /* Entries past the file entry's sector, last first. */
    for (k = count; k-- > 1;) {
        if ((index + k) / per_sector != file_sector) {
            rc = dir_write(em, d, index + k, em->setbuf + k * 32, 1);
            if (rc != 0) {
                return rc;
            }
        }
    }
    /* The file entry's sector: everything of the set in it, in one write. */
    for (k = 1; k < count && (index + k) / per_sector == file_sector; k++) {
    }
    return dir_write(em, d, index, em->setbuf, k);
}

/* A change to a node's file and stream entries: the lengths, the first
 * cluster, the flags, the attributes and the times, from the node. */
static int node_publish(struct ex_mount *em, struct ex_node *n)
{
    struct ex_node d;
    uint32_t count;
    uint8_t *f, *st;
    int rc;

    if (n->is_dir && n->first == em->root) {
        return 0; /* the root has no entry */
    }
    if (n->unlinked) {
        return 0; /* the set is gone; the clusters are the description's until its last put */
    }
    k_memset(&d, 0, sizeof(d));
    d.is_dir = 1;
    d.first = n->dir_cluster == 0 ? em->root : n->dir_cluster;
    if (n->dir_cluster != 0) {
        struct ex_node *pd = node_find(em, 1, 0, 0, n->dir_cluster);

        if (pd) {
            d.nofat = pd->nofat;
            d.length = pd->length;
            pd->refs--;
        } else {
            rc = find_dir(em, em->mnt->root, n->dir_cluster, &pd);
            if (rc != 0) {
                return rc;
            }
            d.nofat = pd->nofat;
            d.length = pd->length;
            ex_put(em->mnt, pd);
        }
    }
    rc = set_load(em, &d, n->index, &count);
    if (rc != 0) {
        return rc;
    }
    f = em->setbuf;
    st = em->setbuf + 32;
    f[4] = (uint8_t)n->attr;
    f[5] = (uint8_t)(n->attr >> 8);
    f[8] = (uint8_t)n->ctime;
    f[9] = (uint8_t)(n->ctime >> 8);
    f[10] = (uint8_t)(n->ctime >> 16);
    f[11] = (uint8_t)(n->ctime >> 24);
    f[12] = (uint8_t)n->mtime;
    f[13] = (uint8_t)(n->mtime >> 8);
    f[14] = (uint8_t)(n->mtime >> 16);
    f[15] = (uint8_t)(n->mtime >> 24);
    f[16] = (uint8_t)n->atime;
    f[17] = (uint8_t)(n->atime >> 8);
    f[18] = (uint8_t)(n->atime >> 16);
    f[19] = (uint8_t)(n->atime >> 24);
    f[20] = n->c10;
    f[21] = n->m10;
    f[22] = n->coff;
    f[23] = n->moff;
    f[24] = n->aoff;
    st[1] = (uint8_t)(FLAG_ALLOC_POSSIBLE | (n->nofat ? FLAG_NO_FAT_CHAIN : 0));
    {
        uint64_t v = n->valid, l = n->length;
        uint32_t i;

        for (i = 0; i < 8; i++) {
            st[8 + i] = (uint8_t)(v >> (8 * i));
            st[24 + i] = (uint8_t)(l >> (8 * i));
        }
        st[20] = (uint8_t)n->first;
        st[21] = (uint8_t)(n->first >> 8);
        st[22] = (uint8_t)(n->first >> 16);
        st[23] = (uint8_t)(n->first >> 24);
    }
    return set_store(em, &d, n->index, count, 1);
}

/* A directory that was read to its end once per mount before its first
 * change: one holding a critical entry this driver does not know is
 * frozen, and every change inside it answers EROFS with a record. */
static int dir_frozen(struct ex_mount *em, struct ex_node *d)
{
    int i;
    uint32_t idx = 0;

    for (i = 0; i < em->nfrozen; i++) {
        if (em->frozen[i] == d->first) {
            return 1;
        }
    }
    if (d->scanned) {
        return 0;
    }
    for (;;) {
        uint8_t raw[32];
        int rc = dir_entry(em, d, idx, raw);
        int bad = 0;

        if (rc < 0) {
            return rc;
        }
        if (rc == 0 || raw[0] == 0x00) {
            break;
        }
        if (raw[0] & 0x80) {
            if (!(raw[0] & 0x40)) {
                /* a primary: critical when bit 5 is clear */
                bad = !(raw[0] & 0x20) && raw[0] != 0x81 && raw[0] != 0x82 && raw[0] != 0x83 &&
                      raw[0] != 0x85;
            } else {
                bad = !(raw[0] & 0x20) && raw[0] != 0xC0 && raw[0] != 0xC1;
            }
        }
        if (bad) {
            klog(KLOG_WARN, "exfat", "directory cluster %d frozen: critical entry 0x%x at %d",
                 (int)d->first, raw[0], (int)idx);
            if (em->nfrozen < EXFAT_FROZEN) {
                em->frozen[em->nfrozen++] = d->first;
            }
            return 1;
        }
        idx++;
    }
    d->scanned = 1;
    return 0;
}

/* The directory grown by one zeroed cluster, chained; a contiguous
 * directory another writer made has its run written into the FAT and its
 * flag cleared first, so that the chain can be followed. */
#define DIR_MAX_BYTES 0x10000000u /* 256 MiB, the format's limit on a directory */

static int dir_grow(struct ex_mount *em, struct ex_node *d)
{
    uint32_t tail, newc, unused;
    uint64_t have = clusters_of(em, d->length);
    int rc;

    if (d->first == em->root) {
        uint32_t c = d->first, steps = 0, next;

        for (;;) {
            if (fat_next(em, c, &next) != 0) {
                return -EIO;
            }
            if (next == EX_EOC || ++steps > em->clusters) {
                break;
            }
            c = next;
        }
        have = steps + 1;
    }
    if ((have + 1) * cluster_bytes(em) > DIR_MAX_BYTES) {
        return -ENOSPC; /* a directory past what the format can name */
    }

    if (d->nofat) {
        rc = chain_into_fat(em, d->first, d->length);
        if (rc != 0) {
            return rc;
        }
        d->nofat = 0;
        rc = node_publish(em, d);
        if (rc != 0) {
            return rc;
        }
    }
    if (d->first == em->root) {
        /* The root's chain is followed to its end: no length to keep. */
        uint32_t c = d->first, steps = 0, next;

        for (;;) {
            rc = fat_next(em, c, &next);
            if (rc != 0) {
                return rc;
            }
            if (next == EX_EOC || ++steps > em->clusters) {
                break;
            }
            c = next;
        }
        tail = c;
    } else {
        rc = chain_at(em, d, d->first, 0, 0, (uint32_t)(have - 1), &tail);
        if (rc != 0) {
            return rc == -ENXIO ? -EIO : rc;
        }
    }
    rc = alloc_run(em, 1, 2, &newc, &unused);
    if (rc != 0) {
        return rc;
    }
    rc = fat_set(em, tail, newc, 1);
    if (rc != 0) {
        free_chain(em, newc, 0, 0);
        return rc;
    }
    d->hint_cluster = 0;
    if (d->first != em->root) {
        d->length += cluster_bytes(em);
        d->valid = d->length;
        rc = node_publish(em, d);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* `count` free slots in a row in `d`, growing it; a run that would put the
 * file entry in a sector's last slot starts in the next sector, an
 * inactive entry filling that slot, so the file and stream entries share
 * a sector. */
static int free_slots(struct ex_mount *em, struct ex_node *d, uint32_t count, uint32_t *index)
{
    uint32_t per_sector = BLOCK_SECTOR_SIZE / 32;
    uint32_t i = 0, run = 0, run_start = 0;
    int end_seen = 0;
    uint8_t raw[32];

    raw[0] = 0;
    for (;;) {
        int rc = dir_entry(em, d, i, raw);

        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            /* The chain ends here, whether or not the end marker was seen:
             * the run must lie in clusters the directory has. */
            rc = dir_grow(em, d);
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        if (raw[0] == 0x00) {
            end_seen = 1; /* every entry from here on is free */
        }
        if (end_seen || !(raw[0] & 0x80)) {
            if (run == 0) {
                if (i % per_sector == per_sector - 1) {
                    /* The last slot of a sector: an inactive entry fills it,
                     * the end marker included, since a scan stops at the
                     * marker and the set past it would be unseen; the
                     * zeros past the set are the marker then. A filler an
                     * earlier run left is passed, not written again. */
                    if (raw[0] == 0x00) {
                        uint8_t filler[32];

                        k_memset(filler, 0, sizeof(filler));
                        filler[0] = 0x05;
                        rc = dir_write(em, d, i, filler, 1);
                        if (rc != 0) {
                            return rc;
                        }
                    }
                    i++;
                    continue;
                }
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
}

/* The set for a new name in the mount's buffer: file entry, stream, names.
 * Its entry count, or an error for a name FAT cannot hold. */
static int set_build(struct ex_mount *em, const char *name, int is_dir, uint32_t first,
                     uint64_t length, uint32_t *count)
{
    uint16_t units[FAT_NAME_UNITS];
    uint32_t n, k, i, entries;
    uint16_t hash;
    uint8_t *f = em->setbuf, *st = em->setbuf + 32;
    uint8_t tenth;
    uint32_t now;
    int rc = fat_name_units(name, units, &n);

    if (rc != 0) {
        return rc;
    }
    rc = name_hash(em, units, n, &hash);
    if (rc != 0) {
        return rc;
    }
    entries = 2 + (n + 14) / 15;
    k_memset(em->setbuf, 0, entries * 32);
    now = ex_stamp_now(&tenth);
    f[0] = 0x85;
    f[1] = (uint8_t)(entries - 1);
    f[4] = (uint8_t)(is_dir ? ATTR_DIRECTORY : 0x20);
    for (i = 0; i < 4; i++) {
        f[8 + i] = f[12 + i] = f[16 + i] = (uint8_t)(now >> (8 * i));
    }
    f[20] = f[21] = tenth;
    f[22] = f[23] = f[24] = 0x80; /* UTC, the offset valid and zero */
    st[0] = 0xC0;
    st[1] = FLAG_ALLOC_POSSIBLE;
    st[3] = (uint8_t)n;
    st[4] = (uint8_t)hash;
    st[5] = (uint8_t)(hash >> 8);
    for (i = 0; i < 8; i++) {
        st[8 + i] = st[24 + i] = (uint8_t)(length >> (8 * i));
    }
    st[20] = (uint8_t)first;
    st[21] = (uint8_t)(first >> 8);
    st[22] = (uint8_t)(first >> 16);
    st[23] = (uint8_t)(first >> 24);
    for (i = 0; i < (n + 14) / 15; i++) {
        uint8_t *ne = em->setbuf + (2 + i) * 32;

        ne[0] = 0xC1;
        for (k = 0; k < 15 && i * 15 + k < n; k++) {
            ne[2 + k * 2] = (uint8_t)units[i * 15 + k];
            ne[3 + k * 2] = (uint8_t)(units[i * 15 + k] >> 8);
        }
    }
    *count = entries;
    return 0;
}

/* The set with `name` in `d`, found as lookup finds it: 1 with the set,
 * 0, or an error. */
static int find_set(struct ex_mount *em, struct ex_node *d, const char *name, struct ex_set *set)
{
    uint16_t units[FAT_NAME_UNITS];
    uint32_t n, cursor = 0;
    uint16_t hash;
    int rc = fat_name_units(name, units, &n);

    if (rc != 0) {
        return rc == -EINVAL ? 0 : rc;
    }
    rc = name_hash(em, units, n, &hash);
    if (rc != 0) {
        return rc;
    }
    for (;;) {
        int same;

        rc = dir_scan(em, d, &cursor, set);
        if (rc <= 0) {
            return rc;
        }
        if (set->count != n || le16(set->stream + 4) != hash) {
            continue;
        }
        rc = units_match(em, set->units, units, n, &same);
        if (rc != 0) {
            return rc;
        }
        if (same) {
            return 1;
        }
    }
}

/* A set unpublished: the file entry inactive first, the barrier, then
 * the secondaries inactive. */
static int set_retire(struct ex_mount *em, struct ex_node *d, uint32_t index, uint32_t count)
{
    uint32_t k;
    uint8_t raw[32];
    int rc = dir_entry(em, d, index, raw);

    if (rc <= 0) {
        return rc == 0 ? -EIO : rc;
    }
    raw[0] &= 0x7F;
    rc = dir_write(em, d, index, raw, 1);
    if (rc != 0) {
        return rc;
    }
    for (k = 1; k < count; k++) {
        rc = dir_entry(em, d, index + k, raw);
        if (rc <= 0) {
            return rc == 0 ? -EIO : rc;
        }
        raw[0] &= 0x7F;
        rc = dir_write(em, d, index + k, raw, 1);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

static int writable_dir(struct vfs_mount *mnt, struct ex_mount *em, struct ex_node *d)
{
    int rc;

    if (!em->writable || (mnt->flags & VFS_MS_RDONLY)) {
        return -EROFS;
    }
    assert_locked(mnt);
    rc = dir_frozen(em, d);
    if (rc < 0) {
        return rc;
    }
    return rc ? -EROFS : 0;
}

/* Whether `d` holds an in-use file entry. */
static int dir_has_entries(struct ex_mount *em, struct ex_node *d, int *has)
{
    uint32_t i = 0;

    for (;;) {
        uint8_t raw[32];
        int rc = dir_entry(em, d, i, raw);

        if (rc < 0) {
            return rc;
        }
        if (rc == 0 || raw[0] == 0x00) {
            *has = 0;
            return 0;
        }
        if (raw[0] == 0x85) {
            *has = 1;
            return 0;
        }
        i++;
    }
}

/* ---- the mutating operations ---- */

static int ex_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode, void **node)
{
    struct ex_mount *em = em_of(mnt);
    struct ex_node *d = dir ? dir : mnt->root;
    struct ex_set set;
    uint32_t count, index;
    struct ex_node *n;
    int rc = writable_dir(mnt, em, d);

    (void)mode;
    if (rc != 0) {
        return rc;
    }
    rc = find_set(em, d, name, &set);
    if (rc < 0) {
        return rc;
    }
    if (rc == 1) {
        return -EEXIST;
    }
    rc = set_build(em, name, 0, 0, 0, &count);
    if (rc != 0) {
        return rc;
    }
    rc = free_slots(em, d, count, &index);
    if (rc != 0) {
        return rc;
    }
    rc = set_store(em, d, index, count, 1);
    if (rc != 0) {
        return rc;
    }
    k_memcpy(set.file, em->setbuf, 32);
    k_memcpy(set.stream, em->setbuf + 32, 32);
    set.index = index;
    set.entries = count;
    n = node_for(em, d, &set);
    if (!n) {
        return -ENOMEM;
    }
    *node = n;
    return 0;
}

static int ex_mkdir(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode)
{
    struct ex_mount *em = em_of(mnt);
    struct ex_node *d = dir ? dir : mnt->root;
    struct ex_set set;
    uint32_t count, index = 0, c, unused;
    int rc = writable_dir(mnt, em, d);

    (void)mode;
    if (rc != 0) {
        return rc;
    }
    rc = find_set(em, d, name, &set);
    if (rc < 0) {
        return rc;
    }
    if (rc == 1) {
        return -EEXIST;
    }
    /* The cluster first, zeroed through and ended, then the set naming it
     * last, so a crash between leaks the cluster and names nothing. */
    rc = alloc_run(em, 1, 2, &c, &unused);
    if (rc != 0) {
        return rc;
    }
    rc = set_build(em, name, 1, c, cluster_bytes(em), &count);
    if (rc == 0) {
        rc = free_slots(em, d, count, &index);
    }
    if (rc == 0) {
        rc = set_store(em, d, index, count, 1);
    }
    if (rc != 0) {
        free_chain(em, c, 0, 0);
    }
    return rc;
}

/* The chain of a set that is going: kept for a description that holds
 * the node, freed at its last put; freed now otherwise. */
static int retire_chain(struct ex_mount *em, struct ex_node *d, const struct ex_set *set)
{
    uint32_t dir_cluster = d->first == em->root ? 0 : d->first;
    int is_dir = (le16(set->file + 4) & ATTR_DIRECTORY) != 0;
    uint32_t first = le32(set->stream + 20);
    struct ex_node *n;
    uint32_t k;

    /* A vendor allocation names clusters of its own; they go with the set,
     * whoever wrote it, or nothing would ever free them. */
    for (k = 2; k < set->entries; k++) {
        uint8_t e[32];
        int rc = dir_entry(em, d, set->index + k, e);

        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            break;
        }
        if ((e[0] & 0x7F) == 0x61 && (e[1] & FLAG_ALLOC_POSSIBLE) && le32(e + 20) != 0) {
            rc = free_chain(em, le32(e + 20), (e[1] & FLAG_NO_FAT_CHAIN) != 0, le64(e + 24));
            if (rc != 0) {
                return rc;
            }
        }
    }
    n = node_find(em, is_dir, dir_cluster, set->index, first);

    if (n) {
        n->refs--; /* node_find held it once more */
        if (n->refs > 0) {
            n->unlinked = 1;
            return 0;
        }
        node_drop(em, n);
    }
    return first ? free_chain(em, first, (set->stream[1] & FLAG_NO_FAT_CHAIN) != 0,
                              le64(set->stream + 24))
                 : 0;
}

static int ex_unlink(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct ex_mount *em = em_of(mnt);
    struct ex_node *d = dir ? dir : mnt->root;
    struct ex_set set;
    int rc = writable_dir(mnt, em, d);

    if (rc != 0) {
        return rc;
    }
    rc = find_set(em, d, name, &set);
    if (rc < 0) {
        return rc;
    }
    if (rc == 0) {
        return -ENOENT;
    }
    if (le16(set.file + 4) & ATTR_DIRECTORY) {
        return -EISDIR;
    }
    rc = set_retire(em, d, set.index, set.entries);
    if (rc != 0) {
        return rc;
    }
    return retire_chain(em, d, &set);
}

static int ex_rmdir(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct ex_mount *em = em_of(mnt);
    struct ex_node *d = dir ? dir : mnt->root;
    struct ex_set set;
    struct ex_node sub, *held;
    int has, rc = writable_dir(mnt, em, d);

    if (rc != 0) {
        return rc;
    }
    rc = find_set(em, d, name, &set);
    if (rc < 0) {
        return rc;
    }
    if (rc == 0) {
        return -ENOENT;
    }
    if (!(le16(set.file + 4) & ATTR_DIRECTORY)) {
        return -ENOTDIR;
    }
    k_memset(&sub, 0, sizeof(sub));
    sub.is_dir = 1;
    sub.first = le32(set.stream + 20);
    sub.length = le64(set.stream + 24);
    sub.nofat = (set.stream[1] & FLAG_NO_FAT_CHAIN) != 0;
    rc = dir_has_entries(em, &sub, &has);
    if (rc != 0) {
        return rc;
    }
    if (has) {
        return -ENOTEMPTY;
    }
    held = node_find(em, 1, 0, 0, sub.first);
    if (held) {
        held->refs--;
        if (held->refs > 0) {
            return -EBUSY; /* a description or a working directory */
        }
    }
    rc = dir_frozen(em, &sub);
    if (rc < 0) {
        return rc;
    }
    if (rc) {
        return -EROFS; /* a frozen directory is not removed */
    }
    rc = set_retire(em, d, set.index, set.entries);
    if (rc != 0) {
        return rc;
    }
    return retire_chain(em, d, &set);
}

/* Whether the directory at `dir` (0 for the root) is `anc` or lies under
 * it, by the parents the nodes remember and the tree otherwise. */
static int is_under(struct ex_mount *em, uint32_t dir, uint32_t anc)
{
    uint32_t steps = 0;

    while (dir != 0 && steps++ < em->clusters) {
        struct ex_node *n;
        uint32_t parent;

        if (dir == anc) {
            return 1;
        }
        n = node_find(em, 1, 0, 0, dir);
        if (!n) {
            int rc = find_dir(em, em->mnt->root, dir, &n);

            if (rc != 0) {
                return rc;
            }
        }
        parent = n->parent;
        ex_put(em->mnt, n);
        dir = parent;
    }
    return 0;
}

static int ex_rename(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
                     const char *newname)
{
    struct ex_mount *em = em_of(mnt);
    struct ex_node *od = olddir ? olddir : mnt->root;
    struct ex_node *nd = newdir ? newdir : mnt->root;
    uint32_t od_cluster = od->first == em->root ? 0 : od->first;
    uint32_t nd_cluster = nd->first == em->root ? 0 : nd->first;
    struct ex_set src, dst;
    struct ex_node *moved;
    uint32_t count, index;
    int is_dir, dst_exists, rc = writable_dir(mnt, em, od);

    if (rc == 0 && nd != od) {
        rc = writable_dir(mnt, em, nd);
    }
    if (rc != 0) {
        return rc;
    }
    rc = find_set(em, od, oldname, &src);
    if (rc < 0) {
        return rc;
    }
    if (rc == 0) {
        return -ENOENT;
    }
    is_dir = (le16(src.file + 4) & ATTR_DIRECTORY) != 0;
    rc = find_set(em, nd, newname, &dst);
    if (rc < 0) {
        return rc;
    }
    dst_exists = rc == 1;
    if (dst_exists) {
        int dst_dir = (le16(dst.file + 4) & ATTR_DIRECTORY) != 0;

        if (od_cluster == nd_cluster && dst.index == src.index) {
            return 0; /* the same set under a spelling of its own name */
        }
        if (is_dir && !dst_dir) {
            return -ENOTDIR;
        }
        if (!is_dir && dst_dir) {
            return -EISDIR;
        }
        if (dst_dir) {
            struct ex_node sub, *held;
            int has;

            k_memset(&sub, 0, sizeof(sub));
            sub.is_dir = 1;
            sub.first = le32(dst.stream + 20);
            sub.length = le64(dst.stream + 24);
            sub.nofat = (dst.stream[1] & FLAG_NO_FAT_CHAIN) != 0;
            rc = dir_has_entries(em, &sub, &has);
            if (rc != 0) {
                return rc;
            }
            if (has) {
                return -ENOTEMPTY;
            }
            held = node_find(em, 1, 0, 0, sub.first);
            if (held) {
                held->refs--;
                if (held->refs > 0) {
                    return -EBUSY;
                }
            }
        }
    }
    if (is_dir) {
        struct ex_node sub;

        k_memset(&sub, 0, sizeof(sub));
        sub.is_dir = 1;
        sub.first = le32(src.stream + 20);
        sub.length = le64(src.stream + 24);
        sub.nofat = (src.stream[1] & FLAG_NO_FAT_CHAIN) != 0;
        rc = dir_frozen(em, &sub);
        if (rc < 0) {
            return rc;
        }
        if (rc) {
            return -EROFS; /* a frozen directory is not renamed */
        }
        if (od_cluster != nd_cluster) {
            rc = is_under(em, nd_cluster, sub.first);
            if (rc < 0) {
                return rc;
            }
            if (rc) {
                return -EINVAL;
            }
        }
    }
    /* Held through the writes below, which sleep: a last close meanwhile
     * would free the node this rename is about to update. */
    moved = node_find(em, is_dir, od_cluster, src.index, le32(src.stream + 20));
    /* The new set in the buffer: the source's file and stream entries
     * under the new name, so the times, the chain and the flags stay. */
    rc = set_build(em, newname, is_dir, 0, 0, &count);
    if (rc != 0) {
        goto out;
    }
    {
        uint8_t hash_lo = em->setbuf[32 + 4], hash_hi = em->setbuf[32 + 5],
                nlen = em->setbuf[32 + 3];

        uint32_t k, src_names = 2 + (src.count + 14) / 15;

        k_memcpy(em->setbuf, src.file, 32);
        k_memcpy(em->setbuf + 32, src.stream, 32);
        em->setbuf[32 + 3] = nlen;
        em->setbuf[32 + 4] = hash_lo;
        em->setbuf[32 + 5] = hash_hi;
        /* The benign secondaries the source carried, a vendor allocation
         * among them, follow the new name entries. */
        for (k = src_names; k < src.entries && count < 256; k++) {
            uint8_t e[32];

            rc = dir_entry(em, od, src.index + k, e);
            if (rc < 0) {
                goto out;
            }
            if (rc == 0) {
                break;
            }
            k_memcpy(em->setbuf + count * 32, e, 32);
            count++;
        }
        em->setbuf[1] = (uint8_t)(count - 1);
    }
    if (od_cluster == nd_cluster && !dst_exists && count == src.entries) {
        /* In place: the name entries first, then the file entry's sector. */
        rc = set_store(em, od, src.index, count, 1);
        if (rc == 0 && moved) {
            moved->index = src.index;
        }
        goto out;
    }
    /* Elsewhere: the room first, so ENOSPC destroys nothing; the new set
     * written with an inactive file entry; a replaced destination and the
     * source unpublished; the new file entry published last. */
    rc = free_slots(em, nd, count, &index);
    if (rc != 0) {
        goto out;
    }
    if (dst_exists && dst.index >= index && dst.index < index + count) {
        rc = -EIO;
        goto out; /* the room found lies inside the destination's set */
    }
    rc = set_store(em, nd, index, count, 0);
    if (rc != 0) {
        goto out;
    }
    if (dst_exists) {
        rc = set_retire(em, nd, dst.index, dst.entries);
        if (rc == 0) {
            rc = retire_chain(em, nd, &dst);
        }
        if (rc != 0) {
            goto out;
        }
    }
    rc = set_retire(em, od, src.index, src.entries);
    if (rc == 0) {
        /* The retirements on the medium before the new name is published:
         * a drive that keeps writes pending could otherwise put the
         * publication first, and a crash between would leave two names
         * on one chain, where the order chosen leaves none. */
        rc = block_flush(em->dev);
    }
    if (rc != 0) {
        goto out;
    }
    {
        uint8_t raw[32];

        k_memcpy(raw, em->setbuf, 32);
        raw[0] = 0x85;
        rc = dir_write(em, nd, index, raw, 1);
        if (rc != 0) {
            goto out;
        }
    }
    if (moved) {
        moved->dir_cluster = nd_cluster;
        moved->index = index;
        if (is_dir) {
            moved->parent = nd_cluster;
        }
    }
    rc = 0;
out:
    if (moved) {
        ex_put(mnt, moved);
    }
    return rc;
}

/* Bytes of a file: `count` from `buf` (zeros for NULL) at `off`, through
 * the chain, for writeback. */
static int put_file_bytes(struct ex_mount *em, struct ex_node *n, uint64_t off, const uint8_t *buf,
                          uint32_t count)
{
    uint32_t cbytes = cluster_bytes(em);

    while (count > 0) {
        uint32_t in = (uint32_t)(off & (cbytes - 1));
        uint32_t take = cbytes - in;
        uint32_t c;
        int rc;

        if (take > count) {
            take = count;
        }
        rc = chain_at(em, n, n->first, n->nofat, n->length, (uint32_t)(off >> (9 + em->spc_shift)),
                      &c);
        if (rc != 0) {
            return rc == -ENXIO ? -EIO : rc;
        }
        rc = ex_write_bytes(em, cluster_offset(em, c) + in, buf, take, 0);
        if (rc != 0) {
            return rc;
        }
        if (buf) {
            buf += take;
        }
        off += take;
        count -= take;
    }
    return 0;
}

/* A contiguous file about to grow: its run into the FAT, the flag
 * cleared and published, then the chain can be joined to. */
static int chain_convert(struct ex_mount *em, struct ex_node *n)
{
    int rc;

    if (!n->nofat || n->first == 0) {
        n->nofat = 0;
        return 0;
    }
    rc = chain_into_fat(em, n->first, n->length);
    if (rc != 0) {
        return rc;
    }
    n->nofat = 0;
    return node_publish(em, n);
}

static void stamp_mtime(struct ex_node *n)
{
    uint8_t tenth;

    n->mtime = ex_stamp_now(&tenth);
    n->m10 = tenth;
    n->moff = 0x80;
}

static int32_t ex_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                        uint64_t offset)
{
    struct ex_mount *em = em_of(mnt);
    struct ex_node *n = handle;
    uint32_t cbytes = cluster_bytes(em);
    uint64_t have, needed, end;
    uint32_t run_first = 0, run_last = 0, tail = 0;
    int joined = 0;
    int rc;

    if (n->is_dir) {
        return -EISDIR;
    }
    if (count == 0) {
        return 0;
    }
    if (offset > EX_FILE_LIMIT || count > EX_FILE_LIMIT - offset) {
        klog(KLOG_WARN, "exfat", "write: offset past the format's limit");
        return -EFBIG;
    }
    rc = begin_write(mnt, em);
    if (rc != 0) {
        return rc;
    }
    assert_locked(mnt);
    end = offset + count;
    have = clusters_of(em, n->length);
    needed = clusters_of(em, end);
    if (needed > have) {
        uint64_t want = needed - have;

        /* A cluster count is 32 bits and the volume has no more than
         * `clusters` of them. Casting a larger demand would ask for its
         * low half and put the bytes at the wrong cluster. */
        if (want > (uint64_t)em->clusters) {
            rc = -ENOSPC;
            goto out;
        }
        rc = room_for(em, (uint32_t)want);
        if (rc == 0) {
            rc = chain_convert(em, n); /* nothing is written before the room is known */
        }
        if (rc == 0) {
            rc = alloc_run(em, (uint32_t)want, 1, &run_first, &run_last);
        }
        if (rc != 0) {
            goto out;
        }
    }
    /* A write past the valid length zero-fills the gap in the old clusters
     * first, including the part inside the last one; the run's zeros are
     * its own fill. */
    if (offset > n->valid && have > 0) {
        uint64_t gap_end = have * cbytes < offset ? have * cbytes : offset;

        if (gap_end > n->valid) {
            uint64_t at = n->valid;

            /* The gap can be longer than a 32-bit count. One cast of it
             * would clear nothing and then publish a valid length over
             * clusters that still hold whatever was on the medium. */
            while (at < gap_end) {
                uint64_t left = gap_end - at;
                uint32_t piece = left > 0x40000000u ? 0x40000000u : (uint32_t)left;

                rc = put_file_bytes(em, n, at, 0, piece);
                if (rc != 0) {
                    goto undo;
                }
                at += piece;
            }
        }
    }
    if (offset < have * cbytes) {
        uint64_t room = have * cbytes - offset; /* up to 2^32 at the limit */
        uint32_t part = room > count ? count : (uint32_t)room;

        rc = put_file_bytes(em, n, offset, buf, part);
        if (rc == 0 && part < count) {
            struct ex_node run;

            k_memset(&run, 0, sizeof(run));
            run.first = run_first;
            run.length = (uint64_t)(needed - have) * cbytes;
            rc = put_file_bytes(em, &run, offset + part - have * cbytes,
                                (const uint8_t *)buf + part, count - part);
        }
    } else {
        struct ex_node run;

        k_memset(&run, 0, sizeof(run));
        run.first = run_first;
        run.length = (uint64_t)(needed - have) * cbytes;
        rc = put_file_bytes(em, &run, offset - have * cbytes, buf, count);
    }
    if (rc != 0) {
        goto undo;
    }
    rc = sync_disk(em); /* the data and the zeros reach the disk first */
    if (rc != 0) {
        goto undo;
    }
    if (run_first != 0 && have > 0) {
        rc = chain_at(em, n, n->first, 0, 0, (uint32_t)(have - 1), &tail);
        if (rc == 0) {
            rc = fat_set(em, tail, run_first, 1);
            joined = rc == 0;
        }
        if (rc == 0) {
            rc = block_flush(em->dev); /* the join on the medium before the lengths */
        }
        if (rc != 0) {
            goto undo;
        }
    } else if (run_first != 0) {
        n->first = run_first; /* published with the lengths below */
    }
    if (end > n->length) {
        n->length = end;
    }
    if (end > n->valid) {
        n->valid = end;
    }
    stamp_mtime(n);
    rc = node_publish(em, n);
    if (rc != 0) {
        goto out; /* the chain is joined; the length is the walk's surplus */
    }
    /* The hint stays: a write changes no cluster the chain had, so the
     * position it remembers is still that cluster, and the next append
     * walks from there rather than from the first cluster again. */
    vfs_ns_unlock(mnt);
    return (int32_t)count;
undo:
    if (joined && fat_set(em, tail, EX_EOC, 1) != 0) {
        /* The join is in the cache and may be on the medium: a tail that
         * cannot be ended again keeps its run, as lost clusters a check
         * frees, rather than a chain that runs into free ones. The
         * ending reaches the medium in `free_chain`, before it frees. */
        goto out;
    }
    if (run_first != 0) {
        free_chain(em, run_first, 0, 0);
    }
out:
    vfs_ns_unlock(mnt);
    return rc;
}

static int ex_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    struct ex_mount *em = em_of(mnt);
    struct ex_node *n = handle;
    uint32_t cbytes = cluster_bytes(em);
    uint64_t have, keep;
    int rc;

    if (n->is_dir) {
        return -EISDIR;
    }
    if (size > EX_FILE_LIMIT) {
        klog(KLOG_WARN, "exfat", "truncate: size past the format's limit");
        return -EFBIG;
    }
    rc = begin_write(mnt, em);
    if (rc != 0) {
        return rc;
    }
    assert_locked(mnt);
    have = clusters_of(em, n->length);
    keep = clusters_of(em, size);
    if (size > n->length) {
        /* A grow: new clusters, chained, unzeroed, since the valid length
         * stays where it was and the new range reads as zeros. */
        uint32_t run_first = 0, run_last = 0;

        if (keep > have) {
            uint64_t want = keep - have;

            if (want > (uint64_t)em->clusters) {
                rc = -ENOSPC;
                goto out;
            }
            rc = room_for(em, (uint32_t)want);
            if (rc == 0) {
                rc = chain_convert(em, n); /* nothing is written before the room is known */
            }
            if (rc == 0) {
                rc = alloc_run(em, (uint32_t)want, 0, &run_first, &run_last);
            }
            if (rc != 0) {
                goto out;
            }
            rc = sync_disk(em); /* the run's bits and entries, before the join */
            if (rc != 0) {
                free_chain(em, run_first, 0, 0);
                goto out;
            }
            if (have > 0) {
                uint32_t tail;

                rc = chain_at(em, n, n->first, 0, 0, (uint32_t)(have - 1), &tail);
                if (rc == 0) {
                    rc = fat_set(em, tail, run_first, 1);
                }
                if (rc == 0) {
                    /* The link on the medium before the length that
                     * counts on it, or a crash leaves a chain shorter
                     * than its length. */
                    rc = block_flush(em->dev);
                    if (rc != 0) {
                        (void)fat_set(em, tail, EX_EOC, 1);
                    }
                }
                if (rc != 0) {
                    free_chain(em, run_first, 0, 0);
                    goto out;
                }
            } else {
                n->first = run_first;
            }
        }
        n->length = size;
        stamp_mtime(n);
        rc = node_publish(em, n);
        n->hint_cluster = 0;
        goto out;
    }
    /* A shrink: the lengths first, the retained tail ended, the detached
     * tail freed last. */
    {
        uint32_t cut_first = 0, tail = 0;
        int was_nofat = n->nofat;
        uint64_t old_length = n->length;

        if (keep < have) {
            if (keep > 0) {
                rc = chain_at(em, n, n->first, n->nofat, n->length, (uint32_t)keep, &cut_first);
                if (rc == 0 && !n->nofat) {
                    rc = chain_at(em, n, n->first, 0, 0, (uint32_t)(keep - 1), &tail);
                }
                if (rc != 0) {
                    goto out;
                }
            } else {
                cut_first = n->first;
            }
        }
        n->length = size;
        if (n->valid > size) {
            n->valid = size;
        }
        if (keep == 0) {
            n->first = 0;
            n->nofat = 0;
        }
        stamp_mtime(n);
        rc = node_publish(em, n);
        if (rc == 0) {
            /* The shorter lengths on the medium before the tail is cut and
             * freed: a drive that keeps writes pending could otherwise
             * free the tail under lengths that still cover it. */
            rc = block_flush(em->dev);
        }
        if (rc != 0) {
            goto out;
        }
        n->hint_cluster = 0;
        if (keep < have) {
            if (keep > 0 && !was_nofat) {
                rc = fat_set(em, tail, EX_EOC, 1);
                if (rc != 0) {
                    goto out;
                }
            }
            rc = free_chain(em, cut_first, was_nofat, was_nofat ? (have - keep) * cbytes : 0);
        }
        (void)old_length;
    }
out:
    vfs_ns_unlock(mnt);
    return rc;
}

static int ex_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st)
{
    struct ex_mount *em = em_of(mnt);
    struct ex_node *n = node;
    int rc;

    if (what & ~(VFS_SET_MODE | VFS_SET_ATIME | VFS_SET_MTIME)) {
        return -EPERM;
    }
    if (what & VFS_SET_MODE) {
        uint32_t base = 0777u & ~em->umask;
        uint32_t want = st->mode & 07777u;

        if ((want & ~0222u) != (base & ~0222u)) {
            return -EPERM;
        }
    }
    if (n->is_dir && n->first == em->root) {
        return 0;
    }
    rc = begin_write(mnt, em);
    if (rc != 0) {
        return rc;
    }
    assert_locked(mnt);
    if (what & VFS_SET_MODE) {
        if (st->mode & 0222u) {
            n->attr = (uint16_t)(n->attr & ~ATTR_READ_ONLY);
        } else {
            n->attr |= ATTR_READ_ONLY;
        }
    }
    if (what & VFS_SET_ATIME) {
        uint16_t date, time;

        fat_stamp(st->atime_sec, &date, &time);
        n->atime = ((uint32_t)date << 16) | time;
        n->aoff = 0x80;
    }
    if (what & VFS_SET_MTIME) {
        uint16_t date, time;

        fat_stamp(st->mtime_sec, &date, &time);
        n->mtime = ((uint32_t)date << 16) | time;
        /* The stamp's field holds two seconds; the 10 ms count carries the
         * odd second, which is what its range of 0 to 199 is for. */
        n->m10 = (uint8_t)((uint32_t)st->mtime_nsec / 10000000u % 100u);
        if (fat_epoch(date, time) == st->mtime_sec - 1) {
            n->m10 = (uint8_t)(n->m10 + 100);
        }
        n->moff = 0x80;
    }
    rc = node_publish(em, n);
    vfs_ns_unlock(mnt);
    return rc;
}

static int ex_fsync(struct vfs_mount *mnt, void *handle)
{
    int rc;

    (void)handle;
    vfs_ns_lock(mnt);
    rc = sync_disk(em_of(mnt));
    vfs_ns_unlock(mnt);
    return rc;
}

/* The main boot sector's flags and percent in use, written through; the
 * backup region is never written, and the checksum skips these bytes. */
static int write_boot_state(struct ex_mount *em, int dirty)
{
    uint8_t flags[2], percent;
    uint32_t used = em->free_known ? em->clusters - em->free_clusters : 0;
    int rc;

    /* The percent first, so that the write which clears the dirty bit is
     * the last of the two: a stop between them leaves the volume dirty
     * with a percent that is already right. */
    if (em->free_known) {
        uint32_t u = used, c = em->clusters;

        while (c > 42949672u) { /* so that u * 100 stays inside 32 bits */
            c >>= 1;
            u >>= 1;
        }
        percent = c ? (uint8_t)(u * 100u / c) : 0;
    } else {
        percent = 0xFF; /* the specification's "not known" */
    }
    rc = ex_write_bytes(em, 112, &percent, 1, 1);
    if (rc != 0) {
        return rc;
    }
    flags[0] = (uint8_t)((em->active_fat & 1) | (dirty ? 2 : 0));
    flags[1] = 0;
    return ex_write_bytes(em, 106, flags, 2, 1);
}

static int ex_sync(struct vfs_mount *mnt)
{
    struct ex_mount *em = em_of(mnt);
    int rc;

    if (!em->writable) {
        return 0;
    }
    vfs_ns_lock(mnt);
    if (!em->dirty_set) {
        vfs_ns_unlock(mnt);
        return 0;
    }
    rc = sync_disk(em);
    if (rc == 0) {
        rc = write_boot_state(em, 1);
    }
    vfs_ns_unlock(mnt);
    return rc;
}

/* The clean unmount: every dirty buffer written, the percent in use, then
 * the dirty bit cleared last. A failure leaves the volume marked dirty
 * and the mount where it was. */
static int ex_prepare_unmount(struct vfs_mount *mnt)
{
    struct ex_mount *em = em_of(mnt);
    int rc;

    if (!em->writable || (mnt->flags & VFS_MS_RDONLY) || !em->dirty_set) {
        return 0;
    }
    rc = sync_disk(em);
    if (rc == 0) {
        rc = write_boot_state(em, 0);
    }
    if (rc != 0) {
        return rc;
    }
    em->dirty_set = 0;
    return 0;
}

/* ---- the check of a dirty volume at mount ----
 *
 * A volume whose dirty bit is set was written by something that did not
 * unmount it. Before a writable mount is published the tree is walked
 * under the bitmap the FAT drivers share, one walk at a time, seeded with
 * the root's chain, the bitmap's and the up-case table's; every set the
 * tree reaches is checked as the reader checks it and its chain marked.
 * Three states this driver's own order leaves behind a crash are repaired,
 * each a metadata write, in an order a second crash cannot make worse: a
 * set failing only its checksum has it rewritten, orphan secondaries are
 * written inactive, and a surplus tail is detached at the last cluster the
 * length justifies and then freed. A cluster allocated that nothing
 * reaches is counted and left. Anything else mounts read-only with the
 * first fault named.
 */

#define CHECK_DEPTH   32
#define CHECK_TAILS   16
#define CHECK_SETS    16   /* checksum-only sets recorded for the rewrite */
#define CHECK_ORPHANS 64   /* orphan secondaries recorded, per volume */
#define CHECK_NAMES   4096 /* names of one directory compared for a duplicate */

struct check_state {
    struct ex_mount *em;
    const char *why;
    int repaired;
    uint32_t lost;
    int ntails;
    uint32_t tail_prev[CHECK_TAILS], tail_first[CHECK_TAILS];
    int nsets;
    uint32_t set_dir[CHECK_SETS], set_index[CHECK_SETS];
    uint64_t set_dir_length[CHECK_SETS];
    uint8_t set_dir_nofat[CHECK_SETS];
    int norphans;
    uint32_t orphan_dir[CHECK_ORPHANS], orphan_index[CHECK_ORPHANS];
    uint64_t orphan_dir_length[CHECK_ORPHANS];
    uint8_t orphan_dir_nofat[CHECK_ORPHANS];
};

static uint16_t check_hashes[CHECK_NAMES];
static uint32_t check_hash_index[CHECK_NAMES];

/* Mark cluster `c`: a fault when it was marked already or its bitmap bit
 * is clear. */
static int check_mark(struct check_state *w, uint32_t c)
{
    uint8_t byte;
    int rc;

    if (!in_heap(w->em, c)) {
        w->why = "a chain outside the heap";
        return -1;
    }
    if (fat_walk_test_set(c)) {
        w->why = "a cluster reached twice";
        return -1;
    }
    rc = object_read(w->em, w->em->bitmap_first, &w->em->bitmap_walk, (uint64_t)(c - 2) / 8, &byte,
                     1);
    if (rc != 0) {
        w->why = "an unreadable bitmap";
        return -1;
    }
    if (!(byte & (1u << ((c - 2) % 8)))) {
        w->why = "a reached cluster whose bitmap bit is clear";
        return -1;
    }
    return 0;
}

/* A chain marked: `length` bytes from `first`, contiguous or through the
 * FAT; a FAT chain longer than the length is recorded as a surplus tail
 * and walked to its end; one shorter is a fault. The cluster count, or -1. */
static long check_chain(struct check_state *w, uint32_t first, int nofat, uint64_t length)
{
    struct ex_mount *em = w->em;
    uint64_t need = clusters_of(em, length);
    uint32_t c = first, prev = 0;
    long count = 0;

    if (nofat) {
        uint64_t i;

        for (i = 0; i < need; i++) {
            if (check_mark(w, (uint32_t)(first + i)) != 0) {
                return -1;
            }
        }
        return (long)need;
    }
    while (c != EX_EOC) {
        uint32_t next;

        if (check_mark(w, c) != 0) {
            return -1;
        }
        count++;
        /* The root's chain has no length: followed to its end, no surplus. */
        if (length != 0 && (uint64_t)count == need + 1) {
            if (w->ntails == CHECK_TAILS) {
                w->why = "more surplus tails than the walk repairs";
                return -1;
            }
            w->tail_prev[w->ntails] = prev;
            w->tail_first[w->ntails] = c;
            w->ntails++;
        }
        if (fat_next(em, c, &next) != 0) {
            w->why = "an unreadable FAT";
            return -1;
        }
        if (next == EX_BAD || next == 0) {
            w->why = "a chain ending in a free or bad entry";
            return -1;
        }
        if (next != EX_EOC && !in_heap(em, next)) {
            w->why = "a chain outside the heap";
            return -1;
        }
        if ((uint32_t)count > em->clusters) {
            w->why = "a loop";
            return -1;
        }
        prev = c;
        c = next;
    }
    if (length != 0 && (uint64_t)count < need) {
        w->why = "a stream shorter than its length";
        return -1;
    }
    return count;
}

/* One directory walked from `*resume`: its sets checked, chains marked,
 * orphans and checksum-only sets recorded, names compared. 1 with a
 * subdirectory in `*sub` and `*sub_length`/`*sub_nofat` to descend into,
 * 0 at the end, -1 for a fault. */
static int check_dir(struct check_state *w, struct ex_node *d, uint32_t *resume, uint32_t *sub,
                     uint64_t *sub_length, int *sub_nofat, uint32_t base, uint32_t *nnames)
{
    struct ex_mount *em = w->em;
    uint32_t i = *resume;
    int is_root = d->first == em->root;

    for (;;) {
        uint8_t raw[32];
        struct ex_set set;
        int rc = dir_entry(em, d, i, raw);
        uint32_t k;

        if (rc < 0) {
            w->why = "an unreadable directory";
            return -1;
        }
        if (rc == 0 || raw[0] == 0x00) {
            *resume = i;
            return 0;
        }
        if (!(raw[0] & 0x80)) {
            i++;
            continue;
        }
        if (raw[0] & 0x40) {
            /* An in-use secondary outside a set: an orphan, freed later. */
            if (w->norphans == CHECK_ORPHANS) {
                w->why = "more orphan secondaries than the walk repairs";
                return -1;
            }
            w->orphan_dir[w->norphans] = d->first;
            w->orphan_index[w->norphans] = i;
            w->orphan_dir_length[w->norphans] = d->length;
            w->orphan_dir_nofat[w->norphans] = (uint8_t)d->nofat;
            w->norphans++;
            i++;
            continue;
        }
        if (raw[0] == 0x81 || raw[0] == 0x82 || raw[0] == 0x83 || raw[0] == 0xA0 ||
            raw[0] == 0xA1) {
            /* The bitmap, the up-case table and the label belong to the
             * root; in another directory they make it invalid. */
            if (!is_root && raw[0] != 0xA0 && raw[0] != 0xA1) {
                w->why = "a root-only critical entry in a subdirectory";
                return -1;
            }
            i++;
            continue;
        }
        if (raw[0] != 0x85) {
            w->why = "a critical entry this driver does not know";
            return -1;
        }
        rc = set_read(em, d, i, raw, &set);
        if (rc < 0) {
            w->why = "an unreadable set";
            return -1;
        }
        if (rc == 0) {
            if (!set.checksum_only) {
                w->why = set.why;
                return -1;
            }
            /* Its checksum will be rewritten; read it again for the walk
             * with the checksum taken as right. */
            if (w->nsets == CHECK_SETS) {
                w->why = "more sets to rewrite than the walk repairs";
                return -1;
            }
            w->set_dir[w->nsets] = d->first;
            w->set_index[w->nsets] = i;
            w->set_dir_length[w->nsets] = d->length;
            w->set_dir_nofat[w->nsets] = (uint8_t)d->nofat;
            w->nsets++;
            set.checksum_only = 0;
            rc = set_load(em, d, i, &k);
            if (rc != 0) {
                w->why = "an unreadable set";
                return -1;
            }
            k_memcpy(set.file, em->setbuf, 32);
            k_memcpy(set.stream, em->setbuf + 32, 32);
            set.entries = k;
            set.count = set.stream[3];
            for (k = 0; k < set.count; k++) {
                set.units[k] = le16(em->setbuf + (2 + k / 15) * 32 + 2 + (k % 15) * 2);
            }
        }
        /* Two names that up-case alike in one directory are one name. */
        {
            uint16_t hash = le16(set.stream + 4);
            uint32_t m;

            for (m = base; m < base + *nnames; m++) {
                if (check_hashes[m] == hash) {
                    struct ex_set other;
                    uint8_t oraw[32];
                    int same = 0;

                    rc = dir_entry(em, d, check_hash_index[m], oraw);
                    if (rc == 1) {
                        int got = set_read(em, d, check_hash_index[m], oraw, &other);

                        /* A set whose checksum alone fails is about to be
                         * repaired: its name counts against this one. */
                        if ((got == 1 || (got == 0 && other.checksum_only)) &&
                            other.count == set.count) {
                            rc = units_match(em, other.units, set.units, set.count, &same);
                        }
                    }
                    if (rc < 0) {
                        w->why = "an unreadable set";
                        return -1;
                    }
                    if (same) {
                        w->why = "two names that up-case alike in one directory";
                        return -1;
                    }
                }
            }
            if (base + *nnames == CHECK_NAMES) {
                w->why = "a directory larger than the walk compares";
                return -1;
            }
            check_hashes[base + *nnames] = hash;
            check_hash_index[base + *nnames] = i;
            (*nnames)++;
        }
        /* The set's chains: the stream's, and a vendor allocation's. */
        {
            uint32_t first = le32(set.stream + 20);
            uint64_t length = le64(set.stream + 24);
            int nofat = (set.stream[1] & FLAG_NO_FAT_CHAIN) != 0;
            int is_dir = (le16(set.file + 4) & ATTR_DIRECTORY) != 0;

            for (k = 2; k < set.entries; k++) {
                uint8_t ve[32];

                if (dir_entry(em, d, i + k, ve) != 1) {
                    w->why = "an unreadable set";
                    return -1;
                }
                if (ve[0] == 0xE1 && (ve[1] & FLAG_ALLOC_POSSIBLE) && le32(ve + 20) != 0) {
                    if (check_chain(w, le32(ve + 20), (ve[1] & FLAG_NO_FAT_CHAIN) != 0,
                                    le64(ve + 24)) < 0) {
                        return -1;
                    }
                }
            }
            if (is_dir) {
                if (first == 0) {
                    w->why = "a directory with no cluster";
                    return -1;
                }
                if (check_chain(w, first, nofat, length) < 0) {
                    return -1;
                }
                *resume = i + set.entries;
                *sub = first;
                *sub_length = length;
                *sub_nofat = nofat;
                return 1;
            }
            if (first != 0 && check_chain(w, first, nofat, length) < 0) {
                return -1;
            }
        }
        i += set.entries;
    }
}

/* 0 when the volume is consistent or was repaired, 1 when it mounts
 * read-only with the fault named, or a negative error. */
K_NOINLINE static int ex_check_volume(struct ex_mount *em)
{
    struct check_state w;
    uint32_t stack_first[CHECK_DEPTH], stack_resume[CHECK_DEPTH], stack_names[CHECK_DEPTH],
        stack_base[CHECK_DEPTH];
    uint64_t stack_length[CHECK_DEPTH];
    uint8_t stack_nofat[CHECK_DEPTH];
    int depth = 0, i, rc = 0;
    uint32_t c;
    uint64_t allocated = 0;

    k_memset(&w, 0, sizeof(w));
    w.em = em;
    if (fat_walk_acquire(em->clusters) != 0) {
        klog(KLOG_WARN, "exfat", "check: no memory for the walk's %d KiB bitmap; read-only",
             (int)(fat_walk_bytes(em->clusters) / 1024));
        return 1;
    }
    klog(KLOG_INFO, "exfat", "check: the volume was not unmounted cleanly; checking");
    /* The system objects first: the root's chain, the bitmap's, the
     * up-case table's, disjoint and allocated. */
    if (check_chain(&w, em->root, 0, 0) < 0 ||
        check_chain(&w, em->bitmap_first, 0, em->bitmap_length) < 0 ||
        check_chain(&w, em->upcase_first, 0, em->upcase_length) < 0) {
        goto done;
    }
    stack_first[0] = em->root;
    stack_resume[0] = 0;
    stack_length[0] = 0;
    stack_nofat[0] = 0;
    stack_names[0] = 0;
    stack_base[0] = 0;
    depth = 1;
    while (depth > 0) {
        struct ex_node d;
        uint32_t sub = 0;
        uint64_t sub_length = 0;
        int sub_nofat = 0, got;

        k_memset(&d, 0, sizeof(d));
        d.is_dir = 1;
        d.first = stack_first[depth - 1];
        d.length = stack_length[depth - 1];
        d.nofat = stack_nofat[depth - 1];
        got = check_dir(&w, &d, &stack_resume[depth - 1], &sub, &sub_length, &sub_nofat,
                        stack_base[depth - 1], &stack_names[depth - 1]);
        if (got < 0) {
            goto done;
        }
        if (got == 0) {
            depth--;
            continue;
        }
        if (depth == CHECK_DEPTH) {
            w.why = "a directory tree deeper than the walk";
            goto done;
        }
        /* A subdirectory's names take the segment after its parent's, so
         * the parent's stand when the walk returns. */
        stack_first[depth] = sub;
        stack_resume[depth] = 0;
        stack_length[depth] = sub_length;
        stack_nofat[depth] = sub_nofat;
        stack_base[depth] = stack_base[depth - 1] + stack_names[depth - 1];
        stack_names[depth] = 0;
        depth++;
    }
    /* The repairs, now that the whole tree is walked: checksums first,
     * then orphans, then the surplus tails, detached before freed. */
    for (i = 0; i < w.nsets; i++) {
        struct ex_node d;
        uint32_t count;

        k_memset(&d, 0, sizeof(d));
        d.is_dir = 1;
        d.first = w.set_dir[i];
        d.length = w.set_dir_length[i];
        d.nofat = w.set_dir_nofat[i];
        rc = set_load(em, &d, w.set_index[i], &count);
        if (rc == 0) {
            rc = set_store(em, &d, w.set_index[i], count, 1);
        }
        if (rc != 0) {
            w.why = "a repair write failed";
            goto done;
        }
        w.repaired++;
        klog(KLOG_WARN, "exfat", "check: a set's checksum rewritten");
    }
    for (i = 0; i < w.norphans; i++) {
        struct ex_node d;
        uint8_t raw[32];

        k_memset(&d, 0, sizeof(d));
        d.is_dir = 1;
        d.first = w.orphan_dir[i];
        d.length = w.orphan_dir_length[i];
        d.nofat = w.orphan_dir_nofat[i];
        rc = dir_entry(em, &d, w.orphan_index[i], raw);
        if (rc == 1) {
            raw[0] &= 0x7F;
            rc = dir_write(em, &d, w.orphan_index[i], raw, 1);
        }
        if (rc != 1 && rc != 0) {
            w.why = "a repair write failed";
            goto done;
        }
        w.repaired++;
    }
    if (w.norphans) {
        klog(KLOG_WARN, "exfat", "check: %d orphan secondaries freed", w.norphans);
    }
    for (i = 0; i < w.ntails; i++) {
        rc = fat_set(em, w.tail_prev[i], EX_EOC, 1);
        if (rc == 0) {
            rc = free_chain(em, w.tail_first[i], 0, 0);
        }
        if (rc != 0) {
            w.why = "a repair write failed";
            goto done;
        }
        w.repaired++;
        klog(KLOG_WARN, "exfat", "check: surplus tail freed");
    }
    /* The bitmap against the walk: a bit set that nothing reached is lost
     * and left; the tails just freed are marked and free, which is right. */
    for (c = 2; c < em->clusters + 2; c++) {
        uint8_t byte;

        rc = object_read(em, em->bitmap_first, &em->bitmap_walk, (uint64_t)(c - 2) / 8, &byte, 1);
        if (rc != 0) {
            w.why = "an unreadable bitmap";
            goto done;
        }
        if (byte & (1u << ((c - 2) % 8))) {
            allocated++;
            if (!fat_walk_marked(c)) {
                w.lost++;
            }
        }
    }
    em->free_clusters = em->clusters - (uint32_t)allocated;
    em->free_known = 1;
    if (w.lost) {
        klog(KLOG_WARN, "exfat", "check: %d lost cluster(s) left for a host tool", (int)w.lost);
    }
    klog(KLOG_INFO, "exfat", "check: consistent, %d repair(s)", w.repaired);
    rc = 0;
done:
    fat_walk_release();
    if (w.why) {
        klog(KLOG_WARN, "exfat", "check: %s; read-only", w.why);
        return 1;
    }
    return rc;
}

/* The path of a directory node within the mount, from the parents the
 * nodes remember and the names their sets carry, up to the root. */
static int ex_path(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct ex_mount *em = em_of(mnt);
    struct ex_node *n = node;
    uint32_t cluster = n->first;
    char rev[VFS_NAME_MAX * 2 + 2];
    uint32_t len = 0, steps = 0;

    if (cap < 2) {
        return -ENAMETOOLONG;
    }
    buf[0] = '\0';
    while (cluster != em->root && steps++ < 64) {
        struct ex_node *cur = node_find(em, 1, 0, 0, cluster);
        struct ex_node parent;
        struct ex_set set;
        uint32_t cursor = 0, parent_cluster;
        int rc, found = 0;

        if (!cur) {
            rc = find_dir(em, mnt->root, cluster, &cur);
            if (rc != 0) {
                return rc;
            }
        }
        parent_cluster = cur->parent;
        ex_put(mnt, cur);
        k_memset(&parent, 0, sizeof(parent));
        parent.is_dir = 1;
        parent.first = parent_cluster == 0 ? em->root : parent_cluster;
        if (parent_cluster != 0) {
            struct ex_node *pn = node_find(em, 1, 0, 0, parent_cluster);

            if (!pn) {
                rc = find_dir(em, mnt->root, parent_cluster, &pn);
                if (rc != 0) {
                    return rc;
                }
            }
            parent.nofat = pn->nofat;
            parent.length = pn->length;
            ex_put(mnt, pn);
        }
        for (;;) {
            rc = dir_scan(em, &parent, &cursor, &set);
            if (rc <= 0) {
                break;
            }
            if ((le16(set.file + 4) & ATTR_DIRECTORY) && le32(set.stream + 20) == cluster) {
                found = 1;
                break;
            }
        }
        if (rc < 0) {
            return rc;
        }
        if (!found) {
            return -ENOENT;
        }
        rc = fat_units_utf8(set.units, set.count, rev, sizeof(rev));
        if (rc != 0) {
            return rc;
        }
        {
            uint32_t l = k_strlen(rev);
            char tmp[VFS_NAME_MAX * 2 + 2];

            if (len + l + 1 >= cap) {
                return -ENAMETOOLONG;
            }
            k_strncpy(tmp, buf, sizeof(tmp));
            k_memcpy(buf, "/", 1);
            k_memcpy(buf + 1, rev, l);
            k_memcpy(buf + 1 + l, tmp, len);
            len += l + 1;
            buf[len] = '\0';
        }
        cluster = parent_cluster == 0 ? em->root : parent_cluster;
    }
    if (len == 0) {
        buf[0] = '/';
        buf[1] = '\0';
    }
    return 0;
}

static const struct vfs_ops exfat_ops = {
    ex_lookup,  0, /* lookup_rest */
    ex_hold,    ex_put,     ex_getattr,
    ex_setattr, ex_open,    ex_release,
    ex_read,    ex_write,   ex_truncate,
    ex_fsync,   ex_readdir, ex_create,
    ex_unlink,  ex_mkdir,   ex_rmdir,
    ex_rename,  0, /* link */
    0,             /* readlink */
    ex_statfs,  ex_sync,    ex_prepare_unmount,
    0,                         /* abort_unmount */
    ex_unmount, ex_path,    0, /* symlink */
};

const struct vfs_type vfs_exfat_type = {"exfat", ex_mount, &exfat_ops};
