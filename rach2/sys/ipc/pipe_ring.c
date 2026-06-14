/*
 * pipe_ring.c - the pure byte-FIFO ring under a pipe (host-tested).
 *
 * A power-of-two ring with monotonically increasing head/tail counters:
 * occupancy is `tail - head` (correct across wrap because the subtraction is
 * unsigned), and the physical slot for a logical position is `pos & (SIZE-1)`.
 * Kept free of any kernel dependency so tests/host/pipe_ring_hosttest.c can link
 * it natively under ASan/UBSan -- the wrap arithmetic is exactly the kind of
 * fence-post logic that is murder to debug in-guest.
 */

#include "pipe.h"

uint32_t pipe_available(const pipe_t *p)
{
    return p->tail - p->head;
}

uint32_t pipe_free_space(const pipe_t *p)
{
    return PIPE_BUF_SIZE - (p->tail - p->head);
}

uint32_t pipe_ring_put(pipe_t *p, const uint8_t *src, uint32_t n)
{
    uint32_t space = pipe_free_space(p);
    uint32_t cnt = (n < space) ? n : space;
    uint32_t i;

    for (i = 0; i < cnt; i++) {
        p->buf[(p->tail + i) & (PIPE_BUF_SIZE - 1)] = src[i];
    }
    p->tail += cnt;
    return cnt;
}

uint32_t pipe_ring_get(pipe_t *p, uint8_t *dst, uint32_t n)
{
    uint32_t avail = pipe_available(p);
    uint32_t cnt = (n < avail) ? n : avail;
    uint32_t i;

    for (i = 0; i < cnt; i++) {
        dst[i] = p->buf[(p->head + i) & (PIPE_BUF_SIZE - 1)];
    }
    p->head += cnt;
    return cnt;
}
