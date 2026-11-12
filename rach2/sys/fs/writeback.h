#ifndef _WRITEBACK_H_
#define _WRITEBACK_H_

/*
 * writeback: the flusher, a kernel thread that writes dirty data back to the
 * disks on Linux's policy (mm/page-writeback.c, fs/fs-writeback.c). Every
 * `dirty_writeback_centisecs` it writes back, without the disk's flush, what
 * has been dirty for `dirty_expire_centisecs` or longer: each file system's
 * own pending writes (`vfs_ops.writeback`), then the buffer cache's dirty
 * buffers (`bcache_writeback`); and once in each expiry period the pages of
 * shared mappings and of the disk-backed window, which keep no age. When a
 * dirty mark leaves the buffer cache past its background threshold
 * (`bcache_dirty_level`), the thread is woken and writes the oldest buffers
 * back until the cache is under it again.
 *
 * Durability is the syncs': `fsync`, `fdatasync`, `sync`, `syncfs`, `msync`
 * with MS_SYNC, O_SYNC and O_DSYNC writes, an unmount and a shutdown write
 * what they promise and flush the disk's cache, whether or not the flusher
 * has written any of it. A stop of the machine loses at most what was
 * dirtied in the last expiry period and the interval after it, as on Linux.
 */

/* /proc/sys/vm's dirty_expire_centisecs and dirty_writeback_centisecs, in
 * hundredths of a second, and dirtytime_expire_seconds, which Linux reads
 * for lazytime mounts and which nothing here does, since no mount keeps
 * times lazily. A writeback interval of 0 stops the periodic write-back;
 * the background threshold still wakes the thread. */
extern int dirty_expire_interval;
extern int dirty_writeback_interval;
extern int dirtytime_expire_interval;

/** Start the flusher; a failure to is logged and leaves the write-back to
 * eviction, to writers past the dirty limit and to the syncs. */
void writeback_init(void);

/** Wake the flusher for a look at the thresholds and the clock now, as
 * after a write of one of the settings. Does not sleep. */
void writeback_kick(void);

#endif
