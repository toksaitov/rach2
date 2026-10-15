/*
 * futex(2): a process sleeps on a word of its memory until another wakes the
 * address, the word having held the value the sleeper expected when it went
 * to sleep. A futex is named by its address space and its user address; see
 * futex.Readme.md.
 */
#ifndef _FUTEX_H_
#define _FUTEX_H_

#include "../lib/kint.h"
#include "../irq.h"

/**
 * futex takes uaddr, op, val, timeout, uaddr2 and val3. It has FUTEX_WAIT,
 * FUTEX_WAKE and their BITSET forms, each with FUTEX_PRIVATE_FLAG, and
 * FUTEX_CLOCK_REALTIME with FUTEX_WAIT_BITSET alone; uaddr2 is not read. A
 * wait returns 0 when woken, -EAGAIN when the word does not hold val,
 * -ETIMEDOUT or -EINTR; a wake returns the number woken. -EFAULT for a word
 * or a timeout the caller cannot read, -EINVAL for an address not aligned
 * to four bytes, a bitset of 0 or a bad timeout, and -ENOSYS for another
 * op.
 */
long sys_futex(isr_params_t *params);

/**
 * Whether the futex call `params` describes, which a signal ended with
 * EINTR, may be restarted: a wait with no timeout may, and a timed one may
 * not, since a restart would measure its timeout again from the start and a
 * handler that ran often enough would keep it from ever ending. Linux
 * restarts a timed wait only when no handler runs, and then with its first
 * deadline.
 */
int futex_restartable(const isr_params_t *params);

/** set_robust_list takes the list's head and its size, which is the size
 * of the head's three words, 12 bytes on i686 and 24 on x86-64; 0, or
 * -EINVAL for another size. */
long sys_set_robust_list(isr_params_t *params);

/**
 * get_robust_list takes a thread id or 0, where the head goes and where its
 * size goes, and writes there the head and length of the robust list of
 * that thread, the caller for 0. The id is the kernel's, the first PID
 * namespace's. 0; -ESRCH for a thread that is not there or has ended;
 * -EPERM when the thread is of another process and the caller is not
 * privileged, and either the caller's effective uid is neither the
 * thread's real nor its effective one, or the thread is privileged; or
 * -EFAULT.
 */
long sys_get_robust_list(isr_params_t *params);

#endif /* _FUTEX_H_ */
