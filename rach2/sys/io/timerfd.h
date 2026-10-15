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

/** timerfd_create(2) takes the clock, CLOCK_REALTIME, CLOCK_MONOTONIC or
 * CLOCK_BOOTTIME, and the flags TFD_NONBLOCK and TFD_CLOEXEC. Returns the
 * descriptor of a new disarmed timer, -EPERM for an alarm clock, -EINVAL
 * for another clock or flag, -ENOMEM or -EMFILE. */
long sys_timerfd_create(struct isr_params *params);

/**
 * timerfd_settime(2) takes the descriptor, the flags, the address of the new
 * struct itimerspec and the address for the old one, or 0. The setting arms
 * the timer, at a time absolute on its clock under TFD_TIMER_ABSTIME, or
 * disarms it for a first expiration of zero, and drops the expirations not
 * read. TFD_TIMER_CANCEL_ON_SET is taken and does nothing. Returns 0,
 * -EBADF, -EINVAL for a descriptor that is not a timerfd, another flag or a
 * time out of range, or -EFAULT.
 */
long sys_timerfd_settime(struct isr_params *params);

/** timerfd_gettime(2) takes the descriptor and the address for its struct
 * itimerspec: the time to the next expiration and the interval, both zero
 * while it is disarmed. Returns 0, -EBADF, -EINVAL for a descriptor that is
 * not a timerfd, or -EFAULT. */
long sys_timerfd_gettime(struct isr_params *params);

/** timerfd_settime with i386's struct of 64-bit times, the time64 call. */
long sys_timerfd_settime64(struct isr_params *params);

/** timerfd_gettime with i386's struct of 64-bit times, the time64 call. */
long sys_timerfd_gettime64(struct isr_params *params);

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

/**
 * timer_create(2) takes the clock, the address of a struct sigevent or 0,
 * and the address for the timer's id. The clock is real time, monotonic
 * time or time since boot, or the processor time of a process or of a
 * thread of the caller's (`clock_cpu_target`). The notification is
 * SIGEV_SIGNAL, SIGEV_NONE or SIGEV_THREAD_ID, whose thread must be one of
 * the caller's process; a null sigevent is SIGALRM with the id as its value.
 * The id is the lowest the process does not have. Returns 0, -EINVAL for
 * another clock, notification or signal, -EAGAIN past PTIMER_MAX or when the
 * heap has no room, or -EFAULT, with no timer made.
 */
long sys_timer_create(struct isr_params *params);

/** timer_settime(2) takes the id, the flags, of which TIMER_ABSTIME is the
 * one, the address of the new struct itimerspec and the address for the old
 * one, or 0, and sets the timer as timerfd_settime does. Returns 0, -EINVAL
 * for an id the process does not have, another flag, a null setting or a
 * time out of range, or -EFAULT. */
long sys_timer_settime(struct isr_params *params);

/** timer_gettime(2) takes the id and the address for the timer's struct
 * itimerspec, as timerfd_gettime gives it. Returns 0, -EINVAL for an id the
 * process does not have, or -EFAULT. */
long sys_timer_gettime(struct isr_params *params);

/** timer_settime with i386's struct of 64-bit times, the time64 call. */
long sys_timer_settime64(struct isr_params *params);

/** timer_gettime with i386's struct of 64-bit times, the time64 call. */
long sys_timer_gettime64(struct isr_params *params);

/** timer_getoverrun(2) takes the id. Returns the overrun of the timer's last
 * signal, at most 0x7FFFFFFF, or -EINVAL for an id the process does not
 * have. */
long sys_timer_getoverrun(struct isr_params *params);

/** timer_delete(2) takes the id and deletes the timer. Returns 0, or -EINVAL
 * for an id the process does not have. */
long sys_timer_delete(struct isr_params *params);

/** Delete the POSIX timers of `tg`, at an exec and when its last thread
 * ends. */
void ptimer_delete_all(struct tgroup *tg);

/** Count the expirations due by the tick `now`, and wake the readers or post
 * the signals; called from the boot processor's timer interrupt, with
 * interrupts off. */
void timerfd_tick(uint32_t now);

/**
 * The clocks' part, in syscall.c: the time on clock `clk` as clock_gettime
 * reads it, in seconds and nanoseconds. 0, or -EINVAL for a clock with no
 * time here.
 */
int32_t clock_now(int32_t clk, int64_t *sec, int32_t *nsec);

/**
 * The clocks' part as well: the target of the clock of processor time `clk`
 * for timer_create, the process whose time it counts, held, which
 * `tgroup_put` lets go of, its thread, 0 for every thread, and whether it
 * counts user time alone. 0, or -EINVAL for a clock that is not of
 * processor time or names no process or no thread of the caller's.
 */
int32_t clock_cpu_target(int32_t clk, struct tgroup **tg, uint32_t *tid, int *user_only);

/** Count a tick of processor time of the thread `p`, user time when `user`,
 * against the POSIX timers that count it; called from the timer tick of the
 * processor that runs `p`. */
void ptimer_cpu_tick(const struct process *p, int user);

#endif /* _TIMERFD_H_ */
