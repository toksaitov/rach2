#ifndef _BCACHE_H_
#define _BCACHE_H_

#include "../block/block.h"
#include "../proc/wait_queue.h"

/*
 * bcache: the fixed buffer cache under the mounted file systems.
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

/* Policy: the fewest buffers the cache keeps room for, whatever memory says:
 * a file system holds up to BCACHE_HOLD_MAX at once, a write of one takes
 * one more under it, and a second process needs the same. The most is half
 * the caches' target, or what `bcache_set_limit` says. */
#define BCACHE_MIN_BUFFERS 16
#define BCACHE_BLOCK_SECTORS 8u
#define BCACHE_BLOCK_BYTES (BCACHE_BLOCK_SECTORS * BLOCK_SECTOR_SIZE)
#define BCACHE_HOLD_MAX 4

struct bbuf {
    struct bbuf *next;     /* the cache's list of buffers */
    block_device_t *dev;   /* NULL: the buffer is empty */
    uint64_t blk;          /* block index on dev, in 8-sector units */
    uint8_t *data;         /* BCACHE_BLOCK_BYTES, only the tail sectors valid
                            * on a short last block */
    uint32_t bytes;        /* valid bytes: a whole block, or the short tail */
    int dirty;
    /* The device has gone while this buffer was held: it is dropped at the
     * release rather than at the removal, because a buffer cannot be reused
     * under the process that holds it. */
    int discard;
    int holder;            /* pid holding it, or -1 */
    uint32_t stamp;        /* last release, for least-recently-released */
    uint32_t tried;        /* the victim search that passed this buffer over */
    wait_queue_t wq;       /* who waits for the hold to end */
};

/** Find or read block `blk` of `dev`; returns it held, or NULL on a failed
 * read or an unwritable full cache. May sleep on a sleeping device or a busy
 * buffer. */
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

/** Let a held buffer go and wake whoever waits for it. */
void bcache_release(struct bbuf *b);

/** Mark a held buffer for writeback. */
void bcache_dirty(struct bbuf *b);

/** Write a held buffer through to the device now; 0 with the buffer clean,
 * or the write's -errno with it dirty and the failure recorded. */
int bcache_write(struct bbuf *b);

/** Write `dev`'s dirty buffers in ascending block order; 0, or the first
 * write's -errno with that buffer and the rest still dirty for the next
 * call. */
int bcache_sync(block_device_t *dev);

/** Drop `dev`'s buffers at unmount; -EBUSY while any is held or dirty. */
int bcache_invalidate(block_device_t *dev);

/**
 * The device has gone: deal with every buffer that names it.
 *
 * A clean buffer nobody holds is dropped at once. A dirty one has nowhere
 * left to be written, so the device is recorded as failed, which takes its
 * mounts read-only, and the buffer is dropped rather than kept dirty
 * forever, since an unmount waits for the dirty ones to go. One a process
 * holds is marked to be dropped at its release, because the holder is asleep
 * in a request that is about to fail and its buffer must not be handed to
 * anyone else in the meantime.
 */
void bcache_device_gone(block_device_t *dev);

/** How many of `dev`'s buffers are held right now. */
int bcache_device_holds(block_device_t *dev);

/**
 * Give back up to `want` buffers, each a frame: empty ones, then clean
 * ones oldest first, then dirty ones written back. Sleeps in the writes.
 * Returns how many went.
 */
uint32_t bcache_shrink(uint32_t want);

/** Buffers the cache holds now. */
uint32_t bcache_resident(void);

/** Fix the most buffers the cache makes, for a test; 0 restores the rule. */
void bcache_set_limit(uint32_t n);

/* Counters for /proc/disk: reads served from the cache, reads from the
 * device, and waits for a busy buffer. */
uint64_t bcache_stat_hits(void);
uint64_t bcache_stat_fills(void);
uint64_t bcache_stat_waits(void);

#endif
