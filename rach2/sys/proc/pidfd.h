#ifndef _PIDFD_H_
#define _PIDFD_H_

#include "../lib/kint.h"

/*
 * pidfd: a descriptor that names a process, which pidfd_open(2) and clone's
 * CLONE_PIDFD make. Unlike a pid, it cannot come to name another process
 * once its own has been reaped and its number given out again: it holds the
 * process's thread group (`tgroup.h`), and finds the process by its id only
 * while the record there is a thread of that group. poll and epoll report
 * it readable once every thread of the process has ended, and hung up as
 * well once the process has been reaped. pidfd_send_signal(2) signals the
 * process, waitid(2) with P_PIDFD waits for it, and pidfd_getfd(2) copies
 * one of its descriptors. A read or a write is EINVAL.
 */

struct isr_params;
struct process;
struct pidfd;

/** A pidfd of the process `p` leads, holding its thread group, not yet
 * given a descriptor; NULL when the heap has no room. */
struct pidfd *pidfd_new(struct process *p);

/** Put `pf` in the reserved descriptor `fd` of the current process, which
 * `fd_alloc` gave, close-on-exec, and non-blocking when `nonblock` is not
 * zero. The descriptor then owns `pf`. */
void pidfd_install(struct pidfd *pf, int fd, int nonblock);

/** Free `pf`, which `pidfd_install` never took, with its hold on the
 * thread group. */
void pidfd_free(struct pidfd *pf);

/**
 * The process the caller's descriptor `fd` names: 0 with `*out` its leader,
 * or NULL once it has been reaped, and `*nonblock` whether the descriptor is
 * non-blocking, when `nonblock` is not NULL; -EBADF for a descriptor that
 * is not open or is not a pidfd.
 */
int32_t pidfd_lookup(int fd, struct process **out, int *nonblock);

/** pidfd_open(2) takes a pid and the flag PIDFD_NONBLOCK: a close-on-exec
 * pidfd of the process, -EINVAL for a pid not above zero, another flag or a
 * thread that does not lead its process, -ESRCH for none, -ENOMEM or
 * -EMFILE. */
long sys_pidfd_open(struct isr_params *params);

/**
 * pidfd_getfd(2) takes a pidfd, a descriptor of its process and flags,
 * which must be 0: a close-on-exec copy of that descriptor in the caller's
 * table, sharing its open file description, as dup(2) makes one. The caller
 * needs the process to be its own, or the right to trace it. -EINVAL for a
 * flag, -EBADF for no pidfd or no such descriptor, which a process that has
 * ended has none of, -ESRCH for a process reaped, -EPERM, -EMFILE or
 * -ENOMEM.
 */
long sys_pidfd_getfd(struct isr_params *params);

#endif /* _PIDFD_H_ */
