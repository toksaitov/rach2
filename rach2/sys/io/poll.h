#ifndef _POLL_H_
#define _POLL_H_

#include "../lib/kint.h"

/*
 * I/O multiplexing: poll and select let a process wait on many descriptors
 * at once and learn which became ready. Each kind of descriptor answers
 * which of readable, writable, error and hangup is true now, and names the
 * wait queue whose wake means a change; the poll core scans the
 * descriptors and, when none is ready, waits on every one of those queues
 * until a wake, a signal or the timeout, then scans again.
 *
 * i386 has select, _newselect, poll, pselect6 and ppoll, and x86-64 has
 * poll, select (i386's _newselect), pselect6 and ppoll; every one reaches
 * the same core. pselect6 and ppoll also take a signal mask the wait runs
 * under. epoll waits through the core as well.
 */

/* poll(2) event bits (Linux values). POLLHUP/POLLERR/POLLNVAL are reported in
 * revents regardless of the requested events. */
#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

/* The struct pollfd of poll(2), as musl lays it out. */
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

/* A scan of what a wait watches, for `poll_wait`: it puts the wait queue of
 * each object in `wqs` and returns the count of those ready. */
typedef int (*poll_scan_fn)(void *ctx, struct wait_queue **wqs);

/* When a wait ends: at the tick `tick`, once time since boot has reached
 * `sec` and `nsec`, the reading at the call and the timeout. A lost tick
 * leaves the tick count behind that clock, and the wait then lasts until the
 * count catches up (`deadline_passed` in poll.c). */
struct poll_deadline {
    int set; /* nonzero for a finite timeout; 0 waits without one */
    uint32_t tick;
    int64_t sec; /* 0, with `nsec` 0, for a timeout ended by its tick alone */
    int64_t nsec;
};

/**
 * Wait until `scan` finds something ready, a signal arrives, or the deadline
 * `d` passes when it is set. `scan` runs with interrupts off, with `ctx` and
 * an array of `n` queues, the most it may name. Returns the ready count, 0
 * at the deadline, -EINTR or -ENOMEM. A deadline moved to a later tick stays
 * moved in `d`, so a caller that waits again with it keeps the same end.
 * Sleeps, so it is for system call context.
 */
int poll_wait(poll_scan_fn scan, void *ctx, uint32_t n, struct poll_deadline *d);

/** In `d`, the deadline of a timeout of `timeout_ms` milliseconds: the first
 * tick at or after its end, and its end by time since boot. A negative
 * timeout has none. */
void poll_ms_deadline(int timeout_ms, struct poll_deadline *d);

/** Install the signal mask a wait runs under, from the user set `umask` of
 * `size` bytes, or nothing for a `umask` of 0. Returns 0, -EINVAL for a size
 * that is not 8, or -EFAULT. */
int32_t poll_mask_set(uintptr_t umask, uint32_t size);

/** Return `ret`, the wait's result, after putting back the mask
 * `poll_mask_set` replaced, unless `ret` is -EINTR: the delivery of the
 * signal puts the mask back after its handler. */
int32_t poll_mask_done(int32_t ret);

/** poll(2) takes the user array of struct pollfd, its length and a timeout
 * in milliseconds, negative for none. Returns the count of entries with
 * events, 0 at the timeout, -EINVAL for more entries than FD_TABLE_LIMIT,
 * -EFAULT, -EINTR or -ENOMEM. */
long sys_poll(struct isr_params *params);

/** ppoll(2) takes the array and its length, the address of a timespec of
 * longs or 0 for none, and a signal mask and its size, the mask the wait
 * runs under. Returns as poll does, or -EINVAL for a timespec out of range
 * or a mask size that is not 8. */
long sys_ppoll(struct isr_params *params);

/**
 * _newselect(2), x86-64's select, takes the count of descriptors, the
 * addresses of the read, write and exception fd_sets, each 0 for none, and
 * the address of a timeval, 0 for none. i386's old select takes the address
 * of those five, Linux's struct sel_arg_struct. Returns the count of bits
 * set in the sets written back, 0 at the timeout, -EBADF for a set bit of a
 * descriptor that is not open or was opened with O_PATH, -EINVAL for a
 * count negative or past 1024 or a timeval out of range, -EFAULT, -EINTR or
 * -ENOMEM.
 */
long sys_select(struct isr_params *params);
long sys__newselect(struct isr_params *params);

/** pselect6(2) takes _newselect's arguments with the address of a timespec
 * of longs in place of the timeval, and as a sixth the address of a pair,
 * the address of a signal mask and its size, the mask the wait runs under.
 * Returns as _newselect does, with -EINVAL for a timespec out of range or a
 * mask size that is not 8. */
long sys_pselect6(struct isr_params *params);

#endif /* _POLL_H_ */
