#ifndef _EVENTFD_H_
#define _EVENTFD_H_

#include "../lib/kint.h"

/*
 * eventfd: a descriptor over a 64-bit counter, the simplest way for one
 * thread or process to wake another. A write of eight bytes adds its value
 * to the counter, and a read of eight bytes takes the counter and sets it to
 * zero, or with EFD_SEMAPHORE takes one; a read of zero waits, and so does a
 * write that would carry the counter past 0xfffffffffffffffe. poll reports
 * the descriptor readable while the counter is not zero and writable while
 * another 1 fits.
 */

struct isr_params;

/** eventfd(323) and eventfd2(328): EBX the initial value, ECX the flags
 * EFD_CLOEXEC, EFD_NONBLOCK and EFD_SEMAPHORE. */
long sys_eventfd(struct isr_params *params);
long sys_eventfd2(struct isr_params *params);

#endif /* _EVENTFD_H_ */
