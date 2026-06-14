/*
 * poll_select.c - the pure fd_set <-> pollfd translation behind select().
 *
 * Kept free of kernel dependencies so tests/host/poll_select_hosttest.c can run
 * the bit/index arithmetic natively under ASan/UBSan.
 */

#include "poll_select.h"

int fdsets_to_pollfds(uint32_t nfds, const uint32_t *r, const uint32_t *w,
                      const uint32_t *e, struct kernel_pollfd *fds, int max_pollfds)
{
    uint32_t fd;
    int n = 0;

    for (fd = 0; fd < nfds; fd++) {
        uint32_t word = fd / 32;
        uint32_t bit = 1u << (fd % 32);
        short ev = 0;

        if (r[word] & bit) {
            ev |= POLLIN;
        }
        if (w[word] & bit) {
            ev |= POLLOUT;
        }
        if (e[word] & bit) {
            ev |= POLLPRI;
        }
        if (!ev) {
            continue;
        }
        if (n >= max_pollfds) {
            return -1; /* more selected fds than the kernel can have */
        }
        fds[n].fd = (int)fd;
        fds[n].events = ev;
        fds[n].revents = 0;
        n++;
    }
    return n;
}

int pollfds_to_fdsets(const struct kernel_pollfd *fds, int n, uint32_t *r,
                      uint32_t *w, uint32_t *e)
{
    int i;
    int count = 0;

    for (i = 0; i < n; i++) {
        uint32_t fd = (uint32_t)fds[i].fd;
        uint32_t word = fd / 32;
        uint32_t bit = 1u << (fd % 32);
        short ev = fds[i].events;
        short re = fds[i].revents;

        /* Project only onto the direction(s) the caller actually selected. The
         * requested set membership is carried in .events by fdsets_to_pollfds
         * (POLLIN<->readfds, POLLOUT<->writefds, POLLPRI<->exceptfds). As in
         * select()'s POLLIN_SET/POLLOUT_SET rule, POLLHUP/POLLERR count as
         * readable and POLLERR also as writable -- but ONLY for an fd that was in
         * that set, so a hangup on a write-only-selected fd is not invented into a
         * read set the caller may have passed as NULL. Each bit set counts once, so
         * an fd ready for both read and write contributes 2 to select()'s return. */
        if ((ev & POLLIN) && (re & (POLLIN | POLLHUP | POLLERR))) {
            r[word] |= bit;
            count++;
        }
        if ((ev & POLLOUT) && (re & (POLLOUT | POLLERR))) {
            w[word] |= bit;
            count++;
        }
        if ((ev & POLLPRI) && (re & POLLPRI)) {
            e[word] |= bit;
            count++;
        }
    }
    return count;
}
