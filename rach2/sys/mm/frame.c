/*
 * frame.c: physical memory from the firmware's map, as frame.h describes it.
 */

#include "frame.h"

/* An entry of the boot record's map (boot.h): a 32-bit size of what
 * follows, a 64-bit base, a 64-bit length and a 32-bit type, packed and
 * little-endian, at these offsets. The record keeps no alignment, so the
 * fields are read a byte at a time. */
#define ENTRY_SIZE 0
#define ENTRY_BASE 4
#define ENTRY_LEN  12
#define ENTRY_TYPE 20

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/* The `index`-th entry, walking the map as its format says. */
static const uint8_t *entry_at(const void *mmap_buf, uint32_t mmap_len, uint32_t index)
{
    const uint8_t *p;
    const uint8_t *end;
    uint32_t i = 0;

    if (mmap_buf == NULL) {
        return NULL;
    }
    p = (const uint8_t *)mmap_buf;
    end = p + mmap_len;
    while (end - p >= 4) {
        uint32_t size = le32(p + ENTRY_SIZE);

        if (size < BOOT_MMAP_ENTRY - 4 || (uint32_t)(end - p) - 4 < size) {
            return NULL;
        }
        if (i == index) {
            return p;
        }
        i++;
        p += size + 4;
    }
    return NULL;
}

int frame_map_entry(const void *mmap_buf, uint32_t mmap_len, uint32_t index, uint64_t *addr,
                    uint64_t *len, uint32_t *type)
{
    const uint8_t *e = entry_at(mmap_buf, mmap_len, index);

    if (e == NULL) {
        return 0;
    }
    *addr = le64(e + ENTRY_BASE);
    *len = le64(e + ENTRY_LEN);
    *type = le32(e + ENTRY_TYPE);
    return 1;
}

/* Put `[start, end)` into the sorted, disjoint list, merging what it
 * touches. Returns -1 when the list is full. */
static int range_add(struct frame_map *m, uint64_t start, uint64_t end)
{
    uint32_t i, j;

    if (end <= start) {
        return 0;
    }
    /* Absorb every range that overlaps or abuts the new one. */
    for (i = 0; i < m->count;) {
        if (m->usable[i].end < start || m->usable[i].start > end) {
            i++;
            continue;
        }
        if (m->usable[i].start < start) {
            start = m->usable[i].start;
        }
        if (m->usable[i].end > end) {
            end = m->usable[i].end;
        }
        for (j = i; j + 1 < m->count; j++) {
            m->usable[j] = m->usable[j + 1];
        }
        m->count--;
    }
    if (m->count >= FRAME_MAP_RANGES) {
        return -1;
    }
    for (i = 0; i < m->count && m->usable[i].start < start; i++) {
    }
    for (j = m->count; j > i; j--) {
        m->usable[j] = m->usable[j - 1];
    }
    m->usable[i].start = start;
    m->usable[i].end = end;
    m->count++;
    return 0;
}

/* Take `[start, end)` out of the list; `*taken` gains the bytes that were
 * in it. Returns -1 when a split needs a range the list has no room for. */
static int range_subtract(struct frame_map *m, uint64_t start, uint64_t end, uint64_t *taken)
{
    uint32_t i, j;

    if (end <= start) {
        return 0;
    }
    for (i = 0; i < m->count;) {
        struct frame_range *r = &m->usable[i];

        if (r->end <= start || r->start >= end) {
            i++;
            continue;
        }
        if (start <= r->start && end >= r->end) {
            *taken += r->end - r->start;
            for (j = i; j + 1 < m->count; j++) {
                m->usable[j] = m->usable[j + 1];
            }
            m->count--;
            continue;
        }
        if (start > r->start && end < r->end) {
            if (m->count >= FRAME_MAP_RANGES) {
                return -1;
            }
            for (j = m->count; j > i + 1; j--) {
                m->usable[j] = m->usable[j - 1];
            }
            m->count++;
            m->usable[i + 1].start = end;
            m->usable[i + 1].end = r->end;
            r->end = start;
            *taken += end - start;
            i += 2;
            continue;
        }
        if (start <= r->start) {
            *taken += end - r->start;
            r->start = end;
        } else {
            *taken += r->end - start;
            r->end = start;
        }
        i++;
    }
    return 0;
}

/* Set `usable_bytes` and `top_frame` from the ranges. */
static void recount(struct frame_map *m)
{
    uint32_t i;

    m->usable_bytes = 0;
    m->top_frame = 0;
    for (i = 0; i < m->count; i++) {
        m->usable_bytes += m->usable[i].end - m->usable[i].start;
    }
    if (m->count > 0) {
        m->top_frame = (m->usable[m->count - 1].end + FRAME_PAGE_SIZE - 1) / FRAME_PAGE_SIZE;
    }
}

/* The end of an entry: its base plus its length, or the space's last byte
 * for a length that wraps. */
static uint64_t entry_end(uint64_t addr, uint64_t len)
{
    return addr + len < addr ? ~(uint64_t)0 : addr + len;
}

int frame_map_build(struct frame_map *m, const void *mmap_buf, uint32_t mmap_len,
                    uint32_t mem_upper_kb, uint64_t reach)
{
    uint32_t i;
    uint64_t addr, len;
    uint32_t type;
    uint64_t dropped = 0;

    m->count = 0;
    m->usable_bytes = 0;
    m->unreachable_bytes = 0;
    m->reserved_bytes = 0;
    m->top_frame = 0;
    m->from_mem_upper = 0;

    for (i = 0; frame_map_entry(mmap_buf, mmap_len, i, &addr, &len, &type); i++) {
        if (type == FRAME_MAP_USABLE && range_add(m, addr, entry_end(addr, len)) != 0) {
            m->count = 0;
            return -1;
        }
    }
    /* Anything of another type wins where it overlaps an available range:
     * the conservative reading of a map that contradicts itself. */
    for (i = 0; frame_map_entry(mmap_buf, mmap_len, i, &addr, &len, &type); i++) {
        if (type != FRAME_MAP_USABLE &&
            range_subtract(m, addr, entry_end(addr, len), &dropped) != 0) {
            m->count = 0;
            return -1;
        }
    }
    /* What lies past the reach is counted and dropped: merged and
     * subtracted like the rest first, so an entry given twice or overlapped
     * by a reserved one counts once and not at all. */
    if (range_subtract(m, reach, ~(uint64_t)0, &m->unreachable_bytes) != 0) {
        m->count = 0;
        return -1;
    }
    if (m->count == 0 && m->unreachable_bytes == 0) {
        /* No map, or nothing available in it: the older field, as one range
         * from the end of the first megabyte. */
        m->from_mem_upper = 1;
        if (mem_upper_kb > 0) {
            uint64_t end = 0x100000ULL + (uint64_t)mem_upper_kb * 1024u;

            if (end > reach) {
                m->unreachable_bytes += end - reach;
                end = reach;
            }
            range_add(m, 0x100000ULL, end);
        }
    }
    recount(m);
    return 0;
}

int frame_map_reserve(struct frame_map *m, uint64_t start, uint64_t end)
{
    uint64_t taken = 0;

    if (range_subtract(m, start, end, &taken) != 0) {
        return -1;
    }
    m->reserved_bytes += taken;
    return 0;
}

int frame_map_usable(const struct frame_map *m, uint64_t frame)
{
    uint64_t start = frame * FRAME_PAGE_SIZE;
    uint64_t end = start + FRAME_PAGE_SIZE;
    uint32_t i;

    for (i = 0; i < m->count; i++) {
        if (m->usable[i].start <= start && m->usable[i].end >= end) {
            return 1;
        }
    }
    return 0;
}

uint64_t frame_map_managed(const struct frame_map *m)
{
    return m->usable_bytes - m->reserved_bytes;
}

int frame_map_place(const struct frame_map *m, uint64_t bytes, uint64_t floor, uint64_t *at)
{
    uint64_t span = (bytes + FRAME_PAGE_SIZE - 1) & ~(uint64_t)(FRAME_PAGE_SIZE - 1);
    uint32_t i = m->count;

    if (bytes == 0 || span < bytes) {
        return -1;
    }
    while (i-- > 0) {
        uint64_t lo = m->usable[i].start > floor ? m->usable[i].start : floor;
        uint64_t hi = m->usable[i].end;
        uint64_t start;

        if (lo >= hi || hi - lo < span) {
            continue;
        }
        start = (hi - span) & ~(uint64_t)(FRAME_PAGE_SIZE - 1);
        if (start >= lo) {
            *at = start;
            return 0;
        }
    }
    return -1;
}
