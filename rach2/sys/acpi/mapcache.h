/*
 * mapcache.h: the windows of physical memory ACPICA asks the kernel to map,
 * each made once and kept, since the kernel's MMIO mappings have no unmap.
 */
#ifndef _ACPI_MAPCACHE_H_
#define _ACPI_MAPCACHE_H_

#include "../lib/kint.h"

/* A window: the whole pages [phys, phys + len) mapped at `va`. */
struct acpi_window {
    uint64_t phys;
    uint64_t len;
    uintptr_t va;
};

/*
 * Map the whole pages [phys, phys + len), page-aligned both, and return
 * their address, or 0 when they cannot be mapped. The kernel's maps through
 * `map_mmio`; a host test hands out a model.
 */
typedef uintptr_t (*acpi_window_map_fn)(uint64_t phys, uint64_t len);

/* The windows made so far, in an array of `cap` the owner gives. */
struct acpi_mapcache {
    struct acpi_window *windows;
    int cap;
    int n;
    uint64_t limit; /* the first physical address past the reach, 0 for none */
    int full;       /* a request was refused because every window is made */
    acpi_window_map_fn map;
};

/**
 * The address of the `len` bytes at `phys`: inside a window already made when
 * one holds them all, so that a table longer than a page is contiguous and a
 * register read a thousand times is mapped once; otherwise through a new
 * window of the whole pages they touch. 0 for no bytes, a range that wraps
 * or passes `limit`, a map that fails, or a new window when all `cap` are
 * made, which also sets `full`. The caller excludes other callers.
 */
uintptr_t acpi_mapcache_get(struct acpi_mapcache *c, uint64_t phys, uint64_t len);

#endif /* _ACPI_MAPCACHE_H_ */
