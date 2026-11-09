/*
 * fence.c: fences, the points on a timeline that work signals, with their
 * callbacks and waits. A stub until 76-3a writes it, which gives timeline
 * contexts and nothing more.
 */

#include "../drm/drm.h"

uint64_t dma_fence_context_alloc(uint32_t num)
{
    static uint64_t next = 1;
    uint64_t first = next;

    next += num;
    return first;
}
