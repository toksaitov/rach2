/*
 * ext2.c: the ext2 file system, read and written through the buffer cache;
 * the contract is in ext2.h and ext2.Readme.md.
 *
 * The locks, from the outermost:
 *
 *   lock         scope      covers                          waits on the disk
 *   rename       mount      the ancestry of a directory's   for the walk up
 *                           move to another parent          `..` alone
 *   node lock    one inode  its attributes, block map and   for that file's
 *                           a directory's entries           own requests
 *   em->meta     mount      the batch, bitmaps,             for one request,
 *                           descriptors and free counts     of a block evicted
 *                                                           after its fetch
 *   buffer       one block  the cache's rules (bcache.h)    for one request
 *
 * They are taken in that order: the rename lock, then directories, then
 * other inodes, then `em->meta`, then buffers. The rename lock is the
 * mount's (`vfs_mount.rename`), which the VFS takes for a rename between
 * two directories before it orders and locks them (`vfs_rename_lock`), so
 * `ext2_rename` and `is_below` run with it held. Related directories go
 * ancestor first; unrelated ones, and every inode after the directories,
 * go by inode number. A strict mount holds `em->meta` across its ordered
 * writes, as `sync` asks.
 *
 * The VFS takes a directory's lock around a change of its entries and a
 * file's around a write, a truncation and a change of attributes, and the
 * lock of the node a name names in unlink, rmdir and rename; the operations
 * here take the same locks again by depth, and those of the nodes they
 * reach themselves. A reader takes its node's lock alone and lets it go
 * around a block the cache lacks. A copy to or from a user buffer under a
 * node lock runs in one step with a `vm_resident` that passed; a page not
 * present is brought in with the locks let go, or, when the caller holds a
 * file lock of its own, ends the call short (`user_refault`).
 *
 * The node lock and `em->meta` count in their holder's `file_locks`
 * (process.h), so that reclaim under either does not enter a file system.
 * The type record sets `file_locked` and leaves `ns_serialised` clear, so
 * the mount's namespace lock covers nothing here: a slow request on one
 * file holds up only the callers that need that file's lock.
 */

#include "ext2.h"

#include "../mm/kheap.h"
#include "../mm/paging.h"

#include "../block/block.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../panic.h"
#include "../proc/process.h"
#include "../rtc.h"
#include "bcache.h"
#include "fatcommon.h"
#include "sleeplock.h"

#define EXT2_SUPER_MAGIC 0xEF53u
#define EXT2_ROOT_INO    2u
#define EXT2_NDIR        12u
#define EXT2_NBLOCKS     15u
#define EXT2_NAME_MAX    255u
#define EXT2_FAST_LINK   60u         /* the bytes of `i_block`, where a shorter target is kept */
#define EXT2_XATTR_MAGIC 0xEA020000u /* h_magic of an extended-attribute block */

/* The bits of `i_flags` lsattr(1) sees, and those chattr(1) may change. */
#define EXT2_FL_USER_VISIBLE    0x0003DFFFu
#define EXT2_FL_USER_MODIFIABLE 0x000380FFu

#define EXT2_COMPAT_RESIZE_INODE 0x0010u
#define EXT2_COMPAT_DIR_INDEX    0x0020u
#define EXT2_RO_SPARSE_SUPER     0x0001u
#define EXT2_RO_LARGE_FILE       0x0002u
#define EXT2_RO_EXTRA_ISIZE      0x0040u
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

/*
 * The fields of a large inode past its first 128 bytes, as offsets from the
 * start of the slot: `i_extra_isize`, which counts the bytes in use after
 * the first 128, then the extra words of the change, modification and
 * access times, and the creation time with its own. An extra word holds the
 * nanoseconds in its high 30 bits and, in its low two, the epoch: bits 32
 * and 33 added to the signed 32-bit seconds, which reach 2446. EXT2_LARGE
 * is the part of the slot this driver reads and writes, and
 * EXT2_EXTRA_NEW the extra bytes a new inode has, which cover every field.
 */
#define EXT2_GOOD_OLD     128u
#define EXT2_EXTRA_ISIZE  128u
#define EXT2_CTIME_EXTRA  132u
#define EXT2_MTIME_EXTRA  136u
#define EXT2_ATIME_EXTRA  140u
#define EXT2_CRTIME       144u
#define EXT2_CRTIME_EXTRA 148u
#define EXT2_LARGE        160u
#define EXT2_EXTRA_NEW    32u

/* The range of seconds an inode holds: 32 signed bits alone, and with the
 * two epoch bits of an extra word. */
#define EXT2_TIME_MIN       (-2147483647 - 1)
#define EXT2_TIME_MAX32     2147483647
#define EXT2_TIME_MAX_EXTRA (((int64_t)3 << 32) + 2147483647)

/* A time of an inode: seconds, and nanoseconds where the inode keeps them. */
struct ext2_time {
    int64_t sec;
    uint32_t nsec;
};

/* The times `stamp` sets. */
#define T_ATIME 1
#define T_MTIME 2
#define T_CTIME 4

/* The states of a node in memory. A node is published in its mount's hash
 * N_LOADING before its inode is read, and a second caller that finds it so
 * takes a hold and sleeps on `em->node_wq`; N_LIVE once read or made;
 * N_FAILED when that failed, after which the holds go and the last frees it;
 * N_DYING from the last put of an unlinked inode, after which it takes no
 * new hold and a caller sleeps until it has gone. */
#define N_LOADING 0
#define N_LIVE    1
#define N_DYING   2
#define N_FAILED  3

/* The shared blocks a node lists for its `fsync` (`node_share`). */
#define EXT2_NODE_SHARED 8

/* An inode in memory, from the heap, on its mount's list and in its hash. */
struct ext2_node {
    struct ext2_node *next;  /* the mount's list of nodes in memory */
    struct ext2_node *prev;  /* the node before it on that list, or NULL */
    struct ext2_node *hnext; /* the next node in its bucket of the mount's hash */
    uint32_t refs;           /* holds; the last put frees the node */
    uint32_t ino;
    uint32_t mode;
    uint32_t uid, gid;
    uint32_t nlink;
    uint64_t size;                /* 64 bits for a regular file, 32 for the rest */
    uint32_t blocks;              /* i_blocks, in 512-byte units */
    uint32_t block[EXT2_NBLOCKS]; /* 12 direct, then single, double and triple indirect */
    struct ext2_time atime, mtime, ctime;
    struct ext2_time crtime;     /* i_crtime, kept where `extra_isize` covers it */
    uint32_t extra_isize;        /* i_extra_isize; 0 for a 128-byte inode */
    uint32_t flags;              /* i_flags, which the VFS enforces in part */
    uint32_t generation;         /* i_generation, new for each inode made */
    uint32_t file_acl;           /* i_file_acl: the extended-attribute block, or 0 */
    uint32_t add_from;           /* a directory's offset of the block where the last entry went */
    int unlinked;                /* no name left; the inode and its blocks go at the last put */
    int state;                   /* N_LOADING, N_LIVE, N_DYING or N_FAILED */
    int load_error;              /* the -errno of a load that failed */
    struct sleep_lock lock;      /* the node lock (`node_lock`) */
    int queued;                  /* on `em->reap`, which no thread is freeing yet */
    struct ext2_node *reap_next; /* the next node on `em->reap` */
    /* The blocks other files share that were dirtied for this inode since
     * its last `fsync`, `nshared` of them: bitmaps, descriptors and the
     * superblock. `shared_over` once more were, after which its `fsync`
     * writes the whole device. Its table block is not listed, since an
     * `fsync` always writes it. */
    uint32_t shared[EXT2_NODE_SHARED];
    int nshared, shared_over;
    /* Set when a store changes a field of the inode other than its times,
     * and when the node is made, and cleared as an `fsync` writes the
     * table block: `fdatasync` writes that block only while it is set. */
    int dirty_meta;
};

/* `write_bytes`'s ways to write on a mount with `sync` or `dirsync`
 * (`em_strict`): through to the medium; through to the device without its
 * flush, which a later flush covers; and through to the medium on any
 * mount, for the superblock's counts and state. 0, and the first two on any
 * other mount, leave the bytes dirty in the cache for `sync`. On any mount
 * WRITE_FIRST and WRITE_AFTER leave them dirty in the cache's two classes,
 * a `first` block reaching the device before any `after` one (bcache.h):
 * `punch` clears pointers first and bits after. */
#define WRITE_THROUGH   1
#define WRITE_UNFLUSHED 2
#define WRITE_DURABLE   3
#define WRITE_FIRST     4
#define WRITE_AFTER     5

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
 * stage, whether the batch has changed it, the file it is of, and its
 * page. */
struct ext2_image {
    uint32_t block;
    int stage;
    int dirty;
    uint32_t owner; /* the inode a data or pointer block is of, or 0 */
    uint8_t *data;
};

/*
 * The file-write batch a mount keeps across calls: the images of the blocks
 * its writes change, `datas` and `metas` of them in use, and the `count`
 * inodes it holds; the bytes accepted since the last drain; the stage a
 * drain is at, where a failed one resumes; and the first failure's errno,
 * kept for the mount's life.
 */
struct ext2_writeback {
    struct ext2_image data[EXT2_WB_DATA];
    struct ext2_image meta[EXT2_WB_META];
    struct ext2_node *nodes[EXT2_WB_NODES];
    /* One heap page per buffer. A single run of 288 pages is more than a
     * reserve run of the heap can give, so it would have to come from the
     * ordinary region in one piece, and after the cache has moved much data
     * that region may have no such gap although memory is free, which would
     * fail a writable mount with ENOMEM. */
    uint8_t *slot[EXT2_WB_DATA + EXT2_WB_META];
    int datas, metas, count;
    uint32_t bytes;
    int stage, error;
};

/* The directories whose last lookup a mount remembers, enough for the walk
 * of a path eight directories deep. */
#define EXT2_LOOKUP_HINTS 8

/* The buckets of a mount's hash of its nodes before it has more nodes than
 * that, which the mount's record holds; a power of two. */
#define EXT2_NODE_FIRST 64

/* A mounted volume, from the heap, at `mnt->priv`. */
struct ext2_mount {
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
    uint32_t new_extra; /* the i_extra_isize of an inode made here, 0 on 128-byte inodes */
    uint32_t state;     /* s_state: EXT2_VALID_FS and EXT2_ERROR_FS */
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
    /* The thread ID of the process planning under `em->meta`, or -1: its
     * read of a block the cache lacks fails with -EAGAIN and leaves the
     * block in `missed`, for it to fetch with the lock let go. */
    int planner;
    uint32_t missed;
    uint32_t alloc_hint;     /* the block after the last one given out */
    uint8_t *gd;             /* the group descriptors, `groups * 32` bytes, from the heap */
    struct ext2_node *nodes; /* the nodes in memory, from the heap, linked both ways */
    /* The same nodes by inode number: `node_buckets` chains, a power of
     * two, in `node_first` until the nodes outnumber them and in a table
     * from the heap after; NULL and 0 before the first node. The name cache
     * holds a node for each name it keeps, thousands of them, and every
     * lookup that reaches the driver asks for its node by number. */
    struct ext2_node **node_hash;
    uint32_t node_buckets;
    uint32_t node_count;
    struct ext2_node *node_first[EXT2_NODE_FIRST];
    /* Where the last lookups found their names, one slot for each of the
     * last EXT2_LOOKUP_HINTS directories, since a path's walk looks up a
     * name in every directory on the way: the directory's inode and the
     * block where the next lookup in it starts. `lookup_next` is the slot a
     * directory without one takes. */
    struct {
        uint32_t dir, block;
    } lookup_hint[EXT2_LOOKUP_HINTS];
    int lookup_next;
    /* The allocator's lock (`meta_lock`), as the comment at the top of the
     * file says. */
    struct sleep_lock meta;
    /* Where a caller sleeps for a node that is loading or dying, and a
     * writer for admission while `closing` is set. */
    wait_queue_t node_wq;
    /* Nodes whose final put was left for `em_reap`, linked by `reap_next`.
     * Each counts in `writers`. A list needs no hold on the mount: every
     * path to an unmount passes `ext2_sync`, which empties it, and the
     * record outlives it. */
    struct ext2_node *reap;
    uint32_t nodes_gone; /* nodes freed, so that a waiter sees one go */
    uint32_t writers;    /* writers and final puts under way */
    int closing;         /* nonzero while a remount keeps new writers out */
    /* A node with shared blocks listed was freed, so the next `fsync` of
     * any file writes the whole device. */
    int shared_lost;
};

/* The kinds of ext2's locks, as `sleep_lock_records` lists them. */
static struct sleep_lock_record node_record = SLEEP_LOCK_RECORD("ext2 node");
static struct sleep_lock_record meta_record = SLEEP_LOCK_RECORD("ext2 meta");

/* The nodes every mount's hash has read in its searches, for /proc/stat. */
static uint32_t node_visits;

uint32_t ext2_node_visits(void)
{
    return node_visits;
}

/** The chain of the mount's hash where inode `ino` is, the hash set up
 * with its first buckets when the mount has none yet. */
static struct ext2_node **node_chain(struct ext2_mount *em, uint32_t ino)
{
    if (!em->node_hash) {
        em->node_hash = em->node_first;
        em->node_buckets = EXT2_NODE_FIRST;
    }
    return &em->node_hash[ino & (em->node_buckets - 1u)];
}

/** Double the buckets of the mount's hash once its nodes outnumber them,
 * every node moved to the new table; a heap with no room for the table
 * leaves the hash as it is, its chains longer. */
static void node_hash_grow(struct ext2_mount *em)
{
    uint32_t size = em->node_buckets * 2u;
    struct ext2_node **table, **old;
    struct ext2_node *n;

    if (em->node_count <= em->node_buckets || size == 0) {
        return;
    }
    table = kzalloc(size * sizeof(*table));
    if (!table) {
        return;
    }
    if (em->node_buckets * 2u != size) {
        kfree(table); /* the heap slept, and another caller grew the hash */
        return;
    }
    old = em->node_hash;
    em->node_hash = table;
    em->node_buckets = size;
    for (n = em->nodes; n; n = n->next) {
        struct ext2_node **chain = node_chain(em, n->ino);

        n->hnext = *chain;
        *chain = n;
    }
    if (old != em->node_first) {
        kfree(old);
    }
}

/** The mount's node in memory for inode `ino`, or NULL. */
static struct ext2_node *node_find(struct ext2_mount *em, uint32_t ino)
{
    struct ext2_node *n;

    for (n = *node_chain(em, ino); n; n = n->hnext) {
        node_visits++;
        if (n->ino == ino) {
            return n;
        }
    }
    return 0;
}

/** A new node from the heap, zeroed, N_LOADING and held once by the caller,
 * on no list and with no number, or NULL when the heap has none. The heap
 * may sleep, so a caller that looked a number up looks again afterwards. */
static struct ext2_node *node_make(void)
{
    struct ext2_node *n = kzalloc(sizeof(*n));

    if (n) {
        n->refs = 1;
        n->state = N_LOADING;
        n->dirty_meta = 1;
        sleep_lock_init(&n->lock, &node_record, SLEEP_LOCK_FILE);
    }
    return n;
}

/** Give node `n` of `node_make` the number `ino`, and put it on the mount's
 * list and in its hash in one step with the caller's look at the hash.
 * Every node a mount has in memory is on the list, so an unmount frees what
 * is left. */
static void node_publish(struct ext2_mount *em, struct ext2_node *n, uint32_t ino)
{
    struct ext2_node **chain;

    n->ino = ino;
    n->next = em->nodes;
    if (em->nodes) {
        em->nodes->prev = n;
    }
    em->nodes = n;
    chain = node_chain(em, ino);
    n->hnext = *chain;
    *chain = n;
    em->node_count++;
    node_hash_grow(em);
}

/** Take node `n` off the mount's list and hash and free it, waking a caller
 * that waits for a dying node to go. A live node's list of shared blocks
 * is lost with it, which `em->shared_lost` records. */
static void node_drop(struct ext2_mount *em, struct ext2_node *n)
{
    struct ext2_node **link;

    for (link = node_chain(em, n->ino); *link; link = &(*link)->hnext) {
        if (*link == n) {
            *link = n->hnext;
            break;
        }
    }
    if (n->prev) {
        n->prev->next = n->next;
    } else {
        em->nodes = n->next;
    }
    if (n->next) {
        n->next->prev = n->prev;
    }
    if (n->state == N_LIVE && !n->unlinked && (n->nshared || n->shared_over)) {
        em->shared_lost = 1;
    }
    em->node_count--;
    kfree(n);
    em->nodes_gone++;
    wq_wake_all(&em->node_wq);
}

/** Drop a hold on `n` that no other holder's unlinked inode depends on, and
 * free the node with the last: a load that failed, or a new inode's. */
static void node_unhold(struct ext2_mount *em, struct ext2_node *n)
{
    n->refs--;
    if (n->refs == 0) {
        node_drop(em, n);
    }
}

/** End the N_LOADING of node `n`, held by the caller: N_LIVE for an `rc` of
 * 0, and otherwise N_FAILED with the caller's hold dropped. Wakes every
 * caller waiting for it. */
static void node_settle(struct ext2_mount *em, struct ext2_node *n, int rc)
{
    n->state = rc == 0 ? N_LIVE : N_FAILED;
    n->load_error = rc;
    wq_wake_all(&em->node_wq);
    if (rc != 0) {
        node_unhold(em, n);
    }
}

/** Free every node of the mount and its hash's table, at its unmount or a
 * mount that failed. */
static void nodes_free(struct ext2_mount *em)
{
    while (em->nodes) {
        node_drop(em, em->nodes);
    }
    if (em->node_hash != em->node_first) {
        kfree(em->node_hash);
    }
    em->node_hash = 0;
    em->node_buckets = 0;
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

/* ---- the locks ---- */

/** Make the mount's locks free, its queue empty and admission open, before
 * anything else uses the record. */
static void em_locks_init(struct ext2_mount *em)
{
    sleep_lock_init(&em->meta, &meta_record, SLEEP_LOCK_FILE);
    wait_queue_init(&em->node_wq);
    em->reap = 0;
    em->writers = 0;
    em->closing = 0;
    em->planner = -1;
}

/** Whether the caller holds a node's lock or an allocator's, of any mount,
 * under which it may not free an inode or wait for one. */
static int file_locks_held(void)
{
    return current_proc && current_proc->file_locks > 0;
}

/**
 * Take the lock of node `n`, which covers its attributes, its block map and
 * a directory's entries, sleeping while another process holds it; the
 * holder takes it again by depth. The caller holds `n`, and takes no node
 * lock after `em->meta` and none against the order at the top of the file.
 */
static void node_lock(struct ext2_node *n)
{
    sleep_lock_take(&n->lock);
}

/** Give back one take of `n`'s lock, as `node_lock` took it. */
static void node_unlock(struct ext2_node *n)
{
    sleep_lock_give(&n->lock);
}

/** Panic unless the caller may change node `n`: it holds `n`'s lock. */
static void assert_node_locked(const struct ext2_node *n)
{
    sleep_lock_assert(&n->lock, "ext2: a change of an inode outside its lock");
}

/**
 * Take `em->meta`, which covers the batch, the bitmaps, the descriptors and
 * the free counts, sleeping while another process holds it; the holder
 * takes it again by depth. Taken after every node lock the caller needs,
 * and held across a disk wait only for one block evicted after its fetch,
 * or by a strict mount for its ordered writes.
 */
static void meta_lock(struct ext2_mount *em)
{
    sleep_lock_take(&em->meta);
}

/** Give back one take of `em->meta`, as `meta_lock` took it. */
static void meta_unlock(struct ext2_mount *em)
{
    sleep_lock_give(&em->meta);
}

/** Panic unless the caller may change the allocator's state: it holds
 * `em->meta`. */
static void assert_meta_locked(struct ext2_mount *em)
{
    sleep_lock_assert(&em->meta, "ext2: an allocation outside em->meta");
}

/** The wait condition of `em_enter`: admission is open on the mount `arg`. */
static int em_admitting(void *arg)
{
    return !((const struct ext2_mount *)arg)->closing;
}

/**
 * Admit a writer to the mount, sleeping while a remount keeps writers out
 * (`em->closing`), and count it in `em->writers` until its `em_leave`. The
 * caller checks once admitted whether the mount is still writable, since
 * the remount it waited for may have made it read-only.
 *
 * It may hold node locks while it waits, so an admitted writer takes no
 * lock beyond those it held when it was admitted, but a new or dying
 * inode's: the remount waits for every admitted writer, and one that slept
 * on a lock held by a writer that waits here would never end.
 */
static void em_enter(struct ext2_mount *em)
{
    while (em->closing) {
        wq_wait_event_nointr(&em->node_wq, em_admitting, em);
    }
    em->writers++;
}

/** End a writer `em_enter` admitted, waking a remount that waits for the
 * last. */
static void em_leave(struct ext2_mount *em)
{
    em->writers--;
    if (em->writers == 0 && em->closing) {
        wq_wake_all(&em->node_wq);
    }
}

/** The wait condition of `em_close`: no writer is left on the mount `arg`,
 * or a node waits on its list for a caller free to reap it. */
static int em_settled(void *arg)
{
    const struct ext2_mount *em = arg;

    return em->writers == 0 || em->reap != 0;
}

static void em_reap(struct ext2_mount *em);

/**
 * Close the mount to new writers for a remount and wait for those inside
 * and for every final put, reaping what is left on the mount's list, since
 * the thread that left a node there may do nothing more on this mount. The
 * caller holds no file lock, and a later remount waits for this one's
 * `em_open`.
 */
static void em_close(struct ext2_mount *em)
{
    while (em->closing) {
        wq_wait_event_nointr(&em->node_wq, em_admitting, em);
    }
    em->closing = 1;
    em_reap(em);
    while (em->writers) {
        wq_wait_event_nointr(&em->node_wq, em_settled, em);
        em_reap(em);
    }
}

/** Let writers in again after `em_close`. */
static void em_open(struct ext2_mount *em)
{
    if (em->closing) {
        em->closing = 0;
        wq_wake_all(&em->node_wq);
    }
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

static int writeback_error(struct ext2_mount *em, int rc);

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

/* The plans of one piece or allocation that may meet a block the cache
 * lacks and start again; the next reads it under `em->meta`. */
#define PLAN_TRIES 8

/** The thread ID that `em->planner` and the sleeping locks name the caller
 * by, 0 before the first process. */
static int plan_caller(void)
{
    return current_proc ? (int)current_proc->pid : 0;
}

/**
 * Cache block `blk` of the volume, held, as `bcache_get` gives it. While the
 * caller plans under `em->meta` (`em->planner`), a block the cache lacks is
 * not read: NULL with `*rc` -EAGAIN and the block in `em->missed`, for the
 * planner to fetch with the lock let go. NULL with -EIO for a failed read.
 */
static struct bbuf *volume_get(struct ext2_mount *em, uint64_t blk, int *rc)
{
    struct bbuf *b;

    if (em->planner == plan_caller() && !bcache_cached(em->dev, blk)) {
        em->missed = (uint32_t)blk;
        *rc = -EAGAIN;
        return 0;
    }
    b = bcache_get(em->dev, blk);
    *rc = b ? 0 : -EIO;
    return b;
}

/** Bring cache block `blk` into the cache when neither it nor the batch has
 * it, sleeping on the disk; the caller holds no buffer and not `em->meta`.
 * A failed read is left for the caller's own read to meet. */
static void volume_fetch(struct ext2_mount *em, uint64_t blk)
{
    struct bbuf *b;

    if (writeback_find(em, (uint32_t)blk) || bcache_cached(em->dev, blk)) {
        return;
    }
    b = bcache_get(em->dev, blk);
    if (b) {
        bcache_release(b);
    }
}

/**
 * Copy `len` bytes at byte offset `off` of the volume into `dst`, one cache
 * block at a time, from the batch's image of a block when it has one, since
 * the batch's writes are not in the cache yet. The offset is 64 bits, so the
 * volume's size is the block layer's business and not this reader's. 0,
 * -EIO, or -EAGAIN for a block the cache lacks under a plan (`volume_get`).
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
        int rc;

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
        b = volume_get(em, blk, &rc);
        if (!b) {
            return rc;
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

/** Whether inode `n`'s extra bytes reach `end`, an offset in its slot: the
 * extra field that ends there is kept. */
static int extra_has(const struct ext2_node *n, uint32_t end)
{
    return EXT2_GOOD_OLD + n->extra_isize >= end;
}

/** The time whose low word is at `raw + lo` and, where the inode's extra
 * bytes reach past `extra`, whose extra word is there. */
static struct ext2_time time_load(const struct ext2_node *n, const uint8_t *raw, uint32_t lo,
                                  uint32_t extra)
{
    struct ext2_time t;
    uint32_t word;

    t.sec = (int32_t)le32(raw + lo);
    t.nsec = 0;
    if (extra_has(n, extra + 4)) {
        word = le32(raw + extra);
        t.sec += (int64_t)(word & 3u) << 32;
        t.nsec = word >> 2;
    }
    return t;
}

/**
 * Load inode `ino` into `n`: its number checked against the count, its
 * table found through its group's descriptor. The first 128 bytes of the
 * slot are read, and of a larger slot the extra fields up to EXT2_LARGE;
 * the owner and group have their high halves at bytes 120 and 122, and a
 * regular file's size has its high word at byte 108. An `i_extra_isize`
 * that is not a multiple of 4 or passes the slot is -EIO, as on Linux's
 * ext4. 0, or -EIO.
 */
static int inode_load(struct ext2_mount *em, uint32_t ino, struct ext2_node *n)
{
    uint8_t raw[EXT2_LARGE];
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
    rc = read_bytes(em, at, raw, em->inode_size > EXT2_GOOD_OLD ? EXT2_LARGE : EXT2_GOOD_OLD);
    if (rc != 0) {
        return rc;
    }
    n->ino = ino;
    n->extra_isize = 0;
    if (em->inode_size > EXT2_GOOD_OLD) {
        n->extra_isize = le16(raw + EXT2_EXTRA_ISIZE);
        if ((n->extra_isize & 3u) || EXT2_GOOD_OLD + n->extra_isize > em->inode_size) {
            klog(KLOG_WARN, "ext2", "inode %d: extra size %d does not fit its %d bytes", (int)ino,
                 (int)n->extra_isize, (int)em->inode_size);
            return -EIO;
        }
    }
    n->mode = le16(raw + 0);
    n->uid = le16(raw + 2) | (le16(raw + 120) << 16);
    n->gid = le16(raw + 24) | (le16(raw + 122) << 16);
    n->size = le32(raw + 4);
    n->atime = time_load(n, raw, 8, EXT2_ATIME_EXTRA);
    n->ctime = time_load(n, raw, 12, EXT2_CTIME_EXTRA);
    n->mtime = time_load(n, raw, 16, EXT2_MTIME_EXTRA);
    n->crtime.sec = 0;
    n->crtime.nsec = 0;
    if (extra_has(n, EXT2_CRTIME + 4)) {
        n->crtime = time_load(n, raw, EXT2_CRTIME, EXT2_CRTIME_EXTRA);
    }
    n->nlink = le16(raw + 26);
    n->blocks = le32(raw + 28);
    n->flags = le32(raw + 32);
    n->generation = le32(raw + 100);
    n->file_acl = le32(raw + 104);
    for (i = 0; i < (int)EXT2_NBLOCKS; i++) {
        n->block[i] = le32(raw + 40 + i * 4);
    }
    if ((n->mode & S_IFMT_) == S_IFREG_) {
        n->size |= (uint64_t)le32(raw + 108) << 32;
    }
    return 0;
}

/** Whether `n` is a fast symbolic link, its target in the pointer words:
 * a link whose `i_blocks` counts no block but its extended-attribute
 * block, as Linux's `ext2_inode_is_fast_symlink` decides. */
static int fast_link(const struct ext2_mount *em, const struct ext2_node *n)
{
    uint32_t attr = n->file_acl ? em->block_size / 512 : 0;

    return (n->mode & S_IFMT_) == S_IFLNK_ && n->blocks == attr;
}

/* ---- the nodes in memory ---- */

/* What a caller that found a dying node waits for: a node of the mount has
 * gone since `gone` was read from `em->nodes_gone`. */
struct node_wait {
    const struct ext2_mount *em;
    uint32_t gone;
};

/** The wait condition of `node_outlive`, on a `struct node_wait`. */
static int node_went(void *arg)
{
    const struct node_wait *w = arg;

    return w->em->nodes_gone != w->gone;
}

/**
 * Wait for the dying node `n` to go, after which the caller looks in the
 * hash again; `n` is not to be read after. A node on the list that no thread
 * is freeing is reaped here, since the thread that left it may be the
 * caller. One that a caller under a file lock cannot reap is -EIO: a
 * directory names an inode with no link count, which only damage does. 0,
 * or -EIO.
 */
static int node_outlive(struct ext2_mount *em, struct ext2_node *n)
{
    struct node_wait w;

    if (n->queued) {
        if (file_locks_held()) {
            klog(KLOG_WARN, "ext2", "inode %d: named while it is freed", (int)n->ino);
            return -EIO;
        }
        em_reap(em);
        return 0;
    }
    w.em = em;
    w.gone = em->nodes_gone;
    wq_wait_event_nointr(&em->node_wq, node_went, &w);
    return 0;
}

/** The wait condition of `node_join`: the node `arg` is no longer loading. */
static int node_settled(void *arg)
{
    return ((const struct ext2_node *)arg)->state != N_LOADING;
}

/** Take a hold on `n`, which another caller may be loading, and wait for the
 * load. `n`, or NULL with `*rc` the failed load's -errno and no hold. */
static struct ext2_node *node_join(struct ext2_mount *em, struct ext2_node *n, int *rc)
{
    if (n->state == N_FAILED) {
        *rc = n->load_error;
        return 0;
    }
    n->refs++;
    while (n->state == N_LOADING) {
        wq_wait_event_nointr(&em->node_wq, node_settled, n);
    }
    if (n->state == N_FAILED) {
        *rc = n->load_error;
        node_unhold(em, n);
        return 0;
    }
    return n;
}

/**
 * The node for inode `ino`, held once more if the mount has it in memory
 * and loaded into a new record otherwise; NULL with `*rc` set, -ENOMEM when
 * the heap has no room or -EIO when the inode cannot be read. The record is
 * published N_LOADING before the load, which sleeps in the cache, so that a
 * second lookup meanwhile waits for it and does not make another; a dying
 * node is waited for, and the inode then looked up anew. The caller may hold
 * node locks: the load holds none, and a node that dies holds none it needs.
 */
static struct ext2_node *node_for(struct ext2_mount *em, uint32_t ino, int *rc)
{
    struct ext2_node *n, *fresh = 0;

    *rc = 0;
    for (;;) {
        n = node_find(em, ino);
        if (n && n->state == N_DYING) {
            *rc = node_outlive(em, n);
            if (*rc != 0) {
                n = 0;
                break;
            }
            continue;
        }
        if (n || fresh) {
            break;
        }
        fresh = node_make();
        if (!fresh) {
            klog(KLOG_WARN, "ext2", "node: no memory");
            *rc = -ENOMEM;
            break;
        }
    }
    if (n || *rc != 0) {
        kfree(fresh);
        return n ? node_join(em, n, rc) : 0;
    }
    node_publish(em, fresh, ino);
    *rc = inode_load(em, ino, fresh);
    node_settle(em, fresh, *rc);
    return *rc == 0 ? fresh : 0;
}

/** The `hold` of `vfs_ops`: one more hold on `node`. */
static void ext2_hold(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    ((struct ext2_node *)node)->refs++;
}

static int release_inode(struct ext2_mount *em, struct ext2_node *n);
static int sb_store(struct ext2_mount *em);
static int sb_note(struct ext2_mount *em);

static void mark_error(struct ext2_mount *em);

/**
 * Free the dying node `n` and its inode, on a writable mount; a read-only
 * mount frees no inode, so one a failure left there is an orphan for
 * e2fsck, and so is one whose free fails, which sets the error bit with the
 * record `ext2: inode N: not freed at its last put`. The caller holds no
 * file lock. Ends the count `node_final` took.
 */
static void node_free(struct ext2_mount *em, struct ext2_node *n)
{
    if (em->writable && !(em->mnt->flags & VFS_MS_RDONLY)) {
        int rc;

        node_lock(n);
        rc = release_inode(em, n);
        node_unlock(n);
        if (rc != 0) {
            klog(KLOG_WARN, "ext2", "inode %d: not freed at its last put", (int)n->ino);
            meta_lock(em);
            mark_error(em);
            meta_unlock(em);
        }
    }
    node_drop(em, n);
    em_leave(em);
}

/**
 * The end of an unlinked inode, whose last hold has gone: N_DYING, counted
 * as a writer without waiting, since a remount that is closing the mount
 * must see it and the caller may be a writer it waits for. A caller with no
 * file lock frees it now; any other leaves it on `em->reap`, since the free
 * takes node locks and `em->meta` out of order and waits on the disk.
 */
static void node_final(struct ext2_mount *em, struct ext2_node *n)
{
    n->state = N_DYING;
    em->writers++;
    if (file_locks_held()) {
        n->queued = 1;
        n->reap_next = em->reap;
        em->reap = n;
        if (em->closing) {
            wq_wake_all(&em->node_wq);
        }
        return;
    }
    node_free(em, n);
}

/**
 * Free the nodes `em->reap` holds, each after a drain of the batch, so that
 * no block is freed while an older write is pending, unless the caller
 * holds a file lock, which leaves them for the next caller. Called at the
 * end of an operation, at the last unlock of a node, at a put and in
 * `ext2_sync`, so an unmount leaves none.
 */
static void em_reap(struct ext2_mount *em)
{
    while (em->reap && !file_locks_held()) {
        struct ext2_node *n = em->reap;

        em->reap = n->reap_next;
        n->queued = 0;
        if (em->wb && em->wb->count) {
            (void)writeback_drain(em);
        }
        node_free(em, n);
    }
}

/**
 * The `put` of `vfs_ops`: drop a hold on `node`, and free the node with the
 * last, in one step, since a reference changes only between sleeps; it
 * takes no lock but those of an unlinked inode's free.
 *
 * An unlinked inode keeps its blocks until its last holder goes, which is
 * what makes an open unlinked file work, and a put of one by a caller with
 * no file lock drains the batch first, so no block is freed while an older
 * write is pending. Its last put marks it N_DYING and frees it on a writable
 * mount (`node_final`); a crash before this leaves an orphan for e2fsck.
 */
static void ext2_put(struct vfs_mount *mnt, void *node)
{
    struct ext2_node *n = node;
    struct ext2_mount *em = em_of(mnt);

    if (n->unlinked && !file_locks_held() && em->wb && em->wb->count) {
        (void)writeback_drain(em);
    }
    if (n->refs > 0) {
        n->refs--;
    }
    if (n->refs == 0) {
        if (n->unlinked) {
            node_final(em, n);
        } else {
            node_drop(em, n);
        }
    }
    em_reap(em);
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
 * Find `name` in directory `d`, whose lock the caller holds, for a lookup:
 * 1 with the inode it names in `*ino` and the entry's byte in `*at`, 0 when
 * it is absent, or -EIO.
 *
 * A directory of this format has no index, so the lookup reads entries
 * until it finds the name. It starts at the block where the last lookup in
 * the same directory found its name and wraps around to it, as Linux's
 * ext2 does: `ls` and `find` look up the names of a directory in the order
 * it lists them, so each is found near the one before, and a directory of
 * n names is listed in time proportional to n and not to n squared. A block
 * starts an entry whatever was written since, since no entry crosses one.
 */
static int dir_lookup(struct ext2_mount *em, const struct ext2_node *d, const char *name,
                      uint32_t *ino, uint32_t *at)
{
    uint32_t start = 0, cursor, limit;
    struct ext2_window w;
    int pass, slot;

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

            *at = cursor;
            got = dir_scan_in(em, d, &w, &cursor, &sc);
            if (got < 0) {
                window_close(&w);
                return got;
            }
            if (got == 0) {
                break;
            }
            if (sc.ino != 0 && names_equal(sc.name, name)) {
                if (slot < 0) {
                    slot = em->lookup_next;
                    em->lookup_next = (em->lookup_next + 1) % EXT2_LOOKUP_HINTS;
                    em->lookup_hint[slot].dir = d->ino;
                }
                em->lookup_hint[slot].block = *at >> em->block_shift;
                window_close(&w);
                *ino = sc.ino;
                return 1;
            }
        }
    }
    window_close(&w);
    return 0;
}

/** Whether the entry at byte `at` of directory `d` still names inode `ino`
 * as `name`, read under `d`'s lock: 1, 0, or -EIO. */
static int dir_names_at(struct ext2_mount *em, struct ext2_node *d, uint32_t at, const char *name,
                        uint32_t ino)
{
    struct ext2_scan sc;
    uint32_t cursor = at;
    int got;

    node_lock(d);
    got = dir_scan(em, d, &cursor, &sc);
    node_unlock(d);
    return got <= 0 ? got : sc.ino == ino && names_equal(sc.name, name);
}

/**
 * The `lookup` of `vfs_ops`: `name` in directory `dir`, the mount's root when
 * it is NULL. 0 with the node held, not locked; -ENOTDIR, -ENOENT, -ENOMEM,
 * or -EIO.
 *
 * The entry is found under the directory's lock, which the caller may hold
 * already, and the node is had with the lock let go, so that the read of a
 * cold inode holds up no other lookup in the directory and a second lookup
 * of the name waits on the load itself (`node_for`). When the lock did go,
 * the entry is read again under it: a name removed or moved meanwhile, whose
 * inode may have been freed or given to another file, is looked up anew.
 */
static int ext2_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node *n;
    uint32_t ino = 0, at = 0;
    int rc, kept;

    if ((d->mode & S_IFMT_) != S_IFDIR_) {
        return -ENOTDIR;
    }
    for (;;) {
        if (name[0] == '.' && name[1] == '\0') {
            rc = 1;
            ino = d->ino;
        } else {
            node_lock(d);
            rc = dir_lookup(em, d, name, &ino, &at);
            node_unlock(d);
        }
        if (rc <= 0) {
            return rc < 0 ? rc : -ENOENT;
        }
        if (ino == d->ino) {
            d->refs++;
            *node = d;
            return 0;
        }
        kept = sleep_lock_held(&d->lock);
        n = node_for(em, ino, &rc);
        if (!n) {
            return rc;
        }
        rc = kept ? 1 : dir_names_at(em, d, at, name, ino);
        if (rc == 1) {
            *node = n;
            return 0;
        }
        ext2_put(mnt, n);
        if (rc < 0) {
            return rc;
        }
    }
}

/* ---- attributes ---- */

/**
 * The `getattr` of `vfs_ops`: the inode's fields as the node has them, with
 * `i_blocks`, the generation and the flags lsattr sees, copied in one step
 * with no lock, since a change of a node is made between sleeps. A device's
 * numbers are in the old encoding in the first
 * pointer word, major and minor a byte each, or in the new one in the second
 * when the first is zero, and `rdev` packs them as the kernel does. Always
 * 0.
 */
static int ext2_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    struct ext2_node *n = node;

    (void)mnt;
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
    st->atime_sec = n->atime.sec;
    st->atime_nsec = n->atime.nsec;
    st->mtime_sec = n->mtime.sec;
    st->mtime_nsec = n->mtime.nsec;
    st->ctime_sec = n->ctime.sec;
    st->ctime_nsec = n->ctime.nsec;
    st->blocks = n->blocks;
    st->blocks_known = 1;
    st->flags = n->flags & EXT2_FL_USER_VISIBLE;
    st->generation = n->generation;
    st->flags_known = 1;
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
 * took put. */
static void ext2_release(struct vfs_mount *mnt, void *handle)
{
    (void)writeback_drain(em_of(mnt));
    ext2_put(mnt, handle);
}

/* The most times a read or a write brings the pages of a user buffer in
 * again with no byte moved between, before it answers -EFAULT, and the most
 * blocks a read fetches for one piece with the locks let go: the three
 * pointer levels and the data block. */
#define EXT2_REFAULTS 8
#define EXT2_FETCHES  4

/**
 * Bring in the pages of the user buffer [addr, addr + len) that a copy
 * under node `n`'s lock found absent (`vm_resident`), with `n`'s lock let
 * go around the faults, since a fault under a file lock could wait for
 * another file's; `write` is nonzero for a buffer the copy
 * fills. Returns 1 once the pages are in, after which `n` may have
 * changed; 0, with nothing let go, when the caller holds a file lock of
 * its own, `n`'s or another's, which this cannot give back, so that the
 * call ends short and its caller brings the pages in; or -EFAULT for
 * pages that cannot be had.
 */
static int user_refault(struct ext2_node *n, const void *addr, uint32_t len, int write)
{
    int mine = sleep_lock_held(&n->lock) ? 1 : 0;
    int rc;

    if (current_proc && (current_proc->file_locks != mine || n->lock.depth > 1)) {
        return 0;
    }
    node_unlock(n);
    rc = vm_prefault_range((uintptr_t)addr, len, write) ? 1 : -EFAULT;
    node_lock(n);
    return rc;
}

/**
 * The first block on the way to byte `at` of `n` that neither the cache
 * nor the batch has, a pointer block or the data block, into `*blk` as a
 * block of the cache: 1, or 0 when the way is had whole, ends in a hole, or
 * meets a fault, which the read under the lock then reports. Reads only
 * pointer blocks the cache has, so it waits on the disk only for one
 * evicted after the look.
 */
static int read_missing(struct ext2_mount *em, const struct ext2_node *n, uint64_t at,
                        uint64_t *blk)
{
    uint32_t per = em->block_size / 4;
    uint32_t index = (uint32_t)(at >> em->block_shift);
    uint32_t b, span = per, div = 1;
    int level, i;

    if (index < EXT2_NDIR) {
        b = n->block[index];
        level = 0;
    } else {
        index -= EXT2_NDIR;
        for (level = 1; level <= 3 && index >= span; level++) {
            index -= span;
            span *= per;
        }
        if (level > 3) {
            return 0;
        }
        b = n->block[EXT2_NDIR + level - 1];
        for (i = 1; i < level; i++) {
            div *= per;
        }
    }
    for (;;) {
        uint64_t off = (uint64_t)b * em->block_size;
        uint8_t ptr[4];

        if (b == 0 || !data_block_ok(em, b)) {
            return 0;
        }
        *blk = off >> BCACHE_BLOCK_SHIFT;
        if (!writeback_find(em, (uint32_t)*blk) && !bcache_cached(em->dev, *blk)) {
            return 1;
        }
        if (level == 0) {
            return 0;
        }
        if (read_bytes(em, off + (uint64_t)(index / div) * 4, ptr, 4) != 0) {
            return 0;
        }
        b = le32(ptr);
        index %= div;
        div /= per;
        level--;
    }
}

/**
 * Bring the first block on the way to byte `at` of `n` that the cache
 * lacks into it, with `n`'s lock let go, so that a disk that takes seconds
 * holds up no other call on the file. The block is only a hint here: the
 * caller, under the lock again, looks the file up anew, and a block freed
 * or evicted meanwhile costs it a read under it. Whether the lock was let
 * go, after which `n` may have changed; a caller that took it more than
 * once keeps it across the fetch.
 */
static int read_fetch(struct vfs_mount *mnt, struct ext2_node *n, uint64_t at)
{
    struct ext2_mount *em = em_of(mnt);
    uint64_t blk;
    struct bbuf *b;

    if (!read_missing(em, n, at, &blk)) {
        return 0;
    }
    node_unlock(n);
    b = bcache_get(em->dev, blk);
    if (b) {
        bcache_release(b);
    }
    node_lock(n);
    return 1;
}

/**
 * Copy `len` bytes of `n`'s data at `at`, inside one block, into `dst`, a
 * hole as zeros. The block, or the batch's image of it, is had first, and
 * the check that `dst` is present (`vm_resident`) and the copy follow it
 * with no sleep between, so that a page of a user buffer that went is found
 * before the copy and not by a fault under `n`'s lock. 0; -EFAULT, with
 * nothing copied, for a page of `dst` that is not present; or the errors of
 * `block_at`.
 */
static int read_piece(struct ext2_mount *em, const struct ext2_node *n, uint64_t at, uint8_t *dst,
                      uint32_t len)
{
    struct ext2_image *pending;
    struct bbuf *buf;
    uint64_t off;
    uint32_t b, in;
    int rc = block_at(em, n, (uint32_t)(at >> em->block_shift), &b);

    if (rc != 0) {
        return rc;
    }
    if (b == 0) {
        if (!vm_resident((uintptr_t)dst, len, 1)) {
            return -EFAULT;
        }
        k_memset(dst, 0, len);
        return 0;
    }
    off = (uint64_t)b * em->block_size + ((uint32_t)at & (em->block_size - 1));
    in = (uint32_t)off & (BCACHE_BLOCK_BYTES - 1);
    pending = writeback_find(em, (uint32_t)(off >> BCACHE_BLOCK_SHIFT));
    if (pending) {
        if (!vm_resident((uintptr_t)dst, len, 1)) {
            return -EFAULT;
        }
        k_memcpy(dst, pending->data + in, len);
        return 0;
    }
    buf = bcache_get(em->dev, off >> BCACHE_BLOCK_SHIFT);
    if (!buf) {
        return -EIO;
    }
    if (in + len > buf->bytes) {
        rc = -EIO;
    } else if (!vm_resident((uintptr_t)dst, len, 1)) {
        rc = -EFAULT;
    } else {
        k_memcpy(dst, buf->data + in, len);
    }
    bcache_release(buf);
    return rc;
}

/**
 * The `read` of `vfs_ops`: up to `count` bytes at `offset`, ending at the
 * size, the batch's pending writes included and a hole read as zeros, under
 * the node's lock, which a block the cache lacks on the way to the data
 * lets go (`read_fetch`); the access time does not change. A page of the
 * buffer that is not present at the copy is brought in with the lock let
 * go (`user_refault`), or ends the read short when the caller holds a file
 * lock. The bytes read, -EISDIR, -EFAULT, or -EIO.
 */
static int32_t ext2_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count,
                         uint64_t offset)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *n = handle;
    uint8_t *out = buf;
    uint32_t done = 0;
    int rc = 0, fetches = 0, refaults = 0;

    if ((n->mode & S_IFMT_) == S_IFDIR_) {
        return -EISDIR;
    }
    node_lock(n);
    while (rc == 0 && done < count && offset + done < n->size) {
        uint64_t at = offset + done;
        uint32_t take = em->block_size - ((uint32_t)at & (em->block_size - 1));

        if (fetches < EXT2_FETCHES && read_fetch(mnt, n, at)) {
            fetches++;
            continue;
        }
        if (take > count - done) {
            take = count - done;
        }
        if (take > n->size - at) {
            take = (uint32_t)(n->size - at);
        }
        rc = read_piece(em, n, at, out + done, take);
        if (rc == -EFAULT) {
            rc = ++refaults <= EXT2_REFAULTS ? user_refault(n, out + done, take, 1) : -EFAULT;
            if (rc == 1) {
                rc = 0;
                fetches = 0;
                continue;
            }
            break;
        }
        if (rc == 0) {
            done += take;
            fetches = 0;
            refaults = 0;
        }
    }
    node_unlock(n);
    if (rc == -EFAULT && done > 0) {
        rc = 0;
    }
    return rc != 0 ? rc : (int32_t)done;
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
 * The entry after byte `*cursor` of directory `d` that names an inode, for
 * `ext2_readdir`, whose caller holds `d`'s lock: 1 with `ent` filled and the
 * cursor past it, 0 at the end, -ENOENT for a removed directory, or -EIO.
 */
static int dir_next(struct ext2_mount *em, struct ext2_node *d, uint32_t *cursor,
                    struct vfs_dirent *ent)
{
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
 * The `readdir` of `vfs_ops`: the next entry with an inode of directory
 * `handle` at `*cursor`, a byte offset, `.` and `..` included. Its type comes
 * from the entry's file-type byte when the volume has the filetype feature,
 * and from the inode's mode otherwise, which costs a read of the inode. 1
 * with `ent` filled, 0 at the end, -ENOTDIR, -ENOENT for a directory removed
 * while held, or -EIO. Takes the directory's lock (`dir_next`).
 */
static int ext2_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor,
                        struct vfs_dirent *ent)
{
    struct ext2_node *d = handle;
    int rc;

    if ((d->mode & S_IFMT_) != S_IFDIR_) {
        return -ENOTDIR;
    }
    node_lock(d);
    rc = dir_next(em_of(mnt), d, cursor, ent);
    node_unlock(d);
    return rc;
}

/**
 * The target of symbolic link `n` into `buf`, as `ext2_readlink` gives it,
 * for a caller that holds `n`'s lock.
 */
static int link_target(struct ext2_mount *em, const struct ext2_node *n, char *buf, uint32_t cap)
{
    uint32_t len;

    if (n->size >= em->block_size) {
        return -EIO;
    }
    len = (uint32_t)n->size;
    if (len + 1 > cap) {
        return -ENAMETOOLONG;
    }
    if (fast_link(em, n)) {
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

/**
 * The `readlink` of `vfs_ops`: a symbolic link's target, from the inode's
 * pointer words when it is shorter than sixty bytes and the inode has no
 * blocks, from its first block otherwise, with a terminating NUL. Returns
 * the length; -ENAMETOOLONG when `cap` cannot hold it; -EINVAL for a node
 * that is not a link; or -EIO for a size of a block or more, a link of 60
 * bytes or more without blocks, a long link without its block, or a failed
 * read. Takes the link's lock (`link_target`).
 */
static int ext2_readlink(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct ext2_node *n = node;
    int rc;

    if ((n->mode & S_IFMT_) != S_IFLNK_) {
        return -EINVAL;
    }
    node_lock(n);
    rc = link_target(em_of(mnt), n, buf, cap);
    node_unlock(n);
    return rc;
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
 * Write the device's dirty buffers and flush the disk's cache, with no lock
 * of the mount held, as `vfs_sync_all` does, so that a disk that takes
 * seconds holds up no other call on the mount; a change made meanwhile is
 * the next sync's. A failure is recorded as the batch's error. 0, or
 * -errno.
 */
static int em_write_out(struct vfs_mount *mnt)
{
    struct ext2_mount *em = em_of(mnt);
    int rc;

    rc = bcache_sync(em->dev);
    if (rc == 0) {
        rc = block_flush(em->dev);
    }
    return rc != 0 ? writeback_error(em, rc) : 0;
}

/**
 * The `sync` of `vfs_ops`: the batch drained, the final puts left on
 * `em->reap` done, and, on a writable mount, the superblock's free counts
 * and state left dirty under `em->meta`, then the device's dirty buffers
 * written and the disk's cache flushed with no lock held (`em_write_out`).
 * The caller holds no file lock. Returns this sync's result, with a failure
 * recorded as the batch's error; the mount's first failure is what `fsync`
 * reports.
 */
static int ext2_sync(struct vfs_mount *mnt)
{
    struct ext2_mount *em = em_of(mnt);
    int rc;

    rc = writeback_drain(em);
    em_reap(em);
    if (rc == 0 && em->writable && !(mnt->flags & VFS_MS_RDONLY)) {
        volume_fetch(em, 0);
        meta_lock(em);
        rc = sb_note(em);
        meta_unlock(em);
    }
    return rc != 0 ? writeback_error(em, rc) : em_write_out(mnt);
}

/**
 * The `prepare_unmount` of `vfs_ops`, used by a remount read-only too:
 * `ext2_sync`, which leaves no final put undone, and then, on a writable
 * mount none of whose writes failed, the superblock's valid bit set again
 * when this mount cleared it and the error bit is clear, and the state
 * written to the medium under `em->meta`. 0, or -errno; a
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
    meta_lock(em);
    if (em->valid_cleared && !(em->state & EXT2_ERROR_FS)) {
        em->state |= EXT2_VALID_FS;
    }
    rc = sb_store(em);
    if (rc != 0) {
        em->state &= ~EXT2_VALID_FS;
        rc = writeback_error(em, rc);
    } else {
        em->valid_cleared = 0;
    }
    meta_unlock(em);
    return rc;
}

/**
 * The `abort_unmount` of `vfs_ops`: on a writable mount, the valid bit that
 * `ext2_prepare_unmount` set is cleared again in the superblock and the
 * device synced and flushed, since the unmount is not happening. A failure
 * is recorded as the batch's error. The state changes under `em->meta`,
 * and the sync and the flush run with no lock held.
 */
static void ext2_abort_unmount(struct vfs_mount *mnt)
{
    struct ext2_mount *em = em_of(mnt);
    struct bbuf *b;
    int rc = -EIO;

    if (!em->writable) {
        return;
    }
    meta_lock(em);
    em->state &= ~EXT2_VALID_FS;
    em->valid_cleared = 1;
    b = bcache_get(em->dev, 0);
    if (b) {
        b->data[1024 + 58] = (uint8_t)em->state;
        b->data[1024 + 59] = (uint8_t)(em->state >> 8);
        bcache_dirty(b);
        bcache_release(b);
    }
    meta_unlock(em);
    if (b) {
        rc = bcache_sync(em->dev);
        if (rc == 0) {
            rc = block_flush(em->dev);
        }
    }
    if (rc != 0) {
        (void)writeback_error(em, rc);
    }
}

static int writeback_init(struct ext2_mount *em);
static int ext2_check_volume(struct ext2_mount *em, const char **why);

/**
 * Make a writable mount read-only, its admission closed and no writer left:
 * the volume synced and its valid bit set, as an unmount leaves it. A node
 * removed while still open prevents it, since its inode is freed at its
 * last put and a read-only mount frees nothing: -EBUSY, as on Linux for such
 * a file. The scan comes after the writers went, so that it sees what they
 * unlinked. 0, or -errno.
 */
static int remount_readonly(struct vfs_mount *mnt, struct ext2_mount *em)
{
    struct ext2_node *n;
    int rc;

    for (n = em->nodes; n; n = n->next) {
        if (n->unlinked) {
            return -EBUSY;
        }
    }
    rc = ext2_prepare_unmount(mnt);
    if (rc == 0) {
        em->writable = 0;
    }
    return rc;
}

/**
 * Make a read-only mount writable: a volume whose valid bit is clear or
 * whose error bit is set is checked as a writable mount checks it, the batch
 * is set up when the mount has none, and the valid bit is cleared on the
 * medium. 0, -EROFS for a volume whose features or check keep it read-only,
 * -ENOMEM for a batch the heap cannot give, or -errno.
 *
 * The check holds no lock: admission is closed (`em_close`) and the mount
 * is read-only, so no writer, allocation or free of an inode changes what
 * it reads. The state and the batch change under `em->meta`.
 */
static int remount_writable(struct ext2_mount *em)
{
    const char *why = 0;
    int rc = 0;

    if (em->readonly_why) {
        return -EROFS;
    }
    if (!(em->state & EXT2_VALID_FS) || (em->state & EXT2_ERROR_FS)) {
        rc = ext2_check_volume(em, &why);
        if (rc == 0 && why) {
            em->readonly_why = why;
            rc = -EROFS;
        }
    }
    meta_lock(em);
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
    meta_unlock(em);
    return rc;
}

/**
 * The `remount` of `vfs_ops`: make the mount read-only or writable, by
 * VFS_MS_RDONLY in `flags`; a remount that keeps the mount as it is changes
 * nothing, and the options are ignored. Admission is closed first, so that
 * no writer is inside while the mount changes and new ones wait
 * (`em_close`). A failed write that makes the mount read-only is no remount:
 * it sets the flags at once and waits for nobody (`writeback_error`).
 */
static int ext2_remount(struct vfs_mount *mnt, uint32_t flags, const char *options)
{
    struct ext2_mount *em = em_of(mnt);
    int rc = 0;

    (void)options;
    em_close(em);
    if (((flags & VFS_MS_RDONLY) != 0) != !em->writable) {
        rc = (flags & VFS_MS_RDONLY) ? remount_readonly(mnt, em) : remount_writable(em);
    }
    em_open(em);
    return rc;
}

/** The `unmount` of `vfs_ops`: the mount's nodes, batch, descriptors and
 * record freed; a batch still pending on a removed device is recorded as a
 * failure first. */
static void ext2_unmount(struct vfs_mount *mnt)
{
    struct ext2_mount *em = em_of(mnt);

    if ((em->dev->flags & BLOCK_GONE) && em->wb && em->wb->count) {
        (void)writeback_error(em, -EIO);
    }
    nodes_free(em);
    mnt->priv = 0;
    writeback_free(em->wb);
    kfree(em->gd);
    kfree(em);
}

/* One directory of the bounded walk for a regular file's current name. */
struct ext2_path_level {
    uint32_t ino, cursor, length;
};

/**
 * The entry of directory `d`, whose lock the caller holds, that names inode
 * `ino` by a name other than `.` and `..`, into `sc`: 1, 0 when it has none,
 * or -EIO.
 */
static int dir_name_of(struct ext2_mount *em, const struct ext2_node *d, uint32_t ino,
                       struct ext2_scan *sc)
{
    uint32_t cursor = 0;

    for (;;) {
        int got = dir_scan(em, d, &cursor, sc);

        if (got <= 0) {
            return got;
        }
        if (sc->ino == ino && !names_equal(sc->name, ".") && !names_equal(sc->name, "..")) {
            return 1;
        }
    }
}

/** Whether node `d`, whose lock the caller holds, is a directory with a
 * name, whose entries may be read: one freed and given to another file
 * after a walk read its number is not. */
static int dir_live(const struct ext2_node *d)
{
    return (d->mode & S_IFMT_) == S_IFDIR_ && !d->unlinked;
}

/**
 * One step of the walk for a file's name: the next entry of directory
 * inode `ino` after byte `*cursor`, into `sc`, read under the directory's
 * lock with the node held, and whether the inode it names is a directory,
 * into `*is_dir`. 1, 0 at the directory's end, or -errno. The lock is let
 * go before the next step, so the walk holds one directory at a time.
 */
static int path_step(struct ext2_mount *em, uint32_t ino, uint32_t *cursor, struct ext2_scan *sc,
                     int *is_dir)
{
    int got;
    struct ext2_node *d = node_for(em, ino, &got);

    if (!d) {
        return got;
    }
    *is_dir = 0;
    node_lock(d);
    got = dir_live(d) ? dir_scan(em, d, cursor, sc) : 0;
    if (got > 0 && sc->ino != 0) {
        if (em->incompat & EXT2_INCOMPAT_FILETYPE) {
            *is_dir = sc->type == EXT2_FT_DIR;
        } else {
            struct ext2_node child;

            got = inode_load(em, sc->ino, &child);
            got = got != 0 ? got : 1;
            *is_dir = got == 1 && (child.mode & S_IFMT_) == S_IFDIR_;
        }
    }
    node_unlock(d);
    ext2_put(em->mnt, d);
    return got;
}

/** Find one live name of `wanted` below the root. Directories have no hard
 * links except . and .., which are not descended. The stack is on the heap,
 * bounded by the path capacity, and no recursive kernel stack is used; the
 * walk holds one directory's lock at a time (`path_step`). */
static int ext2_file_path(struct ext2_mount *em, uint32_t wanted, char *buf, uint32_t cap)
{
    struct ext2_path_level *levels;
    struct ext2_scan sc;
    uint32_t depth = 0, limit = cap / 2;
    int rc = -ENOENT, clipped = 0;

    if (limit == 0) {
        return -ENAMETOOLONG;
    }
    if (limit > PATH_DEPTH) {
        limit = PATH_DEPTH;
    }
    levels = kmalloc(limit * sizeof(*levels));
    if (!levels) {
        return -ENOMEM;
    }
    k_memset(levels, 0, limit * sizeof(*levels));
    levels[0].ino = EXT2_ROOT_INO;
    buf[0] = '\0';
    for (;;) {
        struct ext2_path_level *at = &levels[depth];
        uint32_t length = at->length;
        int got, is_dir;

        got = path_step(em, at->ino, &at->cursor, &sc, &is_dir);
        if (got < 0) {
            rc = got;
            break;
        }
        if (got == 0) {
            if (depth == 0) {
                rc = clipped ? -ENAMETOOLONG : -ENOENT;
                break;
            }
            depth--;
            continue;
        }
        if (sc.ino == 0 || names_equal(sc.name, ".") || names_equal(sc.name, "..")) {
            continue;
        }
        if (sc.ino == wanted) {
            if (length + sc.name_len + 1 >= cap) {
                rc = -ENAMETOOLONG;
                break;
            }
            buf[length] = '/';
            k_memcpy(buf + length + 1, sc.name, sc.name_len);
            buf[length + sc.name_len + 1] = '\0';
            rc = 0;
            break;
        }
        if (!is_dir) {
            continue;
        }
        if (length + sc.name_len + 1 >= cap || depth + 1 == limit) {
            clipped = 1;
            continue;
        }
        buf[length] = '/';
        k_memcpy(buf + length + 1, sc.name, sc.name_len);
        buf[length + sc.name_len + 1] = '\0';
        depth++;
        levels[depth].ino = sc.ino;
        levels[depth].cursor = 0;
        levels[depth].length = length + sc.name_len + 1;
    }
    kfree(levels);
    return rc;
}

/**
 * The parent of directory `cur`, held, into `*parent`, by its `..` entry,
 * read under `cur`'s lock; the node is had with the lock let go, as a
 * lookup has it, and the caller checks under the parent's own lock that it
 * is a directory still. 0, -ENOENT for a removed directory or one without
 * `..`, or -errno.
 */
static int dir_parent(struct ext2_mount *em, struct ext2_node *cur, struct ext2_node **parent)
{
    struct ext2_scan sc;
    uint32_t cursor = 0;
    int rc = 0;

    node_lock(cur);
    while (!cur->unlinked) {
        rc = dir_scan(em, cur, &cursor, &sc);
        if (rc <= 0 || (sc.ino != 0 && names_equal(sc.name, ".."))) {
            break;
        }
    }
    node_unlock(cur);
    if (rc == 1) {
        *parent = node_for(em, sc.ino, &rc);
    } else if (rc == 0) {
        rc = -ENOENT;
    }
    return rc;
}

/**
 * The `path` of `vfs_ops`: a regular file uses the bounded directory walk
 * above. A directory finds the parent by its `..` entry and the name by the
 * parent's entry that names the child's inode, up to the root, with one
 * directory's lock held at a time and each node on the way held. 0;
 * -ENAMETOOLONG past `cap` or PATH_DEPTH levels; -ENOENT without a live
 * name; -ENOMEM for the walk's stack; or the directory reader's error.
 */
static int ext2_path(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *n = node;
    struct ext2_node *cur = n;
    uint32_t len = 0;
    int depth, rc = 0;

    if ((n->mode & S_IFMT_) != S_IFDIR_) {
        return n->nlink ? ext2_file_path(em, n->ino, buf, cap) : -ENOENT;
    }
    buf[0] = '\0';
    for (depth = 0; cur->ino != EXT2_ROOT_INO && depth < PATH_DEPTH; depth++) {
        struct ext2_node *parent = 0;
        struct ext2_scan sc;
        uint32_t child = cur->ino;

        rc = dir_parent(em, cur, &parent);
        if (cur != n) {
            ext2_put(mnt, cur);
        }
        cur = parent;
        if (rc != 0) {
            break;
        }
        node_lock(cur);
        rc = dir_live(cur) ? dir_name_of(em, cur, child, &sc) : 0;
        node_unlock(cur);
        if (rc <= 0) {
            rc = rc < 0 ? rc : -ENOENT;
            break;
        }
        rc = 0;
        if (len + sc.name_len + 1 >= cap) {
            rc = -ENAMETOOLONG;
            break;
        }
        k_memmove(buf + sc.name_len + 1, buf, len + 1);
        buf[0] = '/';
        k_memcpy(buf + 1, sc.name, sc.name_len);
        len += sc.name_len + 1;
    }
    if (cur && cur != n) {
        ext2_put(mnt, cur);
    }
    if (rc != 0) {
        return rc;
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

/** Clamp seconds to ext2's signed 32-bit range and return the disk bits. */
static uint32_t time_seconds(int64_t sec)
{
    if (sec < -2147483647 - 1) {
        return 0x80000000u;
    }
    if (sec > 2147483647) {
        return 0x7FFFFFFFu;
    }
    return (uint32_t)sec;
}

/** The real time in seconds, clamped to ext2's signed 32-bit range, for
 * the deletion time and the generation. */
static uint32_t now_seconds(void)
{
    int64_t sec = 0, nsec = 0;

    rtc_realtime(&sec, &nsec);
    return time_seconds(sec);
}

/**
 * The time `sec` and `nsec` as inode `n` can hold it in the field whose
 * extra word ends at `extra_end`: with the extra word, seconds from 1901
 * to 2446 and the nanoseconds; without, ext2's signed 32-bit seconds and
 * no nanoseconds. Seconds past either end are clamped to it.
 */
static struct ext2_time time_fit(const struct ext2_node *n, uint32_t extra_end, int64_t sec,
                                 int64_t nsec)
{
    struct ext2_time t;
    int64_t max = extra_has(n, extra_end) ? EXT2_TIME_MAX_EXTRA : EXT2_TIME_MAX32;

    t.sec = sec < EXT2_TIME_MIN ? EXT2_TIME_MIN : sec > max ? max : sec;
    t.nsec = 0;
    if (extra_has(n, extra_end) && nsec > 0 && nsec < 1000000000) {
        t.nsec = (uint32_t)nsec;
    }
    return t;
}

/** Set the times of `n` that `which` selects (T_ATIME, T_MTIME, T_CTIME)
 * to the real time, as finely as the inode keeps each. */
static void stamp(struct ext2_node *n, int which)
{
    int64_t sec = 0, nsec = 0;

    rtc_realtime(&sec, &nsec);
    if (which & T_ATIME) {
        n->atime = time_fit(n, EXT2_ATIME_EXTRA + 4, sec, nsec);
    }
    if (which & T_MTIME) {
        n->mtime = time_fit(n, EXT2_MTIME_EXTRA + 4, sec, nsec);
    }
    if (which & T_CTIME) {
        n->ctime = time_fit(n, EXT2_CTIME_EXTRA + 4, sec, nsec);
    }
}

/** Put time `t` into the low word at `raw + lo` and, where the extra bytes
 * of `n` reach past `extra`, the extra word there. `t` is in the range
 * `time_fit` gives. */
static void time_put(const struct ext2_node *n, uint8_t *raw, uint32_t lo, uint32_t extra,
                     struct ext2_time t)
{
    uint32_t low = (uint32_t)(uint64_t)t.sec;
    uint32_t epoch = (uint32_t)((uint64_t)(t.sec - (int32_t)low) >> 32) & 3u;

    LE32_PUT(raw + lo, low);
    if (extra_has(n, extra + 4)) {
        LE32_PUT(raw + extra, epoch | (t.nsec << 2));
    }
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

/** List shared block `block` among those `n`'s next `fsync` writes, or
 * mark the list overflowed when it is full. */
static void node_share(struct ext2_node *n, uint32_t block)
{
    int i;

    for (i = 0; i < n->nshared; i++) {
        if (n->shared[i] == block) {
            return;
        }
    }
    if (n->nshared == EXT2_NODE_SHARED) {
        n->shared_over = 1;
        return;
    }
    n->shared[n->nshared++] = block;
}

/** List for `n` the bitmap at byte `bitmap` of group `g`'s descriptor, 0
 * for blocks and 4 for inodes, and the block of that descriptor. */
static void node_share_group(struct ext2_mount *em, struct ext2_node *n, uint32_t g,
                             uint32_t bitmap)
{
    node_share(n, le32(em->gd + g * 32 + bitmap));
    node_share(n, em->first_data + 1 + (g * 32) / BCACHE_BLOCK_BYTES);
}

/**
 * Copy `len` bytes from `src`, or zeros for NULL, to byte offset `off` of
 * the volume, written the way `sync` says (WRITE_THROUGH and the rest), or
 * left dirty with 0, tagged as a block of inode `owner` alone when that is
 * nonzero (`bcache_dirty_owned`), for a data or pointer block of a file. A
 * whole block is not read first, since nothing of it survives. A block the
 * file-write batch has an image of gets the bytes in its image too, in the
 * same step as the buffer, so that the drain, which copies the image over
 * the cache's block, keeps them: an inode stored into a table block the
 * batch holds, a bit beside the batch's reservations, a descriptor or the
 * superblock's counts. 0, or -errno.
 */
static int write_owned(struct ext2_mount *em, uint32_t owner, uint64_t off, const void *src,
                       uint32_t len, int sync)
{
    const uint8_t *in = src;

    while (len > 0) {
        uint64_t blk = off >> BCACHE_BLOCK_SHIFT;
        uint32_t at = (uint32_t)off & (BCACHE_BLOCK_BYTES - 1);
        uint32_t take = BCACHE_BLOCK_BYTES - at;
        struct ext2_image *pending;
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
        pending = writeback_find(em, (uint32_t)blk);
        if (pending) {
            k_memcpy(pending->data + at, b->data + at, take);
        }
        if (sync == WRITE_DURABLE || (sync == WRITE_THROUGH && em_strict(em))) {
            rc = bcache_write(b);
        } else if (sync == WRITE_UNFLUSHED && em_strict(em)) {
            rc = bcache_write_unflushed(b);
        } else if (sync == WRITE_FIRST || sync == WRITE_AFTER) {
            if (owner) {
                bcache_dirty_owned(b, owner);
            }
            bcache_dirty_ordered(b, sync == WRITE_FIRST ? BCACHE_ORDER_FIRST : BCACHE_ORDER_AFTER);
        } else if (owner) {
            bcache_dirty_owned(b, owner);
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

/** `write_owned` of a block no one file owns: an inode table, a bitmap, a
 * descriptor, the superblock, a directory's entries. */
static int write_bytes(struct ext2_mount *em, uint64_t off, const void *src, uint32_t len, int sync)
{
    return write_owned(em, 0, off, src, len, sync);
}

/**
 * Whether a change of attributes or of the namespace may be made: 0, -EROFS
 * on a read-only mount, or the drain's -errno. A strict mount drains the
 * batch first, so that the change's writes through reach the medium after
 * the file data and pointers an inode it stores names. A mount that writes
 * back does not: `write_bytes` keeps the batch's images current, and
 * `punch` drains a batch that holds the file it frees. The caller holds the
 * node lock the change needs.
 */
static int assert_writable(struct vfs_mount *mnt, struct ext2_mount *em)
{
    int rc = 0;

    if (em_strict(em)) {
        rc = writeback_drain(em);
    }
    if (rc == 0 && (!em->writable || (mnt->flags & VFS_MS_RDONLY))) {
        rc = -EROFS;
    }
    return rc;
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

/**
 * Encode the fields of `n` this driver keeps into the inode `raw`, leaving
 * its other bytes as they are; the deletion time is cleared. On a volume of
 * large inodes `raw` has EXT2_LARGE bytes or more, and the extra size and
 * the extra fields it covers are written too.
 */
static void inode_encode(const struct ext2_mount *em, const struct ext2_node *n, uint8_t *raw)
{
    int i;

    LE16_PUT(raw + 0, n->mode);
    LE16_PUT(raw + 2, n->uid & 0xFFFFu);
    LE32_PUT(raw + 4, (uint32_t)n->size);
    time_put(n, raw, 8, EXT2_ATIME_EXTRA, n->atime);
    time_put(n, raw, 12, EXT2_CTIME_EXTRA, n->ctime);
    time_put(n, raw, 16, EXT2_MTIME_EXTRA, n->mtime);
    LE32_PUT(raw + 20, 0); /* i_dtime: a live inode has none, whatever it had */
    LE16_PUT(raw + 24, n->gid & 0xFFFFu);
    LE16_PUT(raw + 26, n->nlink);
    LE32_PUT(raw + 28, n->blocks);
    LE32_PUT(raw + 32, n->flags);
    for (i = 0; i < (int)EXT2_NBLOCKS; i++) {
        LE32_PUT(raw + 40 + i * 4, n->block[i]);
    }
    LE32_PUT(raw + 100, n->generation);
    LE32_PUT(raw + 104, n->file_acl);
    LE16_PUT(raw + 120, (n->uid >> 16) & 0xFFFFu);
    LE16_PUT(raw + 122, (n->gid >> 16) & 0xFFFFu);
    if ((n->mode & S_IFMT_) == S_IFREG_) {
        LE32_PUT(raw + 108, (uint32_t)(n->size >> 32));
    }
    if (em->inode_size > EXT2_GOOD_OLD) {
        LE16_PUT(raw + EXT2_EXTRA_ISIZE, n->extra_isize);
        if (extra_has(n, EXT2_CRTIME + 4)) {
            time_put(n, raw, EXT2_CRTIME, EXT2_CRTIME_EXTRA, n->crtime);
        }
    }
}

/** Whether byte `at` of an inode slot holds one of its times: the access,
 * change and modification times, their extra words and the creation time. */
static int inode_time_byte(uint32_t at)
{
    return (at >= 8 && at < 20) || (at >= EXT2_CTIME_EXTRA && at < EXT2_CRTIME_EXTRA + 4);
}

/** `inode_encode` of `n` over the `len` bytes of its slot at `raw`, setting
 * `n->dirty_meta` when a byte other than a time's changes. */
static void inode_encode_noting(const struct ext2_mount *em, struct ext2_node *n, uint8_t *raw,
                                uint32_t len)
{
    uint8_t was[EXT2_LARGE];
    uint32_t i;

    k_memcpy(was, raw, len);
    inode_encode(em, n, raw);
    for (i = 0; i < len && !n->dirty_meta; i++) {
        n->dirty_meta = raw[i] != was[i] && !inode_time_byte(i);
    }
}

/**
 * Write inode `n` in the way `how` says, over the bytes its slot has, or over
 * zeros when `fresh`, for an inode just allocated: a freed inode's slot keeps
 * what it had, and the encoder writes only the fields this driver keeps. A
 * fresh inode is written over its whole slot, so that a large slot loses the
 * attributes a freed inode kept in it; any other store writes the part of the
 * slot the encoder covers. A regular file of 2 GiB or more sets `large_file`
 * first.
 *
 * An inode the file-write batch holds is stored from the node, whose pointers
 * may name blocks the batch has not put in the cache yet. That is sound on a
 * mount that writes back, which promises no order, and a strict mount drains
 * the batch before any store but the batch's own (`assert_writable`). A
 * change of a field but the times sets `n->dirty_meta`, and the superblock
 * is listed among `n`'s shared blocks when `large_file` is set for it. 0,
 * or -errno.
 */
static int inode_write(struct ext2_mount *em, struct ext2_node *n, int fresh, int how)
{
    uint8_t raw[256];
    uint64_t at = inode_offset(em, n->ino);
    uint32_t len = em->inode_size > EXT2_GOOD_OLD ? EXT2_LARGE : EXT2_GOOD_OLD;
    int rc;

    if (fresh) {
        len = em->inode_size;
        k_memset(raw, 0, len);
        n->dirty_meta = 1;
        inode_encode(em, n, raw);
    } else {
        rc = read_bytes(em, at, raw, len);
        if (rc != 0) {
            return rc;
        }
        inode_encode_noting(em, n, raw, len);
    }
    if ((n->mode & S_IFMT_) == S_IFREG_ && n->size >= 0x80000000u &&
        !(em->ro_compat & EXT2_RO_LARGE_FILE)) {
        node_share(n, 0);
        rc = large_file_flag(em);
        if (rc != 0) {
            return rc;
        }
    }
    return write_bytes(em, at, raw, len, how);
}

/** Write inode `n` as `inode_write` does with WRITE_THROUGH: the fields this
 * driver keeps, over the bytes the slot has, so that what it does not keep
 * stays as it was. */
static int inode_store(struct ext2_mount *em, struct ext2_node *n)
{
    return inode_write(em, n, 0, WRITE_THROUGH);
}

/**
 * Write a directory's times into the cache, for the next sync to put on
 * the disk: after an entry is added or removed only the times are left,
 * and a crash that loses them loses nothing else, since every inode of
 * the block goes out whole with each write through of it. 0, or -errno.
 */
static int inode_times_later(struct ext2_mount *em, struct ext2_node *d)
{
    return inode_write(em, d, 0, 0);
}

/** Write the primary descriptor of group `g` in the way `how` says; the
 * backup tables are not written. 0, or -errno. */
static int gd_write(struct ext2_mount *em, uint32_t g, int how)
{
    uint64_t at = (uint64_t)(em->first_data + 1) * em->block_size + (uint64_t)g * 32;

    return write_bytes(em, at, em->gd + g * 32, 32, how);
}

/**
 * `gd_write` with WRITE_THROUGH. While the batch holds blocks the descriptor
 * counts them taken and the cache's bitmap does not yet show them: a stop
 * then leaves a free count below the bitmap's, which the mount's check
 * recounts and e2fsck repairs.
 */
static int gd_store(struct ext2_mount *em, uint32_t g)
{
    return gd_write(em, g, WRITE_THROUGH);
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
        (void)writeback_error(em, -EIO);
    } else if (em->writable) {
        (void)sb_store(em);
    }
}

/** Write the superblock's free counts and its state as `write_bytes` does
 * with `sync`; 0, or -errno. */
static int sb_write(struct ext2_mount *em, int sync)
{
    uint8_t raw[8];
    int rc;

    LE32_PUT(raw, (uint32_t)em->free_blocks);
    LE32_PUT(raw + 4, (uint32_t)em->free_inodes);
    rc = write_bytes(em, 1024 + 12, raw, 8, sync);
    if (rc != 0) {
        return rc;
    }
    LE16_PUT(raw, em->state);
    return write_bytes(em, 1024 + 58, raw, 2, sync);
}

/** Write the superblock's free counts and its state to the medium, on any
 * mount; 0, or -errno. */
static int sb_store(struct ext2_mount *em)
{
    return sb_write(em, WRITE_DURABLE);
}

/** Leave the superblock's free counts and its state dirty in the cache, for
 * the sync that follows; 0, or -errno. */
static int sb_note(struct ext2_mount *em)
{
    return sb_write(em, 0);
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
 * in the way `how` says; 0, or -errno. The caller holds `em->meta`, so that
 * no other change of the byte comes between its read and its write. */
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
    return write_bytes(em, at, &byte, 1, how);
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
 * reserved blocks are and the caller is not privileged, or -errno. The
 * caller holds `em->meta`. Under a plan (`volume_get`) a bitmap the cache
 * lacks is -EAGAIN, met before the first change, since every read of the
 * search and of the chosen bit's byte comes before its write.
 */
static int block_alloc(struct ext2_mount *em, uint32_t hint, uint32_t *out)
{
    uint32_t g0, g, tried;
    uint32_t start_in;

    assert_meta_locked(em);
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
 * that may not hold data, or -errno. The caller holds `em->meta`. */
static int block_free(struct ext2_mount *em, uint32_t b)
{
    uint32_t g, i;
    uint8_t *gd;
    int rc;

    assert_meta_locked(em);
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
 * Allocate an inode for the new node `n` of `node_make`, in the group of
 * inode `parent_ino` when it has one free, else in the next group that has,
 * wrapping; an inode below `s_first_ino` is never given, and neither is one
 * in the hash, whose bit may be clear while it dies, so that nothing here
 * waits for a node. `n` is published under the number in one step with the
 * search, before the bit is set and the counts lowered, the group's
 * directory count raised for `is_dir`, and the caller writes the inode
 * initialised and names it, in that order. The bit and the counts have no
 * order between them, so the bitmap's write leaves the disk's cache to the
 * flush of the descriptor's, which puts both on the medium before the inode
 * is written. 0, -ENOSPC, or -errno, with `n->ino` nonzero when `n` was
 * published. The caller holds `em->meta`.
 */
static int inode_alloc(struct ext2_mount *em, uint32_t parent_ino, int is_dir, struct ext2_node *n)
{
    uint32_t g0 = (parent_ino - 1) / em->ipg;
    uint32_t tried;

    assert_meta_locked(em);
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
        for (;;) {
            rc = bit_first_clear(em, le32(gd + 4), from, limit, &i);
            if (rc != 0) {
                return rc;
            }
            if (i >= limit || !node_find(em, base + i + 1)) {
                break;
            }
            from = i + 1;
        }
        if (i >= limit) {
            continue;
        }
        node_publish(em, n, base + i + 1);
        node_share_group(em, n, g, 4);
        rc = bit_write(em, le32(gd + 4), i, 1, WRITE_UNFLUSHED);
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
        return 0;
    }
    return -ENOSPC;
}

/** Clear the bit of inode `ino` and raise its group's free count, lowering
 * the directory count for `was_dir`, once the inode itself was written
 * freed; 0, or -errno. The caller holds `em->meta`. */
static int inode_bit_free(struct ext2_mount *em, uint32_t ino, int was_dir)
{
    uint32_t g = (ino - 1) / em->ipg;
    uint32_t i = (ino - 1) % em->ipg;
    uint8_t *gd = em->gd + g * 32;
    int rc;

    assert_meta_locked(em);
    rc = bit_put(em, le32(gd + 4), i, 0);
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

/**
 * Drop inode `ino`'s reference to its extended-attribute block `b`, once the
 * inode was written freed: the block is freed when the reference count in
 * its header is 1, and the count lowered otherwise, as Linux's
 * `ext2_xattr_delete_inode` does, so that a block several inodes share
 * stays until the last of them goes. A block out of range, or whose header
 * lacks the magic number or a length of one block, is left as it is with
 * the error bit set, for e2fsck. 0, or -errno.
 */
static int attr_release(struct ext2_mount *em, uint32_t ino, uint32_t b)
{
    uint64_t at = (uint64_t)b * em->block_size;
    uint8_t raw[12];
    uint32_t refs;
    int rc;

    if (!data_block_ok(em, b)) {
        klog(KLOG_WARN, "ext2", "inode %d: attribute block %d is out of range", (int)ino, (int)b);
        mark_error(em);
        return 0;
    }
    rc = read_bytes(em, at, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }
    if (le32(raw) != EXT2_XATTR_MAGIC || le32(raw + 8) != 1) {
        klog(KLOG_WARN, "ext2", "inode %d: attribute block %d has no valid header", (int)ino,
             (int)b);
        mark_error(em);
        return 0;
    }
    refs = le32(raw + 4);
    if (refs > 1) {
        LE32_PUT(raw + 4, refs - 1);
        return write_bytes(em, at + 4, raw + 4, 4, WRITE_THROUGH);
    }
    return block_free(em, b);
}

/**
 * Bring into the cache, with `em->meta` let go, the bitmap block and the
 * descriptor block of the group that holds block `hint`, which an
 * allocation from that hint reads first.
 */
static void group_fetch(struct ext2_mount *em, uint32_t hint)
{
    uint32_t g;

    if (hint < em->first_data || hint >= em->blocks) {
        hint = em->first_data;
    }
    g = (hint - em->first_data) / em->bpg;
    volume_fetch(em, le32(em->gd + g * 32));
    volume_fetch(em, em->first_data + 1 + (g * 32) / BCACHE_BLOCK_BYTES);
}

/**
 * Allocate one block from `hint` into `*out` by `block_alloc`, under
 * `em->meta`, which waits on the disk only for a block evicted again since
 * its fetch. The hint's group is fetched first with the lock let go, and a
 * search that meets another bitmap the cache lacks lets the lock go, fetches
 * it and starts again, PLAN_TRIES times before it reads under the lock. The
 * caller holds the lock of the node it allocates for and not `em->meta`. 0,
 * or the errors of `block_alloc`.
 */
static int alloc_block(struct ext2_mount *em, uint32_t hint, uint32_t *out)
{
    uint32_t missed;
    int tries, rc;

    group_fetch(em, hint);
    for (tries = 0;; tries++) {
        meta_lock(em);
        em->planner = tries < PLAN_TRIES ? plan_caller() : -1;
        rc = block_alloc(em, hint, out);
        em->planner = -1;
        missed = em->missed;
        meta_unlock(em);
        if (rc != -EAGAIN) {
            return rc;
        }
        volume_fetch(em, missed);
    }
}

/* ---- the block map, written ---- */

/* The blocks a free of a file collects before it frees them, and the
 * collection when the heap has no room for that many. */
#define PUNCH_BLOCKS 512
#define PUNCH_SMALL  16

/*
 * One `punch`'s blocks, on its caller's stack: the `count` blocks whose
 * pointers are cleared and whose bits are not, of the `cap` that `blocks`
 * holds. Each punch has its own, so that two of them, of two files and
 * interleaved at their disk waits, share nothing but the cache's order.
 */
struct punch_list {
    uint32_t *blocks;
    int count, cap;
};

/* `punch_level`'s answer when the list is full: the walk stops, the blocks
 * listed are freed, and it starts again from the end. */
#define PUNCH_FULL 1

/**
 * Clear the pointers of `n`'s blocks from data index `keep` on below the
 * pointer `*slot`, of `level` (0 for a data block) and covering the data
 * from index `base`, from the end, and list each block whose pointer is
 * cleared in `list`. A pointer is cleared at `slot_at` on the volume,
 * dirtied `first`, or for 0 in the inode in memory, which the caller stores;
 * a pointer block whose children are all gone goes the same way. No bit
 * changes here. `*emptied` says whether the pointer is now 0, and
 * `n->blocks` is kept current. 0, PUNCH_FULL when the list has no room for
 * the next block, or -errno, -EIO for a pointer block out of range.
 */
static int punch_level(struct ext2_mount *em, struct ext2_node *n, uint32_t *slot, uint64_t slot_at,
                       int level, uint32_t base, uint32_t keep, struct punch_list *list,
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
            rc = punch_level(em, n, &child, at, level - 1, child_base, keep, list, &gone);
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
    if (list->count == list->cap) {
        return PUNCH_FULL;
    }
    if (slot_at != 0) {
        uint8_t zero[4] = {0, 0, 0, 0};

        rc = write_owned(em, n->ino, slot_at, zero, 4, WRITE_FIRST);
        if (rc != 0) {
            return rc;
        }
    }
    *slot = 0;
    list->blocks[list->count++] = b;
    if (n->blocks >= sectors) {
        n->blocks -= sectors;
    }
    *emptied = 1;
    return 0;
}

/** Clear `n`'s pointers from data index `keep` on by `punch_level`, listing
 * their blocks: the triple, double and single indirect trees first, then the
 * direct blocks from the last. 0, PUNCH_FULL, or -errno. */
static int punch_from(struct ext2_mount *em, struct ext2_node *n, uint32_t keep,
                      struct punch_list *list)
{
    uint32_t per = em->block_size / 4;
    int level, i, rc, gone;

    for (level = 3; level >= 1; level--) {
        uint32_t lbase = EXT2_NDIR, lspan = per;
        int l;

        for (l = 1; l < level; l++) {
            lbase += lspan;
            lspan *= per;
        }
        if (lbase + lspan > keep && n->block[EXT2_NDIR + level - 1] != 0) {
            rc = punch_level(em, n, &n->block[EXT2_NDIR + level - 1], 0, level, lbase, keep, list,
                             &gone);
            if (rc != 0) {
                return rc;
            }
        }
    }
    for (i = (int)EXT2_NDIR - 1; i >= (int)keep && i >= 0; i--) {
        if (n->block[i] != 0) {
            rc = punch_level(em, n, &n->block[i], 0, 0, (uint32_t)i, keep, list, &gone);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return 0;
}

/**
 * Free the `list->count` blocks of `list`, whose pointers in `n` are
 * cleared, a group at a time: the group's bitmap and descriptor block
 * fetched with `em->meta` let go, then under it each bit cleared and the
 * counts raised, the bitmap and descriptor dirtied `after`, so that they
 * reach the device after every pointer and inode dirtied `first`, and
 * listed among `n`'s shared blocks. The list is emptied. A
 * block that may not hold data is skipped with a record and makes the
 * answer -EIO. 0, or -errno.
 */
static int punch_free(struct ext2_mount *em, struct ext2_node *n, struct punch_list *list)
{
    int rc = 0, i;

    while (list->count > 0) {
        uint32_t g, first = list->blocks[0];
        uint8_t *gd;
        int freed = 0, left = 0;

        if (!data_block_ok(em, first)) {
            klog(KLOG_WARN, "ext2", "free of block %d, which is out of range", (int)first);
            list->blocks[0] = list->blocks[--list->count];
            rc = -EIO;
            continue;
        }
        g = (first - em->first_data) / em->bpg;
        gd = em->gd + g * 32;
        group_fetch(em, first);
        meta_lock(em);
        for (i = 0; i < list->count; i++) {
            uint32_t b = list->blocks[i];
            int err;

            if (!data_block_ok(em, b) || (b - em->first_data) / em->bpg != g) {
                list->blocks[left++] = b;
                continue;
            }
            err = bit_write(em, le32(gd + 0), (b - em->first_data) % em->bpg, 0, WRITE_AFTER);
            if (err != 0) {
                rc = rc != 0 ? rc : err;
                continue;
            }
            LE16_PUT(gd + 12, le16(gd + 12) + 1);
            em->free_blocks++;
            freed = 1;
        }
        list->count = left;
        if (freed) {
            int err = gd_write(em, g, WRITE_AFTER);

            rc = rc != 0 ? rc : err;
            node_share_group(em, n, g, 0);
        }
        meta_unlock(em);
    }
    return rc;
}

/** Whether the file-write batch holds node `n`, whose pending pieces may
 * name blocks a free of `n` would give up. */
static int writeback_holds(const struct ext2_mount *em, const struct ext2_node *n)
{
    int i;

    for (i = 0; em->wb && i < em->wb->count; i++) {
        if (em->wb->nodes[i] == n) {
            return 1;
        }
    }
    return 0;
}

/**
 * Free the blocks of `n` from data index `keep` on, in two phases a round,
 * under `n`'s lock. First up to PUNCH_BLOCKS pointers are cleared from the
 * end and the inode stored, all dirtied `first`, their blocks collected on
 * the caller's stack; then those blocks are freed a group at a time with
 * their bitmaps and descriptors dirtied `after` (`punch_free`). The cache
 * writes every `first` block before any `after` one, whichever path writes
 * (bcache.h), so no pointer on the disk names a block whose bit is clear;
 * two punches of two files keep that by the same two classes and share
 * nothing else. A strict mount syncs between the phases and after the last.
 * Freed one by one and written through, a shrink would cost three requests
 * a block.
 *
 * A batch that holds `n` is drained first, so that no block is freed while a
 * pending piece names it. A failure still frees what its round listed, in
 * the same order, once the inode is stored and a strict mount's sync has
 * put the pointers on the medium; otherwise the blocks stay taken, lost
 * blocks for e2fsck, and the free counts stay exact. A device that failed a write is -EIO and not
 * written to again: the blocks go to the cache first and would reach the device at the sync that
 * ends the round, past the check a write through makes. The caller holds `n`'s lock and not
 * `em->meta`. 0, or -errno.
 */
static int punch(struct ext2_mount *em, struct ext2_node *n, uint32_t keep)
{
    uint32_t small[PUNCH_SMALL];
    struct punch_list list;
    int rc, err;

    assert_node_locked(n);
    if (em->dev->flags & BLOCK_WRITE_FAILED) {
        return -EIO;
    }
    if (writeback_holds(em, n)) {
        rc = writeback_drain(em);
        if (rc != 0) {
            return rc;
        }
    }
    list.blocks = kmalloc(PUNCH_BLOCKS * sizeof(uint32_t));
    list.cap = list.blocks ? PUNCH_BLOCKS : PUNCH_SMALL;
    if (!list.blocks) {
        list.blocks = small;
    }
    do {
        list.count = 0;
        rc = punch_from(em, n, keep, &list);
        if (list.count == 0) {
            break;
        }
        err = inode_write(em, n, 0, WRITE_FIRST);
        if (err == 0) {
            err = em_sync(em);
        }
        if (err == 0) {
            err = punch_free(em, n, &list);
        }
        if (err == 0) {
            err = em_sync(em);
        }
        if (err != 0 && rc >= 0) {
            rc = err;
        }
    } while (rc == PUNCH_FULL);
    if (list.blocks != small) {
        kfree(list.blocks);
    }
    if (rc == 0) {
        em_balance(em);
    }
    return rc;
}

/**
 * The block for data index `index` of `n`, allocated when missing, into
 * `*out`, with `*fresh` set for a new data block. A missing pointer block is
 * allocated and its zeros written before the pointer to it, so that the
 * pointer cannot reach the disk over what the block held before; an inode
 * that has that pointer is stored at once. A data block's pointer is
 * written after the block is in the bitmap. `n->blocks` and the inode's own
 * pointers change in memory; the caller stores the inode with them, and
 * holds `n`'s lock and not `em->meta`. 0, -EFBIG past the triple indirect's
 * reach, -ENOSPC, or -errno.
 */
static int map_alloc(struct ext2_mount *em, struct ext2_node *n, uint32_t index, uint32_t *out,
                     int *fresh)
{
    uint32_t per = em->block_size / 4;
    uint32_t sectors = em->block_size / 512;
    uint32_t *slot;
    uint32_t held;        /* a slot's value, read from its pointer block */
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
                rc = alloc_block(em, hint, &b);
                if (rc != 0) {
                    return rc;
                }
                node_share_group(em, n, (b - em->first_data) / em->bpg, 0);
                rc = write_owned(em, n->ino, (uint64_t)b * em->block_size, 0, em->block_size, 1);
                if (rc != 0) {
                    return rc;
                }
                n->blocks += sectors;
                if (slot_at != 0) {
                    uint8_t raw[4];

                    LE32_PUT(raw, b);
                    rc = write_owned(em, n->ino, slot_at, raw, 4, WRITE_THROUGH);
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
            held = le32(ptr);
            slot = &held;
            rest %= div;
            level--;
        }
    }
    if (*slot == 0) {
        uint32_t b;

        rc = alloc_block(em, hint, &b);
        if (rc != 0) {
            return rc;
        }
        node_share_group(em, n, (b - em->first_data) / em->bpg, 0);
        n->blocks += sectors;
        if (slot_at != 0) {
            uint8_t raw[4];

            LE32_PUT(raw, b);
            rc = write_owned(em, n->ino, slot_at, raw, 4, WRITE_THROUGH);
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
 * -EIO for an image the batch has at another stage or a failed read,
 * -ENOMEM for a full table, and -EAGAIN for a block the cache lacks under a
 * plan (`volume_get`). A piece that fails gives back the new images it took
 * (`writeback_try`); an image the batch had is not changed here.
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
        b = volume_get(em, block, rc);
        if (!b) {
            return 0;
        }
        k_memcpy(im->data, b->data, BCACHE_BLOCK_BYTES);
        bcache_release(b);
    }
    im->block = block;
    im->stage = stage;
    im->dirty = 0;
    im->owner = 0;
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
        parent->owner = n->ino;
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
        if (wb->nodes[plan->node_index] == n) {
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

/** The batch's image of the plan's data block, a block of inode `owner`:
 * the one it has, or a new one, zeroed for a new block or one the piece of
 * `len` bytes writes whole, and a copy of the cache's block otherwise. 0,
 * -ENOMEM when the table is full, -EIO, or -EAGAIN under a plan
 * (`volume_get`). */
static int writeback_prepare_data(struct ext2_mount *em, uint32_t owner, uint32_t len,
                                  struct ext2_write_plan *plan)
{
    struct ext2_writeback *wb = em->wb;
    struct ext2_image *data;
    int i, rc;

    for (i = 0; i < wb->datas && wb->data[i].block != plan->block; i++) {
    }
    if (i == wb->datas) {
        struct bbuf *b;

        if (i == EXT2_WB_DATA) {
            return -ENOMEM;
        }
        data = &wb->data[i];
        if (!plan->fresh && len != BCACHE_BLOCK_BYTES) {
            b = volume_get(em, plan->block, &rc);
            if (!b) {
                return rc;
            }
            k_memcpy(wb->slot[i], b->data, BCACHE_BLOCK_BYTES);
            bcache_release(b);
        } else {
            k_memset(wb->slot[i], 0, BCACHE_BLOCK_BYTES);
        }
        data->block = plan->block;
        data->stage = WB_DATA;
        data->data = wb->slot[i];
        data->dirty = 0;
        wb->datas++;
    }
    plan->data = &wb->data[i];
    plan->data->owner = owner;
    return 0;
}

/**
 * Commit a planned piece, with no I/O and nothing that can fail: the
 * reserved blocks taken in their bitmap and descriptor images and in the
 * free count, the pointer path and the inode's pointers and `i_blocks` set,
 * the superblock's `large_file` flag set, the inode held by the batch, the
 * bytes copied into the data image, and the size and times raised. Reads
 * see the piece at once, since `read_bytes` looks in the images first. The
 * bitmaps, descriptors and superblock it changes are listed among `n`'s
 * shared blocks (`node_share`).
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
        node_share_group(em, n, r->group, 0);
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
        node_share(n, 0);
        em->ro_compat |= EXT2_RO_LARGE_FILE;
        LE32_PUT(plan->feature->data + 1024 + 100, em->ro_compat);
        plan->feature->dirty = 1;
    }
    if (plan->node_index == wb->count) {
        wb->nodes[plan->node_index] = n;
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
    stamp(n, T_MTIME | T_CTIME);
    plan->table->dirty = 1;
    wb->bytes += len;
}

/**
 * Bring into the cache, with no lock but `n`'s held, what the plan of the
 * piece of `len` bytes at `offset` of `n` reads: the inode's table block, the
 * pointer path, the data block of a partial write, the hint's group's bitmap
 * and descriptor block, and the superblock when the file reaches 2 GiB
 * without `large_file`. A hint only: what is evicted again before the plan
 * costs it a restart.
 */
static void writeback_fetch(struct ext2_mount *em, struct ext2_node *n, uint64_t offset,
                            uint32_t len)
{
    uint32_t block = 0;

    volume_fetch(em, inode_offset(em, n->ino) >> BCACHE_BLOCK_SHIFT);
    if (block_at(em, n, (uint32_t)(offset >> 12), &block) == 0 && block != 0 &&
        len != BCACHE_BLOCK_BYTES) {
        volume_fetch(em, block);
    }
    group_fetch(em, em->alloc_hint);
    if (offset + len >= 0x80000000u && !(em->ro_compat & EXT2_RO_LARGE_FILE)) {
        volume_fetch(em, 0);
    }
}

/**
 * Plan, prepare and commit one piece, as `writeback_piece` says, under
 * `em->meta`. A failure gives back the images the piece took before it
 * returns, in the same step as the plan: once the lock goes, other writers
 * change the tables and the counts. 0, or -errno, -EAGAIN for a block the
 * cache lacks under a plan.
 */
static int writeback_try(struct ext2_mount *em, struct ext2_node *n, uint64_t offset,
                         const uint8_t *src, uint32_t len)
{
    struct ext2_writeback *wb = em->wb;
    struct ext2_write_plan plan;
    int old_metas = wb->metas, old_datas = wb->datas;
    int rc;

    assert_meta_locked(em);
    k_memset(&plan, 0, sizeof(plan));
    rc = writeback_plan(em, n, offset, len, &plan);
    if (rc != 0) {
        goto fail;
    }
    rc = writeback_prepare_data(em, n->ino, len, &plan);
    if (rc != 0) {
        goto fail;
    }
    if (em->dev->flags & (BLOCK_GONE | BLOCK_WRITE_FAILED) || em->mnt->flags & VFS_MS_RDONLY) {
        rc = -EIO;
        goto fail;
    }
    if (!vm_resident((uintptr_t)src, len, 0)) {
        rc = -EFAULT;
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
 * Accept one piece of a write, `len` bytes from `src` at `offset` of `n`
 * within one block: planned, its data image prepared, and committed. No
 * image readers see changes until every read and reservation has
 * succeeded, and a piece that fails gives back the images it took. A
 * removed device, one that failed a write, or a mount made read-only is
 * -EIO. `src` may be a user buffer: the check that its pages are present
 * runs in the step of the commit, which copies it, and a page that is not
 * is -EFAULT with nothing committed.
 *
 * What the plan reads is fetched first with `em->meta` let go
 * (`writeback_fetch`). Under the lock the plan reads only what the cache
 * has; a block it lacks gives the piece back, lets the lock go, fetches the
 * block and starts again, and after PLAN_TRIES such starts the plan reads
 * under the lock, one request. The caller holds `n`'s lock and not
 * `em->meta`. 0, or -errno.
 */
static int writeback_piece(struct ext2_mount *em, struct ext2_node *n, uint64_t offset,
                           const uint8_t *src, uint32_t len)
{
    uint32_t missed;
    int tries, rc;

    writeback_fetch(em, n, offset, len);
    for (tries = 0;; tries++) {
        meta_lock(em);
        em->planner = tries < PLAN_TRIES ? plan_caller() : -1;
        rc = writeback_try(em, n, offset, src, len);
        em->planner = -1;
        missed = em->missed;
        meta_unlock(em);
        if (rc != -EAGAIN) {
            return rc;
        }
        volume_fetch(em, missed);
    }
}

/**
 * Record a failure `rc` of the batch: the first one's errno kept for the
 * mount's life, the error bit set in memory, the mount made read-only, and
 * the mount's `write_error` set when it has none, which every later
 * `fsync` reports. Returns `rc`.
 */
static int writeback_error(struct ext2_mount *em, int rc)
{
    struct ext2_writeback *wb = em->wb;

    if (wb && !wb->error) {
        wb->error = rc;
    }
    em->state |= EXT2_ERROR_FS;
    em->mnt->flags |= VFS_MS_RDONLY;
    if (!em->mnt->write_error) {
        em->mnt->write_error = rc;
    }
    return rc;
}

/** The batch's changed images at or past the stage a drain is at: the
 * buffers a drain takes from the cache. */
static uint32_t writeback_dirty(const struct ext2_writeback *wb)
{
    uint32_t n = 0;
    int i;

    for (i = 0; i < wb->datas; i++) {
        n += wb->data[i].dirty && wb->data[i].stage >= wb->stage;
    }
    for (i = 0; i < wb->metas; i++) {
        n += wb->meta[i].dirty && wb->meta[i].stage >= wb->stage;
    }
    return n;
}

/**
 * Write the batch out, stage by stage, under `em->meta`: the inodes encoded
 * into their table images, then each stage's changed images copied into the
 * cache, from the allocation bitmaps and descriptors through the data, the
 * pointer blocks and the superblock to the inode tables, a file's data and
 * pointer blocks tagged as its own. No buffer is held across a sync, which
 * may reach a backing file.
 *
 * On a mount that writes back the copies are all the work: the cache first
 * writes dirty victims with no lock taken until it can give a buffer for
 * each copy without a write (`bcache_reserve`), so that the lock waits on
 * the disk only for what others took meanwhile. A strict mount holds the
 * lock across its ordered writes, as its option asks: each stage is synced
 * and flushed before the next enters the cache, so that a sync or an
 * eviction can write only a stage whose dependencies are on the medium.
 *
 * At the end the batch is emptied, and with the lock let go its inodes'
 * holds are put and the cache balanced (`em_balance`). A failure keeps the
 * images and the stage, so that the next drain resumes there, and is
 * recorded by `writeback_error`; a removed device fails at once. The caller
 * holds no `em->meta`. 0, or -errno.
 */
static int writeback_drain(struct ext2_mount *em)
{
    struct ext2_writeback *wb = em->wb;
    struct ext2_node *held[EXT2_WB_NODES];
    int i, count, rc;

    if (em->dev->flags & BLOCK_GONE) {
        return writeback_error(em, -EIO);
    }
    if (!wb || !wb->count) {
        return 0;
    }
    if (!em_strict(em)) {
        (void)bcache_reserve(em->dev, writeback_dirty(wb));
    }
    meta_lock(em);
    if (!wb->count) {
        meta_unlock(em);
        return 0;
    }
    for (i = 0; i < wb->count; i++) {
        struct ext2_node *n = wb->nodes[i];
        uint64_t at = inode_offset(em, n->ino);
        struct ext2_image *im = writeback_find(em, (uint32_t)(at >> 12));

        inode_encode_noting(em, n, im->data + ((uint32_t)at & 4095u),
                            em->inode_size > EXT2_GOOD_OLD ? EXT2_LARGE : EXT2_GOOD_OLD);
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
            b = bcache_get_blank(em->dev, im->block);
            if (!b) {
                rc = -EIO;
                goto fail;
            }
            k_memcpy(b->data, im->data, BCACHE_BLOCK_BYTES);
            if (im->owner) {
                bcache_dirty_owned(b, im->owner);
            } else {
                bcache_dirty(b);
            }
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
    for (i = 0; i < count; i++) {
        held[i] = wb->nodes[i];
    }
    wb->count = wb->datas = wb->metas = 0;
    wb->bytes = 0;
    wb->stage = 0;
    meta_unlock(em);
    for (i = 0; i < count; i++) {
        ext2_put(em->mnt, held[i]);
    }
    em_balance(em);
    return 0;
fail:
    rc = writeback_error(em, rc);
    meta_unlock(em);
    return rc;
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
        stamp(d, T_MTIME | T_CTIME);
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

/**
 * Whether directory inode `ino` is `ancestor` or below it, into `*below`,
 * by the `..` entries from `ino` up to the root: 0, -ELOOP past PATH_DEPTH
 * levels, -EIO for a directory without `..`, or -errno.
 *
 * The caller holds the mount's rename lock, under which no directory
 * changes its parent, and may hold the locks of directories on the way;
 * so the walk reads copies of the inodes and takes no lock, since one of
 * an ancestor would come after its descendant's. An entry is changed in
 * one step between sleeps, so a `..` read while another call adds a name
 * beside it is whole.
 */
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

/** Bring byte offset `at` of the volume's block into the cache with no lock
 * held, for a caller that reads it under `em->meta`; a failure is left to
 * that read. */
static void block_fetch(struct ext2_mount *em, uint64_t at)
{
    volume_fetch(em, at >> BCACHE_BLOCK_SHIFT);
}

/** Fetch the block of the volume that holds group `g`'s descriptor, which
 * `gd_store` writes in part and so reads first. */
static void gd_fetch(struct ext2_mount *em, uint32_t g)
{
    block_fetch(em, (uint64_t)(em->first_data + 1) * em->block_size + (uint64_t)g * 32);
}

/** Fetch what freeing the inode of `n` reads under `em->meta`: its inode
 * bitmap and its group's descriptor, and its attribute block with that
 * block's bitmap. */
static void inode_blocks_fetch(struct ext2_mount *em, const struct ext2_node *n)
{
    uint32_t g = (n->ino - 1) / em->ipg;

    block_fetch(em, (uint64_t)le32(em->gd + g * 32 + 4) * em->block_size);
    gd_fetch(em, g);
    if (n->file_acl != 0 && data_block_ok(em, n->file_acl)) {
        g = (n->file_acl - em->first_data) / em->bpg;
        block_fetch(em, (uint64_t)n->file_acl * em->block_size);
        block_fetch(em, (uint64_t)le32(em->gd + g * 32) * em->block_size);
    }
}

/**
 * Free inode `n`, which has no name and no holder: the blocks of a regular
 * file, a directory or a long symbolic link from the end (`punch`), then
 * the inode written zeroed with its deletion time, then its reference to
 * its extended-attribute block (`attr_release`), then its bit and counts,
 * these two under `em->meta` with the blocks they read fetched first, so
 * that the lock waits on the disk only for a block evicted again. The node's
 * `unlinked` mark is cleared. 0, or -errno.
 */
static int release_inode(struct ext2_mount *em, struct ext2_node *n)
{
    int was_dir = (n->mode & S_IFMT_) == S_IFDIR_;
    int rc;

    if ((n->mode & S_IFMT_) == S_IFREG_ || was_dir ||
        ((n->mode & S_IFMT_) == S_IFLNK_ && !fast_link(em, n))) {
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
    inode_blocks_fetch(em, n);
    meta_lock(em);
    if (n->file_acl != 0) {
        rc = attr_release(em, n->ino, n->file_acl);
    }
    if (rc == 0) {
        n->unlinked = 0;
        rc = inode_bit_free(em, n->ino, was_dir);
    }
    meta_unlock(em);
    return rc;
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
 * keeps. The node is made before `em->meta`, whose holder should not wait
 * on the heap, and is published N_LOADING under it, so that no gap lets
 * another caller see the number with no node; it goes N_LIVE once written.
 * The parent's group's inode bitmap and descriptor are fetched before
 * `em->meta`, so that its holder waits on the disk only when the parent's
 * group is full or a block was evicted again.
 * 0, -ENOSPC, -ENOMEM, or -errno; a failure frees the inode.
 */
static int inode_new(struct ext2_mount *em, const struct ext2_node *dir, uint32_t mode, int store,
                     struct ext2_node **out)
{
    uint32_t parent_ino = dir->ino;
    int is_dir = (mode & S_IFMT_) == S_IFDIR_;
    struct ext2_node *n = node_make();
    int rc;

    if (!n) {
        klog(KLOG_WARN, "ext2", "node: no memory");
        return -ENOMEM;
    }
    block_fetch(em, (uint64_t)le32(em->gd + (parent_ino - 1) / em->ipg * 32 + 4) * em->block_size);
    gd_fetch(em, (parent_ino - 1) / em->ipg);
    meta_lock(em);
    rc = inode_alloc(em, parent_ino, is_dir, n);
    meta_unlock(em);
    if (rc != 0) {
        if (n->ino != 0) {
            node_settle(em, n, rc);
        } else {
            kfree(n);
        }
        return rc;
    }
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
    n->extra_isize = em->new_extra;
    stamp(n, T_ATIME | T_MTIME | T_CTIME);
    if (extra_has(n, EXT2_CRTIME + 4)) {
        n->crtime = time_fit(n, EXT2_CRTIME_EXTRA + 4, n->ctime.sec, n->ctime.nsec);
    }
    n->generation = em->next_generation++;
    rc = store ? inode_write(em, n, 1, WRITE_THROUGH)
               : write_bytes(em, inode_offset(em, n->ino), 0, em->inode_size, WRITE_THROUGH);
    if (rc != 0) {
        meta_lock(em);
        inode_bit_free(em, n->ino, is_dir);
        meta_unlock(em);
        node_settle(em, n, rc);
        return rc;
    }
    node_settle(em, n, 0);
    *out = n;
    return 0;
}

/* ---- the operations ---- */

/**
 * The `setattr` of `vfs_ops`: the permission bits, the owner, the group, the
 * access and modification times and the flags chattr may change, as `what`
 * selects, with the change time set to the present and the inode stored.
 * A strict mount drains the batch first (`assert_writable`). Takes the
 * node's lock, which the caller may hold already. 0, -EROFS, or -errno.
 */
static int ext2_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *n = node;
    int rc;

    node_lock(n);
    rc = assert_writable(mnt, em);
    if (rc != 0) {
        node_unlock(n);
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
        n->atime = time_fit(n, EXT2_ATIME_EXTRA + 4, st->atime_sec, st->atime_nsec);
    }
    if (what & VFS_SET_MTIME) {
        n->mtime = time_fit(n, EXT2_MTIME_EXTRA + 4, st->mtime_sec, st->mtime_nsec);
    }
    if (what & VFS_SET_FLAGS) {
        n->flags = (n->flags & ~EXT2_FL_USER_MODIFIABLE) | (st->flags & EXT2_FL_USER_MODIFIABLE);
    }
    stamp(n, T_CTIME);
    rc = inode_store(em, n);
    node_unlock(n);
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
 * `size`, under the node's lock, which the caller may hold already; both
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
    node_lock(n);
    rc = assert_writable(mnt, em);
    if (rc != 0) {
        node_unlock(n);
        return rc;
    }
    assert_node_locked(n);
    if (size < n->size) {
        uint32_t keep = (uint32_t)((size + em->block_size - 1) >> em->block_shift);

        n->size = size;
        stamp(n, T_MTIME | T_CTIME);
        rc = inode_store(em, n);
        if (rc == 0 && ((uint32_t)size & (em->block_size - 1)) != 0) {
            uint32_t b;

            rc = block_at(em, n, (uint32_t)(size >> em->block_shift), &b);
            if (rc == 0 && b != 0) {
                uint32_t in = (uint32_t)size & (em->block_size - 1);

                rc = write_owned(em, n->ino, (uint64_t)b * em->block_size + in, 0,
                                 em->block_size - in, 0);
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
            stamp(n, T_MTIME | T_CTIME);
            rc = inode_store(em, n);
        }
    }
    node_unlock(n);
    return rc;
}

/**
 * Write what regular file `n` left dirty, with no lock held, and flush the
 * disk's cache: the shared blocks dirtied for it, then its own data and
 * pointer blocks (`bcache_sync_owned`), then its inode's table block, which
 * `data_only` leaves out unless a field but the times changed
 * (`n->dirty_meta`). A bitmap's or descriptor's bit is so on the device
 * before a pointer to its block, as a strict mount orders them, and a
 * bitmap a free dirtied `after` waits for the device's `first` pointers
 * (`bcache_sync_block`). A list that overflowed, or a node freed with its
 * list (`em->shared_lost`), makes it the whole device's `bcache_sync`. The
 * list is taken and emptied in one step, so a block dirtied for `n` during
 * the writes is the next call's. The caller has drained the batch. A
 * failure is recorded as the batch's error and leaves the next call the
 * whole device. 0, or -errno.
 */
static int node_write_out(struct vfs_mount *mnt, struct ext2_node *n, int data_only)
{
    struct ext2_mount *em = em_of(mnt);
    uint32_t shared[EXT2_NODE_SHARED];
    uint64_t table = inode_offset(em, n->ino) >> BCACHE_BLOCK_SHIFT;
    int count = n->nshared, whole = n->shared_over || em->shared_lost;
    int with_table = !data_only || n->dirty_meta;
    int i, rc = 0;

    k_memcpy(shared, n->shared, sizeof(shared));
    n->nshared = n->shared_over = 0;
    em->shared_lost = 0;
    if (with_table) {
        n->dirty_meta = 0;
    }
    if (whole) {
        rc = bcache_sync(em->dev);
    }
    for (i = 0; !whole && rc == 0 && i < count; i++) {
        rc = bcache_sync_block(em->dev, shared[i]);
    }
    if (!whole && rc == 0) {
        rc = bcache_sync_owned(em->dev, n->ino);
    }
    if (!whole && rc == 0 && with_table) {
        rc = bcache_sync_block(em->dev, table);
    }
    if (rc == 0) {
        rc = block_flush(em->dev);
    }
    if (rc != 0) {
        n->shared_over = 1;
        n->dirty_meta |= with_table;
        rc = writeback_error(em, rc);
    }
    return rc;
}

/**
 * The body of `fsync` and `fdatasync`: the batch drained under `em->meta`,
 * then a regular file's blocks written as `node_write_out` says,
 * `data_only` for `fdatasync`. Any other file, a mount that is not
 * writable, and the sync of a whole mount (a NULL `handle`) write the
 * device's dirty buffers instead (`em_write_out`): a directory's entries
 * are not tagged as its own. A failure is recorded as the batch's error,
 * and the mount's first failure, of this call or any before, is what it
 * returns; 0 when there has been none.
 */
static int file_sync(struct vfs_mount *mnt, void *handle, int data_only)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *n = handle;
    int rc;

    rc = writeback_drain(em);
    if (rc != 0) {
        rc = writeback_error(em, rc);
    } else if (!n || (n->mode & S_IFMT_) != S_IFREG_ || !em->writable ||
               (mnt->flags & VFS_MS_RDONLY)) {
        rc = em_write_out(mnt);
    } else {
        rc = node_write_out(mnt, n, data_only);
    }
    if (mnt->write_error) {
        rc = mnt->write_error;
    }
    return rc;
}

/** The `fsync` of `vfs_ops`: `file_sync` of the file and its inode. */
static int ext2_fsync(struct vfs_mount *mnt, void *handle)
{
    return file_sync(mnt, handle, 0);
}

/** The `fdatasync` of `vfs_ops`: `file_sync` that leaves out an inode whose
 * times alone changed. */
static int ext2_fdatasync(struct vfs_mount *mnt, void *handle)
{
    return file_sync(mnt, handle, 1);
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
 * the file-write batch, under the node's lock, which the caller may hold
 * already.
 *
 * ENOSPC is decided first, for the whole write (`writeback_space`). An
 * overwrite inside the file's blocks or a contiguous append joins the batch
 * a block at a time; any other write drains the batch before it and after
 * each block. The batch drains too when its bytes, images or inodes run out,
 * and a piece that finds its tables full drains it and is tried once more. A
 * write that fails after some of its bytes were accepted returns their
 * count; a failure of the batch's I/O stays recorded for `fsync`, after a
 * close and a retry too.
 *
 * A page of `buf` not present at a piece's commit is brought in with the
 * locks let go, and the rest of the write is decided again from there; when
 * the caller holds the file's lock, or another, which this cannot give
 * back, the write ends short at that piece, 0 bytes for the first, and the
 * caller brings the page in (`user_refault`). Returns the bytes accepted,
 * -EISDIR, -EFBIG, -EROFS, -ENOSPC, -EFAULT, or -errno.
 */
static int32_t ext2_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                          uint64_t offset)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_writeback *wb;
    struct ext2_node *n = handle;
    const uint8_t *in = buf;
    uint32_t done = 0;
    int rc = 0, synchronous, refaults = 0;

    if ((n->mode & S_IFMT_) == S_IFDIR_) {
        return -EISDIR;
    }
    if (count == 0) {
        return 0;
    }
    if (offset > em->file_limit || count > em->file_limit - offset) {
        return past_limit(em, "write", offset);
    }
    node_lock(n);
    assert_node_locked(n);
again:
    wb = em->wb;
    if (!em->writable || (mnt->flags & VFS_MS_RDONLY)) {
        rc = -EROFS;
        goto out;
    }
    rc = writeback_space(em, n, offset + done, count - done);
    if (rc != 0) {
        goto out;
    }
    rc = writeback_contiguous(em, n, offset + done, count - done);
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
        if (rc == -EFAULT) {
            rc = ++refaults <= EXT2_REFAULTS ? user_refault(n, in + done, take, 0) : -EFAULT;
            if (rc == 1) {
                goto again;
            }
            goto out;
        }
        if (rc != 0) {
            goto out;
        }
        done += take;
        refaults = 0;
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
    if (rc == -EIO && em->wb && em->wb->count) {
        (void)writeback_error(em, rc);
    }
    node_unlock(n);
    return done ? (int32_t)done : rc;
}

/**
 * The `fallocate` of `vfs_ops`: ext2 keeps no block reserved ahead of its
 * data, so each hole in the range is given a block by a write of zeros, a
 * block the file has is written only past its size, and without
 * FALLOC_FL_KEEP_SIZE the size grows to the range's end; with it the range
 * stops at the size. A punched hole and any other mode are -EOPNOTSUPP, as
 * Linux's ext2 returns for every mode. The node's lock is held throughout,
 * so that the size and the map do not change between the look at a block
 * and its write. 0, -ENODEV for a file that is not regular, -EFBIG,
 * -ENOMEM, or the errors of `ext2_write`.
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
    zero = kzalloc(em->block_size);
    if (!zero) {
        return -ENOMEM;
    }
    node_lock(n);
    if ((mode & 0x01u) && end > n->size) {
        end = n->size;
    }
    while (at < end && rc == 0) {
        uint32_t take = em->block_size - ((uint32_t)at & (em->block_size - 1));
        uint64_t from = at;
        uint32_t b = 0;

        if (take > end - at) {
            take = (uint32_t)(end - at);
        }
        rc = block_at(em, n, (uint32_t)(at >> em->block_shift), &b);
        if (b != 0 && from < n->size) {
            from = n->size;
        }
        if (rc == 0 && from < at + take) {
            int32_t w = ext2_write(mnt, handle, zero, (uint32_t)(at + take - from), from);

            rc = w < 0 ? (int)w : 0;
        }
        at += take;
    }
    node_unlock(n);
    kfree(zero);
    return rc;
}

/**
 * The node of inode `ino`, which an entry of a directory whose lock the
 * caller holds names, held and locked, into `*out`: the one node every
 * holder of the inode shares (`node_for`), never a copy, so that a change
 * made here is the one an open file sees. The VFS locks the node of the
 * name in unlink, rmdir and rename before it calls the type, and this takes
 * that lock again by depth. 0, or the errors of `node_for`.
 */
static int child_take(struct ext2_mount *em, uint32_t ino, struct ext2_node **out)
{
    int rc;
    struct ext2_node *c = node_for(em, ino, &rc);

    if (!c) {
        return rc;
    }
    node_lock(c);
    *out = c;
    return 0;
}

/** Give back the lock and the hold of a node `child_take` gave. */
static void child_give(struct ext2_mount *em, struct ext2_node *c)
{
    node_unlock(c);
    ext2_put(em->mnt, c);
}

/**
 * Mark node `c`, whose lock the caller holds and whose link count is 0,
 * as having no name, with its change time set: freed at once when the
 * caller's is its only hold, since nothing else can reach it, and
 * otherwise stored with its count of 0, for e2fsck to read it as deleted
 * after a crash, and freed at its last put. 0, or -errno.
 */
static int child_unlinked(struct ext2_mount *em, struct ext2_node *c)
{
    stamp(c, T_CTIME);
    c->unlinked = 1;
    if (c->refs == 1) {
        return release_inode(em, c);
    }
    return inode_store(em, c);
}

/**
 * The `create` of `vfs_ops`: a new regular file `name` in `dir`, the root for
 * NULL, with the permission bits of `mode`: the inode allocated and written
 * whole, then the entry that names it, then the directory's times into the
 * cache. A failure to add the entry frees the inode; a failure to store the
 * times sets the error bit and is not the call's, since the entry is there.
 * The batch is drained first. The caller holds the directory's lock, and
 * the new node's is held until it is named. 0 with the node held, -ENOENT
 * for a removed directory, -EEXIST, -ENAMETOOLONG, -EROFS, -ENOSPC,
 * -ENOMEM, or -errno.
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
    assert_node_locked(d);
    if (d->unlinked) {
        return -ENOENT;
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
    rc = inode_new(em, d, S_IFREG_ | (mode & 07777u), 1, &n);
    if (rc != 0) {
        return rc;
    }
    node_lock(n);
    rc = dir_add(em, d, name, n->ino, 1);
    if (rc != 0) {
        n->nlink = 0;
        release_inode(em, n);
        node_unlock(n);
        node_unhold(em, n);
        return rc;
    }
    stamp(d, T_MTIME | T_CTIME);
    rc = inode_times_later(em, d);
    if (rc != 0) {
        mark_error(em);
    }
    node_unlock(n);
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
 * holds the directory's lock. 0 with the node held, -EINVAL for another
 * kind, or the errors of `ext2_create`.
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
    assert_node_locked(d);
    if (d->unlinked) {
        return -ENOENT;
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
    rc = inode_new(em, d, type | (mode & 07777u), 0, &n);
    if (rc != 0) {
        return rc;
    }
    node_lock(n);
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
        node_unlock(n);
        node_unhold(em, n);
        return rc;
    }
    stamp(d, T_MTIME | T_CTIME);
    rc = inode_times_later(em, d);
    if (rc != 0) {
        mark_error(em);
    }
    node_unlock(n);
    *node = n;
    return 0;
}

/**
 * The `unlink` of `vfs_ops`: the entry `name` in `dir` removed, then the
 * directory's times, then the inode's link count lowered, on the node every
 * holder shares, under its lock (`child_take`). The inode goes with its last
 * name when nothing else holds it, and at the last put otherwise, its count
 * of zero stored meanwhile, so that after a crash e2fsck and the check read
 * it as deleted (`child_unlinked`). The batch is drained first. The caller
 * holds the directory's lock. 0, -ENOENT, -EISDIR, -EROFS, or -errno.
 */
static int ext2_unlink(struct vfs_mount *mnt, void *dir, const char *name)
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
    assert_node_locked(d);
    rc = dir_find(em, d, name, &at, &prev, &sc);
    if (rc < 0) {
        return rc;
    }
    if (rc == 0) {
        return -ENOENT;
    }
    rc = child_take(em, sc.ino, &n);
    if (rc != 0) {
        return rc;
    }
    rc = (n->mode & S_IFMT_) == S_IFDIR_ ? -EISDIR : dir_remove_at(em, d, at, prev, &sc);
    if (rc == 0) {
        stamp(d, T_MTIME | T_CTIME);
        rc = inode_times_later(em, d);
    }
    if (rc == 0) {
        if (n->nlink > 0) {
            n->nlink--;
        }
        if (n->nlink == 0) {
            rc = child_unlinked(em, n);
        } else {
            stamp(n, T_CTIME);
            rc = inode_store(em, n);
        }
    }
    child_give(em, n);
    return rc;
}

/**
 * The `mkdir` of `vfs_ops`: a new directory `name` in `dir`, with the
 * permission bits of `mode`. Its block is written with `.` and `..`, then
 * its inode with a count of two, then the parent's raised count, and the
 * parent's entry last; a stop before the inode leaves a set bit with
 * nothing behind it. A failure frees the new inode, and lowers the parent's
 * count again when the entry was not written. The batch is drained first.
 * The caller holds the parent's lock, and the new directory's is held
 * until it is named. 0, -ENOENT for a removed parent, -EEXIST, -EMLINK at
 * 65000 links, -ENAMETOOLONG, -EROFS, -ENOSPC, -ENOMEM, or -errno.
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
    assert_node_locked(d);
    if (d->unlinked) {
        return -ENOENT;
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
    if (d->nlink >= 65000) {
        return -EMLINK;
    }
    rc = inode_new(em, d, S_IFDIR_ | (mode & 07777u), 0, &n);
    if (rc != 0) {
        return rc;
    }
    node_lock(n);
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
        stamp(d, T_MTIME | T_CTIME);
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
        node_unlock(n);
        node_unhold(em, n);
        return rc;
    }
    node_unlock(n);
    node_unhold(em, n);
    return 0;
}

/**
 * The `rmdir` of `vfs_ops`: the empty directory `name` in `dir` removed: the
 * entry, then the parent's count lowered, then the inode freed. The
 * directory's own lock is held from the check that it is empty until it is
 * marked as having no name, with a link count of 0, so that no entry is
 * added to it meanwhile (`dir_add`) and a mount on it is refused after
 * (`vfs_cover_lock`). One that a working directory or an open description
 * holds keeps its inode and its blocks until its last put, as an open file
 * does; nothing can be added to it, and a listing of it is -ENOENT, as on
 * Linux. The batch is drained first. The caller holds the parent's lock.
 * 0, -ENOENT, -ENOTDIR, -ENOTEMPTY, -EROFS, or -errno.
 */
static int ext2_rmdir(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node *c;
    struct ext2_scan sc;
    uint32_t at, prev;
    int rc, empty = 0;

    rc = assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    assert_node_locked(d);
    rc = dir_find(em, d, name, &at, &prev, &sc);
    if (rc < 0) {
        return rc;
    }
    if (rc == 0) {
        return -ENOENT;
    }
    rc = child_take(em, sc.ino, &c);
    if (rc != 0) {
        return rc;
    }
    rc = (c->mode & S_IFMT_) != S_IFDIR_ ? -ENOTDIR : dir_is_empty(em, c, &empty);
    if (rc == 0 && !empty) {
        rc = -ENOTEMPTY;
    }
    if (rc == 0) {
        rc = dir_remove_at(em, d, at, prev, &sc);
    }
    if (rc == 0) {
        c->nlink = 0;
        c->unlinked = 1;
        if (d->nlink > 2) {
            d->nlink--;
        }
        stamp(d, T_MTIME | T_CTIME);
        rc = inode_store(em, d);
    }
    if (rc == 0) {
        rc = child_unlinked(em, c);
    }
    child_give(em, c);
    return rc;
}

/**
 * The name `name` in directory `d` for the file `n`, for `ext2_link`, whose
 * caller holds both locks: the count raised and stored, then the name
 * written, then the directory's times. 0, -ENOENT for a file with no name
 * left, -EMLINK, -EEXIST, or -errno.
 */
static int link_add(struct ext2_mount *em, struct ext2_node *d, const char *name,
                    struct ext2_node *n)
{
    struct ext2_scan sc;
    uint32_t at, prev;
    int rc;

    if (n->unlinked || n->nlink == 0) {
        return -ENOENT;
    }
    if (n->nlink >= 65000) {
        return -EMLINK;
    }
    rc = dir_find(em, d, name, &at, &prev, &sc);
    if (rc < 0) {
        return rc;
    }
    if (rc == 1) {
        return -EEXIST;
    }
    n->nlink++;
    stamp(n, T_CTIME);
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
    stamp(d, T_MTIME | T_CTIME);
    return inode_times_later(em, d);
}

/**
 * The `link` of `vfs_ops`: a new name `name` in `dir` for the file `node`:
 * the count raised first, then the name written, so that a stop between
 * leaves a count one too high, which is a count repair; then the
 * directory's times. A failure to write the name lowers the count again.
 * The batch is drained first. The caller holds the directory's lock and
 * the file's, which this takes again. 0, -EPERM for a directory, -ENOENT
 * for a removed directory or a file with no name left, -EMLINK at 65000
 * links, -ENAMETOOLONG, -EEXIST, -EROFS, -ENOSPC, or -errno.
 */
static int ext2_link(struct vfs_mount *mnt, void *dir, const char *name, void *node)
{
    struct ext2_mount *em = em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node *n = node;
    int rc;

    rc = assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    assert_node_locked(d);
    if ((n->mode & S_IFMT_) == S_IFDIR_) {
        return -EPERM;
    }
    if (d->unlinked) {
        return -ENOENT;
    }
    if (k_strlen(name) > EXT2_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    node_lock(n);
    rc = link_add(em, d, name, n);
    node_unlock(n);
    return rc;
}

/**
 * The `symlink` of `vfs_ops`: a link `name` in `dir` to `target`. A target
 * shorter than 60 bytes is kept in the inode's pointer words, and a longer
 * one in a block of its own, written first; then the inode, then the entry,
 * then the directory's times. A failure frees the inode. The batch is
 * drained first. The caller holds the directory's lock, and the link's is
 * held until it is named. 0, -ENOENT for a removed directory,
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
    assert_node_locked(d);
    if (d->unlinked) {
        return -ENOENT;
    }
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
    node_lock(n);
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
        stamp(d, T_MTIME | T_CTIME);
        rc = inode_times_later(em, d);
    }
    if (rc != 0) {
        n->nlink = 0;
        release_inode(em, n);
        node_unlock(n);
        node_unhold(em, n);
        return rc;
    }
    node_unlock(n);
    node_unhold(em, n);
    return 0;
}

/**
 * Whether the rename of node `n` from directory `od` over `victim` in
 * directory `nd`, `victim` NULL for a new name, may go on, decided before
 * either node's lock is taken: 0; -EISDIR or -ENOTDIR for a directory and
 * a file; -EINVAL for a directory moved below itself; -ENOTEMPTY for a
 * victim directory above `od`, which holds it; or the errors of `is_below`.
 *
 * The VFS leaves a node unlocked in the last two cases, since a directory
 * above the other one would be locked after its descendant, so they are
 * found here first, by walks up `..` under the mount's rename lock. A
 * file's or a directory's type does not change while it is held.
 */
static int rename_check(struct ext2_mount *em, struct ext2_node *od, struct ext2_node *nd,
                        struct ext2_node *n, struct ext2_node *victim)
{
    int is_dir = (n->mode & S_IFMT_) == S_IFDIR_;
    int below, rc;

    if (victim) {
        int victim_dir = (victim->mode & S_IFMT_) == S_IFDIR_;

        if (victim_dir && !is_dir) {
            return -EISDIR;
        }
        if (!victim_dir && is_dir) {
            return -ENOTDIR;
        }
    }
    if (!is_dir || od == nd) {
        return 0;
    }
    rc = is_below(em, nd->ino, n->ino, &below);
    if (rc != 0 || below) {
        return rc != 0 ? rc : -EINVAL;
    }
    if (victim) {
        rc = is_below(em, od->ino, victim->ino, &below);
        if (rc != 0 || below) {
            return rc != 0 ? rc : -ENOTEMPTY;
        }
    }
    return 0;
}

/**
 * The rename of directory `n` from `oldname` in `od` to `newname` in `nd`,
 * over the empty directory `victim` at `nsc`, `nat` and `nprev` when it is
 * not NULL, for `ext2_rename`, whose caller holds every lock: the victim
 * removed as rmdir removes it, then the new entry, then `..` rewritten and
 * the new parent's count raised when the parents differ, then the old entry
 * removed and the old parent's count lowered. 0, -ENOENT, -EMLINK, or
 * -errno.
 */
static int rename_dir(struct ext2_mount *em, struct ext2_node *od, const char *oldname,
                      struct ext2_node *nd, const char *newname, struct ext2_node *n,
                      struct ext2_node *victim, const struct ext2_scan *nsc, uint32_t nat,
                      uint32_t nprev)
{
    struct ext2_scan osc;
    uint32_t oat, oprev;
    int rc;

    if (od != nd && nd->nlink >= 65000) {
        return -EMLINK;
    }
    if (victim) {
        rc = dir_remove_at(em, nd, nat, nprev, nsc);
        if (rc != 0) {
            return rc;
        }
        victim->nlink = 0;
        victim->unlinked = 1;
        if (nd->nlink > 2) {
            nd->nlink--;
        }
        rc = inode_store(em, nd);
        if (rc == 0) {
            rc = child_unlinked(em, victim);
        }
        if (rc != 0) {
            return rc;
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
        stamp(nd, T_MTIME | T_CTIME);
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
    stamp(od, T_MTIME | T_CTIME);
    return inode_store(em, od);
}

/**
 * The rename of file `n` from `oldname` in `od` to `newname` in `nd`, over
 * the file `victim` at `nat` when it is not NULL, for `ext2_rename`, whose
 * caller holds every lock: the count raised, the new entry written or the
 * victim's entry pointed at `n`, the victim's count lowered as unlink
 * lowers it, the old entry removed and the count lowered again. 0, -ENOENT,
 * or -errno.
 */
static int rename_file(struct ext2_mount *em, struct ext2_node *od, const char *oldname,
                       struct ext2_node *nd, const char *newname, struct ext2_node *n,
                       struct ext2_node *victim, uint32_t nat)
{
    struct ext2_scan osc;
    uint32_t oat, oprev;
    int rc;

    n->nlink++;
    stamp(n, T_CTIME);
    rc = inode_store(em, n);
    if (rc != 0) {
        n->nlink--;
        return rc;
    }
    if (victim) {
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
        if (victim->nlink == 0) {
            rc = child_unlinked(em, victim);
        } else {
            stamp(victim, T_CTIME);
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
    stamp(od, T_MTIME | T_CTIME);
    rc = inode_store(em, od);
    if (rc == 0 && od != nd) {
        nd->mtime = time_fit(nd, EXT2_MTIME_EXTRA + 4, od->mtime.sec, od->mtime.nsec);
        nd->ctime = time_fit(nd, EXT2_CTIME_EXTRA + 4, od->mtime.sec, od->mtime.nsec);
        rc = inode_store(em, nd);
    }
    return rc;
}

/**
 * The `rename` of `vfs_ops`: `oldname` in `olddir` to `newname` in `newdir`,
 * either directory NULL for the root. The batch is drained first. The
 * caller holds both directories' locks, and for a move between two of them
 * the mount's rename lock, under which no directory changes its parent.
 *
 * The source's and the victim's nodes are those every holder shares, had
 * with the directories' locks held, so that each is the inode its entry
 * names. The cases the VFS leaves unlocked are refused first
 * (`rename_check`); then both are locked, the lower inode number first,
 * which takes again the locks the VFS took, and a victim directory is
 * checked empty under its own lock.
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
    struct ext2_node *n, *victim = 0, *first, *second;
    struct ext2_scan osc, nsc;
    uint32_t oat, oprev, nat = 0, nprev = 0;
    int rc, have_new;

    rc = assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    assert_node_locked(od);
    assert_node_locked(nd);
    if (od != nd) {
        sleep_lock_assert(&mnt->rename, "ext2: a move between directories outside the rename lock");
    }
    if (k_strlen(newname) > EXT2_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    if (nd->unlinked) {
        return -ENOENT;
    }
    rc = dir_find(em, od, oldname, &oat, &oprev, &osc);
    if (rc <= 0) {
        return rc < 0 ? rc : -ENOENT;
    }
    have_new = dir_find(em, nd, newname, &nat, &nprev, &nsc);
    if (have_new < 0) {
        return have_new;
    }
    if (have_new && nsc.ino == osc.ino) {
        return 0;
    }
    n = node_for(em, osc.ino, &rc);
    if (!n) {
        return rc;
    }
    if (have_new) {
        victim = node_for(em, nsc.ino, &rc);
        if (!victim) {
            ext2_put(mnt, n);
            return rc;
        }
    }
    rc = rename_check(em, od, nd, n, victim);
    if (rc == 0) {
        first = victim && victim->ino < n->ino ? victim : n;
        second = first == n ? victim : n;
        node_lock(first);
        if (second) {
            node_lock(second);
        }
        if (victim && (victim->mode & S_IFMT_) == S_IFDIR_) {
            int empty;

            rc = dir_is_empty(em, victim, &empty);
            if (rc == 0 && !empty) {
                rc = -ENOTEMPTY;
            }
        }
        if (rc == 0 && (n->mode & S_IFMT_) == S_IFDIR_) {
            rc = rename_dir(em, od, oldname, nd, newname, n, victim, &nsc, nat, nprev);
        } else if (rc == 0) {
            rc = rename_file(em, od, oldname, nd, newname, n, victim, nat);
        }
        if (second) {
            node_unlock(second);
        }
        node_unlock(first);
    }
    if (victim) {
        ext2_put(mnt, victim);
    }
    ext2_put(mnt, n);
    return rc;
}

/* ---- the mount-time check ---- */

/*
 * A volume whose valid bit is clear was not unmounted, and one whose error
 * bit is set was found faulty by an earlier mount. Either is checked
 * before it is written, under the walk lock and the shared bitmap the FAT
 * drivers use: the structural blocks are seeded into the bitmap, the tree
 * is walked from the root and every block every reached inode names is
 * marked, its extended-attribute block among them, the inodes the inode
 * bitmap calls used but the walk did not reach are scanned as orphans, and
 * the bitmaps are compared with the marks. Nothing is repaired. Lost
 * blocks, orphans and shared attribute blocks whose reference count is
 * above the inodes that name them are counted and kept, held out of the
 * allocator by the bits and counts they still have, and the error bit is
 * set so that the volume is not marked clean again before `e2fsck` has
 * run. A fault mounts the volume read-only with the first one named: a
 * block claimed twice or out of range, an inode whose pointers do not
 * match its size or its `i_blocks`, an attribute block of the wrong form
 * or named by more inodes than its count, a directory with two parents or
 * without its parent, an entry of the wrong form, a link count that does
 * not match the entries, a used block or a reached inode the bitmap calls
 * free. The group descriptors' free counts are recomputed from the bitmaps
 * rather than taken from the disk, since a crash leaves them stale.
 *
 * The walk keeps its state on the heap rather than on the stack: the node
 * of each directory level, the hard links and the shared attribute blocks
 * met with their expected and found counts, the reached-inode set, one bit
 * per inode of the volume, and one bitmap block; the block bitmap is the
 * one the FAT walks share. A dirty volume whose set or bitmap the free
 * memory cannot hold, a directory deeper than the levels, or more shared
 * attribute blocks than its table holds mounts read-only and says
 * which. Hard-link records are allocated as files are met.
 */
#define EXT2_CHECK_DEPTH        32  /* the directory levels the walk holds */
#define EXT2_CHECK_LINK_BUCKETS 512 /* the index, not a limit on linked files */
#define EXT2_CHECK_ATTRS        512 /* the shared attribute blocks it counts */

/* A file with more than one link: its inode, its link count, and the names
 * the walk has found for it. */
struct ext2_link {
    struct ext2_link *next;
    uint32_t ino;
    uint32_t expect, found;
};

/* An extended-attribute block whose header counts more than one reference:
 * the block, that count, and the inodes the check has found naming it. */
struct ext2_attr {
    uint32_t block;
    uint32_t expect, found;
};

/*
 * The check's state: the first fault and its inode; the lost blocks, the
 * orphans, the shared attribute blocks counting more references than the
 * inodes that name them (`stale`), the chained records of `link` and the
 * `attrs` entries of `attr`; the inode of each directory level being
 * walked; the set of reached inodes; and one bitmap block.
 */
struct ext2_check {
    struct ext2_mount *em;
    const char *fault;
    uint32_t fault_ino;
    uint32_t lost, orphans, stale, attrs;
    int link_nomem;
    struct ext2_link *link[EXT2_CHECK_LINK_BUCKETS];
    struct ext2_attr attr[EXT2_CHECK_ATTRS];
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
    uint32_t i;
    struct ext2_link *link, *next;

    for (i = 0; i < EXT2_CHECK_LINK_BUCKETS; i++) {
        for (link = c->link[i]; link; link = next) {
            next = link->next;
            kfree(link);
        }
    }
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
 * Mark the extended-attribute block inode `n` names, once however many
 * inodes share it. Its header must have the magic number and a length of
 * one block. A block whose reference count is 1 or less is marked as any
 * other block is; a shared one is marked and entered in the table by its
 * first inode, and each later inode counts toward the reference count
 * there, more inodes than the count being a fault. 0, or -EIO with the
 * fault recorded or for a failed read.
 */
static int check_attr(struct ext2_check *c, const struct ext2_node *n)
{
    struct ext2_mount *em = c->em;
    uint32_t b = n->file_acl, refs, i;
    uint8_t raw[12];
    int rc;

    if (b < em->first_data || b >= em->blocks) {
        return check_fault(c, n->ino, "a block out of range");
    }
    rc = read_bytes(em, (uint64_t)b * em->block_size, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }
    if (le32(raw) != EXT2_XATTR_MAGIC || le32(raw + 8) != 1) {
        return check_fault(c, n->ino, "an attribute block of the wrong form");
    }
    refs = le32(raw + 4);
    if (refs <= 1) {
        return check_mark(c, n->ino, b);
    }
    for (i = 0; i < c->attrs; i++) {
        if (c->attr[i].block == b) {
            if (++c->attr[i].found > c->attr[i].expect) {
                return check_fault(c, n->ino, "an attribute block named past its count");
            }
            return 0;
        }
    }
    if (c->attrs >= EXT2_CHECK_ATTRS) {
        return check_fault(c, n->ino, "more shared attribute blocks than the check's table");
    }
    rc = check_mark(c, n->ino, b);
    if (rc != 0) {
        return rc;
    }
    c->attr[c->attrs].block = b;
    c->attr[c->attrs].expect = refs;
    c->attr[c->attrs].found = 1;
    c->attrs++;
    return 0;
}

/**
 * Mark every block `n` names and check them against its size and its
 * `i_blocks`, which counts the extended-attribute block too (`check_attr`).
 * A fast symbolic link, a device, a pipe and a socket keep no other;
 * `as_data` treats the inode as a file whatever its mode, for the reserved
 * inodes. 0, or -EIO with the fault recorded or for a failed read.
 */
static int check_blocks(struct ext2_check *c, const struct ext2_node *n, int as_data)
{
    const struct ext2_mount *em = c->em;
    uint32_t type = n->mode & S_IFMT_;
    uint32_t per = em->block_size / 4;
    uint32_t attr = n->file_acl ? em->block_size / 512 : 0;
    uint32_t count = 0, last = 0, limit, i;
    int named = 0, rc;

    limit = (uint32_t)((n->size + em->block_size - 1) >> em->block_shift);
    if (n->file_acl != 0) {
        rc = check_attr(c, n);
        if (rc != 0) {
            return rc;
        }
    }
    if (!as_data && type != S_IFREG_ && type != S_IFDIR_ &&
        (type != S_IFLNK_ || fast_link(em, n))) {
        if (n->blocks != attr) {
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
    if (n->blocks != count * (em->block_size / 512) + attr) {
        return check_fault(c, n->ino, "i_blocks that does not match its pointers");
    }
    if (named && last >= limit) {
        return check_fault(c, n->ino, "a block past the size");
    }
    return 0;
}

static int check_dir(struct ext2_check *c, uint32_t ino, uint32_t parent, int depth);

/** Find the exact inode's link counts in the check's index. */
static struct ext2_link *check_link_find(struct ext2_check *c, uint32_t ino)
{
    struct ext2_link *link;

    for (link = c->link[ino % EXT2_CHECK_LINK_BUCKETS]; link; link = link->next) {
        if (link->ino == ino) {
            return link;
        }
    }
    return 0;
}

/** Remember a linked file once; no fixed number of such files is corrupt. */
static int check_link_add(struct ext2_check *c, const struct ext2_node *n)
{
    uint32_t bucket = n->ino % EXT2_CHECK_LINK_BUCKETS;
    struct ext2_link *link = kzalloc(sizeof(*link));

    if (!link) {
        c->link_nomem = 1;
        return -ENOMEM;
    }
    link->ino = n->ino;
    link->expect = n->nlink;
    link->found = 1;
    link->next = c->link[bucket];
    c->link[bucket] = link;
    return 0;
}

/** Require every remembered inode's expected count to match all its names. */
static int check_link_counts(struct ext2_check *c)
{
    uint32_t i;
    struct ext2_link *link;

    for (i = 0; i < EXT2_CHECK_LINK_BUCKETS; i++) {
        for (link = c->link[i]; link; link = link->next) {
            if (link->expect != link->found) {
                return check_fault(c, link->ino, "a link count that does not match its names");
            }
        }
    }
    return 0;
}

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
    uint32_t type;
    struct ext2_link *link;
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
        link = check_link_find(c, ino);
        if (link) {
            if (link->found >= link->expect) {
                return check_fault(c, ino, "a link count that does not match its names");
            }
            link->found++;
            return 0;
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
    return n->nlink != 1 ? check_link_add(c, n) : 0;
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
 * Log what a check without a fault leaves for e2fsck, setting the error bit
 * when it leaves anything: the shared attribute blocks whose count is above
 * the inodes found naming them, then the lost blocks and orphans; or that
 * the volume is clean.
 */
static void check_report(struct ext2_check *c)
{
    uint32_t i;

    for (i = 0; i < c->attrs; i++) {
        if (c->attr[i].found < c->attr[i].expect) {
            c->stale++;
        }
    }
    if (c->stale != 0) {
        klog(KLOG_WARN, "ext2",
             "check: %d attribute blocks with counts above their inodes, kept for e2fsck",
             (int)c->stale);
        c->em->state |= EXT2_ERROR_FS;
    }
    if (c->lost != 0 || c->orphans != 0) {
        klog(KLOG_WARN, "ext2", "check: %d lost blocks, %d orphan inodes, kept for e2fsck",
             (int)c->lost, (int)c->orphans);
        c->em->state |= EXT2_ERROR_FS;
    } else if (c->stale == 0) {
        klog(KLOG_INFO, "ext2", "check: clean");
    }
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
 * blocks were lost, inodes orphaned or attribute blocks left counting
 * inodes that no longer name them; 1 with `*why` naming why it must stay
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
    if (rc == 0) {
        rc = check_link_counts(c);
    }
    if (rc == 0) {
        rc = check_bitmaps(c);
    }
    fat_walk_release();
    if (rc == -ENOMEM && c->link_nomem) {
        check_state_free(c);
        klog(KLOG_WARN, "ext2", "check: no memory to count the hard links");
        *why = "not unmounted cleanly and no memory for the check's hard links";
        return 1;
    }
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
    check_report(c);
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
 * fails with ENOMEM, gives back what it took and leaves `em->wb` NULL, so
 * that a later remount sets the batch up again. 0, -ENOMEM, or the error of
 * `sb_store`, after which the batch stays.
 */
static int writeback_init(struct ext2_mount *em)
{
    struct ext2_writeback *wb = kzalloc(sizeof(*wb));
    int i;

    if (!wb) {
        return -ENOMEM;
    }
    for (i = 0; i < EXT2_WB_DATA + EXT2_WB_META; i++) {
        wb->slot[i] = kalloc_pages(1, 0);
        if (!wb->slot[i]) {
            writeback_free(wb);
            return -ENOMEM;
        }
    }
    em->wb = wb;
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
    /* A new large inode covers every extra field, or more where the
     * superblock wants more; with `extra_isize` every inode has at least
     * the superblock's minimum. */
    if (em->inode_size > EXT2_GOOD_OLD) {
        uint32_t room = em->inode_size - EXT2_GOOD_OLD;
        uint32_t min = (em->ro_compat & EXT2_RO_EXTRA_ISIZE) ? le16(sb + 348) : 0;
        uint32_t want = (em->ro_compat & EXT2_RO_EXTRA_ISIZE) ? le16(sb + 350) : 0;

        em->new_extra = EXT2_EXTRA_NEW;
        if (want > em->new_extra && !(want & 3u) && want <= room) {
            em->new_extra = want;
        }
        if ((min & 3u) || min > room) {
            readonly_why = "the minimum extra inode size does not fit the inode";
        } else if (min > em->new_extra) {
            em->new_extra = min;
        }
    }
    unknown = em->ro_compat & ~(EXT2_RO_SPARSE_SUPER | EXT2_RO_LARGE_FILE | EXT2_RO_EXTRA_ISIZE);
    if (unknown != 0) {
        readonly_why = "unknown read-only-compatible feature";
    } else if (!readonly_why && (em->compat & EXT2_COMPAT_DIR_INDEX)) {
        readonly_why = "dir_index would be left stale by this writer";
    } else if (!readonly_why && em->block_size != 4096) {
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
    em_locks_init(em);
    rc = ext2_mount_on(em, mnt, dev, flags);
    if (rc != 0) {
        nodes_free(em);
        writeback_free(em->wb);
        kfree(em->gd);
        kfree(em);
    }
    return rc;
}

/** The `lock_node` of `vfs_ops`: take the lock of the held node `node`
 * (`node_lock`). */
static void ext2_lock_node(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    node_lock(node);
}

/** The `unlock_node` of `vfs_ops`: give back a take of `node_lock`. The last
 * file lock a system call held is where the final puts it made under them
 * are done (`em_reap`). */
static void ext2_unlock_node(struct vfs_mount *mnt, void *node)
{
    node_unlock(node);
    em_reap(em_of(mnt));
}

/* ---- admission ---- */

/** End an operation `em_enter` admitted, with the final puts it left. */
static void em_finish(struct ext2_mount *em)
{
    em_leave(em);
    em_reap(em);
}

/** The `setattr` of `vfs_ops`: `ext2_setattr`, admitted. */
static int admit_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st)
{
    int rc;

    em_enter(em_of(mnt));
    rc = ext2_setattr(mnt, node, what, st);
    em_finish(em_of(mnt));
    return rc;
}

/** The `write` of `vfs_ops`: `ext2_write`, admitted. */
static int32_t admit_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                           uint64_t offset)
{
    int32_t rc;

    em_enter(em_of(mnt));
    rc = ext2_write(mnt, handle, buf, count, offset);
    em_finish(em_of(mnt));
    return rc;
}

/** The `truncate` of `vfs_ops`: `ext2_truncate`, admitted. */
static int admit_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    int rc;

    em_enter(em_of(mnt));
    rc = ext2_truncate(mnt, handle, size);
    em_finish(em_of(mnt));
    return rc;
}

/** The `create` of `vfs_ops`: `ext2_create`, admitted. */
static int admit_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode,
                        void **node)
{
    int rc;

    em_enter(em_of(mnt));
    rc = ext2_create(mnt, dir, name, mode, node);
    em_finish(em_of(mnt));
    return rc;
}

/** The `unlink` of `vfs_ops`: `ext2_unlink`, admitted. */
static int admit_unlink(struct vfs_mount *mnt, void *dir, const char *name)
{
    int rc;

    em_enter(em_of(mnt));
    rc = ext2_unlink(mnt, dir, name);
    em_finish(em_of(mnt));
    return rc;
}

/** The `mkdir` of `vfs_ops`: `ext2_mkdir`, admitted. */
static int admit_mkdir(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode)
{
    int rc;

    em_enter(em_of(mnt));
    rc = ext2_mkdir(mnt, dir, name, mode);
    em_finish(em_of(mnt));
    return rc;
}

/** The `rmdir` of `vfs_ops`: `ext2_rmdir`, admitted. */
static int admit_rmdir(struct vfs_mount *mnt, void *dir, const char *name)
{
    int rc;

    em_enter(em_of(mnt));
    rc = ext2_rmdir(mnt, dir, name);
    em_finish(em_of(mnt));
    return rc;
}

/** The `rename` of `vfs_ops`: `ext2_rename`, admitted. */
static int admit_rename(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
                        const char *newname)
{
    int rc;

    em_enter(em_of(mnt));
    rc = ext2_rename(mnt, olddir, oldname, newdir, newname);
    em_finish(em_of(mnt));
    return rc;
}

/** The `link` of `vfs_ops`: `ext2_link`, admitted. */
static int admit_link(struct vfs_mount *mnt, void *dir, const char *name, void *node)
{
    int rc;

    em_enter(em_of(mnt));
    rc = ext2_link(mnt, dir, name, node);
    em_finish(em_of(mnt));
    return rc;
}

/** The `symlink` of `vfs_ops`: `ext2_symlink`, admitted. */
static int admit_symlink(struct vfs_mount *mnt, void *dir, const char *name, const char *target)
{
    int rc;

    em_enter(em_of(mnt));
    rc = ext2_symlink(mnt, dir, name, target);
    em_finish(em_of(mnt));
    return rc;
}

/** The `mknod` of `vfs_ops`: `ext2_mknod`, admitted. */
static int admit_mknod(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode,
                       uint32_t rdev, void **node)
{
    int rc;

    em_enter(em_of(mnt));
    rc = ext2_mknod(mnt, dir, name, mode, rdev, node);
    em_finish(em_of(mnt));
    return rc;
}

/** The `fallocate` of `vfs_ops`: `ext2_fallocate`, admitted. */
static int admit_fallocate(struct vfs_mount *mnt, void *handle, uint32_t mode, uint64_t offset,
                           uint64_t length)
{
    int rc;

    em_enter(em_of(mnt));
    rc = ext2_fallocate(mnt, handle, mode, offset, length);
    em_finish(em_of(mnt));
    return rc;
}

static const struct vfs_ops ext2_ops = {
    ext2_lookup,
    0, /* lookup_rest */
    ext2_hold,
    ext2_put,
    ext2_getattr,
    admit_setattr,
    ext2_open,
    ext2_release,
    ext2_read,
    admit_write,
    admit_truncate,
    ext2_fsync,
    ext2_readdir,
    admit_create,
    admit_unlink,
    admit_mkdir,
    admit_rmdir,
    admit_rename,
    admit_link,
    ext2_readlink,
    ext2_statfs,
    ext2_sync,
    ext2_prepare_unmount,
    ext2_abort_unmount, /* abort_unmount */
    ext2_unmount,
    ext2_path,
    admit_symlink,
    admit_mknod,
    0, /* pin: a hold keeps a node */
    0, /* unpin */
    ext2_remount,
    admit_fallocate,
    ext2_fdatasync,
    ext2_lock_node,
    ext2_unlock_node,
};

const struct vfs_type vfs_ext2_type = {"ext2", ext2_mount, &ext2_ops, 0, 0, 1, 1};
