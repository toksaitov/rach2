/*
 * frame.c: physical memory from the firmware's map, as frame.h describes it.
 */

#include "frame.h"

#include "../../vendor/multiboot.h"

#define FOUR_GIB 0x100000000ULL

/* The `index`-th entry, walking the map as its format says. */
static const multiboot_memory_map_t *entry_at(const void *mmap_buf, uint32_t mmap_len,
                                              uint32_t index)
{
    const uint8_t *p;
    const uint8_t *end;
    uint32_t i = 0;

    if (mmap_buf == NULL) {
        return NULL;
    }
    p = (const uint8_t *)mmap_buf;
    end = p + mmap_len;
    while (p + sizeof(multiboot_uint32_t) <= end) {
        const multiboot_memory_map_t *e = (const multiboot_memory_map_t *)p;
        uint32_t advance = e->size + (uint32_t)sizeof(e->size);

        if (advance < (uint32_t)sizeof(multiboot_memory_map_t) ||
            (const uint8_t *)e + advance > end) {
            return NULL;
        }
        if (i == index) {
            return e;
        }
        i++;
        p += advance;
    }
    return NULL;
}

int frame_map_entry(const void *mmap_buf, uint32_t mmap_len, uint32_t index, uint64_t *addr,
                    uint64_t *len, uint32_t *type)
{
    const multiboot_memory_map_t *e = entry_at(mmap_buf, mmap_len, index);

    if (e == NULL) {
        return 0;
    }
    *addr = e->addr;
    *len = e->len;
    *type = e->type;
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

static void recount(struct frame_map *m)
{
    uint32_t i;

    m->usable_bytes = 0;
    m->top_frame = 0;
    for (i = 0; i < m->count; i++) {
        m->usable_bytes += m->usable[i].end - m->usable[i].start;
    }
    if (m->count > 0) {
        m->top_frame =
            (uint32_t)((m->usable[m->count - 1].end + FRAME_PAGE_SIZE - 1) / FRAME_PAGE_SIZE);
    }
}

int frame_map_build(struct frame_map *m, const void *mmap_buf, uint32_t mmap_len,
                    uint32_t mem_upper_kb)
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
        uint64_t end = addr + len;

        if (end < addr) {
            end = ~(uint64_t)0; /* a length that wraps: to the space's last byte */
        }
        if (type != MULTIBOOT_MEMORY_AVAILABLE) {
            continue;
        }
        if (end > FOUR_GIB) {
            m->unreachable_bytes += end - (addr > FOUR_GIB ? addr : FOUR_GIB);
            end = FOUR_GIB;
        }
        if (addr < end && range_add(m, addr, end) != 0) {
            m->count = 0;
            return -1;
        }
    }
    /* Anything of another type wins where it overlaps an available range:
     * the conservative reading of a map that contradicts itself. */
    for (i = 0; frame_map_entry(mmap_buf, mmap_len, i, &addr, &len, &type); i++) {
        uint64_t end = addr + len;

        if (type == MULTIBOOT_MEMORY_AVAILABLE) {
            continue;
        }
        if (end < addr) {
            end = ~(uint64_t)0;
        }
        if (end > FOUR_GIB) {
            end = FOUR_GIB;
        }
        if (range_subtract(m, addr, end, &dropped) != 0) {
            m->count = 0;
            return -1;
        }
    }
    if (m->count == 0 && m->unreachable_bytes == 0) {
        /* No map, or nothing available in it: the older field, as one range
         * from the end of the first megabyte. */
        m->from_mem_upper = 1;
        if (mem_upper_kb > 0) {
            range_add(m, 0x100000ULL, 0x100000ULL + (uint64_t)mem_upper_kb * 1024u);
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

int frame_map_usable(const struct frame_map *m, uint32_t frame)
{
    uint64_t start = (uint64_t)frame * FRAME_PAGE_SIZE;
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
