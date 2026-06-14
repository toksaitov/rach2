/*
 * poll_select_hosttest.c
 *
 * host-deps: rach2/sys/io/poll_select.c
 *
 * Dual-target seam test for the fd_set <-> pollfd translation behind select()
 * (io/poll_select.c). The bit/word arithmetic (word = fd/32, bit = 1u<<(fd%32))
 * and the readable/writable/except event projection are the fence-post-prone
 * part select() is built on, so they run natively here under ASan/UBSan: a set
 * bit becomes a pollfd with the right events, fds that straddle a 32-bit word
 * boundary land in the correct word, an fd selected past what the kernel can
 * hold is rejected, and revents project back onto the three sets with select()'s
 * count-each-ready-direction rule. Returns the failed-check count (0 == pass).
 */

#include "io/poll_select.h"

static int fails;

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/* Zero a full fd_set worth of words. */
static void zero(uint32_t *s)
{
    int i;
    for (i = 0; i < FDSET_WORDS; i++) {
        s[i] = 0;
    }
}

static void set_bit(uint32_t *s, int fd)
{
    s[fd / 32] |= 1u << (fd % 32);
}

static int get_bit(const uint32_t *s, int fd)
{
    return (s[fd / 32] & (1u << (fd % 32))) != 0;
}

int main(void)
{
    uint32_t r[FDSET_WORDS], w[FDSET_WORDS], e[FDSET_WORDS];
    struct kernel_pollfd fds[64];
    int n;

    /* --- fdsets_to_pollfds --- */

    /* Empty sets produce no pollfds. */
    zero(r);
    zero(w);
    zero(e);
    CHECK(fdsets_to_pollfds(64, r, w, e, fds, 64) == 0);

    /* A read bit, a write bit, and an except bit on three distinct fds become
     * three pollfds carrying exactly the matching event, in ascending fd order
     * (only fds set in some set appear; the gaps are skipped). */
    zero(r);
    zero(w);
    zero(e);
    set_bit(r, 1);
    set_bit(w, 4);
    set_bit(e, 9);
    n = fdsets_to_pollfds(10, r, w, e, fds, 64);
    CHECK(n == 3);
    CHECK(fds[0].fd == 1 && fds[0].events == POLLIN && fds[0].revents == 0);
    CHECK(fds[1].fd == 4 && fds[1].events == POLLOUT);
    CHECK(fds[2].fd == 9 && fds[2].events == POLLPRI);

    /* The same fd in all three sets folds into one pollfd with the union. */
    zero(r);
    zero(w);
    zero(e);
    set_bit(r, 5);
    set_bit(w, 5);
    set_bit(e, 5);
    n = fdsets_to_pollfds(6, r, w, e, fds, 64);
    CHECK(n == 1);
    CHECK(fds[0].fd == 5);
    CHECK(fds[0].events == (POLLIN | POLLOUT | POLLPRI));

    /* nfds is a half-open bound: fd == nfds-1 is in, fd == nfds is out. */
    zero(r);
    zero(w);
    zero(e);
    set_bit(r, 7);
    set_bit(r, 8);
    n = fdsets_to_pollfds(8, r, w, e, fds, 64); /* scans fds 0..7 */
    CHECK(n == 1 && fds[0].fd == 7);

    /* Word-boundary fds (31 = top bit of word 0, 32 = bit 0 of word 1, 33) each
     * resolve to the right word -- the classic off-by-one in fd/32, fd%32. */
    zero(r);
    zero(w);
    zero(e);
    set_bit(r, 31);
    set_bit(r, 32);
    set_bit(r, 33);
    n = fdsets_to_pollfds(64, r, w, e, fds, 64);
    CHECK(n == 3);
    CHECK(fds[0].fd == 31 && fds[1].fd == 32 && fds[2].fd == 33);

    /* More selected fds than max_pollfds -> -1 (select must reject as EBADF an fd
     * larger than the kernel could ever hand out). */
    zero(r);
    zero(w);
    zero(e);
    set_bit(r, 0);
    set_bit(r, 1);
    set_bit(r, 2);
    CHECK(fdsets_to_pollfds(64, r, w, e, fds, 2) == -1);
    /* Exactly max_pollfds fits (boundary, not past it). */
    CHECK(fdsets_to_pollfds(64, r, w, e, fds, 3) == 3);

    /* --- pollfds_to_fdsets --- */

    /* POLLIN -> readable, POLLOUT -> writable, POLLPRI -> except; each counts 1.
     * The destination sets must be pre-zeroed (the projection ORs bits in). */
    zero(r);
    zero(w);
    zero(e);
    fds[0].fd = 2;
    fds[0].events = POLLIN;
    fds[0].revents = POLLIN;
    fds[1].fd = 3;
    fds[1].events = POLLOUT;
    fds[1].revents = POLLOUT;
    fds[2].fd = 4;
    fds[2].events = POLLPRI;
    fds[2].revents = POLLPRI;
    CHECK(pollfds_to_fdsets(fds, 3, r, w, e) == 3);
    CHECK(get_bit(r, 2) && !get_bit(w, 2) && !get_bit(e, 2));
    CHECK(get_bit(w, 3) && !get_bit(r, 3));
    CHECK(get_bit(e, 4) && !get_bit(r, 4));

    /* An fd selected for both read and write that is ready for both contributes 2
     * to the count -- select() counts each ready direction, not each fd. */
    zero(r);
    zero(w);
    zero(e);
    fds[0].fd = 6;
    fds[0].events = POLLIN | POLLOUT;
    fds[0].revents = POLLIN | POLLOUT;
    CHECK(pollfds_to_fdsets(fds, 1, r, w, e) == 2);
    CHECK(get_bit(r, 6) && get_bit(w, 6));

    /* POLLHUP projects to readable; POLLERR to both readable and writable -- but
     * only for the direction(s) the fd was selected for. fd 1 was in readfds (HUP
     * -> read, 1); fd 2 was in both (ERR -> read + write, 2). */
    zero(r);
    zero(w);
    zero(e);
    fds[0].fd = 1;
    fds[0].events = POLLIN;
    fds[0].revents = POLLHUP;
    fds[1].fd = 2;
    fds[1].events = POLLIN | POLLOUT;
    fds[1].revents = POLLERR;
    CHECK(pollfds_to_fdsets(fds, 2, r, w, e) == 3); /* HUP:1 + ERR:2 */
    CHECK(get_bit(r, 1) && !get_bit(w, 1));
    CHECK(get_bit(r, 2) && get_bit(w, 2));

    /* Direction gating (the crux): a hangup on an fd selected ONLY for write must
     * not be invented into the read set -- that would inflate the count and set a
     * bit in a set the caller may have passed as NULL. POLLHUP is not in the write
     * trigger, so a write-only fd reporting only POLLHUP yields nothing. */
    zero(r);
    zero(w);
    zero(e);
    fds[0].fd = 5;
    fds[0].events = POLLOUT;
    fds[0].revents = POLLHUP;
    CHECK(pollfds_to_fdsets(fds, 1, r, w, e) == 0);
    CHECK(!get_bit(r, 5) && !get_bit(w, 5));
    /* POLLERR on the same write-only fd DOES project to write (ERR is in the write
     * trigger) and still not to read. */
    zero(r);
    zero(w);
    zero(e);
    fds[0].fd = 5;
    fds[0].events = POLLOUT;
    fds[0].revents = POLLERR;
    CHECK(pollfds_to_fdsets(fds, 1, r, w, e) == 1);
    CHECK(get_bit(w, 5) && !get_bit(r, 5));

    /* A pollfd with no ready bits contributes nothing. */
    zero(r);
    zero(w);
    zero(e);
    fds[0].fd = 0;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    CHECK(pollfds_to_fdsets(fds, 1, r, w, e) == 0);
    CHECK(!get_bit(r, 0) && !get_bit(w, 0) && !get_bit(e, 0));

    /* Round-trip: build read-sets, translate out, mark each pollfd POLLIN, project
     * back -- the same fds (including the word-boundary ones) come back readable. */
    zero(r);
    zero(w);
    zero(e);
    set_bit(r, 0);
    set_bit(r, 31);
    set_bit(r, 32);
    n = fdsets_to_pollfds(64, r, w, e, fds, 64);
    CHECK(n == 3);
    {
        int i;
        uint32_t r2[FDSET_WORDS], w2[FDSET_WORDS], e2[FDSET_WORDS];
        for (i = 0; i < n; i++) {
            fds[i].revents = POLLIN;
        }
        zero(r2);
        zero(w2);
        zero(e2);
        CHECK(pollfds_to_fdsets(fds, n, r2, w2, e2) == 3);
        CHECK(get_bit(r2, 0) && get_bit(r2, 31) && get_bit(r2, 32));
    }

    return fails;
}
