/*
 * pipe_ring.c - the pure byte-FIFO ring under a pipe (host-tested).
 *
 * A power-of-two ring with monotonically increasing head/tail counters:
 * occupancy is `tail - head` (correct across wrap because the subtraction is
 * unsigned), and the physical slot for a logical position is `pos &
 * (SIZE-1)`. Kept free of any kernel dependency so
 * tests/host/pipe_ring_hosttest.c can link it natively under ASan/UBSan: the
 * wrap arithmetic is exactly the kind of fence-post logic that is murder to
 * debug in-guest.
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

/**
 * Copy up to `n` bytes from `src` into the ring, returning the count stored.
 *
 * The write is bounded by the free space, so it stores `min(n, free)` bytes and
 * never overwrites unread data; a short return means the ring filled. Advances
 * `tail` by the count, which is what makes the bytes visible to a reader.
 *
 * @return the number of bytes actually stored, in `[0, n]`.
 */
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

/**
 * Copy up to `n` bytes from the ring into `dst`, returning the count removed.
 *
 * The read is bounded by the available bytes, so it removes `min(n, avail)`; a
 * short return means the ring drained. Advances `head` by the count, which is
 * what frees the slots for a writer.
 *
 * @return the number of bytes actually copied out, in `[0, n]`.
 */
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

/**
 * The readable bytes from the head that lie before the ring's end: their
 * address and count, which is `min(available, SIZE - head's slot)`. A
 * caller that copies them straight out then calls `pipe_ring_consume`;
 * the bytes after the wrap are the next span.
 *
 * @return the span's length, 0 on an empty ring.
 */
uint32_t pipe_ring_read_span(pipe_t *p, const uint8_t **at)
{
    uint32_t slot = p->head & (PIPE_BUF_SIZE - 1);
    uint32_t avail = pipe_available(p);
    uint32_t to_end = PIPE_BUF_SIZE - slot;

    *at = &p->buf[slot];
    return avail < to_end ? avail : to_end;
}

/**
 * The free slots from the tail that lie before the ring's end, the same
 * way: a caller that copies straight in then calls `pipe_ring_produce`.
 *
 * @return the span's length, 0 on a full ring.
 */
uint32_t pipe_ring_write_span(pipe_t *p, uint8_t **at)
{
    uint32_t slot = p->tail & (PIPE_BUF_SIZE - 1);
    uint32_t space = pipe_free_space(p);
    uint32_t to_end = PIPE_BUF_SIZE - slot;

    *at = &p->buf[slot];
    return space < to_end ? space : to_end;
}

/** Remove `n` bytes the caller copied out of a read span. */
void pipe_ring_consume(pipe_t *p, uint32_t n)
{
    p->head += n;
}

/** Make `n` bytes the caller copied into a write span visible. */
void pipe_ring_produce(pipe_t *p, uint32_t n)
{
    p->tail += n;
}
