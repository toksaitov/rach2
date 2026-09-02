/*
 * ext2.c: the ext2 file system over the buffer cache.
 *
 * Revision 0 and 1 superblocks, block sizes 1024 to 4096, inode sizes 128
 * and 256, `filetype` as the one incompatible feature known, `sparse_super`
 * and `large_file` as the read-only-compatible ones. An unknown incompatible
 * feature refuses the mount by name; an unknown read-only-compatible one,
 * `dir_index`, and a block size below 4096 mount read-only by name, the
 * last because `bcache_write` writes a whole 4 KiB buffer and two dependent
 * metadata blocks in one buffer could not be written in order. Every field
 * a lookup depends on is checked against the superblock and the group
 * descriptors before it is used: a block number within the volume and
 * outside the structural blocks, an inode number within the count, a
 * directory entry within its block with a `rec_len` of at least its header
 * and a multiple of four, a name within the entry.
 *
 * The kernel's file offset is 32 bits: an inode whose size does not fit
 * answers `EOVERFLOW` to `open` and `stat` and is left as it is on disk.
 * A device node reports its numbers in `stat` and cannot be opened, because
 * devices live in `/dev`. `ext2.Readme.md` has the contract.
 */

#include "ext2.h"

#include "../block/block.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../panic.h"
#include "../proc/process.h"
#include "../rtc.h"
#include "bcache.h"
#include "fatcommon.h"

#define EXT2_SUPER_MAGIC 0xEF53u
#define EXT2_ROOT_INO 2u
#define EXT2_NDIR 12u
#define EXT2_NBLOCKS 15u
#define EXT2_NAME_MAX 255u
#define EXT2_FAST_LINK 60u /* the bytes of `i_block` a fast link lives in */

#define EXT2_COMPAT_DIR_INDEX 0x0020u
#define EXT2_RO_SPARSE_SUPER 0x0001u
#define EXT2_RO_LARGE_FILE 0x0002u
#define EXT2_INCOMPAT_FILETYPE 0x0002u

#define EXT2_VALID_FS 0x0001u
#define EXT2_ERROR_FS 0x0002u

#define S_IFMT_ 0170000u
#define S_IFREG_ 0100000u
#define S_IFDIR_ 0040000u
#define S_IFLNK_ 0120000u
#define S_IFCHR_ 0020000u
#define S_IFBLK_ 0060000u

#define EXT2_FT_DIR 2u
#define EXT2_FT_MAX 7u

#define PATH_DEPTH 64 /* directories climbed by `ext2_path` before ENAMETOOLONG */
#define BCACHE_BLOCK_SHIFT 12 /* log2 of BCACHE_BLOCK_BYTES */

/* One held inode. `too_big` marks a size past the kernel's offset. */
struct ext2_node {
    int used;
    uint32_t refs;
    uint32_t ino;
    uint32_t mode;
    uint32_t uid, gid;
    uint32_t nlink;
    uint64_t size;
    uint32_t blocks;
    uint32_t block[EXT2_NBLOCKS];
    uint32_t atime, mtime, ctime;
    int too_big;
    int unlinked; /* no name left; the inode and its blocks go at the last put */
};

struct ext2_mount {
    int used;
    block_device_t *dev;
    struct vfs_mount *mnt;
    uint32_t block_size;
    uint32_t block_shift; /* log2 of the block size: the kernel has no 64-bit division */
    uint32_t blocks, inodes, first_data, bpg, ipg, groups, inode_size, first_ino;
    uint32_t r_blocks;
    uint32_t compat, incompat, ro_compat;
    uint32_t state;
    int writable;
    uint64_t free_blocks, free_inodes;
    int valid_cleared; /* the superblock's valid bit was cleared by this mount */
    uint32_t alloc_hint; /* the block after the last one given out */
    uint8_t gd[EXT2_GROUPS * 32];
    struct ext2_node nodes[EXT2_NODES];
};

static struct ext2_mount ext2_mounts[EXT2_MOUNTS];

/* ---- little-endian fields ---- */

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static struct ext2_mount *em_of(struct vfs_mount *mnt)
{
    return mnt->priv;
}

/* ---- bytes off the volume, through the cache ---- */

/**
 * Copy `len` bytes at byte offset `off` of the volume into `dst`, one cache
 * block at a time. The volume is bounded to 16 GiB by the constructor.
 */
static int read_bytes(struct ext2_mount *em, uint64_t off, void *dst, uint32_t len)
{
    uint8_t *out = dst;

    while (len > 0) {
        uint64_t blk = off >> BCACHE_BLOCK_SHIFT;
        uint32_t in = (uint32_t)off & (BCACHE_BLOCK_BYTES - 1);
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

/* ---- the structural blocks ---- */

/* Whether group `g` keeps a copy of the superblock and the descriptors: with
 * `sparse_super`, groups 0 and 1 and the powers of 3, 5 and 7; without it,
 * every group. */
static int has_super(const struct ext2_mount *em, uint32_t g)
{
    uint32_t p = g;

    if (!(em->ro_compat & EXT2_RO_SPARSE_SUPER) || g <= 1) {
        return 1;
    }
    while (p % 3 == 0) {
        p /= 3;
    }
    while (p % 5 == 0) {
        p /= 5;
    }
    while (p % 7 == 0) {
        p /= 7;
    }
    return p == 1;
}

/* The blocks of the descriptor table, after each superblock copy. */
static uint32_t gd_blocks(const struct ext2_mount *em)
{
    return (em->groups * 32 + em->block_size - 1) / em->block_size;
}

static uint32_t inode_table_blocks(const struct ext2_mount *em)
{
    return (em->ipg * em->inode_size + em->block_size - 1) / em->block_size;
}

/**
 * Whether block `b` may hold data: inside the volume, past the boot block,
 * and not a superblock, a descriptor table, a bitmap or an inode table.
 * The constructor requires each group's tables to lie in that group, so
 * the group of `b` is the one whose descriptor is consulted.
 */
static int data_block_ok(const struct ext2_mount *em, uint32_t b)
{
    uint32_t g, in;
    const uint8_t *gd;

    if (b < em->first_data || b >= em->blocks) {
        return 0;
    }
    if (b == 0) {
        return 0; /* the boot block, or the superblock at 4 KiB blocks */
    }
    g = (b - em->first_data) / em->bpg;
    in = (b - em->first_data) % em->bpg;
    if (has_super(em, g) && in <= gd_blocks(em)) {
        return 0;
    }
    gd = em->gd + g * 32;
    if (b == le32(gd + 0) || b == le32(gd + 4)) {
        return 0;
    }
    if (b >= le32(gd + 8) && b < le32(gd + 8) + inode_table_blocks(em)) {
        return 0;
    }
    return 1;
}

/* ---- inodes ---- */

/**
 * Load inode `ino` into `n`: its number checked against the count, its
 * table found through its group's descriptor.
 */
static int inode_load(struct ext2_mount *em, uint32_t ino, struct ext2_node *n)
{
    uint8_t raw[128];
    uint32_t g, index, table;
    uint64_t at;
    int i, rc;

    if (ino == 0 || ino > em->inodes) {
        klog(KLOG_WARN, "ext2", "inode %d is past the count %d", (int)ino, (int)em->inodes);
        return -EIO;
    }
    g = (ino - 1) / em->ipg;
    index = (ino - 1) % em->ipg;
    table = le32(em->gd + g * 32 + 8);
    at = (uint64_t)table * em->block_size + (uint64_t)index * em->inode_size;
    rc = read_bytes(em, at, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }
    n->ino = ino;
    n->mode = le16(raw + 0);
    n->uid = le16(raw + 2) | (le16(raw + 120) << 16);
    n->gid = le16(raw + 24) | (le16(raw + 122) << 16);
    n->size = le32(raw + 4);
    n->atime = le32(raw + 8);
    n->ctime = le32(raw + 12);
    n->mtime = le32(raw + 16);
    n->nlink = le16(raw + 26);
    n->blocks = le32(raw + 28);
    for (i = 0; i < (int)EXT2_NBLOCKS; i++) {
        n->block[i] = le32(raw + 40 + i * 4);
    }
    n->too_big = 0;
    if ((n->mode & S_IFMT_) == S_IFREG_) {
        uint32_t high = le32(raw + 108);

        n->size |= (uint64_t)high << 32;
        if (high != 0) {
            n->too_big = 1;
        }
    }
    return 0;
}

/* ---- the node pool ---- */

static struct ext2_node *node_find(struct ext2_mount *em, uint32_t ino)
{
    int i;

    for (i = 0; i < EXT2_NODES; i++) {
        if (em->nodes[i].used && em->nodes[i].ino == ino) {
            em->nodes[i].refs++;
            return &em->nodes[i];
        }
    }
    return 0;
}

/**
 * The node for inode `ino`, held once more if it is in the pool and loaded
 * into a free entry otherwise; NULL with `*rc` set when the pool is full or
 * the inode cannot be read.
 */
static struct ext2_node *node_for(struct ext2_mount *em, uint32_t ino, int *rc)
{
    struct ext2_node *n = node_find(em, ino);
    int i;

    *rc = 0;
    if (n) {
        return n;
    }
    for (i = 0; i < EXT2_NODES; i++) {
        if (!em->nodes[i].used) {
            n = &em->nodes[i];
            break;
        }
    }
    if (!n) {
        klog(KLOG_WARN, "ext2", "node pool full");
        *rc = -ENOMEM;
        return 0;
    }
    k_memset(n, 0, sizeof(*n));
    *rc = inode_load(em, ino, n);
    if (*rc != 0) {
        return 0;
    }
    n->used = 1;
    n->refs = 1;
    return n;
}

static void ext2_hold(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    ((struct ext2_node *)node)->refs++;
}

static int release_inode(struct ext2_mount *em, struct ext2_node *n);
static int sb_store(struct ext2_mount *em);

static void ext2_put(struct vfs_mount *mnt, void *node)
{
    struct ext2_node *n = node;

    if (n->refs > 0) {
        n->refs--;
    }
    if (n->refs == 0) {
        /* An unlinked inode keeps its blocks until its last holder goes,
         * which is what makes an open unlinked file work; a crash before
         * this leaves an orphan for e2fsck. */
        if (n->unlinked && em_of(mnt)->writable) {
            vfs_ns_lock(mnt);
            release_inode(em_of(mnt), n);
            vfs_ns_unlock(mnt);
        }
        n->used = 0;
    }
}

/* ---- block mapping ---- */

/**
 * Block `index` of `n`'s data: 0 for a hole, or the pointer through the
 * direct, indirect, double and triple levels, each pointer block checked
 * before it is read.
 */
static int block_at(struct ext2_mount *em, const struct ext2_node *n, uint32_t index,
                    uint32_t *out)
{
    uint32_t per = em->block_size / 4;
    uint32_t b;
    int level;

    if (index < EXT2_NDIR) {
        b = n->block[index];
    } else {
        uint32_t span = per;
        uint32_t rest;

        index -= EXT2_NDIR;
        for (level = 1; level <= 3; level++) {
            if (index < span) {
                break;
            }
            index -= span;
            span *= per;
        }
        if (level > 3) {
            return -EFBIG;
        }
        b = n->block[EXT2_NDIR + level - 1];
        rest = index;
        while (level > 0 && b != 0) {
            uint32_t div = 1;
            uint8_t ptr[4];
            int i, rc;

            for (i = 1; i < level; i++) {
                div *= per;
            }
            if (!data_block_ok(em, b)) {
                klog(KLOG_WARN, "ext2", "inode %d: pointer block %d is out of range", (int)n->ino,
                     (int)b);
                return -EIO;
            }
            rc = read_bytes(em, (uint64_t)b * em->block_size + (uint64_t)(rest / div) * 4, ptr, 4);
            if (rc != 0) {
                return rc;
            }
            b = le32(ptr);
            rest %= div;
            level--;
        }
    }
    if (b != 0 && !data_block_ok(em, b)) {
        klog(KLOG_WARN, "ext2", "inode %d: block %d is out of range", (int)n->ino, (int)b);
        return -EIO;
    }
    *out = b;
    return 0;
}

/* Copy `len` bytes of `n`'s data at `off`, a hole read as zeros. */
static int read_data(struct ext2_mount *em, const struct ext2_node *n, uint64_t off, void *dst,
                     uint32_t len)
{
    uint8_t *out = dst;

    while (len > 0) {
        uint32_t index = (uint32_t)(off >> em->block_shift);
        uint32_t in = (uint32_t)off & (em->block_size - 1);
        uint32_t take = em->block_size - in;
        uint32_t b;
        int rc;

        if (take > len) {
            take = len;
        }
        rc = block_at(em, n, index, &b);
        if (rc != 0) {
            return rc;
        }
        if (b == 0) {
            k_memset(out, 0, take);
        } else {
            rc = read_bytes(em, (uint64_t)b * em->block_size + in, out, take);
            if (rc != 0) {
                return rc;
            }
        }
        out += take;
        off += take;
        len -= take;
    }
    return 0;
}

/* ---- directories ---- */

/* One entry read at a byte position of a directory. */
struct ext2_scan {
    uint32_t ino;
    uint32_t rec_len;
    uint32_t name_len;
    uint32_t type;
    char name[EXT2_NAME_MAX + 1];
};

/**
 * The entry at `*cursor` in directory `d`, the cursor moved past it: 1 with
 * `sc` filled, 0 at the directory's end, or a negative errno for an entry
 * that does not fit its block. An entry with inode 0 is returned with that
 * inode, and the caller skips it.
 */
static int dir_scan(struct ext2_mount *em, const struct ext2_node *d, uint32_t *cursor,
                    struct ext2_scan *sc)
{
    uint8_t head[8];
    uint32_t at = *cursor;
    uint32_t in;
    int rc;

    if (at >= d->size) {
        return 0;
    }
    in = at % em->block_size;
    rc = read_data(em, d, at, head, 8);
    if (rc != 0) {
        return rc;
    }
    sc->ino = le32(head);
    sc->rec_len = le16(head + 4);
    sc->name_len = head[6];
    sc->type = head[7];
    if (sc->rec_len < 8 || sc->rec_len % 4 != 0 || in + sc->rec_len > em->block_size ||
        (uint64_t)at + sc->rec_len > d->size || sc->name_len + 8 > sc->rec_len ||
        sc->ino > em->inodes) {
        klog(KLOG_WARN, "ext2", "directory %d: entry at %d is malformed", (int)d->ino, (int)at);
        return -EIO;
    }
    if (sc->ino != 0) {
        rc = read_data(em, d, at + 8, sc->name, sc->name_len);
        if (rc != 0) {
            return rc;
        }
    } else {
        sc->name_len = 0;
    }
    sc->name[sc->name_len] = '\0';
    *cursor = at + sc->rec_len;
    return 1;
}

static int names_equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int ext2_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    uint32_t cursor = 0;

    if ((d->mode & S_IFMT_) != S_IFDIR_) {
        return -ENOTDIR;
    }
    if (name[0] == '.' && name[1] == '\0') {
        d->refs++;
        *node = d;
        return 0;
    }
    for (;;) {
        struct ext2_scan sc;
        int got = dir_scan(em, d, &cursor, &sc);

        if (got < 0) {
            return got;
        }
        if (got == 0) {
            return -ENOENT;
        }
        if (sc.ino != 0 && names_equal(sc.name, name)) {
            struct ext2_node *n;
            int rc;

            if (sc.ino == d->ino) {
                d->refs++;
                *node = d;
                return 0;
            }
            n = node_for(em, sc.ino, &rc);
            if (!n) {
                return rc;
            }
            *node = n;
            return 0;
        }
    }
}

/* ---- attributes ---- */

/* A device's numbers: the old encoding in the first pointer, the new one in
 * the second when the first is zero, packed as the kernel's `rdev`. */
static uint32_t device_rdev(const struct ext2_node *n)
{
    uint32_t major, minor;

    if (n->block[0] != 0) {
        major = (n->block[0] >> 8) & 0xFFu;
        minor = n->block[0] & 0xFFu;
    } else {
        uint32_t v = n->block[1];

        major = (v >> 8) & 0xFFFu;
        minor = (v & 0xFFu) | ((v >> 12) & 0xFFF00u);
    }
    return (major << 8) | (minor & 0xFFu) | ((minor & 0xFFF00u) << 12);
}

static int ext2_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    struct ext2_node *n = node;

    (void)mnt;
    if (n->too_big) {
        return -EOVERFLOW;
    }
    k_memset(st, 0, sizeof(*st));
    st->mode = n->mode;
    st->uid = n->uid;
    st->gid = n->gid;
    st->nlink = n->nlink;
    st->size = n->size;
    st->ino = n->ino;
    if ((n->mode & S_IFMT_) == S_IFCHR_ || (n->mode & S_IFMT_) == S_IFBLK_) {
        st->rdev = device_rdev(n);
    }
    st->atime_sec = n->atime;
    st->mtime_sec = n->mtime;
    st->ctime_sec = n->ctime;
    return 0;
}

static int ext2_open(struct vfs_mount *mnt, void *node, int flags, void **handle)
{
    struct ext2_node *n = node;
    uint32_t type = n->mode & S_IFMT_;

    (void)mnt;
    (void)flags;
    if (n->too_big) {
        return -EOVERFLOW;
    }
    if (type == S_IFCHR_ || type == S_IFBLK_) {
        return -EACCES;
    }
    n->refs++;
    *handle = node;
    return 0;
}

static void ext2_release(struct vfs_mount *mnt, void *handle)
{
    ext2_put(mnt, handle);
}

static int32_t ext2_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count,
                         uint64_t offset)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *n = handle;
    int rc;

    if ((n->mode & S_IFMT_) == S_IFDIR_) {
        return -EISDIR;
    }
    if (offset >= n->size) {
        return 0;
    }
    if (count > n->size - offset) {
        count = (uint32_t)(n->size - offset);
    }
    rc = read_data(em, n, offset, buf, count);
    if (rc != 0) {
        return rc;
    }
    return (int32_t)count;
}

static int ext2_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor,
                        struct vfs_dirent *ent)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *d = handle;

    if ((d->mode & S_IFMT_) != S_IFDIR_) {
        return -ENOTDIR;
    }
    for (;;) {
        struct ext2_scan sc;
        int got = dir_scan(em, d, cursor, &sc);

        if (got <= 0) {
            return got;
        }
        if (sc.ino == 0) {
            continue;
        }
        k_strncpy(ent->name, sc.name, sizeof(ent->name));
        ent->name[sizeof(ent->name) - 1] = '\0';
        ent->ino = sc.ino;
        if (em->incompat & EXT2_INCOMPAT_FILETYPE) {
            ent->is_dir = sc.type == EXT2_FT_DIR;
        } else {
            struct ext2_node probe;
            int rc = inode_load(em, sc.ino, &probe);

            if (rc != 0) {
                return rc;
            }
            ent->is_dir = (probe.mode & S_IFMT_) == S_IFDIR_;
        }
        return 1;
    }
}

/**
 * A symbolic link's target: in the inode's pointer words when it is
 * shorter than sixty bytes, in its first block otherwise. Returns the
 * length, or -ENAMETOOLONG when `cap` cannot hold it.
 */
static int ext2_readlink(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *n = node;
    uint32_t len;

    if ((n->mode & S_IFMT_) != S_IFLNK_) {
        return -EINVAL;
    }
    if (n->size >= em->block_size) {
        return -EIO;
    }
    len = (uint32_t)n->size;
    if (len + 1 > cap) {
        return -ENAMETOOLONG;
    }
    if (len < EXT2_FAST_LINK) {
        uint32_t i;

        for (i = 0; i < len; i++) {
            buf[i] = (char)((n->block[i / 4] >> ((i % 4) * 8)) & 0xFFu);
        }
    } else {
        int rc;

        if (n->block[0] == 0) {
            return -EIO;
        }
        rc = read_data(em, n, 0, buf, len);
        if (rc != 0) {
            return rc;
        }
    }
    buf[len] = '\0';
    return (int)len;
}

static int ext2_statfs(struct vfs_mount *mnt, struct vfs_statfs *st)
{
    struct ext2_mount *em = em_of(mnt);

    st->magic = EXT2_SUPER_MAGIC;
    st->bsize = em->block_size;
    st->blocks = em->blocks;
    st->bfree = em->free_blocks;
    st->namelen = EXT2_NAME_MAX;
    return 0;
}

/* The superblock's free counts are summaries the descriptors hold exactly,
 * written at sync and at unmount. */
static int ext2_sync(struct vfs_mount *mnt)
{
    struct ext2_mount *em = em_of(mnt);
    int rc;

    if (!em->writable || (mnt->flags & VFS_MS_RDONLY)) {
        return 0;
    }
    vfs_ns_lock(mnt);
    rc = bcache_sync(em->dev);
    if (rc == 0) {
        rc = sb_store(em);
    }
    vfs_ns_unlock(mnt);
    return rc;
}

/* The valid bit set again, once everything has reached the disk and only
 * when the error bit is clear; a mount that went read-only after a failed
 * write leaves the volume as it is, for the next mount's check. */
static int ext2_prepare_unmount(struct vfs_mount *mnt)
{
    struct ext2_mount *em = em_of(mnt);
    int rc;

    if (!em->writable || (mnt->flags & VFS_MS_RDONLY)) {
        return 0;
    }
    rc = bcache_sync(em->dev);
    if (rc != 0) {
        return rc;
    }
    if (em->valid_cleared && !(em->state & EXT2_ERROR_FS)) {
        em->state |= EXT2_VALID_FS;
    }
    rc = sb_store(em);
    if (rc != 0) {
        em->state &= ~EXT2_VALID_FS;
        return rc;
    }
    em->valid_cleared = 0;
    return 0;
}

static void ext2_unmount(struct vfs_mount *mnt)
{
    struct ext2_mount *em = em_of(mnt);

    k_memset(em, 0, sizeof(*em));
}

/**
 * The path of a directory node: the parent by its `..` entry, the name by
 * the parent's entry that names the child's inode, up to the root.
 */
static int ext2_path(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *n = node;
    struct ext2_node cur;
    uint32_t len = 0;
    int depth;

    if ((n->mode & S_IFMT_) != S_IFDIR_) {
        return -ENOTDIR;
    }
    buf[0] = '\0';
    cur = *n;
    for (depth = 0; cur.ino != EXT2_ROOT_INO && depth < PATH_DEPTH; depth++) {
        struct ext2_node parent;
        struct ext2_scan sc;
        uint32_t cursor = 0;
        uint32_t child = cur.ino;
        int found = 0, rc;

        for (;;) {
            int got = dir_scan(em, &cur, &cursor, &sc);

            if (got < 0) {
                return got;
            }
            if (got == 0) {
                return -ENOENT;
            }
            if (sc.ino != 0 && names_equal(sc.name, "..")) {
                break;
            }
        }
        rc = inode_load(em, sc.ino, &parent);
        if (rc != 0) {
            return rc;
        }
        cursor = 0;
        for (;;) {
            int got = dir_scan(em, &parent, &cursor, &sc);

            if (got < 0) {
                return got;
            }
            if (got == 0) {
                break;
            }
            if (sc.ino == child && !names_equal(sc.name, ".") && !names_equal(sc.name, "..")) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return -ENOENT;
        }
        if (len + sc.name_len + 1 >= cap) {
            return -ENAMETOOLONG;
        }
        k_memmove(buf + sc.name_len + 1, buf, len + 1);
        buf[0] = '/';
        k_memcpy(buf + 1, sc.name, sc.name_len);
        len += sc.name_len + 1;
        cur = parent;
    }
    if (depth == PATH_DEPTH) {
        return -ENAMETOOLONG;
    }
    if (len == 0) {
        buf[0] = '/';
        buf[1] = '\0';
    }
    return 0;
}


/* ---- the writer ---- */

/*
 * Every metadata write goes through `bcache_write` in the order below, and
 * a file's bytes go through `bcache_dirty` and are synced before a size
 * that covers them is written. The rules, from `ext2.Readme.md`: an inode
 * is freed by writing it zeroed with its deletion time before its bitmap
 * bit clears, and allocated by setting its bit before it is written and
 * named; a block is allocated in its bitmap before a pointer names it, a
 * pointer block is written before the pointer to it, and `i_blocks` goes
 * with the pointers and `i_size` last; an entry is removed before the link
 * count drops; a truncation shrinks the size first, then clears pointers
 * from the end, then frees blocks; a directory grows by a zeroed block
 * chained before the size covers it.
 */

/* The value is taken once: a count read from the bytes being written, as
 * `le16(gd + 12) - 1`, would otherwise be read again after its low byte
 * changed. */
#define LE16_PUT(p, v)                                                                             \
    do {                                                                                           \
        uint32_t put_ = (v);                                                                       \
        (p)[0] = (uint8_t)(put_ & 0xFFu);                                                          \
        (p)[1] = (uint8_t)((put_ >> 8) & 0xFFu);                                                   \
    } while (0)
#define LE32_PUT(p, v)                                                                             \
    do {                                                                                           \
        uint32_t put_ = (v);                                                                       \
        (p)[0] = (uint8_t)(put_ & 0xFFu);                                                          \
        (p)[1] = (uint8_t)((put_ >> 8) & 0xFFu);                                                   \
        (p)[2] = (uint8_t)((put_ >> 16) & 0xFFu);                                                  \
        (p)[3] = (uint8_t)((put_ >> 24) & 0xFFu);                                                  \
    } while (0)

static uint32_t now_seconds(void)
{
    int64_t sec = 0, nsec = 0;

    rtc_realtime(&sec, &nsec);
    if (sec < 0 || sec > 0x7FFFFFFF) {
        return 0;
    }
    return (uint32_t)sec;
}

/* Copy `len` bytes at byte offset `off` of the volume from `src`, or zeros
 * with `src` NULL; through to the device when `sync`, else written back. */
static int write_bytes(struct ext2_mount *em, uint64_t off, const void *src, uint32_t len,
                       int sync)
{
    const uint8_t *in = src;

    while (len > 0) {
        uint64_t blk = off >> BCACHE_BLOCK_SHIFT;
        uint32_t at = (uint32_t)off & (BCACHE_BLOCK_BYTES - 1);
        uint32_t take = BCACHE_BLOCK_BYTES - at;
        struct bbuf *b;
        int rc = 0;

        if (take > len) {
            take = len;
        }
        b = bcache_get(em->dev, blk);
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

static int assert_writable(struct vfs_mount *mnt, struct ext2_mount *em)
{
    if (!em->writable || (mnt->flags & VFS_MS_RDONLY)) {
        return -EROFS;
    }
    return 0;
}

static void assert_locked(struct vfs_mount *mnt)
{
    int me = current_proc ? (int)current_proc->pid : 0;

    if (mnt->ns_holder != me || mnt->ns_depth == 0) {
        k_panic("ext2: a mutation outside the mount's lock");
    }
}

static uint64_t inode_offset(const struct ext2_mount *em, uint32_t ino)
{
    uint32_t g = (ino - 1) / em->ipg;
    uint32_t index = (ino - 1) % em->ipg;
    uint32_t table = le32(em->gd + g * 32 + 8);

    return (uint64_t)table * em->block_size + (uint64_t)index * em->inode_size;
}

/**
 * Write inode `n` through: the fields this driver keeps, into the 128
 * bytes read back first so that what it does not keep stays as it was.
 */
static int inode_store(struct ext2_mount *em, const struct ext2_node *n)
{
    uint8_t raw[128];
    uint64_t at = inode_offset(em, n->ino);
    int i, rc;

    rc = read_bytes(em, at, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }
    LE16_PUT(raw + 0, n->mode);
    LE16_PUT(raw + 2, n->uid & 0xFFFFu);
    LE32_PUT(raw + 4, (uint32_t)n->size);
    LE32_PUT(raw + 8, n->atime);
    LE32_PUT(raw + 12, n->ctime);
    LE32_PUT(raw + 16, n->mtime);
    LE32_PUT(raw + 20, 0); /* i_dtime: a live inode has none, whatever it had */
    LE16_PUT(raw + 24, n->gid & 0xFFFFu);
    LE16_PUT(raw + 26, n->nlink);
    LE32_PUT(raw + 28, n->blocks);
    for (i = 0; i < (int)EXT2_NBLOCKS; i++) {
        LE32_PUT(raw + 40 + i * 4, n->block[i]);
    }
    LE16_PUT(raw + 120, (n->uid >> 16) & 0xFFFFu);
    LE16_PUT(raw + 122, (n->gid >> 16) & 0xFFFFu);
    return write_bytes(em, at, raw, sizeof(raw), 1);
}

/* An inode written zeroed with its deletion time: the first step of a free. */
static int inode_store_freed(struct ext2_mount *em, uint32_t ino)
{
    uint8_t raw[128];

    k_memset(raw, 0, sizeof(raw));
    LE32_PUT(raw + 20, now_seconds()); /* i_dtime */
    return write_bytes(em, inode_offset(em, ino), raw, sizeof(raw), 1);
}

/* The primary descriptor of group `g`, written through. */
static int gd_store(struct ext2_mount *em, uint32_t g)
{
    uint64_t at = (uint64_t)(em->first_data + 1) * em->block_size + (uint64_t)g * 32;

    return write_bytes(em, at, em->gd + g * 32, 32, 1);
}

/* The superblock's free counts and its state, written through. */
static int sb_store(struct ext2_mount *em)
{
    uint8_t raw[8];
    int rc;

    LE32_PUT(raw, (uint32_t)em->free_blocks);
    LE32_PUT(raw + 4, (uint32_t)em->free_inodes);
    rc = write_bytes(em, 1024 + 12, raw, 8, 1);
    if (rc != 0) {
        return rc;
    }
    LE16_PUT(raw, em->state);
    return write_bytes(em, 1024 + 58, raw, 2, 1);
}

/* One bit of a bitmap block, read or written through. */
static int bit_get(struct ext2_mount *em, uint32_t bitmap_block, uint32_t index, int *out)
{
    uint8_t byte;
    int rc = read_bytes(em, (uint64_t)bitmap_block * em->block_size + index / 8, &byte, 1);

    if (rc != 0) {
        return rc;
    }
    *out = (byte >> (index % 8)) & 1;
    return 0;
}

static int bit_put(struct ext2_mount *em, uint32_t bitmap_block, uint32_t index, int value)
{
    uint64_t at = (uint64_t)bitmap_block * em->block_size + index / 8;
    uint8_t byte;
    int rc = read_bytes(em, at, &byte, 1);

    if (rc != 0) {
        return rc;
    }
    if (value) {
        byte = (uint8_t)(byte | (1u << (index % 8)));
    } else {
        byte = (uint8_t)(byte & ~(1u << (index % 8)));
    }
    return write_bytes(em, at, &byte, 1, 1);
}

/* Whether the caller may take from the reserved blocks. */
static int may_use_reserved(void)
{
    return current_proc && current_proc->euid == 0;
}

/**
 * Allocate one block, first-fit from `hint` and wrapping once through every
 * group: the bit is set and the group's count lowered before the block is
 * returned, so that no pointer ever names a free block. ENOSPC when none is
 * free, or when only the root's reserve is and the caller is not root.
 */
static int block_alloc(struct ext2_mount *em, uint32_t hint, uint32_t *out)
{
    uint32_t g0, g, tried;
    uint32_t start_in;

    if (em->free_blocks == 0 ||
        (!may_use_reserved() && em->free_blocks <= em->r_blocks)) {
        return -ENOSPC;
    }
    if (hint < em->first_data || hint >= em->blocks) {
        hint = em->first_data;
    }
    g0 = (hint - em->first_data) / em->bpg;
    start_in = (hint - em->first_data) % em->bpg;
    for (tried = 0; tried < em->groups; tried++) {
        uint32_t i, count, base, bitmap;
        uint8_t *gd;

        g = (g0 + tried) % em->groups;
        gd = em->gd + g * 32;
        base = em->first_data + g * em->bpg;
        count = em->blocks - base < em->bpg ? em->blocks - base : em->bpg;
        bitmap = le32(gd + 0);
        if (le16(gd + 12) == 0) {
            start_in = 0;
            continue;
        }
        for (i = tried == 0 ? start_in : 0; i < count; i++) {
            int used, rc;

            rc = bit_get(em, bitmap, i, &used);
            if (rc != 0) {
                return rc;
            }
            if (used) {
                continue;
            }
            rc = bit_put(em, bitmap, i, 1);
            if (rc != 0) {
                return rc;
            }
            LE16_PUT(gd + 12, le16(gd + 12) - 1);
            rc = gd_store(em, g);
            if (rc != 0) {
                return rc;
            }
            em->free_blocks--;
            em->alloc_hint = base + i + 1;
            *out = base + i;
            return 0;
        }
        start_in = 0;
    }
    return -ENOSPC;
}

static int block_free(struct ext2_mount *em, uint32_t b)
{
    uint32_t g, i;
    uint8_t *gd;
    int rc;

    if (!data_block_ok(em, b)) {
        klog(KLOG_WARN, "ext2", "free of block %d, which is out of range", (int)b);
        return -EIO;
    }
    g = (b - em->first_data) / em->bpg;
    i = (b - em->first_data) % em->bpg;
    gd = em->gd + g * 32;
    rc = bit_put(em, le32(gd + 0), i, 0);
    if (rc != 0) {
        return rc;
    }
    LE16_PUT(gd + 12, le16(gd + 12) + 1);
    rc = gd_store(em, g);
    if (rc != 0) {
        return rc;
    }
    em->free_blocks++;
    return 0;
}

/**
 * Allocate an inode in `parent`'s group when it has one free, else in the
 * first group that has; the bit is set and the counts lowered before the
 * number is returned, and the caller writes the inode initialised and
 * names it, in that order.
 */
static int inode_alloc(struct ext2_mount *em, uint32_t parent_ino, int is_dir, uint32_t *out)
{
    uint32_t g0 = (parent_ino - 1) / em->ipg;
    uint32_t tried;

    if (em->free_inodes == 0) {
        return -ENOSPC;
    }
    for (tried = 0; tried < em->groups; tried++) {
        uint32_t g = (g0 + tried) % em->groups;
        uint8_t *gd = em->gd + g * 32;
        uint32_t i;

        if (le16(gd + 14) == 0) {
            continue;
        }
        for (i = 0; i < em->ipg; i++) {
            uint32_t ino = g * em->ipg + i + 1;
            int used, rc;

            if (ino < em->first_ino || ino > em->inodes) {
                continue;
            }
            rc = bit_get(em, le32(gd + 4), i, &used);
            if (rc != 0) {
                return rc;
            }
            if (used) {
                continue;
            }
            rc = bit_put(em, le32(gd + 4), i, 1);
            if (rc != 0) {
                return rc;
            }
            LE16_PUT(gd + 14, le16(gd + 14) - 1);
            if (is_dir) {
                LE16_PUT(gd + 16, le16(gd + 16) + 1);
            }
            rc = gd_store(em, g);
            if (rc != 0) {
                return rc;
            }
            em->free_inodes--;
            *out = ino;
            return 0;
        }
    }
    return -ENOSPC;
}

/* The bit and the counts of inode `ino` cleared, after the inode itself
 * was written freed. */
static int inode_bit_free(struct ext2_mount *em, uint32_t ino, int was_dir)
{
    uint32_t g = (ino - 1) / em->ipg;
    uint32_t i = (ino - 1) % em->ipg;
    uint8_t *gd = em->gd + g * 32;
    int rc = bit_put(em, le32(gd + 4), i, 0);

    if (rc != 0) {
        return rc;
    }
    LE16_PUT(gd + 14, le16(gd + 14) + 1);
    if (was_dir && le16(gd + 16) > 0) {
        LE16_PUT(gd + 16, le16(gd + 16) - 1);
    }
    rc = gd_store(em, g);
    if (rc != 0) {
        return rc;
    }
    em->free_inodes++;
    return 0;
}

/* ---- the block map, written ---- */

/**
 * Free the blocks `keep` and after of `n`, from the end: each pointer is
 * cleared, written through, and its block freed, and a pointer block whose
 * children are all gone goes the same way. `n->blocks` is kept current;
 * the caller stores the inode.
 */
static int punch_level(struct ext2_mount *em, struct ext2_node *n, uint32_t *slot,
                       uint64_t slot_at, int level, uint32_t base, uint32_t keep,
                       int *emptied)
{
    uint32_t b = *slot;
    uint32_t per = em->block_size / 4;
    uint32_t span = 1;
    uint32_t sectors = em->block_size / 512;
    int i, rc;

    *emptied = 0;
    if (b == 0) {
        *emptied = 1;
        return 0;
    }
    for (i = 0; i < level; i++) {
        span *= per;
    }
    if (level > 0) {
        int all_gone = 1;
        uint32_t k;

        if (!data_block_ok(em, b)) {
            return -EIO;
        }
        for (k = per; k > 0; k--) {
            uint32_t child_base = base + (k - 1) * (span / per);
            uint8_t ptr[4];
            uint32_t child;
            uint64_t at = (uint64_t)b * em->block_size + (uint64_t)(k - 1) * 4;
            int gone;

            if (child_base + span / per <= keep) {
                all_gone = 0;
                break; /* everything below this slot stays */
            }
            rc = read_bytes(em, at, ptr, 4);
            if (rc != 0) {
                return rc;
            }
            child = le32(ptr);
            if (child == 0) {
                continue;
            }
            rc = punch_level(em, n, &child, at, level - 1, child_base, keep, &gone);
            if (rc != 0) {
                return rc;
            }
            if (!gone) {
                all_gone = 0;
                break;
            }
        }
        if (!all_gone) {
            return 0;
        }
    }
    /* This block goes: the pointer to it cleared and written first. */
    *slot = 0;
    if (slot_at != 0) {
        uint8_t zero[4] = {0, 0, 0, 0};

        rc = write_bytes(em, slot_at, zero, 4, 1);
        if (rc != 0) {
            return rc;
        }
    } else {
        rc = inode_store(em, n);
        if (rc != 0) {
            return rc;
        }
    }
    rc = block_free(em, b);
    if (rc != 0) {
        return rc;
    }
    if (n->blocks >= sectors) {
        n->blocks -= sectors;
    }
    *emptied = 1;
    return 0;
}

static int punch(struct ext2_mount *em, struct ext2_node *n, uint32_t keep)
{
    uint32_t per = em->block_size / 4;
    uint32_t base = EXT2_NDIR;
    uint32_t span = per;
    int level, i, rc, gone;

    for (level = 3; level >= 1; level--) {
        uint32_t lbase = EXT2_NDIR, lspan = per;
        int l;

        for (l = 1; l < level; l++) {
            lbase += lspan;
            lspan *= per;
        }
        if (lbase + lspan > keep && n->block[EXT2_NDIR + level - 1] != 0) {
            rc = punch_level(em, n, &n->block[EXT2_NDIR + level - 1], 0, level, lbase, keep,
                             &gone);
            if (rc != 0) {
                return rc;
            }
        }
    }
    (void)base;
    (void)span;
    for (i = (int)EXT2_NDIR - 1; i >= (int)keep && i >= 0; i--) {
        if (n->block[i] != 0) {
            rc = punch_level(em, n, &n->block[i], 0, 0, (uint32_t)i, keep, &gone);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return 0;
}

/**
 * The block for data index `index`, allocated when missing: a missing
 * pointer block is allocated, zeroed and written before the pointer to it
 * is, and a data block's pointer is written after the block is in the
 * bitmap. `n->blocks` and the inode's own pointers change in memory; the
 * caller stores the inode with them.
 */
static int map_alloc(struct ext2_mount *em, struct ext2_node *n, uint32_t index, uint32_t *out,
                     int *fresh)
{
    uint32_t per = em->block_size / 4;
    uint32_t sectors = em->block_size / 512;
    uint32_t *slot;
    uint64_t slot_at = 0; /* 0: the slot is in the inode */
    uint32_t hint = em->alloc_hint;
    int level, rc;

    *fresh = 0;
    if (index < EXT2_NDIR) {
        slot = &n->block[index];
    } else {
        uint32_t span = per;
        uint32_t rest;

        index -= EXT2_NDIR;
        for (level = 1; level <= 3; level++) {
            if (index < span) {
                break;
            }
            index -= span;
            span *= per;
        }
        if (level > 3) {
            return -EFBIG;
        }
        slot = &n->block[EXT2_NDIR + level - 1];
        rest = index;
        while (level > 0) {
            uint32_t div = 1, b;
            uint8_t ptr[4];
            int i;

            for (i = 1; i < level; i++) {
                div *= per;
            }
            if (*slot == 0) {
                rc = block_alloc(em, hint, &b);
                if (rc != 0) {
                    return rc;
                }
                rc = write_bytes(em, (uint64_t)b * em->block_size, 0, em->block_size, 1);
                if (rc != 0) {
                    return rc;
                }
                n->blocks += sectors;
                if (slot_at != 0) {
                    uint8_t raw[4];

                    LE32_PUT(raw, b);
                    rc = write_bytes(em, slot_at, raw, 4, 1);
                    if (rc != 0) {
                        return rc;
                    }
                } else {
                    *slot = b;
                    rc = inode_store(em, n);
                    if (rc != 0) {
                        return rc;
                    }
                }
                *slot = b;
                hint = b + 1;
            }
            b = *slot;
            if (!data_block_ok(em, b)) {
                return -EIO;
            }
            slot_at = (uint64_t)b * em->block_size + (uint64_t)(rest / div) * 4;
            rc = read_bytes(em, slot_at, ptr, 4);
            if (rc != 0) {
                return rc;
            }
            {
                static uint32_t held; /* the slot's value, read from the block */

                held = le32(ptr);
                slot = &held;
            }
            rest %= div;
            level--;
        }
    }
    if (*slot == 0) {
        uint32_t b;

        rc = block_alloc(em, hint, &b);
        if (rc != 0) {
            return rc;
        }
        n->blocks += sectors;
        if (slot_at != 0) {
            uint8_t raw[4];

            LE32_PUT(raw, b);
            rc = write_bytes(em, slot_at, raw, 4, 1);
            if (rc != 0) {
                return rc;
            }
        }
        *slot = b;
        *fresh = 1;
    } else if (!data_block_ok(em, *slot)) {
        return -EIO;
    }
    *out = *slot;
    return 0;
}

/* ---- directories, written ---- */

static uint32_t dirent_len(uint32_t name_len)
{
    return (8 + name_len + 3) & ~3u;
}

static uint32_t ftype_of(uint32_t mode)
{
    switch (mode & S_IFMT_) {
        case S_IFREG_:
            return 1;
        case S_IFDIR_:
            return 2;
        case S_IFCHR_:
            return 3;
        case S_IFBLK_:
            return 4;
        case S_IFLNK_:
            return 7;
        default:
            return 0;
    }
}

/* Find `name` in `d`: 1 with the entry's position and its predecessor's in
 * the same block (or the position itself when it is first), 0 when absent. */
static int dir_find(struct ext2_mount *em, const struct ext2_node *d, const char *name,
                    uint32_t *at, uint32_t *prev_at, struct ext2_scan *sc)
{
    uint32_t cursor = 0, prev = 0;

    for (;;) {
        uint32_t here = cursor;
        int got = dir_scan(em, d, &cursor, sc);

        if (got <= 0) {
            return got;
        }
        if (here % em->block_size == 0) {
            prev = here;
        }
        if (sc->ino != 0 && names_equal(sc->name, name)) {
            *at = here;
            *prev_at = prev;
            return 1;
        }
        prev = here;
    }
}

/* The block of directory `d` holding byte `at`, through its map. */
static int dir_block_at(struct ext2_mount *em, const struct ext2_node *d, uint32_t at,
                        uint64_t *off)
{
    uint32_t b;
    int rc = block_at(em, d, at / em->block_size, &b);

    if (rc != 0) {
        return rc;
    }
    if (b == 0) {
        return -EIO;
    }
    *off = (uint64_t)b * em->block_size + at % em->block_size;
    return 0;
}

/* Write one entry's header and name at `off`. */
static int dirent_write(struct ext2_mount *em, uint64_t off, uint32_t ino, uint32_t rec_len,
                        const char *name, uint32_t name_len, uint32_t ftype)
{
    uint8_t raw[8 + EXT2_NAME_MAX + 4]; /* the header, the name and its padding */
    uint32_t len = dirent_len(name_len);

    k_memset(raw, 0, sizeof(raw));
    LE32_PUT(raw, ino);
    LE16_PUT(raw + 4, rec_len);
    raw[6] = (uint8_t)name_len;
    raw[7] = (uint8_t)((em->incompat & EXT2_INCOMPAT_FILETYPE) ? ftype : 0);
    k_memcpy(raw + 8, name, name_len);
    return write_bytes(em, off, raw, len, 1);
}

/**
 * Add `name` naming `ino` to directory `d`: in the first entry with room
 * behind its name, split; else in a new block, zeroed with one empty entry
 * spanning it, allocated, chained with `i_blocks`, and covered by the size
 * last. The entry itself is the last write.
 */
static int dir_add(struct ext2_mount *em, struct ext2_node *d, const char *name, uint32_t ino,
                   uint32_t ftype)
{
    uint32_t name_len = (uint32_t)k_strlen(name);
    uint32_t need = dirent_len(name_len);
    uint32_t cursor = 0;
    struct ext2_scan sc;
    int rc, fresh;

    if (name_len == 0 || name_len > EXT2_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    for (;;) {
        uint32_t here = cursor;
        uint32_t used, room;
        uint64_t off;
        int got = dir_scan(em, d, &cursor, &sc);

        if (got < 0) {
            return got;
        }
        if (got == 0) {
            break;
        }
        used = sc.ino == 0 ? 0 : dirent_len(sc.name_len);
        room = sc.rec_len - used;
        if (room < need) {
            continue;
        }
        rc = dir_block_at(em, d, here, &off);
        if (rc != 0) {
            return rc;
        }
        if (used == 0) {
            return dirent_write(em, off, ino, sc.rec_len, name, name_len, ftype);
        }
        /* Split: the new entry is written into the slack behind the old
         * name first, where the old record still covers it, and then the
         * old header is shortened to end where the new one begins. A stop
         * between the two leaves the old entry spanning its record and the
         * new one unreachable, which is the volume as it was. */
        rc = dirent_write(em, off + used, ino, room, name, name_len, ftype);
        if (rc != 0) {
            return rc;
        }
        return dirent_write(em, off, sc.ino, used, sc.name, sc.name_len, sc.type);
    }
    /* A new block. */
    {
        uint32_t index = (uint32_t)(d->size >> em->block_shift);
        uint32_t b;
        uint64_t off;
        uint8_t head[8];

        if (d->size + em->block_size > 0xFFFFFFFFu) {
            return -EFBIG;
        }
        rc = map_alloc(em, d, index, &b, &fresh);
        if (rc != 0) {
            return rc;
        }
        off = (uint64_t)b * em->block_size;
        rc = write_bytes(em, off, 0, em->block_size, 0);
        if (rc != 0) {
            return rc;
        }
        LE32_PUT(head, 0);
        LE16_PUT(head + 4, em->block_size);
        head[6] = 0;
        head[7] = 0;
        rc = write_bytes(em, off, head, 8, 1);
        if (rc != 0) {
            return rc;
        }
        d->size += em->block_size;
        d->mtime = d->ctime = now_seconds();
        rc = inode_store(em, d);
        if (rc != 0) {
            return rc;
        }
        return dirent_write(em, off, ino, em->block_size, name, name_len, ftype);
    }
}

/* Remove the entry at `at` in `d`: the first in its block has its inode
 * cleared, any other is merged into its predecessor's record. */
static int dir_remove_at(struct ext2_mount *em, struct ext2_node *d, uint32_t at, uint32_t prev_at,
                         const struct ext2_scan *sc)
{
    uint64_t off;
    int rc;

    if (at % em->block_size == 0) {
        uint8_t zero[4] = {0, 0, 0, 0};

        rc = dir_block_at(em, d, at, &off);
        if (rc != 0) {
            return rc;
        }
        return write_bytes(em, off, zero, 4, 1);
    } else {
        struct ext2_scan ps;
        uint32_t cursor = prev_at;
        uint8_t raw[2];
        int got = dir_scan(em, d, &cursor, &ps);

        if (got <= 0) {
            return got < 0 ? got : -EIO;
        }
        rc = dir_block_at(em, d, prev_at, &off);
        if (rc != 0) {
            return rc;
        }
        LE16_PUT(raw, ps.rec_len + sc->rec_len);
        return write_bytes(em, off + 4, raw, 2, 1);
    }
}

/* Whether a directory has nothing but `.` and `..`. */
static int dir_is_empty(struct ext2_mount *em, const struct ext2_node *d, int *empty)
{
    uint32_t cursor = 0;
    struct ext2_scan sc;

    *empty = 1;
    for (;;) {
        int got = dir_scan(em, d, &cursor, &sc);

        if (got < 0) {
            return got;
        }
        if (got == 0) {
            return 0;
        }
        if (sc.ino != 0 && !names_equal(sc.name, ".") && !names_equal(sc.name, "..")) {
            *empty = 0;
            return 0;
        }
    }
}

/* Rewrite the inode `..` names in directory `d`, its second entry. */
static int dir_set_dotdot(struct ext2_mount *em, const struct ext2_node *d, uint32_t parent)
{
    uint32_t cursor = 0;
    struct ext2_scan sc;
    uint8_t raw[4];
    uint64_t off;
    int rc;

    for (;;) {
        uint32_t here = cursor;
        int got = dir_scan(em, d, &cursor, &sc);

        if (got <= 0) {
            return got < 0 ? got : -EIO;
        }
        if (sc.ino != 0 && names_equal(sc.name, "..")) {
            rc = dir_block_at(em, d, here, &off);
            if (rc != 0) {
                return rc;
            }
            LE32_PUT(raw, parent);
            return write_bytes(em, off, raw, 4, 1);
        }
    }
}

/* Whether inode `ino` is `ancestor` or below it, by `..` from `ino`. */
static int is_below(struct ext2_mount *em, uint32_t ino, uint32_t ancestor, int *below)
{
    int depth;

    *below = 0;
    for (depth = 0; depth < PATH_DEPTH; depth++) {
        struct ext2_node cur;
        struct ext2_scan sc;
        uint32_t cursor = 0;
        int rc;

        if (ino == ancestor) {
            *below = 1;
            return 0;
        }
        if (ino == EXT2_ROOT_INO) {
            return 0;
        }
        rc = inode_load(em, ino, &cur);
        if (rc != 0) {
            return rc;
        }
        for (;;) {
            int got = dir_scan(em, &cur, &cursor, &sc);

            if (got < 0) {
                return got;
            }
            if (got == 0) {
                return -EIO;
            }
            if (sc.ino != 0 && names_equal(sc.name, "..")) {
                break;
            }
        }
        ino = sc.ino;
    }
    return -ELOOP;
}

/* A node in the pool for `ino` with holds on it, or NULL. */
static struct ext2_node *node_held(struct ext2_mount *em, uint32_t ino)
{
    int i;

    for (i = 0; i < EXT2_NODES; i++) {
        if (em->nodes[i].used && em->nodes[i].ino == ino && em->nodes[i].refs > 0) {
            return &em->nodes[i];
        }
    }
    return 0;
}

/**
 * Free an inode that has no name and no holder: its blocks from the end,
 * then the inode written freed, then its bit and count.
 */
static int release_inode(struct ext2_mount *em, struct ext2_node *n)
{
    int was_dir = (n->mode & S_IFMT_) == S_IFDIR_;
    int rc;

    if ((n->mode & S_IFMT_) == S_IFREG_ || was_dir ||
        ((n->mode & S_IFMT_) == S_IFLNK_ && n->size >= EXT2_FAST_LINK)) {
        rc = punch(em, n, 0);
        if (rc != 0) {
            return rc;
        }
    }
    rc = inode_store_freed(em, n->ino);
    if (rc != 0) {
        return rc;
    }
    n->unlinked = 0;
    return inode_bit_free(em, n->ino, was_dir);
}

/* A new inode, allocated, written initialised and held in the pool; the
 * caller names it. */
static int inode_new(struct ext2_mount *em, uint32_t parent_ino, uint32_t mode, int store,
                     struct ext2_node **out)
{
    uint32_t ino;
    struct ext2_node *n;
    int rc, i;

    rc = inode_alloc(em, parent_ino, (mode & S_IFMT_) == S_IFDIR_, &ino);
    if (rc != 0) {
        return rc;
    }
    for (i = 0; i < EXT2_NODES; i++) {
        if (!em->nodes[i].used) {
            break;
        }
    }
    if (i == EXT2_NODES) {
        inode_bit_free(em, ino, (mode & S_IFMT_) == S_IFDIR_);
        klog(KLOG_WARN, "ext2", "node pool full");
        return -ENOMEM;
    }
    n = &em->nodes[i];
    k_memset(n, 0, sizeof(*n));
    n->used = 1;
    n->refs = 1;
    n->ino = ino;
    n->mode = mode;
    n->uid = current_proc ? current_proc->euid : 0;
    n->gid = current_proc ? current_proc->egid : 0;
    n->nlink = (mode & S_IFMT_) == S_IFDIR_ ? 2 : 1;
    n->atime = n->mtime = n->ctime = now_seconds();
    if (store) {
        rc = inode_store(em, n);
        if (rc != 0) {
            n->used = 0;
            inode_bit_free(em, ino, (mode & S_IFMT_) == S_IFDIR_);
            return rc;
        }
    }
    *out = n;
    return 0;
}

/* ---- the operations ---- */

static int ext2_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *n = node;
    int rc;

    rc = assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    if (n->too_big) {
        return -EOVERFLOW;
    }
    vfs_ns_lock(mnt);
    if (what & VFS_SET_MODE) {
        n->mode = (n->mode & S_IFMT_) | (st->mode & 07777u);
    }
    if (what & VFS_SET_UID) {
        n->uid = st->uid;
    }
    if (what & VFS_SET_GID) {
        n->gid = st->gid;
    }
    if (what & VFS_SET_ATIME) {
        n->atime = (uint32_t)st->atime_sec;
    }
    if (what & VFS_SET_MTIME) {
        n->mtime = (uint32_t)st->mtime_sec;
    }
    n->ctime = now_seconds();
    rc = inode_store(em, n);
    vfs_ns_unlock(mnt);
    return rc;
}

static int ext2_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *n = handle;
    int rc;

    if ((n->mode & S_IFMT_) != S_IFREG_) {
        return -EINVAL;
    }
    rc = assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    if (n->too_big) {
        return -EOVERFLOW;
    }
    if (size > 0xFFFFFFFFu) {
        return -EFBIG;
    }
    vfs_ns_lock(mnt);
    assert_locked(mnt);
    if (size < n->size) {
        uint32_t keep = (uint32_t)((size + em->block_size - 1) >> em->block_shift);

        /* The smaller size first, then the pointers from the end, then the
         * blocks; a stop between leaves blocks past the end. The tail of
         * the last block kept is zeroed, so that a later growth over it
         * reads zeros and not what was there. */
        n->size = size;
        n->mtime = n->ctime = now_seconds();
        rc = inode_store(em, n);
        if (rc == 0 && ((uint32_t)size & (em->block_size - 1)) != 0) {
            uint32_t b;

            rc = block_at(em, n, (uint32_t)(size >> em->block_shift), &b);
            if (rc == 0 && b != 0) {
                uint32_t in = (uint32_t)size & (em->block_size - 1);

                rc = write_bytes(em, (uint64_t)b * em->block_size + in, 0, em->block_size - in, 0);
            }
        }
        if (rc == 0) {
            rc = punch(em, n, keep);
        }
        if (rc == 0) {
            rc = inode_store(em, n);
        }
    } else if (size > n->size) {
        /* The gap is a hole; the zeroed tail of the last block, if a
         * shrink left one unsynced, reaches the disk before the size
         * covers it. */
        rc = bcache_sync(em->dev);
        if (rc == 0) {
            n->size = size;
            n->mtime = n->ctime = now_seconds();
            rc = inode_store(em, n);
        }
    }
    vfs_ns_unlock(mnt);
    return rc;
}

static int ext2_fsync(struct vfs_mount *mnt, void *handle)
{
    (void)handle;
    return bcache_sync(em_of(mnt)->dev);
}

static int32_t ext2_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                          uint64_t offset)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *n = handle;
    const uint8_t *in = buf;
    uint64_t end;
    uint32_t done = 0;
    int rc, fresh;

    if ((n->mode & S_IFMT_) == S_IFDIR_) {
        return -EISDIR;
    }
    if (count == 0) {
        return 0;
    }
    if (offset >= 0xFFFFFFFFu || count > 0xFFFFFFFFu - offset) {
        return -EFBIG;
    }
    rc = assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    if (n->too_big) {
        return -EOVERFLOW;
    }
    end = offset + count;
    vfs_ns_lock(mnt);
    assert_locked(mnt);
    /* ENOSPC before anything is written: the blocks the range needs that
     * the file does not have, and at most one pointer block per level. */
    {
        uint32_t first = (uint32_t)(offset >> em->block_shift);
        uint32_t last = (uint32_t)((end - 1) >> em->block_shift);
        uint64_t need = 0;
        uint32_t i;

        for (i = first; i <= last && need <= em->free_blocks + 3; i++) {
            uint32_t b;

            rc = block_at(em, n, i, &b);
            if (rc != 0) {
                goto out;
            }
            if (b == 0) {
                need++;
            }
        }
        if (need > 0) {
            need += 3 + (last - first) / (em->block_size / 4) + 1;
        }
        if (need > em->free_blocks ||
            (!may_use_reserved() && need > 0 && em->free_blocks - need < em->r_blocks)) {
            rc = -ENOSPC;
            goto out;
        }
    }
    /* The bytes: each block allocated as it is reached, its pointer
     * written after the block is in the bitmap, the data written back. */
    while (done < count) {
        uint32_t at = (uint32_t)(offset + done);
        uint32_t in_block = at % em->block_size;
        uint32_t take = em->block_size - in_block;
        uint32_t b;

        if (take > count - done) {
            take = count - done;
        }
        rc = map_alloc(em, n, at / em->block_size, &b, &fresh);
        if (rc != 0) {
            goto out;
        }
        /* A block just given out holds whatever was freed there; the part
         * this write does not cover is zeroed before the bytes land. */
        if (fresh && take != em->block_size) {
            rc = write_bytes(em, (uint64_t)b * em->block_size, 0, em->block_size, 0);
            if (rc != 0) {
                goto out;
            }
        }
        rc = write_bytes(em, (uint64_t)b * em->block_size + in_block, in + done, take, 0);
        if (rc != 0) {
            goto out;
        }
        done += take;
    }
    /* The pointers and i_blocks, then the data synced, then the size. */
    rc = inode_store(em, n);
    if (rc != 0) {
        goto out;
    }
    if (end > n->size) {
        rc = bcache_sync(em->dev);
        if (rc != 0) {
            goto out;
        }
        n->size = end;
    }
    n->mtime = n->ctime = now_seconds();
    rc = inode_store(em, n);
out:
    vfs_ns_unlock(mnt);
    if (rc != 0) {
        return done > 0 && rc != -EROFS ? (int32_t)done : rc;
    }
    return (int32_t)count;
}

static int ext2_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode,
                       void **node)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node *n;
    struct ext2_scan sc;
    uint32_t at, prev;
    int rc;

    rc = assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    assert_locked(mnt);
    if (k_strlen(name) > EXT2_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    rc = dir_find(em, d, name, &at, &prev, &sc);
    if (rc < 0) {
        return rc;
    }
    if (rc == 1) {
        return -EEXIST;
    }
    rc = inode_new(em, d->ino, S_IFREG_ | (mode & 07777u), 1, &n);
    if (rc != 0) {
        return rc;
    }
    rc = dir_add(em, d, name, n->ino, 1);
    if (rc != 0) {
        n->nlink = 0;
        release_inode(em, n);
        n->used = 0;
        return rc;
    }
    *node = n;
    return 0;
}

static int ext2_unlink(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node loaded;
    struct ext2_node *n;
    struct ext2_scan sc;
    uint32_t at, prev;
    int rc;

    rc = assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    assert_locked(mnt);
    rc = dir_find(em, d, name, &at, &prev, &sc);
    if (rc < 0) {
        return rc;
    }
    if (rc == 0) {
        return -ENOENT;
    }
    n = node_held(em, sc.ino);
    if (!n) {
        rc = inode_load(em, sc.ino, &loaded);
        if (rc != 0) {
            return rc;
        }
        n = &loaded;
    }
    if ((n->mode & S_IFMT_) == S_IFDIR_) {
        return -EISDIR;
    }
    /* The entry first, then the count; the inode goes with its last name
     * when nothing holds it, and at the last put otherwise. */
    rc = dir_remove_at(em, d, at, prev, &sc);
    if (rc != 0) {
        return rc;
    }
    d->mtime = d->ctime = now_seconds();
    rc = inode_store(em, d);
    if (rc != 0) {
        return rc;
    }
    if (n->nlink > 0) {
        n->nlink--;
    }
    n->ctime = now_seconds();
    if (n->nlink == 0) {
        if (n == &loaded) {
            return release_inode(em, n);
        }
        n->unlinked = 1;
        return inode_store(em, n); /* the count of zero, for an orphan's sake */
    }
    return inode_store(em, n);
}

static int ext2_mkdir(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node *n;
    struct ext2_scan sc;
    uint32_t at, prev, b;
    uint8_t head[24];
    int rc, fresh;

    rc = assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    assert_locked(mnt);
    if (k_strlen(name) > EXT2_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    rc = dir_find(em, d, name, &at, &prev, &sc);
    if (rc < 0) {
        return rc;
    }
    if (rc == 1) {
        return -EEXIST;
    }
    if (d->nlink >= 65000) {
        return -EMLINK;
    }
    rc = inode_new(em, d->ino, S_IFDIR_ | (mode & 07777u), 0, &n);
    if (rc != 0) {
        return rc;
    }
    /* The new directory's block with . and .., then its inode with a count
     * of two, the parent's count, and the parent's entry last; a stop
     * before the inode leaves a set bit with nothing behind it. */
    rc = map_alloc(em, n, 0, &b, &fresh);
    if (rc == 0) {
        rc = write_bytes(em, (uint64_t)b * em->block_size, 0, em->block_size, 0);
    }
    if (rc == 0) {
        k_memset(head, 0, sizeof(head));
        LE32_PUT(head, n->ino);
        LE16_PUT(head + 4, 12);
        head[6] = 1;
        head[7] = (em->incompat & EXT2_INCOMPAT_FILETYPE) ? 2 : 0;
        head[8] = '.';
        LE32_PUT(head + 12, d->ino);
        LE16_PUT(head + 16, em->block_size - 12);
        head[18] = 2;
        head[19] = head[7];
        head[20] = '.';
        head[21] = '.';
        rc = write_bytes(em, (uint64_t)b * em->block_size, head, 24, 1);
    }
    if (rc == 0) {
        n->size = em->block_size;
        rc = inode_store(em, n);
    }
    if (rc == 0) {
        d->nlink++;
        d->mtime = d->ctime = now_seconds();
        rc = inode_store(em, d);
    }
    if (rc == 0) {
        rc = dir_add(em, d, name, n->ino, 2);
    }
    if (rc != 0) {
        n->nlink = 0;
        release_inode(em, n);
    }
    n->used = 0;
    return rc;
}

static int ext2_rmdir(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node loaded;
    struct ext2_scan sc;
    uint32_t at, prev;
    int rc, empty;

    rc = assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    assert_locked(mnt);
    rc = dir_find(em, d, name, &at, &prev, &sc);
    if (rc < 0) {
        return rc;
    }
    if (rc == 0) {
        return -ENOENT;
    }
    rc = inode_load(em, sc.ino, &loaded);
    if (rc != 0) {
        return rc;
    }
    if ((loaded.mode & S_IFMT_) != S_IFDIR_) {
        return -ENOTDIR;
    }
    rc = dir_is_empty(em, &loaded, &empty);
    if (rc != 0) {
        return rc;
    }
    if (!empty) {
        return -ENOTEMPTY;
    }
    if (node_held(em, sc.ino)) {
        return -EBUSY; /* a description or a working directory is in it */
    }
    rc = dir_remove_at(em, d, at, prev, &sc);
    if (rc != 0) {
        return rc;
    }
    if (d->nlink > 2) {
        d->nlink--;
    }
    d->mtime = d->ctime = now_seconds();
    rc = inode_store(em, d);
    if (rc != 0) {
        return rc;
    }
    loaded.nlink = 0;
    return release_inode(em, &loaded);
}

static int ext2_link(struct vfs_mount *mnt, void *dir, const char *name, void *node)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node *n = node;
    struct ext2_scan sc;
    uint32_t at, prev;
    int rc;

    rc = assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    assert_locked(mnt);
    if ((n->mode & S_IFMT_) == S_IFDIR_) {
        return -EPERM;
    }
    if (n->nlink >= 65000) {
        return -EMLINK;
    }
    if (k_strlen(name) > EXT2_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    rc = dir_find(em, d, name, &at, &prev, &sc);
    if (rc < 0) {
        return rc;
    }
    if (rc == 1) {
        return -EEXIST;
    }
    /* The count first, then the name: a stop between is a count one too
     * high, which is a count repair. */
    n->nlink++;
    n->ctime = now_seconds();
    rc = inode_store(em, n);
    if (rc != 0) {
        n->nlink--;
        return rc;
    }
    rc = dir_add(em, d, name, n->ino, ftype_of(n->mode));
    if (rc != 0) {
        n->nlink--;
        inode_store(em, n);
        return rc;
    }
    d->mtime = d->ctime = now_seconds();
    return inode_store(em, d);
}

static int ext2_symlink(struct vfs_mount *mnt, void *dir, const char *name, const char *target)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node *n;
    struct ext2_scan sc;
    uint32_t at, prev;
    uint32_t tlen = (uint32_t)k_strlen(target);
    int rc;

    rc = assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    assert_locked(mnt);
    if (k_strlen(name) > EXT2_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    if (tlen == 0 || tlen >= em->block_size) {
        return -ENAMETOOLONG;
    }
    rc = dir_find(em, d, name, &at, &prev, &sc);
    if (rc < 0) {
        return rc;
    }
    if (rc == 1) {
        return -EEXIST;
    }
    rc = inode_new(em, d->ino, S_IFLNK_ | 0777u, 0, &n);
    if (rc != 0) {
        return rc;
    }
    if (tlen < EXT2_FAST_LINK) {
        uint32_t i;

        for (i = 0; i < tlen; i++) {
            n->block[i / 4] |= (uint32_t)(uint8_t)target[i] << ((i % 4) * 8);
        }
    } else {
        uint32_t b;
        int fresh;

        rc = map_alloc(em, n, 0, &b, &fresh);
        if (rc == 0) {
            rc = write_bytes(em, (uint64_t)b * em->block_size, 0, em->block_size, 0);
        }
        if (rc == 0) {
            rc = write_bytes(em, (uint64_t)b * em->block_size, target, tlen, 1);
        }
    }
    if (rc == 0) {
        n->size = tlen;
        rc = inode_store(em, n);
    }
    if (rc == 0) {
        rc = dir_add(em, d, name, n->ino, 7);
    }
    if (rc != 0) {
        n->nlink = 0;
        release_inode(em, n);
    }
    n->used = 0;
    return rc;
}

/**
 * Rename within the mount. A file: the count raised, the new entry written,
 * the old entry removed, the count lowered, so that the file has a name at
 * every stop. A destination file is replaced by rewriting its entry to
 * the source's inode, after which the victim loses a count and goes as an
 * unlink's does. A directory moves by a new entry, its `..` rewritten,
 * the new parent's count raised, the old entry removed and the old
 * parent's count lowered; a destination directory must be empty and
 * goes first, as rmdir's does; a directory may not move into itself.
 */
static int ext2_rename(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
                       const char *newname)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *od = olddir ? olddir : mnt->root;
    struct ext2_node *nd = newdir ? newdir : mnt->root;
    struct ext2_node loaded, victim_loaded;
    struct ext2_node *n, *victim = 0;
    struct ext2_scan osc, nsc;
    uint32_t oat, oprev, nat, nprev;
    int rc, have_new, is_dir;

    rc = assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    assert_locked(mnt);
    if (k_strlen(newname) > EXT2_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    rc = dir_find(em, od, oldname, &oat, &oprev, &osc);
    if (rc < 0) {
        return rc;
    }
    if (rc == 0) {
        return -ENOENT;
    }
    have_new = dir_find(em, nd, newname, &nat, &nprev, &nsc);
    if (have_new < 0) {
        return have_new;
    }
    if (have_new && nsc.ino == osc.ino) {
        return 0; /* two names for one file: nothing changes */
    }
    n = node_held(em, osc.ino);
    if (!n) {
        rc = inode_load(em, osc.ino, &loaded);
        if (rc != 0) {
            return rc;
        }
        n = &loaded;
    }
    is_dir = (n->mode & S_IFMT_) == S_IFDIR_;
    if (have_new) {
        int victim_dir;

        victim = node_held(em, nsc.ino);
        if (!victim) {
            rc = inode_load(em, nsc.ino, &victim_loaded);
            if (rc != 0) {
                return rc;
            }
            victim = &victim_loaded;
        }
        victim_dir = (victim->mode & S_IFMT_) == S_IFDIR_;
        if (victim_dir && !is_dir) {
            return -EISDIR;
        }
        if (!victim_dir && is_dir) {
            return -ENOTDIR;
        }
        if (victim_dir) {
            int empty;

            rc = dir_is_empty(em, victim, &empty);
            if (rc != 0) {
                return rc;
            }
            if (!empty) {
                return -ENOTEMPTY;
            }
            if (victim != &victim_loaded) {
                return -EBUSY;
            }
        }
    }
    if (is_dir && od != nd) {
        int below;

        rc = is_below(em, nd->ino, n->ino, &below);
        if (rc != 0) {
            return rc;
        }
        if (below) {
            return -EINVAL;
        }
        if (nd->nlink >= 65000) {
            return -EMLINK;
        }
    }
    if (is_dir) {
        /* A directory: the destination removed as rmdir's would be, the new
         * entry written, `..` rewritten, the new parent's count raised, the
         * old entry removed and the old parent's count lowered. */
        if (have_new) {
            rc = dir_remove_at(em, nd, nat, nprev, &nsc);
            if (rc != 0) {
                return rc;
            }
            if (nd->nlink > 2) {
                nd->nlink--;
            }
            rc = inode_store(em, nd);
            if (rc != 0) {
                return rc;
            }
            victim_loaded.nlink = 0;
            rc = release_inode(em, &victim_loaded);
            if (rc != 0) {
                return rc;
            }
            /* The old entry's position may have moved with the removal
             * when the two names share a directory. */
            rc = dir_find(em, od, oldname, &oat, &oprev, &osc);
            if (rc <= 0) {
                return rc < 0 ? rc : -ENOENT;
            }
        }
        rc = dir_add(em, nd, newname, n->ino, 2);
        if (rc != 0) {
            return rc;
        }
        if (od != nd) {
            rc = dir_set_dotdot(em, n, nd->ino);
            if (rc != 0) {
                return rc;
            }
            nd->nlink++;
            rc = inode_store(em, nd);
            if (rc != 0) {
                return rc;
            }
        }
        rc = dir_find(em, od, oldname, &oat, &oprev, &osc);
        if (rc <= 0) {
            return rc < 0 ? rc : -ENOENT;
        }
        rc = dir_remove_at(em, od, oat, oprev, &osc);
        if (rc != 0) {
            return rc;
        }
        if (od != nd && od->nlink > 2) {
            od->nlink--;
        }
        od->mtime = od->ctime = now_seconds();
        return inode_store(em, od);
    }
    /* A file: the count raised, the new entry written or rewritten, the
     * victim let go, the old entry removed, the count lowered. */
    n->nlink++;
    n->ctime = now_seconds();
    rc = inode_store(em, n);
    if (rc != 0) {
        n->nlink--;
        return rc;
    }
    if (have_new) {
        uint64_t off;
        uint8_t raw[4];

        rc = dir_block_at(em, nd, nat, &off);
        if (rc == 0) {
            LE32_PUT(raw, n->ino);
            rc = write_bytes(em, off, raw, 4, 1);
        }
        if (rc == 0 && (em->incompat & EXT2_INCOMPAT_FILETYPE)) {
            uint8_t ft = (uint8_t)ftype_of(n->mode);

            rc = write_bytes(em, off + 7, &ft, 1, 1);
        }
        if (rc != 0) {
            return rc;
        }
        if (victim->nlink > 0) {
            victim->nlink--;
        }
        victim->ctime = now_seconds();
        if (victim->nlink == 0) {
            if (victim == &victim_loaded) {
                rc = release_inode(em, victim);
            } else {
                victim->unlinked = 1;
                rc = inode_store(em, victim);
            }
        } else {
            rc = inode_store(em, victim);
        }
        if (rc != 0) {
            return rc;
        }
    } else {
        rc = dir_add(em, nd, newname, n->ino, ftype_of(n->mode));
        if (rc != 0) {
            return rc;
        }
    }
    rc = dir_find(em, od, oldname, &oat, &oprev, &osc);
    if (rc <= 0) {
        return rc < 0 ? rc : -ENOENT;
    }
    rc = dir_remove_at(em, od, oat, oprev, &osc);
    if (rc != 0) {
        return rc;
    }
    n->nlink--;
    rc = inode_store(em, n);
    if (rc != 0) {
        return rc;
    }
    od->mtime = od->ctime = now_seconds();
    rc = inode_store(em, od);
    if (rc == 0 && od != nd) {
        nd->mtime = nd->ctime = od->mtime;
        rc = inode_store(em, nd);
    }
    return rc;
}

/* ---- the mount ---- */

/* ---- the mount-time check ---- */

/*
 * A volume whose valid bit is clear was not unmounted, and one whose error
 * bit is set was found wanting by an earlier mount. Either is checked
 * before it is written, under the walk lock and the shared bitmap the FAT
 * drivers use: the structural blocks are seeded into the bitmap, the tree
 * is walked from the root and every block every reached inode names is
 * marked, the inodes the inode bitmap calls used but the walk did not
 * reach are scanned as orphans, and the bitmaps are compared with the
 * marks. Nothing is repaired. Lost blocks and orphans are counted and
 * kept, held out of the allocator by the bits they still have, and the
 * error bit is set so that the volume is not marked clean again before
 * `e2fsck` has run. A fault mounts the volume read-only with the first one
 * named: a block claimed twice or out of range, an inode whose pointers do
 * not match its size or its `i_blocks`, a directory with two parents or
 * without its parent, an entry of the wrong form, a link count that does
 * not match the entries, a used block or a reached inode the bitmap calls
 * free. The group descriptors' free counts are recomputed from the bitmaps
 * and not believed, because a crash leaves them stale.
 *
 * The walk keeps its state here rather than on the stack: the node of each
 * directory level, the hard links met with their expected and found
 * counts, the reached-inode set and one bitmap block. A volume with more
 * inodes than the set, more blocks than the shared bitmap, a directory
 * deeper than the levels or more hard-linked files than the table mounts
 * read-only when dirty and says which limit.
 */
#define EXT2_CHECK_INODES (1u << 20)
#define EXT2_CHECK_DEPTH 32
#define EXT2_CHECK_LINKS 512

struct ext2_link {
    uint32_t ino;
    uint32_t expect, found;
};

struct ext2_check {
    struct ext2_mount *em;
    const char *fault;
    uint32_t fault_ino;
    uint32_t lost, orphans, links;
    struct ext2_link link[EXT2_CHECK_LINKS];
    struct ext2_node node[EXT2_CHECK_DEPTH];
    uint8_t seen[EXT2_CHECK_INODES / 8];
    uint8_t bitmap[4096];
};

static struct ext2_check ext2_check_state; /* one check at a time, under the walk lock */

static int check_fault(struct ext2_check *c, uint32_t ino, const char *why)
{
    if (!c->fault) {
        c->fault = why;
        c->fault_ino = ino;
    }
    return -EIO;
}

static int seen_test(const struct ext2_check *c, uint32_t ino)
{
    return (c->seen[(ino - 1) >> 3] >> ((ino - 1) & 7)) & 1;
}

static void seen_set(struct ext2_check *c, uint32_t ino)
{
    c->seen[(ino - 1) >> 3] |= (uint8_t)(1u << ((ino - 1) & 7));
}

/* Mark a block an inode names; a second mark is a cross-link. */
static int check_mark(struct ext2_check *c, uint32_t ino, uint32_t b)
{
    const struct ext2_mount *em = c->em;

    if (b == 0 || b < em->first_data || b >= em->blocks) {
        return check_fault(c, ino, "a block out of range");
    }
    if (fat_walk_test_set(b)) {
        return check_fault(c, ino, "a block claimed twice");
    }
    return 0;
}

/*
 * Mark what a pointer block at `level` names, `index` being the file block
 * index its first entry stands for; `count` gains every block, data and
 * pointer, and `last` the highest index named.
 */
static int check_level(struct ext2_check *c, uint32_t ino, uint32_t b, int level, uint32_t index,
                       uint32_t *count, uint32_t *last)
{
    struct ext2_mount *em = c->em;
    uint32_t per = em->block_size / 4;
    uint32_t span = 1, j;
    int i, rc;

    rc = check_mark(c, ino, b);
    if (rc != 0) {
        return rc;
    }
    (*count)++;
    for (i = 1; i < level; i++) {
        span *= per;
    }
    for (j = 0; j < per; j++) {
        uint8_t raw[4];
        uint32_t p;

        rc = read_bytes(em, (uint64_t)b * em->block_size + j * 4, raw, 4);
        if (rc != 0) {
            return rc;
        }
        p = le32(raw);
        if (p == 0) {
            continue;
        }
        if (level == 1) {
            rc = check_mark(c, ino, p);
            if (rc != 0) {
                return rc;
            }
            (*count)++;
            *last = index + j;
        } else {
            rc = check_level(c, ino, p, level - 1, index + j * span, count, last);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return 0;
}

/*
 * Mark every block `n` names and check them against its size and its
 * `i_blocks`. A fast symbolic link, a device, a pipe and a socket keep
 * none; `as_data` treats the inode as a file whatever its mode, for the
 * reserved inodes.
 */
static int check_blocks(struct ext2_check *c, const struct ext2_node *n, int as_data)
{
    const struct ext2_mount *em = c->em;
    uint32_t type = n->mode & S_IFMT_;
    uint32_t per = em->block_size / 4;
    uint32_t count = 0, last = 0, limit, i;
    int named = 0, rc;

    if (!as_data && type != S_IFREG_ && type != S_IFDIR_ &&
        (type != S_IFLNK_ || n->blocks == 0)) {
        if (n->blocks != 0) {
            return check_fault(c, n->ino, "blocks on an inode that keeps none");
        }
        return 0;
    }
    for (i = 0; i < EXT2_NDIR; i++) {
        if (n->block[i] == 0) {
            continue;
        }
        rc = check_mark(c, n->ino, n->block[i]);
        if (rc != 0) {
            return rc;
        }
        count++;
        last = i;
        named = 1;
    }
    for (i = 0; i < 3; i++) {
        uint32_t base = EXT2_NDIR + (i >= 1 ? per : 0) + (i >= 2 ? per * per : 0);

        if (n->block[EXT2_NDIR + i] == 0) {
            continue;
        }
        rc = check_level(c, n->ino, n->block[EXT2_NDIR + i], (int)i + 1, base, &count, &last);
        if (rc != 0) {
            return rc;
        }
        named = 1;
    }
    if (n->blocks != count * (em->block_size / 512)) {
        return check_fault(c, n->ino, "i_blocks that does not match its pointers");
    }
    limit = (uint32_t)((n->size + em->block_size - 1) >> em->block_shift);
    if (named && last >= limit) {
        return check_fault(c, n->ino, "a block past the size");
    }
    return 0;
}

static int check_dir(struct ext2_check *c, uint32_t ino, uint32_t parent, int depth);

/*
 * One entry's inode, reached from directory `dir`: a directory is walked
 * once and a second reach is a second parent; a file is marked once and a
 * second name counts toward its link count.
 */
static int check_child(struct ext2_check *c, uint32_t ino, uint32_t dir, int depth,
                       uint32_t *subdirs)
{
    struct ext2_mount *em = c->em;
    struct ext2_node *n;
    uint32_t type, i;
    int rc;

    if (ino > em->inodes) {
        return check_fault(c, dir, "an entry naming an inode past the count");
    }
    if (depth >= EXT2_CHECK_DEPTH) {
        return check_fault(c, ino, "a directory deeper than the check walks");
    }
    n = &c->node[depth];
    rc = inode_load(em, ino, n);
    if (rc != 0) {
        return rc;
    }
    type = n->mode & S_IFMT_;
    if (type == S_IFDIR_) {
        if (seen_test(c, ino)) {
            return check_fault(c, ino, "a directory with two parents");
        }
        seen_set(c, ino);
        (*subdirs)++;
        return check_dir(c, ino, dir, depth);
    }
    if (seen_test(c, ino)) {
        for (i = 0; i < c->links; i++) {
            if (c->link[i].ino == ino) {
                c->link[i].found++;
                return 0;
            }
        }
        return check_fault(c, ino, "named twice with one link");
    }
    seen_set(c, ino);
    if (type == 0) {
        return check_fault(c, ino, "an entry naming a free inode");
    }
    rc = check_blocks(c, n, 0);
    if (rc != 0) {
        return rc;
    }
    if (n->nlink != 1) {
        if (c->links >= EXT2_CHECK_LINKS) {
            return check_fault(c, ino, "more hard links than the check's table");
        }
        c->link[c->links].ino = ino;
        c->link[c->links].expect = n->nlink;
        c->link[c->links].found = 1;
        c->links++;
    }
    return 0;
}

/*
 * Walk directory `ino`, loaded at `c->node[depth]`: its blocks, every
 * entry's form, `.` and `..` in their places, each other entry's inode,
 * and its link count against two plus its subdirectories.
 */
static int check_dir(struct ext2_check *c, uint32_t ino, uint32_t parent, int depth)
{
    struct ext2_mount *em = c->em;
    const struct ext2_node *n = &c->node[depth];
    uint32_t nblocks, i, subdirs = 0, entry = 0;
    int rc;

    rc = check_blocks(c, n, 0);
    if (rc != 0) {
        return rc;
    }
    if (n->size == 0 || (n->size & (em->block_size - 1)) != 0 || n->too_big) {
        return check_fault(c, ino, "a directory size that is not whole blocks");
    }
    nblocks = (uint32_t)(n->size >> em->block_shift);
    for (i = 0; i < nblocks; i++) {
        uint32_t b, off = 0;

        rc = block_at(em, n, i, &b);
        if (rc != 0) {
            return rc;
        }
        if (b == 0) {
            return check_fault(c, ino, "a hole in a directory");
        }
        while (off < em->block_size) {
            uint8_t raw[10];
            uint64_t at = (uint64_t)b * em->block_size + off;
            uint32_t eino, rec, nlen;

            rc = read_bytes(em, at, raw, 8);
            if (rc != 0) {
                return rc;
            }
            eino = le32(raw);
            rec = le16(raw + 4);
            nlen = raw[6];
            if (rec < 8 || (rec & 3) != 0 || off + rec > em->block_size || nlen + 8 > rec) {
                return check_fault(c, ino, "an entry of the wrong form");
            }
            if (entry < 2) {
                rc = read_bytes(em, at + 8, raw + 8, nlen < 2 ? nlen : 2);
                if (rc != 0) {
                    return rc;
                }
            }
            if (entry == 0) {
                if (eino != ino || nlen != 1 || raw[8] != '.') {
                    return check_fault(c, ino, "a first entry that is not .");
                }
            } else if (entry == 1) {
                if (eino != parent || nlen != 2 || raw[8] != '.' || raw[9] != '.') {
                    return check_fault(c, ino, "a second entry that is not the parent");
                }
            } else if (eino != 0) {
                if (nlen == 0) {
                    return check_fault(c, ino, "an entry with an empty name");
                }
                rc = check_child(c, eino, ino, depth + 1, &subdirs);
                if (rc != 0) {
                    return rc;
                }
            }
            entry++;
            off += rec;
        }
    }
    if (entry < 2) {
        return check_fault(c, ino, "a directory without . and ..");
    }
    if (n->nlink != 2 + subdirs) {
        return check_fault(c, ino, "a directory link count that does not match its entries");
    }
    return 0;
}

/*
 * The inode scan and the bitmap compare, group by group: an orphan is an
 * inode the bitmap calls used that the walk did not reach, its blocks
 * marked so that nothing else may claim them; one whose link count is
 * zero is a deleted inode whose bit was not yet cleared, counted with
 * them because `e2fsck` is owed the bit, its pointers not read because
 * they mean nothing, as `e2fsck` reads it; a used block the walk did not
 * mark is lost; the free counts are recounted into the descriptors.
 */
static int check_bitmaps(struct ext2_check *c)
{
    struct ext2_mount *em = c->em;
    uint32_t g, i;
    int rc;

    em->free_inodes = 0;
    em->free_blocks = 0;
    for (g = 0; g < em->groups; g++) {
        uint8_t *gd = em->gd + g * 32;
        uint32_t free = 0;

        rc = read_bytes(em, (uint64_t)le32(gd + 4) * em->block_size, c->bitmap, em->block_size);
        if (rc != 0) {
            return rc;
        }
        for (i = 0; i < em->ipg; i++) {
            uint32_t ino = g * em->ipg + i + 1;
            int used = (c->bitmap[i >> 3] >> (i & 7)) & 1;

            if (ino > em->inodes) {
                break;
            }
            if (!used) {
                free++;
                if (seen_test(c, ino)) {
                    return check_fault(c, ino, "a reached inode the bitmap calls free");
                }
                continue;
            }
            if (seen_test(c, ino) || ino < em->first_ino) {
                continue;
            }
            rc = inode_load(em, ino, &c->node[0]);
            if (rc != 0) {
                return rc;
            }
            c->orphans++;
            if (c->node[0].nlink == 0) {
                continue; /* deleted, as e2fsck reads it: counted, its pointers not read */
            }
            rc = check_blocks(c, &c->node[0], 0);
            if (rc != 0) {
                return rc;
            }
        }
        LE16_PUT(gd + 14, free);
        em->free_inodes += free;
    }
    for (g = 0; g < em->groups; g++) {
        uint8_t *gd = em->gd + g * 32;
        uint32_t lo = em->first_data + g * em->bpg;
        uint32_t free = 0;

        rc = read_bytes(em, (uint64_t)le32(gd + 0) * em->block_size, c->bitmap, em->block_size);
        if (rc != 0) {
            return rc;
        }
        for (i = 0; i < em->bpg && lo + i < em->blocks; i++) {
            int used = (c->bitmap[i >> 3] >> (i & 7)) & 1;
            int marked = fat_walk_marked(lo + i);

            if (!used) {
                free++;
                if (marked) {
                    return check_fault(c, 0, "a used block the bitmap calls free");
                }
            } else if (!marked) {
                c->lost++;
            }
        }
        LE16_PUT(gd + 12, free);
        em->free_blocks += free;
    }
    return 0;
}

/*
 * The check over a dirty volume, run once at mount before the mount is
 * published. Returns 0 with the volume writable, 1 with `*why` naming why
 * it must stay read-only, or a negative errno when the device failed. The
 * superblock's error bit and the descriptors' counts are written here
 * when there is something to record; the counts stay in memory when the
 * volume is read-only.
 */
static int ext2_check_volume(struct ext2_mount *em, const char **why)
{
    struct ext2_check *c = &ext2_check_state;
    uint64_t free_blocks = em->free_blocks, free_inodes = em->free_inodes;
    uint32_t g, ino;
    int rc;

    if (em->blocks + 2 > FAT_WALK_CLUSTERS) {
        *why = "not unmounted cleanly and more blocks than the check's bitmap";
        return 1;
    }
    if (em->inodes > EXT2_CHECK_INODES) {
        *why = "not unmounted cleanly and more inodes than the check's set";
        return 1;
    }
    fat_walk_acquire(em->blocks);
    c->em = em;
    c->fault = 0;
    c->fault_ino = 0;
    c->lost = c->orphans = c->links = 0;
    k_memset(c->seen, 0, (em->inodes + 7) / 8);
    for (g = 0; g < em->groups; g++) {
        const uint8_t *gd = em->gd + g * 32;
        uint32_t lo = em->first_data + g * em->bpg, b;

        if (has_super(em, g)) {
            for (b = lo; b <= lo + gd_blocks(em); b++) {
                fat_walk_test_set(b);
            }
        }
        fat_walk_test_set(le32(gd + 0));
        fat_walk_test_set(le32(gd + 4));
        for (b = 0; b < inode_table_blocks(em); b++) {
            fat_walk_test_set(le32(gd + 8) + b);
        }
    }
    rc = 0;
    for (ino = 1; ino < em->first_ino && rc == 0; ino++) {
        seen_set(c, ino);
        if (ino == EXT2_ROOT_INO) {
            continue;
        }
        rc = inode_load(em, ino, &c->node[0]);
        if (rc == 0 && c->node[0].blocks != 0) {
            rc = check_blocks(c, &c->node[0], 1);
        }
    }
    if (rc == 0) {
        seen_set(c, EXT2_ROOT_INO); /* not below first_ino on every volume */
        rc = inode_load(em, EXT2_ROOT_INO, &c->node[0]);
    }
    if (rc == 0) {
        rc = check_dir(c, EXT2_ROOT_INO, EXT2_ROOT_INO, 0);
    }
    for (g = 0; g < c->links && rc == 0; g++) {
        if (c->link[g].expect != c->link[g].found) {
            rc = check_fault(c, c->link[g].ino, "a link count that does not match its names");
        }
    }
    if (rc == 0) {
        rc = check_bitmaps(c);
    }
    fat_walk_release();
    if (rc != 0 && !c->fault) {
        return rc;
    }
    if (c->fault) {
        /* The counts as the disk has them, since the recount may have
         * stopped part way and only the error bit is written here. */
        em->free_blocks = free_blocks;
        em->free_inodes = free_inodes;
        rc = read_bytes(em, (uint64_t)(em->first_data + 1) * em->block_size, em->gd,
                        em->groups * 32);
        if (rc != 0) {
            return rc;
        }
        klog(KLOG_WARN, "ext2", "check: inode %d: %s", (int)c->fault_ino, c->fault);
        em->state |= EXT2_ERROR_FS;
        rc = sb_store(em);
        if (rc != 0) {
            return rc;
        }
        *why = "the check found a fault";
        return 1;
    }
    for (g = 0; g < em->groups; g++) {
        rc = gd_store(em, g);
        if (rc != 0) {
            return rc;
        }
    }
    if (c->lost != 0 || c->orphans != 0) {
        klog(KLOG_WARN, "ext2", "check: %d lost blocks, %d orphan inodes, kept for e2fsck",
             (int)c->lost, (int)c->orphans);
        em->state |= EXT2_ERROR_FS;
    } else {
        klog(KLOG_INFO, "ext2", "check: clean");
    }
    rc = sb_store(em);
    return rc;
}

static int refuse(const char *field)
{
    klog(KLOG_WARN, "ext2", "refused: %s", field);
    return -EINVAL;
}

/**
 * The volume check, field by field, before anything is published: the
 * superblock's magic, revision, sizes and counts, the features against the
 * three sets, the descriptors against the volume, and the root inode.
 * Returns 0 with the mount described, or a negative errno.
 */
static int ext2_mount_on(struct ext2_mount *em, struct vfs_mount *mnt, block_device_t *dev,
                         uint32_t flags)
{
    uint8_t sb[1024];
    uint32_t log_size, rev, unknown;
    const char *readonly_why = 0;
    struct ext2_node *root;
    uint32_t g;
    int rc;

    em->dev = dev;
    em->mnt = mnt;
    rc = read_bytes(em, 1024, sb, sizeof(sb));
    if (rc != 0) {
        return rc;
    }
    if (le16(sb + 56) != EXT2_SUPER_MAGIC) {
        return refuse("magic");
    }
    rev = le32(sb + 76);
    if (rev > 1) {
        return refuse("revision");
    }
    log_size = le32(sb + 24);
    if (log_size > 2) {
        return refuse("block size");
    }
    em->block_size = 1024u << log_size;
    em->block_shift = 10 + log_size;
    em->inodes = le32(sb + 0);
    em->blocks = le32(sb + 4);
    em->r_blocks = le32(sb + 8);
    em->first_data = le32(sb + 20);
    em->bpg = le32(sb + 32);
    em->ipg = le32(sb + 40);
    em->state = le16(sb + 58);
    if (rev == 0) {
        em->inode_size = 128;
        em->first_ino = 11;
        em->compat = em->incompat = em->ro_compat = 0;
    } else {
        em->first_ino = le32(sb + 84);
        em->inode_size = le16(sb + 88);
        em->compat = le32(sb + 92);
        em->incompat = le32(sb + 96);
        em->ro_compat = le32(sb + 100);
    }
    if (em->inode_size != 128 && em->inode_size != 256) {
        return refuse("inode size");
    }
    if (em->first_data != (log_size == 0 ? 1u : 0u)) {
        return refuse("first data block");
    }
    if (em->blocks == 0 || em->bpg == 0 || em->ipg == 0 || em->inodes == 0 ||
        em->bpg > em->block_size * 8 || em->ipg > em->block_size * 8 || em->first_ino < 2 ||
        em->first_ino > em->inodes) {
        return refuse("counts");
    }
    if ((uint64_t)em->blocks * em->block_size > dev->sector_count * (uint64_t)dev->sector_size) {
        return refuse("block count past the device");
    }
    em->groups = (em->blocks - em->first_data + em->bpg - 1) / em->bpg;
    if (em->groups > EXT2_GROUPS) {
        return refuse("more groups than this driver's table");
    }
    if ((uint64_t)em->groups * em->ipg < em->inodes) {
        return refuse("inodes past the groups");
    }
    if ((uint64_t)em->first_data + 1 + gd_blocks(em) > em->blocks) {
        return refuse("descriptor table past the volume");
    }
    unknown = em->incompat & ~EXT2_INCOMPAT_FILETYPE;
    if (unknown != 0) {
        klog(KLOG_WARN, "ext2", "refused: incompatible feature 0x%x", (int)unknown);
        return -EINVAL;
    }
    unknown = em->ro_compat & ~(EXT2_RO_SPARSE_SUPER | EXT2_RO_LARGE_FILE);
    if (unknown != 0) {
        readonly_why = "unknown read-only-compatible feature";
    } else if (em->compat & EXT2_COMPAT_DIR_INDEX) {
        readonly_why = "dir_index would be left stale by this writer";
    } else if (em->block_size != 4096) {
        readonly_why = "a block size below 4096 cannot be written in order";
    }
    rc = read_bytes(em, (uint64_t)(em->first_data + 1) * em->block_size, em->gd,
                    em->groups * 32);
    if (rc != 0) {
        return rc;
    }
    em->free_blocks = 0;
    em->free_inodes = 0;
    for (g = 0; g < em->groups; g++) {
        const uint8_t *gd = em->gd + g * 32;
        uint32_t bb = le32(gd + 0), ib = le32(gd + 4), it = le32(gd + 8);
        uint32_t lo = em->first_data + g * em->bpg;
        uint32_t hi = lo + em->bpg;
        uint32_t tables = inode_table_blocks(em);

        /* Each group's two bitmaps and inode table lie in that group, past
         * its superblock copy and descriptors when it has them, and apart
         * from each other; that is where `data_block_ok` looks for them. */
        if (hi > em->blocks) {
            hi = em->blocks;
        }
        if (has_super(em, g)) {
            lo += 1 + gd_blocks(em);
        }
        if (bb < lo || bb >= hi || ib < lo || ib >= hi || it < lo || tables == 0 ||
            (uint64_t)it + tables > hi || bb == ib || (bb >= it && bb < it + tables) ||
            (ib >= it && ib < it + tables)) {
            return refuse("group descriptor");
        }
        if (le16(gd + 12) > em->bpg || le16(gd + 14) > em->ipg) {
            return refuse("group free count");
        }
        em->free_blocks += le16(gd + 12);
        em->free_inodes += le16(gd + 14);
    }
    root = node_for(em, EXT2_ROOT_INO, &rc);
    if (!root) {
        return rc;
    }
    if ((root->mode & S_IFMT_) != S_IFDIR_) {
        ext2_put(mnt, root);
        return refuse("root inode");
    }
    if (!readonly_why && !(flags & VFS_MS_RDONLY) &&
        (!(em->state & EXT2_VALID_FS) || (em->state & EXT2_ERROR_FS))) {
        rc = ext2_check_volume(em, &readonly_why);
        if (rc < 0) {
            ext2_put(mnt, root);
            return rc;
        }
    }
    em->writable = readonly_why == 0 && !(flags & VFS_MS_RDONLY);
    if (readonly_why) {
        klog(KLOG_WARN, "ext2", "mounted read-only: %s", readonly_why);
        mnt->flags |= VFS_MS_RDONLY;
    }
    em->alloc_hint = em->first_data;
    if (em->writable) {
        /* The valid bit cleared before the mount is published, so that a
         * volume this mount leaves behind is one the next mount checks. */
        em->state &= ~EXT2_VALID_FS;
        rc = sb_store(em);
        if (rc != 0) {
            ext2_put(mnt, root);
            return rc;
        }
        em->valid_cleared = 1;
    }
    mnt->priv = em;
    mnt->root = root;
    klog(KLOG_INFO, "ext2", "mounted: %d blocks of %d, %d inodes, %d groups%s", (int)em->blocks,
         (int)em->block_size, (int)em->inodes, (int)em->groups,
         em->writable ? "" : ", read-only");
    return 0;
}

static int ext2_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                      const char *options)
{
    struct ext2_mount *em = 0;
    int i, rc;

    (void)options;
    for (i = 0; i < EXT2_MOUNTS; i++) {
        if (!ext2_mounts[i].used) {
            em = &ext2_mounts[i];
            break;
        }
    }
    if (!em) {
        return -ENOMEM;
    }
    k_memset(em, 0, sizeof(*em));
    em->used = 1; /* claimed before the constructor sleeps */
    rc = ext2_mount_on(em, mnt, dev, flags);
    if (rc != 0) {
        em->used = 0;
    }
    return rc;
}

static const struct vfs_ops ext2_ops = {
    ext2_lookup,  0, /* lookup_rest */
    ext2_hold,    ext2_put,      ext2_getattr,  ext2_setattr, ext2_open,   ext2_release,
    ext2_read,    ext2_write,    ext2_truncate, ext2_fsync,   ext2_readdir, ext2_create,
    ext2_unlink,  ext2_mkdir,    ext2_rmdir,    ext2_rename,  ext2_link,   ext2_readlink,
    ext2_statfs,  ext2_sync,     ext2_prepare_unmount, ext2_unmount, ext2_path, ext2_symlink,
};

const struct vfs_type vfs_ext2_type = {"ext2", ext2_mount, &ext2_ops};
