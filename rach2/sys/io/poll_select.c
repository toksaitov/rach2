/*
 * poll_select.c - the pure fd_set <-> pollfd translation behind select().
 *
 * Kept free of kernel dependencies so tests/host/poll_select_hosttest.c can run
 * the bit/index arithmetic natively under ASan/UBSan.
 */

#include "poll_select.h"

/**
 * Translate three select() fd_sets into a packed pollfd array.
 *
 * Scans fds 0..nfds-1; an fd selected in the read, write, or except set
 * contributes one pollfd carrying POLLIN, POLLOUT, and POLLPRI for the
 * direction(s) it was selected in. Fds in no set are skipped, so the array is
 * dense. The .events field records the requested set membership and is the
 * channel pollfds_to_fdsets() reads back to project results onto the right
 * direction.
 *
 * @return the number of pollfds written, or -1 if the selected fds exceed
 *         max_pollfds.
 */
int fdsets_to_pollfds(uint32_t nfds, const uint32_t *r, const uint32_t *w, const uint32_t *e,
                      struct kernel_pollfd *fds, int max_pollfds)
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

/**
 * Whether select() counts one already-scanned pollfd as ready.
 *
 * The per-fd form of `pollfds_to_fdsets`'s rule, so the poll core's
 * readiness test agrees with the projection: an fd is ready only on a revent
 * that lands in a direction it was selected in (POLLIN/POLLHUP/POLLERR read
 * as readable, POLLOUT/POLLERR as writable, POLLPRI as except). Without this
 * the poll core would count a write- or except-only fd reporting only
 * POLLHUP as ready while the projection clears every set, so select() would
 * return 0 with empty sets and a caller waiting forever would busy-spin.
 * POLLNVAL (a closed fd) is handled separately by the scan, so it is not
 * considered here.
 */
int pollfd_select_ready(short events, short revents)
{
    if ((events & POLLIN) && (revents & (POLLIN | POLLHUP | POLLERR))) {
        return 1;
    }
    if ((events & POLLOUT) && (revents & (POLLOUT | POLLERR))) {
        return 1;
    }
    if ((events & POLLPRI) && (revents & POLLPRI)) {
        return 1;
    }
    return 0;
}

/**
 * Project poll results back into select()'s three fd_sets and count them.
 *
 * Each fd is scattered only onto the direction(s) the caller selected, read
 * from .events as set by fdsets_to_pollfds (POLLIN<->readfds, POLLOUT<->
 * writefds, POLLPRI<->exceptfds). Following select()'s POLLIN_SET/POLLOUT_SET
 * rule, POLLHUP and POLLERR count as readable and POLLERR also as writable, but
 * only for an fd that was in that set, so a hangup on a write-only-selected fd
 * is not invented into a read set the caller may have passed as NULL.
 *
 * @return select()'s ready count: each direction set counts once, so an fd
 *         ready for both read and write contributes 2.
 */
int pollfds_to_fdsets(const struct kernel_pollfd *fds, int n, uint32_t *r, uint32_t *w, uint32_t *e)
{
    int i;
    int count = 0;

    for (i = 0; i < n; i++) {
        uint32_t fd = (uint32_t)fds[i].fd;
        uint32_t word = fd / 32;
        uint32_t bit = 1u << (fd % 32);
        short ev = fds[i].events;
        short re = fds[i].revents;

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
