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

#include "ipc/pipe.h"

static int fails;

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
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

    return fails;
}
