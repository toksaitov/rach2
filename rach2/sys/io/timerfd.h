#ifndef _TIMERFD_H_
#define _TIMERFD_H_

#include "../lib/kint.h"

/*
 * timerfd: a timer read through a descriptor. timerfd_settime arms it with a
 * first expiration, relative or absolute on its clock, and an interval; a
 * read of eight bytes returns the expirations since the last read and waits
 * while there are none; poll and epoll report it readable while there are
 * some. A timer expires when its clock reaches its time: the tick handler
 * calls `timerfd_tick`, and a time between two ticks is armed on the
 * timer's interrupt between them, which calls `timerfd_due`.
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
 * read. An absolute time of CLOCK_REALTIME moves with a setting of that
 * clock (`timerfd_clock_was_set`). TFD_TIMER_CANCEL_ON_SET with
 * TFD_TIMER_ABSTIME on CLOCK_REALTIME makes the next read after a setting of
 * the clock fail with ECANCELED, and the next setting arm the timer and fail
 * with it. Returns 0, -EBADF, -EINVAL for a descriptor that is not a
 * timerfd, another flag or a time out of range, -ECANCELED, or -EFAULT.
 */
long sys_timerfd_settime(struct isr_params *params);

/** timerfd_gettime(2) takes the descriptor and the address for its struct
 * itimerspec: the time to the next expiration, zero while it is disarmed,
 * and the interval it was last set with. Returns 0, -EBADF, -EINVAL for a
 * descriptor that is not a timerfd, or -EFAULT. */
long sys_timerfd_gettime(struct isr_params *params);

/** timerfd_settime with i386's struct of 64-bit times, the time64 call. */
long sys_timerfd_settime64(struct isr_params *params);

/** timerfd_gettime with i386's struct of 64-bit times, the time64 call. */
long sys_timerfd_gettime64(struct isr_params *params);

/*
 * POSIX timers, timer_create's, which share the timer of a timerfd: each
 * belongs to its process, by an id from 0, and its expiration posts a
 * signal with SI_TIMER, its id, its value and the overrun, every interval
 * that ended after the first expiration and before the signal was taken,
 * pending or not, under the setting in force when it is taken, to the
 * process or, for SIGEV_THREAD_ID, one of its threads, or nothing for
 * SIGEV_NONE. A process keeps at most PTIMER_MAX, deleted timers whose
 * notifications wait to be posted among them; exec and the end of the
 * process delete them all, and exec discards their pending signals.
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
 * one, or 0, and sets the timer as timerfd_settime does. The expirations
 * of the old setting that the tick counted and no holder of the big lock
 * has signalled yet are signalled first, as Linux signals the first at its
 * expiry and stops the timer until that signal is delivered
 * (kernel/time/posix-timers.c:318-377): the first of them, the setting's
 * first expiration or one after the setting's signal was taken, is posted,
 * or counts with the timer's notification already pending or waiting. The
 * rest, and the old setting's later expirations, are dropped, and a signal
 * of it still pending forwards nothing when taken. Returns 0, -EINVAL for
 * an id the process does not have, another flag, a null setting or a time
 * out of range, or -EFAULT. */
long sys_timer_settime(struct isr_params *params);

/** timer_gettime(2) takes the id and the address for the timer's struct
 * itimerspec, as timerfd_gettime gives it. Returns 0, -EINVAL for an id the
 * process does not have, or -EFAULT. */
long sys_timer_gettime(struct isr_params *params);

/** timer_settime with i386's struct of 64-bit times, the time64 call. */
long sys_timer_settime64(struct isr_params *params);

/** timer_gettime with i386's struct of 64-bit times, the time64 call. */
long sys_timer_gettime64(struct isr_params *params);

/** timer_getoverrun(2) takes the id. Returns the overrun the setting in
 * force counted for the timer's last periodic signal delivered, at most
 * 0x7FFFFFFF, or -EINVAL for an id the process does not have. */
long sys_timer_getoverrun(struct isr_params *params);

/** timer_delete(2) takes the id and deletes the timer, first signalling
 * the expirations the tick counted and no holder of the big lock has
 * signalled yet, as timer_settime does, the setting's first or one after
 * its signal was taken. Its notification that waits for another sender's
 * signal of its number is posted once that signal is taken, with its own
 * overrun. Returns 0, or -EINVAL for an id the process does not have. */
long sys_timer_delete(struct isr_params *params);

/** Delete the POSIX timers of `tg` when its last thread ends, and the
 * notifications of deleted ones that still wait, as Linux's exit_itimers
 * (kernel/exit.c:919). */
void ptimer_delete_all(struct tgroup *tg);

/** Delete the POSIX timers of the process of `self`, the one thread left
 * at an exec, as `ptimer_delete_all` does, and discard their signals still
 * pending, as Linux's posixtimer_exec (fs/exec.c:1220-1229). */
void ptimer_exec(struct process *self);

/** Discard the waiting and not yet posted notifications of `tg`'s POSIX
 * timers of the signals `bits`, which have just been discarded from the
 * process and its threads, and free the deleted timers they kept; with
 * interrupts off. */
void ptimer_discard(struct tgroup *tg, uint64_t bits);

/** Drop the notifications of the POSIX timers that SIGEV_THREAD_ID aims at
 * the ending thread `self`, pending, waiting on it or not yet posted, and
 * free the deleted timers they kept, then publish its zombie state under
 * the same timer lock, as Linux 6.12.115 flushes an ending thread's pending
 * signals under its signal lock (kernel/exit.c:203-207). Called by the
 * thread with the big lock, after every release that may sleep. */
void ptimer_thread_exit(struct process *self);

/** Count the expirations due by the tick `now`, and wake the readers or
 * leave the signals for a holder of the big lock to post
 * (`ptimer_post_due`); called from the timer interrupt that made the
 * clock's pass, without the big lock and with interrupts off. */
void timerfd_tick(uint64_t now);

/** Nonzero while a POSIX timer has an expiration whose signal waits to be
 * posted; read without a lock by a tick deciding whether to try the big
 * lock. */
int ptimer_posts_due(void);

/** Post the signals of the POSIX timers that expired since the last call,
 * in the order they expired. The caller holds the big lock. */
void ptimer_post_due(void);

struct k_siginfo;

/** After the signal `signo` of the process `tg`, whose information `info`
 * is, has been taken: a periodic POSIX timer's signal of the setting in
 * force has that setting's expirations past the one it stands for added to
 * its overrun in `info`, si_overrun, which also keeps what the signal
 * carried while queued, and the timer moved past the clock and armed
 * between ticks again, as Linux's posixtimer_rearm restarts a timer at the
 * delivery of its signal (kernel/time/posix-timers.c:258-279). Those
 * expirations alone, without the carry, are what timer_getoverrun then
 * reports, as Linux's it_overrun_last, which timer_overrun_to_int adds to
 * si_overrun (kernel/time/posix-timers.c:237-242, 270-274). Then the
 * timers' notifications that waited for that signal number are posted,
 * oldest first.
 * Process context, with interrupts off from the signal's dequeue through
 * the call, so that no tick posts the timer's signal again before its
 * overrun is counted, as dequeue_signal calls posixtimer_rearm before it
 * lets interrupts on (kernel/signal.c:682-699). */
void ptimer_rearm(struct tgroup *tg, int signo, struct k_siginfo *info);

/** Expire the timers armed between ticks whose clock has reached their
 * time, leaving the POSIX timers' signals for `ptimer_post_due`, and arm the
 * rest again, from the interrupt between ticks (`timer_hr_pass`), with
 * interrupts off. */
void timerfd_due(void);

/** Count the ticks of the timers of CLOCK_REALTIME again and cancel the
 * timerfds set to be canceled whose recorded offset of real time is not the
 * clock's now, after a setting of that clock, refused or not, or a leap
 * second; a slew is none, since real time and the ticks run at its rate
 * alike. Called by clock.c, with interrupts on or off, after it lets go of
 * the clock's lock. */
void timerfd_clock_was_set(void);

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

/**
 * The clocks' part as well: the processor time, in nanoseconds, of the
 * target `clock_cpu_target` gave, the process `tg`, or its thread `tid`
 * when that is not 0, user time alone for `user_only`, as Linux's
 * posix_cpu_timer_set samples the task a timer was made for and not the
 * caller (kernel/time/posix-cpu-timers.c:719-751). 0, or -ESRCH when the
 * thread has ended, as Linux's timer then has no task.
 */
int32_t clock_cpu_sample(const struct tgroup *tg, uint32_t tid, int user_only, uint64_t *ns);

/** Count a tick of processor time of the thread `p`, user time when `user`,
 * against the POSIX timers that count it, leaving each signal due for
 * `ptimer_post_due`; called from the timer tick of the processor that runs
 * `p`, without the big lock and with interrupts off. */
void ptimer_cpu_tick(const struct process *p, int user);

#endif /* _TIMERFD_H_ */
