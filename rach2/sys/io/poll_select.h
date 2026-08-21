#ifndef _POLL_SELECT_H_
#define _POLL_SELECT_H_

#include "../lib/kint.h"
#include "poll.h"

/*
 * select(), _newselect(), and pselect6() are layered on poll() by translating
 * the three fd_set bitmaps to a pollfd array and back. The bit/index arithmetic
 * lives apart from poll.c and is exercised natively under ASan in
 * tests/host/poll_select_hosttest.c.
 *
 * A musl fd_set is FD_SETSIZE=1024 bits; the caller's buffer is that full size,
 * so we copy and clear all of it even though Rach2 fds only reach MAX_FDS.
 */
#define FDSET_BITS  1024
#define FDSET_WORDS (FDSET_BITS / 32) /* 32 x 32-bit words = 128 bytes */

/** Translate three select() fd_sets into a packed pollfd array; pollfd
 * count, or -1 if the selected fds exceed max_pollfds. */
int fdsets_to_pollfds(uint32_t nfds, const uint32_t *r, const uint32_t *w,
                      const uint32_t *e, struct kernel_pollfd *fds, int max_pollfds);

/** Whether select() counts one scanned pollfd ready (the per-fd projection
 * rule). */
int pollfd_select_ready(short events, short revents);

/** Project poll results back into select()'s three fd_sets; returns the
 * ready count. */
int pollfds_to_fdsets(const struct kernel_pollfd *fds, int n, uint32_t *r,
                      uint32_t *w, uint32_t *e);

#endif /* _POLL_SELECT_H_ */
