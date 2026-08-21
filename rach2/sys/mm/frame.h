#ifndef _FRAME_H_
#define _FRAME_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"
#include "../../vendor/multiboot.h"

/*
 * Physical frame pool sizing. The kernel manages RAM in PAGE_SIZE frames via a
 * bitmap (the bitmap itself lives in mm/paging.c). This module sizes that pool:
 * it reads the multiboot memory map to find how much RAM actually exists, then
 * caps the pool to the direct-mapped window. The kernel only identity-maps the
 * low FRAME_DIRECT_MAP_LIMIT bytes, and the ELF loader and fork write user
 * frames through their physical (identity) address, so a frame the kernel
 * cannot directly touch must never be handed out. Total resident memory is
 * bounded by that window.
 *
 * Linux/BSD analogue: the boot-time physical page allocator sized from the
 * firmware/e820 memory map (Linux memblock then the buddy allocator).
 *
 * These two constants mirror HARD_MEMORY_LIMIT and PAGE_SIZE in mm/paging.h;
 * they are kept local so this file stays pure and host-linkable, and
 * init_paging asserts they still agree at boot.
 */
#define FRAME_PAGE_SIZE        0x1000u
#define FRAME_DIRECT_MAP_LIMIT 0x2000000u  /* 32 MB; == HARD_MEMORY_LIMIT */

/** Highest available physical address in a multiboot memory map, or 0. */
uint64_t frame_top_available(const void *mmap_buf, uint32_t mmap_len);

/** Number of PAGE_SIZE frames to manage, capped to the direct-mapped window. */
size_t frame_usable_count(const multiboot_info_t *info);

#endif /* _FRAME_H_ */
