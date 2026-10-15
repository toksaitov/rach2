#ifndef _SECCOMP_H_
#define _SECCOMP_H_

#include "../lib/kint.h"
#include "../irq.h"
#include "process.h"

/*
 * seccomp: the system call filters of seccomp(2) and prctl(2), as Linux
 * has them. A thread has a chain of filters, newest first, each a classic
 * BPF program that reads a call's number, the architecture, the instruction
 * pointer and the six arguments, and returns an action. Every filter of the
 * chain runs, and the action of highest precedence is taken. A chain is
 * shared, and counted, by the threads and children that inherit it, and a
 * filter is never changed once installed.
 */

#define SECCOMP_MODE_DISABLED 0u
#define SECCOMP_MODE_STRICT   1u
#define SECCOMP_MODE_FILTER   2u

struct seccomp_filter;

/**
 * Apply the current thread's mode and filters to the call `nr` it is
 * making, whose arguments are in `tf`. Returns 1 when the call is to run;
 * 0 when it is not, with its result in `*ret`. In the strict mode a call
 * other than read, write, exit and sigreturn ends the process with SIGKILL.
 * A filter's kill does not return, its errno, at most 4095, is the call's
 * error, and its trap posts SIGSYS with the result register back at the
 * call's number.
 */
int seccomp_permit(isr_params_t *tf, uint32_t nr, long *ret);

/**
 * seccomp(2) takes the operation, its flags and its argument.
 *
 * SECCOMP_SET_MODE_STRICT, with no flags and no argument, leaves the thread
 * only read, write, exit and sigreturn; any other call ends the process
 * with SIGKILL. SECCOMP_SET_MODE_FILTER adds the filter described by the
 * `struct sock_fprog` at the argument. SECCOMP_GET_ACTION_AVAIL returns 0
 * for an action the filters may return and -EOPNOTSUPP for another, and
 * SECCOMP_GET_NOTIF_SIZES stores Linux's three sizes. SECCOMP_RET_USER_NOTIF
 * is not one of the actions, since a filter cannot be installed with a
 * listener (SECCOMP_FILTER_FLAG_NEW_LISTENER is -EINVAL), and a filter that
 * returns it fails the call with ENOSYS, as Linux does without a listener.
 * Another operation, or flags the operation does not take, is -EINVAL, and
 * an argument that cannot be read or written is -EFAULT.
 */
long sys_seccomp(isr_params_t *params);

/** prctl's PR_SET_SECCOMP: `mode` strict, or filter with the program at the
 * user address `prog`, as seccomp(2) sets them without flags. 0, or -EINVAL
 * for another mode or the strict mode after a filter, or an error of the
 * filter's. */
int32_t seccomp_prctl_set(uint32_t mode, uintptr_t prog);

/** The child `child` takes `parent`'s mode and chain. */
void seccomp_inherit(process_t *child, const process_t *parent);

/** Drop `p`'s reference to its chain, freeing the filters no other thread
 * or filter refers to. */
void seccomp_release(process_t *p);

/** The number of filters in `p`'s chain. */
uint32_t seccomp_filter_count(const process_t *p);

#endif /* _SECCOMP_H_ */
