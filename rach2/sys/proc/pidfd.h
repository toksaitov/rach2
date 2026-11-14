#ifndef _PIDFD_H_
#define _PIDFD_H_

#include "../lib/kint.h"

/*
 * pidfd: a descriptor that names a process, or with PIDFD_THREAD one thread
 * of it, which pidfd_open(2) and clone's CLONE_PIDFD make. Unlike a pid, it
 * cannot come to name another process or thread once its own has let the
 * number go and it is given out again: it holds the process's thread group
 * (`tgroup.h`) and the `serial` of the id's lifetime (process.h), and finds
 * the thread by its id only while the record there is a thread of that
 * group with that serial. poll and epoll report a pidfd of a process
 * readable once every thread of the process has ended, and one of a thread
 * once that thread has, and each hung up as well once its thread has let
 * its id go: the process reaped, or the thread freed. A pidfd's pollers
 * are woken only by a change of its own id's lifetime, as Linux 6.12's
 * do_notify_pidfd wakes the queue of one struct pid. pidfd_send_signal(2)
 * signals the process, or the thread for a pidfd of a thread, waitid(2)
 * with P_PIDFD waits for it, and pidfd_getfd(2) copies one of the
 * descriptors of the pidfd's thread. A read or a write is EINVAL.
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
 * pidfd of a process, with the lifetime of that id. */
struct pidfd_ref {
    struct tgroup *tg;
    struct pidns *ns;
    uint32_t pid;
    uint32_t thread; /* a pidfd of the one thread (PIDFD_THREAD) */
    uint64_t serial; /* the `serial` (process.h) of `pid`'s lifetime */
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

/** Wake the pollers of the pidfds that the end of the thread `t`, a zombie
 * now, changes: those of its id's lifetime, and those of its process's
 * leader when `t` is the last thread to end, as Linux 6.12's exit_notify
 * and do_notify_parent call do_notify_pidfd. A thread whose id goes with
 * its end (`proc_tid_gone`) is marked told (`pidfd_told`), so that the free
 * of its record wakes them no more. The caller holds the big lock. */
void pidfd_exit(struct process *t);

/** Wake the pollers of the pidfds of `p`'s id, which `p` lets go: at the
 * free of its record or the detach of a zombie thread by its tracer, as
 * Linux 6.12's __change_pid wakes wait_pidfd at the release; once only,
 * so not when `pidfd_exit` has told them already. The caller holds the big
 * lock. */
void pidfd_id_gone(struct process *p);

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
 * pidfd_getfd(2) takes a pidfd, a descriptor of its thread, the leader
 * for a pidfd of a process, and flags, which must be 0: a close-on-exec
 * copy of that descriptor in the caller's table, sharing its open file
 * description, as dup(2) makes one. The thread is the pidfd's by its id's
 * lifetime, as Linux 6.12's pidfd_getfd takes the task by PIDTYPE_PID, so a
 * pidfd of a thread that does not lead its process copies from it too. The
 * caller needs the thread to be of its own process, or the right to trace
 * it. -EINVAL for a flag, -EBADF for no pidfd or no such descriptor, which
 * a thread that has ended has none of, -ESRCH for a process reaped or a
 * thread that has let its id go, -EPERM, -EMFILE or -ENOMEM.
 */
long sys_pidfd_getfd(struct isr_params *params);

#endif /* _PIDFD_H_ */
