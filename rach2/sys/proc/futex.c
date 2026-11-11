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
#include "../lib/spinlock.h"
#include "smp.h"
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
 * the count bounds the walk and not the number of waiters. `_futex_hb`'s
 * initializer has this many entries. */
#define FUTEX_BUCKETS 64

/* What a waiting thread sleeps on: one record however many futexes it
 * waits on, so that a wake of any of them ends the sleep. */
struct futex_sleeper {
    int woken;       /* a wake marked one of its waiters */
    int timed;       /* the wait has a deadline */
    uint64_t target; /* the tick the sleep ends at, with `timed` */
    wait_queue_t wq; /* the sleeper's own queue, which only it sleeps on */
};

struct futex_hb;

/* A waiter filed on one futex, on its thread's kernel stack or in the
 * vector of a futex_waitv call while the thread sleeps. Its links, key and
 * marks change under the lock of the bucket that `hb` names. */
struct futex_q {
    const void *space; /* the address space's page directory, or shared memory */
    uintptr_t uaddr;   /* the address in it, or the frame and offset */
    uint32_t bitset;   /* a wake's bitset must meet it */
    int woken;         /* a wake marked it and took it off its list */
    struct futex_sleeper *s;
    struct futex_q *next;
    /* The bucket whose list holds it, NULL while it is on none. A requeue
     * changes it to the bucket it moves the waiter to, holding both; a
     * wake sets it NULL after its last touch of the waiter and its
     * sleeper, so that the waiting thread, which may then return, reads it
     * without a lock (`futex_unqueue_q`), as Linux's lock_ptr. */
    struct futex_hb *volatile hb;
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

/* A hash bucket, Linux's struct futex_hash_bucket (kernel/futex/futex.h:115):
 * the waiters of the keys that hash to it, the newest first, and the lock
 * over that list and its waiters, taken with interrupts off. A futex word is
 * read or exchanged under its bucket's lock, so a wake that follows a
 * store to the word finds the waiter that compared it. */
struct futex_hb {
    struct spinlock lock;
    struct futex_q *head;
};

/* Every bucket's lock is of one class, keyed by `futex_hb_key`, as Linux's
 * spin_lock_init in futex_init makes them (kernel/futex/core.c:1202); two
 * are taken at once only in address order (`futex_hb_lock2`). */
#ifdef RACH2_LOCKDEP
static struct lockdep_key futex_hb_key;
#define FUTEX_HB_INIT {{0, {"futex_hb", &futex_hb_key, 0, 0}}, NULL}
#else
#define FUTEX_HB_INIT {{0}, NULL}
#endif
#define FUTEX_HB_INIT4  FUTEX_HB_INIT, FUTEX_HB_INIT, FUTEX_HB_INIT, FUTEX_HB_INIT
#define FUTEX_HB_INIT16 FUTEX_HB_INIT4, FUTEX_HB_INIT4, FUTEX_HB_INIT4, FUTEX_HB_INIT4

static struct futex_hb _futex_hb[FUTEX_BUCKETS] = {FUTEX_HB_INIT16, FUTEX_HB_INIT16,
                                                   FUTEX_HB_INIT16, FUTEX_HB_INIT16};

/** The bucket a futex named `space` and `uaddr` is filed in. */
static struct futex_hb *futex_hash(const void *space, uintptr_t uaddr)
{
    return &_futex_hb[(((uintptr_t)space >> 4) ^ (uaddr >> 2) ^ (uaddr >> 12)) % FUTEX_BUCKETS];
}

/** Take `hb`'s lock with interrupts off; the flags `futex_hb_unlock` puts
 * back. */
static uint32_t futex_hb_lock(struct futex_hb *hb)
{
    return spin_lock_irqsave(&hb->lock);
}

/** Give back the lock `futex_hb_lock` took. */
static void futex_hb_unlock(struct futex_hb *hb, uint32_t flags)
{
    spin_unlock_irqrestore(&hb->lock, flags);
}

/**
 * Take the locks of two buckets, the one at the lower address first and the
 * other as the second of its class, or one lock when they are the same
 * bucket, as Linux's double_lock_hb (kernel/futex/futex.h:322), so that two
 * requeues in opposite directions cannot each hold the lock the other waits
 * for. The flags `futex_hb_unlock2` puts back.
 */
static uint32_t futex_hb_lock2(struct futex_hb *a, struct futex_hb *b)
{
    struct futex_hb *first = a < b ? a : b, *second = a < b ? b : a;
    uint32_t flags = futex_hb_lock(first);

    if (first != second) {
        spin_lock_nested(&second->lock, 1);
    }
    return flags;
}

/** Give back the locks `futex_hb_lock2` took of `a` and `b`. */
static void futex_hb_unlock2(struct futex_hb *a, struct futex_hb *b, uint32_t flags)
{
    if (a != b) {
        spin_unlock(&a->lock);
    }
    futex_hb_unlock(b, flags);
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
 * The name of the futex at `uaddr`: its address space and address, or, when
 * `shared` and the word is in a shared mapping, the page's frame and the
 * offset in it, which every process mapping that page names alike. A futex
 * word in memory two processes share, a PTHREAD_PROCESS_SHARED mutex in
 * /dev/shm among them, is named the second way unless the caller passed
 * FUTEX_PRIVATE_FLAG. For `shared` the caller holds the regions' lock to
 * read, under which the region and the frame are read with no sleep. 0, or
 * -EFAULT when the shared mapping's page is not present, which the caller
 * brings in with its locks let go before it names the futex again, as
 * Linux's get_futex_key brings the page in (kernel/futex/core.c:210).
 */
static int futex_key(uintptr_t uaddr, int shared, const void **space, uintptr_t *key)
{
    const vma_t *r;
    uintptr_t phys;

    *space = current_proc->page_dir;
    *key = uaddr;
    if (!shared || current_proc->mm == NULL) {
        return 0;
    }
    r = vm_find(&current_proc->mm->vm, uaddr);
    if (!r || r->type != VM_SHARED) {
        return 0;
    }
    phys = paging_user_phys(current_proc->page_dir, uaddr);
    if (phys == 0) {
        return -EFAULT;
    }
    *space = &futex_shared_space;
    *key = phys;
    return 0;
}

/**
 * Name the futex at `uaddr` as `futex_key` does, for a call that reads no
 * word: the regions' lock is held only while a futex that may be `shared`
 * is named, and a page that is not there is brought in. 0, or -EFAULT for a
 * page that cannot be had.
 */
static int futex_name(uintptr_t uaddr, int shared, const void **space, uintptr_t *key)
{
    struct mm *mm = shared ? current_proc->mm : NULL;
    int rc;

    for (;;) {
        if (mm) {
            mmap_read_lock(mm);
        }
        rc = futex_key(uaddr, shared, space, key);
        if (mm) {
            mmap_unlock(mm);
        }
        if (rc == 0 || !vm_prefault_range(uaddr, sizeof(uint32_t), 0)) {
            return rc;
        }
    }
}

/** The wait's condition: a waiter was woken, or the deadline has come. */
static int futex_sleeper_done(void *arg)
{
    const struct futex_sleeper *s = (const struct futex_sleeper *)arg;

    return s->woken || (s->timed && tick_count() >= s->target);
}

/** File `q` on the list of `hb`, whose lock the caller holds. */
static void futex_queue(struct futex_q *q, struct futex_hb *hb)
{
    q->next = hb->head;
    hb->head = q;
    q->hb = hb;
}

/** Take `q` off the list of its bucket, `q->hb`, whose lock the caller
 * holds; `q->hb` is left for the caller to change. */
static void futex_unlink(struct futex_q *q)
{
    struct futex_q **pp;

    for (pp = &q->hb->head; *pp; pp = &(*pp)->next) {
        if (*pp == q) {
            *pp = q->next;
            return;
        }
    }
}

/**
 * Take `q` off its bucket's list unless a wake has, as Linux's futex_unqueue
 * (kernel/futex/core.c:589): the bucket is read without a lock and read again
 * under it, since a requeue may have moved the waiter meanwhile, and a NULL
 * there means a wake took it off and is done with it. A waiter is taken off
 * so before its wait returns, since it lives in the waiting thread's memory.
 * 1 when this call took it off, 0 when a wake had.
 */
static int futex_unqueue_q(struct futex_q *q)
{
    struct futex_hb *hb;
    uint32_t flags;

    for (;;) {
        hb = q->hb;
        if (hb == NULL) {
            return 0;
        }
        flags = futex_hb_lock(hb);
        if (hb == q->hb) {
            break;
        }
        futex_hb_unlock(hb, flags);
    }
    futex_unlink(q);
    q->hb = NULL;
    futex_hb_unlock(hb, flags);
    return 1;
}

/**
 * Take the first `n` waiters of `v` off their lists, as Linux's
 * futex_unqueue_multiple (kernel/futex/waitwake.c:386): the index of the
 * last one a wake had marked, or -1 when none was.
 */
static int futex_unqueue(struct futex_vec *v, uint32_t n)
{
    uint32_t i;
    int last = -1;

    for (i = 0; i < n; i++) {
        (void)futex_unqueue_q(&v[i].q);
        if (v[i].q.woken) {
            last = (int)i;
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
 * Under the regions' lock held to read, each futex is named, and its word
 * read and its waiter filed under its bucket's lock, one word at a time, as
 * Linux's futex_wait_multiple_setup (kernel/futex/waitwake.c:414): a wake
 * after the comparison finds the waiter. The read does not fault; a word
 * whose page is not there takes every waiter filed so far off, lets the
 * lock go, brings the page in and starts again.
 *
 * `wchan` names the sleep for /proc/PID/wchan, as Linux's wchan names it:
 * `futex_wait_queue` for one futex and `futex_wait_multiple` for
 * futex_waitv, whose static `futex_sleep_multiple` has that one caller.
 *
 * Returns the index of the woken futex, the last of them when a wake marked
 * more than one, also when a word compared afterwards differs; -EAGAIN when
 * a word differs, -ETIMEDOUT at the deadline, -EINTR for a signal, or
 * -EFAULT for a word the process cannot read.
 */
static int futex_sleep_on(struct futex_vec *v, uint32_t n, struct futex_sleeper *s, uint64_t ticks,
                          const char *wchan)
{
    struct mm *mm = current_proc->mm;
    uint32_t i, cur, flags;
    int rc, woken;

    s->woken = 0;
    wait_queue_init(&s->wq);
    for (i = 0; i < n; i++) {
        v[i].q.hb = NULL;
        v[i].q.woken = 0;
    }
    if (mm == NULL) {
        return -EFAULT;
    }
    for (;;) {
        rc = 0;
        mmap_read_lock(mm);
        for (i = 0; i < n && rc == 0; i++) {
            struct futex_q *q = &v[i].q;
            struct futex_hb *hb;

            rc = futex_key(v[i].addr, v[i].shared, &q->space, &q->uaddr);
            if (rc != 0) {
                break;
            }
            q->pi = NULL;
            q->task = current_proc;
            q->requeue = 0;
            q->woken = 0;
            q->s = s;
            hb = futex_hash(q->space, q->uaddr);
            flags = futex_hb_lock(hb);
            rc = get_user_u32_nofault(&cur, v[i].addr);
            if (rc == 0 && cur != v[i].val) {
                rc = -EAGAIN;
            }
            if (rc == 0) {
                futex_queue(q, hb);
            }
            futex_hb_unlock(hb, flags);
            if (rc != 0) {
                break;
            }
        }
        mmap_unlock(mm);
        if (rc == 0) {
            break;
        }
        woken = futex_unqueue(v, i);
        if (woken >= 0) {
            return woken;
        }
        if (rc != -EFAULT || !vm_prefault_range(v[i].addr, sizeof(uint32_t), 0)) {
            return rc;
        }
    }
    if (s->timed && (ticks == 0 || tick_count() >= s->target)) {
        woken = futex_unqueue(v, n);
        return woken >= 0 ? woken : -ETIMEDOUT;
    }

    current_proc->wakeup_tick = s->timed ? (s->target != 0 ? s->target : 1u) : 0;
    rc = wq_wait_event_as(&s->wq, futex_sleeper_done, s, wchan);
    current_proc->wakeup_tick = 0;

    woken = futex_unqueue(v, n);
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
 * Wake the waiter `q`, which the caller has taken off the list of its
 * bucket, whose lock it holds: mark it and its sleeper, wake the sleeper's
 * own queue, so the bucket's other sleepers stay asleep, and then set its
 * bucket NULL. The waiter and its sleeper are in the waiting thread's
 * memory, which it may free once it reads that NULL, so that store comes
 * last, as Linux's smp_store_release of lock_ptr (kernel/futex/waitwake.c:
 * 117-123); Linux wakes the thread later through a reference it took, and
 * this wake is made before the store instead, since the queue it wakes is
 * the waiter's own.
 */
static void futex_wake_one(struct futex_q *q)
{
    q->woken = 1;
    q->s->woken = 1;
    wq_wake_all(&q->s->wq);
    k_barrier();
    q->hb = NULL;
}

/**
 * Wake waiters of the futex `space` and `key`, filed in `hb`, whose lock the
 * caller holds, whose bitset meets `bitset`, as Linux's futex_wake walks its
 * bucket: the first is woken whatever `n` is, and the walk stops once the
 * count reaches `n`. A PI waiter, or one of FUTEX_WAIT_REQUEUE_PI, ends it
 * with -EINVAL, the waiters before it woken. The count, or -EINVAL.
 */
static int futex_wake_key(struct futex_hb *hb, const void *space, uintptr_t key, int n,
                          uint32_t bitset)
{
    struct futex_q **pp = &hb->head;
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
 * futex_wake (kernel/futex/waitwake.c:155): an `n` of 0 or less wakes one,
 * as Linux's FUTEX_WAKE does, but none with `strict`, futex_wake(2)'s
 * FLAGS_STRICT. The number woken; `futex_addr_check`'s errors, or -EINVAL
 * from `futex_wake_key`.
 */
static int futex_wake(uintptr_t uaddr, int n, uint32_t bitset, int flags_op, int strict)
{
    int shared = !(flags_op & FUTEX_PRIVATE_FLAG);
    struct futex_hb *hb;
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
    rc = futex_name(uaddr, shared, &space, &uaddr);
    if (rc != 0) {
        return rc;
    }
    hb = futex_hash(space, uaddr);
    flags = futex_hb_lock(hb);
    rc = futex_wake_key(hb, space, uaddr, n, bitset);
    futex_hb_unlock(hb, flags);
    return rc;
}

/* The bits of futex_requeue's `shared`: its first futex, or its second,
 * may be shared. */
#define FUTEX_SHARED1 1u
#define FUTEX_SHARED2 2u

/* Two futexes a call names, their keys and their buckets. */
struct futex_pair {
    const void *space1, *space2;
    uintptr_t key1, key2;
    struct futex_hb *hb1, *hb2;
};

/**
 * Name the futexes at `addr1` and `addr2` into `f`, as `futex_key` names
 * each, the first as one that may be shared when `shared1` and the second
 * when `shared2`, and find their buckets. 0, or `futex_key`'s -EFAULT.
 */
static int futex_pair_name(struct futex_pair *f, uintptr_t addr1, int shared1, uintptr_t addr2,
                           int shared2)
{
    int rc = futex_key(addr1, shared1, &f->space1, &f->key1);

    if (rc == 0) {
        rc = futex_key(addr2, shared2, &f->space2, &f->key2);
    }
    if (rc == 0) {
        f->hb1 = futex_hash(f->space1, f->key1);
        f->hb2 = futex_hash(f->space2, f->key2);
    }
    return rc;
}

/** Compare the word at `addr` with `val` under its bucket's lock, without a
 * fault: 0 when it holds it, -EAGAIN when it does not, -EFAULT when it is
 * not there now. */
static int futex_cmp_word(uintptr_t addr, uint32_t val)
{
    uint32_t word;
    int rc = get_user_u32_nofault(&word, addr);

    return rc != 0 ? rc : word != val ? -EAGAIN : 0;
}

/**
 * Bring in the words a call needs after a fault under its locks, which it
 * has let go: the one at `addr1` for a read when `need1`, and the one at
 * `addr2` when `need2`, for a write with `write2`. 1 when each is there, 0
 * when one cannot be had, which the call answers with -EFAULT.
 */
static int futex_fault_in(uintptr_t addr1, int need1, uintptr_t addr2, int need2, int write2)
{
    return (!need1 || vm_prefault_range(addr1, sizeof(uint32_t), 0)) &&
           (!need2 || vm_prefault_range(addr2, sizeof(uint32_t), write2));
}

/**
 * Wake up to `nr_wake` waiters of `f`'s first futex and move up to
 * `nr_requeue` more to its second, under both buckets' locks, as Linux's
 * requeue_futex moves one (kernel/futex/requeue.c:76): a moved waiter takes
 * the second key and, when the buckets differ, the second bucket. The
 * waiters woken and moved are counted in `*count`. 0, or -EINVAL for a PI
 * waiter or one of FUTEX_WAIT_REQUEUE_PI, which stops the walk.
 */
static int futex_requeue_walk(const struct futex_pair *f, int nr_wake, int nr_requeue, int *count)
{
    struct futex_q **pp = &f->hb1->head;

    while (*pp && *count - nr_wake < nr_requeue) {
        struct futex_q *q = *pp;

        if (q->space != f->space1 || q->uaddr != f->key1) {
            pp = &q->next;
            continue;
        }
        if (q->pi || q->requeue) {
            return -EINVAL;
        }
        if (++*count <= nr_wake) {
            *pp = q->next;
            futex_wake_one(q);
            continue;
        }
        q->space = f->space2;
        q->uaddr = f->key2;
        if (f->hb1 == f->hb2) {
            pp = &q->next;
        } else {
            *pp = q->next;
            futex_queue(q, f->hb2);
        }
    }
    return 0;
}

/**
 * Wake up to `nr_wake` waiters of the futex at `addr1` and move up to
 * `nr_requeue` more to the futex at `addr2`, as Linux's futex_requeue
 * without PI (kernel/futex/requeue.c:380): each address named as
 * `futex_key` names it, as one that may be shared when `shared` has its
 * FUTEX_SHARED1 or FUTEX_SHARED2 bit, and with `cmp` only while the word at
 * `addr1` holds `*cmp`, which is read under both buckets' locks. The
 * waiters woken and moved; -EINVAL for a negative count, then
 * `futex_addr_check`'s errors for each address, -EFAULT for a word that
 * cannot be read, -EAGAIN when it differs, and -EINVAL for a PI waiter or
 * one of FUTEX_WAIT_REQUEUE_PI, which stops the walk. A moved waiter keeps
 * its sleeper, so a wake of the second futex ends its wait, and its wait
 * takes it off the second futex's list when it ends otherwise. A fault
 * comes before the walk, with no waiter touched, and the call starts again
 * once the words are in.
 */
static int futex_requeue(uintptr_t addr1, uintptr_t addr2, unsigned int shared, int nr_wake,
                         int nr_requeue, const uint32_t *cmp)
{
    int shared1 = (shared & FUTEX_SHARED1) != 0, shared2 = (shared & FUTEX_SHARED2) != 0;
    struct mm *mm = current_proc->mm;
    struct futex_pair f;
    uint32_t flags;
    int rc, count;

    if (nr_wake < 0 || nr_requeue < 0) {
        return -EINVAL;
    }
    rc = futex_addr_check(addr1, shared1);
    if (rc == 0) {
        rc = futex_addr_check(addr2, shared2);
    }
    if (rc != 0) {
        return rc;
    }
    if (!cmp && !shared1 && !shared2) {
        mm = NULL;
    } else if (mm == NULL) {
        return -EFAULT;
    }
    for (;;) {
        count = 0;
        if (mm) {
            mmap_read_lock(mm);
        }
        rc = futex_pair_name(&f, addr1, shared1, addr2, shared2);
        if (rc == 0) {
            flags = futex_hb_lock2(f.hb1, f.hb2);
            rc = cmp ? futex_cmp_word(addr1, *cmp) : 0;
            if (rc == 0) {
                rc = futex_requeue_walk(&f, nr_wake, nr_requeue, &count);
            }
            futex_hb_unlock2(f.hb1, f.hb2, flags);
        }
        if (mm) {
            mmap_unlock(mm);
        }
        if (rc != -EFAULT || count != 0 ||
            !futex_fault_in(addr1, cmp || shared1, addr2, shared2, 0)) {
            break;
        }
    }
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
 * Apply FUTEX_WAKE_OP's encoded `op` to the word at `addr`, under the
 * regions' lock and a bucket's lock, as Linux's futex_atomic_op_inuser
 * (kernel/futex/waitwake.c:203): the operation on its argument, shifted into
 * a bit with FUTEX_OP_OPARG_SHIFT (a shift past 31 taken modulo 32), in one
 * compare-and-exchange that does not fault, and then the comparison of the
 * old value. 1 or 0, the comparison; -ENOSYS for an unknown operation,
 * which leaves the word, or comparison, which does not; -EFAULT, with the
 * word as it was, for a page that is not there or not writable now, which
 * the caller brings in with its locks let go.
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
        if (get_user_u32_nofault(&old, addr) != 0) {
            return -EFAULT;
        }
        value = kind == FUTEX_OP_SET    ? (uint32_t)oparg
                : kind == FUTEX_OP_ADD  ? old + (uint32_t)oparg
                : kind == FUTEX_OP_OR   ? old | (uint32_t)oparg
                : kind == FUTEX_OP_ANDN ? old & ~(uint32_t)oparg
                                        : old ^ (uint32_t)oparg;
        if (cmpxchg_user_u32_nofault(addr, old, value, &seen) != 0) {
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
 * FUTEX_WAKE_OP, as Linux's futex_wake_op (kernel/futex/waitwake.c:253):
 * under both buckets' locks, change the word at `addr2` by `op`
 * (`futex_op`), wake up to `nr_wake` waiters of `addr1`, and, when the
 * comparison of the second word's old value holds, up to `nr_wake2` of
 * `addr2`, each walk waking one at least. A second word that is not there
 * is brought in with the locks let go, and the call starts again. The
 * waiters woken; `futex_addr_check`'s errors for either address, -EFAULT for
 * a second word that cannot be written, `futex_op`'s -ENOSYS, or -EINVAL for
 * a PI waiter.
 */
static int futex_wake_op(uintptr_t addr1, uintptr_t addr2, int nr_wake, int nr_wake2, uint32_t op,
                         int shared)
{
    struct mm *mm = current_proc->mm;
    struct futex_pair f;
    uint32_t flags;
    int rc, cond, more;

    rc = futex_addr_check(addr1, shared);
    if (rc == 0) {
        rc = futex_addr_check(addr2, shared);
    }
    if (rc != 0) {
        return rc;
    }
    if (mm == NULL) {
        return -EFAULT;
    }
    do {
        mmap_read_lock(mm);
        rc = futex_pair_name(&f, addr1, shared, addr2, shared);
        if (rc == 0) {
            flags = futex_hb_lock2(f.hb1, f.hb2);
            rc = cond = futex_op(addr2, op);
            if (cond >= 0) {
                rc = futex_wake_key(f.hb1, f.space1, f.key1, nr_wake, FUTEX_MATCH_ANY);
            }
            if (cond > 0 && rc >= 0) {
                more = futex_wake_key(f.hb2, f.space2, f.key2, nr_wake2, FUTEX_MATCH_ANY);
                rc = more < 0 ? more : rc + more;
            }
            futex_hb_unlock2(f.hb1, f.hb2, flags);
        }
        mmap_unlock(mm);
    } while (rc == -EFAULT && futex_fault_in(addr1, shared, addr2, 1, 1));
    return rc;
}

/* PI state exists only while a waiter holds a reference to it. Every PI
 * operation holds the big lock (`futex_pi_call`), which serializes the
 * states, their owners and the PI waiters' coming and going; the bucket
 * locks cover the lists the PI waiters are on beside the others. */
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

/** Find the PI state of a futex key, under the big lock. */
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

/** File a PI or requeue waiter on `hb`, whose lock the caller holds, by
 * priority, FIFO among equal priorities. */
static void futex_enqueue(struct futex_q *q, struct futex_hb *hb)
{
    struct futex_q **pp = &hb->head;

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
    q->hb = hb;
}

/** Find the oldest highest-priority waiter of pi, under its bucket's lock. */
static struct futex_q *pi_top(struct futex_pi *pi)
{
    struct futex_q *q, *best = NULL;

    for (q = futex_hash(pi->space, pi->key)->head; q; q = q->next) {
        if (q->pi == pi && (!best || sched_priority(q->task) > sched_priority(best->task))) {
            best = q;
        }
    }
    return best;
}

/** The priority of pi's top waiter, read under its bucket's lock, or -1
 * when it has none. */
static int pi_top_priority(struct futex_pi *pi)
{
    struct futex_hb *hb = futex_hash(pi->space, pi->key);
    uint32_t flags = futex_hb_lock(hb);
    struct futex_q *q = pi_top(pi);
    int prio = q ? (int)sched_priority(q->task) : -1;

    futex_hb_unlock(hb, flags);
    return prio;
}

/** Rebuild donations along every blocked-owner chain, without recursion,
 * under the big lock and no bucket's lock. Removing all donations first also
 * removes a timed-out waiter's boost. */
void futex_pi_recalculate(void)
{
    struct futex_pi *pi;
    uint32_t flags = kirq_save();
    int changed, prio;

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
            prio = pi_top_priority(pi);
            if (prio > (int)pi->owner->pi_priority) {
                pi->owner->pi_priority = (uint8_t)prio;
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

/** Reject an ordinary wait mixed with PI ownership at the same key, under
 * the key's bucket's lock. */
static int pi_mixed(const void *space, uintptr_t key)
{
    struct futex_q *q;

    for (q = futex_hash(space, key)->head; q; q = q->next) {
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
 * The caller holds the big lock, the regions' lock to read and the target
 * key's bucket's lock, and spare is preallocated; the word is read and
 * exchanged without a fault. Returns 1 for acquisition, 0 for an attached
 * waiter, or a negative Linux error, -EFAULT for a word not there now,
 * which the caller brings in with its locks let go. A failed atomic
 * exchange asks the caller to retry. */
static int pi_attach(uintptr_t addr, struct futex_q *q, struct futex_pi **spare, int trylock,
                     int force)
{
    const void *space = q->target_space;
    uintptr_t key = q->target_key;
    struct futex_pi *pi = pi_find(space, key);
    process_t *owner;
    uint32_t word, seen, tid = pid_seen_by(current_proc, q->task->pid);
    int rc;

    if (get_user_u32_nofault(&word, addr) != 0) {
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
        rc = cmpxchg_user_u32_nofault(
            addr, word, tid | (word & FUTEX_OWNER_DIED) | (force ? FUTEX_WAITERS : 0), &seen);
        if (rc != 0 || seen != word) {
            return rc != 0 ? -EFAULT : -EAGAIN;
        }
        return 1;
    } else {
        rc = cmpxchg_user_u32_nofault(addr, word, word | FUTEX_WAITERS, &seen);
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

/** Transfer kernel ownership and wake the top waiter, under the big lock and
 * no bucket's lock; its reference keeps the state alive until it has fixed
 * the user word in its own mapping. */
static void pi_handoff(struct futex_pi *pi)
{
    struct futex_hb *hb = futex_hash(pi->space, pi->key);
    process_t *old = pi->owner;
    struct futex_q *q;
    uint32_t flags;

    flags = futex_hb_lock(hb);
    q = pi_top(pi);
    pi->owner = q ? q->task : NULL;
    if (q) {
        futex_unlink(q);
        q->task->pi_blocked = NULL;
        futex_wake_one(q);
    }
    futex_hb_unlock(hb, flags);
    if (old) {
        old->pi_priority = 0;
        sched_priority_changed(old);
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
 * dead owner whose address space no longer exists, under the big lock and
 * the regions' lock, the page brought in with that lock let go when it is
 * not there. Returns 0 or -EFAULT. */
static int pi_fixup(uintptr_t addr, struct futex_pi *pi)
{
    struct mm *mm = current_proc->mm;
    uint32_t word, seen = 0, value;
    int rc;

    if (mm == NULL) {
        return -EFAULT;
    }
    for (;;) {
        mmap_read_lock(mm);
        do {
            rc = get_user_u32_nofault(&word, addr);
            if (rc == 0) {
                value = pid_seen_by(current_proc, current_proc->pid) | FUTEX_WAITERS |
                        ((pi->died || (word & FUTEX_OWNER_DIED)) ? FUTEX_OWNER_DIED : 0);
                rc = cmpxchg_user_u32_nofault(addr, word, value, &seen);
            }
        } while (rc == 0 && seen != word);
        mmap_unlock(mm);
        if (rc == 0) {
            pi->died = 0;
            return 0;
        }
        if (!vm_prefault_range(addr, sizeof(word), 1)) {
            return -EFAULT;
        }
    }
}

/** Sleep with a queued PI or requeue waiter and then remove its links,
 * under the big lock, which the sleep lets go and takes back. A handoff
 * wins over a simultaneous timeout or signal. The sleep takes its name from
 * `q->wchan`. */
static int pi_sleep(struct futex_q *q, uintptr_t addr)
{
    struct futex_sleeper *s = q->s;
    struct futex_pi *pi;
    int rc;

    current_proc->wakeup_tick = s->timed ? (s->target ? s->target : 1u) : 0;
    rc = wq_wait_event_as(&s->wq, futex_sleeper_done, s, q->wchan);
    current_proc->wakeup_tick = 0;
    (void)futex_unqueue_q(q);
    pi = q->pi;
    if (!q->woken) {
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
    if (q->woken && pi) {
        rc = pi_fixup(addr, pi);
        if (rc != 0) {
            pi_handoff(pi);
        }
    }
    pi_put(pi);
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
 * the selected clock for LOCK_PI2, under the big lock, as Linux's
 * futex_lock_pi (kernel/futex/pi.c:918): the word is read and exchanged
 * under the regions' lock and its bucket's lock, and brought in with both
 * let go when it is not there. TRYLOCK never sleeps. */
static int futex_lock_pi(uintptr_t addr, const struct kernel_timespec __user *utime, int op,
                         int trylock)
{
    struct mm *mm = current_proc->mm;
    struct futex_q q;
    struct futex_sleeper s;
    struct futex_pi *spare;
    struct futex_hb *hb;
    uint32_t flags;
    int rc;

    rc = pi_deadline(&s, trylock ? NULL : utime,
                     (op & FUTEX_CLOCK_REALTIME) || (op & 127) == FUTEX_LOCK_PI);
    if (rc != 0) {
        return rc;
    }
    if (mm == NULL) {
        return -EFAULT;
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
    for (;;) {
        mmap_read_lock(mm);
        rc = futex_key(addr, !(op & FUTEX_PRIVATE_FLAG), &q.target_space, &q.target_key);
        if (rc == 0) {
            hb = futex_hash(q.target_space, q.target_key);
            flags = futex_hb_lock(hb);
            do {
                rc = pi_attach(addr, &q, &spare, trylock, 0);
            } while (rc == -EAGAIN && !trylock);
            if (rc == 0) {
                futex_enqueue(&q, hb);
            }
            futex_hb_unlock(hb, flags);
        }
        mmap_unlock(mm);
        if (rc == -EBUSY) {
            (void)wq_wait_event_nointr(&pi_exit_queue, pi_owner_ready, &q.owner_tid);
        } else if (rc != -EFAULT || !vm_prefault_range(addr, sizeof(uint32_t), 1)) {
            break;
        }
    }
    kfree(spare);
    if (rc != 0) {
        return rc == 1 ? 0 : rc;
    }
    futex_pi_recalculate();
    return pi_sleep(&q, addr);
}

/**
 * UNLOCK_PI's step under the regions' lock and the bucket's lock of `space`
 * and `key`: check that the word at `addr` names the caller, and write the
 * top waiter's id with FUTEX_WAITERS, or 0 with no waiter, setting
 * `*handoff` to the state to hand over, or NULL. 0; -EPERM for a word of
 * another owner; -EINVAL for ordinary waiters on the key or a state of
 * another owner; -EAGAIN when the word changed, as Linux returns it; or
 * -EFAULT for a word not there or not writable now.
 */
static int pi_unlock_word(uintptr_t addr, const void *space, uintptr_t key,
                          struct futex_pi **handoff)
{
    struct futex_pi *pi;
    struct futex_q *q;
    uint32_t word, seen, value;
    int rc = get_user_u32_nofault(&word, addr);

    if (rc != 0) {
        return rc;
    }
    if ((word & FUTEX_TID_MASK) != pid_seen_by(current_proc, current_proc->pid)) {
        return -EPERM;
    }
    if (pi_mixed(space, key)) {
        return -EINVAL;
    }
    pi = pi_find(space, key);
    if (pi && pi->owner != current_proc) {
        return -EINVAL;
    }
    q = pi ? pi_top(pi) : NULL;
    value = q ? FUTEX_WAITERS | pid_seen_by(current_proc, q->task->pid) : 0;
    rc = cmpxchg_user_u32_nofault(addr, word, value, &seen);
    if (rc != 0) {
        return rc;
    }
    if (seen != word) {
        return -EAGAIN;
    }
    *handoff = pi;
    return 0;
}

/** Unlock a PI word only for its owner, passing it to the top waiter with
 * WAITERS still set, or atomically clearing it when no waiter remains,
 * under the big lock, as Linux's futex_unlock_pi (kernel/futex/pi.c:1107).
 * The top waiter whose id is written stays the top until the handoff, since
 * PI waiters come and go only under the big lock. */
static int futex_unlock_pi(uintptr_t addr, int op)
{
    struct mm *mm = current_proc->mm;
    struct futex_pi *handoff;
    struct futex_hb *hb;
    const void *space;
    uintptr_t key;
    uint32_t flags;
    int rc;

    if (mm == NULL) {
        return -EFAULT;
    }
    do {
        handoff = NULL;
        mmap_read_lock(mm);
        rc = futex_key(addr, !(op & FUTEX_PRIVATE_FLAG), &space, &key);
        if (rc == 0) {
            hb = futex_hash(space, key);
            flags = futex_hb_lock(hb);
            rc = pi_unlock_word(addr, space, key, &handoff);
            futex_hb_unlock(hb, flags);
        }
        mmap_unlock(mm);
    } while (rc == -EFAULT && vm_prefault_range(addr, sizeof(uint32_t), 1));
    if (handoff) {
        handoff->died = 0;
        pi_handoff(handoff);
    }
    return rc;
}

/** Wait on a condition word to be requeued to a distinct PI mutex, under
 * the big lock, as Linux's futex_wait_requeue_pi (kernel/futex/requeue.c:
 * 781): the condition word is compared and the waiter filed under its
 * bucket's lock. */
static int futex_wait_requeue_pi(uintptr_t addr, uint32_t val,
                                 const struct kernel_timespec __user *utime, uintptr_t addr2,
                                 int op)
{
    struct mm *mm = current_proc->mm;
    struct futex_q q;
    struct futex_sleeper s;
    struct futex_hb *hb;
    uint32_t flags, word;
    int rc, shared = !(op & FUTEX_PRIVATE_FLAG);

    if ((addr2 & 3u) || addr == addr2) {
        return -EINVAL;
    }
    rc = pi_deadline(&s, utime, (op & FUTEX_CLOCK_REALTIME) != 0);
    if (rc != 0) {
        return rc;
    }
    if (mm == NULL || !vm_prefault_range(addr2, sizeof(word), 1)) {
        return -EFAULT;
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
        mmap_read_lock(mm);
        rc = futex_key(addr, shared, &q.space, &q.uaddr);
        if (rc == 0) {
            rc = futex_key(addr2, shared, &q.target_space, &q.target_key);
        }
        if (rc == 0 && q.space == q.target_space && q.uaddr == q.target_key) {
            rc = -EINVAL;
        } else if (rc == 0) {
            hb = futex_hash(q.space, q.uaddr);
            flags = futex_hb_lock(hb);
            rc = get_user_u32_nofault(&word, addr);
            if (rc == 0 && word != val) {
                rc = -EAGAIN;
            }
            if (rc == 0) {
                futex_enqueue(&q, hb);
            }
            futex_hb_unlock(hb, flags);
        }
        mmap_unlock(mm);
        if (rc != -EFAULT || !futex_fault_in(addr, 1, addr2, 1, 1)) {
            break;
        }
    }
    return rc == 0 ? pi_sleep(&q, addr2) : rc;
}

/** Move one validated condition waiter from its bucket to `hb2`, whose
 * locks the caller holds both of, or give it the free mutex and wake it.
 * Returns 0 or the PI attachment error. */
static int pi_requeue_one(struct futex_q *q, uintptr_t addr, struct futex_pi **spare,
                          struct futex_hb *hb2)
{
    int rc = pi_attach(addr, q, spare, 0, 1);

    if (rc < 0) {
        return rc;
    }
    futex_unlink(q);
    if (rc == 1) {
        q->pi = *spare;
        *spare = NULL;
        q->pi->space = q->target_space;
        q->pi->key = q->target_key;
        q->pi->owner = q->task;
        q->pi->refs = 1;
        q->pi->died = 0;
        q->pi->next = pi_states;
        pi_states = q->pi;
        futex_wake_one(q);
    } else {
        futex_enqueue(q, hb2);
    }
    return 0;
}

/**
 * Move up to `nr_requeue` + 1 condition waiters of `f`'s first futex to
 * the PI mutex at `addr2`, `f`'s second, under the big lock and both
 * buckets' locks, counting them in `*count`. 0; -EINVAL for a waiter of
 * another kind or one that asked for another mutex; or the attachment's
 * error.
 */
static int pi_requeue_walk(const struct futex_pair *f, uintptr_t addr2, uint32_t nr_requeue,
                           struct futex_pi **spare, int *count)
{
    struct futex_q *q, *next;
    int rc;

    for (q = f->hb1->head; q; q = next) {
        next = q->next;
        if (q->space != f->space1 || q->uaddr != f->key1) {
            continue;
        }
        if (!q->requeue || q->pi || q->target_space != f->space2 || q->target_key != f->key2) {
            return -EINVAL;
        }
        rc = pi_requeue_one(q, addr2, spare, f->hb2);
        if (rc < 0) {
            return rc;
        }
        if ((uint32_t)(++*count - 1) == nr_requeue) {
            break;
        }
    }
    return 0;
}

/** Compare a condition word and move up to nr_requeue + 1 waiters to its
 * PI mutex, under the big lock and both buckets' locks. The first waiter
 * may acquire it immediately; val must be 1. A word that is not there is
 * brought in with the locks let go, which can happen only before the
 * first waiter moves, since its attachment reads the mutex's word and
 * leaves it written or a state that later ones only read. */
static int futex_cmp_requeue_pi(uintptr_t addr, uint32_t val, uint32_t nr_requeue, uintptr_t addr2,
                                uint32_t cmp, int op)
{
    struct mm *mm = current_proc->mm;
    struct futex_pi *spare;
    struct futex_pair f;
    uint32_t flags;
    int rc, count, shared = !(op & FUTEX_PRIVATE_FLAG);

    if (val != 1 || addr == addr2 || (addr2 & 3u) || nr_requeue > 0x7FFFFFFFu) {
        return -EINVAL;
    }
    if (mm == NULL) {
        return -EFAULT;
    }
    spare = (struct futex_pi *)kmalloc(sizeof(*spare));
    if (!spare) {
        return -ENOMEM;
    }
    do {
        count = 0;
        mmap_read_lock(mm);
        rc = futex_pair_name(&f, addr, shared, addr2, shared);
        if (rc == 0 && f.space1 == f.space2 && f.key1 == f.key2) {
            rc = -EINVAL;
        } else if (rc == 0) {
            flags = futex_hb_lock2(f.hb1, f.hb2);
            rc = futex_cmp_word(addr, cmp);
            if (rc == 0) {
                rc = pi_requeue_walk(&f, addr2, nr_requeue, &spare, &count);
            }
            futex_hb_unlock2(f.hb1, f.hb2, flags);
        }
        mmap_unlock(mm);
    } while (rc == -EFAULT && count == 0 && futex_fault_in(addr, 1, addr2, 1, 1));
    futex_pi_recalculate();
    kfree(spare);
    return rc == -EBUSY ? -EAGAIN : (rc < 0 ? rc : count);
}

/** Transfer every contended PI lock of an exiting thread, under the big
 * lock. Robust-list cleanup has already marked accessible words; other
 * mappings are repaired by the new owners before returning from their
 * waits. */
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
 * it, as Linux's `handle_futex_death` does (kernel/futex/core.c:662): the
 * word keeps its waiters bit, gains FUTEX_OWNER_DIED in place of the id,
 * and one waiter is woken, so the next owner learns from EOWNERDEAD that
 * the state the lock guarded may be half made. A word that cannot be read
 * or written is left.
 *
 * With `pending`, the word is that of the ordinary futex the thread was
 * taking or giving back when it ended. When that word has no owner, the
 * thread ended after an unlock stored it and before the unlock's wake, or
 * after a wake and before it took the lock; either way a wake is owed. One
 * waiter is woken, and the word is left as it is, since FUTEX_OWNER_DIED in
 * it would tell the next owner of a death that left the lock's state whole.
 *
 * The word's page is made present and writable first, and the word is then
 * read, compared and written under the regions' lock with no fault between,
 * so no sleep comes between the read and the write. A waiter that set
 * FUTEX_WAITERS during such a sleep would have its bit written over and
 * would not be woken. The word is read in one load and written in one
 * compare-and-exchange, read again when it changed meanwhile, as Linux's
 * does: a thread on another processor may take the lock or set
 * FUTEX_WAITERS between the two, and a write of its bytes one at a time
 * would let it see a word that is neither. A page that went meanwhile is
 * brought in again.
 */
static void robust_release(uintptr_t uaddr, uint32_t tid, int pending, int pi)
{
    struct mm *mm = current_proc->mm;
    uint32_t word, died, seen;
    int rc, wake = 0;

    if ((uaddr & 3u) != 0 || mm == NULL || !vm_prefault_range(uaddr, sizeof(word), 1)) {
        return;
    }
    for (;;) {
        mmap_read_lock(mm);
        while ((rc = get_user_u32_nofault(&word, uaddr)) == 0) {
            if (pending && (word & FUTEX_TID_MASK) == 0) {
                wake = 1;
                break;
            }
            if ((word & FUTEX_TID_MASK) != tid) {
                break;
            }
            died = (word & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
            rc = cmpxchg_user_u32_nofault(uaddr, word, died, &seen);
            if (rc != 0) {
                break;
            }
            if (seen == word) {
                wake = (died & FUTEX_WAITERS) != 0;
                break;
            }
        }
        mmap_unlock(mm);
        if (rc != -EFAULT || !vm_prefault_range(uaddr, sizeof(word), 1)) {
            break;
        }
    }
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

/** The robust lists and the PI states are released under the big lock,
 * which every PI operation holds, before a bucket's lock is taken. */
void proc_thread_gone(process_t *p)
{
    uint32_t zero = 0;

    bkl_enter();
    p->futex_exit_state = 1;
    if (p->robust_list != 0) {
        robust_walk(p, p->robust_list, 0);
    }
    if (p->compat_robust_list != 0) {
        robust_walk(p, p->compat_robust_list, 1);
    }
    pi_exit(p);
    p->futex_exit_state = 2;
    wq_wake_all(&pi_exit_queue);
    p->robust_list = 0;
    p->compat_robust_list = 0;
    if (p->clear_child_tid != 0 && p->mm != NULL && p->mm->users > 1 &&
        copy_to_user((void __user *)p->clear_child_tid, &zero, sizeof(zero)) == 0) {
        (void)futex_wake(p->clear_child_tid, 1, FUTEX_MATCH_ANY, 0, 0);
    }
    p->clear_child_tid = 0;
    bkl_exit();
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

/**
 * Run the PI operation `cmd` of the futex call `params` under the big lock,
 * which every PI operation and a thread's release of its robust list hold
 * before they take a bucket's lock, so that the PI states, their owners and
 * the donations along the blocked-owner chains change one operation at a
 * time. The other operations take the bucket locks alone.
 */
static long futex_pi_call(isr_params_t *params, int cmd)
{
    uintptr_t uaddr = SC_ARG(params, 0);
    int op = (int)SC_ARG(params, 1);
    uint32_t val = (uint32_t)SC_ARG(params, 2);
    const struct kernel_timespec __user *utime =
        (const struct kernel_timespec __user *)SC_ARG(params, 3);
    long rc;

    bkl_enter();
    switch (cmd) {
    case FUTEX_UNLOCK_PI:
        rc = futex_unlock_pi(uaddr, op);
        break;
    case FUTEX_WAIT_REQUEUE_PI:
        rc = futex_wait_requeue_pi(uaddr, val, utime, SC_ARG(params, 4), op);
        break;
    case FUTEX_CMP_REQUEUE_PI:
        rc = futex_cmp_requeue_pi(uaddr, val, (uint32_t)SC_ARG(params, 3), SC_ARG(params, 4),
                                  (uint32_t)SC_ARG(params, 5), op);
        break;
    default:
        rc = futex_lock_pi(uaddr, utime, op, cmd == FUTEX_TRYLOCK_PI);
        break;
    }
    bkl_exit();
    return rc;
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
    case FUTEX_UNLOCK_PI:
    case FUTEX_WAIT_REQUEUE_PI:
    case FUTEX_CMP_REQUEUE_PI:
        return futex_pi_call(params, cmd);
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
