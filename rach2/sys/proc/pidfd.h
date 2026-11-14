#ifndef _PIDFD_H_
#define _PIDFD_H_

#include "../lib/kint.h"

/*
 * pidfd: a descriptor that names a process, or with PIDFD_THREAD one thread
 * of it, which pidfd_open(2) and clone's CLONE_PIDFD make. Unlike a pid, it
 * cannot come to name another process once its own has been reaped and its
 * number given out again: it holds the process's thread group
 * (`tgroup.h`), and finds the thread by its id only while the record there
 * is a thread of that group. poll and epoll report a pidfd of a process
 * readable once every thread of the process has ended, and one of a thread
 * once that thread has, and each hung up as well once its thread has let
 * its id go: the process reaped, or the thread freed. pidfd_send_signal(2)
 * signals the process, or the thread for a pidfd of a thread, waitid(2)
 * with P_PIDFD waits for it, and pidfd_getfd(2) copies one of the process's
 * descriptors. A read or a write is EINVAL.
 */

struct isr_params;
struct process;
struct pidfd;
struct pidns;
struct tgroup;

/* A hold on the process a pidfd names that outlasts the descriptor, for a
 * call that may sleep between finding the pidfd and acting on its process:
 * the pidfd's thread group and the process's PID namespace, each held by a
 * reference, and the kernel id of the thread it names, the leader's for a
 * pidfd of a process. */
struct pidfd_ref {
    struct tgroup *tg;
    struct pidns *ns;
    uint32_t pid;
    uint32_t thread; /* a pidfd of the one thread (PIDFD_THREAD) */
};

/** A pidfd of the process of `p`, or of the thread `p` alone when `thread`
 * is not zero, holding its thread group, not yet given a descriptor; NULL
 * when the heap has no room. */
struct pidfd *pidfd_new(struct process *p, int thread);

/** Put `pf` in the reserved descriptor `fd` of the current process, which
 * `fd_alloc` gave, close-on-exec, non-blocking when `nonblock` is not zero,
 * and with O_EXCL among its status flags for a pidfd of one thread, as
 * Linux keeps PIDFD_THREAD in f_flags. The descriptor then owns `pf`. */
void pidfd_install(struct pidfd *pf, int fd, int nonblock);

/** Free `pf`, which `pidfd_install` never took, with its hold on the
 * thread group. */
void pidfd_free(struct pidfd *pf);

/**
 * The thread the caller's descriptor `fd` names: 0 with `*out` the thread,
 * the leader for a pidfd of a process, or NULL once it has let its id go,
 * and `*nonblock` whether the descriptor is non-blocking, when `nonblock`
 * is not NULL; -EBADF for a descriptor that is not open or is not a pidfd,
 * which sets neither.
 */
int32_t pidfd_lookup(int fd, struct process **out, int *nonblock);

/** The process a thread `t` that `pidfd_lookup` found stands for, as
 * Linux's pidfd_get_task looks up the pidfd's pid as a thread group id:
 * `t` when it leads its process, else NULL, which a call that acts on a
 * whole process answers with -ESRCH. */
struct process *pidfd_process(struct process *t);

/** Hold in `*ref` the process the caller's descriptor `fd` names, until
 * `pidfd_ref_put`, whatever happens to the descriptor meanwhile: 0, or
 * -EBADF for a descriptor that is not open or is not a pidfd, which holds
 * nothing. */
int32_t pidfd_ref_get(int fd, struct pidfd_ref *ref);

/** The thread `ref` holds, the leader for a pidfd of a process, or NULL
 * once it has let its id go. */
struct process *pidfd_ref_task(const struct pidfd_ref *ref);

/** Whether the caller's PID namespace, or one above it, has the process
 * `ref` holds, reaped or not, as Linux's access_pidfd_pidns asks. */
int pidfd_ref_seen(const struct pidfd_ref *ref);

/** Let go of the thread group and the namespace `ref` holds. */
void pidfd_ref_put(struct pidfd_ref *ref);

/** pidfd_open(2) takes a pid and the flags PIDFD_NONBLOCK and
 * PIDFD_THREAD: a close-on-exec pidfd of the process, or with PIDFD_THREAD
 * of the thread with that id, -EINVAL for another flag, a pid not above
 * zero or, without PIDFD_THREAD, a thread that does not lead its process,
 * -ESRCH for no thread with the id (`proc_tid_gone`), -ENOMEM or -EMFILE,
 * in the order of Linux 6.12's pidfd_open. */
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
