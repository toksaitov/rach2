/*
 * ext2_internal.h: what the files of the ext2 driver share: its constants,
 * the records of a mount, a node and the write batch, and the functions one
 * file calls in another.
 *
 * The locks, their order and what each covers are in ext2.Readme.md's
 * Locks section; in short, from the outermost: the mount's rename lock,
 * then directories' node locks, then other nodes', then `em->batch`, then
 * the groups' locks, then `em->super`, then buffers, then the spinlocks
 * `em->count_lock` and a batch's `wb->lock`. The rename lock is the mount's
 * (`vfs_mount.rename`), which the VFS takes for a rename between two
 * directories before it orders and locks them (`vfs_rename_lock`), so
 * `ext2_rename` and `is_below` run with it held. Related directories go
 * ancestor first; unrelated ones, and every inode after the directories,
 * go by inode number. A strict mount holds `em->batch` across a drain's
 * ordered writes, and a group's lock across the writes through of its
 * bitmap and descriptor, as `sync` asks.
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
 * The big lock covers the node table, and a lookup's and a change of
 * attributes' steps between their sleeps; the cache's writes have a lock of
 * their own (bcache.h). A reader may run
 * without it: `read`, `readdir`, `getattr`, `open`, `release`, `hold` and
 * `put` of a node the caller holds, into kernel buffers (the type sets
 * `reads_unlocked`). So may a writer of a regular file's data: `write`,
 * `truncate`, `fallocate`, `fsync` and `fdatasync` (the type sets
 * `writes_unlocked`), under the file's node lock as Linux's ext2 writes
 * under the inode's `i_rwsem`, with the batch's, the groups' and the
 * superblock's locks for what they share with other files. Each takes the
 * big lock only around the check and copy of a user buffer (`vm_resident`),
 * and for the node table, an error's mark, a final put and a reap
 * (`ext2_big_enter`); the node hash's changes and the look of a sync's walk
 * into it are under `em->share_lock` as well (`table_names`). A reader
 * holds its node's lock, which every writer of the node's blocks holds too,
 * and reads the batch's images under `wb->lock`, in the step that finds
 * them.
 *
 * The operations on names, create, mknod, unlink, mkdir, rmdir, link,
 * symlink and rename, let the big lock go once admitted (`names_begin` in
 * ext2.c) and run under the directories' locks the VFS took, as a writer of
 * a file's data runs under its node's. They search, decide and write under
 * those locks and the groups', and take the big lock (`ext2_big_enter`) only
 * for the node table: a node had or given back (`ext2_node_for`,
 * `ext2_node_unhold`, `ext2_node_settle`), a new inode published
 * (`inode_new`), and the count of a node's holds when its last name goes,
 * with its free (`child_unlinked`). The walk of `..` entries in `is_below`
 * takes none: each read copies from a buffer it holds, as each write
 * copies into one.
 *
 * The node locks, `em->batch`, the groups' locks and `em->super` count in
 * their holder's `file_locks` (process.h), so that reclaim under any of them
 * does not enter a file system. The type record sets `file_locked` and leaves
 * `ns_serialised` clear, so the mount's namespace lock covers nothing here: a
 * slow request on one file holds up only the callers that need that file's
 * lock.
 */

#ifndef _EXT2_INTERNAL_H_
#define _EXT2_INTERNAL_H_

#include "ext2.h"
#include "ext2xattr.h"
#include "../arch/arch.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../block/block.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../panic.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/smp.h"
#include "../rtc.h"
#include "../timer.h"
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
#define EXT2_INDEX_FL           0x00001000u /* a hashed index lies behind the entries */

#define EXT2_COMPAT_RESIZE_INODE 0x0010u
#define EXT2_RO_SPARSE_SUPER     0x0001u
#define EXT2_RO_LARGE_FILE       0x0002u
#define EXT2_RO_EXTRA_ISIZE      0x0040u
#define EXT2_INCOMPAT_FILETYPE   0x0002u

#define EXT2_VALID_FS 0x0001u
#define EXT2_ERROR_FS 0x0002u

/* s_errors and the `errors=` option: what a fault met at run time does. */
#define EXT2_ERRORS_CONTINUE 1
#define EXT2_ERRORS_RO       2
#define EXT2_ERRORS_PANIC    3

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

/*
 * The modes of a mount (`em->mode`). RO writes nothing but at its opening;
 * RW admits changes, allocates and keeps the superblock's state; STOPPED,
 * which a fault under EXT2_ERRORS_RO makes of RW, admits no change and
 * allocates nothing, while what is under way still finishes, rolls back,
 * reclaims and drains, as Linux's ext2 goes on under SB_RDONLY, until a
 * remount writable checks the volume. A mount whose device write failed is
 * FAILED whatever its mode (`ext2_failed`), and writes nothing more.
 */
#define EXT2_MODE_RO      0
#define EXT2_MODE_RW      1
#define EXT2_MODE_STOPPED 2

/* The bitmaps of a group, by the byte of its descriptor that names each. */
#define EXT2_BLOCK_BITMAP 0
#define EXT2_INODE_BITMAP 4

/* A time of an inode: seconds, and nanoseconds where the inode keeps them. */
struct ext2_time {
    int64_t sec;
    uint32_t nsec;
};

/* The times `ext2_stamp` sets. */
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

/* The shared blocks a node lists for its `fsync` (`ext2_node_share`). */
#define EXT2_NODE_SHARED 8

/* An inode in memory, from the heap, on its mount's list and in its hash. */
struct ext2_node {
    struct ext2_node *next;  /* the mount's list of nodes in memory */
    struct ext2_node *prev;  /* the node before it on that list, or NULL */
    struct ext2_node *hnext; /* the next node in its bucket of the mount's hash */
    uint32_t refs; /* holds, changed atomically (`ext2_node_ref`); the last put frees it */
    uint32_t ino;
    uint32_t mode;
    uint32_t uid, gid;
    uint32_t nlink;
    uint64_t size;                /* 64 bits for a regular file, 32 for the rest */
    uint32_t size_seq;            /* odd while `size` changes (`ext2_node_size_set`) */
    uint32_t blocks;              /* i_blocks, in 512-byte units */
    uint32_t block[EXT2_NBLOCKS]; /* 12 direct, then single, double and triple indirect */
    struct ext2_time atime, mtime, ctime;
    struct ext2_time crtime; /* i_crtime, kept where `extra_isize` covers it */
    uint32_t extra_isize;    /* i_extra_isize; 0 for a 128-byte inode */
    uint32_t flags;          /* i_flags, which the VFS enforces in part */
    uint32_t generation;     /* i_generation, new for each inode made */
    uint32_t file_acl;       /* i_file_acl: the extended-attribute block, or 0 */
    /* A fast link, its target in `block`: decided once, when the inode is
     * read or the link made, as Linux sets `i_link` (`readlink_cached`). */
    int fast_target;
    uint32_t add_from; /* a directory's offset of the block where the last entry went */
    /* A directory's block where its next lookup starts, the one its last
     * lookup found a name in, kept under its lock, as Linux keeps
     * `i_dir_start_lookup` (fs/ext2/dir.c:356 and 395). */
    uint32_t lookup_from;
    /* Entry changes, under the directory lock; not stored on disk. */
    uint64_t dir_version;
    /* A field `inode_encode` writes changed in memory and no store of it
     * has succeeded since, under the node lock: set by every change outside
     * the batch (`ext2_node_dirty`) and cleared only by a store that
     * succeeded (`ext2_inode_write`) or the inode's free
     * (`ext2_release_inode`). The last put, `sync`, `fsync` and a remount
     * writable store a marked node, and every change of a directory's
     * entries stores it first (`dir_settle`). The batch sets no mark: it
     * holds its nodes and its drain stores them. */
    int unstored;
    int unlinked;                /* no name left; the inode and its blocks go at the last put */
    int state;                   /* N_LOADING, N_LIVE, N_DYING or N_FAILED */
    int load_error;              /* the -errno of a load that failed */
    struct sleep_lock lock;      /* the node lock (`ext2_node_lock`) */
    int queued;                  /* on `em->reap`, which no thread is freeing yet */
    struct ext2_node *reap_next; /* the next node on `em->reap` */
    /* The blocks other files share that were dirtied for this inode since
     * its last `fsync`, `nshared` of them: bitmaps, descriptors and the
     * superblock. `shared_over` once more were, after which its `fsync`
     * writes the whole device. Its table block is not listed, since an
     * `fsync` always writes it. */
    uint32_t shared[EXT2_NODE_SHARED];
    int nshared, shared_over;
    /* Writes under way of blocks taken off the list (`node_blocks_write`),
     * which wake `node_wq` as the last ends. */
    int writing;
    /* Calls under way that write its table block: `fdatasync` writes that
     * block too while one is, since it cleared `dirty_meta` at its start. */
    int table_writing;
    /* Its list went to a write of the whole device that may not have reached
     * it yet (`ext2_device_write_begin`). */
    int unlisted;
    /* Made while a list lost with a node was not yet on the device
     * (`em->shared_lost`, `em->device_lost`), which may have been an earlier
     * node's of this inode: its blocks count as listed while either is set,
     * until a write of the whole device ends with neither (`table_names`). */
    int lost_list;
    /* Set when a store changes a field of the inode other than its times,
     * and when the node is made, and cleared as an `fsync` writes the
     * table block: `fdatasync` writes that block only while it is set. */
    int dirty_meta;
};

/* One open file, holding `node` until release. The directory cursor and
 * version belong to this open, under the node lock, not to other opens. */
struct ext2_handle {
    struct ext2_node *node;
    uint32_t cursor;      /* the last position returned, or ~0u before the first read */
    uint64_t dir_version; /* the directory version at that position */
};

/* `ext2_write_bytes`'s ways to write on a mount with `sync` or `dirsync`
 * (`ext2_em_strict`): through to the medium; through to the device without its
 * flush, which a later flush covers; and through to the medium on any
 * mount, for the superblock's counts and state. 0, and the first two on any
 * other mount, leave the bytes dirty in the cache for `sync`. On any mount
 * WRITE_FIRST and WRITE_AFTER leave them dirty in the cache's two classes,
 * a `first` block reaching the device before any `after` one (bcache.h):
 * `ext2_punch` clears pointers first and bits after. */
#define WRITE_THROUGH      1
#define WRITE_UNFLUSHED    2
#define WRITE_DURABLE      3
#define WRITE_FIRST        4
#define WRITE_AFTER        5
/* `ext2_write_owned`'s mark of a pointer block at `level` of its file's tree, 1
 * for one that names data, added to one of the ways above; the cache then
 * writes it after the blocks below it (`bcache_dirty_pointer`). */
#define WRITE_LEVEL(level) ((level) << 4)
#define WRITE_WAY          15
/* The kind of the block a write changes, a READ_ kind, added to the above:
 * a write of part of a block the cache lacks reads it first under that
 * kind's rule (`read_failed`), so that a failed read is logged and reported
 * as the read primitive's of that kind is. */
#define WRITE_KIND(kind)   ((kind) << 8)

/*
 * The file-write batch's limits: the bytes it takes, its data and metadata
 * images, and the inodes it holds. Then its stages, drained in order: the
 * allocation bitmaps and descriptors, the data, the pointer blocks a level
 * at a time from those that name data up (WB_LEAF to WB_LEAF + 2), the
 * superblock's `large_file` flag, and the inode table blocks.
 */
#define EXT2_WB_BYTES       (1024u * 1024u)
/* What a process writes before em_balance asks it to write back. */
#define EXT2_BALANCE_SHARE  (1024u * 1024u)
/* The blocks a write or a drain handles between two offers of the big lock:
 * at some 58 us a block, the 15 ms of a whole batch becomes about 1 ms. */
#define EXT2_SERVICE_BLOCKS 16u
#define EXT2_WB_DATA        256
#define EXT2_WB_META        32
#define EXT2_WB_NODES       16
#define WB_ALLOC            0
#define WB_DATA             1
#define WB_LEAF             2
#define WB_FEATURE          5
#define WB_INODE            6

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
    uint64_t born;        /* the tick of the first byte since the batch was empty */
    uint32_t inode_dirty; /* bit per `nodes` entry, until its table buffer is stored */
    int stage, error;
    /* Kept by `em->batch` and this spinlock together: `datas`, `metas`,
     * `count`, `nodes` and each image's `block` and `stage`; a holder of
     * either may read them. */
    struct spinlock lock;
};

/* The groups' locks a mount has, which the group numbers hash to by their
 * low bits, as Linux's `bgl_lock_ptr` hashes them to NR_BG_LOCKS; a power
 * of two. */
#define EXT2_GROUP_LOCKS 64

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
    /* What a fault met at run time does, an EXT2_ERRORS_ value: s_errors,
     * or `errors=` of the mount's or a remount's options; 0 only before
     * the constructor reads the superblock. */
    int errors;
    /* The generation the next inode made gets, counted from the mount's
     * second, so a number reused for another file has another generation. */
    uint32_t next_generation;
    /* EXT2_MODE_RO, EXT2_MODE_RW or EXT2_MODE_STOPPED, asked only through
     * the predicates `ext2_admits`, `ext2_allocates`, `ext2_writes` and
     * `ext2_keeps_state`, and changed only by the mount, `ext2_mark_error`,
     * `finish_writes` and `remount_writable`, each under `em->super`. Read
     * without a lock, as one word. */
    int mode;
    /* Set while a writable mount or remount checks and opens the volume,
     * before the mode is EXT2_MODE_RW: the only time a mount that is not
     * writable may write (`ext2_writes`). */
    int opening;
    /* The free counts, kept equal to the sums of the descriptors'. */
    uint64_t free_blocks, free_inodes;
    int valid_cleared; /* the superblock's valid bit was cleared by this mount */
    /* Set when this mount's check stopped at one of its own limits before
     * it could judge the volume: no unmount sets the valid bit, so the
     * volume keeps the state that called for a check, as Linux puts back at
     * unmount the state the volume had at mount (`ext2_put_super`). */
    int check_owed;
    /* Why the volume may only be read, which a remount cannot change, or
     * NULL for a volume read-only because it was asked to be. */
    const char *readonly_why;
    /* The file-write batch, set up when the mount becomes writable. */
    struct ext2_writeback *wb;
    uint32_t alloc_hint; /* the block after the last one given out */
    /* The group descriptors, `groups * 32` bytes from the heap, and after
     * them a byte per group, set once its block bitmap has been checked
     * (`bitmap_check` in ext2_alloc.c). */
    uint8_t *gd;
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
    /* The locks ext2.Readme.md's Locks section lists: the batch's
     * (`ext2_batch_lock`), the groups', which group `g` takes at
     * `g % EXT2_GROUP_LOCKS` (`ext2_group_lock`), and the superblock's
     * (`ext2_super_lock`). */
    struct sleep_lock batch;
    struct sleep_lock group_locks[EXT2_GROUP_LOCKS];
    struct sleep_lock super;
    /* Keeps `free_blocks`, `free_inodes`, `alloc_hint`, `next_generation`,
     * `state`, `compat` and `ro_compat` (`ext2_count_blocks` and the
     * functions beside it). A 32-bit one of them may be read alone as a
     * single word, which is current at some moment of the read. */
    struct spinlock count_lock;
    /* Keeps `writers` and `closing`, which a writer without the big lock
     * changes on its admission (`em_enter`). */
    struct spinlock admit_lock;
    /* Keeps what an `fsync` reads of the lists of shared blocks: each
     * node's `shared`, `nshared`, `shared_over`, `writing`,
     * `table_writing`, `unlisted`, `lost_list` and `dirty_meta`, and the
     * mount's `shared_lost`, `device_lost` and `device_writes`, as Linux
     * keeps an inode's list of metadata buffers under the mapping's
     * `i_private_lock` (fs/buffer.c:475-486); and the node hash's chains,
     * which change under it and the big lock both, so that a sync's walk,
     * which may run without the big lock, looks a node up under it alone.
     * Taken under the cache's walks and its spinlock (`table_names`), so
     * nothing under it calls the cache. */
    struct spinlock share_lock;
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
    int device_writes; /* writes of the whole device under way */
    int device_lost;   /* one of them took over `shared_lost` */
};

/* ---- bytes off the volume, through the cache ---- */

/* The plans of one piece or allocation that may meet a block the cache
 * lacks and start again; the next reads it under its locks. */
#define PLAN_TRIES 8

/*
 * The kinds of block the read primitive reads (`ext2_read_bytes`,
 * `ext2_block_get`), each with its rules in `read_rules` in ext2.c: what is
 * checked before the read and whether a failed read is reported. QUIET is
 * the check's and the mount's, and a hint's that may not report; DATA a
 * file's data or a directory's entries; SLOT an inode's slot in its table;
 * BITMAP a block or inode bitmap; DESC a block of the group descriptors;
 * SUPER the superblock; MAP a pointer block a lookup or a map walks;
 * RECLAIM one a free walks; ATTR an attribute block read or replaced; and
 * ATTR_FREE one whose reference an inode's free drops.
 */
#define READ_QUIET     0
#define READ_DATA      1
#define READ_SLOT      2
#define READ_BITMAP    3
#define READ_DESC      4
#define READ_SUPER     5
#define READ_MAP       6
#define READ_RECLAIM   7
#define READ_ATTR      8
#define READ_ATTR_FREE 9
#define READ_KINDS     10

/* ---- the writer ---- */

/*
 * The order of writes, from ext2.Readme.md, which a mount with `sync` or
 * `dirsync` keeps on the medium (`ext2_em_strict`) and any other mount leaves
 * to the cache: an inode is freed by writing it zeroed with its deletion time
 * before its bitmap bit clears, and allocated by setting its bit before it
 * is written and named; a block is allocated in its bitmap before a pointer
 * names it, a pointer block is written before the pointer to it, and
 * `i_blocks` goes with the pointers and `i_size` last; an entry is removed
 * before the link count drops; a truncation shrinks the size first, then
 * clears pointers from the end, then frees blocks; a directory grows by a
 * zeroed block chained before the size covers it. A free of a file's blocks
 * is one batch whose order the cache keeps (`ext2_punch`), and a file's writes
 * go through the file-write batch, whose stages keep the same order
 * (`ext2_writeback_drain`).
 */

/* Store `v` at `p`, little-endian. The value is taken once: a count read
 * from the bytes being written, as `ext2_le16(gd + 12) - 1`, would otherwise be
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

/*
 * One `ext2_punch`'s blocks, on its caller's stack: the `count` blocks whose
 * pointers are cleared and whose bits are not, of the `cap` that `blocks`
 * holds. Each punch has its own, so that two of them, of two files and
 * interleaved at their disk waits, share nothing but the cache's order.
 */
struct punch_list {
    uint32_t *blocks;
    int count, cap;
};

/* A file with more than one link: its inode, its link count, and the names
 * the walk has found for it. */
struct ext2_link {
    struct ext2_link *next;
    uint32_t ino;
    uint32_t expect, found;
};

/* ext2.c */
struct ext2_node *ext2_node_find(struct ext2_mount *em, uint32_t ino);
uint32_t ext2_node_ref(struct ext2_node *n, int delta);
void ext2_node_size_set(struct ext2_node *n, uint64_t size);
struct ext2_node *ext2_node_make(void);
void ext2_node_publish(struct ext2_mount *em, struct ext2_node *n, uint32_t ino);
void ext2_node_unhold(struct ext2_mount *em, struct ext2_node *n);
void ext2_node_settle(struct ext2_mount *em, struct ext2_node *n, int rc);
uint32_t ext2_le16(const uint8_t *p);
uint32_t ext2_le32(const uint8_t *p);
struct ext2_mount *ext2_em_of(struct vfs_mount *mnt);
void ext2_node_lock(struct ext2_node *n);
void ext2_node_unlock(struct ext2_node *n);
void ext2_assert_node_locked(const struct ext2_node *n);
void ext2_batch_lock(struct ext2_mount *em);
void ext2_batch_unlock(struct ext2_mount *em);
void ext2_assert_batch_locked(struct ext2_mount *em);
void ext2_group_lock(struct ext2_mount *em, uint32_t g);
void ext2_group_unlock(struct ext2_mount *em, uint32_t g);
int ext2_group_held(struct ext2_mount *em, uint32_t g);
void ext2_assert_group_locked(struct ext2_mount *em, uint32_t g);
void ext2_super_lock(struct ext2_mount *em);
void ext2_super_unlock(struct ext2_mount *em);
void ext2_count_blocks(struct ext2_mount *em, int delta, uint32_t hint);
void ext2_count_inodes(struct ext2_mount *em, int delta);
uint64_t ext2_free_blocks(struct ext2_mount *em);
uint64_t ext2_free_inodes(struct ext2_mount *em);
uint32_t ext2_alloc_hint(struct ext2_mount *em);
uint32_t ext2_generation_next(struct ext2_mount *em);
uint32_t ext2_state_change(struct ext2_mount *em, uint32_t set, uint32_t clear);
uint32_t ext2_feature_add(struct ext2_mount *em, uint32_t *word, uint32_t bits);
void ext2_plan_begin(struct ext2_mount *em, int tries);
uint32_t ext2_plan_end(struct ext2_mount *em);
int ext2_zone_check(struct ext2_mount *em, uint32_t ino, const char *what, uint32_t b);
struct bbuf *ext2_block_get(struct ext2_mount *em, int kind, uint32_t ino, uint64_t blk, int *rc);
int ext2_plan_has(struct ext2_mount *em, uint32_t blk);
void ext2_volume_fetch(struct ext2_mount *em, uint64_t blk);
int ext2_read_bytes(struct ext2_mount *em, int kind, uint32_t ino, uint64_t off, void *dst,
                    uint32_t len);
int ext2_read_reclaim(struct ext2_mount *em, int kind, uint32_t ino, uint64_t off, void *dst,
                      uint32_t len);
int ext2_has_super(const struct ext2_mount *em, uint32_t g);
uint32_t ext2_gd_blocks(const struct ext2_mount *em);
uint32_t ext2_inode_table_blocks(const struct ext2_mount *em);
int ext2_data_block_ok(const struct ext2_mount *em, uint32_t b);
int ext2_extra_has(const struct ext2_node *n, uint32_t end);
int ext2_fast_link(const struct ext2_mount *em, const struct ext2_node *n);
int ext2_inode_load(struct ext2_mount *em, uint32_t ino, struct ext2_node *n);
int ext2_inode_check_load(struct ext2_mount *em, uint32_t ino, struct ext2_node *n);
int ext2_stale(struct ext2_mount *em, uint32_t ino);
struct ext2_node *ext2_node_for(struct ext2_mount *em, uint32_t ino, int *rc);
void ext2_put(struct vfs_mount *mnt, void *node);
int ext2_block_at(struct ext2_mount *em, const struct ext2_node *n, uint32_t index, uint32_t *out);
int ext2_read_data(struct ext2_mount *em, const struct ext2_node *n, uint64_t off, void *dst,
                   uint32_t len, int *unread);
void ext2_device_write_begin(struct ext2_mount *em);
void ext2_device_write_end(struct ext2_mount *em);
int ext2_em_write_out(struct vfs_mount *mnt);
struct ext2_time ext2_time_fit(const struct ext2_node *n, uint32_t extra_end, int64_t sec,
                               int64_t nsec);
void ext2_node_dirty(struct ext2_node *n);
void ext2_stamp(struct ext2_node *n, int which);
void ext2_stamp_batch(struct ext2_node *n, int which);
int ext2_em_strict(const struct ext2_mount *em);
int ext2_big_enter(void);
void ext2_big_exit(int taken);
int ext2_em_sync(struct ext2_mount *em);
void ext2_em_balance(struct ext2_mount *em);
void ext2_node_share(struct ext2_mount *em, struct ext2_node *n, uint32_t block);
void ext2_node_share_group(struct ext2_mount *em, struct ext2_node *n, uint32_t g, uint32_t bitmap);
int ext2_write_owned(struct ext2_mount *em, uint32_t owner, uint64_t off, const void *src,
                     uint32_t len, int sync);
int ext2_write_bytes(struct ext2_mount *em, uint64_t off, const void *src, uint32_t len, int sync);
int ext2_failed(const struct ext2_mount *em);
int ext2_admits(const struct ext2_mount *em);
int ext2_allocates(const struct ext2_mount *em);
int ext2_writes(const struct ext2_mount *em);
int ext2_keeps_state(const struct ext2_mount *em);
int ext2_assert_writable(struct vfs_mount *mnt, struct ext2_mount *em);
uint64_t ext2_inode_offset(const struct ext2_mount *em, uint32_t ino);
int ext2_slot_at(struct ext2_mount *em, int kind, uint32_t ino, uint64_t *at);
void ext2_node_meta_dirty(struct ext2_mount *em, struct ext2_node *n);
int ext2_inode_encode(struct ext2_mount *em, struct ext2_node *n, uint8_t *raw, uint32_t len);
int ext2_inode_write(struct ext2_mount *em, struct ext2_node *n, int fresh, int how);
int ext2_inode_store(struct ext2_mount *em, struct ext2_node *n);
int ext2_node_write_marked(struct ext2_mount *em, struct ext2_node *n);
int ext2_inode_times_later(struct ext2_mount *em, struct ext2_node *d);
void ext2_mark_error(struct ext2_mount *em);
int ext2_sb_store(struct ext2_mount *em);
uint32_t ext2_pointer_levels(const struct ext2_mount *em, uint32_t index);
void ext2_block_fetch(struct ext2_mount *em, uint64_t at);
int ext2_release_inode(struct ext2_mount *em, struct ext2_node *n);
int ext2_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags, const char *options);

/* ext2_alloc.c */
int ext2_gd_store(struct ext2_mount *em, uint32_t g);
int ext2_bit_first_clear(struct ext2_mount *em, uint32_t g, int which, uint32_t from,
                         uint32_t count, uint32_t *out);
int ext2_may_use_reserved(void);
int ext2_block_refused(struct ext2_mount *em, uint32_t g, uint32_t bit);
int ext2_inode_alloc(struct ext2_mount *em, uint32_t parent_ino, int is_dir, struct ext2_node *n);
int ext2_inode_bit_free(struct ext2_mount *em, uint32_t ino, int was_dir);
int ext2_attr_release(struct ext2_mount *em, uint32_t ino, uint32_t b);
void ext2_group_fetch(struct ext2_mount *em, uint32_t hint);
int ext2_alloc_block(struct ext2_mount *em, uint32_t hint, uint32_t *out);
void ext2_block_unwind(struct ext2_mount *em, uint32_t b);
int ext2_punch_free(struct ext2_mount *em, struct ext2_node *n, struct punch_list *list);
void ext2_gd_fetch(struct ext2_mount *em, uint32_t g);

/* ext2_batch.c */
void ext2_writeback_free(struct ext2_writeback *wb);
int ext2_writeback_has(struct ext2_mount *em, uint32_t block);
int ext2_writeback_copy(struct ext2_mount *em, uint32_t block, uint32_t at, void *buf, uint32_t len,
                        int into);
int ext2_batch_pending(const struct ext2_mount *em);
int ext2_writeback_holds(const struct ext2_mount *em, const struct ext2_node *n);
int ext2_writeback_keeps(struct ext2_mount *em, const struct ext2_node *n);
int ext2_writeback_error(struct ext2_mount *em, int rc);
int ext2_writeback_drain(struct ext2_mount *em);
int ext2_writeback_contiguous(struct ext2_mount *em, struct ext2_node *n, uint64_t offset,
                              uint32_t count);
int ext2_writeback_space(struct ext2_mount *em, struct ext2_node *n, uint64_t offset,
                         uint32_t count);
int ext2_writeback_fit(struct ext2_mount *em, struct ext2_node *n, uint64_t at, const uint8_t *in,
                       uint32_t take);
int ext2_writeback_room(struct ext2_mount *em, int synchronous);
int ext2_writeback_init(struct ext2_mount *em);

/* ext2_dir.c */
int ext2_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node);
int ext2_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor, struct vfs_dirent *ent);
int ext2_readlink(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap);
int ext2_readlink_cached(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap);
int ext2_path(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap);
uint32_t ext2_ftype_of(uint32_t mode);
int ext2_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode, void **node);
int ext2_mknod(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode, uint32_t rdev,
               void **node);
int ext2_unlink(struct vfs_mount *mnt, void *dir, const char *name);
int ext2_mkdir(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode);
int ext2_rmdir(struct vfs_mount *mnt, void *dir, const char *name);
int ext2_link(struct vfs_mount *mnt, void *dir, const char *name, void *node);
int ext2_symlink(struct vfs_mount *mnt, void *dir, const char *name, const char *target);
int ext2_rename(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
                const char *newname);

/* ext2_file.c */
int ext2_open(struct vfs_mount *mnt, void *node, int flags, void **handle);
void ext2_release(struct vfs_mount *mnt, void *handle);
int32_t ext2_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count, uint64_t offset);
int32_t ext2_read_frames(struct vfs_mount *mnt, void *handle, uint32_t first, uint32_t count,
                         const uintptr_t *frames, const struct block_frames_done *done);
int ext2_punch(struct ext2_mount *em, struct ext2_node *n, uint32_t keep);
int ext2_map_alloc(struct ext2_mount *em, struct ext2_node *n, uint32_t index, uint32_t *out,
                   int *fresh);
int ext2_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st);
int ext2_truncate(struct vfs_mount *mnt, void *handle, uint64_t size);
int ext2_fsync(struct vfs_mount *mnt, void *handle);
int ext2_fdatasync(struct vfs_mount *mnt, void *handle);
int32_t ext2_file_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                        uint64_t offset);
int ext2_fallocate(struct vfs_mount *mnt, void *handle, uint32_t mode, uint64_t offset,
                   uint64_t len);
void ext2_cachestat(struct vfs_mount *mnt, void *node, uint64_t first, uint64_t last,
                    struct vfs_cachestat *cs);

/* ext2_check.c */
int ext2_check_volume(struct ext2_mount *em);

#endif
