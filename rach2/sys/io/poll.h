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
 * carry a signal mask to swap for the duration; Rach2 accepts that argument
 * but does not apply it (a documented limitation; see the note in poll.c).
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
    int   fd;
    short events;
    short revents;
};

struct isr_params;

/* poll(168) / ppoll(309). */
int32_t sys_poll(struct isr_params *params);
int32_t sys_ppoll(struct isr_params *params);

/* select(82) old single-arg-struct form, _newselect(142) normal form, and
 * pselect6(308). */
int32_t sys_select(struct isr_params *params);
int32_t sys__newselect(struct isr_params *params);
int32_t sys_pselect6(struct isr_params *params);

#endif /* _POLL_H_ */
