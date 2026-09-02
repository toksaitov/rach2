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
#include "bcache.h"

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
 * Whether a data pointer may name block `b`: inside the volume, past the
 * boot block, and not a superblock, a descriptor table, a bitmap or an
 * inode table. A pointer that names one of those is a fault the walk
 * refuses, since reading it as data would read the volume's own tables.
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
        /* The high word of the size, with `large_file`; a file the kernel's
         * 32-bit offset cannot reach is marked and left as it is. */
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

static void ext2_put(struct vfs_mount *mnt, void *node)
{
    struct ext2_node *n = node;

    (void)mnt;
    if (n->refs > 0) {
        n->refs--;
    }
    if (n->refs == 0) {
        n->used = 0;
    }
}

/* ---- block mapping ---- */

/**
 * The block that holds byte `index * block_size` of `n`: 0 for a hole, or
 * the pointer through the direct, indirect, double and triple levels, each
 * pointer block checked before it is read.
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
        sc->name_len + 8 > sc->rec_len) {
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
        return -EACCES; /* a device on a volume is not a device; /dev has those */
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

            ent->is_dir = inode_load(em, sc.ino, &probe) == 0 &&
                          (probe.mode & S_IFMT_) == S_IFDIR_;
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
    if (n->size >= em->block_size || n->size > EXT2_NAME_MAX * 4) {
        return -EIO; /* longer than a block: not a link this format writes */
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
        int rc = read_data(em, n, 0, buf, len);

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

static int ext2_sync(struct vfs_mount *mnt)
{
    (void)mnt;
    return 0;
}

static int ext2_prepare_unmount(struct vfs_mount *mnt)
{
    (void)mnt;
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

/* ---- the mount ---- */

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
    } else if (!(em->state & EXT2_VALID_FS) || (em->state & EXT2_ERROR_FS)) {
        readonly_why = "not unmounted cleanly";
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

        if (bb >= em->blocks || ib >= em->blocks || it >= em->blocks ||
            it + inode_table_blocks(em) > em->blocks) {
            return refuse("group descriptor");
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
    em->writable = readonly_why == 0 && !(flags & VFS_MS_RDONLY);
    if (readonly_why) {
        klog(KLOG_WARN, "ext2", "mounted read-only: %s", readonly_why);
        mnt->flags |= VFS_MS_RDONLY;
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
    ext2_lookup, 0, /* lookup_rest */
    ext2_hold,   ext2_put, ext2_getattr, 0, /* setattr */
    ext2_open,   ext2_release, ext2_read, 0, /* write */
    0,                                     /* truncate */
    0,                                     /* fsync */
    ext2_readdir, 0,                       /* create */
    0,                                     /* unlink */
    0,                                     /* mkdir */
    0,                                     /* rmdir */
    0,                                     /* rename */
    0,                                     /* link */
    ext2_readlink, ext2_statfs, ext2_sync, ext2_prepare_unmount, ext2_unmount, ext2_path,
    0, /* symlink */
};

const struct vfs_type vfs_ext2_type = {"ext2", ext2_mount, &ext2_ops};
