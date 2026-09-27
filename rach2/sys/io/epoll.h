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
 * appears and not again until it has gone and come back. EPOLLONESHOT stops
 * the reports after the first until epoll_ctl changes the registration. A
 * registration ends when its descriptor is closed: the next wait finds the
 * number naming another open file, or none, and drops it. A wait uses the
 * poll core (poll.c). An epoll descriptor inside another epoll set is
 * reported ready by what its own set holds, but a change in it does not wake
 * a wait on the outer set.
 */

struct isr_params;

int32_t sys_epoll_create(struct isr_params *params);
int32_t sys_epoll_create1(struct isr_params *params);
int32_t sys_epoll_ctl(struct isr_params *params);
int32_t sys_epoll_wait(struct isr_params *params);
int32_t sys_epoll_pwait(struct isr_params *params);
int32_t sys_epoll_pwait2(struct isr_params *params);

#endif /* _EPOLL_H_ */
