/*
 * ext2.c: the ext2 file system, read and written through the buffer cache;
 * the contract is in ext2.h and ext2.Readme.md.
 */

#include "ext2.h"

#include "../mm/kheap.h"

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
#define EXT2_ROOT_INO    2u
#define EXT2_NDIR        12u
#define EXT2_NBLOCKS     15u
#define EXT2_NAME_MAX    255u
#define EXT2_FAST_LINK   60u /* the bytes of `i_block`, where a shorter target is kept */

/* The bits of `i_flags` lsattr(1) sees, and those chattr(1) may change. */
#define EXT2_FL_USER_VISIBLE    0x0003DFFFu
#define EXT2_FL_USER_MODIFIABLE 0x000380FFu

#define EXT2_COMPAT_RESIZE_INODE 0x0010u
#define EXT2_COMPAT_DIR_INDEX    0x0020u
#define EXT2_RO_SPARSE_SUPER     0x0001u
#define EXT2_RO_LARGE_FILE       0x0002u
#define EXT2_INCOMPAT_FILETYPE   0x0002u

#define EXT2_VALID_FS 0x0001u
#define EXT2_ERROR_FS 0x0002u

#define S_IFMT_   0170000u
#define S_IFREG_  0100000u
#define S_IFDIR_  0040000u
#define S_IFLNK_  0120000u
#define S_IFCHR_  0020000u
#define S_IFBLK_  0060000u
#define S_IFIFO_  0010000u
#define S_IFSOCK_ 0140000u
#define S_ISGID_  0002000u

#define EXT2_FT_DIR 2u
#define EXT2_FT_MAX 7u

/* Directories climbed by `ext2_path` and by the rename check that a directory
 * is not moved below itself: a directory deeper than this has no path of
 * VFS_PATH_MAX bytes, since each level is a slash and at least one byte. */
#define PATH_DEPTH         (VFS_PATH_MAX / 2)
#define BCACHE_BLOCK_SHIFT 12 /* log2 of BCACHE_BLOCK_BYTES */

/* An inode in memory, from the heap, on its mount's list. */
struct ext2_node {
    struct ext2_node *next; /* the mount's list of nodes in memory */
    int used;
    uint32_t refs; /* holds; the last `ext2_put` frees the node */
    uint32_t ino;
    uint32_t mode;
    uint32_t uid, gid;
    uint32_t nlink;
    uint64_t size;                /* 64 bits for a regular file, 32 for the rest */
    uint32_t blocks;              /* i_blocks, in 512-byte units */
    uint32_t block[EXT2_NBLOCKS]; /* 12 direct, then single, double and triple indirect */
    uint32_t atime, mtime, ctime;
    uint32_t flags;      /* i_flags, which the VFS enforces in part */
    uint32_t generation; /* i_generation, new for each inode made */
    uint32_t add_from;   /* a directory's offset of the block where the last entry went */
    int unlinked;        /* no name left; the inode and its blocks go at the last put */
};

/* `write_bytes`'s ways to write on a mount with `sync` or `dirsync`
 * (`em_strict`): through to the medium; through to the device without its
 * flush, which a later flush covers; and through to the medium on any
 * mount, for the superblock's counts and state. 0, and the first two on any
 * other mount, leave the bytes dirty in the cache for `sync`. */
#define WRITE_THROUGH   1
#define WRITE_UNFLUSHED 2
#define WRITE_DURABLE   3

/* The kinds of a mount's `batch`. */
#define BATCH_NONE  0
#define BATCH_ALLOC 1
#define BATCH_FREE  2

/*
 * The file-write batch's limits: the bytes it takes, its data and metadata
 * images, and the inodes it holds. Then its stages, drained in order: the
 * allocation bitmaps and descriptors, the data, the pointer blocks a level
 * at a time from those that name data up (WB_LEAF to WB_LEAF + 2), the
 * superblock's `large_file` flag, and the inode table blocks.
 */
#define EXT2_WB_BYTES (1024u * 1024u)
#define EXT2_WB_DATA  256
#define EXT2_WB_META  32
#define EXT2_WB_NODES 16
#define WB_ALLOC      0
#define WB_DATA       1
#define WB_LEAF       2
#define WB_FEATURE    5
#define WB_INODE      6

/* The batch's private copy of a cache block it changes: the block, its
 * stage, whether the batch has changed it, and its page. */
struct ext2_image {
    uint32_t block;
    int stage;
    int dirty;
    uint8_t *data;
};

/* An inode the batch holds, with its size when the batch took it or at the
 * last drain. */
struct ext2_pending {
    struct ext2_node *node;
    uint64_t committed_size;
};

/*
 * The file-write batch a mount keeps across calls: the images of the blocks
 * its writes change, `datas` and `metas` of them in use, and the `count`
 * inodes it holds; the bytes accepted since the last drain; the drains done
 * (`generation`); the stage a drain is at, where a failed one resumes; and
 * the first failure's errno, stage, block and generation, kept for the
 * mount's life.
 */
struct ext2_writeback {
    struct ext2_image data[EXT2_WB_DATA];
    struct ext2_image meta[EXT2_WB_META];
    struct ext2_pending nodes[EXT2_WB_NODES];
    /* One heap page per buffer. A single run of 288 pages is more than a
     * reserve run of the heap can give, so it would have to come from the
     * ordinary region in one piece, and after the cache has moved much data
     * that region may have no such gap although memory is free, which would
     * fail a writable mount with ENOMEM. */
    uint8_t *slot[EXT2_WB_DATA + EXT2_WB_META];
    int datas, metas, count;
    uint32_t bytes, generation;
    int stage, draining, error, error_stage;
    uint32_t error_block, error_generation;
};

/* The directories whose last lookup a mount remembers, enough for the walk
 * of a path eight directories deep. */
#define EXT2_LOOKUP_HINTS 8

/* A mounted volume, from the heap, at `mnt->priv`. */
struct ext2_mount {
    int used;
    block_device_t *dev;
    struct vfs_mount *mnt;
    uint32_t block_size;
    uint32_t block_shift; /* log2 of the block size: the kernel has no 64-bit division */
    uint64_t file_limit;  /* the largest regular file the pointers and i_blocks address */
    /* The superblock's block and inode counts, first data block, blocks and
     * inodes per group, inode size and first unreserved inode; the groups. */
    uint32_t blocks, inodes, first_data, bpg, ipg, groups, inode_size, first_ino;
    uint32_t reserved_gdt; /* blocks kept after each descriptor table for a grow */
    uint32_t r_blocks;     /* blocks only a privileged caller may take */
    uint32_t compat, incompat, ro_compat;
    uint32_t state; /* s_state: EXT2_VALID_FS and EXT2_ERROR_FS */
    /* The generation the next inode made gets, counted from the mount's
     * second, so a number reused for another file has another generation. */
    uint32_t next_generation;
    int writable;
    /* The free counts, kept equal to the sums of the descriptors'. */
    uint64_t free_blocks, free_inodes;
    int valid_cleared; /* the superblock's valid bit was cleared by this mount */
    /* Why the volume may only be read, which a remount cannot change, or
     * NULL for a volume read-only because it was asked to be. */
    const char *readonly_why;
    /* The file-write batch, set up when the mount becomes writable. */
    struct ext2_writeback *wb;
    /* The batch of metadata writes in progress, or BATCH_NONE: the bitmap
     * bytes, descriptors and pointer slots it touches are marked dirty and
     * written together at its end. `BATCH_ALLOC` puts the bitmaps and
     * descriptors first, so no pointer reaches the disk before the bit of
     * the block it names; `BATCH_FREE`, which `punch` opens, puts them after
     * the pointers, so no pointer on the disk names a block whose bit is
     * clear. `meta` is the bitmap and descriptor blocks noted so far,
     * `metas` how many; a block past the table is written through at once
     * in an allocation, and in a free the table is emptied by a sync first,
     * which keeps the order. No caller opens an allocation batch. */
    int batch;
    uint64_t meta[16];
    int metas;
    uint32_t alloc_hint;     /* the block after the last one given out */
    uint8_t *gd;             /* the group descriptors, `groups * 32` bytes, from the heap */
    struct ext2_node *nodes; /* the nodes in memory, from the heap */
    /* Where the last lookups found their names, one slot for each of the
     * last EXT2_LOOKUP_HINTS directories, since a path's walk looks up a
     * name in every directory on the way: the directory's inode and the
     * block where the next lookup in it starts. `lookup_next` is the slot a
     * directory without one takes. */
    struct {
        uint32_t dir, block;
    } lookup_hint[EXT2_LOOKUP_HINTS];
    int lookup_next;
};

/** A new node from the heap, zeroed, on the mount's list, or NULL when the
 * heap has none. Every node a mount has in memory is on the list, so an
 * unmount frees what is left. */
static struct ext2_node *node_new(struct ext2_mount *em)
{
    struct ext2_node *n = kzalloc(sizeof(*n));

    if (n) {
        n->used = 1;
        n->next = em->nodes;
        em->nodes = n;
    }
    return n;
}

/** Take node `n` off the mount's list and free it. */
static void node_drop(struct ext2_mount *em, struct ext2_node *n)
{
    struct ext2_node **link = &em->nodes;

    while (*link && *link != n) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = n->next;
    }
    kfree(n);
}

/* ---- little-endian fields ---- */

/** The little-endian 16-bit field at `p`. */
static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/** The little-endian 32-bit field at `p`. */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** The driver's record of mount `mnt`. */
static struct ext2_mount *em_of(struct vfs_mount *mnt)
{
    return mnt->priv;
}

static int writeback_drain(struct ext2_mount *em);

/** Give back a write batch and every buffer page it holds; NULL is nothing. */
static void writeback_free(struct ext2_writeback *wb)
{
    int i;

    if (!wb) {
        return;
    }
    for (i = 0; i < EXT2_WB_DATA + EXT2_WB_META; i++) {
        if (wb->slot[i]) {
            kfree_pages(wb->slot[i], 1);
        }
    }
    kfree(wb);
}

static int writeback_error(struct ext2_mount *em, int rc, uint32_t block);

/** The batch's image of block `block`, metadata or data, or NULL; the cache
 * is neither read nor changed. */
static struct ext2_image *writeback_find(struct ext2_mount *em, uint32_t block)
{
    struct ext2_writeback *wb = em->wb;
    int i;

    if (!wb) {
        return 0;
    }
    for (i = 0; i < wb->metas; i++) {
        if (wb->meta[i].block == block) {
            return &wb->meta[i];
        }
    }
    for (i = 0; i < wb->datas; i++) {
        if (wb->data[i].block == block) {
            return &wb->data[i];
        }
    }
    return 0;
}

/* ---- bytes off the volume, through the cache ---- */

/**
 * Copy `len` bytes at byte offset `off` of the volume into `dst`, one cache
 * block at a time, from the batch's image of a block when it has one, since
 * the batch's writes are not in the cache yet. The offset is 64 bits, so the
 * volume's size is the block layer's business and not this reader's. 0, or
 * -EIO.
 */
static int read_bytes(struct ext2_mount *em, uint64_t off, void *dst, uint32_t len)
{
    uint8_t *out = dst;

    while (len > 0) {
        uint64_t blk = off >> BCACHE_BLOCK_SHIFT;
        uint32_t in = (uint32_t)off & (BCACHE_BLOCK_BYTES - 1);
        uint32_t take = BCACHE_BLOCK_BYTES - in;
        struct bbuf *b;
        struct ext2_image *pending;

        if (take > len) {
            take = len;
        }
        pending = writeback_find(em, (uint32_t)blk);
        if (pending) {
            k_memcpy(out, pending->data + in, take);
            out += take;
            off += take;
            len -= take;
            continue;
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

/** Whether `g`, which is not 0, is a power of `base`, 1 included. */
static int is_power(uint32_t g, uint32_t base)
{
    while (g % base == 0) {
        g /= base;
    }
    return g == 1;
}

/** Whether group `g` keeps a copy of the superblock and the descriptors: with
 * `sparse_super`, groups 0 and 1 and the powers of 3, 5 and 7, each power
 * of one base alone, so 15 has none; without it, every group. */
static int has_super(const struct ext2_mount *em, uint32_t g)
{
    if (!(em->ro_compat & EXT2_RO_SPARSE_SUPER) || g <= 1) {
        return 1;
    }
    return is_power(g, 3) || is_power(g, 5) || is_power(g, 7);
}

/** The blocks of the descriptor table, which follows each superblock copy. */
static uint32_t gd_blocks(const struct ext2_mount *em)
{
    return (em->groups * 32 + em->block_size - 1) / em->block_size;
}

/** The blocks of a group's inode table. */
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
 * table found through its group's descriptor. The first 128 bytes of the
 * slot are read; the owner and group have their high halves at bytes 120
 * and 122, and a regular file's size has its high word at byte 108. 0, or
 * -EIO.
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
    n->flags = le32(raw + 32);
    n->generation = le32(raw + 100);
    for (i = 0; i < (int)EXT2_NBLOCKS; i++) {
        n->block[i] = le32(raw + 40 + i * 4);
    }
    if ((n->mode & S_IFMT_) == S_IFREG_) {
        n->size |= (uint64_t)le32(raw + 108) << 32;
    }
    return 0;
}

/* ---- the nodes in memory ---- */

/**
 * The node for inode `ino`, held once more if the mount has it in memory
 * and loaded into a new record otherwise; NULL with `*rc` set, -ENOMEM when
 * the heap has no room or -EIO when the inode cannot be read. It runs under
 * the mount's namespace lock and puts the new record on the list before the
 * load, which sleeps in the cache, so that a second lookup meanwhile finds
 * the record taken and does not take another.
 */
static struct ext2_node *node_for(struct ext2_mount *em, uint32_t ino, int *rc)
{
    struct ext2_node *n;

    vfs_ns_lock(em->mnt);
    *rc = 0;
    for (n = em->nodes; n; n = n->next) {
        if (n->ino == ino) {
            n->refs++;
            vfs_ns_unlock(em->mnt);
            return n;
        }
    }
    n = node_new(em);
    if (!n) {
        klog(KLOG_WARN, "ext2", "node: no memory");
        *rc = -ENOMEM;
        vfs_ns_unlock(em->mnt);
        return 0;
    }
    n->ino = ino;
    *rc = inode_load(em, ino, n);
    if (*rc != 0) {
        node_drop(em, n);
        vfs_ns_unlock(em->mnt);
        return 0;
    }
    n->refs = 1;
    vfs_ns_unlock(em->mnt);
    return n;
}

/** The `hold` of `vfs_ops`: one more hold on `node`. */
static void ext2_hold(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    ((struct ext2_node *)node)->refs++;
}

static int release_inode(struct ext2_mount *em, struct ext2_node *n);
static int sb_store(struct ext2_mount *em);
static int meta_deferred(struct ext2_mount *em, uint64_t at);
static int meta_noted(const struct ext2_mount *em, uint64_t blk);

static void mark_error(struct ext2_mount *em);

/**
 * The `put` of `vfs_ops`: drop a hold on `node`, and free the node with the
 * last, under the mount's namespace lock, so that an operation holding a
 * pointer to the node across a sleep finds it as it left it.
 *
 * An unlinked inode keeps its blocks until its last holder goes, which is
 * what makes an open unlinked file work, and a put of one drains the batch
 * first, so no block is freed while an older write is pending. On a writable
 * mount its last put frees the inode; a crash before this leaves an orphan
 * for e2fsck, and so does a free that fails, which sets the error bit with
 * the record `ext2: inode N: not freed at its last put`.
 */
static void ext2_put(struct vfs_mount *mnt, void *node)
{
    struct ext2_node *n = node;

    vfs_ns_lock(mnt);
    if (n->unlinked && em_of(mnt)->wb && em_of(mnt)->wb->count) {
        (void)writeback_drain(em_of(mnt));
    }
    if (n->refs > 0) {
        n->refs--;
    }
    if (n->refs == 0) {
        if (n->unlinked && em_of(mnt)->writable && !(mnt->flags & VFS_MS_RDONLY) &&
            release_inode(em_of(mnt), n) != 0) {
            klog(KLOG_WARN, "ext2", "inode %d: not freed at its last put", (int)n->ino);
            mark_error(em_of(mnt));
        }
        node_drop(em_of(mnt), n);
    }
    vfs_ns_unlock(mnt);
}

/* ---- block mapping ---- */

/**
 * Block `index` of `n`'s data, into `*out`: 0 for a hole, or the pointer
 * through the direct, indirect, double and triple levels, each pointer block
 * checked before it is read. 0, -EFBIG past the triple indirect's reach, or
 * -EIO, with the error bit set and a record for a block out of range.
 */
static int block_at(struct ext2_mount *em, const struct ext2_node *n, uint32_t index, uint32_t *out)
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
                mark_error(em);
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
        mark_error(em);
        return -EIO;
    }
    *out = b;
    return 0;
}

/** Copy `len` bytes of `n`'s data at `off` into `dst`, a hole read as zeros;
 * 0, or the errors of `block_at`. */
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

/* A copy of one block of a directory, for a scan that reads all of a block's
 * entries: the block is read once and not twice for each entry. `data` is
 * NULL for a scan without one, and `index` is the copy's block of the
 * directory, or -1 before the first read. */
struct ext2_window {
    uint8_t *data;
    int64_t index;
};

/** A window for a scan of `em`'s directories; one with no copy, which
 * reads each entry from the cache, when the heap has no room. */
static void window_open(struct ext2_mount *em, struct ext2_window *w)
{
    w->data = kmalloc(em->block_size);
    w->index = -1;
}

/** Free the window's copy. */
static void window_close(struct ext2_window *w)
{
    kfree(w->data);
    w->data = 0;
}

/**
 * Copy `len` bytes at byte `at` of directory `d` into `dst`: through window
 * `w` when it has a copy and the bytes lie inside one block, which a valid
 * entry's do, and by `read_data` otherwise, for a scan without a copy or a
 * corrupt record whose header runs past its block. 0, or -EIO.
 */
static int dir_bytes(struct ext2_mount *em, const struct ext2_node *d, struct ext2_window *w,
                     uint32_t at, void *dst, uint32_t len)
{
    int64_t index = (int64_t)(at >> em->block_shift);
    int rc;

    if (!w || !w->data || (at & (em->block_size - 1)) + len > em->block_size) {
        return read_data(em, d, at, dst, len);
    }
    if (w->index != index) {
        w->index = -1;
        rc = read_data(em, d, (uint64_t)index << em->block_shift, w->data, em->block_size);
        if (rc != 0) {
            return rc;
        }
        w->index = index;
    }
    k_memcpy(dst, w->data + (at & (em->block_size - 1)), len);
    return 0;
}

/**
 * The entry at byte `*cursor` of directory `d`, the cursor moved past it: 1
 * with `sc` filled, 0 at the directory's end, or -EIO. A malformed entry, a
 * `rec_len` below 8 or not a multiple of 4, a record past its block or the
 * directory's size, a name past its record or an inode past the count, is
 * -EIO with the error bit set and a record. An entry with inode 0 is
 * returned with that inode and no name, and the caller skips it. With window
 * `w` the entry is read from a copy of its block, which the caller must not
 * write while it scans.
 */
static int dir_scan_in(struct ext2_mount *em, const struct ext2_node *d, struct ext2_window *w,
                       uint32_t *cursor, struct ext2_scan *sc)
{
    uint8_t head[8];
    uint32_t at = *cursor;
    uint32_t in;
    int rc;

    if (at >= d->size) {
        return 0;
    }
    in = at % em->block_size;
    rc = dir_bytes(em, d, w, at, head, 8);
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
        mark_error(em);
        return -EIO;
    }
    if (sc->ino != 0) {
        rc = dir_bytes(em, d, w, at + 8, sc->name, sc->name_len);
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

/** `dir_scan_in` without a window: each entry read through the cache. */
static int dir_scan(struct ext2_mount *em, const struct ext2_node *d, uint32_t *cursor,
                    struct ext2_scan *sc)
{
    return dir_scan_in(em, d, 0, cursor, sc);
}

/** Whether the strings `a` and `b` are equal. */
static int names_equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** The lookup-hint slot of directory inode `dir`, or -1 when it has none. */
static int lookup_slot(const struct ext2_mount *em, uint32_t dir)
{
    int i;

    for (i = 0; i < EXT2_LOOKUP_HINTS; i++) {
        if (em->lookup_hint[i].dir == dir) {
            return i;
        }
    }
    return -1;
}

/**
 * The `lookup` of `vfs_ops`: `name` in directory `dir`, the mount's root when
 * it is NULL. 0 with the node held, -ENOTDIR, -ENOENT, -ENOMEM, or -EIO.
 *
 * A directory of this format has no index, so the lookup reads entries
 * until it finds the name. It starts at the block where the last lookup in
 * the same directory found its name and wraps around to it, as Linux's
 * ext2 does: `ls` and `find` look up the names of a directory in the order
 * it lists them, so each is found near the one before, and a directory of
 * n names is listed in time proportional to n and not to n squared. A block
 * starts an entry whatever was written since, since no entry crosses one.
 */
static int ext2_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    uint32_t start = 0, cursor, limit, at;
    struct ext2_window w;
    int pass, slot;

    if ((d->mode & S_IFMT_) != S_IFDIR_) {
        return -ENOTDIR;
    }
    if (name[0] == '.' && name[1] == '\0') {
        d->refs++;
        *node = d;
        return 0;
    }
    slot = lookup_slot(em, d->ino);
    if (slot >= 0 && ((uint64_t)em->lookup_hint[slot].block << em->block_shift) < d->size) {
        start = em->lookup_hint[slot].block << em->block_shift;
    }
    window_open(em, &w);
    for (pass = 0; pass < 2; pass++) {
        cursor = pass == 0 ? start : 0;
        limit = pass == 0 ? (uint32_t)d->size : start;
        while (cursor < limit) {
            struct ext2_scan sc;
            int got;

            at = cursor;
            got = dir_scan_in(em, d, &w, &cursor, &sc);
            if (got < 0) {
                window_close(&w);
                return got;
            }
            if (got == 0) {
                break;
            }
            if (sc.ino != 0 && names_equal(sc.name, name)) {
                struct ext2_node *n;
                int rc;

                if (slot < 0) {
                    slot = em->lookup_next;
                    em->lookup_next = (em->lookup_next + 1) % EXT2_LOOKUP_HINTS;
                    em->lookup_hint[slot].dir = d->ino;
                }
                em->lookup_hint[slot].block = at >> em->block_shift;
                window_close(&w);
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
    window_close(&w);
    return -ENOENT;
}

/* ---- attributes ---- */

/**
 * The `getattr` of `vfs_ops`: the inode's fields as the node has them, with
 * `i_blocks`, the generation and the flags lsattr sees, under the mount's
 * namespace lock. A device's numbers are in the old encoding in the first
 * pointer word, major and minor a byte each, or in the new one in the second
 * when the first is zero, and `rdev` packs them as the kernel does. Always
 * 0.
 */
static int ext2_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    struct ext2_node *n = node;

    vfs_ns_lock(mnt);
    k_memset(st, 0, sizeof(*st));
    st->mode = n->mode;
    st->uid = n->uid;
    st->gid = n->gid;
    st->nlink = n->nlink;
    st->size = n->size;
    st->ino = n->ino;
    if ((n->mode & S_IFMT_) == S_IFCHR_ || (n->mode & S_IFMT_) == S_IFBLK_) {
        uint32_t major, minor;

        if (n->block[0] != 0) {
            major = (n->block[0] >> 8) & 0xFFu;
            minor = n->block[0] & 0xFFu;
        } else {
            uint32_t v = n->block[1];

            major = (v >> 8) & 0xFFFu;
            minor = (v & 0xFFu) | ((v >> 12) & 0xFFF00u);
        }
        st->rdev = (major << 8) | (minor & 0xFFu) | ((minor & 0xFFF00u) << 12);
    }
    st->atime_sec = n->atime;
    st->mtime_sec = n->mtime;
    st->ctime_sec = n->ctime;
    st->blocks = n->blocks;
    st->blocks_known = 1;
    st->flags = n->flags & EXT2_FL_USER_VISIBLE;
    st->generation = n->generation;
    st->flags_known = 1;
    vfs_ns_unlock(mnt);
    return 0;
}

/** The `open` of `vfs_ops`: the handle is the node, held once more; `flags`
 * is not read. A device node is -EACCES, since devices are opened in
 * `/dev`. */
static int ext2_open(struct vfs_mount *mnt, void *node, int flags, void **handle)
{
    struct ext2_node *n = node;
    uint32_t type = n->mode & S_IFMT_;

    (void)mnt;
    (void)flags;
    if (type == S_IFCHR_ || type == S_IFBLK_) {
        return -EACCES;
    }
    n->refs++;
    *handle = node;
    return 0;
}

/** The `release` of `vfs_ops`, at a description's last close: the batch
 * drained, a failure kept for the next `fsync`, and the hold `ext2_open`
 * took put, under the mount's namespace lock. */
static void ext2_release(struct vfs_mount *mnt, void *handle)
{
    vfs_ns_lock(mnt);
    (void)writeback_drain(em_of(mnt));
    ext2_put(mnt, handle);
    vfs_ns_unlock(mnt);
}

/**
 * The `read` of `vfs_ops`: up to `count` bytes at `offset`, ending at the
 * size, the batch's pending writes included and a hole read as zeros, under
 * the mount's namespace lock; the access time does not change. The bytes
 * read, -EISDIR, or -EIO.
 */
static int32_t ext2_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count,
                         uint64_t offset)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *n = handle;
    int rc;

    if ((n->mode & S_IFMT_) == S_IFDIR_) {
        return -EISDIR;
    }
    vfs_ns_lock(mnt);
    if (offset >= n->size) {
        vfs_ns_unlock(mnt);
        return 0;
    }
    if (count > n->size - offset) {
        count = (uint32_t)(n->size - offset);
    }
    rc = read_data(em, n, offset, buf, count);
    vfs_ns_unlock(mnt);
    if (rc != 0) {
        return rc;
    }
    return (int32_t)count;
}

/**
 * The getdents64 type of the file-type byte a directory entry stores under
 * the filetype feature: regular file, directory, character and block
 * device, FIFO, socket and symbolic link are 1 through 7 on disk, and 0 or a
 * value past EXT2_FT_MAX is VFS_DT_UNKNOWN.
 */
static int ext2_dtype(uint32_t ft)
{
    static const unsigned char dt[EXT2_FT_MAX + 1] = {0, 8, 4, 2, 6, 1, 12, 10};

    return ft <= EXT2_FT_MAX ? dt[ft] : VFS_DT_UNKNOWN;
}

/**
 * The `readdir` of `vfs_ops`: the next entry with an inode of directory
 * `handle` at `*cursor`, a byte offset, `.` and `..` included. Its type comes
 * from the entry's file-type byte when the volume has the filetype feature,
 * and from the inode's mode otherwise, which costs a read of the inode. 1
 * with `ent` filled, 0 at the end, -ENOTDIR, -ENOENT for a directory removed
 * while held, or -EIO.
 */
static int ext2_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor,
                        struct vfs_dirent *ent)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *d = handle;

    if ((d->mode & S_IFMT_) != S_IFDIR_) {
        return -ENOTDIR;
    }
    if (d->unlinked) {
        return -ENOENT;
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
            ent->type = ext2_dtype(sc.type);
        } else {
            struct ext2_node probe;
            int rc = inode_load(em, sc.ino, &probe);

            if (rc != 0) {
                return rc;
            }
            ent->is_dir = (probe.mode & S_IFMT_) == S_IFDIR_;
            ent->type = (int)VFS_DT_OF(probe.mode);
        }
        return 1;
    }
}

/**
 * The `readlink` of `vfs_ops`: a symbolic link's target, from the inode's
 * pointer words when it is shorter than sixty bytes and the inode has no
 * blocks, from its first block otherwise, with a terminating NUL. Returns
 * the length; -ENAMETOOLONG when `cap` cannot hold it; -EINVAL for a node
 * that is not a link; or -EIO for a size of a block or more, a link of 60
 * bytes or more without blocks, a long link without its block, or a failed
 * read.
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
    if (n->blocks == 0) {
        uint32_t i;

        if (len >= EXT2_FAST_LINK) {
            return -EIO; /* the pointer words hold at most 59 bytes */
        }
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

/** The blocks the volume's own structures take: those before the first
 * group, each superblock copy and descriptor table, and each group's two
 * bitmaps and inode table, as Linux's ext2 counts them. */
static uint32_t overhead_blocks(const struct ext2_mount *em)
{
    uint32_t n = em->first_data + em->groups * (2 + inode_table_blocks(em));
    uint32_t g;

    for (g = 0; g < em->groups; g++) {
        if (has_super(em, g)) {
            n += 1 + gd_blocks(em);
        }
    }
    return n;
}

/**
 * The `statfs` of `vfs_ops`: the volume's size, free and reserved blocks and
 * inodes. The size leaves out the blocks of the volume's own structures
 * (`overhead_blocks`), as Linux's ext2 does under its default `bsddf`, so
 * `df` counts as used only what files and directories take, as `du` does.
 * Always 0.
 */
static int ext2_statfs(struct vfs_mount *mnt, struct vfs_statfs *st)
{
    struct ext2_mount *em = em_of(mnt);
    uint32_t overhead = overhead_blocks(em);

    st->magic = EXT2_SUPER_MAGIC;
    st->bsize = em->block_size;
    st->blocks = em->blocks > overhead ? em->blocks - overhead : 0;
    st->bfree = em->free_blocks;
    st->reserved = em->r_blocks;
    st->files = em->inodes;
    st->ffree = em->free_inodes;
    st->namelen = EXT2_NAME_MAX;
    return 0;
}

/**
 * The `sync` of `vfs_ops`: the batch drained, the device's dirty buffers
 * written, on a writable mount the superblock's free counts and state
 * written, and the disk's cache flushed, under the mount's namespace lock.
 * Returns this sync's result, with a failure recorded as the batch's error;
 * the mount's first failure is what `fsync` reports.
 */
static int ext2_sync(struct vfs_mount *mnt)
{
    struct ext2_mount *em = em_of(mnt);
    int rc;

    vfs_ns_lock(mnt);
    rc = writeback_drain(em);
    if (rc == 0) {
        rc = bcache_sync(em->dev);
    }
    if (rc == 0 && em->writable && !(mnt->flags & VFS_MS_RDONLY)) {
        rc = sb_store(em);
    }
    if (rc == 0) {
        rc = block_flush(em->dev);
    }
    if (rc != 0) {
        (void)writeback_error(em, rc, 0);
    }
    vfs_ns_unlock(mnt);
    return rc;
}

/**
 * The `prepare_unmount` of `vfs_ops`, used by a remount read-only too:
 * `ext2_sync`, and then, on a writable mount none of whose writes failed,
 * the superblock's valid bit set again when this mount cleared it and the
 * error bit is clear, and the state written to the medium. 0, or -errno; a
 * failed write of the state clears the valid bit again and is recorded as
 * the batch's error.
 */
static int ext2_prepare_unmount(struct vfs_mount *mnt)
{
    struct ext2_mount *em = em_of(mnt);
    int rc = ext2_sync(mnt);

    if (rc != 0 || !em->writable || (mnt->flags & VFS_MS_RDONLY) || mnt->write_error) {
        return rc;
    }
    if (em->valid_cleared && !(em->state & EXT2_ERROR_FS)) {
        em->state |= EXT2_VALID_FS;
    }
    rc = sb_store(em);
    if (rc != 0) {
        em->state &= ~EXT2_VALID_FS;
        return writeback_error(em, rc, 0);
    }
    em->valid_cleared = 0;
    return 0;
}

/**
 * The `abort_unmount` of `vfs_ops`: on a writable mount, the valid bit that
 * `ext2_prepare_unmount` set is cleared again in the superblock and the
 * device synced and flushed, since the unmount is not happening. A failure
 * is recorded as the batch's error. Under the mount's namespace lock.
 */
static void ext2_abort_unmount(struct vfs_mount *mnt)
{
    struct ext2_mount *em = em_of(mnt);
    struct bbuf *b;
    int rc = -EIO;

    if (!em->writable) {
        return;
    }
    vfs_ns_lock(mnt);
    em->state &= ~EXT2_VALID_FS;
    em->valid_cleared = 1;
    b = bcache_get(em->dev, 0);
    if (b) {
        b->data[1024 + 58] = (uint8_t)em->state;
        b->data[1024 + 59] = (uint8_t)(em->state >> 8);
        bcache_dirty(b);
        bcache_release(b);
        rc = bcache_sync(em->dev);
        if (rc == 0) {
            rc = block_flush(em->dev);
        }
    }
    if (rc != 0) {
        (void)writeback_error(em, rc, 0);
    }
    vfs_ns_unlock(mnt);
}

static int writeback_init(struct ext2_mount *em);
static int ext2_check_volume(struct ext2_mount *em, const char **why);

/**
 * The `remount` of `vfs_ops`: make the mount read-only or writable, by
 * VFS_MS_RDONLY in `flags`; a remount that keeps the mount as it is changes
 * nothing, and the options are ignored.
 *
 * Read-only, the volume is synced and its valid bit set, as an unmount
 * leaves it, which a node removed while still open would prevent, since its
 * inode is freed at its last put and a read-only mount frees nothing:
 * -EBUSY, as on Linux for such a file. Writable, a volume whose valid
 * bit is clear or whose error bit is set is checked as a writable mount
 * checks it, the batch is set up when the mount has none, and the valid bit
 * is cleared on the medium; a volume whose features or check keep it
 * read-only is -EROFS, and a batch the heap cannot give is -ENOMEM.
 */
static int ext2_remount(struct vfs_mount *mnt, uint32_t flags, const char *options)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *n;
    const char *why = 0;
    int rc = 0;

    (void)options;
    if (((flags & VFS_MS_RDONLY) != 0) == !em->writable) {
        return 0;
    }
    if (flags & VFS_MS_RDONLY) {
        for (n = em->nodes; n; n = n->next) {
            if (n->unlinked) {
                return -EBUSY;
            }
        }
        rc = em->writable ? ext2_prepare_unmount(mnt) : 0;
        if (rc == 0) {
            em->writable = 0;
        }
        return rc;
    }
    if (em->readonly_why) {
        return -EROFS;
    }
    vfs_ns_lock(mnt);
    if (!(em->state & EXT2_VALID_FS) || (em->state & EXT2_ERROR_FS)) {
        rc = ext2_check_volume(em, &why);
        if (rc == 0 && why) {
            em->readonly_why = why;
            rc = -EROFS;
        }
    }
    if (rc == 0 && !em->wb) {
        rc = writeback_init(em);
    } else if (rc == 0) {
        em->state &= ~EXT2_VALID_FS;
        rc = sb_store(em);
    }
    if (rc == 0) {
        em->valid_cleared = 1;
        em->writable = 1;
    }
    vfs_ns_unlock(mnt);
    return rc;
}

/** The `unmount` of `vfs_ops`: the mount's nodes, batch, descriptors and
 * record freed; a batch still pending on a removed device is recorded as a
 * failure first. */
static void ext2_unmount(struct vfs_mount *mnt)
{
    struct ext2_mount *em = em_of(mnt);

    if ((em->dev->flags & BLOCK_GONE) && em->wb && em->wb->count) {
        (void)writeback_error(em, -EIO, 0);
    }
    while (em->nodes) {
        node_drop(em, em->nodes);
    }
    mnt->priv = 0;
    writeback_free(em->wb);
    kfree(em->gd);
    kfree(em);
}

/**
 * The `path` of `vfs_ops`: the path of directory `node`, the parent by its
 * `..` entry and the name by the parent's entry that names the child's
 * inode, up to the root. 0; -ENAMETOOLONG past `cap` or past PATH_DEPTH
 * levels; -ENOTDIR; -ENOENT for a directory without `..` or not named in its
 * parent; or -EIO.
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
 * The order of writes, from ext2.Readme.md, which a mount with `sync` or
 * `dirsync` keeps on the medium (`em_strict`) and any other mount leaves to
 * the cache: an inode is freed by writing it zeroed with its deletion time
 * before its bitmap bit clears, and allocated by setting its bit before it
 * is written and named; a block is allocated in its bitmap before a pointer
 * names it, a pointer block is written before the pointer to it, and
 * `i_blocks` goes with the pointers and `i_size` last; an entry is removed
 * before the link count drops; a truncation shrinks the size first, then
 * clears pointers from the end, then frees blocks; a directory grows by a
 * zeroed block chained before the size covers it. A free of a file's blocks
 * is one batch whose order the cache keeps (`punch`), and a file's writes
 * go through the file-write batch, whose stages keep the same order
 * (`writeback_drain`).
 */

/* Store `v` at `p`, little-endian. The value is taken once: a count read
 * from the bytes being written, as `le16(gd + 12) - 1`, would otherwise be
 * read again after its low byte changed. */
#define LE16_PUT(p, v)                           \
    do {                                         \
        uint32_t put_ = (v);                     \
        (p)[0] = (uint8_t)(put_ & 0xFFu);        \
        (p)[1] = (uint8_t)((put_ >> 8) & 0xFFu); \
    } while (0)
#define LE32_PUT(p, v)                            \
    do {                                          \
        uint32_t put_ = (v);                      \
        (p)[0] = (uint8_t)(put_ & 0xFFu);         \
        (p)[1] = (uint8_t)((put_ >> 8) & 0xFFu);  \
        (p)[2] = (uint8_t)((put_ >> 16) & 0xFFu); \
        (p)[3] = (uint8_t)((put_ >> 24) & 0xFFu); \
    } while (0)

/** The real time in seconds, or 0 before the epoch or past what a 32-bit
 * ext2 time holds, 2038-01-19 03:14:07 UTC. */
static uint32_t now_seconds(void)
{
    int64_t sec = 0, nsec = 0;

    rtc_realtime(&sec, &nsec);
    if (sec < 0 || sec > 0x7FFFFFFF) {
        return 0;
    }
    return (uint32_t)sec;
}

/**
 * Whether the mount puts each metadata write, and each stage of a file's
 * write, on the medium before the next, in the order this driver keeps, as
 * a mount with `sync` or `dirsync` requires. Any other mount writes back as
 * Linux's ext2 does: what an operation changes stays dirty in the buffer
 * cache, and reaches the disk, in no promised order, at `sync`, `fsync`,
 * `syncfs`, unmount, the periodic sync of `update` and the cache's eviction;
 * a stop of the kernel or of the power before then can leave the volume in
 * a state the order does not promise, which `e2fsck` repairs.
 */
static int em_strict(const struct ext2_mount *em)
{
    return em->mnt && (em->mnt->flags & (VFS_MS_SYNCHRONOUS | VFS_MS_DIRSYNC)) != 0;
}

/** A barrier of the order: `bcache_sync` of the mount's device on a strict
 * mount, and nothing on one that writes back, whose order is `sync`'s. 0,
 * or -errno. */
static int em_sync(struct ext2_mount *em)
{
    return em_strict(em) ? bcache_sync(em->dev) : 0;
}

/** On a mount that writes back, write the device's dirty buffers in runs,
 * without the disk's flush, once the cache is heavy with them, as Linux
 * makes a writer write back when its dirty pages pass a share of memory: a
 * block left to an eviction goes to the disk alone. The operation that
 * called it is done in the cache, so a failed write is not its failure; the
 * cache records it against the device, as it records an eviction's. */
static void em_balance(struct ext2_mount *em)
{
    if (!em_strict(em) && bcache_dirty_heavy()) {
        (void)bcache_sync_unflushed(em->dev);
    }
}

/**
 * Copy `len` bytes from `src`, or zeros for NULL, to byte offset `off` of
 * the volume, written the way `sync` says (WRITE_THROUGH and the rest), or
 * left dirty with 0. Inside a batch a write left to the cache is dirtied in
 * the batch's order: first when its block is noted and the batch allocates,
 * or when it is not noted and the batch frees, and after otherwise. A whole
 * block is not read first, since nothing of it survives. 0, or -errno.
 */
static int write_bytes(struct ext2_mount *em, uint64_t off, const void *src, uint32_t len, int sync)
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
        if (sync == WRITE_DURABLE || (sync == WRITE_THROUGH && em_strict(em))) {
            rc = bcache_write(b);
        } else if (sync && em_strict(em)) {
            rc = bcache_write_unflushed(b);
        } else if (em->batch) {
            int first = meta_noted(em, blk) == (em->batch == BATCH_ALLOC);

            bcache_dirty_ordered(b, first ? BCACHE_ORDER_FIRST : BCACHE_ORDER_AFTER);
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

/** Drain the batch before a change of attributes or of the namespace, so
 * that no block is freed or reused while an older write is pending: 0,
 * -EROFS on a read-only mount, or the drain's -errno. The lock it takes is
 * let go again; the caller holds it for the change. */
static int assert_writable(struct vfs_mount *mnt, struct ext2_mount *em)
{
    int rc;

    vfs_ns_lock(mnt);
    rc = writeback_drain(em);
    if (rc == 0 && (!em->writable || (mnt->flags & VFS_MS_RDONLY))) {
        rc = -EROFS;
    }
    vfs_ns_unlock(mnt);
    return rc;
}

/** Panic unless the calling process holds the mount's namespace lock, which
 * every change to the volume is made under. */
static void assert_locked(struct vfs_mount *mnt)
{
    int me = current_proc ? (int)current_proc->pid : 0;

    if (mnt->ns_holder != me || mnt->ns_depth == 0) {
        k_panic("ext2: a mutation outside the mount's lock");
    }
}

/** The byte offset on the volume of inode `ino`'s slot, found through its
 * group's descriptor. */
static uint64_t inode_offset(const struct ext2_mount *em, uint32_t ino)
{
    uint32_t g = (ino - 1) / em->ipg;
    uint32_t index = (ino - 1) % em->ipg;
    uint32_t table = le32(em->gd + g * 32 + 8);

    return (uint64_t)table * em->block_size + (uint64_t)index * em->inode_size;
}

/**
 * Set the superblock's `large_file` read-only-compatible flag, written
 * through. e2fsck expects it once a regular file reaches 2 GiB, and the
 * flag is set before the inode that needs it is stored, so a stop between
 * the two leaves a volume e2fsck finds no fault in. 0, or -errno.
 */
static int large_file_flag(struct ext2_mount *em)
{
    uint8_t raw[4];

    em->ro_compat |= EXT2_RO_LARGE_FILE;
    LE32_PUT(raw, em->ro_compat);
    return write_bytes(em, 1024 + 100, raw, 4, 1);
}

/** Encode the fields of `n` this driver keeps into the 128-byte inode `raw`,
 * leaving its other bytes as they are; the deletion time is cleared. */
static void inode_encode(const struct ext2_node *n, uint8_t *raw)
{
    int i;

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
    LE32_PUT(raw + 32, n->flags);
    for (i = 0; i < (int)EXT2_NBLOCKS; i++) {
        LE32_PUT(raw + 40 + i * 4, n->block[i]);
    }
    LE32_PUT(raw + 100, n->generation);
    LE16_PUT(raw + 120, (n->uid >> 16) & 0xFFFFu);
    LE16_PUT(raw + 122, (n->gid >> 16) & 0xFFFFu);
    if ((n->mode & S_IFMT_) == S_IFREG_) {
        LE32_PUT(raw + 108, (uint32_t)(n->size >> 32));
    }
}

/**
 * Write inode `n` in the way `how` says, over the bytes its slot has, or
 * over zeros when `fresh`, for an inode just allocated: a freed inode's
 * slot keeps what it had, and the encoder writes only the fields this
 * driver keeps. Inside a batch the write is left for the batch's end. A
 * regular file of 2 GiB or more sets `large_file` first. A store while the
 * file-write batch holds inodes is a panic, since it must come after their
 * drain. 0, or -errno.
 */
static int inode_write(struct ext2_mount *em, const struct ext2_node *n, int fresh, int how)
{
    uint8_t raw[128];
    uint64_t at = inode_offset(em, n->ino);
    int rc;

    if (em->wb && em->wb->count) {
        k_panic("ext2: inode store before writeback drain");
    }
    if (fresh) {
        k_memset(raw, 0, sizeof(raw));
    } else {
        rc = read_bytes(em, at, raw, sizeof(raw));
        if (rc != 0) {
            return rc;
        }
    }
    inode_encode(n, raw);
    if ((n->mode & S_IFMT_) == S_IFREG_ && n->size >= 0x80000000u &&
        !(em->ro_compat & EXT2_RO_LARGE_FILE)) {
        rc = large_file_flag(em);
        if (rc != 0) {
            return rc;
        }
    }
    return write_bytes(em, at, raw, sizeof(raw), em->batch ? 0 : how);
}

/** Write inode `n` as `inode_write` does with WRITE_THROUGH: the fields this
 * driver keeps, over the 128 bytes the slot has, so that what it does not
 * keep stays as it was. */
static int inode_store(struct ext2_mount *em, const struct ext2_node *n)
{
    return inode_write(em, n, 0, WRITE_THROUGH);
}

/**
 * Write a directory's times into the cache, for the next sync to put on
 * the disk: after an entry is added or removed only the times are left,
 * and a crash that loses them loses nothing else, since every inode of
 * the block goes out whole with each write through of it. 0, or -errno.
 */
static int inode_times_later(struct ext2_mount *em, const struct ext2_node *d)
{
    return inode_write(em, d, 0, 0);
}

/** Write the primary descriptor of group `g` with WRITE_THROUGH, or leave it
 * for the batch to write when one is open; the backup tables are not
 * written. 0, or -errno. */
static int gd_store(struct ext2_mount *em, uint32_t g)
{
    uint64_t at = (uint64_t)(em->first_data + 1) * em->block_size + (uint64_t)g * 32;

    return write_bytes(em, at, em->gd + g * 32, 32, !meta_deferred(em, at));
}

/**
 * Whether a write of a bitmap or descriptor at byte `at` may wait for the
 * end of the batch, its block noted once: only inside a batch, and only
 * while the table of noted blocks has room. A free batch whose table is full
 * empties it with a sync first, which keeps the order; an allocation batch
 * writes the block at once instead.
 */
static int meta_deferred(struct ext2_mount *em, uint64_t at)
{
    uint64_t blk = at >> BCACHE_BLOCK_SHIFT;
    int i;

    if (!em->batch) {
        return 0;
    }
    for (i = 0; i < em->metas; i++) {
        if (em->meta[i] == blk) {
            return 1;
        }
    }
    if (em->metas == (int)(sizeof(em->meta) / sizeof(em->meta[0]))) {
        if (em->batch != BATCH_FREE || em_sync(em) != 0) {
            return 0;
        }
        em->metas = 0;
    }
    em->meta[em->metas++] = blk;
    return 1;
}

/** Whether `blk` is one of the bitmap and descriptor blocks the batch has
 * noted to write first. */
static int meta_noted(const struct ext2_mount *em, uint64_t blk)
{
    int i;

    for (i = 0; i < em->metas; i++) {
        if (em->meta[i] == blk) {
            return 1;
        }
    }
    return 0;
}

/**
 * The end of a batch. On a mount with `sync` or `dirsync` an allocation's
 * noted bitmap and descriptor blocks are written first, then everything
 * else the write left dirty, data and pointer blocks, so that no pointer
 * reaches the disk before the bit of the block it names, and a free's sync
 * writes its pointer blocks and inode before its bitmaps and descriptors;
 * on any other mount the blocks stay dirty for the sync, and the cache is
 * balanced (`em_balance`). The cache keeps each order while the batch is
 * open: an allocation's noted blocks are dirtied `first` and everything else
 * it dirties `after`, a free's the other way round, so an eviction under
 * pressure writes the blocks that must go first before the one it chose,
 * and a sync does the same. 0, or -errno.
 */
static int batch_end(struct ext2_mount *em)
{
    int kind = em->batch;
    int i, rc = 0;

    em->batch = BATCH_NONE;
    for (i = 0; kind == BATCH_ALLOC && em_strict(em) && i < em->metas && rc == 0; i++) {
        struct bbuf *b = bcache_get(em->dev, em->meta[i]);

        if (!b) {
            rc = -EIO;
            break;
        }
        rc = bcache_write(b);
        bcache_release(b);
    }
    em->metas = 0;
    if (rc == 0) {
        rc = em_sync(em);
    }
    if (rc == 0) {
        em_balance(em);
    }
    return rc;
}

/** Record a fault met while reading: the error bit set, and written when the
 * mount writes, so that an orderly unmount does not mark the volume valid
 * and the next mount checks it. With file writes pending it is recorded as
 * the batch's failure instead, which makes the mount read-only. */
static void mark_error(struct ext2_mount *em)
{
    if (em->state & EXT2_ERROR_FS) {
        return;
    }
    em->state |= EXT2_ERROR_FS;
    if (em->wb && em->wb->count) {
        (void)writeback_error(em, -EIO, 0);
    } else if (em->writable) {
        (void)sb_store(em);
    }
}

/** Write the superblock's free counts and its state to the medium, on any
 * mount (WRITE_DURABLE); 0, or -errno. */
static int sb_store(struct ext2_mount *em)
{
    uint8_t raw[8];
    int rc;

    LE32_PUT(raw, (uint32_t)em->free_blocks);
    LE32_PUT(raw + 4, (uint32_t)em->free_inodes);
    rc = write_bytes(em, 1024 + 12, raw, 8, WRITE_DURABLE);
    if (rc != 0) {
        return rc;
    }
    LE16_PUT(raw, em->state);
    return write_bytes(em, 1024 + 58, raw, 2, WRITE_DURABLE);
}

/**
 * The first clear bit at or after `from` and before `count` of the bitmap
 * in block `bitmap_block`, into `*out`, or `count` when every one is set.
 * The bitmap is read 64 bytes at a time and a byte with every bit set is
 * passed over whole, so an allocation in a group whose first part is full
 * does not read each of its bits through the cache alone. 0, or -EIO.
 */
static int bit_first_clear(struct ext2_mount *em, uint32_t bitmap_block, uint32_t from,
                           uint32_t count, uint32_t *out)
{
    uint8_t chunk[64];
    uint32_t i = from;

    while (i < count) {
        uint32_t first = i / 8;
        uint32_t take = (count - 1) / 8 - first + 1;
        int rc;

        if (take > sizeof(chunk)) {
            take = sizeof(chunk);
        }
        rc = read_bytes(em, (uint64_t)bitmap_block * em->block_size + first, chunk, take);
        if (rc != 0) {
            return rc;
        }
        for (; i < count && i / 8 < first + take; i++) {
            uint8_t b = chunk[i / 8 - first];

            if (b == 0xFFu) {
                i |= 7u;
            } else if (!((b >> (i % 8)) & 1u)) {
                *out = i;
                return 0;
            }
        }
    }
    *out = count;
    return 0;
}

/** Set bit `index` of the bitmap in block `bitmap_block` to `value`, written
 * in the way `how` says unless a batch defers it; 0, or -errno. */
static int bit_write(struct ext2_mount *em, uint32_t bitmap_block, uint32_t index, int value,
                     int how)
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
    return write_bytes(em, at, &byte, 1, meta_deferred(em, at) ? 0 : how);
}

/** `bit_write` with WRITE_THROUGH. */
static int bit_put(struct ext2_mount *em, uint32_t bitmap_block, uint32_t index, int value)
{
    return bit_write(em, bitmap_block, index, value, WRITE_THROUGH);
}

/** The pointer blocks data index `index` can need at most: none for a
 * direct block, one per level of indirection above them. */
static uint32_t pointer_levels(const struct ext2_mount *em, uint32_t index)
{
    uint32_t per = em->block_size / 4;

    if (index < EXT2_NDIR) {
        return 0;
    }
    index -= EXT2_NDIR;
    if (index < per) {
        return 1;
    }
    if (index < per + per * per) {
        return 2;
    }
    return 3;
}

/** Whether the caller may take the blocks the superblock reserves: a
 * privileged process (`proc_privileged`). */
static int may_use_reserved(void)
{
    return current_proc && proc_privileged(current_proc);
}

/**
 * Allocate one block into `*out`, first-fit from `hint`: every group from
 * the hint's on, wrapping, and last the part of the hint's group before the
 * hint. The bit is set and the group's count lowered before the block is
 * returned, so that no pointer ever names a free block, and the block after
 * it becomes the mount's hint. 0, -ENOSPC when none is free or only the
 * reserved blocks are and the caller is not privileged, or -errno.
 */
static int block_alloc(struct ext2_mount *em, uint32_t hint, uint32_t *out)
{
    uint32_t g0, g, tried;
    uint32_t start_in;

    if (em->free_blocks == 0 || (!may_use_reserved() && em->free_blocks <= em->r_blocks)) {
        return -ENOSPC;
    }
    if (hint < em->first_data || hint >= em->blocks) {
        hint = em->first_data;
    }
    g0 = (hint - em->first_data) / em->bpg;
    start_in = (hint - em->first_data) % em->bpg;
    for (tried = 0; tried <= em->groups; tried++) {
        uint32_t i, count, base, bitmap, from;
        uint8_t *gd;
        int rc;

        g = (g0 + tried) % em->groups;
        gd = em->gd + g * 32;
        base = em->first_data + g * em->bpg;
        count = em->blocks - base < em->bpg ? em->blocks - base : em->bpg;
        bitmap = le32(gd + 0);
        from = tried == 0 ? start_in : 0;
        if (tried == em->groups) {
            count = start_in < count ? start_in : count;
        }
        if (le16(gd + 12) == 0) {
            continue;
        }
        rc = bit_first_clear(em, bitmap, from, count, &i);
        if (rc != 0) {
            return rc;
        }
        if (i >= count) {
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
    return -ENOSPC;
}

/** Free block `b`: its bit cleared and its group's count raised, as
 * `bit_put` and `gd_store` write them. 0, -EIO with a record for a block
 * that may not hold data, or -errno. */
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
 * Allocate an inode into `*out`, in the group of inode `parent_ino` when it
 * has one free, else in the next group that has, wrapping; an inode below
 * `s_first_ino` is never given. The bit is set and the counts lowered, the
 * group's directory count raised for `is_dir`, before the number is
 * returned, and the caller writes the inode initialised and names it, in
 * that order. The bit and the counts have no order between them, so the
 * bitmap's write leaves the disk's cache to the flush of the descriptor's,
 * which puts both on the medium before the inode is written. 0, -ENOSPC, or
 * -errno.
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
        uint32_t base = g * em->ipg, from = 0, limit = em->ipg, i;
        int rc;

        if (le16(gd + 14) == 0 || base >= em->inodes) {
            continue;
        }
        if (base + 1 < em->first_ino) {
            from = em->first_ino - 1 - base;
        }
        if (em->inodes - base < limit) {
            limit = em->inodes - base;
        }
        rc = bit_first_clear(em, le32(gd + 4), from, limit, &i);
        if (rc != 0) {
            return rc;
        }
        if (i >= limit) {
            continue;
        }
        rc = bit_write(em, le32(gd + 4), i, 1, em->batch ? WRITE_THROUGH : WRITE_UNFLUSHED);
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
        *out = base + i + 1;
        return 0;
    }
    return -ENOSPC;
}

/** Clear the bit of inode `ino` and raise its group's free count, lowering
 * the directory count for `was_dir`, once the inode itself was written
 * freed; 0, or -errno. */
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
 * Free the blocks of `n` from data index `keep` on below the pointer
 * `*slot`, of `level` (0 for a data block) and covering the data from index
 * `base`, from the end. Each pointer is cleared, at `slot_at` on the volume
 * or, for 0, in the inode, which is stored, and then its block freed, so the
 * pointer goes to the disk before the bit; a pointer block whose children
 * are all gone goes the same way. `*emptied` says whether the pointer is now
 * 0. `n->blocks` is kept current; the caller stores the inode. 0, or -errno,
 * -EIO for a pointer block out of range.
 */
static int punch_level(struct ext2_mount *em, struct ext2_node *n, uint32_t *slot, uint64_t slot_at,
                       int level, uint32_t base, uint32_t keep, int *emptied)
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
    *slot = 0;
    if (slot_at != 0) {
        uint8_t zero[4] = {0, 0, 0, 0};

        rc = write_bytes(em, slot_at, zero, 4, !em->batch);
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

/** Free `n`'s blocks from data index `keep` on, by `punch_level`: the
 * triple, double and single indirect trees first, then the direct blocks
 * from the last. 0, or -errno. */
static int punch_from(struct ext2_mount *em, struct ext2_node *n, uint32_t keep)
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
            rc = punch_level(em, n, &n->block[EXT2_NDIR + level - 1], 0, level, lbase, keep, &gone);
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
 * Free the blocks of `n` from data index `keep` on, the pointers from the
 * end, in one batch: each pointer slot, bitmap byte and descriptor changes
 * in the cache as it is reached and is written once at the end, the
 * pointer blocks and the inode before the bitmaps and descriptors. Freed
 * one by one and written through, a shrink would cost three requests a
 * block. A failure inside the batch still writes what it marked, in the
 * same order. A device that failed a write is -EIO and not written to
 * again: the batch's blocks go to the cache first and would reach the
 * device at the sync that ends it, past the check a write through makes.
 * 0, or -errno.
 */
static int punch(struct ext2_mount *em, struct ext2_node *n, uint32_t keep)
{
    int rc, end;

    if (em->dev->flags & BLOCK_WRITE_FAILED) {
        return -EIO;
    }
    em->batch = BATCH_FREE;
    rc = punch_from(em, n, keep);
    end = batch_end(em);
    return rc != 0 ? rc : end;
}

/**
 * The block for data index `index` of `n`, allocated when missing, into
 * `*out`, with `*fresh` set for a new data block. A missing pointer block is
 * allocated and its zeros written before the pointer to it, so that the
 * pointer cannot reach the disk over what the block held before; outside a
 * batch an inode that has that pointer is stored at once. A data block's
 * pointer is written after the block is in the bitmap. `n->blocks` and the
 * inode's own pointers change in memory; the caller stores the inode with
 * them. 0, -EFBIG past the triple indirect's reach, -ENOSPC, or -errno.
 *
 * A new pointer block's zeros are written with WRITE_THROUGH rather than
 * noted with the bitmaps to go first in a batch, since the pointers the
 * block is about to hold would then be first too and could reach the disk,
 * under eviction, before the bitmap that allocates the blocks they name.
 * That is one write per new pointer block.
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
                    rc = write_bytes(em, slot_at, raw, 4, !em->batch);
                    if (rc != 0) {
                        return rc;
                    }
                } else if (!em->batch) {
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
            rc = write_bytes(em, slot_at, raw, 4, !em->batch);
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

/**
 * The batch's image of metadata block `block` at stage `stage`: the one it
 * has, or a new one in the next free page, zeroed with `blank` and a copy of
 * the cache's block otherwise, not yet marked changed. NULL with `*rc` set:
 * -EIO for an image the batch has at another stage or a failed read, and
 * -ENOMEM for a full table. A piece that fails gives back the new images it
 * took (`writeback_piece`); an image the batch had is not changed here.
 */
static struct ext2_image *writeback_meta(struct ext2_mount *em, uint32_t block, int stage,
                                         int blank, int *rc)
{
    struct ext2_writeback *wb = em->wb;
    struct ext2_image *im;
    struct bbuf *b;
    int i;

    for (i = 0; i < wb->metas; i++) {
        im = &wb->meta[i];
        if (im->block == block) {
            if (im->stage != stage) {
                *rc = -EIO;
                return 0;
            }
            return im;
        }
    }
    if (wb->metas == EXT2_WB_META) {
        *rc = -ENOMEM;
        return 0;
    }
    im = &wb->meta[wb->metas];
    im->data = wb->slot[EXT2_WB_DATA + wb->metas];
    if (blank) {
        k_memset(im->data, 0, BCACHE_BLOCK_BYTES);
    } else {
        b = bcache_get(em->dev, block);
        if (!b) {
            *rc = -EIO;
            return 0;
        }
        k_memcpy(im->data, b->data, BCACHE_BLOCK_BYTES);
        bcache_release(b);
    }
    im->block = block;
    im->stage = stage;
    im->dirty = 0;
    wb->metas++;
    return im;
}

/* A block chosen for a piece and not yet taken: its number, group and bit,
 * and the batch's images of its bitmap and descriptor blocks. */
struct ext2_reservation {
    uint32_t block, group, bit;
    struct ext2_image *bitmap, *descriptor;
};

/**
 * Choose a free block for the piece being planned, the next of the `count`
 * reservations in `list`, first-fit from the hint as `block_alloc` searches,
 * passing over the blocks the piece has chosen already, and take the images
 * of its bitmap and descriptor blocks. The free count already excludes the
 * blocks of committed pieces but not this piece's reservations, so those
 * are subtracted before another block is admitted, against the reserved
 * blocks for a caller that is not privileged. 0, -ENOSPC, -EIO for a clear
 * bit on a structural block, or the errors of `writeback_meta`.
 */
static int writeback_reserve(struct ext2_mount *em, struct ext2_reservation *list, int count)
{
    uint32_t start = em->alloc_hint;
    uint32_t g0, tried, g, from, end, bit, block;
    struct ext2_reservation *r = &list[count];
    int i, rc = 0;

    if (em->free_blocks <= (uint32_t)count ||
        (!may_use_reserved() && em->free_blocks - (uint32_t)count <= em->r_blocks)) {
        return -ENOSPC;
    }
    if (start >= em->blocks) {
        start = em->first_data;
    }
    g0 = (start - em->first_data) / em->bpg;
    for (tried = 0; tried <= em->groups; tried++) {
        uint8_t *gd;

        g = (g0 + tried) % em->groups;
        gd = em->gd + g * 32;
        if (le16(gd + 12) == 0) {
            continue;
        }
        from = tried == 0 ? (start - em->first_data) % em->bpg : 0;
        end = em->blocks - (em->first_data + g * em->bpg);
        if (end > em->bpg) {
            end = em->bpg;
        }
        if (tried == em->groups && end > (start - em->first_data) % em->bpg) {
            end = (start - em->first_data) % em->bpg;
        }
        for (bit = from; bit < end; bit++) {
            rc = bit_first_clear(em, le32(gd), bit, end, &bit);
            if (rc != 0) {
                return rc;
            }
            if (bit >= end) {
                break;
            }
            block = em->first_data + g * em->bpg + bit;
            for (i = 0; i < count && list[i].block != block; i++) {
            }
            if (i != count) {
                continue;
            }
            if (!data_block_ok(em, block)) {
                return -EIO;
            }
            r->bitmap = writeback_meta(em, le32(gd), WB_ALLOC, 0, &rc);
            if (!r->bitmap) {
                return rc;
            }
            r->descriptor =
                writeback_meta(em, em->first_data + 1 + (g * 32) / 4096, WB_ALLOC, 0, &rc);
            if (!r->descriptor) {
                return rc;
            }
            r->block = block;
            r->group = g;
            r->bit = bit;
            return 0;
        }
    }
    return -ENOSPC;
}

/*
 * What a piece of a write needs before it is committed: the blocks it
 * reserves, `allocs` of them; the images of the `paths` pointer blocks on its
 * path, with the byte offset of the slot in each and the block each slot is
 * to name; the images of its inode's table block and, when the file reaches
 * 2 GiB, the superblock; the inode's pointer at the top of the path, its
 * index and value; the data block, `fresh` when new, and its image; and the
 * inode's place in the batch's table, `count` for a new one.
 */
struct ext2_write_plan {
    struct ext2_reservation reservations[4];
    struct ext2_image *parents[3], *table, *feature, *data;
    uint32_t slots[3], children[3];
    uint32_t root, root_slot, block;
    int paths, fresh, allocs, node_index;
};

/**
 * Plan the pointer path and the data block of data index `index` of `n`
 * into `plan`: the image of each pointer block on the path, a missing one
 * reserved and blank, and a missing data block reserved, nothing yet taken.
 * The arithmetic is for 4 KiB blocks of 1024 pointers, which a writable
 * mount has. 0, -EIO for a block out of range, or the errors of
 * `writeback_reserve` and `writeback_meta`.
 */
static int writeback_plan_map(struct ext2_mount *em, struct ext2_node *n, uint32_t index,
                              struct ext2_write_plan *plan)
{
    uint32_t rest = index, div, block;
    int depth = (int)pointer_levels(em, index), level, rc = 0;

    plan->root_slot = depth ? EXT2_NDIR + depth - 1 : index;
    plan->root = n->block[plan->root_slot];
    if (depth) {
        rest -= EXT2_NDIR;
        if (depth >= 2) {
            rest -= 1024;
        }
        if (depth == 3) {
            rest -= 1024u * 1024u;
        }
    }
    block = plan->root;
    for (level = depth; level > 0; level--) {
        struct ext2_image *parent;
        int blank = block == 0;

        if (blank) {
            rc = writeback_reserve(em, plan->reservations, plan->allocs);
            if (rc != 0) {
                return rc;
            }
            block = plan->reservations[plan->allocs++].block;
            if (plan->paths == 0) {
                plan->root = block;
            } else {
                plan->children[plan->paths - 1] = block;
            }
        } else if (!data_block_ok(em, block)) {
            return -EIO;
        }
        parent = writeback_meta(em, block, WB_LEAF + level - 1, blank, &rc);
        if (!parent) {
            return rc;
        }
        div = level == 3 ? 1024u * 1024u : (level == 2 ? 1024u : 1u);
        plan->slots[plan->paths] = (rest / div) * 4;
        plan->parents[plan->paths] = parent;
        block = le32(parent->data + plan->slots[plan->paths]);
        plan->children[plan->paths++] = block;
        rest %= div;
    }
    if (block == 0) {
        rc = writeback_reserve(em, plan->reservations, plan->allocs);
        if (rc != 0) {
            return rc;
        }
        block = plan->reservations[plan->allocs++].block;
        plan->fresh = 1;
        if (plan->paths) {
            plan->children[plan->paths - 1] = block;
        } else {
            plan->root = block;
        }
    } else if (!data_block_ok(em, block)) {
        return -EIO;
    }
    plan->block = block;
    return 0;
}

/**
 * Plan a piece of `len` bytes at `offset` of `n`: its place in the batch's
 * inode table, -ENOMEM when that is full; the image of its inode's table
 * block; the superblock's image when the file reaches 2 GiB without
 * `large_file`; and the block mapping, by `writeback_plan_map`.
 */
static int writeback_plan(struct ext2_mount *em, struct ext2_node *n, uint64_t offset, uint32_t len,
                          struct ext2_write_plan *plan)
{
    struct ext2_writeback *wb = em->wb;
    int rc = 0;

    for (plan->node_index = 0; plan->node_index < wb->count; plan->node_index++) {
        if (wb->nodes[plan->node_index].node == n) {
            break;
        }
    }
    if (plan->node_index == wb->count && wb->count == EXT2_WB_NODES) {
        return -ENOMEM;
    }
    plan->table = writeback_meta(em, (uint32_t)(inode_offset(em, n->ino) >> 12), WB_INODE, 0, &rc);
    if (!plan->table) {
        return rc;
    }
    if (offset + len >= 0x80000000u && !(em->ro_compat & EXT2_RO_LARGE_FILE)) {
        plan->feature = writeback_meta(em, 0, WB_FEATURE, 0, &rc);
        if (!plan->feature) {
            return rc;
        }
    }
    return writeback_plan_map(em, n, (uint32_t)(offset >> 12), plan);
}

/** The batch's image of the plan's data block: the one it has, or a new one,
 * zeroed for a new block or one the piece of `len` bytes writes whole, and
 * a copy of the cache's block otherwise. 0, -ENOMEM when the table is full,
 * or -EIO. */
static int writeback_prepare_data(struct ext2_mount *em, uint32_t len, struct ext2_write_plan *plan)
{
    struct ext2_writeback *wb = em->wb;
    struct ext2_image *data;
    int i;

    for (i = 0; i < wb->datas && wb->data[i].block != plan->block; i++) {
    }
    if (i == wb->datas) {
        struct bbuf *b;

        if (i == EXT2_WB_DATA) {
            return -ENOMEM;
        }
        data = &wb->data[i];
        data->block = plan->block;
        data->stage = WB_DATA;
        data->data = wb->slot[i];
        if (plan->fresh || len == BCACHE_BLOCK_BYTES) {
            k_memset(data->data, 0, BCACHE_BLOCK_BYTES);
        } else {
            b = bcache_get(em->dev, plan->block);
            if (!b) {
                return -EIO;
            }
            k_memcpy(data->data, b->data, BCACHE_BLOCK_BYTES);
            bcache_release(b);
        }
        wb->datas++;
    }
    plan->data = &wb->data[i];
    return 0;
}

/**
 * Commit a planned piece, with no I/O and nothing that can fail: the
 * reserved blocks taken in their bitmap and descriptor images and in the
 * free count, the pointer path and the inode's pointers and `i_blocks` set,
 * the superblock's `large_file` flag set, the inode held by the batch, the
 * bytes copied into the data image, and the size and times raised. Reads
 * see the piece at once, since `read_bytes` looks in the images first.
 */
static void writeback_commit(struct ext2_mount *em, struct ext2_node *n, uint64_t offset,
                             const uint8_t *src, uint32_t len, struct ext2_write_plan *plan)
{
    struct ext2_writeback *wb = em->wb;
    int i;

    for (i = 0; i < plan->allocs; i++) {
        struct ext2_reservation *r = &plan->reservations[i];
        uint8_t *gd = em->gd + r->group * 32;

        r->bitmap->data[r->bit / 8] |= (uint8_t)(1u << (r->bit % 8));
        r->bitmap->dirty = 1;
        LE16_PUT(gd + 12, le16(gd + 12) - 1);
        k_memcpy(r->descriptor->data + (r->group * 32) % 4096, gd, 32);
        r->descriptor->dirty = 1;
        em->free_blocks--;
        em->alloc_hint = r->block + 1;
    }
    for (i = 0; i < plan->paths; i++) {
        if (le32(plan->parents[i]->data + plan->slots[i]) != plan->children[i]) {
            LE32_PUT(plan->parents[i]->data + plan->slots[i], plan->children[i]);
            plan->parents[i]->dirty = 1;
        }
    }
    if (plan->feature) {
        em->ro_compat |= EXT2_RO_LARGE_FILE;
        LE32_PUT(plan->feature->data + 1024 + 100, em->ro_compat);
        plan->feature->dirty = 1;
    }
    if (plan->node_index == wb->count) {
        wb->nodes[plan->node_index].node = n;
        wb->nodes[plan->node_index].committed_size = n->size;
        wb->count++;
        n->refs++;
    }
    n->block[plan->root_slot] = plan->root;
    n->blocks += (uint32_t)plan->allocs * 8;
    k_memcpy(plan->data->data + ((uint32_t)offset & 4095u), src, len);
    plan->data->dirty = 1;
    if (offset + len > n->size) {
        n->size = offset + len;
    }
    n->mtime = n->ctime = now_seconds();
    plan->table->dirty = 1;
    wb->bytes += len;
}

/**
 * Accept one piece of a write, `len` bytes from `src` at `offset` of `n`
 * within one block: planned, its data image prepared, and committed. No
 * image readers see changes until every read and reservation has
 * succeeded, and a piece that fails gives back the images it took. A
 * removed device, one that failed a write, or a mount made read-only is
 * -EIO. 0, or -errno.
 */
static int writeback_piece(struct ext2_mount *em, struct ext2_node *n, uint64_t offset,
                           const uint8_t *src, uint32_t len)
{
    struct ext2_writeback *wb = em->wb;
    struct ext2_write_plan plan;
    int old_metas = wb->metas, old_datas = wb->datas;
    int rc;

    k_memset(&plan, 0, sizeof(plan));
    rc = writeback_plan(em, n, offset, len, &plan);
    if (rc != 0) {
        goto fail;
    }
    rc = writeback_prepare_data(em, len, &plan);
    if (rc != 0) {
        goto fail;
    }
    if (em->dev->flags & (BLOCK_GONE | BLOCK_WRITE_FAILED) || em->mnt->flags & VFS_MS_RDONLY) {
        rc = -EIO;
        goto fail;
    }
    writeback_commit(em, n, offset, src, len, &plan);
    return 0;
fail:
    wb->metas = old_metas;
    wb->datas = old_datas;
    return rc;
}

/**
 * Record a failure `rc` of the batch at `block`: the first one's errno,
 * stage, block and generation kept for the mount's life, the error bit set
 * in memory, the mount made read-only, and the mount's `write_error` set
 * when it has none, which every later `fsync` reports. Returns `rc`.
 */
static int writeback_error(struct ext2_mount *em, int rc, uint32_t block)
{
    struct ext2_writeback *wb = em->wb;

    if (wb && !wb->error) {
        wb->error = rc;
        wb->error_stage = wb->stage;
        wb->error_block = block;
        wb->error_generation = wb->generation;
    }
    em->state |= EXT2_ERROR_FS;
    em->mnt->flags |= VFS_MS_RDONLY;
    if (!em->mnt->write_error) {
        em->mnt->write_error = rc;
    }
    return rc;
}

/**
 * Write the batch out, stage by stage, under the mount's namespace lock: the
 * inodes encoded into their table images, then each stage's changed images
 * copied into the cache, from the allocation bitmaps and descriptors through
 * the data, the pointer blocks and the superblock to the inode tables. On a
 * strict mount each stage is synced and flushed before the next enters the
 * cache, so that a sync or an eviction can write only a stage whose
 * dependencies are on the medium; on any other mount the stages stay dirty
 * in the cache for the sync. No buffer is held across a sync, which may
 * reach a backing file. At the end the batch is emptied, its inodes' holds
 * put, and the cache balanced (`em_balance`).
 *
 * A failure keeps the images and the stage, so that the next drain resumes
 * there, and is recorded by `writeback_error`; a removed device fails at
 * once. 0, or -errno.
 */
static int writeback_drain(struct ext2_mount *em)
{
    struct ext2_writeback *wb = em->wb;
    int i, count, rc;
    uint32_t block = 0;

    if (em->dev->flags & BLOCK_GONE) {
        return writeback_error(em, -EIO, 0);
    }
    if (!wb || !wb->count) {
        return 0;
    }
    wb->draining = 1;
    for (i = 0; i < wb->count; i++) {
        struct ext2_node *n = wb->nodes[i].node;
        uint64_t at = inode_offset(em, n->ino);
        struct ext2_image *im = writeback_find(em, (uint32_t)(at >> 12));

        inode_encode(n, im->data + ((uint32_t)at & 4095u));
    }
    for (; wb->stage <= WB_INODE; wb->stage++) {
        struct ext2_image *images = wb->stage == WB_DATA ? wb->data : wb->meta;
        int wrote = 0;

        count = wb->stage == WB_DATA ? wb->datas : wb->metas;
        rc = em_sync(em);
        if (rc != 0) {
            goto fail;
        }
        for (i = 0; i < count; i++) {
            struct ext2_image *im = &images[i];
            struct bbuf *b;

            if (!im->dirty || im->stage != wb->stage) {
                continue;
            }
            block = im->block;
            b = bcache_get_blank(em->dev, block);
            if (!b) {
                rc = -EIO;
                goto fail;
            }
            k_memcpy(b->data, im->data, BCACHE_BLOCK_BYTES);
            bcache_dirty(b);
            bcache_release(b);
            wrote = 1;
        }
        if (wrote) {
            rc = em_sync(em);
            if (rc == 0 && em_strict(em)) {
                rc = block_flush(em->dev);
            }
            if (rc != 0) {
                goto fail;
            }
        }
    }
    count = wb->count;
    wb->count = wb->datas = wb->metas = 0;
    wb->bytes = 0;
    wb->stage = 0;
    wb->draining = 0;
    wb->generation++;
    for (i = 0; i < count; i++) {
        struct ext2_node *n = wb->nodes[i].node;

        wb->nodes[i].committed_size = n->size;
        ext2_put(em->mnt, n);
    }
    em_balance(em);
    return 0;
fail:
    wb->draining = 0;
    return writeback_error(em, rc, block);
}

/**
 * Whether the write of `count` bytes at `offset` of `n` may join the batch:
 * 1 for an overwrite of blocks the file has inside its size, or an append
 * that starts at the size on a block boundary or in a last block the file
 * has; 0 for any other write, which drains the batch first and after each
 * block; or the errors of `block_at`.
 */
static int writeback_contiguous(struct ext2_mount *em, struct ext2_node *n, uint64_t offset,
                                uint32_t count)
{
    uint32_t first, last, i, block;
    int rc;

    if (offset > n->size || (offset < n->size && count > n->size - offset)) {
        return 0;
    }
    if (offset == n->size && !(offset & 4095u)) {
        return 1;
    }
    first = (uint32_t)(offset >> 12);
    last = offset == n->size ? first : (uint32_t)((offset + count - 1) >> 12);
    for (i = first; i <= last; i++) {
        rc = block_at(em, n, i, &block);
        if (rc != 0) {
            return rc;
        }
        if (!block) {
            return 0;
        }
    }
    return 1;
}

/* ---- directories, written ---- */

/** The record length of an entry with a name of `name_len` bytes: the
 * 8-byte header and the name, rounded up to 4. */
static uint32_t dirent_len(uint32_t name_len)
{
    return (8 + name_len + 3) & ~3u;
}

/** The file-type byte of an entry for an inode of mode `mode`, 1 to 7, or 0
 * for a kind the format has no type for. */
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
    case S_IFIFO_:
        return 5;
    case S_IFSOCK_:
        return 6;
    case S_IFLNK_:
        return 7;
    default:
        return 0;
    }
}

/** Find `name` in `d`: 1 with the entry in `sc`, its position in `*at` and
 * its predecessor's in the same block in `*prev_at`, or the position itself
 * when it is first in its block; 0 when absent, or -EIO. */
static int dir_find(struct ext2_mount *em, const struct ext2_node *d, const char *name,
                    uint32_t *at, uint32_t *prev_at, struct ext2_scan *sc)
{
    uint32_t cursor = 0, prev = 0;
    struct ext2_window w;
    int got;

    window_open(em, &w);
    for (;;) {
        uint32_t here = cursor;

        got = dir_scan_in(em, d, &w, &cursor, sc);
        if (got <= 0) {
            break;
        }
        if (here % em->block_size == 0) {
            prev = here;
        }
        if (sc->ino != 0 && names_equal(sc->name, name)) {
            *at = here;
            *prev_at = prev;
            break;
        }
        prev = here;
    }
    window_close(&w);
    return got;
}

/** The byte offset on the volume of byte `at` of directory `d`, through its
 * map, into `*off`; 0, -EIO for a hole, or the errors of `block_at`. */
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

/** Encode the entry naming `ino`, its header, name and padding, at `raw`,
 * with the record length `rec_len` and the type byte `type` as the volume
 * keeps it; returns the entry's own length. */
static uint32_t dirent_encode(uint8_t *raw, uint32_t ino, uint32_t rec_len, const char *name,
                              uint32_t name_len, uint32_t type)
{
    uint32_t len = dirent_len(name_len);

    k_memset(raw, 0, len);
    LE32_PUT(raw, ino);
    LE16_PUT(raw + 4, rec_len);
    raw[6] = (uint8_t)name_len;
    raw[7] = (uint8_t)type;
    k_memcpy(raw + 8, name, name_len);
    return len;
}

/** Write the entry naming `ino`, with the record length `rec_len`, at byte
 * `off` of the volume with WRITE_THROUGH; its type byte is `ftype` when the
 * volume has the filetype feature and 0 otherwise. 0, or -errno. */
static int dirent_write(struct ext2_mount *em, uint64_t off, uint32_t ino, uint32_t rec_len,
                        const char *name, uint32_t name_len, uint32_t ftype)
{
    uint8_t raw[8 + EXT2_NAME_MAX + 4]; /* the header, the name and its padding */
    uint32_t type = (em->incompat & EXT2_INCOMPAT_FILETYPE) ? ftype : 0;
    uint32_t len = dirent_encode(raw, ino, rec_len, name, name_len, type);

    return write_bytes(em, off, raw, len, WRITE_THROUGH);
}

/**
 * Split the entry `old` at `off` for the new entry `entry`, of `len` bytes
 * with the rest of `old`'s record, which goes behind `old`'s name. 0, or
 * -errno.
 *
 * A sector is the unit the disk writes whole, so when the two headers and
 * the new name lie in one, one write does both. Otherwise the new entry
 * is written into the slack behind the old name first, where the old
 * record still covers it, and then the old header is shortened to end
 * where the new one begins; a stop between the two leaves the old entry
 * spanning its record and the new one unreachable, which is the volume as
 * it was.
 */
static int dirent_split(struct ext2_mount *em, uint64_t off, const struct ext2_scan *old,
                        const uint8_t *entry, uint32_t len)
{
    uint8_t pair[2 * (8 + EXT2_NAME_MAX + 4)];
    uint32_t used = dirent_len(old->name_len);
    int rc;

    if ((off >> 9) == ((off + used + len - 1) >> 9)) {
        dirent_encode(pair, old->ino, used, old->name, old->name_len, old->type);
        k_memcpy(pair + used, entry, len);
        return write_bytes(em, off, pair, used + len, WRITE_THROUGH);
    }
    rc = write_bytes(em, off + used, entry, len, WRITE_THROUGH);
    if (rc != 0) {
        return rc;
    }
    used = dirent_encode(pair, old->ino, used, old->name, old->name_len, old->type);
    return write_bytes(em, off, pair, used, WRITE_THROUGH);
}

/**
 * Add `name` naming `ino`, of file type `ftype`, to directory `d`: in the
 * first empty record with room, whole, or the first entry with room behind
 * its name, split; else in a new block, allocated, written zeroed with one
 * empty entry spanning it, and chained with `i_blocks` and covered by the
 * size in one write of the inode. The entry itself is the last write. 0,
 * -ENOENT for a removed directory, -ENAMETOOLONG, -EFBIG for a directory
 * that would pass 4 GiB less a block, -ENOSPC, decided before any block is
 * taken for the block and the pointer blocks its index can need, or -errno.
 *
 * The search for room starts at the block the last entry went into and
 * wraps around to it, so a directory that grows at its end is not read
 * whole for each name added to it; a block before that one still takes a
 * name before the directory grows.
 */
static int dir_add(struct ext2_mount *em, struct ext2_node *d, const char *name, uint32_t ino,
                   uint32_t ftype)
{
    uint32_t name_len = (uint32_t)k_strlen(name);
    uint32_t need = dirent_len(name_len);
    uint32_t start = d->add_from < d->size ? d->add_from : 0;
    uint32_t cursor = start, limit = (uint32_t)d->size;
    struct ext2_scan sc;
    struct ext2_window w;
    uint8_t entry[8 + EXT2_NAME_MAX + 4];
    int rc, fresh;

    if (d->unlinked) {
        return -ENOENT;
    }
    if (name_len == 0 || name_len > EXT2_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    window_open(em, &w);
    for (;;) {
        uint32_t here = cursor;
        uint32_t used, room;
        uint64_t off;
        int got = here < limit ? dir_scan_in(em, d, &w, &cursor, &sc) : 0;

        if (got < 0) {
            window_close(&w);
            return got;
        }
        if (got == 0 && start != 0) {
            cursor = 0;
            limit = start;
            start = 0;
            continue;
        }
        if (got == 0) {
            break;
        }
        used = sc.ino == 0 ? 0 : dirent_len(sc.name_len);
        room = sc.rec_len - used;
        if (room < need) {
            continue;
        }
        window_close(&w);
        rc = dir_block_at(em, d, here, &off);
        if (rc != 0) {
            return rc;
        }
        d->add_from = here & ~(em->block_size - 1u);
        if (used == 0) {
            return dirent_write(em, off, ino, sc.rec_len, name, name_len, ftype);
        }
        used = dirent_encode(entry, ino, room, name, name_len,
                             (em->incompat & EXT2_INCOMPAT_FILETYPE) ? ftype : 0);
        return dirent_split(em, off, &sc, entry, used);
    }
    window_close(&w);
    {
        uint32_t index = (uint32_t)(d->size >> em->block_shift);
        uint32_t b;
        uint64_t off;
        uint8_t head[8];

        if (d->size + em->block_size > 0xFFFFFFFFu) {
            return -EFBIG;
        }
        if (1 + pointer_levels(em, index) > em->free_blocks ||
            (!may_use_reserved() &&
             em->free_blocks - (1 + pointer_levels(em, index)) < em->r_blocks)) {
            return -ENOSPC;
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
        d->add_from = index << em->block_shift;
        return dirent_write(em, off, ino, em->block_size, name, name_len, ftype);
    }
}

/** Remove the entry `sc` at byte `at` of `d`, whose predecessor in its block
 * is at `prev_at`: the first in its block has its inode cleared, and any
 * other is merged into its predecessor's record, with WRITE_THROUGH. 0, or
 * -errno. */
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

/** Whether directory `d` has nothing but `.` and `..`, into `*empty`; 0, or
 * -EIO. */
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

/** Make the `..` entry of directory `d` name `parent`, with WRITE_THROUGH; 0,
 * -EIO for a directory without one, or -errno. */
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

/** Whether directory inode `ino` is `ancestor` or below it, into `*below`,
 * by the `..` entries from `ino` up to the root: 0, -ELOOP past PATH_DEPTH
 * levels, -EIO for a directory without `..`, or -errno. */
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

/** The node in memory of inode `ino` when something holds it, or NULL; no
 * hold is taken. */
static struct ext2_node *node_held(struct ext2_mount *em, uint32_t ino)
{
    struct ext2_node *n;

    for (n = em->nodes; n; n = n->next) {
        if (n->ino == ino && n->refs > 0) {
            return n;
        }
    }
    return 0;
}

/**
 * Free inode `n`, which has no name and no holder: the blocks of a regular
 * file, a directory or a long symbolic link from the end (`punch`), then
 * the inode written zeroed with its deletion time, then its bit and counts.
 * The node's `unlinked` mark is cleared. 0, or -errno.
 */
static int release_inode(struct ext2_mount *em, struct ext2_node *n)
{
    int was_dir = (n->mode & S_IFMT_) == S_IFDIR_;
    int rc;

    if ((n->mode & S_IFMT_) == S_IFREG_ || was_dir ||
        ((n->mode & S_IFMT_) == S_IFLNK_ && n->blocks != 0)) {
        rc = punch(em, n, 0);
        if (rc != 0) {
            return rc;
        }
    }
    {
        uint8_t raw[128];

        k_memset(raw, 0, sizeof(raw));
        LE32_PUT(raw + 20, now_seconds()); /* i_dtime */
        rc = write_bytes(em, inode_offset(em, n->ino), raw, sizeof(raw), 1);
    }
    if (rc != 0) {
        return rc;
    }
    n->unlinked = 0;
    return inode_bit_free(em, n->ino, was_dir);
}

/**
 * A new inode of mode `mode` in the directory `dir`, allocated, written
 * initialised and held once in a new node, into `*out`; the caller names
 * it. Its owner is the caller's effective uid, and its group the caller's
 * effective gid, or the directory's when the directory is setgid, which
 * then passes the bit on to a new directory, as Linux's `inode_init_owner`
 * does. Its link count is 2 for a directory and 1 otherwise.
 *
 * With `store` the inode is written whole over zeros; without, its slot is
 * written zeroed, for a caller that stores the inode once it has the rest.
 * Either way the slot's old bytes are gone, since a freed inode's slot
 * keeps what it had and the encoder writes only the fields this driver
 * keeps. 0, -ENOSPC, -ENOMEM, or -errno; a failure frees the inode.
 */
static int inode_new(struct ext2_mount *em, const struct ext2_node *dir, uint32_t mode, int store,
                     struct ext2_node **out)
{
    uint32_t parent_ino = dir->ino;
    uint32_t ino;
    struct ext2_node *n;
    int rc;

    rc = inode_alloc(em, parent_ino, (mode & S_IFMT_) == S_IFDIR_, &ino);
    if (rc != 0) {
        return rc;
    }
    n = node_new(em);
    if (!n) {
        inode_bit_free(em, ino, (mode & S_IFMT_) == S_IFDIR_);
        klog(KLOG_WARN, "ext2", "node: no memory");
        return -ENOMEM;
    }
    n->refs = 1;
    n->ino = ino;
    n->uid = current_proc ? current_proc->euid : 0;
    n->gid = current_proc ? current_proc->egid : 0;
    if (dir->mode & S_ISGID_) {
        n->gid = dir->gid;
        if ((mode & S_IFMT_) == S_IFDIR_) {
            mode |= S_ISGID_;
        }
    }
    n->mode = mode;
    n->nlink = (mode & S_IFMT_) == S_IFDIR_ ? 2 : 1;
    n->atime = n->mtime = n->ctime = now_seconds();
    n->generation = em->next_generation++;
    rc = store ? inode_write(em, n, 1, WRITE_THROUGH)
               : write_bytes(em, inode_offset(em, ino), 0, 128, WRITE_THROUGH);
    if (rc != 0) {
        node_drop(em, n);
        inode_bit_free(em, ino, (mode & S_IFMT_) == S_IFDIR_);
        return rc;
    }
    *out = n;
    return 0;
}

/* ---- the operations ---- */

/**
 * The `setattr` of `vfs_ops`: the permission bits, the owner, the group, the
 * access and modification times and the flags chattr may change, as `what`
 * selects, with the change time set to the present and the inode stored.
 * The batch is drained first. Takes the mount's namespace lock. 0, -EROFS,
 * or -errno.
 */
static int ext2_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *n = node;
    int rc;

    vfs_ns_lock(mnt);
    rc = assert_writable(mnt, em);
    if (rc != 0) {
        vfs_ns_unlock(mnt);
        return rc;
    }
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
    if (what & VFS_SET_FLAGS) {
        n->flags = (n->flags & ~EXT2_FL_USER_MODIFIABLE) | (st->flags & EXT2_FL_USER_MODIFIABLE);
    }
    n->ctime = now_seconds();
    rc = inode_store(em, n);
    vfs_ns_unlock(mnt);
    return rc;
}

/**
 * Refuse `offset` for the operation `op`, past the largest file this volume
 * can address, with a record of the offset and the limit: the limit is the
 * block pointers' reach or the 32-bit `i_blocks`, whichever is smaller. The
 * seek that put the descriptor there was legal; the write or truncate is
 * not. Returns -EFBIG.
 */
static int past_limit(const struct ext2_mount *em, const char *op, uint64_t offset)
{
    klog(KLOG_WARN, "ext2",
         "%s: offset %d MiB is past the file limit of %d MiB "
         "for %d-byte blocks",
         op, (int)(offset >> 20), (int)(em->file_limit >> 20), (int)em->block_size);
    return -EFBIG;
}

/**
 * The `truncate` of `vfs_ops`: the size of regular file `handle` made
 * `size`, under the mount's namespace lock, the batch drained first; both
 * directions set the modification and change times.
 *
 * A shrink writes the smaller size first, then zeroes the tail of the last
 * block kept, so that a later growth over it reads zeros and not what was
 * there, then frees the blocks from the end (`punch`) and stores the inode
 * again; a stop between leaves blocks past the end. A growth leaves the gap
 * a hole, and puts the zeroed tail a shrink may have left unsynced on the
 * disk before the size covers it. 0, -EINVAL for a file that is not
 * regular, -EFBIG, -EROFS, or -errno.
 */
static int ext2_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *n = handle;
    int rc;

    if ((n->mode & S_IFMT_) != S_IFREG_) {
        return -EINVAL;
    }
    if (size > em->file_limit) {
        return past_limit(em, "truncate", size);
    }
    vfs_ns_lock(mnt);
    rc = assert_writable(mnt, em);
    if (rc != 0) {
        vfs_ns_unlock(mnt);
        return rc;
    }
    assert_locked(mnt);
    if (size < n->size) {
        uint32_t keep = (uint32_t)((size + em->block_size - 1) >> em->block_shift);

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
        rc = em_sync(em);
        if (rc == 0) {
            n->size = size;
            n->mtime = n->ctime = now_seconds();
            rc = inode_store(em, n);
        }
    }
    vfs_ns_unlock(mnt);
    return rc;
}

/**
 * The `fsync` of `vfs_ops`: the batch drained, the device's dirty buffers
 * written and the disk's cache flushed, whatever file `handle` is, under
 * the mount's namespace lock. A failure is recorded as the batch's error,
 * and the mount's first failure, of this call or any before, is what it
 * returns; 0 when there has been none.
 */
static int ext2_fsync(struct vfs_mount *mnt, void *handle)
{
    struct ext2_mount *em = em_of(mnt);
    int rc;

    (void)handle;
    vfs_ns_lock(mnt);
    rc = writeback_drain(em);
    if (rc == 0) {
        rc = bcache_sync(em->dev);
    }
    if (rc == 0) {
        rc = block_flush(em->dev);
    }
    if (rc != 0) {
        (void)writeback_error(em, rc, 0);
    }
    if (mnt->write_error) {
        rc = mnt->write_error;
    }
    vfs_ns_unlock(mnt);
    return rc;
}

/* One step of the walk below: a block pointer, its level (0 for a data
 * block), and the first and last data indexes, both included and counted
 * from the start of its subtree, that the step covers. */
struct writeback_step {
    uint32_t block;
    int level;
    uint32_t first;
    uint32_t last;
};

/**
 * Add to `*need` the blocks `n` lacks under `step`. An absent subtree needs
 * its data blocks and every pointer block covering the range, each counted
 * once by dividing both ends of the range at each level, also when the
 * range crosses a leaf or a higher boundary. A present pointer block is read,
 * the batch's accepted allocations included, and its children counted. 0,
 * or -EIO, with the error bit set and a record, for a block out of range.
 */
static int writeback_missing(struct ext2_mount *em, const struct ext2_node *n,
                             const struct writeback_step *step, uint64_t *need)
{
    uint32_t per = em->block_size / 4;
    uint32_t block = step->block;
    uint32_t first = step->first;
    uint32_t last = step->last;
    int level = step->level;
    uint32_t span = 1, i;
    int depth;

    if (block == 0) {
        *need += last - first + 1;
        while (level-- > 0) {
            first /= per;
            last /= per;
            *need += last - first + 1;
        }
        return 0;
    }
    if (!data_block_ok(em, block)) {
        klog(KLOG_WARN, "ext2", "inode %d: block %d is out of range", (int)n->ino, (int)block);
        mark_error(em);
        return -EIO;
    }
    if (level == 0) {
        return 0;
    }
    for (depth = 1; depth < level; depth++) {
        span *= per;
    }
    for (i = first / span; i <= last / span; i++) {
        uint8_t raw[4];
        uint32_t begin = i == first / span ? first % span : 0;
        uint32_t end = i == last / span ? last % span : span - 1;
        int rc;

        rc = read_bytes(em, (uint64_t)block * em->block_size + (uint64_t)i * 4, raw, 4);
        if (rc == 0) {
            struct writeback_step under;

            under.block = le32(raw);
            under.level = level - 1;
            under.first = begin;
            under.last = end;
            rc = writeback_missing(em, n, &under, need);
        }
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/** Whether the blocks the write of `count` bytes at `offset` of `n` lacks,
 * data and pointer blocks, fit the free count, less the reserved blocks for
 * a caller that is not privileged: 0, -ENOSPC, or the errors of
 * `writeback_missing`. */
static int writeback_space(struct ext2_mount *em, struct ext2_node *n, uint64_t offset,
                           uint32_t count)
{
    uint32_t first = (uint32_t)(offset >> em->block_shift);
    uint32_t last = (uint32_t)((offset + count - 1) >> em->block_shift);
    uint32_t base = 0, span = 1, slot;
    uint64_t need = 0;
    int level = 0;

    for (slot = 0; slot < EXT2_NDIR + 3 && base <= last; slot++) {
        if (slot >= EXT2_NDIR) {
            span *= em->block_size / 4;
            level++;
        }
        if (first < base + span) {
            uint32_t begin = first > base ? first - base : 0;
            uint32_t end = last < base + span ? last - base : span - 1;
            struct writeback_step step;
            int rc;

            step.block = n->block[slot];
            step.level = level;
            step.first = begin;
            step.last = end;
            rc = writeback_missing(em, n, &step, &need);

            if (rc != 0) {
                return rc;
            }
            if (need > em->free_blocks ||
                (!may_use_reserved() && need > 0 && em->free_blocks - need < em->r_blocks)) {
                return -ENOSPC;
            }
        }
        base += span;
    }
    return 0;
}

/**
 * The `write` of `vfs_ops`: `count` bytes from `buf` at `offset`, through
 * the file-write batch, under the mount's namespace lock, which this takes.
 *
 * ENOSPC is decided first, for the whole write (`writeback_space`). An
 * overwrite inside the file's blocks or a contiguous append joins the batch
 * a block at a time; any other write drains the batch before it and after
 * each block. The batch drains too when its bytes, images or inodes run out,
 * and a piece that finds its tables full drains it and is tried once more. A
 * write that fails after some of its bytes were accepted returns their
 * count; a failure of the batch's I/O stays recorded for `fsync`, after a
 * close and a retry too. Returns the bytes accepted, -EISDIR, -EFBIG,
 * -EROFS, -ENOSPC, or -errno.
 */
static int32_t ext2_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                          uint64_t offset)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_writeback *wb = em->wb;
    struct ext2_node *n = handle;
    const uint8_t *in = buf;
    uint32_t done = 0;
    int rc = 0, synchronous;

    if ((n->mode & S_IFMT_) == S_IFDIR_) {
        return -EISDIR;
    }
    if (count == 0) {
        return 0;
    }
    if (offset > em->file_limit || count > em->file_limit - offset) {
        return past_limit(em, "write", offset);
    }
    vfs_ns_lock(mnt);
    assert_locked(mnt);
    if (!em->writable || (mnt->flags & VFS_MS_RDONLY)) {
        rc = -EROFS;
        goto out;
    }
    rc = writeback_space(em, n, offset, count);
    if (rc != 0) {
        goto out;
    }
    rc = writeback_contiguous(em, n, offset, count);
    if (rc < 0) {
        goto out;
    }
    synchronous = !rc;
    if (synchronous) {
        rc = writeback_drain(em);
        if (rc != 0) {
            goto out;
        }
    }
    while (done < count) {
        uint64_t at = offset + done;
        uint32_t take = 4096u - ((uint32_t)at & 4095u);

        if (take > count - done) {
            take = count - done;
        }
        if (take > EXT2_WB_BYTES - wb->bytes) {
            rc = writeback_drain(em);
            if (rc != 0) {
                goto out;
            }
        }
        rc = writeback_piece(em, n, at, in + done, take);
        if (rc == -ENOMEM && wb->count) {
            rc = writeback_drain(em);
            if (rc == 0) {
                rc = writeback_piece(em, n, at, in + done, take);
            }
        }
        if (rc != 0) {
            goto out;
        }
        done += take;
        if (synchronous || wb->bytes == EXT2_WB_BYTES) {
            rc = writeback_drain(em);
            if (rc != 0) {
                goto out;
            }
        }
        if (wb->datas == EXT2_WB_DATA || wb->metas == EXT2_WB_META || wb->count == EXT2_WB_NODES) {
            rc = writeback_drain(em);
            if (rc != 0) {
                goto out;
            }
        }
    }
    rc = 0;
out:
    if (rc == -EIO && wb && wb->count) {
        (void)writeback_error(em, rc, 0);
    }
    vfs_ns_unlock(mnt);
    return done ? (int32_t)done : rc;
}

/**
 * The `fallocate` of `vfs_ops`: ext2 keeps no block reserved ahead of its
 * data, so each hole in the range is given a block by a write of zeros, a
 * block the file has is written only past its size, and without
 * FALLOC_FL_KEEP_SIZE the size grows to the range's end; with it the range
 * stops at the size. A punched hole and any other mode are -EOPNOTSUPP, as
 * Linux's ext2 returns for every mode. 0, -ENODEV for a file that is not
 * regular, -EFBIG, -ENOMEM, or the errors of `ext2_write`.
 */
static int ext2_fallocate(struct vfs_mount *mnt, void *handle, uint32_t mode, uint64_t offset,
                          uint64_t len)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *n = handle;
    uint64_t at = offset, end = offset + len;
    uint8_t *zero;
    int rc = 0;

    if (mode & ~0x01u) {
        return -EOPNOTSUPP;
    }
    if ((n->mode & S_IFMT_) != S_IFREG_) {
        return -ENODEV;
    }
    if (end > em->file_limit) {
        return past_limit(em, "fallocate", offset);
    }
    if ((mode & 0x01u) && end > n->size) {
        end = n->size;
    }
    zero = kzalloc(em->block_size);
    if (!zero) {
        return -ENOMEM;
    }
    while (at < end && rc == 0) {
        uint32_t take = em->block_size - ((uint32_t)at & (em->block_size - 1));
        uint64_t from = at;
        uint32_t b = 0;

        if (take > end - at) {
            take = (uint32_t)(end - at);
        }
        vfs_ns_lock(mnt);
        rc = block_at(em, n, (uint32_t)(at >> em->block_shift), &b);
        vfs_ns_unlock(mnt);
        if (b != 0 && from < n->size) {
            from = n->size;
        }
        if (rc == 0 && from < at + take) {
            int32_t w = ext2_write(mnt, handle, zero, (uint32_t)(at + take - from), from);

            rc = w < 0 ? (int)w : 0;
        }
        at += take;
    }
    kfree(zero);
    return rc;
}

/**
 * The `create` of `vfs_ops`: a new regular file `name` in `dir`, the root for
 * NULL, with the permission bits of `mode`: the inode allocated and written
 * whole, then the entry that names it, then the directory's times into the
 * cache. A failure to add the entry frees the inode; a failure to store the
 * times sets the error bit and is not the call's, since the entry is there.
 * The batch is drained first. The caller holds the mount's namespace lock.
 * 0 with the node held, -EEXIST, -ENAMETOOLONG, -EROFS, -ENOSPC, -ENOMEM,
 * or -errno.
 */
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
    rc = inode_new(em, d, S_IFREG_ | (mode & 07777u), 1, &n);
    if (rc != 0) {
        return rc;
    }
    rc = dir_add(em, d, name, n->ino, 1);
    if (rc != 0) {
        n->nlink = 0;
        release_inode(em, n);
        node_drop(em, n);
        return rc;
    }
    d->mtime = d->ctime = now_seconds();
    rc = inode_times_later(em, d);
    if (rc != 0) {
        mark_error(em);
    }
    *node = n;
    return 0;
}

/**
 * The `mknod` of `vfs_ops`: a FIFO, a socket or a device node `name` in
 * `dir`, in the order `create` keeps: the inode's bit, then the inode written
 * whole, then the entry that names it, then the directory's times. A
 * device's number is where Linux keeps it: the old encoding, a byte each of
 * major and minor, in the first pointer word when both fit, and the new
 * encoding in the second word otherwise, with the first zero. The caller
 * holds the mount's namespace lock. 0 with the node held, -EINVAL for
 * another kind, or the errors of `ext2_create`.
 */
static int ext2_mknod(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode,
                      uint32_t rdev, void **node)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node *n;
    struct ext2_scan sc;
    uint32_t type = mode & S_IFMT_;
    uint32_t at, prev;
    int rc;

    if (type != S_IFIFO_ && type != S_IFSOCK_ && type != S_IFCHR_ && type != S_IFBLK_) {
        return -EINVAL;
    }
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
    rc = inode_new(em, d, type | (mode & 07777u), 0, &n);
    if (rc != 0) {
        return rc;
    }
    if (type == S_IFCHR_ || type == S_IFBLK_) {
        uint32_t major = (rdev >> 8) & 0xFFFu;
        uint32_t minor = (rdev & 0xFFu) | ((rdev >> 12) & 0xFFF00u);

        if (major < 256 && minor < 256) {
            n->block[0] = (major << 8) | minor;
        } else {
            n->block[1] = (minor & 0xFFu) | (major << 8) | ((minor & 0xFFF00u) << 12);
        }
    }
    rc = inode_store(em, n);
    if (rc == 0) {
        rc = dir_add(em, d, name, n->ino, ftype_of(n->mode));
    }
    if (rc != 0) {
        n->nlink = 0;
        release_inode(em, n);
        node_drop(em, n);
        return rc;
    }
    d->mtime = d->ctime = now_seconds();
    rc = inode_times_later(em, d);
    if (rc != 0) {
        mark_error(em);
    }
    *node = n;
    return 0;
}

/**
 * The `unlink` of `vfs_ops`: the entry `name` in `dir` removed, then the
 * directory's times, then the inode's link count lowered. The inode goes
 * with its last name when nothing holds it (`release_inode`), and at the
 * last put otherwise, its count of zero stored meanwhile, so that after a
 * crash e2fsck and the check read it as deleted. The batch is drained
 * first. The caller holds the mount's namespace lock. 0, -ENOENT, -EISDIR,
 * -EROFS, or -errno.
 */
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
    rc = dir_remove_at(em, d, at, prev, &sc);
    if (rc != 0) {
        return rc;
    }
    d->mtime = d->ctime = now_seconds();
    rc = inode_times_later(em, d);
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
        return inode_store(em, n);
    }
    return inode_store(em, n);
}

/**
 * The `mkdir` of `vfs_ops`: a new directory `name` in `dir`, with the
 * permission bits of `mode`. Its block is written with `.` and `..`, then
 * its inode with a count of two, then the parent's raised count, and the
 * parent's entry last; a stop before the inode leaves a set bit with
 * nothing behind it. A failure frees the new inode, and lowers the parent's
 * count again when the entry was not written. The batch is drained first.
 * The caller holds the mount's namespace lock. 0, -EEXIST, -EMLINK at 65000
 * links, -ENAMETOOLONG, -EROFS, -ENOSPC, -ENOMEM, or -errno.
 */
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
    rc = inode_new(em, d, S_IFDIR_ | (mode & 07777u), 0, &n);
    if (rc != 0) {
        return rc;
    }
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
        if (rc != 0) {
            d->nlink--;
            (void)inode_store(em, d);
        }
    }
    if (rc != 0) {
        n->nlink = 0;
        release_inode(em, n);
    }
    node_drop(em, n);
    return rc;
}

/**
 * The `rmdir` of `vfs_ops`: the empty directory `name` in `dir` removed: the
 * entry, then the parent's count lowered, then the inode freed. One that a
 * working directory or an open description holds keeps its inode and its
 * blocks until its last put, as an open file does; nothing can be added to
 * it, and a listing of it is -ENOENT, as on Linux. The batch is drained
 * first. The caller holds the mount's namespace lock. 0, -ENOENT, -ENOTDIR,
 * -ENOTEMPTY, -EROFS, or -errno.
 */
static int ext2_rmdir(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node loaded, *held;
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
    held = node_held(em, sc.ino);
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
    if (held) {
        held->nlink = 0;
        held->unlinked = 1;
        held->ctime = now_seconds();
        return inode_store(em, held);
    }
    loaded.nlink = 0;
    return release_inode(em, &loaded);
}

/**
 * The `link` of `vfs_ops`: a new name `name` in `dir` for the file `node`:
 * the count raised first, then the name written, so that a stop between
 * leaves a count one too high, which is a count repair; then the
 * directory's times. A failure to write the name lowers the count again.
 * The batch is drained first. The caller holds the mount's namespace lock.
 * 0, -EPERM for a directory, -EMLINK at 65000 links, -ENAMETOOLONG,
 * -EEXIST, -EROFS, -ENOSPC, or -errno.
 */
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
    return inode_times_later(em, d);
}

/**
 * The `symlink` of `vfs_ops`: a link `name` in `dir` to `target`. A target
 * shorter than 60 bytes is kept in the inode's pointer words, and a longer
 * one in a block of its own, written first; then the inode, then the entry,
 * then the directory's times. A failure frees the inode. The batch is
 * drained first. The caller holds the mount's namespace lock. 0,
 * -ENAMETOOLONG for a name past 255 bytes or for a target that is empty or
 * of a block or more, -EEXIST, -EROFS, -ENOSPC, -ENOMEM, or -errno.
 */
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
    rc = inode_new(em, d, S_IFLNK_ | 0777u, 0, &n);
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
    if (rc == 0) {
        d->mtime = d->ctime = now_seconds();
        rc = inode_times_later(em, d);
    }
    if (rc != 0) {
        n->nlink = 0;
        release_inode(em, n);
    }
    node_drop(em, n);
    return rc;
}

/**
 * The `rename` of `vfs_ops`: `oldname` in `olddir` to `newname` in `newdir`,
 * either directory NULL for the root. The batch is drained first, and the
 * caller holds the mount's namespace lock.
 *
 * A file: the count raised, the new entry written, the old entry removed,
 * the count lowered, so that the file has a name at every stop. A
 * destination file is replaced by rewriting its entry to the source's
 * inode, after which the victim loses a count and goes as an unlink's does.
 * A directory moves by a new entry, its `..` rewritten, the new parent's
 * count raised, the old entry removed and the old parent's count lowered; a
 * destination directory must be empty and goes first, as rmdir's does. The
 * old entry is found again before it is removed, since a removal in the
 * same directory may have moved it. Two names of one file change nothing.
 *
 * Returns 0; -ENOENT for a missing source or a removed destination
 * directory, which takes no entry; -EISDIR, -ENOTDIR, -ENOTEMPTY; -EINVAL
 * for a directory moved below itself, found by a walk up `..` before any
 * write; -EMLINK; -ENAMETOOLONG; -EROFS; -ENOSPC; or -errno.
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
    if (nd->unlinked) {
        return -ENOENT;
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
        return 0;
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
            victim->nlink = 0;
            if (victim == &victim_loaded) {
                rc = release_inode(em, victim);
            } else {
                victim->unlinked = 1;
                victim->ctime = now_seconds();
                rc = inode_store(em, victim);
            }
            if (rc != 0) {
                return rc;
            }
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
            nd->mtime = nd->ctime = now_seconds();
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
            n->nlink--;
            (void)inode_store(em, n);
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

/* ---- the mount-time check ---- */

/*
 * A volume whose valid bit is clear was not unmounted, and one whose error
 * bit is set was found faulty by an earlier mount. Either is checked
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
 * rather than taken from the disk, since a crash leaves them stale.
 *
 * The walk keeps its state on the heap rather than on the stack: the node
 * of each directory level, the hard links met with their expected and
 * found counts, the reached-inode set, one bit per inode of the volume,
 * and one bitmap block; the block bitmap is the one the FAT walks share.
 * A dirty volume whose set or bitmap the free memory cannot hold, a
 * directory deeper than the levels or more hard-linked files than the
 * table mounts read-only and says which.
 */
#define EXT2_CHECK_DEPTH 32  /* the directory levels the walk holds */
#define EXT2_CHECK_LINKS 512 /* the files with more than one link it counts */

/* A file with more than one link: its inode, its link count, and the names
 * the walk has found for it. */
struct ext2_link {
    uint32_t ino;
    uint32_t expect, found;
};

/*
 * The check's state: the first fault and its inode; the lost blocks, the
 * orphans and the `links` entries of `link`; the inode of each directory
 * level being walked; the set of reached inodes; and one bitmap block.
 */
struct ext2_check {
    struct ext2_mount *em;
    const char *fault;
    uint32_t fault_ino;
    uint32_t lost, orphans, links;
    struct ext2_link link[EXT2_CHECK_LINKS];
    struct ext2_node node[EXT2_CHECK_DEPTH];
    uint8_t *seen; /* `(inodes + 7) / 8` bytes */
    uint8_t bitmap[4096];
};

/**
 * The check's state and both bitmaps, taken together and given back
 * together: the block bitmap through `fat_walk_acquire`, which counts the
 * free frames first, and the state and inode set from the heap. NULL when
 * the heap cannot give any of them, with nothing held.
 */
static struct ext2_check *check_state(struct ext2_mount *em)
{
    struct ext2_check *c;

    if (fat_walk_acquire(em->blocks) != 0) {
        return 0;
    }
    c = kzalloc(sizeof(*c));
    if (c) {
        c->seen = kzalloc((em->inodes + 7) / 8);
    }
    if (!c || !c->seen) {
        fat_walk_release();
        if (c) {
            kfree(c);
        }
        return 0;
    }
    c->em = em;
    return c;
}

/** Free the check's state and inode set; the walk bitmap is given back
 * apart, by `fat_walk_release`. */
static void check_state_free(struct ext2_check *c)
{
    kfree(c->seen);
    kfree(c);
}

/** Record the fault `why` at inode `ino` when it is the first; -EIO, which
 * ends the walk. */
static int check_fault(struct ext2_check *c, uint32_t ino, const char *why)
{
    if (!c->fault) {
        c->fault = why;
        c->fault_ino = ino;
    }
    return -EIO;
}

/** Whether inode `ino` is in the set of reached inodes. */
static int seen_test(const struct ext2_check *c, uint32_t ino)
{
    return (c->seen[(ino - 1) >> 3] >> ((ino - 1) & 7)) & 1;
}

/** Put inode `ino` in the set of reached inodes. */
static void seen_set(struct ext2_check *c, uint32_t ino)
{
    c->seen[(ino - 1) >> 3] |= (uint8_t)(1u << ((ino - 1) & 7));
}

/** Mark block `b`, which inode `ino` names: 0, or -EIO with the fault
 * recorded for a block out of range or one marked already, a structural
 * block or one another pointer names. */
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

/**
 * Mark pointer block `b` of inode `ino`, at `level`, and what it names,
 * `index` being the data index its first entry stands for; `*count` gains
 * every block, data and pointer, and `*last` is the highest data index
 * named. A pointer block for an index at or past `limit`, the blocks the
 * size covers, is a fault. 0, or -EIO with the fault recorded or for a
 * failed read.
 */
static int check_level(struct ext2_check *c, uint32_t ino, uint32_t b, int level, uint32_t index,
                       uint32_t limit, uint32_t *count, uint32_t *last)
{
    struct ext2_mount *em = c->em;
    uint32_t per = em->block_size / 4;
    uint32_t span = 1, j;
    int i, rc;

    if (index >= limit) {
        return check_fault(c, ino, "a pointer block past the size");
    }
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
            rc = check_level(c, ino, p, level - 1, index + j * span, limit, count, last);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return 0;
}

/**
 * Mark every block `n` names and check them against its size and its
 * `i_blocks`. A fast symbolic link, a device, a pipe and a socket keep
 * none; `as_data` treats the inode as a file whatever its mode, for the
 * reserved inodes. 0, or -EIO with the fault recorded or for a failed read.
 */
static int check_blocks(struct ext2_check *c, const struct ext2_node *n, int as_data)
{
    const struct ext2_mount *em = c->em;
    uint32_t type = n->mode & S_IFMT_;
    uint32_t per = em->block_size / 4;
    uint32_t count = 0, last = 0, limit, i;
    int named = 0, rc;

    limit = (uint32_t)((n->size + em->block_size - 1) >> em->block_shift);
    if (!as_data && type != S_IFREG_ && type != S_IFDIR_ && (type != S_IFLNK_ || n->blocks == 0)) {
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
        rc =
            check_level(c, n->ino, n->block[EXT2_NDIR + i], (int)i + 1, base, limit, &count, &last);
        if (rc != 0) {
            return rc;
        }
        named = 1;
    }
    if (n->blocks != count * (em->block_size / 512)) {
        return check_fault(c, n->ino, "i_blocks that does not match its pointers");
    }
    if (named && last >= limit) {
        return check_fault(c, n->ino, "a block past the size");
    }
    return 0;
}

static int check_dir(struct ext2_check *c, uint32_t ino, uint32_t parent, int depth);

/**
 * Check the inode `ino` an entry of directory `dir` names, with the entry's
 * type byte `ft`, at `depth`: a directory is walked once, counted in
 * `*subdirs`, and a second reach is a second parent; a file's blocks are
 * marked once, and a second name counts toward its link count in the
 * table. The type byte, where the volume keeps one, must name the inode's
 * kind, since `ext2_readdir` reports the type from it. 0, or -EIO with the
 * fault recorded or for a failed read.
 */
static int check_child(struct ext2_check *c, uint32_t ino, uint32_t dir, uint32_t ft, int depth,
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
    if ((em->incompat & EXT2_INCOMPAT_FILETYPE) && ft != 0 && (ft > 7 || ft != ftype_of(n->mode))) {
        return check_fault(c, ino, "an entry type that does not match its inode");
    }
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

/**
 * Walk directory `ino`, loaded at `c->node[depth]`, whose parent is
 * `parent`: its blocks, every entry's form, `.` and `..` in their places,
 * each other entry's inode, and its link count against two plus its
 * subdirectories. 0, or -EIO with the fault recorded or for a failed read.
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
    if (n->size == 0 || (n->size & (em->block_size - 1)) != 0 || n->size > 0xFFFFFFFFu) {
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
                rc = check_child(c, eino, ino, raw[7], depth + 1, &subdirs);
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

/**
 * The inode scan and the bitmap compare, group by group. An orphan is an
 * inode at or past `s_first_ino` the bitmap calls used that the walk did
 * not reach, its blocks marked so that nothing else may claim them; one
 * whose link count is zero is a deleted inode whose bit was not yet
 * cleared, counted with them for `e2fsck` to clear, its pointers not read,
 * since they mean nothing, as `e2fsck` reads such an inode. A used block the
 * walk did not mark is lost. The free counts are recounted into the
 * descriptors in memory and into the mount's totals. 0, or -EIO with the
 * fault recorded or for a failed read.
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
                continue;
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

/**
 * Check a volume whose valid bit is clear or whose error bit is set, at a
 * writable mount or remount, before it is written, as the section above
 * says: the structural blocks and the reserved inodes below `s_first_ino`
 * marked first, then the tree walked from the root, the link counts
 * compared, and the bitmaps.
 *
 * Returns 0 with the volume writable, the recounted free counts written to
 * the descriptors and the superblock, and the error bit set as well when
 * blocks were lost or inodes orphaned; 1 with `*why` naming why it must stay
 * read-only, the error bit written and the counts as the disk has them,
 * since the recount may have stopped part way; or -errno when the device
 * failed.
 */
static int ext2_check_volume(struct ext2_mount *em, const char **why)
{
    struct ext2_check *c;
    uint64_t free_blocks = em->free_blocks, free_inodes = em->free_inodes;
    uint32_t g, ino;
    int rc;

    c = check_state(em);
    if (!c) {
        klog(KLOG_WARN, "ext2", "check: no memory for the %d KiB of workspace the volume needs",
             (int)((fat_walk_bytes(em->blocks) + (em->inodes + 7) / 8 + sizeof(*c)) / 1024));
        *why = "not unmounted cleanly and the check's workspace is more than the free memory";
        return 1;
    }
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
        check_state_free(c);
        return rc;
    }
    if (c->fault) {
        em->free_blocks = free_blocks;
        em->free_inodes = free_inodes;
        rc = read_bytes(em, (uint64_t)(em->first_data + 1) * em->block_size, em->gd,
                        em->groups * 32);
        if (rc != 0) {
            check_state_free(c);
            return rc;
        }
        klog(KLOG_WARN, "ext2", "check: inode %d: %s", (int)c->fault_ino, c->fault);
        check_state_free(c);
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
            check_state_free(c);
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
    check_state_free(c);
    return sb_store(em);
}

/* ---- the mount ---- */

/**
 * The largest regular file a volume with `1 << block_shift`-byte blocks
 * can hold: the direct, single, double and triple pointers' reach, capped
 * by the 32-bit `i_blocks` count of 512-byte units, which the pointer
 * blocks share. The cap leaves one part in `per` for those pointer blocks,
 * a little more than they take, since the kernel divides only by shifting.
 */
static uint64_t file_limit(uint32_t block_shift)
{
    uint64_t block_size = (uint64_t)1 << block_shift;
    uint64_t per = block_size / 4;
    uint64_t reach = (EXT2_NDIR + per + per * per + per * per * per) << block_shift;
    uint64_t units = (uint64_t)0xFFFFFFFFu * 512u;
    uint64_t cap = units - (units >> (block_shift - 2)) - 2 * block_size;

    cap &= ~(block_size - 1);
    return reach < cap ? reach : cap;
}

/** Log `ext2: refused: FIELD` and return -EINVAL, which fails the mount on
 * `field`. */
static int refuse(const char *field)
{
    klog(KLOG_WARN, "ext2", "refused: %s", field);
    return -EINVAL;
}

/**
 * Set up the file-write batch and clear the superblock's valid bit on the
 * medium, before the mount is published or made writable.
 *
 * Each buffer is one page of the heap, which is one cache block, so no
 * buffer needs pages next to another's. A mount that cannot have them all
 * fails with ENOMEM and leaves what it took to `writeback_free`. 0,
 * -ENOMEM, or the error of `sb_store`.
 */
static int writeback_init(struct ext2_mount *em)
{
    int i;

    em->wb = kzalloc(sizeof(*em->wb));
    if (!em->wb) {
        return -ENOMEM;
    }
    for (i = 0; i < EXT2_WB_DATA + EXT2_WB_META; i++) {
        em->wb->slot[i] = kalloc_pages(1, 0);
        if (!em->wb->slot[i]) {
            return -ENOMEM;
        }
    }
    em->state &= ~EXT2_VALID_FS;
    return sb_store(em);
}

/**
 * The constructor's work on the record `em`, which `ext2_mount` frees, with
 * its nodes, batch and descriptors, when this fails: the volume checked
 * field by field before anything is published, the superblock's magic,
 * revision, sizes and counts, the features against the three sets, the
 * descriptors against the volume, and the root inode; then, for a writable
 * mount, the check of a volume not cleanly unmounted and the batch. Returns
 * 0 with `mnt->priv` and `mnt->root` set and the record `ext2: mounted:
 * ...`, or the errors `vfs_ext2_type` lists in ext2.h.
 *
 * A group count whose descriptor table, 32 bytes a group, would overflow
 * the size asked of the heap or not fit the volume fails the mount, since the
 * product would wrap and the checks after it read past what was allocated.
 * Each group's two bitmaps and inode table must lie in that group, past its
 * superblock copy and descriptors when it has them, and apart from each
 * other; that is where `data_block_ok` looks for them.
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
    em->next_generation = now_seconds() << 8;
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
    em->file_limit = file_limit(em->block_shift);
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
        if (em->compat & EXT2_COMPAT_RESIZE_INODE) {
            em->reserved_gdt = le16(sb + 206);
        }
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
    if ((uint64_t)em->groups * em->ipg < em->inodes) {
        return refuse("inodes past the groups");
    }
    if ((uint64_t)em->first_data + 1 + gd_blocks(em) + em->reserved_gdt > em->blocks) {
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
    if (em->groups == 0 || em->groups > 0xFFFFFFFFu / 32 || em->blocks <= em->first_data ||
        (em->groups * 32 + em->block_size - 1) / em->block_size > em->blocks - em->first_data - 1) {
        klog(KLOG_WARN, "ext2", "refused: %d groups do not fit a table on %d blocks",
             (int)em->groups, (int)em->blocks);
        return -EINVAL;
    }
    em->gd = kmalloc(em->groups * 32);
    if (!em->gd) {
        return -ENOMEM;
    }
    rc = read_bytes(em, (uint64_t)(em->first_data + 1) * em->block_size, em->gd, em->groups * 32);
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

        if (hi > em->blocks) {
            hi = em->blocks;
        }
        if (has_super(em, g)) {
            lo += 1 + gd_blocks(em);
        }
        if (bb < lo || bb >= hi || ib < lo || ib >= hi || it < lo || tables == 0 ||
            (uint64_t)it + tables > hi || bb == ib || (bb >= it && bb < it + tables) ||
            (ib >= it && ib < it + tables)) {
            klog(KLOG_WARN, "ext2",
                 "refused: group %d: bitmaps at %d and %d, table at %d, "
                 "in blocks %d to %d",
                 (int)g, (int)bb, (int)ib, (int)it, (int)lo, (int)hi);
            return -EINVAL;
        }
        /* Against the group's own size: the last group is often short. */
        if (le16(gd + 12) > hi - (em->first_data + g * em->bpg) ||
            le16(gd + 14) > (g == em->groups - 1 ? em->inodes - g * em->ipg : em->ipg)) {
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
        node_drop(em, root); /* not ext2_put: the mount is not on `mnt` yet */
        return refuse("root inode");
    }
    if (!readonly_why && !(flags & VFS_MS_RDONLY) &&
        (!(em->state & EXT2_VALID_FS) || (em->state & EXT2_ERROR_FS))) {
        rc = ext2_check_volume(em, &readonly_why);
        if (rc < 0) {
            node_drop(em, root);
            return rc;
        }
    }
    em->writable = readonly_why == 0 && !(flags & VFS_MS_RDONLY);
    em->readonly_why = readonly_why;
    if (readonly_why) {
        klog(KLOG_WARN, "ext2", "mounted read-only: %s", readonly_why);
        mnt->flags |= VFS_MS_RDONLY;
    }
    em->alloc_hint = em->first_data;
    if (em->writable) {
        rc = writeback_init(em);
        if (rc != 0) {
            node_drop(em, root);
            return rc;
        }
        em->valid_cleared = 1;
    }
    mnt->priv = em;
    mnt->root = root;
    klog(KLOG_INFO, "ext2", "mounted: %d blocks of %d, %d inodes, %d groups%s", (int)em->blocks,
         (int)em->block_size, (int)em->inodes, (int)em->groups, em->writable ? "" : ", read-only");
    return 0;
}

/** The constructor of `vfs_ext2_type`, whose contract is in ext2.h: the
 * record from the heap, given to `ext2_mount_on` and freed, with its nodes,
 * batch and descriptors, when that fails. */
static int ext2_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                      const char *options)
{
    struct ext2_mount *em = kzalloc(sizeof(*em));
    int rc;

    (void)options;
    if (!em) {
        return -ENOMEM;
    }
    em->used = 1;
    rc = ext2_mount_on(em, mnt, dev, flags);
    if (rc != 0) {
        while (em->nodes) {
            node_drop(em, em->nodes);
        }
        writeback_free(em->wb);
        kfree(em->gd);
        kfree(em);
    }
    return rc;
}

static const struct vfs_ops ext2_ops = {
    ext2_lookup,
    0, /* lookup_rest */
    ext2_hold,
    ext2_put,
    ext2_getattr,
    ext2_setattr,
    ext2_open,
    ext2_release,
    ext2_read,
    ext2_write,
    ext2_truncate,
    ext2_fsync,
    ext2_readdir,
    ext2_create,
    ext2_unlink,
    ext2_mkdir,
    ext2_rmdir,
    ext2_rename,
    ext2_link,
    ext2_readlink,
    ext2_statfs,
    ext2_sync,
    ext2_prepare_unmount,
    ext2_abort_unmount, /* abort_unmount */
    ext2_unmount,
    ext2_path,
    ext2_symlink,
    ext2_mknod,
    0, /* pin: a hold keeps a node */
    0, /* unpin */
    ext2_remount,
    ext2_fallocate,
};

const struct vfs_type vfs_ext2_type = {"ext2", ext2_mount, &ext2_ops, 1, 0};
