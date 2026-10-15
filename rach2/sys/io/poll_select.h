#ifndef _POLL_SELECT_H_
#define _POLL_SELECT_H_

#include "../lib/kint.h"
#include "poll.h"

/*
 * The translation between the three fd_sets of select and an array of
 * pollfd, through which the select calls wait in the poll core. It is apart
 * from poll.c and uses nothing of the kernel, so that
 * tests/host/poll_select_hosttest.c runs it on the host under ASan and UBSan.
 *
 * A musl fd_set is FD_SETSIZE=1024 bits, which is also the most descriptors
 * a process here may hold; the caller's buffer is that full size, and all of
 * it is copied and cleared.
 */
#define FDSET_BITS  1024
#define FDSET_WORDS (FDSET_BITS / 32) /* 32 x 32-bit words = 128 bytes */

/**
 * Translate the fd_sets `r`, `w` and `e` of select, over the descriptors
 * below `nfds`, into the array `fds`: one entry for each descriptor set in
 * any of them, in order, whose events are POLLIN, POLLOUT and POLLPRI for
 * the sets it is in, which `pollfds_to_fdsets` reads back. Returns the count
 * of entries, or -1 when they would be more than `max_pollfds`.
 */
int fdsets_to_pollfds(uint32_t nfds, const uint32_t *r, const uint32_t *w, const uint32_t *e,
                      struct kernel_pollfd *fds, int max_pollfds);

/** Whether select counts a scanned entry ready, from its `events` as
 * `fdsets_to_pollfds` set them and its `revents`: the rule of
 * `pollfds_to_fdsets` for one entry. POLLNVAL is not asked about here; the
 * scan answers it. */
int pollfd_select_ready(short events, short revents);

/** Set in the fd_sets `r`, `w` and `e` the bit of each of the `n` scanned
 * entries of `fds` for each set it was selected in and is ready in, and
 * return the count of bits set, as select counts them: a descriptor ready
 * for reading and for writing counts twice. POLLHUP and POLLERR count as
 * readable and POLLERR as writable. */
int pollfds_to_fdsets(const struct kernel_pollfd *fds, int n, uint32_t *r, uint32_t *w,
                      uint32_t *e);

#endif /* _POLL_SELECT_H_ */
