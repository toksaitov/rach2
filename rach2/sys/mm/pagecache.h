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
 * (`pc_shrink`), so the cache holds at most 8 MiB and often less. A compile
 * on a KRFFS disk root needs its hot set resident: the compiler, the headers
 * and the objects, and the header page of every node in the ring, since a
 * lookup walks the ring. With 1024 slots the ring's header pages of a root
 * with the manual staged do not fit beside the hot set, and every lookup
 * reads the disk again; 2048 holds both with room for the ring to grow
 * (tests/host/pagecache_ring_hosttest.c). */
#ifndef PC_SLOTS
#define PC_SLOTS 2048u
#endif

/*
 * The backing store, accessed one page at a time. `disk_off` is a page-aligned
 * byte offset into it. Both return 0 on success or a negative errno.
 */
typedef int (*pc_read_fn)(uint32_t disk_off, void *page);
typedef int (*pc_write_fn)(uint32_t disk_off, const void *page);

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
 * past that bound is a wild pointer the fault handler panics on.
 */
void pc_set_backing(pc_read_fn read, pc_write_fn write, uint32_t fs_size);

/**
 * Serve a kernel fault at `fault_address` in the window: the page is read in
 * when it is not resident, and mapped read-only for a read, or writable and
 * marked dirty for a `write`. Returns 1 when the page is mapped; 0, on which
 * the fault handler panics, with no backing, for an address past the backed
 * size (a wild kernel pointer), when no slot can be had, or when the read
 * fails. The page fault handler calls it with interrupts off.
 */
int pc_fault(uintptr_t fault_address, int write);

#define PC_SYNC_BUSY   1
#define PC_SYNC_FAILED 2

/**
 * Write the dirty resident pages to the backing store, for sync and fsync.
 *
 * Each page is pinned and mapped read-only before its write, so a store
 * meanwhile marks it dirty again for a later write. Returns 0, or the OR of
 * PC_SYNC_BUSY for pinned slots passed and PC_SYNC_FAILED for failed
 * writes, whose pages stay dirty. Both bits can be set, so a failure cannot
 * hide a busy slot from a caller that waits for the writes in flight. The
 * backing's writes may sleep.
 */
int pc_sync(void);

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

/** The pages read in since `init_pagecache`, for the host tests. */
uint32_t pc_stat_reads(void);

/** The pages evicted since `init_pagecache`, for the host tests. */
uint32_t pc_stat_evictions(void);

/** The writebacks that succeeded since `init_pagecache`, for the host
 * tests. */
uint32_t pc_stat_writebacks(void);

#endif
