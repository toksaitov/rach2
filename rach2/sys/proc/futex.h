#ifndef _FUTEX_H_
#define _FUTEX_H_

#include "../lib/kint.h"
#include "../isr.h"

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

#endif /* _FUTEX_H_ */
