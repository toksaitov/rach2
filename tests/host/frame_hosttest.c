/**
 * Dual-target seam test for the physical-frame-pool sizing (mm/frame.c).
 *
 * host-deps: rach2/sys/mm/frame.c
 *
 * The fallible part is frame_top_available()'s walk of a multiboot memory map
 * (the "next = entry + entry->size + 4" arithmetic and bounds checks), so a
 * crafted map is fed to it natively under ASan/UBSan: it must return the top
 * of the highest AVAILABLE region, ignore reserved regions, and not read past
 * the buffer on a short/garbage entry. Returns the failed-check count
 * (0 == pass).
 */

#include "mm/frame.h"

static int fails;

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/**
 * Build one multiboot mmap entry into buf at offset, return bytes written (24).
 *
 * 'size' is the standard 20 (entry size excluding the size field itself).
 */
static uint32_t put_entry(uint8_t *buf, uint32_t off, uint64_t addr, uint64_t len, uint32_t type)
{
    multiboot_memory_map_t e;
    uint32_t i;

    e.size = 20;
    e.addr = addr;
    e.len = len;
    e.type = type;
    for (i = 0; i < sizeof(e); i++) {
        buf[off + i] = ((const uint8_t *)&e)[i];
    }
    return (uint32_t)sizeof(e);
}

int main(void)
{
    uint8_t map[256];
    uint32_t n = 0;

    /* Typical PC layout: low 640 KB available, BIOS reserved, then a big
     * available region from 1 MB to 128 MB. */
    n += put_entry(map, n, 0x0, 0x9FC00, MULTIBOOT_MEMORY_AVAILABLE);
    n += put_entry(map, n, 0x9FC00, 0x60400, MULTIBOOT_MEMORY_RESERVED);
    n += put_entry(map, n, 0x100000, 0x7F00000, MULTIBOOT_MEMORY_AVAILABLE);

    /* Top of available RAM = 0x100000 + 0x7F00000 = 0x8000000 (128 MB). */
    CHECK(frame_top_available(map, n) == 0x8000000ULL);

    /* A reserved region above does not raise the top. */
    {
        uint32_t m = n;
        m += put_entry(map, m, 0x8000000, 0x1000000, MULTIBOOT_MEMORY_RESERVED);
        CHECK(frame_top_available(map, m) == 0x8000000ULL);
    }

    /* NULL / empty buffer -> 0. */
    CHECK(frame_top_available(0, 0) == 0);
    CHECK(frame_top_available(map, 0) == 0);

    /* A truncated final entry (only the size field fits) is not over-read. */
    CHECK(frame_top_available(map, n + 4) == 0x8000000ULL);

    /* A single small available region below the 32 MB window. */
    {
        uint8_t m2[64];
        uint32_t k = 0;
        k += put_entry(m2, k, 0x100000, 0x300000, MULTIBOOT_MEMORY_AVAILABLE);
        CHECK(frame_top_available(m2, k) == 0x400000ULL); /* 4 MB top */
    }

    return (fails > 50) ? 50 : fails;
}
