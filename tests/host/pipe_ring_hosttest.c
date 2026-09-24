/**
 * Dual-target host test for the pipe byte-FIFO ring (ipc/pipe_ring.c).
 *
 * host-deps: rach2/sys/ipc/pipe_ring.c
 *
 * The wrap arithmetic (occupancy = tail - head, slot = pos & (SIZE-1)) is the
 * fence-post logic a one-off bug hides in, so it runs natively under
 * ASan/UBSan: put never overruns free space, get never reads past occupancy, a
 * transfer that straddles the physical end of the buffer stays byte-correct,
 * and a full page round-trips. Returns the failed-check count (0 == pass).
 */

extern int printf(const char *fmt, ...);

#include "ipc/pipe.h"

static int fails;

#define CHECK(cond)                                           \
    do {                                                      \
        if (!(cond)) {                                        \
            printf("  FAIL: line %d: %s\n", __LINE__, #cond); \
            fails++;                                          \
        }                                                     \
    } while (0)

int main(void)
{
    pipe_t p;
    unsigned char src[8192];
    unsigned char dst[8192];
    unsigned int i, got;

    for (i = 0; i < sizeof(src); i++) {
        src[i] = (unsigned char)(i & 0xff);
    }

    /* Empty ring. */
    p.head = 0;
    p.tail = 0;
    CHECK(pipe_available(&p) == 0);
    CHECK(pipe_free_space(&p) == PIPE_BUF_SIZE);

    /* A put is bounded by free space; a full page fits exactly (no lost
     * slot). */
    CHECK(pipe_ring_put(&p, src, PIPE_BUF_SIZE) == PIPE_BUF_SIZE);
    CHECK(pipe_available(&p) == PIPE_BUF_SIZE);
    CHECK(pipe_free_space(&p) == 0);
    CHECK(pipe_ring_put(&p, src, 1) == 0); /* full: nothing more accepted */

    /* A get is bounded by occupancy and returns the bytes in order. */
    got = pipe_ring_get(&p, dst, PIPE_BUF_SIZE);
    CHECK(got == PIPE_BUF_SIZE);
    for (i = 0; i < PIPE_BUF_SIZE; i++) {
        CHECK(dst[i] == (unsigned char)(i & 0xff));
    }
    CHECK(pipe_available(&p) == 0);
    CHECK(pipe_ring_get(&p, dst, 1) == 0); /* empty: nothing to read */

    /* Position the ring so the next transfer straddles the physical wrap point,
     * then verify a put/get across the wrap stays byte-correct. */
    p.head = PIPE_BUF_SIZE - 10;
    p.tail = PIPE_BUF_SIZE - 10;
    CHECK(pipe_ring_put(&p, src, 100) == 100); /* spans the [.. wrap ..] boundary */
    CHECK(pipe_available(&p) == 100);
    got = pipe_ring_get(&p, dst, 100);
    CHECK(got == 100);
    for (i = 0; i < 100; i++) {
        CHECK(dst[i] == (unsigned char)(i & 0xff));
    }

    /* A partial put (more than free space) clamps to free space. */
    p.head = 0;
    p.tail = PIPE_BUF_SIZE - 50; /* 50 bytes free */
    CHECK(pipe_free_space(&p) == 50);
    CHECK(pipe_ring_put(&p, src, 200) == 50);
    CHECK(pipe_free_space(&p) == 0);

    /* The spans: what a caller that copies straight from and into the ring
       is given. An empty ring has no read span and a whole-page write
       span; a full ring whose head is past the array's start gives its
       bytes in two read spans, neither crossing the end; produce and
       consume advance as put and get do. */
    {
        const unsigned char *rp;
        unsigned char *wp;
        unsigned int n;

        p.head = 0;
        p.tail = 0;
        CHECK(pipe_ring_read_span(&p, &rp) == 0);
        CHECK(pipe_ring_write_span(&p, &wp) == PIPE_BUF_SIZE && wp == p.buf);
        /* A full ring starting at slot 100. */
        p.head = 100;
        p.tail = 100 + PIPE_BUF_SIZE;
        for (i = 0; i < PIPE_BUF_SIZE; i++) {
            p.buf[(100 + i) & (PIPE_BUF_SIZE - 1)] = (unsigned char)(i & 0xff);
        }
        CHECK(pipe_ring_write_span(&p, &wp) == 0);
        n = pipe_ring_read_span(&p, &rp);
        CHECK(n == PIPE_BUF_SIZE - 100 && rp == &p.buf[100]);
        CHECK(rp[0] == 0 && rp[n - 1] == (unsigned char)((n - 1) & 0xff));
        pipe_ring_consume(&p, n);
        CHECK(pipe_available(&p) == 100);
        n = pipe_ring_read_span(&p, &rp);
        CHECK(n == 100 && rp == p.buf);
        CHECK(rp[0] == (unsigned char)((PIPE_BUF_SIZE - 100) & 0xff));
        pipe_ring_consume(&p, n);
        CHECK(pipe_available(&p) == 0 && pipe_free_space(&p) == PIPE_BUF_SIZE);
        /* Writing across the end: the tail at slot PIPE_BUF_SIZE - 10 gives
           a ten-byte span, then a span from the array's start. */
        p.head = PIPE_BUF_SIZE - 10;
        p.tail = PIPE_BUF_SIZE - 10;
        n = pipe_ring_write_span(&p, &wp);
        CHECK(n == 10 && wp == &p.buf[PIPE_BUF_SIZE - 10]);
        pipe_ring_produce(&p, n);
        n = pipe_ring_write_span(&p, &wp);
        CHECK(n == PIPE_BUF_SIZE - 10 && wp == p.buf);
        pipe_ring_produce(&p, 30);
        CHECK(pipe_available(&p) == 40);
        n = pipe_ring_read_span(&p, &rp);
        CHECK(n == 10 && rp == &p.buf[PIPE_BUF_SIZE - 10]);
        pipe_ring_consume(&p, 10);
        n = pipe_ring_read_span(&p, &rp);
        CHECK(n == 30 && rp == p.buf);
        /* The byte forms and the spans agree. */
        p.head = 0;
        p.tail = 0;
        CHECK(pipe_ring_put(&p, src, 4000) == 4000);
        CHECK(pipe_ring_get(&p, dst, 3000) == 3000);
        CHECK(pipe_ring_put(&p, src, 2000) == 2000);
        n = pipe_ring_read_span(&p, &rp);
        CHECK(n == 1096 && rp == &p.buf[3000]);
        pipe_ring_consume(&p, n);
        n = pipe_ring_read_span(&p, &rp);
        CHECK(n == 1904 && rp == p.buf && rp[0] == src[96]);
    }

    return fails > 255 ? 255 : fails;
}
