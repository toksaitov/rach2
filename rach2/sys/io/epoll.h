#ifndef _EPOLL_H_
#define _EPOLL_H_

#include "../lib/kint.h"

/*
 * epoll: a descriptor that keeps a set of descriptors to watch, so a wait
 * names the set once instead of passing it to every poll. epoll_ctl adds,
 * changes and removes a descriptor with the events it is watched for and a
 * word of data the program chooses; epoll_wait returns the descriptors
 * ready, with their events and data.
 *
 * A registration is level-triggered, reported at every wait while its
 * descriptor is ready, or with EPOLLET edge-triggered, reported when an event
 * appears or its descriptor's wait queue is woken for a kind of change among
 * its events (data, room, or a hangup), and not again until then.
 * EPOLLONESHOT stops the reports after the first until epoll_ctl changes the
 * registration. A registration ends when its descriptor is closed: the next
 * wait finds the number naming another open file, or none, and drops it. A
 * wait uses the poll core (poll.c). An epoll set inside another is ready
 * while a wait on it would report an event, and a wait on the outer set is
 * on the queues of the sets inside it and of what they watch, four sets
 * deep at most, as Linux nests them; an add that would nest sets deeper,
 * counting those above the set added to as well as those below the set
 * added, or make a cycle, is refused.
 */

struct isr_params;
struct fd_entry;
struct wait_queue;

/** Whether the descriptor `e` is an epoll set. */
int epoll_is_set(const struct fd_entry *e);

/**
 * The readiness of the epoll set `e` for poll and select: POLLIN among
 * `events` while a wait on it would report an event. The wait queues a poll
 * of it must be on, the set's own and those of what it watches, go to
 * `wqs`, as many as `room` has, and their count, which may be more, to
 * `*n`. The caller has interrupts off, as the scan of `poll_wait` does.
 */
short epoll_poll_set(struct fd_entry *e, short events, struct wait_queue **wqs, uint32_t room,
                     uint32_t *n);

/** epoll_create(2) takes a size, which must be positive and is otherwise
 * unused. Returns the descriptor of a new set, -EINVAL, -ENOMEM or -EMFILE. */
long sys_epoll_create(struct isr_params *params);

/** epoll_create1(2) takes the flags, of which EPOLL_CLOEXEC is the one.
 * Returns the descriptor of a new set, -EINVAL for another flag, -ENOMEM or
 * -EMFILE. */
long sys_epoll_create1(struct isr_params *params);

/**
 * epoll_ctl(2) takes the set, the operation, the descriptor and the address
 * of its struct epoll_event, which EPOLL_CTL_DEL does not read.
 * EPOLL_CTL_ADD registers the descriptor; EPOLL_CTL_MOD gives the
 * registration new events and data and turns a one-shot one on again; and
 * EPOLL_CTL_DEL removes it. EPOLLWAKEUP is taken and dropped.
 *
 * Returns 0, -EBADF when either descriptor is not open, -EINVAL when the
 * set is not an epoll set or is the descriptor added, for an unknown
 * operation and for EPOLL_CTL_MOD with EPOLLEXCLUSIVE in the new events or
 * the old, -EPERM for a regular file or a directory, -EFAULT, -EEXIST for an
 * add of a descriptor registered already, -ENOENT for a change or removal
 * of one that is not, -ELOOP for an add that nests sets more than four deep
 * or makes a cycle, or -ENOMEM.
 */
long sys_epoll_ctl(struct isr_params *params);

/** epoll_wait(2) takes the set, the user array of events, its length and a
 * timeout in milliseconds, negative for none. Returns the events copied, 0
 * at the timeout, -EBADF, -EINVAL for a descriptor that is not a set or a
 * length not from 1 to INT_MAX / 12, -EFAULT, -EINTR or -ENOMEM. */
long sys_epoll_wait(struct isr_params *params);

/** epoll_pwait(2) takes epoll_wait's arguments and then a signal mask and its
 * size in bytes, the mask the wait runs under (`poll_mask_set`); a null mask
 * leaves the caller's. Returns as epoll_wait does, or -EINVAL for a size
 * that is not 8. */
long sys_epoll_pwait(struct isr_params *params);

/**
 * epoll_pwait2(2) is epoll_pwait with the address of a struct
 * __kernel_timespec, 64-bit seconds and nanoseconds, in place of the
 * milliseconds, and no timeout for a null address. Negative seconds, or
 * nanoseconds outside 0 to 999999999, are -EINVAL; a timeout past
 * 2147483647 milliseconds, about 24.8 days, is cut to it.
 */
long sys_epoll_pwait2(struct isr_params *params);

#endif /* _EPOLL_H_ */
