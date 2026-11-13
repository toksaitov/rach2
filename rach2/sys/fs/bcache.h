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
 * and through the kernel's flusher (`writeback.h`), which writes it back
 * once it has been dirty for `dirty_expire_centisecs` or the dirty buffers
 * pass the background threshold, as Linux's does. Eviction, the flusher and
 * a writer past the dirty limit write without the disk's flush; `fsync`,
 * `sync` and an unmount end with it. A dirty victim is written before its
 * buffer is reused. A write that fails, by either path, is recorded
 * against the device through `vfs_device_failed`: the mounts on it go
 * read-only, and the buffer stays dirty for a later sync to try again.
 *
 * A held buffer is busy: a second process that asks for it sleeps on the
 * buffer's queue and is woken at release, as Linux's `lock_buffer` sleeps.
 *
 * Locks: no call needs the big lock. A get that finds its block cached, or
 * a clean buffer to fill, a release, an invalidation and the questions
 * `bcache_cached`, `bcache_state`, `bcache_copy_cached`, `bcache_busy` and
 * `bcache_wait_unheld` take the cache's spinlock for their bookkeeping. A
 * dirty mark, a write, a sync, a reserve, the writes of a shrink, the
 * removal of a device, the release of a dirty buffer that a removal or an
 * invalidation marked to drop, and a get that searches the cache for a
 * victim, which it may write, take the cache's write lock too, a mutex,
 * which keeps the order the dirty marks give and which each gives up around
 * its requests and waits; a caller may hold buffers and its own sleeping
 * locks when it calls them. A get with interrupts off takes no mutex: it
 * fills a miss only from an empty or a clean buffer. A failed write is
 * recorded against the device's mounts under the big lock, which the cache
 * takes for it when the caller runs without it.
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
 * the device before any `after` buffer of the device marked after it,
 * whichever path writes; 0 is no place. The marks clear with the write. */
#define BCACHE_ORDER_FIRST   1
#define BCACHE_ORDER_AFTER   2

struct bbuf {
    struct bbuf *next;   /* the cache's list of buffers */
    struct bbuf *prev;   /* the buffer before it on that list, or NULL */
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
    uint64_t owner;         /* the one file this dirty block is written for,
                             * or 0 for a shared block; clear while clean */
    int level;              /* its height among its owner's pointer blocks:
                             * 1 names data, 0 is no pointer block; clear
                             * while clean (`bcache_dirty_pointer`) */
    uint64_t dirtied;       /* the count of dirty marks when it last went
                             * dirty; 0 while clean */
    uint64_t touched;       /* the count at its last dirty mark */
    wait_queue_t wq;        /* who waits for the hold to end */
    struct bbuf *hold_next; /* the next buffer its holder holds */
    struct bbuf *lru_prev;  /* the keyed buffers in release order, oldest first */
    struct bbuf *lru_next;
    struct bbuf *empty_next; /* the next buffer keyed to nothing */
    struct bbuf *dirty_prev; /* its dirty list, by class, latest first */
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
 * A `BCACHE_ORDER_FIRST` buffer reaches the device before a
 * `BCACHE_ORDER_AFTER` buffer of the same device whose last mark came after
 * the first went dirty, whichever path writes: a sync writes the firsts
 * before the rest, in separate requests, and writes those an `after` waits
 * for before it; an eviction or the memory reclaim that chose an `after`
 * buffer writes every first of its device before it, or passes it over
 * while one of them is held, failed its write or waits itself
 * (`bcache_sync_last`). On a write-through disk the flush comes between. The
 * marks clear with the write. When ext2 frees a file's blocks its pointer
 * blocks and inode are first and its bitmaps and descriptors after, so no
 * pointer on the disk names a block whose bit is clear, however the cache is
 * pressed while the free runs.
 */
void bcache_dirty_ordered(struct bbuf *b, int order);

/**
 * Mark a held buffer for writeback as a block of `owner` alone, chosen by
 * the file system, such as a file's inode: its data and pointer blocks. An
 * owner of 0 makes the block shared again. Either way it is no pointer
 * block (`bcache_dirty_pointer`) after the mark.
 *
 * The tag lasts until the buffer is written, and the last one given stands;
 * a later `bcache_dirty` or `bcache_dirty_ordered` leaves it and its place
 * stays. A tag gone stale, as on a block freed and used again, costs one
 * needless write by `bcache_sync_owned`, and a block a device writes last
 * (`bcache_sync_last`) may then be kept waiting for it, or not wait for it,
 * so a file system that uses the block again for something shared marks it
 * with 0. Panics when the caller does not hold the buffer.
 */
void bcache_dirty_owned(struct bbuf *b, uint64_t owner);

/**
 * Mark a held buffer for writeback as a pointer block of `owner`, as
 * `bcache_dirty_owned` does, at `level` in its tree: 1 for a block that
 * names data, 2 for one that names such blocks, and so on. An `owner` of 0
 * is the tree of the shared blocks, such as a directory's.
 *
 * Such a block reaches the device, by any path, only after every dirty
 * block of the same owner below its level that went dirty before its last
 * mark, unless that one is an `after` or of the class `bcache_sync_last`
 * names: a block's data before the pointer to it, and a pointer block
 * before the one above it. The file system marks the children first.
 * Eviction, the shrink, the reserve and a write after a failure treat it
 * as they treat a block written last. The level lasts until the write or a
 * later `bcache_dirty_owned`, which makes the block a leaf again. Panics
 * when the caller does not hold the buffer.
 */
void bcache_dirty_pointer(struct bbuf *b, uint64_t owner, int level);

/**
 * Write a held buffer through to the device now, in the caller's order: no
 * block it would wait for in a sync is written first.
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
 * Write the buffers of `dev` that were dirty when the call began, the
 * `first` ones in a pass of their own and then the rest, each pass in
 * ascending block order and in runs, and on a write-through disk end with a
 * flush. A block that must wait for others goes after them: an `after` after
 * the firsts it follows, and a block the device writes last
 * (`bcache_sync_last`) after the blocks it may name, whenever they were
 * dirtied. A buffer dirtied first after the call began is the next call's,
 * so that a steady writer does not keep the sync.
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

/**
 * Flush `dev`'s own cache to the medium (`block_flush`), and record a
 * failure as a failed write is: the device marked failed, and its mounts made
 * read-only with the error recorded once for each file system's syncs to
 * report (`vfs_device_failed`), as Linux's `mapping_set_error` records a
 * failed write-back. Every flush a file system makes goes through it, so
 * that a failure its own operation meets reaches the descriptions that did
 * not make it. The caller holds no buffer of the device. 0 or -errno.
 */
int bcache_flush(block_device_t *dev);

/**
 * Name the blocks of `dev` that point at others, such as ext2's inode
 * tables, which a sync writes after every other: `last(arg, blk)` is 1 for
 * such a block, and NULL names none. `names(arg, blk, owner)` is 1 when that
 * block may point at the blocks tagged `owner` (`bcache_dirty_owned`); NULL
 * says yes to every owner.
 *
 * Such a block reaches the device, by any path, only after every dirty
 * block it may name: one of the device that is not of the class, not an
 * `after`, shared or of an owner it names, and dirty since before the
 * block's last mark. A write of the device that fails so keeps every such
 * block marked since off the device, and the `after` blocks that wait for
 * it. A sync writes what one waits for first, holding it so that nothing is
 * stored in it meanwhile; an eviction, shrink or reserve passes it over
 * until nothing is left to wait for, and once a write of the device has
 * failed writes none of them: not alone, not in a run, and not ahead of an
 * `after`, which stays dirty with it. On a write-through disk the flush
 * comes between. A sync still retries them. The callbacks run under the
 * walks of the dirty buffers, under the cache's write lock, and at a
 * release under its spinlock, and must not sleep; the caller clears them
 * before `arg` goes, which waits for a walk that runs them.
 */
void bcache_sync_last(block_device_t *dev, int (*last)(void *arg, uint64_t blk),
                      int (*names)(void *arg, uint64_t blk, uint64_t owner), void *arg);

/**
 * Write `owner`'s dirty buffers on `dev` and no others, as `bcache_sync` does
 * in its passes and rules, in runs that hold only that owner's blocks, and
 * without the flush that ends a sync: dirty neighbours stay dirty and the
 * caller flushes. A run of `first` buffers keeps its flush, and a block of
 * the `after` class goes after the device's firsts it follows, whoever
 * owns them. An `owner` of 0 writes every dirty buffer of the device.
 *
 * @return 0, or the first failure's -errno with that buffer and the rest
 *         still dirty for the next call.
 */
int bcache_sync_owned(block_device_t *dev, uint64_t owner);

/**
 * Write one shared block of `dev` if it is in the cache and dirty, alone and
 * without the flush that ends a sync, and with the flush for a `first`. A
 * block of the `after` class goes after the device's firsts it follows,
 * whoever dirtied them, which a file's sync needs before a shared bitmap or
 * descriptor, and a block the device writes last after the blocks it may
 * name (`bcache_sync_last`), with the flush between on a write-through
 * disk. A holder of the block is waited for, and the caller's holds are
 * bound as for `bcache_sync`.
 *
 * @return 0, also for a block that is clean or gone from the cache, or the
 *         first failure's -errno with its buffer still dirty.
 */
int bcache_sync_block(block_device_t *dev, uint64_t blk);

/**
 * Clear the way for `bcache_sync_block` of the same block, for a caller that
 * must check something in the same step as that call takes the block: it
 * writes what the block waits for, one write a call.
 *
 * @return 0 when a `bcache_sync_block` now takes the block without a sleep
 *         and holds it until its write ends, or finds nothing to write; 1
 *         after a wait or a write, which slept, so that the caller checks
 *         again and calls this again; or the -errno of a failed write.
 */
int bcache_sync_block_ready(block_device_t *dev, uint64_t blk);

/**
 * Write dirty victims until the next `n` misses for blocks of `dev` can have
 * a buffer without a write, so that a caller about to take `n` buffers under
 * a short lock does not wait on the disk there. Call it with no file lock
 * held; it sleeps.
 *
 * Best effort: nothing is pinned, another process may take or dirty the
 * buffers afterwards, and a failed write, a buffer that may not be written
 * now, or a request above the cache's limit ends it early.
 *
 * @return how many buffers were ready at the last look, at most `n`.
 */
uint32_t bcache_reserve(block_device_t *dev, uint32_t n);

/** Whether `dev`'s block `blk` is in the cache, so that a `bcache_get` of it
 * reads nothing unless it is taken for another block first. */
int bcache_cached(block_device_t *dev, uint64_t blk);

/** Whether `dev`'s block `blk` is in the cache: 0 when it is not, 1 when it
 * is, and 2 when it is and is dirty, for cachestat(2). */
int bcache_state(block_device_t *dev, uint64_t blk);

/** Copy a resident buffer's bytes [offset, offset + count) into `out`
 * without a disk read, a wait or a hold. Return 1, or 0 for an absent,
 * discarded or busy buffer or a range outside its valid bytes. The state
 * check and the copy are one step under the cache's spinlock, so the caller
 * needs no big lock. */
int bcache_copy_cached(block_device_t *dev, uint64_t blk, uint32_t offset, void *out,
                       uint32_t count);

/** Whether `dev`'s block `blk` is in the cache and held, by a write, a fill
 * or a caller, so that a `bcache_get` of it would sleep until the holder
 * lets it go, the length of a disk request when that is a write or a fill. */
int bcache_busy(block_device_t *dev, uint64_t blk);

/**
 * Sleep until nobody holds `dev`'s block `blk`, or at once when the cache
 * lacks it; nothing is read. For a caller about to take a lock under which
 * it would otherwise wait on the holder's disk request. The caller holds no
 * buffer, and may find the block held again by the time it looks.
 */
void bcache_wait_unheld(block_device_t *dev, uint64_t blk);

/** `bcache_sync` without the disk's flush: every write is answered and has
 * reached the device, in the same order, and on a write-through disk the
 * writes may still be in its own cache until the caller's next
 * `block_flush`, a `bcache_sync` or a `bcache_write`. */
int bcache_sync_unflushed(block_device_t *dev);

/**
 * Drop `dev`'s buffers at unmount and clear its failed-write mark. -EBUSY,
 * with nothing dropped, when a first walk of the buffers finds one of
 * `dev`'s held or dirty; 0 otherwise. Both walks let the cache's lock go
 * between batches, so a buffer of `dev` taken behind the first walk may be
 * held or dirty when the second reaches it: a held one is dropped at its
 * release, and a dirty one is left for its write. The caller has stopped
 * every use of `dev`, so that neither comes but by a race.
 */
int bcache_invalidate(block_device_t *dev);

/**
 * Drop every buffer of `dev` at the end of an unmount, whatever its state,
 * and clear the device's failed-write mark: a dirty one's write is given up,
 * a held one is dropped at its release, and the rest at once, as Linux's
 * `kill_block_super` goes on past a failed `sync_blockdev` and
 * `invalidate_bdev` passes over a busy page (fs/super.c:1722). The count of
 * dirty buffers given up. The device is not marked failed or gone, so it
 * can be mounted again. The caller has stopped every use of `dev` and
 * written what could be written; a buffer still dirty is one whose write
 * failed, or one held back behind it by the cache's order.
 */
int bcache_drop(block_device_t *dev);

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

/** How many of `dev`'s buffers are held, each counted as a walk of the
 * buffers reaches it: the walk lets the cache's lock go between batches,
 * so one taken behind it is not counted, and one let go once counted is. */
int bcache_device_holds(block_device_t *dev);

/**
 * Give back up to `want` buffers, each a frame: empty ones, then clean
 * ones oldest first, then, with `can_write`, dirty ones written back where
 * the caller may write them, which sleeps and takes the write lock. A
 * buffer a process holds or waits for is left alone. Returns how many
 * went; `*written` is increased by the writebacks.
 */
uint32_t bcache_shrink(uint32_t want, int can_write, uint32_t *written);

/** Buffers the cache holds now. */
uint32_t bcache_resident(void);

/** The most buffers the cache makes now, for /proc/disk. */
uint32_t bcache_capacity(void);

/** The buckets of the cache's index by device and block, for /proc/disk. */
uint32_t bcache_buckets(void);

/**
 * Where the dirty buffers stand against Linux's thresholds, taken of the
 * cache's limit, the most memory dirty data can hold here, as Linux takes
 * them of the memory its page cache can grow into: 0 below the background
 * threshold (`dirty_background_ratio` per cent, or `dirty_background_bytes`
 * when set), 1 at or past it, where the flusher writes back, and 2 at or
 * past the dirty limit (`vm_dirty_ratio` per cent, or `vm_dirty_bytes`),
 * where a writer writes back itself (`balance_dirty_pages`). A background
 * threshold not below the limit is half the limit, as in Linux's
 * `domain_dirty_limits`.
 */
int bcache_dirty_level(void);

/** The dirty buffers now. */
uint32_t bcache_dirty_count(void);

/* /proc/sys/vm's dirty_background_ratio, dirty_background_bytes,
 * dirty_ratio and dirty_bytes: percents from 0 to 100 and byte counts, of
 * which the bytes count when not 0. */
extern int dirty_background_ratio;
extern unsigned long dirty_background_bytes;
extern int vm_dirty_ratio;
extern unsigned long vm_dirty_bytes;

/** Name the call that wakes the flusher when a dirty mark makes the first
 * dirty buffer of a clean cache or leaves the dirty buffers at or past the
 * background threshold; NULL names none. The call runs under the cache's
 * write lock and must not sleep. */
void bcache_set_flusher(void (*kick)(void));

/** The count of dirty marks so far, which grows by one at each; a buffer
 * gone dirty after a reading of it was marked later than it. */
uint64_t bcache_marks(void);

/**
 * Write the buffers of `dev` that went dirty at or before the mark count
 * `since` (`bcache_marks`), in the passes, runs and order of `bcache_sync`,
 * without the disk's flush: the write-back of the flusher and of a writer
 * past the dirty limit. A block one of them waits for is written first,
 * however young. 0 for `since` writes nothing, and a count not reached yet
 * every buffer dirty when the call began. A device whose write has failed
 * is left to `bcache_sync`. Holds as `bcache_sync`.
 *
 * @return 0, or the first failure's -errno with that buffer and the rest
 *         still dirty.
 */
int bcache_writeback(block_device_t *dev, uint64_t since);

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
