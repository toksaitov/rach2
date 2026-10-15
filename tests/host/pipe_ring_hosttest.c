/**
 * Host test of the pipe's byte ring, ipc/pipe_ring.c.
 *
 * host-deps: rach2/sys/ipc/pipe_ring.c
 *
 * The ring's count of bytes is tail - head, and a byte's slot is its
 * position & (PIPE_BUF_SIZE - 1). The test runs that arithmetic under the
 * sanitizers: a put never writes past the free space, a get never reads past
 * the bytes in the ring, a transfer across the end of the buffer keeps every
 * byte, and a full ring gives back all it holds.
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

static unsigned char ring[PIPE_BUF_SIZE];
static unsigned char src[2 * PIPE_BUF_SIZE];
static unsigned char dst[2 * PIPE_BUF_SIZE];

/**
 * Run every check; the exit status is the count of failed checks, at most
 * 255.
 *
 * A caller that copies straight from and into the ring is given spans: an
 * empty ring has no read span and one write span of the whole buffer; a full
 * ring whose head is past the buffer's start gives its bytes in two read
 * spans, neither crossing the end; and `pipe_ring_produce` and
 * `pipe_ring_consume` advance the ring as a put and a get do.
 */
int main(void)
{
    pipe_t p;
    unsigned int i, got;

    for (i = 0; i < sizeof(src); i++) {
        src[i] = (unsigned char)(i & 0xff);
    }

    /* Empty ring. */
    p.buf = ring;
    p.head = 0;
    p.tail = 0;
    CHECK(pipe_available(&p) == 0);
    CHECK(pipe_free_space(&p) == PIPE_BUF_SIZE);

    /* A put is bounded by the free space; a full ring uses every slot. */
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

    /* A put and a get across the end of the buffer keep every byte. */
    p.head = PIPE_BUF_SIZE - 10;
    p.tail = PIPE_BUF_SIZE - 10;
    CHECK(pipe_ring_put(&p, src, 100) == 100); /* across the buffer's end */
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

    /* The spans. */
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
        /* A write span stops at the buffer's end; the next starts at 0. */
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
        CHECK(pipe_ring_put(&p, src, PIPE_BUF_SIZE - 96) == PIPE_BUF_SIZE - 96);
        CHECK(pipe_ring_get(&p, dst, PIPE_BUF_SIZE - 1096) == PIPE_BUF_SIZE - 1096);
        CHECK(pipe_ring_put(&p, src, 2000) == 2000);
        n = pipe_ring_read_span(&p, &rp);
        CHECK(n == 1096 && rp == &p.buf[PIPE_BUF_SIZE - 1096]);
        pipe_ring_consume(&p, n);
        n = pipe_ring_read_span(&p, &rp);
        CHECK(n == 1904 && rp == p.buf && rp[0] == src[96]);
    }

    return fails > 255 ? 255 : fails;
}
