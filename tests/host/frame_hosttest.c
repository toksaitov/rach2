/**
 * Host test for the physical memory map (mm/frame.c).
 *
 * host-deps: rach2/sys/mm/frame.c
 *
 * The map is built from memory maps in the boot record's form (boot.h)
 * crafted here, under ASan and UBSan, with i686's reach of 4 GiB: QEMU's
 * maps as this kernel recorded them at boot for 64 MiB, 512 MiB, 2 GiB,
 * 3 GiB and 4 GiB; the Lenovo G50-70's map as its boot of 2026-09-12
 * printed it (dev-log/50-shortcomings/lenovo-boot.log), with NVS and ACPI
 * ranges, a range that ends inside a page, and most of its memory above
 * 4 GiB; and synthetic maps with holes, an overlap between an available
 * range and a reserved one, a range crossing 4 GiB, an entry whose length
 * wraps, a truncated last entry, an empty map, and more ranges than the map
 * keeps. Then with x86-64's reach, the direct map's 64 TiB: QEMU's maps at
 * 6 GiB and 8 GiB, the Lenovo's, a map past the direct map, and a map shaped
 * as a UEFI firmware gives one, of the record's whole BOOT_MMAP_MAX entries,
 * with the reservations the boot makes on it. The reservations are replayed
 * against each and the four numbers checked, and the frame table is placed
 * with `frame_map_place`. Returns the failed-check count (0 == pass).
 * tests/host/frame_mutate.sh counts the NVS and ACPI types as memory and
 * requires the Lenovo's two rows, and no other, to fail.
 */

#include "mm/frame.h"

/* The host's printf, declared here: the kernel's headers define the
 * integer types the host's stdio.h would define again. */
int printf(const char *format, ...);

/* The map's types (boot.h): usable, reserved, ACPI's reclaimable tables
 * and ACPI's non-volatile storage; the boot prints the last two as `acpi`
 * and `nvs`. */
#define MAP_USABLE   1
#define MAP_RESERVED 2
#define MAP_ACPI     3
#define MAP_NVS      4

/* The reach of each architecture: i686's 32-bit physical addresses, and
 * the 64 TiB of x86-64's direct map (KVA_DIRECT_SIZE). */
#define REACH_I686   FRAME_REACH_32
#define REACH_X86_64 0x400000000000ULL

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

/** Write `v` into `p` as `bytes` little-endian bytes. */
static void put_le(uint8_t *p, uint64_t v, uint32_t bytes)
{
    uint32_t i;

    for (i = 0; i < bytes; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/** Build one map entry into buf at offset, in the record's form of
 * BOOT_MMAP_ENTRY bytes; return the bytes written. */
static uint32_t put_entry(uint8_t *buf, uint32_t off, uint64_t addr, uint64_t len, uint32_t type)
{
    put_le(buf + off, BOOT_MMAP_ENTRY - 4, 4);
    put_le(buf + off + 4, addr, 8);
    put_le(buf + off + 12, len, 8);
    put_le(buf + off + 20, type, 4);
    return BOOT_MMAP_ENTRY;
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

    n += put_entry(buf, n, 0x0, 0x9FC00, MAP_USABLE);
    n += put_entry(buf, n, 0x9FC00, 0x400, MAP_RESERVED);
    n += put_entry(buf, n, 0xF0000, 0x10000, MAP_RESERVED);
    n += put_entry(buf, n, 0x100000, low_end - 0x20000 - 0x100000, MAP_USABLE);
    n += put_entry(buf, n, low_end - 0x20000, 0x20000, MAP_RESERVED);
    n += put_entry(buf, n, 0xFFFC0000, 0x40000, MAP_RESERVED);
    if (total > low_end) {
        n += put_entry(buf, n, 0x100000000ULL, total - low_end, MAP_USABLE);
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

        CHECK(frame_map_build(&m, map, n, 0, REACH_I686) == 0);
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

/* One firmware range as the boot prints it: its start, its end and its type. */
struct printed_range {
    uint64_t start;
    uint64_t end;
    uint32_t type;
};

/* The Lenovo's map, line for line and in the firmware's order. */
static const struct printed_range lenovo_ranges[] = {
    {0x0000000000000000ULL, 0x000000000009D400ULL, MAP_USABLE},
    {0x000000000009D400ULL, 0x00000000000A0000ULL, MAP_RESERVED},
    {0x00000000000E0000ULL, 0x0000000000100000ULL, MAP_RESERVED},
    {0x0000000000100000ULL, 0x00000000A56B0000ULL, MAP_USABLE},
    {0x00000000A56B0000ULL, 0x00000000A5EB0000ULL, MAP_RESERVED},
    {0x00000000A5EB0000ULL, 0x00000000AAABF000ULL, MAP_USABLE},
    {0x00000000AAABF000ULL, 0x00000000AAEBF000ULL, MAP_RESERVED},
    {0x00000000AAEBF000ULL, 0x00000000AAFBF000ULL, MAP_NVS},
    {0x00000000AAFBF000ULL, 0x00000000AAFFF000ULL, MAP_ACPI},
    {0x00000000AAFFF000ULL, 0x00000000AB000000ULL, MAP_USABLE},
    {0x00000000AB000000ULL, 0x00000000AFA00000ULL, MAP_RESERVED},
    {0x00000000E0000000ULL, 0x00000000F0000000ULL, MAP_RESERVED},
    {0x00000000FE101000ULL, 0x00000000FE113000ULL, MAP_RESERVED},
    {0x00000000FEB00000ULL, 0x00000000FEB10000ULL, MAP_RESERVED},
    {0x00000000FEC00000ULL, 0x00000000FEC01000ULL, MAP_RESERVED},
    {0x00000000FED00000ULL, 0x00000000FEE01000ULL, MAP_RESERVED},
    {0x00000000FFC00000ULL, 0x0000000100000000ULL, MAP_RESERVED},
    {0x0000000100000000ULL, 0x000000044F600000ULL, MAP_USABLE},
};

#define LENOVO_RANGES (sizeof(lenovo_ranges) / sizeof(lenovo_ranges[0]))
/* The allocator's break at that boot; see `lenovo`. */
#define LENOVO_BREAK  0x736000u

/**
 * The Lenovo's map, checked against the four numbers its boot printed:
 * usable 2787701 KiB, unreachable 13883392 KiB, reserved 6989 KiB and
 * managed 2780712 KiB.
 *
 * That boot loaded no module, so its reservations come to everything below
 * the allocator's break, which covers the kernel image. The printed reserved
 * figure puts the break in the kilobyte from 0x736000, and the break is a
 * page boundary, since the last thing the boot carves is a page table; so
 * the break was 0x736000. The boot took its managed figure from the frame
 * table and not from this arithmetic, so the two must agree.
 *
 * The pages probed are the edges of the available ranges below 4 GiB, the
 * page the first range ends inside, which is not whole, the first pages of
 * the NVS and ACPI ranges, types QEMU's maps do not have, and the one
 * available page after them, which is the last usable page of the map.
 */
static void lenovo(void)
{
    uint8_t map[LENOVO_RANGES * BOOT_MMAP_ENTRY];
    struct frame_map m;
    uint32_t n = 0;
    uint32_t i;

    for (i = 0; i < LENOVO_RANGES; i++) {
        n += put_entry(map, n, lenovo_ranges[i].start,
                       lenovo_ranges[i].end - lenovo_ranges[i].start, lenovo_ranges[i].type);
    }
    CHECK(frame_map_build(&m, map, n, 0, REACH_I686) == 0);
    CHECK(m.usable_bytes == KIB(2787701));
    CHECK(m.unreachable_bytes == KIB(13883392));
    CHECK(!m.from_mem_upper);
    CHECK(m.count == 4);
    CHECK(m.top_frame == 0xAB000u);
    CHECK(frame_map_usable(&m, 0x9Cu));
    CHECK(!frame_map_usable(&m, 0x9Du));
    CHECK(frame_map_usable(&m, 0x100u));
    CHECK(frame_map_usable(&m, 0xA56AFu));
    CHECK(!frame_map_usable(&m, 0xA56B0u));
    CHECK(frame_map_usable(&m, 0xA5EB0u));
    CHECK(frame_map_usable(&m, 0xAAABEu));
    CHECK(!frame_map_usable(&m, 0xAAABFu));
    CHECK(!frame_map_usable(&m, 0xAAEBFu));
    CHECK(!frame_map_usable(&m, 0xAAFBFu));
    CHECK(frame_map_usable(&m, 0xAAFFFu));
    CHECK(!frame_map_usable(&m, m.top_frame));
    CHECK(frame_map_reserve(&m, 0, LENOVO_BREAK) == 0);
    CHECK(m.reserved_bytes == KIB(6989));
    CHECK(frame_map_managed(&m) == KIB(2780712));
    CHECK(!frame_map_usable(&m, 0));
    CHECK(!frame_map_usable(&m, LENOVO_BREAK / FRAME_PAGE_SIZE - 1));
    CHECK(frame_map_usable(&m, LENOVO_BREAK / FRAME_PAGE_SIZE));
    CHECK(frame_map_usable(&m, m.top_frame - 1));
}

static void synthetic(void)
{
    uint8_t map[(FRAME_MAP_RANGES + 1) * BOOT_MMAP_ENTRY];
    uint32_t n;
    struct frame_map m;

    /* Holes: three available ranges apart, with nothing between them. */
    n = 0;
    n += put_entry(map, n, MIB(1), MIB(1), MAP_USABLE);
    n += put_entry(map, n, MIB(4), MIB(2), MAP_USABLE);
    n += put_entry(map, n, MIB(8), MIB(8), MAP_USABLE);
    CHECK(frame_map_build(&m, map, n, 0, REACH_I686) == 0);
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
    n += put_entry(map, n, MIB(8), MIB(8), MAP_USABLE);
    n += put_entry(map, n, MIB(1), MIB(3), MAP_USABLE);
    n += put_entry(map, n, MIB(3), MIB(6), MAP_RESERVED);
    CHECK(frame_map_build(&m, map, n, 0, REACH_I686) == 0);
    CHECK(m.count == 2);
    CHECK(m.usable[0].start == MIB(1) && m.usable[0].end == MIB(3));
    CHECK(m.usable[1].start == MIB(9) && m.usable[1].end == MIB(16));
    CHECK(m.usable_bytes == MIB(9));

    /* Two available entries that overlap each other are one range. */
    n = 0;
    n += put_entry(map, n, MIB(1), MIB(4), MAP_USABLE);
    n += put_entry(map, n, MIB(3), MIB(4), MAP_USABLE);
    CHECK(frame_map_build(&m, map, n, 0, REACH_I686) == 0);
    CHECK(m.count == 1 && m.usable_bytes == MIB(6));

    /* A range crossing 4 GiB: the part below is usable, the part above is
     * unreachable, and the arithmetic does not wrap. */
    n = 0;
    n += put_entry(map, n, MIB(1), MIB(4095) + MIB(512), MAP_USABLE);
    CHECK(frame_map_build(&m, map, n, 0, REACH_I686) == 0);
    CHECK(m.usable_bytes == MIB(4095));
    CHECK(m.unreachable_bytes == MIB(512));
    CHECK(m.top_frame == 0x100000u);
    CHECK(frame_map_usable(&m, 0xFFFFFu));
    CHECK(!frame_map_usable(&m, 0x100000u));

    /* A range wholly above 4 GiB is unreachable in full. */
    n = 0;
    n += put_entry(map, n, 0x100000000ULL, MIB(256), MAP_USABLE);
    CHECK(frame_map_build(&m, map, n, 0, REACH_I686) == 0);
    CHECK(m.count == 0 && m.unreachable_bytes == MIB(256) && !m.from_mem_upper);

    /* A length that wraps the 64-bit space ends at the space's last byte,
     * which is one short of the length a wider number would give. */
    n = 0;
    n += put_entry(map, n, 0xFFFFFFFFFFF00000ULL, 0x200000, MAP_USABLE);
    CHECK(frame_map_build(&m, map, n, 0, REACH_I686) == 0);
    CHECK(m.count == 0 && m.unreachable_bytes == 0x100000u - 1);

    /* A truncated last entry is not read; the entries before it are. */
    n = 0;
    n += put_entry(map, n, MIB(1), MIB(2), MAP_USABLE);
    n += put_entry(map, n, MIB(4), MIB(2), MAP_USABLE);
    CHECK(frame_map_build(&m, map, n + 4, 0, REACH_I686) == 0);
    CHECK(m.usable_bytes == MIB(4));
    CHECK(frame_map_build(&m, map, n - 4, 0, REACH_I686) == 0);
    CHECK(m.usable_bytes == MIB(2));

    /* No map at all: the older field, one range from 1 MiB. */
    CHECK(frame_map_build(&m, 0, 0, 31 * 1024, REACH_I686) == 0);
    CHECK(m.from_mem_upper && m.count == 1);
    CHECK(m.usable[0].start == MIB(1) && m.usable[0].end == MIB(32));
    CHECK(frame_map_build(&m, map, 0, 0, REACH_I686) == 0);
    CHECK(m.from_mem_upper && m.count == 0 && m.top_frame == 0);

    /* More ranges than the map keeps: refused whole. */
    {
        uint32_t i;

        n = 0;
        for (i = 0; i < FRAME_MAP_RANGES + 1; i++) {
            n += put_entry(map, n, MIB(1 + 2 * i), MIB(1), MAP_USABLE);
        }
        CHECK(frame_map_build(&m, map, n, 0, REACH_I686) == -1);
        CHECK(m.count == 0);
    }
}

/**
 * QEMU's maps at 6 GiB and 8 GiB with x86-64's reach: all of the memory
 * usable and none unreachable, the top frame at 4 GiB plus what lies above
 * 3 GiB, and the frame table placed at the top of memory, above the window,
 * where reserving it takes its whole frames from the managed figure.
 */
static void x86_64_qemu(void)
{
    static const uint32_t sizes[] = {6144, 8192};
    uint8_t map[512];
    uint32_t i;

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        struct frame_map m;
        uint32_t n = qemu_map(map, sizes[i]);
        uint64_t top = 0x100000000ULL + MIB(sizes[i]) - MIB(3072);
        uint64_t table = top / FRAME_PAGE_SIZE * 4u;
        uint64_t at = 0;

        CHECK(frame_map_build(&m, map, n, 0, REACH_X86_64) == 0);
        CHECK(m.usable_bytes + KIB(513) == MIB(sizes[i]));
        CHECK(m.unreachable_bytes == 0);
        CHECK(m.top_frame * FRAME_PAGE_SIZE == top);
        CHECK(frame_map_usable(&m, 0x100000u) && frame_map_usable(&m, m.top_frame - 1));
        CHECK(!frame_map_usable(&m, m.top_frame));
        boot_reservations(&m);
        CHECK(m.reserved_bytes == MIB(19) - KIB(385));
        CHECK(frame_map_place(&m, table, FRAME_DIRECT_MAP_LIMIT, &at) == 0);
        CHECK(at == top - table);
        CHECK(frame_map_reserve(&m, at, at + table) == 0);
        CHECK(m.reserved_bytes == MIB(19) - KIB(385) + table);
        CHECK(frame_map_managed(&m) == MIB(sizes[i]) - KIB(513) - m.reserved_bytes);
        CHECK(!frame_map_usable(&m, at / FRAME_PAGE_SIZE));
        CHECK(frame_map_usable(&m, at / FRAME_PAGE_SIZE - 1));
    }
}

/**
 * The Lenovo's map with x86-64's reach: the 13883392 KiB above 4 GiB that
 * i686 counts unreachable are usable, the top frame is the map's last, and
 * the reservations below the break take what they took on i686.
 */
static void x86_64_lenovo(void)
{
    uint8_t map[LENOVO_RANGES * BOOT_MMAP_ENTRY];
    struct frame_map m;
    uint32_t n = 0;
    uint32_t i;

    for (i = 0; i < LENOVO_RANGES; i++) {
        n += put_entry(map, n, lenovo_ranges[i].start,
                       lenovo_ranges[i].end - lenovo_ranges[i].start, lenovo_ranges[i].type);
    }
    CHECK(frame_map_build(&m, map, n, 0, REACH_X86_64) == 0);
    CHECK(m.usable_bytes == KIB(2787701) + KIB(13883392));
    CHECK(m.unreachable_bytes == 0 && !m.from_mem_upper);
    CHECK(m.count == 5);
    CHECK(m.top_frame == 0x44F600u);
    CHECK(!frame_map_usable(&m, 0xAAEBFu) && !frame_map_usable(&m, 0xAAFBFu));
    CHECK(frame_map_usable(&m, 0x100000u) && frame_map_usable(&m, 0x44F5FFu));
    CHECK(frame_map_reserve(&m, 0, LENOVO_BREAK) == 0);
    CHECK(m.reserved_bytes == KIB(6989));
    CHECK(frame_map_managed(&m) == KIB(2780712) + KIB(13883392));
}

/**
 * Memory past the direct map: x86-64 reaches 64 TiB, and what lies above
 * is counted unreachable, by the same arithmetic i686 uses at 4 GiB.
 */
static void x86_64_past_reach(void)
{
    uint8_t map[2 * BOOT_MMAP_ENTRY];
    struct frame_map m;
    uint32_t n = 0;

    n += put_entry(map, n, MIB(1), MIB(255), MAP_USABLE);
    n += put_entry(map, n, REACH_X86_64 - MIB(1024 * 1024), MIB(2 * 1024 * 1024), MAP_USABLE);
    CHECK(frame_map_build(&m, map, n, 0, REACH_X86_64) == 0);
    CHECK(m.usable_bytes == MIB(255) + MIB(1024 * 1024));
    CHECK(m.unreachable_bytes == MIB(1024 * 1024));
    CHECK(m.top_frame == REACH_X86_64 / FRAME_PAGE_SIZE);
    CHECK(frame_map_usable(&m, REACH_X86_64 / FRAME_PAGE_SIZE - 1));
}

/* The UEFI-shaped map's groups: a run of memory the firmware lists as two
 * entries of two of its types, both usable, then a small region of
 * another type. */
/* The runs that, with the map's eight other entries, fill the record's
 * BOOT_MMAP_MAX, three entries each, and the entries left over, which the
 * last range above 4 GiB takes as adjacent pieces. */
#define UEFI_GROUPS     ((BOOT_MMAP_MAX - 8u) / 3u)
#define UEFI_PIECES     ((BOOT_MMAP_MAX - 8u) % 3u)
#define UEFI_GROUP_SPAN (MIB(16) + KIB(64))

static uint64_t uefi_group(uint32_t k)
{
    return MIB(1) + (uint64_t)k * UEFI_GROUP_SPAN;
}

/**
 * A map shaped as a UEFI firmware gives one, through GRUB's conversion of
 * its types, with the record's whole BOOT_MMAP_MAX entries: the first
 * 640 KiB, then UEFI_GROUPS runs of 16 MiB each listed as two usable
 * entries and closed by 64 KiB of runtime, ACPI or NVS memory, the
 * platform's ranges below 4 GiB, and 12 GiB above it with a hole, its last
 * range in as many adjacent pieces as fill the record. With x86-64's reach all of
 * it is usable; with i686's the 12 GiB less the hole is unreachable. The
 * boot's reservations on it, the kernel, eight modules each inside a run
 * and the break, and the frame table at the top, all fit the ranges the
 * map keeps.
 */
static void uefi(void)
{
    static const uint32_t holes[] = {MAP_RESERVED, MAP_ACPI, MAP_NVS};
    uint8_t map[BOOT_MMAP_MAX * BOOT_MMAP_ENTRY];
    uint64_t low = KIB(640) + (uint64_t)UEFI_GROUPS * MIB(16);
    uint64_t high = MIB(12 * 1024) - MIB(2);
    struct frame_map m;
    uint32_t n = 0;
    uint32_t k;
    uint64_t at = 0;

    n += put_entry(map, n, 0, KIB(640), MAP_USABLE);
    n += put_entry(map, n, KIB(640), KIB(384), MAP_RESERVED);
    for (k = 0; k < UEFI_GROUPS; k++) {
        n += put_entry(map, n, uefi_group(k), MIB(8), MAP_USABLE);
        n += put_entry(map, n, uefi_group(k) + MIB(8), MIB(8), MAP_USABLE);
        n += put_entry(map, n, uefi_group(k) + MIB(16), KIB(64), holes[k % 3]);
    }
    n += put_entry(map, n, 0xE0000000ULL, MIB(256), MAP_RESERVED);
    n += put_entry(map, n, 0xFEC00000ULL, KIB(4), MAP_RESERVED);
    n += put_entry(map, n, 0xFFC00000ULL, MIB(4), MAP_RESERVED);
    n += put_entry(map, n, 0x100000000ULL, MIB(8 * 1024), MAP_USABLE);
    n += put_entry(map, n, 0x300000000ULL, MIB(2), MAP_RESERVED);
    for (k = 0; k < UEFI_PIECES; k++) {
        n += put_entry(map, n, 0x300000000ULL + MIB(2) + k * MIB(1), MIB(1), MAP_USABLE);
    }
    n += put_entry(map, n, 0x300000000ULL + MIB(2) + UEFI_PIECES * MIB(1),
                   MIB(4 * 1024) - MIB(2) - UEFI_PIECES * MIB(1), MAP_USABLE);
    CHECK(n == BOOT_MMAP_MAX * BOOT_MMAP_ENTRY);

    CHECK(frame_map_build(&m, map, n, 0, REACH_I686) == 0);
    CHECK(m.count == 1 + UEFI_GROUPS);
    CHECK(m.usable_bytes == low && m.unreachable_bytes == high);

    CHECK(frame_map_build(&m, map, n, 0, REACH_X86_64) == 0);
    CHECK(m.count == 1 + UEFI_GROUPS + 2);
    CHECK(m.usable_bytes == low + high && m.unreachable_bytes == 0);
    CHECK(m.top_frame == 0x400000000ULL / FRAME_PAGE_SIZE);
    CHECK(!frame_map_usable(&m, (uefi_group(7) + MIB(16)) / FRAME_PAGE_SIZE));
    CHECK(frame_map_usable(&m, (uefi_group(7) + MIB(16) - 1) / FRAME_PAGE_SIZE));
    CHECK(frame_map_usable(&m, (uefi_group(7) + MIB(8)) / FRAME_PAGE_SIZE));
    CHECK(!frame_map_usable(&m, 0x300000000ULL / FRAME_PAGE_SIZE));

    CHECK(frame_map_reserve(&m, 0, MIB(6)) == 0);
    for (k = 10; k < 10 + BOOT_MODULE_MAX; k++) {
        CHECK(frame_map_reserve(&m, uefi_group(k) + MIB(4), uefi_group(k) + MIB(5)) == 0);
    }
    CHECK(frame_map_reserve(&m, 0, MIB(19)) == 0);
    CHECK(frame_map_place(&m, m.top_frame * 4u, FRAME_DIRECT_MAP_LIMIT, &at) == 0);
    CHECK(at == 0x400000000ULL - m.top_frame * 4u);
    CHECK(frame_map_reserve(&m, at, at + m.top_frame * 4u) == 0);
    CHECK(m.reserved_bytes ==
          KIB(640) + (MIB(19) - MIB(1) - KIB(64)) + BOOT_MODULE_MAX * MIB(1) + m.top_frame * 4u);
    CHECK(frame_map_managed(&m) == low + high - m.reserved_bytes);
}

/**
 * The bound FRAME_MAP_RANGES is sized for: a map of BOOT_MMAP_MAX disjoint
 * usable entries keeps every one, and the reservations that split a range,
 * one for each of BOOT_MODULE_MAX modules and one for the frame table, all
 * fit; one split more is refused and leaves the map as it was.
 */
static void ranges_bound(void)
{
    uint8_t map[BOOT_MMAP_MAX * BOOT_MMAP_ENTRY];
    struct frame_map m;
    uint64_t reserved;
    uint32_t n = 0;
    uint32_t i;

    for (i = 0; i < BOOT_MMAP_MAX; i++) {
        n += put_entry(map, n, MIB(1 + 4 * i), MIB(2), MAP_USABLE);
    }
    CHECK(frame_map_build(&m, map, n, 0, REACH_X86_64) == 0);
    CHECK(m.count == BOOT_MMAP_MAX);
    for (i = 0; i < BOOT_MODULE_MAX + 1; i++) {
        CHECK(frame_map_reserve(&m, MIB(1 + 4 * i) + KIB(512), MIB(2 + 4 * i)) == 0);
    }
    CHECK(m.count == FRAME_MAP_RANGES);
    reserved = m.reserved_bytes;
    CHECK(frame_map_reserve(&m, MIB(1 + 4 * i) + KIB(512), MIB(2 + 4 * i)) == -1);
    CHECK(m.count == FRAME_MAP_RANGES && m.reserved_bytes == reserved);
}

/**
 * The frame table's place: the top of the highest range that holds it at
 * or above the floor, page-aligned, whole frames; a lower range when the
 * highest is too small; and a refusal when none holds it.
 */
static void place(void)
{
    uint8_t map[3 * BOOT_MMAP_ENTRY];
    struct frame_map m;
    uint32_t n = 0;
    uint64_t at = 0;

    n += put_entry(map, n, MIB(1), MIB(63), MAP_USABLE);
    n += put_entry(map, n, MIB(128), MIB(64), MAP_USABLE);
    n += put_entry(map, n, 0x100000000ULL, MIB(1) + 0x800, MAP_USABLE);
    CHECK(frame_map_build(&m, map, n, 0, REACH_X86_64) == 0);
    CHECK(frame_map_place(&m, KIB(1024), MIB(32), &at) == 0 && at == 0x100000000ULL);
    CHECK(frame_map_place(&m, KIB(1024) + 1, MIB(32), &at) == 0 &&
          at == MIB(192) - MIB(1) - KIB(4));
    CHECK(frame_map_place(&m, MIB(64), MIB(32), &at) == 0 && at == MIB(128));
    CHECK(frame_map_place(&m, MIB(32), MIB(160), &at) == 0 && at == MIB(160));
    CHECK(frame_map_place(&m, MIB(64) + 1, MIB(32), &at) == -1);
    CHECK(frame_map_place(&m, MIB(42), MIB(150), &at) == 0 && at == MIB(150));
    CHECK(frame_map_place(&m, MIB(42) + 1, MIB(150), &at) == -1);
    CHECK(frame_map_place(&m, 0, MIB(32), &at) == -1);
}

int main(void)
{
    qemu_sizes();
    lenovo();
    synthetic();
    x86_64_qemu();
    x86_64_lenovo();
    x86_64_past_reach();
    uefi();
    ranges_bound();
    place();
    return (fails > 50) ? 50 : fails;
}
