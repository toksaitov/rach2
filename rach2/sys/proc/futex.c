/*
 * futex.c: futex(2), the robust lists, and the part of a thread's end that
 * concerns futexes, as futex.h and futex.Readme.md describe them.
 */
#include "futex.h"

#include "ns.h"
#include "process.h"
#include "wait_queue.h"
#include "../mm/mm.h"
#include "../mm/kheap.h"
#include "../fs/krffs.h"
#include "../fs/vfs.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../mm/paging.h"
#include "../mm/usercopy.h"
#include "../syscall.h"
#include "../syscall_internal.h"
#include "../rtc.h"
#include "../timer.h"

/* The operations of futex(2) and its flags, Linux's values; FUTEX_MATCH_ANY
 * is the bitset of a plain wait or wake, which meets every other. */
#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_WAIT_BITSET    9
#define FUTEX_WAKE_BITSET    10
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_MATCH_ANY      0xFFFFFFFFu

/* futex_waitv's flags for each futex, Linux's FUTEX2_ values: the word's
 * size, of which only 32 bits is taken, and FUTEX2_PRIVATE, which is
 * FUTEX_PRIVATE_FLAG. FUTEX_WAITV_MAX is the most futexes one call names. */
#define FUTEX2_SIZE_U32  0x02u
#define FUTEX2_SIZE_MASK 0x03u
#define FUTEX2_PRIVATE   0x80u
#define FUTEX_WAITV_MAX  128u

/* The clocks a futex_waitv deadline is read on, Linux's clockid_t values. */
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

/* The bits of a robust futex's word beside the owner's thread id. */
#define FUTEX_WAITERS    0x80000000u
#define FUTEX_OWNER_DIED 0x40000000u
#define FUTEX_TID_MASK   0x3FFFFFFFu

/* Policy: the entries a thread's end walks on its robust list, Linux's
 * ROBUST_LIST_LIMIT; a longer list, or a loop in one, is left there. */
#define ROBUST_LIST_LIMIT 2048

/* The head of a robust list, as glibc lays it out: the first entry, the
 * distance from an entry to its futex word, and the entry being taken or
 * released when the thread ended, if any. An i386 call's list on x86-64 has
 * the head of 32-bit words, struct robust_head32 (futex.h). */
struct robust_head {
    uintptr_t next;
    long futex_offset;
    uintptr_t op_pending;
};

/* One futex of a futex_waitv call, as Linux's struct futex_waitv lays it
 * out, alike in both ABIs. */
struct futex_waitv {
    uint64_t val;      /* the value expected, which must fit the word */
    uint64_t uaddr;    /* the word's address */
    uint32_t flags;    /* FUTEX2_SIZE_U32, with FUTEX2_PRIVATE or without */
    uint32_t reserved; /* must be 0 */
};

/* Linux's struct __kernel_timespec, whose seconds are 64 bits in both ABIs. */
struct futex_timespec64 {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* The name futexes in shared memory have instead of an address space. */
static const char futex_shared_space;

/* Policy: the hash buckets waiters are filed in. A wake walks one bucket, so
 * the count bounds the walk and not the number of waiters. */
#define FUTEX_BUCKETS 64

/* What a waiting thread sleeps on: one record however many futexes it
 * waits on, so that a wake of any of them ends the sleep. */
struct futex_sleeper {
    int woken;       /* a wake marked one of its waiters */
    int timed;       /* the wait has a deadline */
    uint64_t target; /* the tick the sleep ends at, with `timed` */
    wait_queue_t wq; /* the sleeper's own queue, which only it sleeps on */
};

/* A waiter filed on one futex, on its thread's kernel stack or in the
 * vector of a futex_waitv call while the thread sleeps. */
struct futex_q {
    const void *space; /* the address space's page directory, or shared memory */
    uintptr_t uaddr;   /* the address in it, or the frame and offset */
    uint32_t bitset;   /* a wake's bitset must meet it */
    int woken;         /* a wake marked it and took it off its list */
    struct futex_sleeper *s;
    struct futex_q *next;
};

/* One futex of a wait, as the caller named it, and its waiter. */
struct futex_vec {
    uintptr_t addr; /* the word's user address, aligned to four bytes */
    uint32_t val;   /* the value the word must hold for the wait to sleep */
    int shared;     /* named as futex_key names a futex that may be shared */
    struct futex_q q;
};

/* Each bucket's waiters, the newest first. */
static struct futex_q *_waiters[FUTEX_BUCKETS];

/** The bucket a futex named `space` and `uaddr` is filed in. */
static uint32_t futex_bucket(const void *space, uintptr_t uaddr)
{
    return (uint32_t)((((uintptr_t)space >> 4) ^ (uaddr >> 2) ^ (uaddr >> 12)) % FUTEX_BUCKETS);
}

/* The ticks from now until the absolute time of `sec` seconds and `nsec`
 * nanoseconds, under a second, on the real-time clock, or on the monotonic
 * clock without `realtime`; 0 when it has passed. */
static uint64_t futex_ticks_until(int64_t at_sec, uint32_t at_nsec, int realtime)
{
    int64_t sec;
    int64_t nsec;
    int64_t dsec;
    int32_t dnsec;
    uint32_t seconds32, ticks32;
    uint64_t total;

    if (realtime) {
        get_current_time(&sec, &nsec);
    } else {
        tick_now(&seconds32, &ticks32, &total);
        sec = seconds32;
        nsec = (int64_t)ticks32 * TICK_NS;
    }
    dsec = at_sec - sec;
    dnsec = (int32_t)at_nsec - (int32_t)nsec;
    if (dnsec < 0) {
        dnsec += 1000000000;
        dsec--;
    }
    if (dsec < 0 || (dsec == 0 && dnsec == 0)) {
        return 0;
    }
    return tick_span(dsec, (uint32_t)dnsec);
}

/**
 * The time `ts` names, as seconds and nanoseconds under a second in `*sec`
 * and `*nsec`: nanoseconds out of that range carry their whole seconds into
 * the seconds, as Linux's timespec64_to_ktime takes them, since futex_waitv
 * does not refuse such a time. The seconds are first cut to 2^40 either way,
 * where tick_span cuts a span, so the sum cannot overflow.
 */
static void futex_ts_norm(const struct futex_timespec64 *ts, int64_t *sec, uint32_t *nsec)
{
    const int64_t cap = (int64_t)1 << 40;
    uint64_t mag, q;
    uint32_t r1, r2, rem;

    *sec = ts->tv_sec > cap ? cap : (ts->tv_sec < -cap ? -cap : ts->tv_sec);
    if (ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000) {
        *nsec = (uint32_t)ts->tv_nsec;
        return;
    }
    /* 10^9 is 15625 * 64000, two divisors tick_div takes. */
    mag = ts->tv_nsec < 0 ? (uint64_t)0 - (uint64_t)ts->tv_nsec : (uint64_t)ts->tv_nsec;
    q = tick_div(tick_div(mag, 15625u, &r1), 64000u, &r2);
    rem = r2 * 15625u + r1;
    if (ts->tv_nsec >= 0) {
        *sec += (int64_t)q;
        *nsec = rem;
        return;
    }
    *sec -= (int64_t)q;
    *nsec = 0;
    if (rem != 0) {
        *sec -= 1;
        *nsec = 1000000000u - rem;
    }
}

/**
 * The name of the futex at `uaddr`, whose page is resident: its address space
 * and address, or, when `shared` and the page is in a shared mapping, the
 * page's frame and the offset in it, which every process mapping that page
 * names alike. A futex word in memory two processes share, a
 * PTHREAD_PROCESS_SHARED mutex in /dev/shm among them, is named the second
 * way unless the caller passed FUTEX_PRIVATE_FLAG.
 */
static void futex_key(uintptr_t uaddr, int shared, const void **space, uintptr_t *key)
{
    const vma_t *r;
    uintptr_t phys;

    *space = current_proc->page_dir;
    *key = uaddr;
    if (!shared || current_proc->mm == NULL) {
        return;
    }
    r = vm_find(&current_proc->mm->vm, uaddr);
    if (r == NULL || r->type != VM_SHARED) {
        return;
    }
    phys = paging_user_phys(current_proc->page_dir, uaddr);
    if (phys != 0) {
        *space = &futex_shared_space;
        *key = phys;
    }
}

/** The wait's condition: a waiter was woken, or the deadline has come. */
static int futex_sleeper_done(void *arg)
{
    const struct futex_sleeper *s = (const struct futex_sleeper *)arg;

    return s->woken || (s->timed && tick_count() >= s->target);
}

/* Take q off its bucket's list, when a wake has not already. */
static void futex_unlink(struct futex_q *q, uint32_t bucket)
{
    struct futex_q **pp;

    for (pp = &_waiters[bucket]; *pp; pp = &(*pp)->next) {
        if (*pp == q) {
            *pp = q->next;
            return;
        }
    }
}

/**
 * Take the first `n` waiters of `v` off their lists, with interrupts off;
 * the index of the last one a wake had marked, or -1 when none was.
 */
static int futex_unqueue(struct futex_vec *v, uint32_t n)
{
    uint32_t i;
    int last = -1;

    for (i = 0; i < n; i++) {
        if (v[i].q.woken) {
            last = (int)i;
        } else {
            futex_unlink(&v[i].q, futex_bucket(v[i].q.space, v[i].q.uaddr));
        }
    }
    return last;
}

/**
 * Sleep on the `n` futexes of `v` while each word holds its value, until a
 * wake of any of them, the deadline `s` has, or a signal, as Linux's
 * futex_wait_multiple does. `s` has `timed` and `target` set, and `ticks`
 * is 0 for a deadline that had passed when the call began.
 *
 * Each word's page is made resident, which may sleep, and with interrupts
 * off every page is then found still resident, each futex named and its word
 * compared, and the waiters filed, so no read sleeps between a comparison
 * and the filing: a wake after the comparison finds the waiter. A page that
 * left while another was brought in starts the round again.
 *
 * Returns the index of the woken futex, the last of them when a wake marked
 * more than one; -EAGAIN when a word differs, -ETIMEDOUT at the deadline,
 * -EINTR for a signal, or -EFAULT for a word the process cannot read.
 */
static int futex_sleep_on(struct futex_vec *v, uint32_t n, struct futex_sleeper *s, uint64_t ticks)
{
    uint32_t i, cur, flags;
    int rc, woken;

    s->woken = 0;
    wait_queue_init(&s->wq);
    for (;;) {
        for (i = 0; i < n; i++) {
            if (!vm_prefault_range(v[i].addr, sizeof(uint32_t), 0)) {
                return -EFAULT;
            }
        }
        flags = kirq_save();
        for (i = 0; i < n; i++) {
            if (paging_user_phys(current_proc->page_dir, v[i].addr) == 0) {
                break;
            }
        }
        if (i == n) {
            break;
        }
        kirq_restore(flags);
    }

    for (i = 0; i < n; i++) {
        struct futex_q *q = &v[i].q;
        uint32_t bucket;

        if (get_user_u32(&cur, v[i].addr) != 0) {
            rc = -EFAULT;
        } else if (cur != v[i].val) {
            rc = -EAGAIN;
        } else {
            rc = 0;
        }
        if (rc != 0) {
            (void)futex_unqueue(v, i);
            kirq_restore(flags);
            return rc;
        }
        futex_key(v[i].addr, v[i].shared, &q->space, &q->uaddr);
        q->woken = 0;
        q->s = s;
        bucket = futex_bucket(q->space, q->uaddr);
        q->next = _waiters[bucket];
        _waiters[bucket] = q;
    }
    if (s->timed && (ticks == 0 || tick_count() >= s->target)) {
        (void)futex_unqueue(v, n);
        kirq_restore(flags);
        return -ETIMEDOUT;
    }
    kirq_restore(flags);

    current_proc->wakeup_tick = s->timed ? (s->target != 0 ? s->target : 1u) : 0;
    rc = wq_wait_event(&s->wq, futex_sleeper_done, s);
    current_proc->wakeup_tick = 0;

    flags = kirq_save();
    woken = futex_unqueue(v, n);
    kirq_restore(flags);
    if (woken >= 0) {
        return woken;
    }
    if (rc != 0) {
        return rc;
    }
    return -ETIMEDOUT;
}

/**
 * Sleep on `uaddr` while it holds `val`.
 *
 * `utime` is a relative timeout, or an absolute one with `absolute`, on the
 * monotonic clock or, with FUTEX_CLOCK_REALTIME in `flags_op`, on the
 * real-time clock; `flags_op` has FUTEX_PRIVATE_FLAG for a futex no other
 * process maps. The deadline's tick is fixed before the word's page is
 * brought in, so the time a fault sleeps counts against it, and a deadline
 * the fault outlasted ends the call. Returns 0 when woken, -EAGAIN when the
 * word differs, -ETIMEDOUT at the deadline, -EINTR for a signal, -EINVAL for
 * a timeout out of range and -EFAULT for an address or a timeout the process
 * cannot read.
 */
static int32_t futex_wait(uintptr_t uaddr, uint32_t val, const struct kernel_timespec __user *utime,
                          uint32_t bitset, int absolute, int flags_op)
{
    struct futex_vec v;
    struct futex_sleeper s;
    struct kernel_timespec ts;
    uint64_t ticks = 0;
    int rc;

    s.timed = 0;
    if (utime) {
        if (copy_from_user(&ts, utime, sizeof(ts)) != 0) {
            return -EFAULT;
        }
        if (sizeof(long) > 4u && in_compat_syscall()) {
            /* futex_time64's high word of the nanoseconds, which Linux
             * ignores for an i386 caller (get_timespec64). */
            ts.tv_nsec = (long)(uint32_t)ts.tv_nsec;
        }
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) {
            return -EINVAL;
        }
        ticks = absolute ? futex_ticks_until(ts.tv_sec, (uint32_t)ts.tv_nsec,
                                             (flags_op & FUTEX_CLOCK_REALTIME) != 0)
                         : tick_span(ts.tv_sec, (uint32_t)ts.tv_nsec);
        s.timed = 1;
    }
    s.target = rtc_ticks_now() + ticks + 1u;

    v.addr = uaddr;
    v.val = val;
    v.shared = !(flags_op & FUTEX_PRIVATE_FLAG);
    v.q.bitset = bitset;
    rc = futex_sleep_on(&v, 1, &s, ticks);
    return rc > 0 ? 0 : rc;
}

/**
 * Wake up to `n` waiters on the futex at `uaddr` whose bitset meets
 * `bitset`, named as `futex_key` names it with `flags_op`; the number
 * woken, or -EFAULT when a futex that may be shared has a page that cannot
 * be brought in.
 *
 * Each waiter marked is woken on its sleeper's own queue, so the bucket's
 * other sleepers stay asleep. The waiter and its sleeper are in memory the
 * sleeping thread frees once it has left its wait, and the wake comes before
 * interrupts are on again: the thread cannot leave its wait before then,
 * since the big lock keeps it from running on another processor and
 * interrupts off keep the tick from switching to it here.
 */
static int futex_wake(uintptr_t uaddr, uint32_t n, uint32_t bitset, int flags_op)
{
    const void *space;
    uint32_t bucket;
    struct futex_q **pp;
    uint32_t flags;
    int woken = 0;

    if (!(flags_op & FUTEX_PRIVATE_FLAG) && !vm_prefault_range(uaddr, sizeof(uint32_t), 0)) {
        return -EFAULT;
    }
    futex_key(uaddr, !(flags_op & FUTEX_PRIVATE_FLAG), &space, &uaddr);
    bucket = futex_bucket(space, uaddr);

    flags = kirq_save();
    pp = &_waiters[bucket];
    while (*pp && (uint32_t)woken < n) {
        struct futex_q *q = *pp;

        if (q->space == space && q->uaddr == uaddr && (q->bitset & bitset) != 0) {
            q->woken = 1;
            q->s->woken = 1;
            *pp = q->next;
            woken++;
            wq_wake_all(&q->s->wq);
        } else {
            pp = &q->next;
        }
    }
    kirq_restore(flags);
    return woken;
}

int futex_restartable(const isr_params_t *params)
{
    int cmd = (int)SC_ARG(params, 1) & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);

    return !((cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) && SC_ARG(params, 3) != 0);
}

/**
 * Release the robust futex whose word is at `uaddr` if thread `tid` holds
 * it, as Linux's `handle_futex_death` does: the word keeps its waiters bit,
 * gains FUTEX_OWNER_DIED in place of the id, and one waiter is woken, so the
 * next owner learns from EOWNERDEAD that the state the lock guarded may be
 * half made. A word that cannot be read or written is left.
 *
 * With `pending`, the word is that of the ordinary futex the thread was
 * taking or giving back when it ended. When that word has no owner, the
 * thread ended after an unlock stored it and before the unlock's wake, or
 * after a wake and before it took the lock; either way a wake is owed. One
 * waiter is woken, and the word is left as it is, since FUTEX_OWNER_DIED in
 * it would tell the next owner of a death that left the lock's state whole.
 *
 * The word's page is made present and writable first, and the word is then
 * read, compared and written with interrupts off, so no fault sleeps between
 * the read and the write. A waiter that set FUTEX_WAITERS during such a
 * sleep would have its bit written over and would not be woken. The word is
 * read in one load and written in one compare-and-exchange, read again when
 * it changed meanwhile, as Linux's does: a thread on another processor may
 * take the lock or set FUTEX_WAITERS between the two, and a write of its
 * bytes one at a time would let it see a word that is neither.
 */
static void robust_release(uintptr_t uaddr, uint32_t tid, int pending)
{
    uint32_t word, died, seen;
    uint32_t flags;
    int wake = 0;

    if ((uaddr & 3u) != 0 || !vm_prefault_range(uaddr, sizeof(word), 1)) {
        return;
    }
    flags = kirq_save();
    while (get_user_u32(&word, uaddr) == 0) {
        if (pending && (word & FUTEX_TID_MASK) == 0) {
            wake = 1;
            break;
        }
        if ((word & FUTEX_TID_MASK) != tid) {
            break;
        }
        died = (word & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
        if (cmpxchg_user_u32(uaddr, word, died, &seen) != 0) {
            break;
        }
        if (seen == word) {
            wake = (died & FUTEX_WAITERS) != 0;
            break;
        }
    }
    kirq_restore(flags);
    if (wake) {
        (void)futex_wake(uaddr, 1, FUTEX_MATCH_ANY, 0);
    }
}

/** Read the robust list's pointer at `u` into `*v`: a native word, or with
 * `narrow` a 32-bit one, zero-extended as Linux's compat_ptr. 0 or
 * -EFAULT. */
static int robust_ptr(uintptr_t u, int narrow, uintptr_t *v)
{
    uint32_t w;

    if (!narrow) {
        return copy_from_user(v, (const void __user *)u, sizeof(*v)) != 0 ? -EFAULT : 0;
    }
    if (copy_from_user(&w, (const void __user *)u, sizeof(w)) != 0) {
        return -EFAULT;
    }
    *v = w;
    return 0;
}

/** The futex word of the robust list entry `entry` with the list's
 * `offset`, cut to 32 bits for a list of `narrow` words. */
static uintptr_t robust_uaddr(uintptr_t entry, long offset, int narrow)
{
    uintptr_t u = entry + (uintptr_t)offset;

    if (narrow) {
        u &= 0xFFFFFFFFu;
    }
    return u;
}

/**
 * Walk the robust list at `list` of the current thread `p`, which is ending
 * or committing an execve, and release each futex on it that the thread
 * still holds, then the one it was taking or giving back, a waiter of which
 * is also woken when it has no owner and is not a priority-inheritance
 * futex. The list is circular through its head; the low bit of an entry
 * marks a priority-inheritance futex and is not part of the address. With
 * `narrow` the list is an i386 call's, of 32-bit words, and an entry's word
 * is its address plus the offset cut to 32 bits, as Linux's
 * `compat_exit_robust_list` finds it.
 */
static void robust_walk(process_t *p, uintptr_t list, int narrow)
{
    struct robust_head head;
    uintptr_t entry, pending;
    int left = ROBUST_LIST_LIMIT;

    if (narrow) {
        struct robust_head32 h32;

        if (copy_from_user(&h32, (const void __user *)list, sizeof(h32)) != 0) {
            return;
        }
        head.next = h32.next;
        head.futex_offset = h32.futex_offset;
        head.op_pending = h32.op_pending;
    } else if (copy_from_user(&head, (const void __user *)list, sizeof(head)) != 0) {
        return;
    }
    entry = head.next & ~(uintptr_t)1;
    pending = head.op_pending & ~(uintptr_t)1;
    while (entry != list && entry != 0 && left-- > 0) {
        uintptr_t next;

        if (robust_ptr(entry, narrow, &next) != 0) {
            break;
        }
        if (entry != pending) {
            robust_release(robust_uaddr(entry, head.futex_offset, narrow), p->pid, 0);
        }
        entry = next & ~(uintptr_t)1;
    }
    if (pending != 0) {
        robust_release(robust_uaddr(pending, head.futex_offset, narrow), p->pid,
                       (head.op_pending & 1u) == 0);
    }
}

void proc_thread_gone(process_t *p)
{
    uint32_t zero = 0;

    if (p->robust_list != 0) {
        robust_walk(p, p->robust_list, 0);
    }
    if (p->compat_robust_list != 0) {
        robust_walk(p, p->compat_robust_list, 1);
    }
    p->robust_list = 0;
    p->compat_robust_list = 0;
    if (p->clear_child_tid != 0 && p->mm != NULL && p->mm->users > 1 &&
        copy_to_user((void __user *)p->clear_child_tid, &zero, sizeof(zero)) == 0) {
        (void)futex_wake(p->clear_child_tid, 1, FUTEX_MATCH_ANY, 0);
    }
    p->clear_child_tid = 0;
}

long sys_set_robust_list(isr_params_t *params)
{
    if (!current_proc) {
        return -ESRCH;
    }
    if (sizeof(long) > 4u && in_compat_syscall()) {
        if (SC_ARG(params, 1) != sizeof(struct robust_head32)) {
            return -EINVAL;
        }
        current_proc->compat_robust_list = SC_ARG(params, 0);
        return 0;
    }
    if (SC_ARG(params, 1) != sizeof(struct robust_head)) {
        return -EINVAL;
    }
    current_proc->robust_list = SC_ARG(params, 0);
    current_proc->robust_len = (uint32_t)SC_ARG(params, 1);
    return 0;
}

/**
 * The head is read from the thread's record before anything is copied out: a
 * copy may sleep to bring the caller's page in, and the thread may end and
 * its record be freed meanwhile.
 */
long sys_get_robust_list(isr_params_t *params)
{
    process_t *t = current_proc;
    uintptr_t head;
    unsigned long len = sizeof(struct robust_head);

    if (!current_proc) {
        return -ESRCH;
    }
    if (SC_ARG(params, 0) != 0) {
        t = proc_named((uint32_t)SC_ARG(params, 0));
        if (t == NULL || t->state == PROC_ZOMBIE || t->state == PROC_UNUSED) {
            return -ESRCH;
        }
        if (t->tg != current_proc->tg && !proc_privileged(current_proc) &&
            ((current_proc->euid != t->ruid && current_proc->euid != t->euid) ||
             proc_privileged(t))) {
            return -EPERM;
        }
    }
    if (sizeof(long) > 4u && in_compat_syscall()) {
        uint32_t w[2];

        w[0] = (uint32_t)t->compat_robust_list;
        w[1] = sizeof(struct robust_head32);
        if (copy_to_user((void __user *)SC_ARG(params, 2), &w[1], sizeof(w[1])) != 0 ||
            copy_to_user((void __user *)SC_ARG(params, 1), &w[0], sizeof(w[0])) != 0) {
            return -EFAULT;
        }
        return 0;
    }
    head = t->robust_list;
    if (copy_to_user((void __user *)SC_ARG(params, 1), &head, sizeof(head)) != 0 ||
        copy_to_user((void __user *)SC_ARG(params, 2), &len, sizeof(len)) != 0) {
        return -EFAULT;
    }
    return 0;
}

long sys_futex(isr_params_t *params)
{
    uintptr_t uaddr = SC_ARG(params, 0);
    int op = (int)SC_ARG(params, 1);
    uint32_t val = (uint32_t)SC_ARG(params, 2);
    const struct kernel_timespec __user *utime =
        (const struct kernel_timespec __user *)SC_ARG(params, 3);
    uint32_t val3 = (uint32_t)SC_ARG(params, 5);
    int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);

    if (!current_proc) {
        return -ESRCH;
    }
    if ((op & FUTEX_CLOCK_REALTIME) && cmd != FUTEX_WAIT_BITSET) {
        return -ENOSYS;
    }
    if ((uaddr & 3u) != 0) {
        return -EINVAL;
    }
    switch (cmd) {
    case FUTEX_WAIT:
        return futex_wait(uaddr, val, utime, FUTEX_MATCH_ANY, 0, op);
    case FUTEX_WAIT_BITSET:
        if (val3 == 0) {
            return -EINVAL;
        }
        return futex_wait(uaddr, val, utime, val3, 1, op);
    case FUTEX_WAKE:
        return futex_wake(uaddr, val, FUTEX_MATCH_ANY, op);
    case FUTEX_WAKE_BITSET:
        if (val3 == 0) {
            return -EINVAL;
        }
        return futex_wake(uaddr, val, val3, op);
    default:
        return -ENOSYS;
    }
}

long sys_futex_waitv(isr_params_t *params)
{
    const struct futex_waitv __user *uw = (const struct futex_waitv __user *)SC_ARG(params, 0);
    uint32_t nr = (uint32_t)SC_ARG(params, 1);
    const struct futex_timespec64 __user *utime =
        (const struct futex_timespec64 __user *)SC_ARG(params, 3);
    int32_t clockid = (int32_t)SC_ARG(params, 4);
    struct futex_waitv w;
    struct futex_timespec64 ts;
    struct futex_sleeper s;
    struct futex_vec *v;
    uint64_t ticks = 0;
    int64_t sec;
    uint32_t nsec, i;
    int rc = 0;

    if (!current_proc) {
        return -ESRCH;
    }
    if ((uint32_t)SC_ARG(params, 2) != 0 || nr == 0 || nr > FUTEX_WAITV_MAX || uw == NULL) {
        return -EINVAL;
    }
    s.timed = 0;
    if (utime) {
        if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC) {
            return -EINVAL;
        }
        if (copy_from_user(&ts, utime, sizeof(ts)) != 0) {
            return -EFAULT;
        }
        futex_ts_norm(&ts, &sec, &nsec);
        ticks = futex_ticks_until(sec, nsec, clockid == CLOCK_REALTIME);
        s.timed = 1;
    }
    s.target = rtc_ticks_now() + ticks + 1u;

    v = (struct futex_vec *)kmalloc(nr * sizeof(*v));
    if (v == NULL) {
        return -ENOMEM;
    }
    for (i = 0; i < nr && rc == 0; i++) {
        if (copy_from_user(&w, &uw[i], sizeof(w)) != 0) {
            rc = -EFAULT;
        } else if ((w.flags & ~(FUTEX2_SIZE_MASK | FUTEX2_PRIVATE)) != 0 || w.reserved != 0 ||
                   (w.flags & FUTEX2_SIZE_MASK) != FUTEX2_SIZE_U32 ||
                   (uint32_t)(w.val >> 32) != 0) {
            rc = -EINVAL;
        } else {
            v[i].addr = (uintptr_t)w.uaddr;
            v[i].val = (uint32_t)w.val;
            v[i].shared = (w.flags & FUTEX2_PRIVATE) == 0;
            v[i].q.bitset = FUTEX_MATCH_ANY;
        }
    }
    for (i = 0; i < nr && rc == 0; i++) {
        if ((v[i].addr & 3u) != 0) {
            rc = -EINVAL;
        } else if (!user_range_ok(v[i].addr, sizeof(uint32_t))) {
            rc = -EFAULT;
        }
    }
    if (rc == 0) {
        rc = futex_sleep_on(v, nr, &s, ticks);
    }
    kfree(v);
    return rc;
}
