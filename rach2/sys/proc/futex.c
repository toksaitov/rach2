#include "futex.h"

#include "process.h"
#include "wait_queue.h"
#include "../mm/mm.h"
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

/* The bits of a robust futex's word beside the owner's thread id. */
#define FUTEX_WAITERS    0x80000000u
#define FUTEX_OWNER_DIED 0x40000000u
#define FUTEX_TID_MASK   0x3FFFFFFFu

/* Policy: the entries a thread's end walks on its robust list, Linux's
 * ROBUST_LIST_LIMIT; a longer list, or a loop in one, is left there. */
#define ROBUST_LIST_LIMIT 2048

/* The head of a robust list, as glibc lays it out: the first entry, the
 * distance from an entry to its futex word, and the entry being taken or
 * released when the thread ended, if any. */
struct robust_head {
    uint32_t next;
    int32_t futex_offset;
    uint32_t op_pending;
};

/* The name futexes in shared memory have instead of an address space. */
static const char futex_shared_space;

/* Policy: the hash buckets waiters are filed in. A wake walks one bucket, so
 * the count bounds the walk and not the number of waiters. */
#define FUTEX_BUCKETS 64

/* A sleeping waiter, on its own kernel stack while it sleeps. */
struct futex_q {
    const void *space; /* the address space's page directory, or shared memory */
    uint32_t uaddr;    /* the address in it, or the frame and offset */
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

/**
 * The name of the futex at `uaddr`, whose page is resident: its address space
 * and address, or, when `shared` and a shared mapping holds the page, the
 * page's frame and the offset in it, which every process mapping that page
 * names alike. A futex word in memory two processes share, a
 * PTHREAD_PROCESS_SHARED mutex in /dev/shm among them, is named the second
 * way unless the caller passed FUTEX_PRIVATE_FLAG.
 */
static void futex_key(uint32_t uaddr, int shared, const void **space, uint32_t *key)
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
        *key = (uint32_t)phys;
    }
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
 * absolute one with `absolute`, on the monotonic clock or, with
 * FUTEX_CLOCK_REALTIME in `flags_op`, on the real-time clock; `flags_op` has
 * FUTEX_PRIVATE_FLAG for a futex no other process maps. Returns 0 when woken,
 * -EAGAIN when the word differs, -ETIMEDOUT at the deadline, -EINTR for a
 * signal and -EFAULT for an address the process cannot read.
 */
static int32_t futex_wait(uint32_t uaddr, uint32_t val, const struct kernel_timespec __user *utime,
                          uint32_t bitset, int absolute, int flags_op)
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
        ticks = absolute ? futex_ticks_until(&ts, (flags_op & FUTEX_CLOCK_REALTIME) != 0)
                         : futex_ticks(ts.tv_sec, ts.tv_nsec);
        q.timed = 1;
    }
    if (!vm_prefault_range(uaddr, sizeof(uint32_t), 0)) {
        return -EFAULT;
    }

    futex_key(uaddr, !(flags_op & FUTEX_PRIVATE_FLAG), &q.space, &q.uaddr);
    q.bitset = bitset;
    q.woken = 0;
    q.next = NULL;
    bucket = futex_bucket(q.space, q.uaddr);

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

/* Wake up to `n` waiters on the futex at `uaddr` whose bitset meets
 * `bitset`, named as `futex_key` names it with `flags_op`; the number
 * woken. */
static int futex_wake(uint32_t uaddr, uint32_t n, uint32_t bitset, int flags_op)
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
    return futex_wake(uaddr, n, FUTEX_MATCH_ANY, 0);
}

/**
 * Release the robust futex whose word is at `uaddr` if thread `tid` holds
 * it, as Linux's `handle_futex_death` does: the word keeps its waiters bit,
 * gains FUTEX_OWNER_DIED in place of the id, and one waiter is woken, so the
 * next owner learns from EOWNERDEAD that the state the lock guarded may be
 * half made. A word that cannot be read or written is left.
 *
 * The word's page is made present and writable first, and the word is then
 * read, compared and written with interrupts off, so no fault sleeps between
 * the read and the write. A waiter that set FUTEX_WAITERS during such a
 * sleep would have its bit written over and would not be woken.
 */
static void robust_release(uint32_t uaddr, uint32_t tid)
{
    uint32_t word;
    uint32_t flags;
    int wake = 0;

    if ((uaddr & 3u) != 0 || !vm_prefault_range(uaddr, sizeof(word), 1)) {
        return;
    }
    flags = kirq_save();
    if (copy_from_user(&word, (const void __user *)uaddr, sizeof(word)) == 0 &&
        (word & FUTEX_TID_MASK) == tid) {
        word = (word & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
        wake = copy_to_user((void __user *)uaddr, &word, sizeof(word)) == 0 &&
               (word & FUTEX_WAITERS) != 0;
    }
    kirq_restore(flags);
    if (wake) {
        (void)futex_wake(uaddr, 1, FUTEX_MATCH_ANY, 0);
    }
}

/**
 * Walk the robust list of the current thread `p`, which is ending, and
 * release each futex on it that the thread still holds, the one it was
 * taking or giving back included. The list is circular through its head;
 * the low bit of an entry marks a priority-inheritance futex and is not part
 * of the address.
 */
static void robust_exit(process_t *p)
{
    struct robust_head head;
    uint32_t entry;
    uint32_t pending;
    int left = ROBUST_LIST_LIMIT;

    if (p->robust_list == 0 ||
        copy_from_user(&head, (const void __user *)p->robust_list, sizeof(head)) != 0) {
        return;
    }
    entry = head.next & ~1u;
    pending = head.op_pending & ~1u;
    while (entry != p->robust_list && entry != 0 && left-- > 0) {
        uint32_t next;

        if (copy_from_user(&next, (const void __user *)entry, 4) != 0) {
            break;
        }
        if (entry != pending) {
            robust_release(entry + (uint32_t)head.futex_offset, p->pid);
        }
        entry = next & ~1u;
    }
    if (pending != 0) {
        robust_release(pending + (uint32_t)head.futex_offset, p->pid);
    }
}

void proc_thread_gone(process_t *p)
{
    uint32_t zero = 0;

    robust_exit(p);
    p->robust_list = 0;
    if (p->clear_child_tid != 0 && p->mm != NULL && p->mm->refs > 1 &&
        copy_to_user((void __user *)p->clear_child_tid, &zero, sizeof(zero)) == 0) {
        (void)futex_wake(p->clear_child_tid, 1, FUTEX_MATCH_ANY, 0);
    }
    p->clear_child_tid = 0;
}

int32_t sys_set_robust_list(isr_params_t *params)
{
    if (!current_proc) {
        return -ESRCH;
    }
    if (params->ecx != sizeof(struct robust_head)) {
        return -EINVAL;
    }
    current_proc->robust_list = params->ebx;
    current_proc->robust_len = params->ecx;
    return 0;
}

/**
 * get_robust_list (312): the head and length of the robust list of thread
 * EBX, the caller for 0, into the words at ECX and EDX.
 *
 * The head is read from the thread's record before anything is copied out: a
 * copy may sleep to bring the caller's page in, and the thread may end and
 * its record be freed meanwhile.
 */
int32_t sys_get_robust_list(isr_params_t *params)
{
    process_t *t = current_proc;
    uint32_t head;
    uint32_t len = sizeof(struct robust_head);

    if (!current_proc) {
        return -ESRCH;
    }
    if (params->ebx != 0) {
        t = get_process(params->ebx);
        if (t == NULL || t->state == PROC_ZOMBIE || t->state == PROC_UNUSED) {
            return -ESRCH;
        }
        if (current_proc->euid != 0 && current_proc->euid != t->ruid &&
            current_proc->euid != t->euid) {
            return -EPERM;
        }
    }
    head = t->robust_list;
    if (copy_to_user((void __user *)params->ecx, &head, sizeof(head)) != 0 ||
        copy_to_user((void __user *)params->edx, &len, sizeof(len)) != 0) {
        return -EFAULT;
    }
    return 0;
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
