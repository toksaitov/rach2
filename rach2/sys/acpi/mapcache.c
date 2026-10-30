/* mapcache.c: the windows of physical memory ACPICA maps, as mapcache.h
 * describes. */

#include "mapcache.h"

#define PAGE_MASK_64 ((uint64_t)0xFFFu)

uintptr_t acpi_mapcache_get(struct acpi_mapcache *c, uint64_t phys, uint64_t len)
{
    uint64_t start, end;
    uintptr_t va;
    int i;

    if (len == 0 || phys + len < phys || phys + len > ~PAGE_MASK_64) {
        return 0;
    }
    if (c->limit != 0 && phys + len > c->limit) {
        return 0;
    }
    for (i = 0; i < c->n; i++) {
        const struct acpi_window *w = &c->windows[i];

        if (phys >= w->phys && phys + len <= w->phys + w->len) {
            return w->va + (uintptr_t)(phys - w->phys);
        }
    }
    if (c->n == c->cap) {
        c->full = 1;
        return 0;
    }
    start = phys & ~PAGE_MASK_64;
    end = (phys + len + PAGE_MASK_64) & ~PAGE_MASK_64;
    va = c->map(start, end - start);
    if (va == 0) {
        return 0;
    }
    c->windows[c->n].phys = start;
    c->windows[c->n].len = end - start;
    c->windows[c->n].va = va;
    c->n++;
    return va + (uintptr_t)(phys - start);
}
