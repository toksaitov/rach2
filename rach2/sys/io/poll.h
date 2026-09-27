#ifndef _POLL_H_
#define _POLL_H_

#include "../lib/kint.h"

/*
 * I/O multiplexing: poll/select let one process wait on many descriptors at
 * once and learn which became ready. Each pollable backing answers "which of
 * {readable, writable, error/hangup} is true now" and names the wait queue
 * whose wake signals a change; the poll core scans all the descriptors, and if
 * none is ready, parks the process on every one of those queues until a wake or
 * the timeout, then re-scans (lost-wakeup-safe).
 *
 * Linux i386 exposes the whole family and musl/busybox may use any of them,
 * so all route to one core: select(82), _newselect(142), poll(168),
 * pselect6(308), ppoll(309). The pselect6/ppoll "p" variants additionally
 * carry a signal mask the wait runs under. epoll waits through the same
 * core.
 */

/* poll(2) event bits (Linux values). POLLHUP/POLLERR/POLLNVAL are reported in
 * revents regardless of the requested events. */
#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

/* The struct pollfd the user passes (matches musl's layout exactly). */
struct kernel_pollfd {
    int fd;
    short events;
    short revents;
};

struct isr_params;
struct wait_queue;
struct fd_entry;

/** The readiness of one descriptor: a subset of `events` for POLLIN and
 * POLLOUT, with POLLHUP and POLLERR always reported, and in `*wq` the wait
 * queue whose wake means look again, or NULL for a descriptor always ready. */
short poll_fd(struct fd_entry *e, short events, struct wait_queue **wq);

/* A scan of what a wait watches: the wait queue of each object in `wqs`, and
 * the count of those ready. */
typedef int (*poll_scan_fn)(void *ctx, struct wait_queue **wqs);

/** Wait until `scan` finds something ready, a signal arrives, or the tick
 * `deadline` passes when `has_deadline`; `n` bounds the queues a scan names.
 * Returns the ready count, 0 at the deadline, -EINTR or -ENOMEM. */
int poll_wait(poll_scan_fn scan, void *ctx, uint32_t n, int has_deadline, uint32_t deadline);

/** The deadline of a timeout in milliseconds, rounded up to a tick; a
 * negative timeout has none. */
uint32_t poll_ms_deadline(int timeout_ms, int *has_deadline);

/** Install the signal mask a wait runs under, from the user set `umask` of
 * `size` bytes (0 for none), and restore it after with `poll_mask_done`,
 * which passes the wait's result through. */
int32_t poll_mask_set(uint32_t umask, uint32_t size);
int32_t poll_mask_done(int32_t ret);

/* poll(168) / ppoll(309). */
int32_t sys_poll(struct isr_params *params);
int32_t sys_ppoll(struct isr_params *params);

/* select(82) old single-arg-struct form, _newselect(142) normal form, and
 * pselect6(308). */
int32_t sys_select(struct isr_params *params);
int32_t sys__newselect(struct isr_params *params);
int32_t sys_pselect6(struct isr_params *params);

#endif /* _POLL_H_ */
