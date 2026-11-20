#ifndef _EVENTFD_H_
#define _EVENTFD_H_

#include "../lib/kint.h"

/*
 * eventfd: a descriptor over a 64-bit counter, the simplest way for one
 * thread or process to wake another. A write of eight bytes adds its value
 * to the counter, and a read of eight bytes takes the counter and sets it to
 * zero, or with EFD_SEMAPHORE takes one; a read while the counter is zero
 * waits, and so does a write that would carry the counter past
 * 0xfffffffffffffffe. poll reports the descriptor readable while the
 * counter is not zero, writable while another 1 fits, and in error at
 * 0xffffffffffffffff, which only `eventfd_signal` reaches.
 */

struct isr_params;
struct eventfd;
struct fd_entry;

/*
 * The kernel's side, for irqfd and ioeventfd (rach2/sys/kvm/), VFIO's
 * interrupts and a transport's doorbells, as Linux's eventfd_ctx. A holder
 * takes a reference from a descriptor of the caller's (`eventfd_get`),
 * which keeps the counter after the descriptor closes, and drops it with
 * `eventfd_put`. A hook on the counter (`eventfd_hook_add`) is told of
 * each rise, by a program's write or the kernel's `eventfd_signal`, and of
 * the last close of the descriptor. The counter and the hooks have a
 * spinlock, so `eventfd_signal` may be called from an interrupt handler,
 * and so do the references, which `eventfd_get` and `eventfd_put` take and
 * drop with or without the big lock; adding and removing a hook is done
 * under it.
 */
struct eventfd_hook {
    /* The counter rose: called with the counter's lock held and
     * interrupts off, from the writer's or the signaller's context, so it
     * must not sleep or call the eventfd's functions. Return 1 to take the
     * counter, as a read would, Linux's eventfd_ctx_do_read in irqfd's
     * wakeup, or 0 to leave it. */
    int (*signal)(struct eventfd_hook *h);
    /* The last descriptor closed: called once, under the big lock, which
     * the close takes for it, after the hook has left the counter's list;
     * NULL for none. It may sleep. */
    void (*hangup)(struct eventfd_hook *h);
    struct eventfd_hook *next; /* the counter's list */
};

/** A reference to the eventfd the caller's descriptor `fd` is open on, or
 * NULL with `*err` -EBADF for no such descriptor or -EINVAL for one that is
 * not an eventfd, as Linux's eventfd_ctx_fdget. */
struct eventfd *eventfd_get(int fd, int32_t *err);

/** `eventfd_get` of an entry the caller has looked up, NULL for none, so
 * that a caller that also keeps the entry's description takes both from
 * the one lookup, which a close between two lookups cannot split. */
struct eventfd *eventfd_entry_get(const struct fd_entry *e, int32_t *err);

/** Drop a reference `eventfd_get` took; the last one, after the last
 * close, frees the counter. */
void eventfd_put(struct eventfd *ev);

/** Add 1 to the counter, as Linux's eventfd_signal, waking the readers and
 * telling the hooks; nothing at 0xFFFFFFFFFFFFFFFF. Safe in an interrupt
 * handler. */
void eventfd_signal(struct eventfd *ev);

/** As `eventfd_signal`, passing over `skip` if it is on this counter.
 * A driver uses this when its input hook and notification share a counter. */
void eventfd_signal_except(struct eventfd *ev, const struct eventfd_hook *skip);

/** Put `h` on the counter's list: 1 when the counter is above 0 already,
 * which the caller acts on as on a signal, else 0. After the last close,
 * `h` is not added and its hangup runs at once: -1. */
int eventfd_hook_add(struct eventfd *ev, struct eventfd_hook *h);

/** Take `h` off the counter's list: 1 when it was on it, 0 when it was
 * not, as after the last close, whose hangup of `h` then runs or has run.
 * The counter's value, taken as a read would, goes into `*cnt` unless
 * NULL, as Linux's eventfd_ctx_remove_wait_queue. */
int eventfd_hook_remove(struct eventfd *ev, struct eventfd_hook *h, uint64_t *cnt);

/** eventfd2(2) takes the initial value and the flags EFD_CLOEXEC,
 * EFD_NONBLOCK and EFD_SEMAPHORE, and eventfd(2) the value alone. Returns
 * the new descriptor, -EINVAL for another flag, -ENOMEM or -EMFILE. */
long sys_eventfd(struct isr_params *params);
long sys_eventfd2(struct isr_params *params);

#endif /* _EVENTFD_H_ */
