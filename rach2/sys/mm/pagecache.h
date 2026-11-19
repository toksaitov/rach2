/*
 * The page cache: a window of kernel addresses over a backing store, the
 * KRFFS root on a disk, whose pages are read in when first touched. A fault
 * in the window is served by polling the backing page into a frame of the
 * cache and mapping it; dirty frames are written back on eviction and on
 * sync. The fault path polls and never sleeps, so it runs with interrupts
 * off.
 */
#ifndef _PAGECACHE_H_
#define _PAGECACHE_H_

#include "../arch/arch.h"
#include "../lib/kint.h"

/* Base of the demand-paged window, the architecture's KVA_PCWIN. */
#define PCWIN_BASE KVA_PCWIN

/* Policy: the most resident pages, in PAGE_SIZE frames, and a power of two,
 * since the lookup hash is sized at twice this. A page's frame is taken
 * when the page is first read and given back under memory pressure
 * (`pc_shrink`), so the cache holds at most 16 MiB and often less. A compile
 * on a KRFFS disk root needs its hot set resident: the compiler, the headers
 * and the objects, and the header page of every node in the ring, since a
 * lookup walks the ring. Once the kernel's own build has added its nodes to
 * os-real's root, the ring's header pages alone outnumber 2048 slots, and
 * every lookup reads the disk again; 4096 holds them beside the hot set
 * with room for the ring to grow (tests/host/pagecache_ring_hosttest.c). */
#ifndef PC_SLOTS
#define PC_SLOTS 4096u
#endif

/* The most pages one writeback puts in one write of the backing: the page
 * and the dirty pages beside it on the backing, so that a disk that flushes
 * its cache after each write (ata.c) flushes once for the run. 32 pages are
 * 128 KiB, a quarter of the most one ATA or AHCI write takes. */
#define PC_RUN_PAGES 32u

/*
 * The backing store. `disk_off` is a page-aligned byte offset into it. The
 * read reads one page; the write writes `count` pages, 1 to PC_RUN_PAGES,
 * that follow each other from `disk_off` and lie in `pages` one after
 * another, and is on the medium when it returns 0, as one write of the
 * block device is. Both return 0 on success or a negative errno; a write
 * that fails promises nothing of any of its pages.
 */
typedef int (*pc_read_fn)(uint32_t disk_off, void *page);
typedef int (*pc_write_fn)(uint32_t disk_off, const void *pages, uint32_t count);

/**
 * Size the window against the lowest device memory BAR inside it, make its
 * page tables in the kernel's directory, and set the cache's capacity to
 * half the caches' target (`mem_cache_target`); no frame is taken here. The
 * boot calls it once, after `init_paging` and before the first fork, and it
 * panics when the tables cannot be had. The cache serves no fault until
 * `pc_set_backing` names a backing store.
 */
void init_pagecache(void);

/**
 * Give back up to `want` frames: those of slots that hold no page and clean
 * resident pages first, whatever their age, then, with `can_write`, dirty
 * ones written back through the backing, which may sleep. A pinned slot, a
 * fill or a write in flight, is passed, and so is a page whose writeback
 * fails. Returns how many came back; `*written` gains the writebacks
 * tried.
 */
uint32_t pc_shrink(uint32_t want, int can_write, uint32_t *written);

/** Frames the cache holds now. */
uint32_t pc_resident(void);

/**
 * Point the cache at a backing store of `fs_size` bytes and start it again
 * empty. This is a remount: every page a previous backing had is dropped
 * without a writeback, its frame kept by its slot, so a caller whose
 * previous backing had dirty pages calls `pc_sync` first. NULL functions and
 * a size of 0 take the backing away. `fs_size` is clamped to
 * `pc_window_size()` and rounded up to a whole page; a window fault at or
 * past that bound is a wild pointer the fault handler panics on. A failure
 * held for `pc_on_failure`, a previous backing's, is dropped.
 */
void pc_set_backing(pc_read_fn read, pc_write_fn write, uint32_t fs_size);

/**
 * Have `failed` called with what the backing's write answered for each
 * write of a page that fails, whoever wrote it, a sync, the flusher or an
 * eviction, so that the file system the window holds records the failure
 * for its syncs to report: the page stays dirty, and a later write of it
 * that succeeds would otherwise hide the failure. `failed` may be called
 * with interrupts off, from a page fault; NULL calls nothing. A write that
 * fails before a hook is installed, such as the boot's write of the root's
 * orphan repair before the root is mounted, is held, the last one's answer,
 * and `failed` is called with it here, so that the root's syncs report it.
 */
void pc_on_failure(void (*failed)(int rc));

/**
 * Serve a kernel fault at `fault_address` in the window: the page is read in
 * when it is not resident, and mapped read-only for a read, or writable and
 * marked dirty for a `write`. Returns 1 when the page is mapped; 0, on which
 * the fault handler panics, with no backing, for an address past the backed
 * size (a wild kernel pointer), when no slot can be had, or when the read
 * fails. The page fault handler calls it with interrupts off. A file system
 * that must survive a failed read copies through `pc_copy_out` and
 * `pc_copy_in` instead, which bring pages in the same way.
 */
int pc_fault(uintptr_t fault_address, int write);

/**
 * Copy `size` bytes at byte `off` of the backing into `buf` through the
 * cache, reading in each page that is not resident, as a read through the
 * window would but with the read's failure returned: 0, or -1 with no
 * backing, for a range past the backed size, or when a page cannot be read
 * or given a slot, which leaves `buf` filled up to that page. A file system
 * that must answer a device error, and not panic on it, reads this way. Each
 * page is brought in and copied with interrupts off and the big lock held,
 * as a window fault is served; the caller may hold the big lock, and no
 * spinlock.
 */
int pc_copy_out(uint32_t off, void *buf, uint32_t size);

/**
 * Copy `buf` into `size` bytes at byte `off` of the backing through the
 * cache, as `pc_copy_out` reads them, marking each page dirty, as a store
 * through the window would: 0, or -1 as `pc_copy_out` answers. A range
 * that touches at most two pages is stored whole or not at all, since
 * every page it touches is brought in before a byte is copied; a longer
 * one goes in such pieces, and a failure leaves the pieces before it
 * written. A page under writeback takes the bytes and stays dirty for a
 * later write, as a store into it does.
 */
int pc_copy_in(uint32_t off, const void *buf, uint32_t size);

#define PC_SYNC_BUSY   1
#define PC_SYNC_FAILED 2

/**
 * Write the dirty resident pages to the backing store, for sync and fsync.
 *
 * Each page is pinned and mapped read-only before its write, so a store
 * meanwhile marks it dirty again for a later write. Returns 0, or the OR of
 * PC_SYNC_BUSY for pinned slots passed and PC_SYNC_FAILED for failed
 * writes, whose pages stay dirty. Both bits can be set, so a failure cannot
 * hide a busy slot from a caller that waits for the writes in flight. A
 * pinned slot passed is marked, so that its release is counted
 * (`pc_releases`) and wakes the syncs that wait for it. The backing's
 * writes may sleep.
 */
int pc_sync(void);

/**
 * Write the dirty resident pages back as `pc_sync` does, and wait for the
 * writes in flight on pinned slots, until a pass meets none: each pass that
 * meets one sleeps until a pin a sync passed is let go (`pc_wait_release`).
 * Returns 0, or PC_SYNC_FAILED when a write failed in any pass, even when a
 * later pass wrote the page, so that a caller with no error cursor of its
 * own, such as the fsync of a descriptor with no mount, reports it.
 *
 * A pinned slot is another caller's write, the flusher's or reclaim's, which
 * sleeps in the disk driver, or a fill: a sync that must say the pages are on
 * the disk, fsync above all, waits for it with no deadline, as Linux's fsync
 * waits for the pages under write-back (`filemap_write_and_wait_range`,
 * mm/filemap.c:667 in 6.12) and fails only on a write that failed. The
 * caller is a process and holds the big lock; it may sleep. It is defined
 * beside sync(2) in syscall_fs.c, which uses it too, since this file builds
 * for the host tests without a scheduler.
 */
int pc_sync_wait(void);

/** The count of releases of pins that `pc_sync` passed, which wraps: the
 * value `pc_wait_release` waits to see move. */
uint32_t pc_releases(void);

/**
 * Sleep until `pc_releases` differs from `seen`, read before the pass of
 * `pc_sync` that met a pinned slot, so that a release during that pass ends
 * the wait at once. The sleep is uninterruptible, as Linux's wait for a page
 * under write-back (`folio_wait_writeback`), and has no deadline; the
 * caller is a process. The host tests that reach it supply their own.
 */
void pc_wait_release(uint32_t seen);

/** The window's span in bytes, which its page tables cover. */
uint32_t pc_window_size(void);

/**
 * Cap the live cache to `slots` frames, clamped to 1 to PC_SLOTS, for the
 * host tests' eviction checks: each resident page at or above the new
 * capacity is written back and dropped. Returns 0, or -1 if a page's
 * writeback failed: that page is left dirty and the capacity is held high
 * enough to keep it reclaimable, so a later sync still writes it. The tests
 * call it on a cache with no window fault in flight, so taking those pages
 * races nothing.
 */
int pc_set_capacity(uint32_t slots);

/**
 * Where the dirty pages stand against Linux's thresholds, taken of the live
 * capacity: 0 below the background threshold (`dirty_background_ratio` per
 * cent, or `dirty_background_bytes` in pages when set), 1 at or past it,
 * and 2 at or past the dirty limit (`vm_dirty_ratio` per cent, or
 * `vm_dirty_bytes`). A background threshold not below the limit is half the
 * limit. A page that goes dirty at or past the threshold wakes the flusher
 * (`pc_set_flusher`), which writes back until the count is under it
 * (`pc_write_background`), and past the limit a writer writes back itself
 * (`pc_balance_dirty`), as Linux's background writeback and
 * `balance_dirty_pages` do, so that the clock, which evicts a clean page
 * before a dirty one, finds clean pages and a fault seldom writes back with
 * interrupts off.
 */
int pc_dirty_level(void);

/** The resident pages marked dirty now. */
uint32_t pc_dirty_count(void);

/** Name the call that wakes the flusher when a page goes dirty at or past
 * the background threshold; it is made from a fault, with interrupts off,
 * and must not sleep. NULL calls nothing. */
void pc_set_flusher(void (*kick)(void));

/**
 * Write dirty pages back, a run at a time (`PC_RUN_PAGES`), until the count
 * is under the background threshold or no page is left to try: a page
 * pinned by a write in flight is passed, and so is one whose run failed in
 * this call, which stays dirty and is told to the failure hook. The writes
 * may sleep. Returns the pages written.
 */
uint32_t pc_write_background(void);

/**
 * A writer's throttle: at or past the dirty limit, write runs back until the
 * dirty pages are under the midpoint of the background threshold and the
 * limit, as Linux's `balance_dirty_pages` holds a writer; below the limit,
 * nothing. A file system calls it after each change it makes through the
 * window, from a system call, where the writes may sleep. Returns the pages
 * written.
 */
uint32_t pc_balance_dirty(void);

/** The pages read in since `init_pagecache`, for the host tests. */
uint32_t pc_stat_reads(void);

/** The pages evicted since `init_pagecache`, for the host tests. */
uint32_t pc_stat_evictions(void);

/** The pages that writebacks which succeeded wrote since `init_pagecache`,
 * for the host tests. */
uint32_t pc_stat_writebacks(void);

#endif
