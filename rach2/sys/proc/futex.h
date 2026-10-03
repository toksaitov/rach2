#ifndef _FUTEX_H_
#define _FUTEX_H_

#include "../lib/kint.h"
#include "../irq.h"

/*
 * futex(2): a process sleeps on a word of its memory until another wakes the
 * address, the word having held the value the sleeper expected when it went
 * to sleep. A futex is named by its address space and its user address; see
 * futex.Readme.md.
 */

/** The futex system call, 240: its arguments are uaddr, op, val, timeout,
 * uaddr2 and val3. */
long sys_futex(isr_params_t *params);

/** Wake up to `n` waiters on `uaddr` in the current address space, as
 * FUTEX_WAKE does; the number woken. */
int futex_wake_address(uintptr_t uaddr, uint32_t n);

/**
 * Whether the futex call `params` describes, which a signal ended with
 * EINTR, may be restarted: a wait with no timeout may, and a timed one may
 * not, since a restart would measure its timeout again from the start and a
 * handler that ran often enough would keep it from ever ending. Linux
 * restarts a timed wait only when no handler runs, and then with its first
 * deadline.
 */
int futex_restartable(const isr_params_t *params);

/** set_robust_list: its arguments are the list's head and its size, 12.
 * */
long sys_set_robust_list(isr_params_t *params);

/** get_robust_list: its arguments are thread id or 0, where the head goes
 * and where its size goes. */
long sys_get_robust_list(isr_params_t *params);

#endif /* _FUTEX_H_ */
