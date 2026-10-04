#ifndef _FRAME_H_
#define _FRAME_H_

#include "../boot.h"
#include "../lib/kdef.h"
#include "../lib/kint.h"

/*
 * frame: physical memory as the firmware's map describes it.
 *
 * Boot starts with every byte unavailable, adds the ranges the boot record's
 * map (boot.h) calls usable, takes away every range of any other type where
 * it overlaps one of them, and then takes away what the boot itself
 * occupies: the kernel image and the boot data below it, the modules, and
 * the tables the allocator carves. The arithmetic is in 64 bits, so a range
 * that ends at 4 GiB is right. What is left is what the allocator manages,
 * frame by frame, each named by its number.
 *
 * The kernel addresses physical memory below a bound its architecture
 * gives, the reach (`mmu_phys_reach`): 4 GiB on i686, whose physical
 * addresses are 32 bits, and on x86-64 the direct map's 64 TiB, or 64 GiB
 * on a processor without 1 GiB pages. The four numbers the boot reports
 * come from here and from nothing else: usable is the available ranges
 * below the reach; unreachable is the available ranges at or above it;
 * reserved is what the boot occupies of the usable ranges; managed is
 * usable less reserved. The map gives no
 * installed figure, and none is invented from it.
 *
 * Pure: no kernel service is called, so tests/host/frame_hosttest.c drives
 * it with maps no firmware offers on demand.
 */

#define FRAME_PAGE_SIZE        0x1000u
/* The identity-mapped window: mirrors HARD_MEMORY_LIMIT in mm/paging.h, and
 * init_paging checks that they agree. A frame above it is reached through a
 * mapping in the kernel's own layout, never through its physical address. */
#define FRAME_DIRECT_MAP_LIMIT 0x2000000u
/* The reach of a 32-bit physical address. */
#define FRAME_REACH_32         0x100000000ULL
/* The type of a usable range in the boot record's map (boot.h). */
#define FRAME_MAP_USABLE       1u
/*
 * How many disjoint usable ranges the map keeps. Built from a map of n
 * entries, the list has at most n ranges: a usable entry adds at most one,
 * and an entry of another type splits at most one in two. The record keeps
 * BOOT_MMAP_MAX entries, enough for a UEFI map, which lists the firmware's
 * boot-time and runtime regions one by one and runs to a hundred entries
 * where a BIOS map has a score. Each reservation after the build splits at
 * most one range more: a module, of which the record keeps BOOT_MODULE_MAX,
 * and the frame table; the kernel image and the allocator's break start at
 * 0 and only trim. A map that still needs more, which no record can give,
 * is refused by name rather than read in part.
 */
#define FRAME_MAP_RANGES       (BOOT_MMAP_MAX + BOOT_MODULE_MAX + 1)

/** One half-open range of physical addresses, in bytes. */
struct frame_range {
    uint64_t start;
    uint64_t end;
};

/** The map after the subtractions, and the four numbers. */
struct frame_map {
    struct frame_range usable[FRAME_MAP_RANGES]; /* sorted, disjoint, below the reach */
    uint32_t count;
    uint64_t usable_bytes;      /* available below the reach, other types subtracted */
    uint64_t unreachable_bytes; /* available at or above the reach */
    uint64_t reserved_bytes;    /* what `frame_map_reserve` took out */
    uint64_t top_frame;         /* the first frame number past the highest usable byte */
    int from_mem_upper;         /* no map: the ranges came from `mem_upper` */
};

/**
 * Build `m` from a memory map of `mmap_len` bytes at `mmap_buf`, in the
 * boot record's form, or, when the map is absent (`mmap_buf` NULL or
 * nothing available in it), from `mem_upper_kb`, the kilobytes above 1 MiB
 * the older field reports, as one range from 1 MiB. What lies at or above
 * `reach` is counted as unreachable and kept out of the ranges.
 *
 * Every entry is read as the map's format says: the `size` field gives the
 * bytes that follow it. The walk stops at a short or non-advancing entry.
 * Returns 0, or -1 when the ranges do not fit `FRAME_MAP_RANGES`, in which
 * case `m` holds nothing usable.
 */
int frame_map_build(struct frame_map *m, const void *mmap_buf, uint32_t mmap_len,
                    uint32_t mem_upper_kb, uint64_t reach);

/**
 * Take `[start, end)` out of the usable ranges, counting the part that was
 * usable as reserved. Taking out a range already reserved counts nothing
 * twice. Returns 0, or -1 when the split would need more ranges than the
 * map has room for.
 */
int frame_map_reserve(struct frame_map *m, uint64_t start, uint64_t end);

/** Whether frame number `frame` lies in a usable range after the
 * reservations. */
int frame_map_usable(const struct frame_map *m, uint64_t frame);

/** Usable less reserved: what the allocator manages, in bytes. */
uint64_t frame_map_managed(const struct frame_map *m);

/**
 * Find room for `bytes`, rounded up to whole frames, in a usable range at
 * or above `floor`: the highest page-aligned place that holds it, so that
 * a large table goes to the top of memory and leaves the low memory to what
 * needs it. Sets `*at` and returns 0, or returns -1 when no range holds it.
 * Nothing is reserved; the caller reserves what it takes.
 */
int frame_map_place(const struct frame_map *m, uint64_t bytes, uint64_t floor, uint64_t *at);

/**
 * The `index`-th entry of a memory map in the boot record's form, for the
 * boot's record of the map as the firmware gave it. Returns 1 with the
 * entry's fields, or 0 past the last entry or at a malformed one.
 */
int frame_map_entry(const void *mmap_buf, uint32_t mmap_len, uint32_t index, uint64_t *addr,
                    uint64_t *len, uint32_t *type);

#endif /* _FRAME_H_ */
