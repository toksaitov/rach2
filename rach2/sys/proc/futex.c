/*
 * futex.c: futex(2), futex_waitv(2) and the futex2 calls, the robust lists,
 * and the part of a thread's end that concerns futexes, as futex.h and
 * futex.Readme.md describe them.
 */
#include "futex.h"

#include "ns.h"
#include "process.h"
#include "sched.h"
#include "../lib/kstring.h"
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
#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAKE_OP         5
#define FUTEX_LOCK_PI         6
#define FUTEX_UNLOCK_PI       7
#define FUTEX_TRYLOCK_PI      8
#define FUTEX_WAIT_REQUEUE_PI 11
#define FUTEX_CMP_REQUEUE_PI  12
#define FUTEX_LOCK_PI2        13
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET     10
#define FUTEX_PRIVATE_FLAG    128
#define FUTEX_CLOCK_REALTIME  256
#define FUTEX_MATCH_ANY       0xFFFFFFFFu

/* The futex2 flags, of each futex of futex_waitv and futex_requeue and of
 * futex_wake and futex_wait, Linux's FUTEX2_ values: the word's size, of
 * which only 32 bits is taken, and FUTEX2_PRIVATE, which is
 * FUTEX_PRIVATE_FLAG. Linux 6.12 takes no other, FUTEX2_NUMA (4) among
 * them. FUTEX_WAITV_MAX is the most futexes one call names. */
#define FUTEX2_SIZE_U32  0x02u
#define FUTEX2_SIZE_MASK 0x03u
#define FUTEX2_PRIVATE   0x80u
#define FUTEX2_VALID     (FUTEX2_SIZE_MASK | FUTEX2_PRIVATE)
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
    struct futex_pi *pi; /* reference held until this wait returns */
    process_t *task;
    int requeue; /* WAIT_REQUEUE_PI, including before transfer to the mutex */
    const void *target_space;
    uintptr_t target_key; /* the requeue destination expected by the waiter */
    uint32_t owner_tid;   /* an exiting owner, in the caller's PID namespace */
    const char *wchan;    /* the name of a PI or requeue-PI sleep */
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
 * `wchan` names the sleep for /proc/PID/wchan, as Linux's wchan names it:
 * `futex_wait_queue` for one futex and `futex_wait_multiple` for
 * futex_waitv, whose static `futex_sleep_multiple` has that one caller.
 *
 * Returns the index of the woken futex, the last of them when a wake marked
 * more than one; -EAGAIN when a word differs, -ETIMEDOUT at the deadline,
 * -EINTR for a signal, or -EFAULT for a word the process cannot read.
 */
static int futex_sleep_on(struct futex_vec *v, uint32_t n, struct futex_sleeper *s, uint64_t ticks,
                          const char *wchan)
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
        q->pi = NULL;
        q->task = current_proc;
        q->requeue = 0;
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
    rc = wq_wait_event_as(&s->wq, futex_sleeper_done, s, wchan);
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

static long futex_wait_restart(isr_params_t *params);

/**
 * Sleep on `uaddr` while it holds `val` until the tick `target` of `s`,
 * with `s->timed`, and file a wait a signal cut short for its restart, as
 * Linux's futex_wait does: without a deadline it stands for ERESTARTSYS,
 * which SA_RESTART restarts; with one, for ERESTART_RESTARTBLOCK, whose
 * restart (`futex_wait_restart`) sleeps on to the same tick and which a
 * handler ends with EINTR. `ticks` is 0 for a deadline that had passed when
 * the call began. Returns as `futex_wait` does.
 */
static int32_t futex_wait_until(uintptr_t uaddr, uint32_t val, uint32_t bitset, int flags_op,
                                struct futex_sleeper *s, uint64_t ticks)
{
    struct futex_vec v;
    int rc;

    v.addr = uaddr;
    v.val = val;
    v.shared = !(flags_op & FUTEX_PRIVATE_FLAG);
    v.q.bitset = bitset;
    rc = futex_sleep_on(&v, 1, s, ticks, "futex_wait_queue");
    if (rc == -EINTR && s->timed) {
        struct restart_block *b = &current_proc->restart;

        b->fn = futex_wait_restart;
        b->uaddr = uaddr;
        b->val = val;
        b->bitset = bitset;
        b->flags = flags_op;
        b->tick = s->target;
        current_proc->restart_kind = RESTART_BLOCK;
    }
    return rc > 0 ? 0 : rc;
}

/**
 * The resumption of a timed futex wait a signal that ran no handler cut
 * short (restart_syscall): the same word, value and bitset, to the same
 * deadline tick, with the block cleared first, as Linux's
 * futex_wait_restart clears it.
 */
static long futex_wait_restart(isr_params_t *params)
{
    struct restart_block *b = &current_proc->restart;
    struct futex_sleeper s;
    uint64_t now = rtc_ticks_now();

    (void)params;
    b->fn = NULL;
    s.timed = 1;
    s.target = b->tick;
    return futex_wait_until(b->uaddr, b->val, b->bitset, (int)b->flags, &s,
                            s.target > now ? s.target - now : 0);
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
    struct futex_sleeper s;
    struct kernel_timespec ts;
    uint64_t ticks = 0;

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
    return futex_wait_until(uaddr, val, bitset, flags_op, &s, ticks);
}

/**
 * Linux's get_futex_key's checks of a futex's address `addr`: aligned to
 * four bytes, -EINVAL; in user space, as access_ok judges it, -EFAULT; and
 * for one that may be `shared`, its page brought in, -EFAULT when it
 * cannot be. 0 when it passes.
 */
static int futex_addr_check(uintptr_t addr, int shared)
{
    if ((addr & 3u) != 0) {
        return -EINVAL;
    }
    if (!user_span_ok(addr, sizeof(uint32_t))) {
        return -EFAULT;
    }
    if (shared && !vm_prefault_range(addr, sizeof(uint32_t), 0)) {
        return -EFAULT;
    }
    return 0;
}

/**
 * Wake the waiter `q`, which the caller has taken off its list, with
 * interrupts off: mark it and its sleeper and wake the sleeper's own queue,
 * so the bucket's other sleepers stay asleep.
 *
 * The waiter and its sleeper are in memory the sleeping thread frees once it
 * has left its wait, and the wake comes before interrupts are on again: the
 * thread cannot leave its wait before then, since the big lock keeps it
 * from running on another processor and interrupts off keep the tick from
 * switching to it here.
 */
static void futex_wake_one(struct futex_q *q)
{
    q->woken = 1;
    q->s->woken = 1;
    wq_wake_all(&q->s->wq);
}

/**
 * Wake waiters of the futex `space` and `key`, filed in `bucket`, whose
 * bitset meets `bitset`, as Linux's futex_wake walks its bucket: the first
 * is woken whatever `n` is, and the walk stops once the count reaches `n`.
 * A PI waiter, or one of FUTEX_WAIT_REQUEUE_PI, ends it with -EINVAL, the
 * waiters before it woken. Interrupts are off. The count, or -EINVAL.
 */
static int futex_wake_key(const void *space, uintptr_t key, uint32_t bucket, int n, uint32_t bitset)
{
    struct futex_q **pp = &_waiters[bucket];
    int woken = 0;

    while (*pp) {
        struct futex_q *q = *pp;

        if (q->space != space || q->uaddr != key) {
            pp = &q->next;
            continue;
        }
        if (q->pi || q->requeue) {
            return -EINVAL;
        }
        if ((q->bitset & bitset) == 0) {
            pp = &q->next;
            continue;
        }
        *pp = q->next;
        futex_wake_one(q);
        if (++woken >= n) {
            break;
        }
    }
    return woken;
}

/**
 * Wake up to `n` waiters on the futex at `uaddr` whose bitset meets
 * `bitset`, named as `futex_key` names it with `flags_op`, as Linux's
 * futex_wake: an `n` of 0 or less wakes one, as Linux's FUTEX_WAKE does,
 * but none with `strict`, futex_wake(2)'s FLAGS_STRICT. The number woken;
 * `futex_addr_check`'s errors, or -EINVAL from `futex_wake_key`.
 */
static int futex_wake(uintptr_t uaddr, int n, uint32_t bitset, int flags_op, int strict)
{
    int shared = !(flags_op & FUTEX_PRIVATE_FLAG);
    const void *space;
    uint32_t flags;
    int rc;

    rc = futex_addr_check(uaddr, shared);
    if (rc != 0) {
        return rc;
    }
    if (strict && n == 0) {
        return 0;
    }
    futex_key(uaddr, shared, &space, &uaddr);
    flags = kirq_save();
    rc = futex_wake_key(space, uaddr, futex_bucket(space, uaddr), n, bitset);
    kirq_restore(flags);
    return rc;
}

/**
 * Bring in the pages a futex operation on `addr1` and `addr2` reads with
 * interrupts off, and turn interrupts off once each is still there: the
 * first word's when `need1`, for a read, and the second's when `need2`, for
 * a write with `write2`. A page that left while another was brought in
 * starts the round again. The saved interrupt state in `*flags`; 0, or
 * -EFAULT for a page that cannot be had, with interrupts as they were.
 */
static int futex_pin2(uintptr_t addr1, int need1, uintptr_t addr2, int need2, int write2,
                      uint32_t *flags)
{
    for (;;) {
        if ((need1 && !vm_prefault_range(addr1, sizeof(uint32_t), 0)) ||
            (need2 && !vm_prefault_range(addr2, sizeof(uint32_t), write2))) {
            return -EFAULT;
        }
        *flags = kirq_save();
        if ((!need1 || vm_resident(addr1, sizeof(uint32_t), 0)) &&
            (!need2 || vm_resident(addr2, sizeof(uint32_t), write2))) {
            return 0;
        }
        kirq_restore(*flags);
    }
}

/* The bits of futex_requeue's `shared`: its first futex, or its second,
 * may be shared. */
#define FUTEX_SHARED1 1u
#define FUTEX_SHARED2 2u

/**
 * Wake up to `nr_wake` waiters of the futex at `addr1` and move up to
 * `nr_requeue` more to the futex at `addr2`, as Linux's futex_requeue
 * without PI: each address named as `futex_key` names it, as one that may
 * be shared when `shared` has its FUTEX_SHARED1 or FUTEX_SHARED2 bit, and
 * with `cmp` only while the word at `addr1`
 * holds `*cmp`. The waiters woken and moved; -EINVAL for a negative count,
 * then `futex_addr_check`'s errors for each address, -EFAULT for a word that
 * cannot be read, -EAGAIN when it differs, and -EINVAL for a PI waiter or
 * one of FUTEX_WAIT_REQUEUE_PI, which stops the walk. A moved waiter keeps
 * its sleeper, so a wake of the second futex ends its wait, and its wait
 * takes it off the second futex's list when it ends otherwise.
 */
static int futex_requeue(uintptr_t addr1, uintptr_t addr2, unsigned int shared, int nr_wake,
                         int nr_requeue, const uint32_t *cmp)
{
    int shared1 = (shared & FUTEX_SHARED1) != 0, shared2 = (shared & FUTEX_SHARED2) != 0;
    const void *space1, *space2;
    uintptr_t key1, key2;
    uint32_t b1, b2, flags = 0, word = 0;
    struct futex_q **pp;
    int rc, count = 0;

    if (nr_wake < 0 || nr_requeue < 0) {
        return -EINVAL;
    }
    rc = futex_addr_check(addr1, shared1);
    if (rc == 0) {
        rc = futex_addr_check(addr2, shared2);
    }
    if (rc == 0) {
        rc = futex_pin2(addr1, cmp != NULL || shared1, addr2, shared2, 0, &flags);
    }
    if (rc != 0) {
        return rc;
    }
    futex_key(addr1, shared1, &space1, &key1);
    futex_key(addr2, shared2, &space2, &key2);
    if (cmp && get_user_u32(&word, addr1) != 0) {
        rc = -EFAULT;
    } else if (cmp && word != *cmp) {
        rc = -EAGAIN;
    }
    b1 = futex_bucket(space1, key1);
    b2 = futex_bucket(space2, key2);
    pp = &_waiters[b1];
    while (rc == 0 && *pp && count - nr_wake < nr_requeue) {
        struct futex_q *q = *pp;

        if (q->space != space1 || q->uaddr != key1) {
            pp = &q->next;
            continue;
        }
        if (q->pi || q->requeue) {
            rc = -EINVAL;
            break;
        }
        if (++count <= nr_wake) {
            *pp = q->next;
            futex_wake_one(q);
            continue;
        }
        q->space = space2;
        q->uaddr = key2;
        if (b1 == b2) {
            pp = &q->next;
        } else {
            *pp = q->next;
            q->next = _waiters[b2];
            _waiters[b2] = q;
        }
    }
    kirq_restore(flags);
    return rc != 0 ? rc : count;
}

/* FUTEX_WAKE_OP's operations on the second word and its comparisons, the
 * fields of its encoded argument, Linux's values. */
#define FUTEX_OP_SET         0
#define FUTEX_OP_ADD         1
#define FUTEX_OP_OR          2
#define FUTEX_OP_ANDN        3
#define FUTEX_OP_XOR         4
#define FUTEX_OP_OPARG_SHIFT 8
#define FUTEX_OP_CMP_EQ      0
#define FUTEX_OP_CMP_NE      1
#define FUTEX_OP_CMP_LT      2
#define FUTEX_OP_CMP_LE      3
#define FUTEX_OP_CMP_GT      4
#define FUTEX_OP_CMP_GE      5

/** The 12-bit field of `v` at bit `at`, sign-extended, as Linux's
 * sign_extend32 takes FUTEX_WAKE_OP's argument and comparand. */
static int32_t futex_op_field(uint32_t v, int at)
{
    int32_t f = (int32_t)((v >> at) & 0xFFFu);

    return f >= 0x800 ? f - 0x1000 : f;
}

/**
 * Apply FUTEX_WAKE_OP's encoded `op` to the word at `addr`, resident and
 * writable with interrupts off, as Linux's futex_atomic_op_inuser: the
 * operation on its argument, shifted into a bit with FUTEX_OP_OPARG_SHIFT
 * (a shift past 31 taken modulo 32), in one compare-and-exchange, and then
 * the comparison of the old value. 1 or 0, the comparison; -ENOSYS for an
 * unknown operation, which leaves the word, or comparison, which does not;
 * -EFAULT, which asks the caller to bring the page in again.
 */
static int futex_op(uintptr_t addr, uint32_t op)
{
    uint32_t kind = (op >> 28) & 7u, cmp = (op >> 24) & 15u;
    int32_t oparg = futex_op_field(op, 12), cmparg = futex_op_field(op, 0);
    uint32_t old, value, seen;
    int32_t was;

    if (op & ((uint32_t)FUTEX_OP_OPARG_SHIFT << 28)) {
        oparg = (int32_t)((uint32_t)1 << (oparg & 31));
    }
    if (kind > FUTEX_OP_XOR) {
        return -ENOSYS;
    }
    do {
        if (get_user_u32(&old, addr) != 0) {
            return -EFAULT;
        }
        value = kind == FUTEX_OP_SET    ? (uint32_t)oparg
                : kind == FUTEX_OP_ADD  ? old + (uint32_t)oparg
                : kind == FUTEX_OP_OR   ? old | (uint32_t)oparg
                : kind == FUTEX_OP_ANDN ? old & ~(uint32_t)oparg
                                        : old ^ (uint32_t)oparg;
        if (cmpxchg_user_u32(addr, old, value, &seen) != 0) {
            return -EFAULT;
        }
    } while (seen != old);
    was = (int32_t)old;
    switch (cmp) {
    case FUTEX_OP_CMP_EQ:
        return was == cmparg;
    case FUTEX_OP_CMP_NE:
        return was != cmparg;
    case FUTEX_OP_CMP_LT:
        return was < cmparg;
    case FUTEX_OP_CMP_GE:
        return was >= cmparg;
    case FUTEX_OP_CMP_LE:
        return was <= cmparg;
    case FUTEX_OP_CMP_GT:
        return was > cmparg;
    default:
        return -ENOSYS;
    }
}

/**
 * FUTEX_WAKE_OP, as Linux's futex_wake_op: change the word at `addr2` by
 * `op` (`futex_op`), wake up to `nr_wake` waiters of `addr1`, and, when
 * the comparison of the second word's old value holds, up to `nr_wake2` of
 * `addr2`, each walk waking one at least. The waiters woken;
 * `futex_addr_check`'s errors for either address, -EFAULT for a second
 * word that cannot be written, `futex_op`'s -ENOSYS, or -EINVAL for a PI
 * waiter.
 */
static int futex_wake_op(uintptr_t addr1, uintptr_t addr2, int nr_wake, int nr_wake2, uint32_t op,
                         int shared)
{
    const void *space1, *space2;
    uintptr_t key1, key2;
    uint32_t flags = 0;
    int rc, cond, more;

    rc = futex_addr_check(addr1, shared);
    if (rc == 0) {
        rc = futex_addr_check(addr2, shared);
    }
    for (;;) {
        if (rc == 0) {
            rc = futex_pin2(addr1, shared, addr2, 1, 1, &flags);
        }
        if (rc != 0) {
            return rc;
        }
        cond = futex_op(addr2, op);
        if (cond != -EFAULT) {
            break;
        }
        kirq_restore(flags);
    }
    if (cond < 0) {
        kirq_restore(flags);
        return cond;
    }
    futex_key(addr1, shared, &space1, &key1);
    futex_key(addr2, shared, &space2, &key2);
    rc = futex_wake_key(space1, key1, futex_bucket(space1, key1), nr_wake, FUTEX_MATCH_ANY);
    if (rc >= 0 && cond) {
        more = futex_wake_key(space2, key2, futex_bucket(space2, key2), nr_wake2, FUTEX_MATCH_ANY);
        rc = more < 0 ? more : rc + more;
    }
    kirq_restore(flags);
    return rc;
}

/* PI state exists only while a waiter holds a reference to it. The big
 * lock and masked interrupts serialize its owner and the bucket links. */
struct futex_pi {
    const void *space;
    uintptr_t key;
    process_t *owner;
    unsigned int refs;
    int died; /* the exiting owner could not update a waiter's mapping */
    struct futex_pi *next;
};
static struct futex_pi *pi_states;
static wait_queue_t pi_exit_queue;

/** Find the PI state of a resident futex key, with interrupts masked. */
static struct futex_pi *pi_find(const void *space, uintptr_t key)
{
    struct futex_pi *pi;

    for (pi = pi_states; pi; pi = pi->next) {
        if (pi->space == space && pi->key == key) {
            return pi;
        }
    }
    return NULL;
}

/** Queue PI and requeue waiters by priority, FIFO among equal priorities. */
static void futex_enqueue(struct futex_q *q)
{
    struct futex_q **pp = &_waiters[futex_bucket(q->space, q->uaddr)];

    while (*pp) {
        struct futex_q *at = *pp;

        if (at->space == q->space && at->uaddr == q->uaddr && q->task && at->task &&
            sched_priority(q->task) > sched_priority(at->task)) {
            break;
        }
        pp = &at->next;
    }
    q->next = *pp;
    *pp = q;
}

/** Find the oldest highest-priority waiter of pi, with interrupts masked. */
static struct futex_q *pi_top(struct futex_pi *pi)
{
    struct futex_q *q, *best = NULL;

    for (q = _waiters[futex_bucket(pi->space, pi->key)]; q; q = q->next) {
        if (q->pi == pi && (!best || sched_priority(q->task) > sched_priority(best->task))) {
            best = q;
        }
    }
    return best;
}

/** Rebuild donations along every blocked-owner chain, without recursion.
 * Removing all donations first also removes a timed-out waiter's boost. */
void futex_pi_recalculate(void)
{
    struct futex_pi *pi;
    struct futex_q *q;
    uint32_t flags = kirq_save();
    int changed;

    for (pi = pi_states; pi; pi = pi->next) {
        if (pi->owner) {
            pi->owner->pi_priority = 0;
        }
    }
    do {
        changed = 0;
        for (pi = pi_states; pi; pi = pi->next) {
            if (!pi->owner) {
                continue;
            }
            q = pi_top(pi);
            if (q && sched_priority(q->task) > pi->owner->pi_priority) {
                pi->owner->pi_priority = (uint8_t)sched_priority(q->task);
                changed = 1;
            }
        }
    } while (changed);
    for (pi = pi_states; pi; pi = pi->next) {
        if (pi->owner) {
            sched_priority_changed(pi->owner);
        }
    }
    kirq_restore(flags);
}

/** Drop a waiter's reference; no state or owner pointer survives its last
 * waiter. The owner can then unlock its word entirely in user space. */
static void pi_put(struct futex_pi *pi)
{
    struct futex_pi **pp;
    process_t *owner;

    if (!pi || --pi->refs != 0) {
        return;
    }
    owner = pi->owner;
    for (pp = &pi_states; *pp != pi; pp = &(*pp)->next) {
    }
    *pp = pi->next;
    kfree(pi);
    if (owner) {
        owner->pi_priority = 0;
        sched_priority_changed(owner);
    }
    futex_pi_recalculate();
}

/** Whether making task wait on owner would close a blocked-owner cycle.
 * Existing chains are acyclic, since each new edge passes this check. */
static int pi_deadlock(process_t *task, process_t *owner)
{
    while (owner) {
        if (owner == task) {
            return 1;
        }
        owner = owner->pi_blocked ? owner->pi_blocked->owner : NULL;
    }
    return 0;
}

/** Reject an ordinary wait mixed with PI ownership at the same key. */
static int pi_mixed(const void *space, uintptr_t key)
{
    struct futex_q *q;

    for (q = _waiters[futex_bucket(space, key)]; q; q = q->next) {
        if (q->space == space && q->uaddr == key && !q->pi) {
            return 1;
        }
    }
    return 0;
}

/** Whether the user word agrees with kernel ownership, including the
 * zero-TID OWNER_DIED interval before a new owner repairs the word. */
static int pi_owner_matches(const struct futex_pi *pi, uint32_t word)
{
    return pi->owner && ((word & FUTEX_TID_MASK) == pid_seen_by(current_proc, pi->owner->pid) ||
                         ((word & FUTEX_TID_MASK) == 0 && (word & FUTEX_OWNER_DIED)));
}

/** Attach q to a PI owner, or acquire a free word for q's task.
 * The word is resident and writable, interrupts are masked, and spare is
 * preallocated. Returns 1 for acquisition, 0 for an attached waiter, or a
 * negative Linux error. A failed atomic exchange asks the caller to retry. */
static int pi_attach(uintptr_t addr, struct futex_q *q, struct futex_pi **spare, int trylock,
                     int force)
{
    const void *space = q->target_space;
    uintptr_t key = q->target_key;
    struct futex_pi *pi = pi_find(space, key);
    process_t *owner;
    uint32_t word, seen, tid = pid_seen_by(current_proc, q->task->pid);
    int rc;

    if (get_user_u32(&word, addr) != 0) {
        return -EFAULT;
    }
    if ((word & FUTEX_TID_MASK) == tid) {
        return -EDEADLK;
    }
    if (pi_mixed(space, key)) {
        return -EINVAL;
    }
    if (pi) {
        if (!pi_owner_matches(pi, word)) {
            return -EINVAL;
        }
        owner = pi->owner;
    } else if ((word & FUTEX_TID_MASK) == 0) {
        rc = cmpxchg_user_u32(addr, word,
                              tid | (word & FUTEX_OWNER_DIED) | (force ? FUTEX_WAITERS : 0), &seen);
        if (rc != 0 || seen != word) {
            return rc != 0 ? -EFAULT : -EAGAIN;
        }
        return 1;
    } else {
        rc = cmpxchg_user_u32(addr, word, word | FUTEX_WAITERS, &seen);
        if (rc != 0 || seen != word) {
            return rc != 0 ? -EFAULT : -EAGAIN;
        }
        owner = proc_named(word & FUTEX_TID_MASK);
        if (!owner || owner->state == PROC_ZOMBIE || owner->state == PROC_UNUSED) {
            return -ESRCH;
        }
        if (owner->futex_exit_state) {
            q->owner_tid = word & FUTEX_TID_MASK;
            return owner->futex_exit_state == 1 ? -EBUSY : -ESRCH;
        }
        if (!owner->mm) {
            return -EPERM;
        }
    }
    if (trylock) {
        return -EAGAIN;
    }
    if (pi_deadlock(q->task, owner)) {
        return -EDEADLK;
    }
    if (!pi) {
        pi = *spare;
        *spare = NULL;
        pi->space = space;
        pi->key = key;
        pi->owner = owner;
        pi->refs = 0;
        pi->died = 0;
        pi->next = pi_states;
        pi_states = pi;
    }
    pi->refs++;
    q->pi = pi;
    q->space = space;
    q->uaddr = key;
    q->task->pi_blocked = pi;
    return 0;
}

/** Transfer kernel ownership and wake the top waiter; its reference keeps
 * the state alive until it has fixed the user word in its own mapping. */
static void pi_handoff(struct futex_pi *pi)
{
    struct futex_q *q = pi_top(pi);
    process_t *old = pi->owner;

    pi->owner = q ? q->task : NULL;
    if (old) {
        old->pi_priority = 0;
        sched_priority_changed(old);
    }
    if (q) {
        futex_unlink(q, futex_bucket(q->space, q->uaddr));
        q->woken = q->s->woken = 1;
        q->task->pi_blocked = NULL;
        wq_wake_all(&q->s->wq);
    }
    futex_pi_recalculate();
}

/** Parse a PI wait's absolute deadline before any word fault can sleep. */
static int pi_deadline(struct futex_sleeper *s, const struct kernel_timespec __user *utime,
                       int realtime)
{
    struct kernel_timespec ts;
    uint64_t ticks;

    s->timed = utime != NULL;
    s->woken = 0;
    wait_queue_init(&s->wq);
    if (!utime) {
        s->target = 0;
        return 0;
    }
    if (copy_from_user(&ts, utime, sizeof(ts)) != 0) {
        return -EFAULT;
    }
    if (sizeof(long) > 4u && in_compat_syscall()) {
        ts.tv_nsec = (long)(uint32_t)ts.tv_nsec;
    }
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) {
        return -EINVAL;
    }
    ticks = futex_ticks_until(ts.tv_sec, (uint32_t)ts.tv_nsec, realtime);
    s->target = ticks ? rtc_ticks_now() + ticks + 1u : tick_count();
    return 0;
}

/** Repair a handoff's word through the new owner's mapping, including a
 * dead owner whose address space no longer exists. Returns 0 or -EFAULT. */
static int pi_fixup(uintptr_t addr, struct futex_pi *pi)
{
    uint32_t word, seen, flags, value;

    if (!vm_prefault_range(addr, sizeof(word), 1)) {
        return -EFAULT;
    }
    flags = kirq_save();
    for (;;) {
        if (get_user_u32(&word, addr) != 0) {
            kirq_restore(flags);
            return -EFAULT;
        }
        value = pid_seen_by(current_proc, current_proc->pid) | FUTEX_WAITERS |
                ((pi->died || (word & FUTEX_OWNER_DIED)) ? FUTEX_OWNER_DIED : 0);
        if (cmpxchg_user_u32(addr, word, value, &seen) != 0) {
            kirq_restore(flags);
            return -EFAULT;
        }
        if (seen == word) {
            pi->died = 0;
            kirq_restore(flags);
            return 0;
        }
    }
}

/** Sleep with a queued PI or requeue waiter and then remove its links.
 * A handoff wins over a simultaneous timeout or signal. The sleep takes its
 * name from `q->wchan`. */
static int pi_sleep(struct futex_q *q, uintptr_t addr)
{
    struct futex_sleeper *s = q->s;
    struct futex_pi *pi;
    uint32_t flags;
    int rc;

    current_proc->wakeup_tick = s->timed ? (s->target ? s->target : 1u) : 0;
    rc = wq_wait_event_as(&s->wq, futex_sleeper_done, s, q->wchan);
    current_proc->wakeup_tick = 0;
    flags = kirq_save();
    pi = q->pi;
    if (!q->woken) {
        futex_unlink(q, futex_bucket(q->space, q->uaddr));
        current_proc->pi_blocked = NULL;
        rc = rc != 0 ? rc : -ETIMEDOUT;
        if (q->requeue && pi && rc == -EINTR) {
            rc = -EAGAIN;
        }
    } else if (!pi) {
        rc = -EAGAIN;
    } else {
        rc = 0;
    }
    if (rc == -EINTR) {
        current_proc->restart_kind = RESTART_NOINTR;
    }
    futex_pi_recalculate();
    kirq_restore(flags);
    if (q->woken && pi) {
        rc = pi_fixup(addr, pi);
        if (rc != 0) {
            flags = kirq_save();
            pi_handoff(pi);
            kirq_restore(flags);
        }
    }
    flags = kirq_save();
    pi_put(pi);
    kirq_restore(flags);
    return rc;
}

/** Whether a named owner has finished releasing its futexes. A PID lookup
 * each time avoids retaining a process record across a sleep or reap. */
static int pi_owner_ready(void *arg)
{
    process_t *p = proc_named(*(uint32_t *)arg);

    return !p || p->futex_exit_state != 1;
}

/** Lock a PI futex, with an absolute real-time deadline for LOCK_PI or
 * the selected clock for LOCK_PI2. TRYLOCK never sleeps. */
static int futex_lock_pi(uintptr_t addr, const struct kernel_timespec __user *utime, int op,
                         int trylock)
{
    struct futex_q q;
    struct futex_sleeper s;
    struct futex_pi *spare;
    const void *space;
    uintptr_t key;
    uint32_t flags;
    int rc;

    rc = pi_deadline(&s, trylock ? NULL : utime,
                     (op & FUTEX_CLOCK_REALTIME) || (op & 127) == FUTEX_LOCK_PI);
    if (rc != 0) {
        return rc;
    }
    spare = (struct futex_pi *)kmalloc(sizeof(*spare));
    if (!spare) {
        return -ENOMEM;
    }
    k_memset(&q, 0, sizeof(q));
    q.task = current_proc;
    q.s = &s;
    q.bitset = FUTEX_MATCH_ANY;
    q.wchan = "futex_lock_pi"; /* rt_mutex_slowlock_block is __sched, skipped */
retry_owner:
    if (!vm_prefault_range(addr, sizeof(uint32_t), 1)) {
        kfree(spare);
        return -EFAULT;
    }
    flags = kirq_save();
    futex_key(addr, !(op & FUTEX_PRIVATE_FLAG), &space, &key);
    q.target_space = space;
    q.target_key = key;
    do {
        rc = pi_attach(addr, &q, &spare, trylock, 0);
    } while (rc == -EAGAIN && !trylock);
    if (rc == 0) {
        futex_enqueue(&q);
        futex_pi_recalculate();
    }
    kirq_restore(flags);
    if (rc == -EBUSY) {
        (void)wq_wait_event_nointr(&pi_exit_queue, pi_owner_ready, &q.owner_tid);
        goto retry_owner;
    }
    kfree(spare);
    if (rc != 0) {
        return rc == 1 ? 0 : rc;
    }
    return pi_sleep(&q, addr);
}

/** Unlock a PI word only for its owner, passing it to the top waiter with
 * WAITERS still set, or atomically clearing it when no waiter remains. */
static int futex_unlock_pi(uintptr_t addr, int op)
{
    const void *space;
    uintptr_t key;
    struct futex_pi *pi;
    struct futex_q *q;
    uint32_t word, seen, value, flags;
    int rc = 0;

    if (!vm_prefault_range(addr, sizeof(word), 1)) {
        return -EFAULT;
    }
    flags = kirq_save();
    futex_key(addr, !(op & FUTEX_PRIVATE_FLAG), &space, &key);
    if (get_user_u32(&word, addr) != 0) {
        rc = -EFAULT;
    } else if ((word & FUTEX_TID_MASK) != pid_seen_by(current_proc, current_proc->pid)) {
        rc = -EPERM;
    } else if (pi_mixed(space, key)) {
        rc = -EINVAL;
    } else {
        pi = pi_find(space, key);
        q = pi ? pi_top(pi) : NULL;
        if (pi && pi->owner != current_proc) {
            rc = -EINVAL;
        } else {
            value = q ? FUTEX_WAITERS | pid_seen_by(current_proc, q->task->pid) : 0;
            if (cmpxchg_user_u32(addr, word, value, &seen) != 0) {
                rc = -EFAULT;
            } else if (seen != word) {
                rc = -EAGAIN;
            } else if (pi) {
                pi->died = 0;
                pi_handoff(pi);
            }
        }
    }
    kirq_restore(flags);
    return rc;
}

/** Wait on a condition word to be requeued to a distinct PI mutex.
 * Both pages are faulted before the source comparison and queue insertion. */
static int futex_wait_requeue_pi(uintptr_t addr, uint32_t val,
                                 const struct kernel_timespec __user *utime, uintptr_t addr2,
                                 int op)
{
    struct futex_q q;
    struct futex_sleeper s;
    uint32_t flags, word;
    int rc;

    if ((addr2 & 3u) || addr == addr2) {
        return -EINVAL;
    }
    rc = pi_deadline(&s, utime, (op & FUTEX_CLOCK_REALTIME) != 0);
    if (rc != 0) {
        return rc;
    }
    k_memset(&q, 0, sizeof(q));
    q.task = current_proc;
    q.s = &s;
    q.bitset = FUTEX_MATCH_ANY;
    q.requeue = 1;
    /* Linux's requeue moves the waiter without a wake, so it stays in
     * futex_wait_queue's schedule(); Rach2's unlock hands the mutex over,
     * so the waiter never blocks again in rt_mutex_wait_proxy_lock. */
    q.wchan = "futex_wait_queue";
    for (;;) {
        if (!vm_prefault_range(addr2, sizeof(word), 1) ||
            !vm_prefault_range(addr, sizeof(word), 0)) {
            return -EFAULT;
        }
        flags = kirq_save();
        if (paging_user_phys(current_proc->page_dir, addr2) != 0) {
            break;
        }
        kirq_restore(flags);
    }
    futex_key(addr, !(op & FUTEX_PRIVATE_FLAG), &q.space, &q.uaddr);
    futex_key(addr2, !(op & FUTEX_PRIVATE_FLAG), &q.target_space, &q.target_key);
    if (q.space == q.target_space && q.uaddr == q.target_key) {
        rc = -EINVAL;
    } else if (get_user_u32(&word, addr) != 0) {
        rc = -EFAULT;
    } else if (word != val) {
        rc = -EAGAIN;
    } else {
        futex_enqueue(&q);
        rc = 0;
    }
    kirq_restore(flags);
    return rc == 0 ? pi_sleep(&q, addr2) : rc;
}

/** Move one validated condition waiter, retaining the source bucket before
 * attachment replaces its key. Returns 0 or the PI attachment error. */
static int pi_requeue_one(struct futex_q *q, uintptr_t addr, struct futex_pi **spare)
{
    uint32_t bucket = futex_bucket(q->space, q->uaddr);
    int rc = pi_attach(addr, q, spare, 0, 1);

    if (rc < 0) {
        return rc;
    }
    futex_unlink(q, bucket);
    if (rc == 1) {
        q->woken = q->s->woken = 1;
        q->pi = *spare;
        *spare = NULL;
        q->pi->space = q->target_space;
        q->pi->key = q->target_key;
        q->pi->owner = q->task;
        q->pi->refs = 1;
        q->pi->died = 0;
        q->pi->next = pi_states;
        pi_states = q->pi;
        wq_wake_all(&q->s->wq);
    } else {
        futex_enqueue(q);
    }
    return 0;
}

/** Compare a condition word and move up to nr_requeue + 1 waiters to its
 * PI mutex. The first waiter may acquire it immediately; val must be 1. */
static int futex_cmp_requeue_pi(uintptr_t addr, uint32_t val, uint32_t nr_requeue, uintptr_t addr2,
                                uint32_t cmp, int op)
{
    struct futex_pi *spare;
    struct futex_q *q, *next;
    const void *space, *space2;
    uintptr_t key, key2;
    uint32_t flags, word;
    int rc = 0, count = 0;

    if (val != 1 || addr == addr2 || (addr2 & 3u) || nr_requeue > 0x7FFFFFFFu) {
        return -EINVAL;
    }
    spare = (struct futex_pi *)kmalloc(sizeof(*spare));
    if (!spare) {
        return -ENOMEM;
    }
    for (;;) {
        if (!vm_prefault_range(addr2, sizeof(word), 1) ||
            !vm_prefault_range(addr, sizeof(word), 0)) {
            kfree(spare);
            return -EFAULT;
        }
        flags = kirq_save();
        if (paging_user_phys(current_proc->page_dir, addr2) != 0) {
            break;
        }
        kirq_restore(flags);
    }
    futex_key(addr, !(op & FUTEX_PRIVATE_FLAG), &space, &key);
    futex_key(addr2, !(op & FUTEX_PRIVATE_FLAG), &space2, &key2);
    if (space == space2 && key == key2) {
        rc = -EINVAL;
    } else if (get_user_u32(&word, addr) != 0) {
        rc = -EFAULT;
    } else if (word != cmp) {
        rc = -EAGAIN;
    } else {
        for (q = _waiters[futex_bucket(space, key)]; q; q = next) {
            next = q->next;
            if (q->space != space || q->uaddr != key) {
                continue;
            }
            if (!q->requeue || q->pi || q->target_space != space2 || q->target_key != key2) {
                rc = -EINVAL;
                break;
            }
            rc = pi_requeue_one(q, addr2, &spare);
            if (rc < 0) {
                break;
            }
            count++;
            if ((uint32_t)(count - 1) == nr_requeue) {
                break;
            }
        }
    }
    futex_pi_recalculate();
    kirq_restore(flags);
    kfree(spare);
    return rc == -EBUSY ? -EAGAIN : (rc < 0 ? rc : count);
}

/** Transfer every contended PI lock of an exiting thread. Robust-list
 * cleanup has already marked accessible words; other mappings are repaired
 * by the new owners before returning from their waits. */
static void pi_exit(process_t *p)
{
    struct futex_pi *pi;

    for (pi = pi_states; pi; pi = pi->next) {
        if (pi->owner == p) {
            pi->died = 1;
            pi_handoff(pi);
        }
    }
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
static void robust_release(uintptr_t uaddr, uint32_t tid, int pending, int pi)
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
    if (wake && !pi) {
        (void)futex_wake(uaddr, 1, FUTEX_MATCH_ANY, 0, 0);
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
    int pi_entry;

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
    pi_entry = (int)(head.next & 1u);
    entry = head.next & ~(uintptr_t)1;
    pending = head.op_pending & ~(uintptr_t)1;
    while (entry != list && entry != 0 && left-- > 0) {
        uintptr_t next;

        if (robust_ptr(entry, narrow, &next) != 0) {
            break;
        }
        if (entry != pending) {
            robust_release(robust_uaddr(entry, head.futex_offset, narrow), pid_seen_by(p, p->pid),
                           0, pi_entry);
        }
        pi_entry = (int)(next & 1u);
        entry = next & ~(uintptr_t)1;
    }
    if (pending != 0) {
        robust_release(robust_uaddr(pending, head.futex_offset, narrow), pid_seen_by(p, p->pid),
                       (head.op_pending & 1u) == 0, (head.op_pending & 1u) != 0);
    }
}

void proc_thread_gone(process_t *p)
{
    uint32_t zero = 0;

    p->futex_exit_state = 1;
    if (p->robust_list != 0) {
        robust_walk(p, p->robust_list, 0);
    }
    if (p->compat_robust_list != 0) {
        robust_walk(p, p->compat_robust_list, 1);
    }
    {
        uint32_t flags = kirq_save();

        pi_exit(p);
        p->futex_exit_state = 2;
        if (pi_exit_queue.head) {
            wq_wake_all(&pi_exit_queue);
        }
        kirq_restore(flags);
    }
    p->robust_list = 0;
    p->compat_robust_list = 0;
    if (p->clear_child_tid != 0 && p->mm != NULL && p->mm->users > 1 &&
        copy_to_user((void __user *)p->clear_child_tid, &zero, sizeof(zero)) == 0) {
        (void)futex_wake(p->clear_child_tid, 1, FUTEX_MATCH_ANY, 0, 0);
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
    if ((op & FUTEX_CLOCK_REALTIME) && cmd != FUTEX_WAIT_BITSET && cmd != FUTEX_WAIT_REQUEUE_PI &&
        cmd != FUTEX_LOCK_PI2) {
        return -ENOSYS;
    }
    if ((uaddr & 3u) != 0) {
        return -EINVAL;
    }
    switch (cmd) {
    case FUTEX_LOCK_PI:
    case FUTEX_LOCK_PI2:
    case FUTEX_TRYLOCK_PI:
        return futex_lock_pi(uaddr, utime, op, cmd == FUTEX_TRYLOCK_PI);
    case FUTEX_UNLOCK_PI:
        return futex_unlock_pi(uaddr, op);
    case FUTEX_WAIT_REQUEUE_PI:
        return futex_wait_requeue_pi(uaddr, val, utime, SC_ARG(params, 4), op);
    case FUTEX_CMP_REQUEUE_PI:
        return futex_cmp_requeue_pi(uaddr, val, (uint32_t)SC_ARG(params, 3), SC_ARG(params, 4),
                                    val3, op);
    case FUTEX_WAIT:
        return futex_wait(uaddr, val, utime, FUTEX_MATCH_ANY, 0, op);
    case FUTEX_WAIT_BITSET:
        if (val3 == 0) {
            return -EINVAL;
        }
        return futex_wait(uaddr, val, utime, val3, 1, op);
    case FUTEX_WAKE:
        return futex_wake(uaddr, (int)val, FUTEX_MATCH_ANY, op, 0);
    case FUTEX_WAKE_BITSET:
        if (val3 == 0) {
            return -EINVAL;
        }
        return futex_wake(uaddr, (int)val, val3, op, 0);
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE:
        return futex_requeue(
            uaddr, SC_ARG(params, 4), (op & FUTEX_PRIVATE_FLAG) ? 0 : FUTEX_SHARED1 | FUTEX_SHARED2,
            (int)val, (int)SC_ARG(params, 3), cmd == FUTEX_CMP_REQUEUE ? &val3 : NULL);
    case FUTEX_WAKE_OP:
        return futex_wake_op(uaddr, SC_ARG(params, 4), (int)val, (int)SC_ARG(params, 3), val3,
                             !(op & FUTEX_PRIVATE_FLAG));
    default:
        return -ENOSYS;
    }
}

/**
 * The futex2 flags `flags` of a word, as Linux's futex2_to_flags and
 * futex_flags_valid take them: a flag Linux 6.12 does not know, FUTEX2_NUMA
 * among them, or a size other than 32 bits, -EINVAL. 0 when they pass.
 */
static int futex2_flags_check(uint32_t flags)
{
    if ((flags & ~FUTEX2_VALID) != 0 || (flags & FUTEX2_SIZE_MASK) != FUTEX2_SIZE_U32) {
        return -EINVAL;
    }
    return 0;
}

/** Whether `v`, a value or mask of a 32-bit futex2 word, fits it, as
 * Linux's futex_validate_input requires; an i386 caller's always does. */
static int futex2_fits(uint64_t v)
{
    return (v >> 32) == 0;
}

/**
 * Read a futex2 call's absolute timeout at `utime`, on clock `clockid`,
 * into the deadline of `s`, and its ticks from now into `*ticks`, as
 * Linux's futex2_setup_timeout: none for a NULL `utime`; -EINVAL for a
 * clock other than CLOCK_REALTIME and CLOCK_MONOTONIC; -EFAULT; and
 * -EINVAL for negative seconds or nanoseconds outside 0 to 999999999, the
 * high word of a 32-bit caller's nanoseconds ignored, as get_timespec64
 * ignores it. The deadline's tick is fixed before any page is brought in,
 * so a fault's sleep counts against it.
 */
static int futex2_deadline(const struct futex_timespec64 __user *utime, int32_t clockid,
                           struct futex_sleeper *s, uint64_t *ticks)
{
    struct futex_timespec64 ts;

    s->timed = 0;
    *ticks = 0;
    if (utime) {
        if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC) {
            return -EINVAL;
        }
        if (copy_from_user(&ts, utime, sizeof(ts)) != 0) {
            return -EFAULT;
        }
        if (sizeof(long) == 4u || in_compat_syscall()) {
            ts.tv_nsec = (int64_t)(uint32_t)ts.tv_nsec;
        }
        if (ts.tv_sec < 0 || (uint64_t)ts.tv_nsec >= 1000000000u) {
            return -EINVAL;
        }
        *ticks = futex_ticks_until(ts.tv_sec, (uint32_t)ts.tv_nsec, clockid == CLOCK_REALTIME);
        s->timed = 1;
    }
    s->target = rtc_ticks_now() + *ticks + 1u;
    return 0;
}

long sys_futex_wake(isr_params_t *params)
{
    uintptr_t uaddr = SC_ARG(params, 0);
    unsigned long mask = SC_ARG(params, 1);
    uint32_t flags = (uint32_t)SC_ARG(params, 3);
    int rc;

    if (!current_proc) {
        return -ESRCH;
    }
    rc = futex2_flags_check(flags);
    if (rc != 0) {
        return rc;
    }
    if (!futex2_fits(mask) || (uint32_t)mask == 0) {
        return -EINVAL;
    }
    return futex_wake(uaddr, (int)SC_ARG(params, 2), (uint32_t)mask,
                      (flags & FUTEX2_PRIVATE) ? FUTEX_PRIVATE_FLAG : 0, 1);
}

long sys_futex_wait(isr_params_t *params)
{
    uintptr_t uaddr = SC_ARG(params, 0);
    unsigned long val = SC_ARG(params, 1);
    unsigned long mask = SC_ARG(params, 2);
    uint32_t flags = (uint32_t)SC_ARG(params, 3);
    const struct futex_timespec64 __user *utime =
        (const struct futex_timespec64 __user *)SC_ARG(params, 4);
    struct futex_sleeper s;
    struct futex_vec v;
    uint64_t ticks;
    int rc;

    if (!current_proc) {
        return -ESRCH;
    }
    rc = futex2_flags_check(flags);
    if (rc != 0) {
        return rc;
    }
    if (!futex2_fits(val) || !futex2_fits(mask)) {
        return -EINVAL;
    }
    rc = futex2_deadline(utime, (int32_t)SC_ARG(params, 5), &s, &ticks);
    if (rc != 0) {
        return rc;
    }
    if ((uint32_t)mask == 0 || (uaddr & 3u) != 0) {
        return -EINVAL;
    }
    if (!user_span_ok(uaddr, sizeof(uint32_t))) {
        return -EFAULT;
    }
    v.addr = uaddr;
    v.val = (uint32_t)val;
    v.shared = (flags & FUTEX2_PRIVATE) == 0;
    v.q.bitset = (uint32_t)mask;
    rc = futex_sleep_on(&v, 1, &s, ticks, "futex_wait_queue");
    return rc > 0 ? 0 : rc;
}

/**
 * Read the `nr` struct futex_waitv at `uw` into `v`, as Linux's
 * futex_parse_waitv: -EFAULT for one that cannot be read; -EINVAL for one
 * with flags Linux does not know or a size other than 32 bits
 * (`futex2_flags_check`), a reserved word not 0, or a value that does not
 * fit 32 bits.
 */
static int futex2_parse(const struct futex_waitv __user *uw, uint32_t nr, struct futex_vec *v)
{
    struct futex_waitv w;
    uint32_t i;

    for (i = 0; i < nr; i++) {
        if (copy_from_user(&w, &uw[i], sizeof(w)) != 0) {
            return -EFAULT;
        }
        if ((w.flags & ~FUTEX2_VALID) != 0 || w.reserved != 0 || futex2_flags_check(w.flags) != 0 ||
            !futex2_fits(w.val)) {
            return -EINVAL;
        }
        v[i].addr = (uintptr_t)w.uaddr;
        v[i].val = (uint32_t)w.val;
        v[i].shared = (w.flags & FUTEX2_PRIVATE) == 0;
        v[i].q.bitset = FUTEX_MATCH_ANY;
    }
    return 0;
}

long sys_futex_requeue(isr_params_t *params)
{
    const struct futex_waitv __user *uw = (const struct futex_waitv __user *)SC_ARG(params, 0);
    struct futex_vec v[2];
    int rc;

    if (!current_proc) {
        return -ESRCH;
    }
    if ((uint32_t)SC_ARG(params, 1) != 0 || uw == NULL) {
        return -EINVAL;
    }
    rc = futex2_parse(uw, 2, v);
    if (rc != 0) {
        return rc;
    }
    if (v[0].shared != v[1].shared) {
        return -EINVAL;
    }
    return futex_requeue(v[0].addr, v[1].addr,
                         (v[0].shared ? FUTEX_SHARED1 : 0) | (v[1].shared ? FUTEX_SHARED2 : 0),
                         (int)SC_ARG(params, 2), (int)SC_ARG(params, 3), &v[0].val);
}

long sys_futex_waitv(isr_params_t *params)
{
    const struct futex_waitv __user *uw = (const struct futex_waitv __user *)SC_ARG(params, 0);
    uint32_t nr = (uint32_t)SC_ARG(params, 1);
    const struct futex_timespec64 __user *utime =
        (const struct futex_timespec64 __user *)SC_ARG(params, 3);
    struct futex_sleeper s;
    struct futex_vec *v;
    uint64_t ticks;
    uint32_t i;
    int rc;

    if (!current_proc) {
        return -ESRCH;
    }
    if ((uint32_t)SC_ARG(params, 2) != 0 || nr == 0 || nr > FUTEX_WAITV_MAX || uw == NULL) {
        return -EINVAL;
    }
    rc = futex2_deadline(utime, (int32_t)SC_ARG(params, 4), &s, &ticks);
    if (rc != 0) {
        return rc;
    }
    v = (struct futex_vec *)kmalloc(nr * sizeof(*v));
    if (v == NULL) {
        return -ENOMEM;
    }
    rc = futex2_parse(uw, nr, v);
    for (i = 0; i < nr && rc == 0; i++) {
        if ((v[i].addr & 3u) != 0) {
            rc = -EINVAL;
        } else if (!user_span_ok(v[i].addr, sizeof(uint32_t))) {
            rc = -EFAULT;
        }
    }
    if (rc == 0) {
        rc = futex_sleep_on(v, nr, &s, ticks, "futex_wait_multiple");
    }
    kfree(v);
    return rc;
}
