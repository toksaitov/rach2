#ifndef _TIMERFD_H_
#define _TIMERFD_H_

#include "../lib/kint.h"

/*
 * timerfd: a timer read through a descriptor. timerfd_settime arms it with a
 * first expiration, relative or absolute on its clock, and an interval; a
 * read of eight bytes returns the expirations since the last read and waits
 * while there are none; poll and epoll report it readable while there are
 * some. The kernel keeps time in 10 ms ticks, so an expiration is rounded up
 * to a tick. The tick handler calls `timerfd_tick`.
 */

struct isr_params;

int32_t sys_timerfd_create(struct isr_params *params);
int32_t sys_timerfd_settime(struct isr_params *params);
int32_t sys_timerfd_gettime(struct isr_params *params);
int32_t sys_timerfd_settime64(struct isr_params *params);
int32_t sys_timerfd_gettime64(struct isr_params *params);

/*
 * POSIX timers, timer_create's, which share the timer of a timerfd: each
 * belongs to its process, by an id from 0, and its expiration posts a
 * signal with SI_TIMER, its id, its value and the overrun, the expirations
 * it went on to count while the signal was pending, to the process or, for
 * SIGEV_THREAD_ID, one of its threads, or nothing for SIGEV_NONE. A process
 * keeps at most PTIMER_MAX; exec and the end of the process delete them.
 */
#define PTIMER_MAX 64

struct tgroup;
struct process;

int32_t sys_timer_create(struct isr_params *params);
int32_t sys_timer_settime(struct isr_params *params);
int32_t sys_timer_gettime(struct isr_params *params);
int32_t sys_timer_settime64(struct isr_params *params);
int32_t sys_timer_gettime64(struct isr_params *params);
int32_t sys_timer_getoverrun(struct isr_params *params);
int32_t sys_timer_delete(struct isr_params *params);

/** Delete the POSIX timers of `tg`, at an exec and when its last thread
 * ends. */
void ptimer_delete_all(struct tgroup *tg);

/** Count the expirations due by the tick `now`, and wake the readers or post
 * the signals; called from the timer interrupt. */
void timerfd_tick(uint32_t now);

/*
 * The clocks' part, in syscall.c: the time on clock `clk` as clock_gettime
 * reads it, in seconds and nanoseconds. 0, or -EINVAL for a clock with no
 * time here.
 */
int32_t clock_now(int32_t clk, int64_t *sec, int32_t *nsec);

/*
 * The clocks' part as well: the target of the clock of processor time `clk`
 * for timer_create, the process whose time it counts, held, which
 * `tgroup_put` lets go of, its thread, 0 for every thread, and whether it
 * counts user time alone. 0, or -EINVAL for a clock that is not of
 * processor time or names no process or no thread of the caller's.
 */
int32_t clock_cpu_target(int32_t clk, struct tgroup **tg, uint32_t *tid, int *user_only);

/** Count a tick of processor time of the thread `p`, user time when `user`,
 * against the POSIX timers that count it; called from the timer tick. */
void ptimer_cpu_tick(const struct process *p, int user);

#endif /* _TIMERFD_H_ */
