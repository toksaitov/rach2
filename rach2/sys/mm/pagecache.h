#ifndef _PAGECACHE_H_
#define _PAGECACHE_H_

#include "../lib/kint.h"

/*
 * The page cache lifts KRFFS off the RAM-disk module onto a real disk by
 * demand-mapping a fixed kernel-virtual window over a backing store. A fault
 * in the window is serviced by polling the backing page into a reserved
 * cache frame and mapping it; dirty frames are written back on eviction and
 * on fsync. The fault path polls and never sleeps, so it runs with
 * interrupts off.
 */

/* Base of the demand-paged window. It sat at USER_ADDR_MAX (0xC0000000) until
   the first real machine: its firmware places device MMIO from 0xC0500000,
   and since the window is sized down to the lowest BAR above its base, a
   window starting at 0xC0000000 was five megabytes long there, smaller than
   any disk worth mounting. So the window starts a quarter of the way up
   instead. User space still ends at 0xC0000000, and the 0xC0000000-0xD0000000
   gap is exactly where that firmware's identity-mapped MMIO lives. */
#define PCWIN_BASE 0xD0000000u

/* Policy: the most resident pages, in PAGE_SIZE frames, and a power of two,
   since the lookup hash is sized at twice this. A page's frame is taken
   when the page is first read and given back under memory pressure
   (`pc_shrink`), so the cache holds at most 8 MiB and often less. A compile
   on a KRFFS disk root needs its hot set resident: the compiler, the headers
   and the objects, and the header page of every node in the ring, since a
   lookup walks the ring. At 256 slots every compile re-read most of that
   from the disk, at more than a minute per file; at 1024 the self-build
   suite ran at speed until the manual's pages put the ring's header pages
   past the slots, and every lookup then evicted the hot set, at five times
   the suite's time. 2048 holds both with room for the ring to grow. */
#ifndef PC_SLOTS
#define PC_SLOTS 2048u
#endif

/*
 * The backing store, accessed one page at a time. `disk_off` is a page-aligned
 * byte offset into it. Both return 0 on success or a negative errno.
 */
typedef int (*pc_read_fn)(uint32_t disk_off, void *page);
typedef int (*pc_write_fn)(uint32_t disk_off, const void *page);

/*
 * Pre-map the window's page tables in the kernel directory and size the
 * window against the lowest device MMIO BAR; no frame is taken here. Call
 * once after init_paging and before the first fork. The cache serves no
 * fault until pc_set_backing names a backing store.
 */
void init_pagecache(void);

/**
 * Give back up to `want` frames: clean resident pages first, then, with
 * `can_write`, dirty ones written back through the backing, which may
 * sleep. Skip pinned slots, including writes in flight. Returns how many
 * came back; `*written` counts the writebacks.
 */
uint32_t pc_shrink(uint32_t want, int can_write, uint32_t *written);

/** Frames the cache holds now. */
uint32_t pc_resident(void);

/*
 * Point the cache at a backing store of `fs_size` bytes and (re)start it
 * empty. This is a remount: it DROPS any frames a previous backing held
 * WITHOUT writing them back, so the caller must pc_sync() first if a
 * previous backing held dirty pages. `fs_size` is rounded up to a whole page
 * and clamped to pc_window_size(); a window fault at or past that bound is a
 * wild pointer the fault handler panics on.
 */
void pc_set_backing(pc_read_fn read, pc_write_fn write, uint32_t fs_size);

/*
 * Service a fault at `fault_address` in the window. Returns 1 if handled
 * (the page is now mapped, read-only on a read or read-write on a write), or
 * 0 if the address is past the backed size (a wild kernel pointer the caller
 * must panic on). `write` requests write permission and marks the page
 * dirty.
 */
int pc_fault(uint32_t fault_address, int write);

#define PC_SYNC_BUSY   1
#define PC_SYNC_FAILED 2

/**
 * Write dirty cache frames to the backing store (fsync and sync).
 *
 * Reserve each slot and frame and re-protect it read-only before writing.
 * Concurrent stores mark it dirty again for a later write. Return 0, or
 * the OR of PC_SYNC_BUSY for skipped busy slots and PC_SYNC_FAILED for
 * failed writes. Both bits can be set, so a failure cannot hide a busy
 * slot from a caller waiting for writeback. Failed pages stay dirty.
 */
int pc_sync(void);

/* The pre-mapped window span in bytes (the page-table coverage in the
 * window). */
uint32_t pc_window_size(void);

/*
 * Cap the live cache to `slots` frames (clamped to 1..PC_SLOTS) for the
 * eviction self-test, writing back then dropping each resident frame at or
 * above the new capacity. Returns 0, or -1 if a frame's writeback failed:
 * that frame is left dirty and the capacity is held high enough to keep it
 * reclaimable, so a later sync still flushes it. The test calls it on a
 * quiescent cache, with no window fault in flight, so reclaiming those
 * frames races nothing.
 */
int pc_set_capacity(uint32_t slots);

/* Cumulative counters for the boot self-test's VMPROBE line. */
uint32_t pc_stat_reads(void);
uint32_t pc_stat_evictions(void);
uint32_t pc_stat_writebacks(void);

#endif
