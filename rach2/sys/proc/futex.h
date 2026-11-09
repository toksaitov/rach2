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
 * futex_waitv takes an array of struct futex_waitv, their count, flags, an
 * absolute timeout as a struct __kernel_timespec or NULL, and the clock it
 * is read on, CLOCK_MONOTONIC or CLOCK_REALTIME. It sleeps while each of
 * the 1 to 128 words holds its value, until a wake of any of them, and
 * returns that futex's index. -EAGAIN when a word does not hold its value,
 * -ETIMEDOUT, -EINTR, which the dispatcher restarts since the timeout is
 * absolute; -EINVAL for flags other than 0, a count out of range, a NULL
 * array, another clock with a timeout, an entry whose size is not 32 bits,
 * whose flags are unknown, whose reserved word is not 0, whose value does
 * not fit 32 bits or whose address is not aligned to four; -EFAULT for an
 * array, timeout or word the caller cannot read; -ENOMEM.
 */
long sys_futex_waitv(isr_params_t *params);

/**
 * Whether the futex call `params` describes, which a signal ended with
 * EINTR, may be restarted: a wait with no timeout may, and a timed one may
 * not, since a restart would measure its timeout again from the start and a
 * handler that ran often enough would keep it from ever ending. Linux
 * restarts a timed wait only when no handler runs, and then with its first
 * deadline.
 */
int futex_restartable(const isr_params_t *params);

/* The head of a robust list of 32-bit words, Linux's
 * compat_robust_list_head: an i386 program's on x86-64, and the native
 * head's layout on i686. */
struct robust_head32 {
    uint32_t next;        /* the first entry, the head's address for none */
    int32_t futex_offset; /* from an entry to its futex word */
    uint32_t op_pending;  /* the entry being taken or released, or 0 */
};

/** set_robust_list takes the list's head and its size, which is the size
 * of the head's three words, 12 bytes on i686 and 24 on x86-64; 0, or
 * -EINVAL for another size. On x86-64 an i386 call names the thread's list
 * of 32-bit words, of 12 bytes, which is kept and walked beside its native
 * one, as Linux keeps `compat_robust_list`. */
long sys_set_robust_list(isr_params_t *params);

/**
 * get_robust_list takes a thread id or 0, where the head goes and where its
 * size goes, and writes there the head and length of the robust list of
 * that thread, the caller for 0. The id is the thread's number in the
 * caller's PID namespace. 0; -ESRCH for a thread that the namespace does not
 * show or that has ended; -EPERM when the thread is of another process and
 * the caller is not privileged, and either the caller's effective uid is
 * neither the thread's real nor its effective one, or the thread is
 * privileged; or -EFAULT. An i386 call on x86-64 gets the thread's list of
 * 32-bit words and its size, 12, as two 32-bit words.
 */
long sys_get_robust_list(isr_params_t *params);

#endif /* _FUTEX_H_ */
