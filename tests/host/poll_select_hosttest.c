/**
 * Host test of the translation between fd_set words and pollfds behind
 * select(), io/poll_select.c.
 *
 * host-deps: rach2/sys/io/poll_select.c
 *
 * A descriptor's bit is bit fd % 32 of word fd / 32, and select's read,
 * write and exception sets map to the events POLLIN, POLLOUT and POLLPRI.
 * The test runs that arithmetic under the sanitizers: a set bit becomes a
 * pollfd with its events, descriptors on either side of a word boundary land
 * in their words, more selected descriptors than the kernel can hold fail,
 * and the revents map back onto the three sets, counted once for each ready
 * direction as select() counts them.
 */

#include "io/poll_select.h"

static int fails;

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/** Zero the `FDSET_WORDS` words of the set `s`. */
static void zero(uint32_t *s)
{
    int i;
    for (i = 0; i < FDSET_WORDS; i++) {
        s[i] = 0;
    }
}

/** Set the bit of descriptor `fd` in the set `s`. */
static void set_bit(uint32_t *s, int fd)
{
    s[fd / 32] |= 1u << (fd % 32);
}

/** Whether the bit of descriptor `fd` is set in `s`. */
static int get_bit(const uint32_t *s, int fd)
{
    return (s[fd / 32] & (1u << (fd % 32))) != 0;
}

/**
 * Run every check; the exit status is the count of failed checks, at most
 * 255.
 *
 * `fdsets_to_pollfds` makes one pollfd for each descriptor in any set, in
 * ascending order, with the union of its events, and scans the descriptors
 * below `nfds` only. More selected descriptors than its room gives -1, which
 * select() returns as -EBADF.
 *
 * `pollfds_to_fdsets` ORs bits into sets the caller zeroed, and counts each
 * ready direction, so a descriptor ready for reading and writing counts 2.
 * POLLHUP maps to the read set and POLLERR to the read and write sets, each
 * only in a direction the descriptor was selected for. A hangup on a
 * descriptor selected only for writing sets no bit: a bit in the read set
 * would raise the count, and could fall in a set the caller passed as NULL.
 *
 * `pollfd_select_ready` applies the same rule to one descriptor, so that the
 * poll core's test of readiness agrees with what `pollfds_to_fdsets` sets. A
 * descriptor selected for writing or for exceptions that has only POLLHUP is
 * not ready, so select() waits, where it would otherwise return 0 with empty
 * sets in a busy loop.
 */
int main(void)
{
    uint32_t r[FDSET_WORDS], w[FDSET_WORDS], e[FDSET_WORDS];
    struct kernel_pollfd fds[64];
    int n;

    /* fdsets_to_pollfds */

    /* Empty sets produce no pollfds. */
    zero(r);
    zero(w);
    zero(e);
    CHECK(fdsets_to_pollfds(64, r, w, e, fds, 64) == 0);

    /* Three bits in three sets make three pollfds, one event each. */
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

    /* A descriptor in all three sets makes one pollfd with every event. */
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

    /* Descriptor nfds - 1 is scanned and descriptor nfds is not. */
    zero(r);
    zero(w);
    zero(e);
    set_bit(r, 7);
    set_bit(r, 8);
    n = fdsets_to_pollfds(8, r, w, e, fds, 64); /* scans fds 0..7 */
    CHECK(n == 1 && fds[0].fd == 7);

    /* Descriptors 31, 32 and 33, on either side of a word boundary. */
    zero(r);
    zero(w);
    zero(e);
    set_bit(r, 31);
    set_bit(r, 32);
    set_bit(r, 33);
    n = fdsets_to_pollfds(64, r, w, e, fds, 64);
    CHECK(n == 3);
    CHECK(fds[0].fd == 31 && fds[1].fd == 32 && fds[2].fd == 33);

    /* More selected descriptors than `max_pollfds` give -1. */
    zero(r);
    zero(w);
    zero(e);
    set_bit(r, 0);
    set_bit(r, 1);
    set_bit(r, 2);
    CHECK(fdsets_to_pollfds(64, r, w, e, fds, 2) == -1);
    /* As many as `max_pollfds` fit. */
    CHECK(fdsets_to_pollfds(64, r, w, e, fds, 3) == 3);

    /* pollfds_to_fdsets */

    /* POLLIN, POLLOUT and POLLPRI set a bit in their own sets, 1 each. */
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

    /* A descriptor ready for reading and writing counts 2. */
    zero(r);
    zero(w);
    zero(e);
    fds[0].fd = 6;
    fds[0].events = POLLIN | POLLOUT;
    fds[0].revents = POLLIN | POLLOUT;
    CHECK(pollfds_to_fdsets(fds, 1, r, w, e) == 2);
    CHECK(get_bit(r, 6) && get_bit(w, 6));

    /* POLLHUP on a reader counts 1; POLLERR on a reader and writer 2. */
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

    /* POLLHUP on a descriptor selected only for writing sets no bit. */
    zero(r);
    zero(w);
    zero(e);
    fds[0].fd = 5;
    fds[0].events = POLLOUT;
    fds[0].revents = POLLHUP;
    CHECK(pollfds_to_fdsets(fds, 1, r, w, e) == 0);
    CHECK(!get_bit(r, 5) && !get_bit(w, 5));
    /* POLLERR on it sets its write bit and not its read bit. */
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

    /* Sets to pollfds and back, with every pollfd readable. */
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

    /* The same rule for one descriptor. */
    CHECK(pollfd_select_ready(POLLOUT, POLLHUP) == 0); /* HUP does not satisfy write */
    CHECK(pollfd_select_ready(POLLPRI, POLLHUP) == 0); /* nor except */
    CHECK(pollfd_select_ready(POLLIN, POLLHUP) != 0);  /* but HUP reads as readable */
    CHECK(pollfd_select_ready(POLLIN, POLLERR) != 0);  /* ERR reads as readable */
    CHECK(pollfd_select_ready(POLLOUT, POLLERR) != 0); /* and ERR as writable */
    CHECK(pollfd_select_ready(POLLOUT, POLLOUT) != 0); /* the plain writable case */
    CHECK(pollfd_select_ready(POLLPRI, POLLPRI) != 0);
    CHECK(pollfd_select_ready(POLLIN, POLLOUT) == 0); /* wrong direction: not ready */
    CHECK(pollfd_select_ready(POLLIN, 0) == 0);       /* nothing ready */
    /* It agrees with pollfds_to_fdsets on POLLHUP for a writer. */
    zero(r);
    zero(w);
    zero(e);
    fds[0].fd = 7;
    fds[0].events = POLLOUT;
    fds[0].revents = POLLHUP;
    CHECK((pollfds_to_fdsets(fds, 1, r, w, e) > 0) == (pollfd_select_ready(POLLOUT, POLLHUP) != 0));

    return fails > 255 ? 255 : fails;
}
