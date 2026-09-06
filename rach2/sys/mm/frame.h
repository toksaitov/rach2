#ifndef _FRAME_H_
#define _FRAME_H_

#include "../lib/kdef.h"
#include "../lib/kint.h"

/*
 * frame: physical memory as the firmware's map describes it.
 *
 * Boot starts with every byte unavailable, adds the ranges the Multiboot map
 * calls available, takes away every range of any other type where it
 * overlaps one of them, and then takes away what the boot itself occupies:
 * the kernel image and the boot data below it, the modules, and the tables
 * the allocator carves. The arithmetic is in 64 bits, so a range that ends
 * at 4 GiB is right. What is left is what the allocator manages, frame by
 * frame, each named by its number.
 *
 * The four numbers the boot reports come from here and from nothing else:
 * usable is the available ranges below 4 GiB, which is what this kernel can
 * address; unreachable is the available ranges at or above it, which it
 * cannot; reserved is what the boot occupies of the usable ranges; managed
 * is usable less reserved. The map gives no installed figure, and none is
 * invented from it.
 *
 * Pure: no kernel service is called, so tests/host/frame_hosttest.c drives
 * it with maps no firmware offers on demand.
 */

#define FRAME_PAGE_SIZE        0x1000u
/* The identity-mapped window: mirrors HARD_MEMORY_LIMIT in mm/paging.h, and
 * init_paging checks that they agree. A frame above it is reached through a
 * mapping in the kernel's own layout, never through its physical address. */
#define FRAME_DIRECT_MAP_LIMIT 0x2000000u
/* How many disjoint usable ranges a map may leave after the subtractions.
 * A format bound: firmware maps have a few dozen entries, and a map that
 * needs more than this is refused by name rather than read in part. */
#define FRAME_MAP_RANGES       32

/** One half-open range of physical addresses, in bytes. */
struct frame_range {
    uint64_t start;
    uint64_t end;
};

/** The map after the subtractions, and the four numbers. */
struct frame_map {
    struct frame_range usable[FRAME_MAP_RANGES]; /* sorted, disjoint, below 4 GiB */
    uint32_t count;
    uint64_t usable_bytes;      /* available below 4 GiB, other types subtracted */
    uint64_t unreachable_bytes; /* available at or above 4 GiB */
    uint64_t reserved_bytes;    /* what `frame_map_reserve` took out */
    uint32_t top_frame;         /* the first frame number past the highest usable byte */
    int from_mem_upper;         /* no map: the ranges came from `mem_upper` */
};

/**
 * Build `m` from a Multiboot memory map of `mmap_len` bytes at `mmap_buf`,
 * or, when the map is absent (`mmap_buf` NULL or nothing available in it),
 * from `mem_upper_kb`, the kilobytes above 1 MiB the older field reports,
 * as one range from 1 MiB.
 *
 * Every entry is read as the map's format says: the `size` field gives the
 * bytes that follow it. The walk stops at a short or non-advancing entry.
 * Returns 0, or -1 when the ranges do not fit `FRAME_MAP_RANGES`, in which
 * case `m` holds nothing usable.
 */
int frame_map_build(struct frame_map *m, const void *mmap_buf, uint32_t mmap_len,
                    uint32_t mem_upper_kb);

/**
 * Take `[start, end)` out of the usable ranges, counting the part that was
 * usable as reserved. Taking out a range already reserved counts nothing
 * twice. Returns 0, or -1 when the split would need more ranges than the
 * map has room for.
 */
int frame_map_reserve(struct frame_map *m, uint64_t start, uint64_t end);

/** Whether frame number `frame` lies in a usable range after the
 * reservations. */
int frame_map_usable(const struct frame_map *m, uint32_t frame);

/** Usable less reserved: what the allocator manages, in bytes. */
uint64_t frame_map_managed(const struct frame_map *m);

/**
 * The `index`-th entry of a Multiboot memory map, for the boot's record of
 * the map as the firmware gave it. Returns 1 with the entry's fields, or 0
 * past the last entry or at a malformed one.
 */
int frame_map_entry(const void *mmap_buf, uint32_t mmap_len, uint32_t index, uint64_t *addr,
                    uint64_t *len, uint32_t *type);

#endif /* _FRAME_H_ */
