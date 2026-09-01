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
 * Nothing writes through the cache yet: `bcache_dirty` and `bcache_sync`
 * exist for the first file system that writes, and the host test proves what
 * they do.
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

#define BCACHE_BUFFERS 64
#define BCACHE_BLOCK_SECTORS 8u
#define BCACHE_BLOCK_BYTES (BCACHE_BLOCK_SECTORS * BLOCK_SECTOR_SIZE)
#define BCACHE_HOLD_MAX 4

struct bbuf {
    block_device_t *dev;   /* NULL: the buffer is empty */
    uint64_t blk;          /* block index on dev, in 8-sector units */
    uint8_t *data;         /* BCACHE_BLOCK_BYTES, only the tail sectors valid
                            * on a short last block */
    uint32_t bytes;        /* valid bytes: a whole block, or the short tail */
    int dirty;
    int holder;            /* pid holding it, or -1 */
    uint32_t stamp;        /* last release, for least-recently-released */
    wait_queue_t wq;       /* who waits for the hold to end */
};

/** Find or read block `blk` of `dev`; returns it held, or NULL on a failed
 * read or an unwritable full cache. May sleep on a sleeping device or a busy
 * buffer. */
struct bbuf *bcache_get(block_device_t *dev, uint64_t blk);

/** Let a held buffer go and wake whoever waits for it. */
void bcache_release(struct bbuf *b);

/** Mark a held buffer for writeback. Milestone 37 is the first caller. */
void bcache_dirty(struct bbuf *b);

/** Write `dev`'s dirty buffers in ascending block order; 0, or the first
 * write's -errno with that buffer and the rest still dirty for the next
 * call. */
int bcache_sync(block_device_t *dev);

/** Drop `dev`'s buffers at unmount; -EBUSY while any is held or dirty. */
int bcache_invalidate(block_device_t *dev);

/* Counters for /proc/disk: reads served from the cache, reads from the
 * device, and waits for a busy buffer. */
uint64_t bcache_stat_hits(void);
uint64_t bcache_stat_fills(void);
uint64_t bcache_stat_waits(void);

#endif
