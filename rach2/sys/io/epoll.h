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
 * deep at most, as Linux nests them.
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
 * `wqs`, as many as `room` has, and their count to `*n`.
 */
short epoll_poll_set(struct fd_entry *e, short events, struct wait_queue **wqs, uint32_t room,
                     uint32_t *n);

int32_t sys_epoll_create(struct isr_params *params);
int32_t sys_epoll_create1(struct isr_params *params);
int32_t sys_epoll_ctl(struct isr_params *params);
int32_t sys_epoll_wait(struct isr_params *params);
int32_t sys_epoll_pwait(struct isr_params *params);
int32_t sys_epoll_pwait2(struct isr_params *params);

#endif /* _EPOLL_H_ */
