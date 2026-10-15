/*
 * poll_select.c: the translation between the fd_sets of select and an array
 * of pollfd, as poll_select.h describes it.
 */

#include "poll_select.h"

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
            return -1;
        }
        fds[n].fd = (int)fd;
        fds[n].events = ev;
        fds[n].revents = 0;
        n++;
    }
    return n;
}

/**
 * The poll core's test of readiness agrees with the projection onto the
 * fd_sets: an entry is ready only for an event that lands in a set it was
 * selected in. Otherwise the core would count a descriptor selected for
 * writing or exceptions that reports only POLLHUP as ready while the
 * projection clears every set, so select would return 0 with empty sets,
 * and a caller that waits without a timeout would spin.
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
 * The events count as Linux's POLLIN_SET, POLLOUT_SET and POLLEX_SET have
 * them, but only for a set the descriptor was selected in, so that a hangup
 * of one selected only for writing goes to no read set, which the caller
 * may have passed as NULL.
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
