/*
 * rcvwnd_hosttest.c: the receiver's window-update decision.
 *
 * host-deps: rach2/sys/network/tcp/rcvwnd.c
 *
 * The rule this guards is worth two hundred times the throughput of getting it
 * wrong. Before it existed, a reader drained its ring and told nobody, so a
 * peer that had filled the window waited for its own persist timer before
 * probing again, and bulk receive ran at about a kilobyte a second while
 * transmit ran at two hundred. So the case that matters most below is the
 * plain one: a full buffer, drained, must produce a window.
 *
 * The opposite mistake is announcing after every byte, which spends a segment
 * per byte and draws segments just as small in reply. That is the silly-window
 * syndrome RFC 1122 4.2.3.3 exists to prevent, and its rule is one maximum
 * segment size or half the buffer, whichever is smaller. Which of those two
 * wins depends entirely on the geometry, so the arithmetic is driven here at
 * ring and segment sizes the running kernel never uses.
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

/* The kernel's geometry: a 4096-byte ring and a 1460-byte segment, where one
 * segment is smaller than half the ring (2048) and therefore sets the bar. */
#define RING 4096u
#define MSS  1460u

int main(void)
{
    /* ---- the case the change exists for ---- */

    CHECK(rcvwnd_update(0, 0, RING, MSS) == RING,
          "a buffer drained from full to empty announces the whole ring");
    CHECK(rcvwnd_update(0, RING - MSS, RING, MSS) == MSS,
          "draining exactly one segment announces it");
    CHECK(rcvwnd_update(0, RING - 3000u, RING, MSS) == 3000u,
          "draining past the bar announces the whole new window, not the bar");

    /* ---- the quiet cases: silly-window avoidance ---- */

    CHECK(rcvwnd_update(0, RING, RING, MSS) == 0, "a full buffer has nothing to announce");
    CHECK(rcvwnd_update(0, RING - 1u, RING, MSS) == 0, "one byte read is not worth a segment");
    CHECK(rcvwnd_update(0, RING - (MSS - 1u), RING, MSS) == 0,
          "one byte short of a segment stays quiet");
    CHECK(rcvwnd_update(2048, RING - 2048u, RING, MSS) == 0,
          "a window already advertised is not advertised again");
    CHECK(rcvwnd_update(RING, 0, RING, MSS) == 0,
          "an empty buffer the peer already knows about stays quiet");
    CHECK(rcvwnd_update(3000, RING - 3500u, RING, MSS) == 0,
          "growth below the bar stays quiet even from a large window");

    /* ---- which half of the rule wins follows the geometry ---- */

    /* On a large ring one segment is far smaller than half the buffer, so the
     * segment sets the bar; a kernel that always used half would stay quiet
     * here and lose the throughput this test exists to keep. */
    CHECK(rcvwnd_update(0, 32768u - MSS, 32768u, MSS) == MSS,
          "on a large ring one segment is the bar, not half the buffer");
    CHECK(rcvwnd_update(0, 32768u - (MSS - 1u), 32768u, MSS) == 0,
          "one byte short of a segment stays quiet on a large ring");

    /* On a tiny ring half the buffer is smaller and wins. */
    CHECK(rcvwnd_update(0, 0, 8u, MSS) == 8u,
          "on a tiny ring half the buffer is the bar and a full drain clears it");
    CHECK(rcvwnd_update(0, 6u, 8u, MSS) == 0,
          "on a tiny ring a two-byte gain is below half and stays quiet");

    /* ---- the window is a 16-bit field ---- */

    /* A ring at or past the 16-bit limit must report the largest window that
     * fits, not the low bits of the real one. Truncating 65536 would yield 0,
     * which this function uses to mean "stay quiet", so a fully drained buffer
     * would announce nothing at all. */
    CHECK(rcvwnd_update(0, 0, 65536u, MSS) == 65535u,
          "a ring at the 16-bit limit announces 65535, not a truncated zero");
    CHECK(rcvwnd_update(0, 0, 70000u, MSS) == 65535u,
          "a ring past the limit is clamped, not wrapped to its low bits");
    CHECK(rcvwnd_update(65535u, 0, 65536u, MSS) == 0,
          "a clamped window already advertised is not advertised again");

    /* ---- refusals, not wraps ---- */

    CHECK(rcvwnd_update(0, 1u, 0u, MSS) == 0,
          "a zero ring announces nothing instead of underflowing");
    CHECK(rcvwnd_update(0, RING + 1u, RING, MSS) == 0,
          "more buffered than the ring has announces nothing instead of wrapping");
    CHECK(rcvwnd_update(0, 0, 1u, 0) == 1u,
          "a zero segment size still makes progress instead of dividing by nothing");

    return fails;
}
