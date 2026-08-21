#ifndef _PAGECACHE_H_
#define _PAGECACHE_H_

#include "../lib/kint.h"

/*
 * The page cache lifts KRFFS off the RAM-disk module onto a real disk by
 * demand-mapping a fixed kernel-virtual window over a backing store. A fault in
 * the window is serviced by polling the backing page into a reserved cache frame
 * and mapping it; dirty frames are written back on eviction and on fsync. The
 * fault path polls and never sleeps, so it runs with interrupts off.
 */

/* Base of the demand-paged window. It sat at USER_ADDR_MAX (0xC0000000) until
   the first real machine: its firmware places device MMIO from 0xC0500000,
   and since the window is sized down to the lowest BAR above its base, a
   window starting at 0xC0000000 was five megabytes long there, smaller than
   any disk worth mounting. So the window starts a quarter of the way up
   instead. User space still ends at 0xC0000000, and the 0xC0000000-0xD0000000
   gap is exactly where that firmware's identity-mapped MMIO lives. */
#define PCWIN_BASE 0xD0000000u

/* Resident cache capacity, in PAGE_SIZE frames. Must be a power of two: the
   lookup hash is sized at twice this. 1024 frames is a fixed 4 MiB
   reservation. It was 256 (1 MiB) until the machine became its own build
   host: a compile faults the 269 KiB compiler, the sources, the headers and
   the objects through this window, and under 1 MiB every compile re-read
   most of that working set from the disk, which measured at more than a
   minute per file. Four MiB holds the toolchain's hot set and leaves the
   32 MiB pool most of its memory. */
#ifndef PC_SLOTS
#define PC_SLOTS 1024u
#endif

/* The reservation's floor on a small-memory machine (see init_pagecache):
   256 frames, the size the cache had before it grew, so any machine that
   booted the old cache boots this one. */
#define PC_MIN_SLOTS 256u

/*
 * The backing store, accessed one page at a time. `disk_off` is a page-aligned
 * byte offset into it. Both return 0 on success or a negative errno.
 */
typedef int (*pc_read_fn)(uint32_t disk_off, void *page);
typedef int (*pc_write_fn)(uint32_t disk_off, const void *page);

/*
 * Reserve the cache frames, pre-map the window's page tables in the kernel
 * directory, and size the window against the lowest device MMIO BAR. Call once
 * after init_paging and before the first fork. The cache serves no fault until
 * pc_set_backing names a backing store.
 */
void init_pagecache(void);

/*
 * Point the cache at a backing store of `fs_size` bytes and (re)start it empty.
 * This is a remount: it DROPS any frames a previous backing held WITHOUT writing
 * them back, so the caller must pc_sync() first if a previous backing held dirty
 * pages. `fs_size` is rounded up to a whole page and clamped to pc_window_size();
 * a window fault at or past that bound is a wild pointer the fault handler panics
 * on.
 */
void pc_set_backing(pc_read_fn read, pc_write_fn write, uint32_t fs_size);

/* Whether a backing store is currently set (the window is a live file
 * system). */
int pc_has_backing(void);

/*
 * Service a fault at `fault_address` in the window. Returns 1 if handled (the
 * page is now mapped, read-only on a read or read-write on a write), or 0 if the
 * address is past the backed size (a wild kernel pointer the caller must panic
 * on). `write` requests write permission and marks the page dirty.
 */
int pc_fault(uint32_t fault_address, int write);

/*
 * Write back every dirty cache frame to the backing store and re-protect each
 * read-only (fsync and sync). Returns 0, or -1 if any writeback failed (those
 * pages stay dirty for a later retry).
 */
int pc_sync(void);

/* The pre-mapped window span in bytes (the page-table coverage in the
 * window). */
uint32_t pc_window_size(void);

/*
 * Cap the live cache to `slots` frames (clamped to 1..PC_SLOTS) for the eviction
 * self-test, writing back then dropping each resident frame at or above the new
 * capacity. Returns 0, or -1 if a frame's writeback failed: that frame is left
 * dirty and the capacity is held high enough to keep it reclaimable, so a later
 * sync still flushes it. The test calls it on a quiescent cache, with no window
 * fault in flight, so reclaiming those frames races nothing.
 */
int pc_set_capacity(uint32_t slots);

/* Cumulative counters for the boot self-test's VMPROBE line. */
uint32_t pc_stat_reads(void);
uint32_t pc_stat_evictions(void);
uint32_t pc_stat_writebacks(void);

#endif
