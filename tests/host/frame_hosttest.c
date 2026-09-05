/**
 * Host test for the physical memory map (mm/frame.c).
 *
 * host-deps: rach2/sys/mm/frame.c
 *
 * The map is built from Multiboot memory maps crafted here, under ASan and
 * UBSan: QEMU's maps as this kernel recorded them at boot for 64 MiB,
 * 512 MiB, 2 GiB, 3 GiB and 4 GiB, since no record of the Lenovo's exists
 * yet, and synthetic maps with holes, an overlap between an available range
 * and a reserved one, a range crossing 4 GiB, an entry whose length wraps,
 * a truncated last entry, an empty map, and more ranges than the map keeps.
 * The reservations the boot makes are replayed against them and the four
 * numbers checked. Returns the failed-check count (0 == pass).
 */

#include "mm/frame.h"

#include "../../rach2/vendor/multiboot.h"

/* The host's printf, declared here: the kernel's headers define the
 * integer types the host's stdio.h would define again. */
int printf(const char *format, ...);

static int fails;

#define CHECK(cond)                                        \
    do {                                                   \
        if (!(cond)) {                                     \
            printf("frame_hosttest: line %d\n", __LINE__); \
            fails++;                                       \
        }                                                  \
    } while (0)

#define KIB(n) ((uint64_t)(n) * 1024u)
#define MIB(n) ((uint64_t)(n) * 1024u * 1024u)

/** Build one multiboot mmap entry into buf at offset, return bytes written. */
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

/* QEMU's map for `mib` MiB of memory, as the kernel recorded it: the low
 * 639 KiB, a hole to 1 MiB, low RAM to 128 KiB short of its end or of
 * 3 GiB, a reserved range there, the firmware's range below 4 GiB, and the
 * rest above 4 GiB when the machine has more than 3 GiB. */
static uint32_t qemu_map(uint8_t *buf, uint32_t mib)
{
    uint64_t total = MIB(mib);
    uint64_t low_end = total > MIB(3072) ? MIB(3072) : total;
    uint32_t n = 0;

    n += put_entry(buf, n, 0x0, 0x9FC00, MULTIBOOT_MEMORY_AVAILABLE);
    n += put_entry(buf, n, 0x9FC00, 0x400, MULTIBOOT_MEMORY_RESERVED);
    n += put_entry(buf, n, 0xF0000, 0x10000, MULTIBOOT_MEMORY_RESERVED);
    n += put_entry(buf, n, 0x100000, low_end - 0x20000 - 0x100000, MULTIBOOT_MEMORY_AVAILABLE);
    n += put_entry(buf, n, low_end - 0x20000, 0x20000, MULTIBOOT_MEMORY_RESERVED);
    n += put_entry(buf, n, 0xFFFC0000, 0x40000, MULTIBOOT_MEMORY_RESERVED);
    if (total > low_end) {
        n += put_entry(buf, n, 0x100000000ULL, total - low_end, MULTIBOOT_MEMORY_AVAILABLE);
    }
    return n;
}

/* The boot's own reservations on that map: a 5 MiB kernel from 1 MiB, a
 * 10 MiB module after it, and the allocator's break 3 MiB past the module. */
static void boot_reservations(struct frame_map *m)
{
    CHECK(frame_map_reserve(m, 0, MIB(6)) == 0);
    CHECK(frame_map_reserve(m, MIB(6), MIB(16)) == 0);
    CHECK(frame_map_reserve(m, 0, MIB(19)) == 0);
}

static void qemu_sizes(void)
{
    static const uint32_t sizes[] = {64, 512, 2048, 3072, 4096};
    /* What the kernel printed at each size, in KiB. */
    static const uint64_t usable[] = {65023, 523775, 2096639, 3145215, 3145215};
    static const uint64_t unreachable[] = {0, 0, 0, 0, 1048576};
    uint8_t map[512];
    uint32_t i;

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        struct frame_map m;
        uint32_t n = qemu_map(map, sizes[i]);

        CHECK(frame_map_build(&m, map, n, 0) == 0);
        CHECK(m.usable_bytes == KIB(usable[i]));
        CHECK(m.unreachable_bytes == KIB(unreachable[i]));
        /* The two together are what QEMU was given less its fixed holes: the
         * kilobyte at 0x9FC00, the 384 KiB below 1 MiB and the 128 KiB at
         * the end of low RAM. */
        CHECK(m.usable_bytes + m.unreachable_bytes + KIB(513) == MIB(sizes[i]));
        CHECK(!m.from_mem_upper);
        CHECK((uint64_t)m.top_frame * FRAME_PAGE_SIZE ==
              (sizes[i] > 3072 ? MIB(3072) : MIB(sizes[i])) - 0x20000);
        boot_reservations(&m);
        CHECK(m.reserved_bytes == MIB(19) - KIB(385));
        CHECK(frame_map_managed(&m) == m.usable_bytes - m.reserved_bytes);
        CHECK(!frame_map_usable(&m, 0));
        CHECK(!frame_map_usable(&m, (uint32_t)(MIB(18) / FRAME_PAGE_SIZE)));
        CHECK(frame_map_usable(&m, (uint32_t)(MIB(19) / FRAME_PAGE_SIZE)));
        CHECK(!frame_map_usable(&m, m.top_frame));
        CHECK(frame_map_usable(&m, m.top_frame - 1));
    }
}

static void synthetic(void)
{
    uint8_t map[1024];
    uint32_t n;
    struct frame_map m;

    /* Holes: three available ranges apart, with nothing between them. */
    n = 0;
    n += put_entry(map, n, MIB(1), MIB(1), MULTIBOOT_MEMORY_AVAILABLE);
    n += put_entry(map, n, MIB(4), MIB(2), MULTIBOOT_MEMORY_AVAILABLE);
    n += put_entry(map, n, MIB(8), MIB(8), MULTIBOOT_MEMORY_AVAILABLE);
    CHECK(frame_map_build(&m, map, n, 0) == 0);
    CHECK(m.count == 3);
    CHECK(m.usable_bytes == MIB(11));
    CHECK(m.top_frame == MIB(16) / FRAME_PAGE_SIZE);
    CHECK(!frame_map_usable(&m, (uint32_t)(MIB(3) / FRAME_PAGE_SIZE)));
    CHECK(frame_map_usable(&m, (uint32_t)(MIB(5) / FRAME_PAGE_SIZE)));
    /* A reservation over a hole counts only the usable part. */
    CHECK(frame_map_reserve(&m, MIB(1), MIB(5)) == 0);
    CHECK(m.reserved_bytes == MIB(2));
    CHECK(frame_map_managed(&m) == MIB(9));
    /* Reserving it again counts nothing twice. */
    CHECK(frame_map_reserve(&m, MIB(1), MIB(5)) == 0);
    CHECK(m.reserved_bytes == MIB(2));
    /* A reservation inside a range splits it. */
    CHECK(frame_map_reserve(&m, MIB(10), MIB(11)) == 0);
    CHECK(m.count == 3);
    CHECK(m.reserved_bytes == MIB(3));
    CHECK(!frame_map_usable(&m, (uint32_t)(MIB(10) / FRAME_PAGE_SIZE)));
    CHECK(frame_map_usable(&m, (uint32_t)(MIB(11) / FRAME_PAGE_SIZE)));

    /* An overlap: the reserved entry wins where the two meet, and the
     * unordered entries are sorted. */
    n = 0;
    n += put_entry(map, n, MIB(8), MIB(8), MULTIBOOT_MEMORY_AVAILABLE);
    n += put_entry(map, n, MIB(1), MIB(3), MULTIBOOT_MEMORY_AVAILABLE);
    n += put_entry(map, n, MIB(3), MIB(6), MULTIBOOT_MEMORY_RESERVED);
    CHECK(frame_map_build(&m, map, n, 0) == 0);
    CHECK(m.count == 2);
    CHECK(m.usable[0].start == MIB(1) && m.usable[0].end == MIB(3));
    CHECK(m.usable[1].start == MIB(9) && m.usable[1].end == MIB(16));
    CHECK(m.usable_bytes == MIB(9));

    /* Two available entries that overlap each other are one range. */
    n = 0;
    n += put_entry(map, n, MIB(1), MIB(4), MULTIBOOT_MEMORY_AVAILABLE);
    n += put_entry(map, n, MIB(3), MIB(4), MULTIBOOT_MEMORY_AVAILABLE);
    CHECK(frame_map_build(&m, map, n, 0) == 0);
    CHECK(m.count == 1 && m.usable_bytes == MIB(6));

    /* A range crossing 4 GiB: the part below is usable, the part above is
     * unreachable, and the arithmetic does not wrap. */
    n = 0;
    n += put_entry(map, n, MIB(1), MIB(4095) + MIB(512), MULTIBOOT_MEMORY_AVAILABLE);
    CHECK(frame_map_build(&m, map, n, 0) == 0);
    CHECK(m.usable_bytes == MIB(4095));
    CHECK(m.unreachable_bytes == MIB(512));
    CHECK(m.top_frame == 0x100000u);
    CHECK(frame_map_usable(&m, 0xFFFFFu));
    CHECK(!frame_map_usable(&m, 0x100000u));

    /* A range wholly above 4 GiB is unreachable in full. */
    n = 0;
    n += put_entry(map, n, 0x100000000ULL, MIB(256), MULTIBOOT_MEMORY_AVAILABLE);
    CHECK(frame_map_build(&m, map, n, 0) == 0);
    CHECK(m.count == 0 && m.unreachable_bytes == MIB(256) && !m.from_mem_upper);

    /* A length that wraps the 64-bit space ends at the space's last byte,
     * which is one short of the length a wider number would give. */
    n = 0;
    n += put_entry(map, n, 0xFFFFFFFFFFF00000ULL, 0x200000, MULTIBOOT_MEMORY_AVAILABLE);
    CHECK(frame_map_build(&m, map, n, 0) == 0);
    CHECK(m.count == 0 && m.unreachable_bytes == 0x100000u - 1);

    /* A truncated last entry is not read; the entries before it are. */
    n = 0;
    n += put_entry(map, n, MIB(1), MIB(2), MULTIBOOT_MEMORY_AVAILABLE);
    n += put_entry(map, n, MIB(4), MIB(2), MULTIBOOT_MEMORY_AVAILABLE);
    CHECK(frame_map_build(&m, map, n + 4, 0) == 0);
    CHECK(m.usable_bytes == MIB(4));
    CHECK(frame_map_build(&m, map, n - 4, 0) == 0);
    CHECK(m.usable_bytes == MIB(2));

    /* No map at all: the older field, one range from 1 MiB. */
    CHECK(frame_map_build(&m, 0, 0, 31 * 1024) == 0);
    CHECK(m.from_mem_upper && m.count == 1);
    CHECK(m.usable[0].start == MIB(1) && m.usable[0].end == MIB(32));
    CHECK(frame_map_build(&m, map, 0, 0) == 0);
    CHECK(m.from_mem_upper && m.count == 0 && m.top_frame == 0);

    /* More ranges than the map keeps: refused whole. */
    {
        uint32_t i;

        n = 0;
        for (i = 0; i < FRAME_MAP_RANGES + 1; i++) {
            n += put_entry(map, n, MIB(1 + 2 * i), MIB(1), MULTIBOOT_MEMORY_AVAILABLE);
        }
        CHECK(frame_map_build(&m, map, n, 0) == -1);
        CHECK(m.count == 0);
    }
}

int main(void)
{
    qemu_sizes();
    synthetic();
    return (fails > 50) ? 50 : fails;
}
