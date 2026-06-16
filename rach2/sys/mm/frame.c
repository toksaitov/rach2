#include "frame.h"

/**
 * Return the highest available physical address in the multiboot memory map.
 *
 * Each entry's `size` field gives the bytes that follow it, so the next entry
 * is at `p + size + sizeof(size)`. The walk is defensive: it stops on a short
 * or non-advancing entry rather than read past the buffer or loop forever.
 * Returns 0 when `mmap_buf` is NULL or no AVAILABLE region is found.
 */
uint64_t frame_top_available(const void *mmap_buf, uint32_t mmap_len)
{
    const uint8_t *p;
    const uint8_t *end;
    uint64_t top = 0;

    if (mmap_buf == NULL)
    {
        return 0;
    }

    p = (const uint8_t *)mmap_buf;
    end = p + mmap_len;

    while (p + sizeof(multiboot_uint32_t) <= end)
    {
        const multiboot_memory_map_t *e = (const multiboot_memory_map_t *)p;
        uint32_t advance = e->size + (uint32_t)sizeof(e->size);

        if (advance < (uint32_t)sizeof(multiboot_memory_map_t) ||
            (const uint8_t *)e + advance > end)
        {
            break;
        }

        if (e->type == MULTIBOOT_MEMORY_AVAILABLE)
        {
            uint64_t e_end = e->addr + e->len;

            if (e_end > top)
            {
                top = e_end;
            }
        }

        p += advance;
    }

    return top;
}

/**
 * Return the number of usable physical page frames reported by the bootloader.
 *
 * Prefers the multiboot memory map; falls back to the legacy `mem_upper`
 * field when no map is present. The count is capped to the direct-mapped
 * window so the frame allocator never hands out a page the kernel cannot
 * address, and never exceeds real RAM.
 */
size_t frame_usable_count(const multiboot_info_t *info)
{
    uint64_t top = 0;

    if (info != NULL && (info->flags & MULTIBOOT_INFO_MEM_MAP))
    {
        top = frame_top_available((const void *)(uintptr_t)info->mmap_addr,
                                  info->mmap_length);
    }
    if (top == 0 && info != NULL && (info->flags & MULTIBOOT_INFO_MEMORY))
    {
        /* Fallback: mem_upper is KB of RAM above the 1 MB mark. */
        top = (uint64_t)0x100000u + (uint64_t)info->mem_upper * 1024u;
    }

    /* Cap to the direct-mapped window (and never hand out beyond real RAM). */
    if (top == 0 || top > (uint64_t)FRAME_DIRECT_MAP_LIMIT)
    {
        top = (uint64_t)FRAME_DIRECT_MAP_LIMIT;
    }

    return (size_t)(top / FRAME_PAGE_SIZE);
}
