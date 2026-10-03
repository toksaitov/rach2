#ifndef _FUTEX_H_
#define _FUTEX_H_

#include "../lib/kint.h"
#include "../arch/x86/isr.h"

/*
 * futex(2): a process sleeps on a word of its memory until another wakes the
 * address, the word having held the value the sleeper expected when it went
 * to sleep. A futex is named by its address space and its user address; see
 * futex.Readme.md.
 */

/** The futex system call, 240: EBX=uaddr, ECX=op, EDX=val, ESI=timeout,
 * EDI=uaddr2, EBP=val3. */
int32_t sys_futex(isr_params_t *params);

/** Wake up to `n` waiters on `uaddr` in the current address space, as
 * FUTEX_WAKE does; the number woken. */
int futex_wake_address(uint32_t uaddr, uint32_t n);

/**
 * Whether the futex call `params` describes, which a signal ended with
 * EINTR, may be restarted: a wait with no timeout may, and a timed one may
 * not, since a restart would measure its timeout again from the start and a
 * handler that ran often enough would keep it from ever ending. Linux
 * restarts a timed wait only when no handler runs, and then with its first
 * deadline.
 */
int futex_restartable(const isr_params_t *params);

/** set_robust_list (311): EBX=the list's head, ECX=its size, 12. */
int32_t sys_set_robust_list(isr_params_t *params);

/** get_robust_list (312): EBX=thread id or 0, ECX=where the head goes,
 * EDX=where its size goes. */
int32_t sys_get_robust_list(isr_params_t *params);

#endif /* _FUTEX_H_ */
