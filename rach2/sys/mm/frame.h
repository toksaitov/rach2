#ifndef _FRAME_H_
#define _FRAME_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"
#include "../../vendor/multiboot.h"

/*
 * Physical frame pool sizing. The kernel manages RAM in PAGE_SIZE frames via a
 * bitmap (the bitmap itself currently lives in mm/paging.c). This module sizes
 * that pool honestly: it reads the multiboot memory map to find how much RAM
 * actually exists, then CAPS the pool to the direct-mapped window -- the kernel
 * only identity-maps the low FRAME_DIRECT_MAP_LIMIT bytes, and the ELF loader
 * and fork write user frames through their physical (identity) address, so a
 * frame the kernel cannot directly touch must never be handed out. Total
 * resident memory is bounded by that window until a high-memory/kmap split
 * lands (a documented future effort). Phase 5 (COW) will add per-frame refcounts
 * here.
 *
 * Linux/BSD analogue: the boot-time physical page allocator sized from the
 * firmware/e820 memory map (Linux memblock -> the buddy allocator).
 *
 * These two constants MIRROR HARD_MEMORY_LIMIT and PAGE_SIZE in mm/paging.h;
 * they are kept local so this file stays pure and host-linkable, and init_paging
 * asserts they still agree at boot.
 */
#define FRAME_PAGE_SIZE        0x1000u
#define FRAME_DIRECT_MAP_LIMIT 0x2000000u  /* 32 MB; == HARD_MEMORY_LIMIT */

/*
 * Walk a multiboot memory-map buffer (mmap_addr/mmap_length) and return the
 * highest physical address (addr+len) of any AVAILABLE region, or 0 if none.
 * Pure: operates only on the passed buffer, so the host seam test drives it.
 */
uint64_t frame_top_available(const void *mmap_buf, uint32_t mmap_len);

/*
 * The number of PAGE_SIZE frames the kernel should manage: the lesser of the
 * top of available RAM and the direct-mapped window. Uses the multiboot memory
 * map when present, otherwise the mem_upper/mem_lower fields, otherwise the
 * full window.
 */
size_t frame_usable_count(const multiboot_info_t *info);

#endif /* _FRAME_H_ */
