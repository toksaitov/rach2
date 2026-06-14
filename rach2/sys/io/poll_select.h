#ifndef _POLL_SELECT_H_
#define _POLL_SELECT_H_

#include "../lib/kint.h"
#include "poll.h"

/*
 * select()/_newselect()/pselect6() are layered on poll() by translating the three
 * fd_set bitmaps to a pollfd array and back. This is pure index/bit logic -- the
 * fence-post-prone part -- so it lives apart from poll.c and is exercised
 * natively under ASan in tests/host/poll_select_hosttest.c.
 *
 * A musl fd_set is FD_SETSIZE=1024 bits; the caller's buffer is that full size,
 * so we copy and clear all of it even though Rach2 fds only reach MAX_FDS.
 */
#define FDSET_BITS  1024
#define FDSET_WORDS (FDSET_BITS / 32) /* 32 x 32-bit words = 128 bytes */

/*
 * Build pollfds from the read/write/except sets: one pollfd per fd in [0, nfds)
 * that is set in any set, with the union of POLLIN/POLLOUT/POLLPRI. Returns the
 * pollfd count, or -1 if a set bit names an fd that cannot fit in max_pollfds
 * (i.e. an fd the kernel could never have, which select must reject as EBADF).
 */
int fdsets_to_pollfds(uint32_t nfds, const uint32_t *r, const uint32_t *w,
                      const uint32_t *e, struct kernel_pollfd *fds, int max_pollfds);

/*
 * Project pollfd revents back onto the three (pre-zeroed) fd_sets, gated by the
 * direction each fd was selected for (its .events, as set by fdsets_to_pollfds):
 * readable = POLLIN|POLLHUP|POLLERR (only if POLLIN was requested), writable =
 * POLLOUT|POLLERR (only if POLLOUT was requested), except = POLLPRI. Returns the
 * total number of bits set, which is select()'s return value.
 */
int pollfds_to_fdsets(const struct kernel_pollfd *fds, int n, uint32_t *r,
                      uint32_t *w, uint32_t *e);

#endif /* _POLL_SELECT_H_ */
