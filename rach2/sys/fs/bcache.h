#ifndef _BCACHE_H_
#define _BCACHE_H_

#include "../block/block.h"
#include "../proc/wait_queue.h"

/*
 * bcache: the buffer cache under the mounted file systems.
 *
 * The operations are the ones in Lions' commentary: get a block and hold it,
 * release it, mark it dirty, write a device's dirty buffers, drop a device's
 * buffers at unmount. A block is eight sectors; the last block of a device
 * whose sector count is not a multiple of eight is read and written as the
 * sectors that exist.
 *
 * Writes: metadata is written through and data is written back. A file
 * system writes a metadata buffer with `bcache_write`, which reaches the
 * device before it returns, so the order in which a driver writes metadata
 * is the order in which it reaches the disk. Data goes through
 * `bcache_dirty` and reaches the disk at eviction, at `fsync`, at `sync`,
 * and through the periodic sync process. A dirty victim is written before
 * its buffer is reused. A write that fails, by either path, is recorded
 * against the device through `vfs_device_failed`: the mounts on it go
 * read-only, and the buffer stays dirty for a later sync to try again.
 *
 * A held buffer is busy. The kernel does not preempt inside a syscall, so a
 * second process reaches a busy buffer only while the holder is asleep in a
 * request; the second sleeps on the buffer's queue and is woken at release.
 * Deadlock is kept out by two checked rules: a process holds at most
 * `BCACHE_HOLD_MAX` buffers, and while it holds any it may take only a higher
 * block on the same device. A release or a dirty mark by a process that does
 * not hold the buffer is a defect too. `bcache_get`, `bcache_release` and
 * `bcache_dirty` panic on a violation, so a file system that breaks a rule
 * fails in the host test and not on a machine.
 */

/* Policy: the fewest buffers the cache makes room for, however small the
 * target the memory gives it: a file system holds up to BCACHE_HOLD_MAX at
 * once, a write of one takes one more under it, and a second process needs
 * the same. The most is half the caches' target, or what
 * `bcache_set_limit` says. */
#define BCACHE_MIN_BUFFERS   16
#define BCACHE_BLOCK_SECTORS 8u
#define BCACHE_BLOCK_BYTES   (BCACHE_BLOCK_SECTORS * BLOCK_SECTOR_SIZE)
#define BCACHE_HOLD_MAX      4
/* A dirty buffer's place in its device's writeback: a `first` buffer reaches
 * the device before any `after` buffer of the device does, whichever path
 * writes; 0 is no place. The marks clear with the write. */
#define BCACHE_ORDER_FIRST   1
#define BCACHE_ORDER_AFTER   2

struct bbuf {
    struct bbuf *next;   /* the cache's list of buffers */
    struct bbuf *hnext;  /* the next buffer in its hash bucket */
    block_device_t *dev; /* NULL: the buffer is empty */
    uint64_t blk;        /* block index on dev, in 8-sector units */
    uint8_t *data;       /* BCACHE_BLOCK_BYTES, only the tail sectors valid
                          * on a short last block */
    uint32_t bytes;      /* valid bytes: a whole block, or the short tail */
    int dirty;
    /* The device has gone while this buffer was held: it is dropped at the
     * release rather than at the removal, because a buffer cannot be reused
     * under the process that holds it. */
    int discard;
    int holder;             /* pid holding it, or -1 */
    uint32_t stamp;         /* last release, for least-recently-released */
    int waiters;            /* processes waiting for the hold to end, woken or not */
    int write_failed;       /* its last write failed: not a victim until one succeeds */
    int order;              /* BCACHE_ORDER_FIRST, BCACHE_ORDER_AFTER or 0 */
    wait_queue_t wq;        /* who waits for the hold to end */
    struct bbuf *hold_next; /* the next buffer its holder holds */
    struct bbuf *lru_prev;  /* the keyed buffers in release order, oldest first */
    struct bbuf *lru_next;
    struct bbuf *empty_next; /* the next buffer keyed to nothing */
    struct bbuf *dirty_prev; /* the dirty buffers, in no order */
    struct bbuf *dirty_next;
};

/**
 * Find or read block `blk` of `dev` and hold it.
 *
 * A miss on the block after the last one read from the device reads a run
 * of the blocks that follow with it. May sleep on the device, on a busy
 * buffer and on the write of a dirty victim, and panics when the caller
 * breaks a hold rule.
 *
 * @return the buffer, held; NULL for a block past the device's end, a
 *         failed read, or a full cache with no buffer it may write.
 */
struct bbuf *bcache_get(block_device_t *dev, uint64_t blk);

/**
 * Find block `blk` of `dev`, or take a buffer for it zeroed without a read.
 *
 * For a block the caller will write whole, or wants zeros in: the device's
 * bytes would be thrown away, and on a stick the read costs what the write
 * does. A cached block is returned as it is. Held, or NULL on an unwritable
 * full cache.
 */
struct bbuf *bcache_get_blank(block_device_t *dev, uint64_t blk);

/** Let a held buffer go and wake whoever waits for it; it becomes the most
 * recently released. A buffer of a device that went while it was held is
 * dropped. Panics when the caller does not hold it. */
void bcache_release(struct bbuf *b);

/** Mark a held buffer for writeback. A place it already has stays. Panics
 * when the caller does not hold it. */
void bcache_dirty(struct bbuf *b);

/**
 * Mark a held buffer for writeback with a place in its device's order.
 *
 * A `BCACHE_ORDER_FIRST` buffer reaches the device before any
 * `BCACHE_ORDER_AFTER` buffer of the same device does, whichever path
 * writes: a sync writes the firsts before the rest, in separate requests,
 * and an eviction or the memory reclaim that chose an `after` buffer writes
 * the device's firsts before it, or passes it over while one of them is
 * held or failed its write. The marks clear with the write. When ext2 frees
 * a file's blocks its pointer blocks and inode are first and its bitmaps and
 * descriptors after, so no pointer on the disk names a block whose bit is
 * clear, however the cache is pressed while the free runs.
 */
void bcache_dirty_ordered(struct bbuf *b, int order);

/**
 * Write a held buffer through to the device now.
 *
 * @return 0 with the buffer clean; the write's -errno with it dirty and the
 *         failure recorded; or -EROFS without a write once a write on the
 *         device has failed, so that an operation in progress when the
 *         device failed stops at its next metadata write.
 */
int bcache_write(struct bbuf *b);

/**
 * `bcache_write` without the disk's flush: the buffer is clean when the
 * write is answered, but on a write-through disk it may still be in the
 * disk's own cache, and it is on the medium once the next `bcache_write`
 * or `block_flush` of the device has been answered. For writes with no
 * order among themselves that one later write must follow: a caller writes
 * all but the last this way.
 */
int bcache_write_unflushed(struct bbuf *b);

/**
 * Write `dev`'s dirty buffers, the `first` ones in a pass of their own and
 * then the rest, each pass in ascending block order and in runs, with a
 * `first` dirtied meanwhile still before the next `after`, and on a
 * write-through disk end with a flush.
 *
 * A dirty buffer another process holds is waited for. The caller holds no
 * buffer of the device, and none that would forbid writing it; a caller
 * that does panics. A failed flush is recorded against the device as a
 * failed write is.
 *
 * @return 0, or the first failure's -errno with that buffer and the rest
 *         still dirty for the next call.
 */
int bcache_sync(block_device_t *dev);

/** `bcache_sync` without the disk's flush: every write is answered and has
 * reached the device, in the same order, and on a write-through disk the
 * writes may still be in its own cache until the caller's next
 * `block_flush`, a `bcache_sync` or a `bcache_write`. */
int bcache_sync_unflushed(block_device_t *dev);

/** Drop `dev`'s buffers at unmount and clear its failed-write mark; -EBUSY,
 * with nothing dropped, while any is held or dirty. */
int bcache_invalidate(block_device_t *dev);

/**
 * The device has gone: record it as failed, which takes its mounts
 * read-only, mark it gone, and deal with every buffer that names it.
 *
 * A buffer nobody holds is dropped at once, a dirty one too, since it has
 * nowhere left to be written and an unmount waits for the dirty ones to
 * go. One a process holds is marked to be dropped at its release, because
 * the holder is asleep in a request that is about to fail and its buffer
 * must not be handed to anyone else in the meantime.
 */
void bcache_device_gone(block_device_t *dev);

/** How many of `dev`'s buffers are held right now. */
int bcache_device_holds(block_device_t *dev);

/**
 * Give back up to `want` buffers, each a frame: empty ones, then clean
 * ones oldest first, then, with `can_write`, dirty ones written back where
 * the caller may write them, which sleeps. A buffer a process holds or
 * waits for is left alone. Returns how many went; `*written` is increased
 * by the writebacks.
 */
uint32_t bcache_shrink(uint32_t want, int can_write, uint32_t *written);

/** Buffers the cache holds now. */
uint32_t bcache_resident(void);

/** Whether the dirty buffers are a quarter of the cache's limit or more,
 * the share at which a file system that writes back writes them in runs
 * (`bcache_sync_unflushed`) rather than leave each to an eviction, which
 * writes its one block alone. */
int bcache_dirty_heavy(void);

/** Fix the most buffers the cache makes, for a test; 0 restores the rule. */
void bcache_set_limit(uint32_t n);

/** Gets served from the cache, for /proc/disk. */
uint64_t bcache_stat_hits(void);

/** Blocks read from a device into the cache, for /proc/disk. */
uint64_t bcache_stat_fills(void);

/** Waits for a busy buffer, for /proc/disk. */
uint64_t bcache_stat_waits(void);

/** The buffers the cache's walks of its lists have read, which is what its
 * bookkeeping costs, for /proc/disk. */
uint64_t bcache_stat_visits(void);

#endif
