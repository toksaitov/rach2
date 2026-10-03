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
 * Judge the call `nr` the current thread is making, whose arguments are in
 * `tf`, by its mode and filters. Returns 1 when the call is to run; 0 when
 * it is not, with its result in `*ret`. A filter's kill does not return,
 * and its trap posts SIGSYS with EAX back at the call's number.
 */
int seccomp_permit(isr_params_t *tf, uint32_t nr, long *ret);

/** seccomp (354): set the strict mode or add a filter, and the two queries. */
long sys_seccomp(isr_params_t *params);

/** prctl's PR_SET_SECCOMP: `mode` strict, or filter with the program at the
 * user address `prog`. */
int32_t seccomp_prctl_set(uint32_t mode, uint32_t prog);

/** The child `child` takes `parent`'s mode and chain. */
void seccomp_inherit(process_t *child, const process_t *parent);

/** Drop `p`'s reference to its chain, freeing the filters no one else
 * holds. */
void seccomp_release(process_t *p);

/** The number of filters in `p`'s chain. */
uint32_t seccomp_filter_count(const process_t *p);

#endif /* _SECCOMP_H_ */
