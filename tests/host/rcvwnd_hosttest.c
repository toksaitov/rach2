/*
 * rcvwnd_hosttest.c: the TCP receiver's decision to announce a window that
 * reopened, network/tcp/rcvwnd.c.
 *
 * host-deps: rach2/sys/network/tcp/rcvwnd.c
 *
 * A reader that drains a full receive ring must announce the window that
 * reopened. Without the announcement a peer that filled the window waits for
 * its persist timer before it probes again, and a bulk transfer to the
 * receiver stops for that time at every full window. The first rows check
 * that case.
 *
 * Announcing after every byte read spends a segment for each byte and draws
 * segments as small in reply, the silly-window syndrome. RFC 1122 4.2.3.3
 * waits until the window has grown by one maximum segment size or half the
 * buffer, whichever is smaller. Which of the two is smaller depends on the
 * sizes of the ring and the segment, so the rows drive the arithmetic at
 * rings from 8 bytes to 512 KiB.
 */

extern int printf(const char *fmt, ...);

#include "../../rach2/sys/network/tcp/rcvwnd.h"

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

/* A 4096-byte ring and a 1460-byte segment, which is smaller than half the
 * ring (2048) and so is the threshold. */
#define RING 4096u
#define MSS  1460u

/**
 * Run every row; the exit status is the count of failed rows, at most 255.
 *
 * The window field has 16 bits, and a ring at or past that limit announces
 * the largest window that fits. Its low 16 bits would not do: for a ring of
 * 65536 they are 0, which `rcvwnd_update` returns to mean "send nothing", and
 * a drained ring would announce nothing.
 */
int main(void)
{
    /* ---- a drained ring announces its window ---- */

    CHECK(rcvwnd_update(0, 0, RING, 65535u, MSS) == RING,
          "a buffer drained from full to empty announces the whole ring");
    CHECK(rcvwnd_update(0, RING - MSS, RING, 65535u, MSS) == MSS,
          "draining exactly one segment announces it");
    CHECK(rcvwnd_update(0, RING - 3000u, RING, 65535u, MSS) == 3000u,
          "draining past the bar announces the whole new window, not the bar");

    /* ---- the quiet cases: silly-window avoidance ---- */

    CHECK(rcvwnd_update(0, RING, RING, 65535u, MSS) == 0, "a full buffer has nothing to announce");
    CHECK(rcvwnd_update(0, RING - 1u, RING, 65535u, MSS) == 0,
          "one byte read is not worth a segment");
    CHECK(rcvwnd_update(0, RING - (MSS - 1u), RING, 65535u, MSS) == 0,
          "one byte short of a segment stays quiet");
    CHECK(rcvwnd_update(2048, RING - 2048u, RING, 65535u, MSS) == 0,
          "a window already advertised is not advertised again");
    CHECK(rcvwnd_update(RING, 0, RING, 65535u, MSS) == 0,
          "an empty buffer the peer already knows about stays quiet");
    CHECK(rcvwnd_update(3000, RING - 3500u, RING, 65535u, MSS) == 0,
          "growth below the bar stays quiet even from a large window");

    /* ---- which half of the rule wins follows the geometry ---- */

    /* On a large ring one segment is smaller than half: the threshold. */
    CHECK(rcvwnd_update(0, 32768u - MSS, 32768u, 65535u, MSS) == MSS,
          "on a large ring one segment is the bar, not half the buffer");
    CHECK(rcvwnd_update(0, 32768u - (MSS - 1u), 32768u, 65535u, MSS) == 0,
          "one byte short of a segment stays quiet on a large ring");

    /* On a tiny ring half the buffer is smaller and wins. */
    CHECK(rcvwnd_update(0, 0, 8u, 65535u, MSS) == 8u,
          "on a tiny ring half the buffer is the bar and a full drain clears it");
    CHECK(rcvwnd_update(0, 6u, 8u, 65535u, MSS) == 0,
          "on a tiny ring a two-byte gain is below half and stays quiet");

    /* ---- the window is a 16-bit field ---- */

    CHECK(rcvwnd_update(0, 0, 65536u, 65535u, MSS) == 65535u,
          "a ring at the 16-bit limit announces 65535, not a truncated zero");
    CHECK(rcvwnd_update(0, 0, 70000u, 65535u, MSS) == 65535u,
          "a ring past the limit is clamped, not wrapped to its low bits");
    CHECK(rcvwnd_update(65535u, 0, 65536u, 65535u, MSS) == 0,
          "a clamped window already advertised is not advertised again");

    /* ---- sizes that would wrap give 0; the window scale ---- */

    CHECK(rcvwnd_update(0, 1u, 0u, 65535u, MSS) == 0,
          "a zero ring announces nothing instead of underflowing");
    CHECK(rcvwnd_update(0, RING + 1u, RING, 65535u, MSS) == 0,
          "more buffered than the ring has announces nothing instead of wrapping");
    CHECK(rcvwnd_update(0, 0, 524288u, 65535u << 4, MSS) == 524288u,
          "a scaled window offers the whole ring, past what the field says unscaled");
    CHECK(rcvwnd_update(0, 0, 524288u, 65535u << 3, MSS) == 65535u << 3,
          "and no more than the field says at the scale");
    CHECK(rcvwnd_update(0, 0, 1u, 65535u, 0) == 1u,
          "a zero segment size still makes progress instead of dividing by nothing");

    return fails > 255 ? 255 : fails;
}
