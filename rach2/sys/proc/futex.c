#include "futex.h"

#include "process.h"
#include "wait_queue.h"
#include "../fs/krffs.h"
#include "../fs/vfs.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../mm/paging.h"
#include "../mm/usercopy.h"
#include "../syscall_internal.h"

extern volatile uint32_t kernel_uptime_seconds;
extern volatile uint32_t kernel_second_ticks;

#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_WAIT_BITSET    9
#define FUTEX_WAKE_BITSET    10
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_MATCH_ANY      0xFFFFFFFFu

/* Policy: the hash buckets waiters are filed in. A wake walks one bucket, so
 * the count bounds the walk and not the number of waiters. */
#define FUTEX_BUCKETS 64

/* A sleeping waiter, on its own kernel stack while it sleeps. */
struct futex_q {
    const void *space; /* the address space: the process's page directory */
    uint32_t uaddr;
    uint32_t bitset;
    int woken;
    int timed;
    uint32_t target; /* the tick the sleep ends at, with `timed` */
    struct futex_q *next;
};

static struct futex_q *_waiters[FUTEX_BUCKETS];
static wait_queue_t _queues[FUTEX_BUCKETS];

static uint32_t futex_bucket(const void *space, uint32_t uaddr)
{
    return (((uint32_t)space >> 4) ^ (uaddr >> 2) ^ (uaddr >> 12)) % FUTEX_BUCKETS;
}

/* Whole 10 ms ticks in `sec` seconds and `nsec` nanoseconds, a partial tick
 * counted as one, clamped so a later signed comparison of ticks stays valid. */
static uint32_t futex_ticks(int32_t sec, int32_t nsec)
{
    uint32_t s = (uint32_t)sec;
    uint32_t ticks;

    if (s > 21474836u) {
        s = 21474836u;
    }
    ticks = s * 100u + ((uint32_t)nsec + 9999999u) / 10000000u;
    return ticks > 0x7FFFFFFEu ? 0x7FFFFFFEu : ticks;
}

/* The ticks from now until the absolute time `at` on the real-time clock, or
 * on the monotonic clock without `realtime`; 0 when it has passed. */
static uint32_t futex_ticks_until(const struct kernel_timespec *at, int realtime)
{
    int64_t sec;
    int64_t nsec;
    int64_t dsec;
    int32_t dnsec;
    uint32_t flags;

    if (realtime) {
        get_current_time(&sec, &nsec);
    } else {
        flags = kirq_save();
        sec = kernel_uptime_seconds;
        nsec = (int64_t)kernel_second_ticks * 10000000;
        kirq_restore(flags);
    }
    dsec = (int64_t)at->tv_sec - sec;
    dnsec = at->tv_nsec - (int32_t)nsec;
    if (dnsec < 0) {
        dnsec += 1000000000;
        dsec--;
    }
    if (dsec < 0 || (dsec == 0 && dnsec == 0)) {
        return 0;
    }
    return futex_ticks(dsec > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)dsec, dnsec);
}

static int futex_q_done(void *arg)
{
    const struct futex_q *q = (const struct futex_q *)arg;

    return q->woken || (q->timed && (int32_t)(q->target - kernel_ticks) <= 0);
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
 * Sleep on `uaddr` while it holds `val`.
 *
 * The word is compared and the waiter filed with interrupts off, and the page
 * was made resident before, so the read cannot sleep between the two: a wake
 * after the comparison finds the waiter. `utime` is a relative timeout, or an
 * absolute one with `absolute`, on the monotonic clock or with `realtime` on
 * the real-time clock. Returns 0 when woken, -EAGAIN when the word differs,
 * -ETIMEDOUT at the deadline, -EINTR for a signal and -EFAULT for an address
 * the process cannot read.
 */
static int32_t futex_wait(uint32_t uaddr, uint32_t val, const struct kernel_timespec __user *utime,
                          uint32_t bitset, int absolute, int realtime)
{
    struct futex_q q;
    struct kernel_timespec ts;
    uint32_t bucket, cur, flags, ticks = 0;
    int rc;

    q.timed = 0;
    if (utime) {
        if (copy_from_user(&ts, utime, sizeof(ts)) != 0) {
            return -EFAULT;
        }
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) {
            return -EINVAL;
        }
        ticks = absolute ? futex_ticks_until(&ts, realtime) : futex_ticks(ts.tv_sec, ts.tv_nsec);
        q.timed = 1;
    }
    if (!vm_prefault_range(uaddr, sizeof(uint32_t), 0)) {
        return -EFAULT;
    }

    q.space = current_proc->page_dir;
    q.uaddr = uaddr;
    q.bitset = bitset;
    q.woken = 0;
    q.next = NULL;
    bucket = futex_bucket(q.space, uaddr);

    flags = kirq_save();
    if (copy_from_user(&cur, (const void __user *)uaddr, sizeof(cur)) != 0) {
        kirq_restore(flags);
        return -EFAULT;
    }
    if (cur != val) {
        kirq_restore(flags);
        return -EAGAIN;
    }
    if (q.timed && ticks == 0) {
        kirq_restore(flags);
        return -ETIMEDOUT;
    }
    q.target = kernel_ticks + ticks + 1u;
    q.next = _waiters[bucket];
    _waiters[bucket] = &q;
    kirq_restore(flags);

    current_proc->wakeup_tick = q.timed ? (q.target != 0 ? q.target : 1u) : 0;
    rc = wq_wait_event(&_queues[bucket], futex_q_done, &q);
    current_proc->wakeup_tick = 0;

    flags = kirq_save();
    if (!q.woken) {
        futex_unlink(&q, bucket);
    }
    kirq_restore(flags);
    if (q.woken) {
        return 0;
    }
    if (rc != 0) {
        return rc;
    }
    return -ETIMEDOUT;
}

/* Wake up to `n` waiters of this address space on `uaddr` whose bitset meets
 * `bitset`; the number woken. */
static int futex_wake(uint32_t uaddr, uint32_t n, uint32_t bitset)
{
    const void *space = current_proc->page_dir;
    uint32_t bucket = futex_bucket(space, uaddr);
    struct futex_q **pp;
    uint32_t flags;
    int woken = 0;

    flags = kirq_save();
    pp = &_waiters[bucket];
    while (*pp && (uint32_t)woken < n) {
        struct futex_q *q = *pp;

        if (q->space == space && q->uaddr == uaddr && (q->bitset & bitset) != 0) {
            q->woken = 1;
            *pp = q->next;
            woken++;
        } else {
            pp = &q->next;
        }
    }
    kirq_restore(flags);
    if (woken) {
        wq_wake_all(&_queues[bucket]);
    }
    return woken;
}

int futex_wake_address(uint32_t uaddr, uint32_t n)
{
    return futex_wake(uaddr, n, FUTEX_MATCH_ANY);
}

int32_t sys_futex(isr_params_t *params)
{
    uint32_t uaddr = params->ebx;
    int op = (int)params->ecx;
    uint32_t val = params->edx;
    const struct kernel_timespec __user *utime = (const struct kernel_timespec __user *)params->esi;
    uint32_t val3 = params->ebp;
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
        return futex_wait(uaddr, val, utime, FUTEX_MATCH_ANY, 0, 0);
    case FUTEX_WAIT_BITSET:
        if (val3 == 0) {
            return -EINVAL;
        }
        return futex_wait(uaddr, val, utime, val3, 1, (op & FUTEX_CLOCK_REALTIME) != 0);
    case FUTEX_WAKE:
        return futex_wake(uaddr, val, FUTEX_MATCH_ANY);
    case FUTEX_WAKE_BITSET:
        if (val3 == 0) {
            return -EINVAL;
        }
        return futex_wake(uaddr, val, val3);
    default:
        return -ENOSYS;
    }
}
