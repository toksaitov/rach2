/*
 * pipe_ring.c: the ring of bytes under a pipe, whose operations pipe.h
 * declares. It has no kernel dependency, so tests/host/pipe_ring_hosttest.c
 * links it under the sanitizers.
 *
 * `head` and `tail` count the bytes consumed and produced since the pipe was
 * made. The count buffered is `tail - head`, right across the counters' wrap
 * since the subtraction is unsigned, and a count masked by
 * `PIPE_BUF_SIZE - 1` is a slot.
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

uint32_t pipe_ring_read_span(pipe_t *p, const uint8_t **at)
{
    uint32_t slot = p->head & (PIPE_BUF_SIZE - 1);
    uint32_t avail = pipe_available(p);
    uint32_t to_end = PIPE_BUF_SIZE - slot;

    *at = &p->buf[slot];
    return avail < to_end ? avail : to_end;
}

uint32_t pipe_ring_write_span(pipe_t *p, uint8_t **at)
{
    uint32_t slot = p->tail & (PIPE_BUF_SIZE - 1);
    uint32_t space = pipe_free_space(p);
    uint32_t to_end = PIPE_BUF_SIZE - slot;

    *at = &p->buf[slot];
    return space < to_end ? space : to_end;
}

void pipe_ring_consume(pipe_t *p, uint32_t n)
{
    p->head += n;
}

void pipe_ring_produce(pipe_t *p, uint32_t n)
{
    p->tail += n;
}
