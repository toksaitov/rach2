/*
 * pipe.c: anonymous pipes and FIFOs, as pipe.h describes them: the
 * operations of a pipe end, pipe2(2), and the open of a FIFO file.
 */

#include "pipe.h"
#include "../irq.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../fs/krffs.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../mm/usercopy.h"
#include "../mm/paging.h"
#include "../mm/mm.h"
#include "../mm/pin.h"
#include "../mm/kheap.h"
#include "../lib/kstring.h"
#include "../lib/kerrno.h"
#include "../lib/ksignal.h"
#include "../lib/spinlock.h"
#include "../proc/smp.h"
#include "../syscall_internal.h"
#include "../timer.h"

/* The longest a reader that finds its pipe empty spins for the next write
 * before it sleeps, in microseconds (`pipe_read_spin`). */
#define PIPE_SPIN_US 4u

/* Linux ioctls a pipe answers. */
#define FIONREAD 0x541B /* bytes available to read */

/* The flags pipe2 takes, Linux's O_CLOEXEC and O_NONBLOCK. */
#define O_CLOEXEC_FLAG  02000000
#define O_NONBLOCK_FLAG 04000

/* The live pipes, each allocated from the heap when made and freed when
 * its last description closes (`pipe_info_put`), and the lock over the
 * list, the next inode number, and each pipe's `files` and `nr_accounted`,
 * as Linux keeps `pipe->files` under the inode's `i_lock` (fs/pipe.c:707-719
 * in 6.12). A pipe's own lock may be held when it is taken; nothing is
 * taken under it. */
static pipe_t *pipes;
static struct spinlock pipes_lock = SPINLOCK_INIT_NAMED("pipes_lock");

/* The kind of every pipe's lock, which gives the lock-order checker one
 * class for them (`sleep_lock_init`), and /proc/fs/lockwait a line for the
 * holds of one that crossed a disk wait, as a read whose page comes in
 * from the disk makes. */
static struct sleep_lock_record pipe_lock_record = SLEEP_LOCK_RECORD("pipe");

uint32_t pipe_max_size = PIPE_MAX_SIZE;
unsigned long pipe_user_pages_soft = PIPE_USER_PAGES_SOFT;
unsigned long pipe_user_pages_hard;

void pipe_init_pool(void)
{
    pipes = NULL;
}

static uint32_t next_pipe_inode = 1;

/**
 * The next inode number to give a pipe: rising, nonzero, and not one a live
 * pipe has. Called under `pipes_lock`.
 *
 * A program reading /proc/PID/fd takes two descriptors that show one number
 * for ends of one pipe, so no two live pipes share a number, and the number
 * rises so that a new pipe does not read as one that closed. The counter is
 * 32 bits and wraps, so the list of live pipes is searched and a number in
 * use is stepped over. Zero is skipped because it is what an unset field
 * reads as.
 */
static uint32_t pipe_next_inode(void)
{
    for (;;) {
        uint32_t candidate = next_pipe_inode++;
        const pipe_t *p;

        if (candidate == 0) {
            continue;
        }
        for (p = pipes; p && p->inode != candidate; p = p->next) {
        }
        if (!p) {
            return candidate;
        }
    }
}

/**
 * The pages the live pipes of the real user `user` hold, Linux's
 * `user->pipe_bufs`: each pipe's ring size in pages, whether or not the ring
 * is allocated yet, as charged (`nr_accounted`). Called under `pipes_lock`.
 */
static uint32_t pipe_user_pages(uint32_t user)
{
    const pipe_t *p;
    uint32_t pages = 0;

    for (p = pipes; p; p = p->next) {
        if (p->user == user) {
            pages += p->nr_accounted;
        }
    }
    return pages;
}

uint32_t pipe_round_size(uint32_t size)
{
    uint32_t want = PAGE_SIZE;

    if (size > 0x80000000u) {
        return 0;
    }
    while (want < size) {
        want <<= 1;
    }
    return want;
}

/** Whether `pages` passes the limit `limit` of a user's pipe pages, 0
 * being none, as Linux's `too_many_pipe_buffers_soft` and `_hard` judge. */
static int pipe_pages_over(unsigned long pages, unsigned long limit)
{
    return limit != 0 && pages > limit;
}

/**
 * A new pipe, not yet on the live list, with one end of each kind counted
 * open, two descriptions (`files`), and no ring yet; NULL when the heap has
 * no room. It is charged to the caller's real user once it is on the list
 * (`pipe_publish`).
 */
static pipe_t *pipe_alloc(void)
{
    pipe_t *p = kzalloc(sizeof(*p));

    if (!p) {
        return NULL;
    }
    p->in_use = 1;
    p->user = current_proc ? current_proc->ruid : 0;
    p->read_open = 1;
    p->write_open = 1;
    p->files = 2;
    wait_queue_init(&p->read_wq);
    wait_queue_init(&p->write_wq);
    wait_queue_init(&p->rdwr_wq);
    wait_queue_init(&p->splice_wq);
    wait_queue_init(&p->open_wq);
    sleep_lock_init(&p->lock, &pipe_lock_record, 0);
    return p;
}

/**
 * Size `p`, a new pipe of `pipe_alloc`, charge it to its user and put it on
 * the live list, with its inode number: 0, or -1 for an unprivileged caller
 * whose user's pipes would pass `pipe_user_pages_hard`, with `p` left off
 * the list for the caller to free. Its size is Linux's `alloc_pipe_info`'s:
 * PIPE_BUF_SIZE, but for an unprivileged caller at most `pipe_max_size`,
 * and PIPE_MIN_PAGES once the user's pipes would pass
 * `pipe_user_pages_soft`. The count and the charge are one hold of
 * `pipes_lock`, as Linux's `account_pipe_buffers` adds and reads the
 * user's count in one atomic step (fs/pipe.c:767-772 and 807 in 6.12). Called
 * under `pipes_lock`.
 */
static int pipe_publish(pipe_t *p)
{
    int privileged = !current_proc || proc_privileged(current_proc);
    uint32_t size = PIPE_BUF_SIZE, held;

    if (!privileged) {
        held = pipe_user_pages(p->user);
        if (size > pipe_max_size) {
            size = pipe_max_size;
        }
        if (pipe_pages_over((unsigned long)held + size / PAGE_SIZE, pipe_user_pages_soft)) {
            size = PIPE_MIN_PAGES * PAGE_SIZE;
        }
        if (pipe_pages_over((unsigned long)held + size / PAGE_SIZE, pipe_user_pages_hard)) {
            return -1;
        }
    }
    p->size = size;
    p->nr_accounted = size / PAGE_SIZE;
    p->inode = pipe_next_inode();
    p->next = pipes;
    pipes = p;
    return 0;
}

/** Free `p` with its ring, once it is off the live list or was never on
 * it. */
static void pipe_free(pipe_t *p)
{
    kfree(p->buf);
    kfree(p->buf_pages);
    kfree(p);
}

/**
 * Let go of one of the descriptions or unfinished FIFO opens that `files`
 * counts, and free `p` when it was the last: the pipe leaves the live list
 * under `pipes_lock`, so no FIFO open finds it after, and is freed outside
 * it, as Linux's `put_pipe_info` (fs/pipe.c:707-719 in 6.12). The caller
 * does not hold the pipe's lock: two closes of its ends on two processors
 * each give the lock back whole before either counts itself out here, so
 * neither touches the pipe after the other frees it.
 */
static void pipe_info_put(pipe_t *p)
{
    uint32_t irq = spin_lock_irqsave(&pipes_lock);
    int last = --p->files == 0;

    if (last) {
        pipe_t **link = &pipes;

        while (*link && *link != p) {
            link = &(*link)->next;
        }
        if (*link) {
            *link = p->next;
        }
    }
    spin_unlock_irqrestore(&pipes_lock, irq);
    if (last) {
        pipe_free(p);
    }
}

/** Whether `p` holds bytes or a buffer, as Linux's `pipe_empty` is false:
 * a write that faults in a page leaves an empty buffer (`pipe_buf_fault`),
 * which poll reports, as Linux 6.12's fs/pipe.c:687-688 does, and a read
 * lets go of (`pipe_buf_skip_empty`). */
static int pipe_holds(const pipe_t *p)
{
    return pipe_available(p) != 0 || p->bufs != 0;
}

/** The wait condition of a read of the pipe `arg`: it holds bytes or a
 * buffer (`pipe_holds`), or no writer is left, when the read returns 0, as
 * Linux's `pipe_readable`. */
static int pipe_readable(void *arg)
{
    pipe_t *p = (pipe_t *)arg;
    return pipe_holds(p) || !p->write_open;
}

/** The wait condition of a write to the pipe `arg`: the ring has room, or
 * no reader is left, when the write fails with EPIPE. */
static int pipe_writable(void *arg)
{
    pipe_t *p = (pipe_t *)arg;
    return pipe_free_space(p) != 0 || !p->read_open;
}

/** The slots of Linux's ring for `p` that no buffer holds: its size in pages,
 * Linux's `max_usage`, less its buffers, those of no bytes among them. A
 * write may leave more buffers than that (`pipe_buf_slots`), and then none
 * is free. */
static uint32_t pipe_slots_free(const pipe_t *p)
{
    uint32_t max = p->size / PAGE_SIZE;

    return p->bufs < max ? max - p->bufs : 0;
}

/** Whether `p` has room for a buffer more, as Linux's `pipe_full` judges a
 * splice, a tee, vmsplice and poll: a byte of the ring and a slot free. */
static int pipe_slot_room(const pipe_t *p)
{
    return pipe_free_space(p) != 0 && pipe_slots_free(p) != 0;
}

/** The wait condition of a splice, a tee or a vmsplice into the pipe `arg`:
 * a buffer more fits (`pipe_slot_room`), or no reader is left. */
static int pipe_slot_writable(void *arg)
{
    pipe_t *p = (pipe_t *)arg;
    return pipe_slot_room(p) || !p->read_open;
}

/** The wait condition of a reader of the pipe `arg` that a splice's hold
 * stopped: the hold has ended. */
static int pipe_unspliced(void *arg)
{
    return !((pipe_t *)arg)->spliced;
}

/** Take the lock of `p`, and of `q` too when it is not NULL, the lower
 * address first, as Linux's `pipe_double_lock` orders them, so that two
 * tees between the same pipes never wait on each other; the second is
 * taken at subclass 1 of the pipes' class, the order the lock-order checker
 * is told, as Linux 6.12 tells it by `pipe_lock_cmp_fn` (fs/pipe.c:81-85).
 * May sleep; a signal does not end the wait, as Linux's `mutex_lock` of a
 * pipe. */
static void pipe_lock(pipe_t *p, pipe_t *q)
{
    if (q && (uintptr_t)q < (uintptr_t)p) {
        pipe_t *t = p;

        p = q;
        q = t;
    }
    sleep_lock_take(&p->lock);
    if (q) {
        sleep_lock_subclass(1);
        sleep_lock_take(&q->lock);
    }
}

/** Give back the locks `pipe_lock` took of `p` and `q`. */
static void pipe_unlock(pipe_t *p, pipe_t *q)
{
    sleep_lock_give(&p->lock);
    if (q) {
        sleep_lock_give(&q->lock);
    }
}

/**
 * Whether a signal ends a pipe's wait that its signal-work flag ended: the
 * answer of `signal_pending`, which reads and changes the signal state and
 * so runs under the big lock, taken here when the caller runs without it,
 * as Linux dequeues under `sighand->siglock` (kernel/signal.c:632 in 6.12).
 * Called with no pipe's lock held, since the big lock comes before every
 * pipe's. An ignored signal is discarded and the flag cleared, so the wait
 * goes back to sleep.
 */
static int pipe_signal_pending(void)
{
    int big = !bkl_held();
    int pending;

    if (big) {
        bkl_enter();
    }
    pending = signal_pending();
    if (big) {
        bkl_exit();
    }
    return pending;
}

/**
 * Wait on `wq` until `cond` holds of `arg`, as the wait named `name`, or
 * "wq_wait_event" when that is NULL, with the locks of `p` and `q`
 * (`pipe_lock`) given back for the wait and taken again after it, as
 * Linux's pipe_read and pipe_write drop `pipe->mutex` to wait. Returns 0,
 * or -EINTR when a signal ends the wait; the locks are held again either
 * way, and the caller judges the pipe again, since others used it
 * meanwhile.
 *
 * A pipe's calls run without the big lock, which `signal_pending` needs, so
 * the sleep is ended by the thread's signal-work flag alone
 * (`wq_wait_event_sigflag`), as Linux's wait tests TIF_SIGPENDING
 * (include/linux/sched/signal.h:404 in 6.12), and the flag is judged under
 * the big lock after it (`pipe_signal_pending`).
 */
static int pipe_sleep(pipe_t *p, pipe_t *q, wait_queue_t *wq, wq_cond_fn cond, void *arg,
                      const char *name)
{
    int rc;

    pipe_unlock(p, q);
    do {
        rc = wq_wait_event_sigflag(wq, cond, arg, name ? name : "wq_wait_event");
    } while (rc == -EINTR && !pipe_signal_pending());
    pipe_lock(p, q);
    return rc;
}

/** Record the caller as the last writer of `p`, on the processor it runs
 * on, for a reader's spin (`pipe_read_spin`); the lock of `p` is held. */
static void pipe_note_writer(pipe_t *p)
{
    p->writer = current_proc;
    p->writer_cpu = (int)arch_cpu_id();
}

/**
 * Spin, with the lock of `p` given back, until `p` is readable
 * (`pipe_readable`), for at most PIPE_SPIN_US, while the thread that wrote
 * it last runs on another processor (`sched_spin_on`), as Linux's
 * mutex_optimistic_spin spins while a mutex's owner runs
 * (kernel/locking/mutex.c:350-385). Returns 1 when it spun, with the lock
 * held again, and the caller judges the pipe again; 0 when it did not,
 * since no writer runs elsewhere or the counter's rate is not known.
 *
 * A sleep costs the writer the interprocessor interrupt that wakes the
 * reader, an exit of 2 to 4 us under KVM, and the reader a halt, and a
 * reader that empties a busy pipe mostly finds the next write a microsecond
 * or two later. The bound, about what the wake costs the writer, keeps a
 * reader from burning its processor while a writer runs long between
 * writes, so that a spin that ends in a sleep costs at most twice what the
 * sleep alone would.
 */
static int pipe_read_spin(pipe_t *p)
{
    const process_t *w = p->writer;
    int cpu = p->writer_cpu;
    uint64_t start;

    if (!w || w == current_proc || cpu == (int)arch_cpu_id() || !timer_cycles_agree() ||
        !sched_spin_on(w, cpu)) {
        return 0;
    }
    pipe_unlock(p, NULL);
    start = arch_cycles();
    for (;;) {
        if (pipe_readable(p) || *(volatile uint32_t *)&current_proc->sig_woken ||
            timer_cycles_us(arch_cycles() - start) >= PIPE_SPIN_US || !sched_spin_on(w, cpu)) {
            break;
        }
    }
    pipe_lock(p, NULL);
    return 1;
}

/** Wait until no splice holds the bytes of `p` (`pipe_splice_peek`), with
 * the locks of `p` and `q` held and given back for the wait (`pipe_sleep`):
 * 0, or -EINTR when a signal ends the wait. A reader waits even with
 * O_NONBLOCK, as Linux's waits for the pipe's lock, since the hold ends
 * without any other process acting. */
static int32_t pipe_wait_unspliced(pipe_t *p, pipe_t *q)
{
    while (p->spliced) {
        if (pipe_sleep(p, q, &p->splice_wq, pipe_unspliced, p, NULL) < 0) {
            return -EINTR;
        }
    }
    return 0;
}

/** Whether the description of `e` has O_NONBLOCK. */
static int fd_is_nonblock(fd_entry_t *e)
{
    return e->ofd && (e->ofd->status_flags & OFD_NONBLOCK);
}

/**
 * Wake the waiters on one end's queue `wq` for a change of the kinds
 * `kinds` (`wq_wake`), and the pollers of a FIFO's descriptions open on both
 * ends, whose readiness any change can move. A pipe no poll has asked about
 * (`polled`) has no pollers and no count of wakes that anyone reads, so the
 * change wakes `wq` only with a sleeper on it, as Linux's `pipe_write`
 * wakes the readers only of a pipe that was empty or has been polled, and
 * `pipe_read` the writers only of one that was full (fs/pipe.c in 6.12): a
 * wake of an empty queue costs a pass with interrupts off. The barrier
 * orders the change before the reads of `polled` and the queue, as the
 * unlock of the queue a sleeper files itself on (`wq_wait`) and the barrier
 * after `pipe_poll` sets `polled` order theirs before they read the pipe:
 * either they see the change or this sees them.
 */
static void pipe_wake(pipe_t *p, wait_queue_t *wq, uint32_t kinds)
{
    sched_mb();
    if (!p->polled) {
        if (wq_active(wq)) {
            wq_wake(wq, kinds);
        }
        return;
    }
    wq_wake(wq, kinds);
    if (p->rdwr_open) {
        wq_wake(&p->rdwr_wq, kinds);
    }
}

/*
 * Linux keeps a pipe's bytes in page buffers, one to a slot of its ring, and
 * refuses to shrink a pipe below its buffers, however few bytes they hold.
 * The functions below keep the same buffers over the byte ring, `buf_pages`
 * in pipe.h, so that `pipe_set_size` can apply the rule: a write opens a
 * page when the last buffer cannot take its bytes, and leaves it empty when
 * the page's source faults; vmsplice and splice from a file open one for
 * each page of their source; and a buffer leaves once read whole, or, empty,
 * once a read reaches it.
 */

/** The slots of `buf_pages` for a ring of `size` bytes: twice its pages and
 * two. A write, a tee, a splice and vmsplice each open a buffer only while
 * fewer than the pages hold bytes (`pipe_slots_free`), as Linux's
 * `pipe_full` stops them at `max_usage`, so the slots past the pages are a
 * margin no path reaches; were one reached, `pipe_buf_open` would fold the
 * bytes into the newest buffer. */
static uint32_t pipe_buf_slots(uint32_t size)
{
    return 2 * (size / PAGE_SIZE) + 2;
}

/** The `i`th buffer of `p`, oldest first. */
static struct pipe_page *pipe_buf_page(const pipe_t *p, uint32_t i)
{
    return &p->buf_pages[(p->buf_first + i) % p->buf_cap];
}

/** The counter value at which the `i`th buffer of `p`, oldest first, began. */
static uint32_t pipe_buf_at(const pipe_t *p, uint32_t i)
{
    return pipe_buf_page(p, i)->start;
}

/**
 * Begin a buffer of `p` at the counter value `at`, whose byte lies `offset`
 * bytes into its page and which takes later writes' bytes when `merge`.
 * With every slot in use the bytes join the newest buffer instead, which
 * takes `merge`; the count is then still past the pages, so the shrinks it
 * refuses are Linux's until that buffer alone is left.
 */
static void pipe_buf_open(pipe_t *p, uint32_t at, uint32_t offset, int merge)
{
    struct pipe_page *pg;

    if (!p->buf_pages) {
        return;
    }
    if (p->bufs < p->buf_cap) {
        pg = pipe_buf_page(p, p->bufs);
        pg->start = at;
        pg->offset = (uint16_t)(offset < PAGE_SIZE ? offset : PAGE_SIZE);
        p->bufs++;
    } else {
        pg = pipe_buf_page(p, p->bufs - 1);
    }
    pg->merge = (uint16_t)(merge != 0);
}

/** The counter value at which the `i`th buffer of `p`, oldest first, ends:
 * where the next began, or the tail for the newest. */
static uint32_t pipe_buf_end(const pipe_t *p, uint32_t i)
{
    return i + 1 < p->bufs ? pipe_buf_at(p, i + 1) : p->tail;
}

/** Let go of the oldest buffer of `p`. */
static void pipe_buf_shift(pipe_t *p)
{
    p->buf_first = (p->buf_first + 1) % p->buf_cap;
    if (--p->bufs == 0) {
        p->buf_first = 0;
    }
}

/**
 * Let go of the buffers of `p` that a read has passed; called after `head`
 * moves. A buffer is passed when its end is at or before the head and it
 * began before the head: an empty buffer the head has just reached stays,
 * as Linux's `pipe_read` stops once its count is spent, before the next
 * buffer, and one the head moved past goes, as Linux's lets go of each
 * empty buffer it meets on the way.
 */
static void pipe_buf_drop(pipe_t *p)
{
    uint32_t avail = pipe_available(p);

    while (p->bufs > 0 && pipe_buf_at(p, 0) != p->head && p->tail - pipe_buf_end(p, 0) >= avail) {
        pipe_buf_shift(p);
    }
}

/**
 * Let go of the empty buffers at the front of `p`, as Linux's `pipe_read`
 * lets go of an empty buffer it reaches with room left (fs/pipe.c:364-373
 * in 6.12), and wake the
 * writers, who may wait for a slot, when one went. Called by a read with
 * room left before it reads the next buffer or waits.
 */
static void pipe_buf_skip_empty(pipe_t *p)
{
    uint32_t bufs = p->bufs;

    while (p->bufs > 0 && pipe_buf_end(p, 0) == p->head) {
        pipe_buf_shift(p);
    }
    if (p->bufs != bufs) {
        pipe_wake(p, &p->write_wq, WQ_ROOM);
    }
}

/** Whether the newest buffer of `p` has no byte, or there is none: a
 * splice's next bytes then open a buffer, since the buffer its earlier
 * bytes took is gone or another writer opened one after it. */
static int pipe_buf_tail_empty(const pipe_t *p)
{
    return p->bufs == 0 || pipe_buf_at(p, p->bufs - 1) == p->tail;
}

/**
 * The bytes of a write of `count` that the newest buffer of `p` takes, as
 * Linux's `pipe_write` merges them: the write's first `count` mod 4096
 * bytes, when the pipe has a buffer, an empty one a faulted write left
 * among them, the newest buffer takes merges and they fit in its page
 * after its offset and the bytes written to it.
 */
static uint32_t pipe_buf_merge_room(const pipe_t *p, uint32_t count)
{
    uint32_t chars = count & (PAGE_SIZE - 1);
    const struct pipe_page *pg;

    if (chars == 0 || p->bufs == 0) {
        return 0;
    }
    pg = pipe_buf_page(p, p->bufs - 1);
    if (!pg->merge || p->tail - pg->start > PAGE_SIZE ||
        pg->offset + (p->tail - pg->start) + chars > PAGE_SIZE) {
        return 0;
    }
    return chars;
}

/**
 * Account for `n` bytes of a write about to be written at the tail of `p`:
 * the first `*fill` of them join the newest buffer, and the rest open
 * buffers of a page each, which take later writes' bytes. `*fill` is left
 * with the room of the buffer the bytes ended in, for the same write's next
 * bytes.
 */
static void pipe_buf_add(pipe_t *p, uint32_t n, uint32_t *fill)
{
    uint32_t at = p->tail;

    if (p->bufs == 0) {
        *fill = 0;
    }
    while (n > 0) {
        uint32_t take;

        if (*fill == 0) {
            pipe_buf_open(p, at, 0, 1);
            *fill = PAGE_SIZE;
        }
        take = n < *fill ? n : *fill;
        *fill -= take;
        at += take;
        n -= take;
    }
}

/**
 * Account for `n` bytes about to be written at the tail of `p` from a source
 * whose first byte lies `src` bytes into a page, as Linux's vmsplice and
 * splice from a file link the source's pages: a buffer for each source page
 * the bytes reach, none taking later writes' bytes. `fresh` opens one for
 * the first byte even inside a page, as for a call's first bytes or those
 * after another writer's, and so does a pipe whose newest buffer is empty
 * or gone (`pipe_buf_tail_empty`), since a reader emptied it or a faulted
 * write opened one while the call slept part way through a page.
 */
static void pipe_buf_add_pages(pipe_t *p, uint32_t n, uint32_t src, int fresh)
{
    uint32_t at = p->tail;

    if (pipe_buf_tail_empty(p)) {
        fresh = 1;
    }
    while (n > 0) {
        uint32_t off = src & (PAGE_SIZE - 1), take = PAGE_SIZE - off;

        if (take > n) {
            take = n;
        }
        if (fresh || off == 0) {
            pipe_buf_open(p, at, off, 0);
        }
        fresh = 0;
        at += take;
        src += take;
        n -= take;
    }
}

/**
 * Open in `b` a buffer for each of the first `k` buffers of `a`, which hold
 * the first `n` bytes buffered in `a`, at the same offset in its page, an
 * empty one among them, as Linux's tee and splice between pipes link each
 * buffer, or a part of it, into the other pipe; the last is linked whole
 * when `whole`, and otherwise in part. Called before `b`'s tail moves past
 * the `n` bytes. A buffer that a splice, `move`, carries whole keeps taking
 * later writes' bytes if it did, as Linux's `splice_pipe_to_pipe` moves it
 * with its flags; a part of one, and every buffer tee links, takes none. A
 * pipe `a` that keeps no buffers gives `b` one for each page of the bytes.
 */
static void pipe_buf_link(const pipe_t *a, pipe_t *b, uint32_t k, uint32_t n, int move, int whole)
{
    uint32_t i;

    if (!a->buf_pages) {
        pipe_buf_add_pages(b, n, 0, 1);
        return;
    }
    for (i = 0; i < k; i++) {
        const struct pipe_page *pg = pipe_buf_page(a, i);
        uint32_t read = i == 0 ? a->head - pg->start : 0;
        uint32_t from = i == 0 ? 0 : pg->start - a->head;
        int all = i + 1 < k || whole;

        pipe_buf_open(b, b->tail + from, pg->offset + read, move && all && pg->merge);
    }
}

/**
 * A ring of `size` bytes in `*ring` and the slots for its buffers' bounds in
 * `*pages`: 0, or -ENOMEM with neither allocated.
 */
static int32_t pipe_ring_new(uint32_t size, uint8_t **ring, struct pipe_page **pages)
{
    *ring = kmalloc(size);
    *pages = kmalloc(pipe_buf_slots(size) * sizeof(struct pipe_page));
    if (!*ring || !*pages) {
        kfree(*ring);
        kfree(*pages);
        return -ENOMEM;
    }
    return 0;
}

/** Give `p` its ring, at its first write when nothing has resized it yet:
 * 0, or -ENOMEM. */
static int32_t pipe_ring_ready(pipe_t *p)
{
    if (p->buf) {
        return 0;
    }
    if (pipe_ring_new(p->size, &p->buf, &p->buf_pages) < 0) {
        p->buf = NULL;
        p->buf_pages = NULL;
        return -ENOMEM;
    }
    p->buf_cap = pipe_buf_slots(p->size);
    p->buf_first = 0;
    p->bufs = 0;
    return 0;
}

/**
 * The bytes of `n` from `addr` that lie in `addr`'s own page. A copy to or
 * from user memory goes a page at a time, so a page that faults leaves the
 * bytes of the pages before it moved and counted.
 */
static uint32_t page_piece(uintptr_t addr, uint32_t n)
{
    uint32_t left = PAGE_SIZE - (uint32_t)(addr & (PAGE_SIZE - 1));

    return n < left ? n : left;
}

/* Defined below, beside the vector helpers it shares with the write. */
static int32_t pipe_read_vec(fd_entry_t *e, const struct kernel_iovec *iov, int n);

/** The read of a pipe end: `count` bytes into `buf`, read as a vector of
 * one (`pipe_read_vec`), as Linux reads a pipe for read(2) and readv(2)
 * through the one `pipe_read`. */
static int32_t pipe_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct kernel_iovec one;

    one.iov_base = (__force uintptr_t)buf;
    one.iov_len = count;
    return pipe_read_vec(e, &one, 1);
}

/**
 * Linux's SIGPIPE for a write to a pipe no reader is left on: SI_USER,
 * from the writer, to its thread. The signal state is the big lock's, so
 * it is posted under the big lock, taken here when the caller runs without
 * it, and after the pipe's lock is given back, since the big lock comes
 * before every pipe's in the lock order; Linux posts it under
 * `pipe->mutex` (fs/pipe.c:458 and 499 in 6.12), under which its
 * `siglock` comes.
 */
static void pipe_sigpipe(void)
{
    struct k_siginfo info;
    int big = !bkl_held();

    k_memset(&info, 0, sizeof(info));
    info.code = SI_USER;
    info.pid = current_proc->tgid;
    info.uid = current_proc->ruid;
    if (big) {
        bkl_enter();
    }
    signal_post_thread(current_proc, SIGPIPE, &info);
    if (big) {
        bkl_exit();
    }
}

/* How `pipe_put` and `pipe_put_vec` take their bytes, a mask. */
#define PUT_NONBLOCK 0x1u /* never wait for room */
#define PUT_MERGE    0x2u /* a write(2)'s, which fill Linux's page buffers */
#define PUT_PINNED   0x4u /* vmsplice's, from pages a pin may take */

/** Wait for room in `p` for a put with `how` that has written `done`
 * bytes, with the pipe's lock given back for the wait (`pipe_sleep`): 0
 * once the ring has a byte free, or a buffer more (`pipe_slot_room`) when
 * the next byte opens one, `opens`, -EAGAIN when the put may not wait, as
 * with PUT_NONBLOCK or for a splice past its first byte, or -EINTR. */
static int32_t pipe_put_wait(pipe_t *p, uint32_t how, uint32_t done, int opens)
{
    int merge = (how & PUT_MERGE) != 0;

    if ((how & PUT_NONBLOCK) || (!merge && done > 0)) {
        return -EAGAIN;
    }
    if (pipe_sleep(p, NULL, &p->write_wq, opens ? pipe_slot_writable : pipe_writable, p,
                   merge ? "pipe_write" : "pipe_wait_writable") < 0) {
        return -EINTR;
    }
    return 0;
}

/** Whether the next byte of a splice's `pipe_put`, `src` bytes into its
 * source page, opens a buffer of `p`, as `pipe_buf_add_pages` judges it: at
 * a page's start, as the call's first, `fresh`, or the first after another
 * writer's, `other`, or after a newest buffer that is empty or gone. */
static int pipe_put_opens(const pipe_t *p, uint32_t src, int fresh, int other)
{
    return fresh || other || pipe_buf_tail_empty(p) || (src & (PAGE_SIZE - 1)) == 0;
}

/** Whether `p` has room for the next bytes of a put: a byte free, and when
 * the next byte opens a buffer, `opens`, a slot free as well, as Linux's
 * `pipe_write` and `add_to_pipe` stop at `pipe_full`. */
static int pipe_put_room(const pipe_t *p, int opens)
{
    if (pipe_free_space(p) == 0) {
        return 0;
    }
    return !opens || pipe_slots_free(p) != 0;
}

/** Whether a pin may take the caller's page at `addr` for a read
 * (`paging_pin_allows`), judged under the address space's regions' lock,
 * which keeps its region from changing during the look. */
static int pipe_pin_allows(uintptr_t addr)
{
    struct mm *mm = current_proc->mm;
    int ok;

    mmap_read_lock(mm);
    ok = paging_pin_allows(mm, addr, 0);
    mmap_unlock(mm);
    return ok;
}

/** Make present the page of the caller's memory at `addr` that the next of
 * `n` bytes of a `pipe_put` with `how` come from, as `pipe_read` does: 1,
 * or 0 for a page the caller cannot read or, with PUT_PINNED, one a pin may
 * not take before it is brought in, since a page of a mapping without
 * PROT_READ passes only when it is there already (`pipe_pin_allows`). */
static int pipe_put_fault(uintptr_t addr, uint32_t n, uint32_t how)
{
    if ((how & PUT_PINNED) && !pipe_pin_allows(addr)) {
        return 0;
    }
    return vm_prefault_range(addr, page_piece(addr, n), 0);
}

/** Copy the `n` bytes at `from`, within one page, to `at` for a `pipe_put`
 * with `how`: 0, or -EFAULT for a page the caller cannot read or, with
 * PUT_PINNED, one a pin may not take. Takes no page in before the check. */
static int32_t pipe_put_copy(uint8_t *at, const char __user *from, uint32_t n, uint32_t how)
{
    if ((how & PUT_PINNED) && !pipe_pin_allows((__force uintptr_t)from)) {
        return -EFAULT;
    }
    return copy_from_user(at, (const void __user *)from, n) != 0 ? -EFAULT : 0;
}

/** Whether the `len` bytes at `addr` lie in user space, as Linux's
 * access_ok judges them (`user_span_ok`), or in the thread's window
 * (`user_range_ok`). An address below USER_ADDR_MIN passes, since its page
 * fails in the transfer, as Linux's fails in the copy. */
static int pipe_span_ok(uintptr_t addr, uint32_t len)
{
    return user_span_ok(addr, len) || user_range_ok(addr, len);
}

/**
 * The write of a splice or vmsplice into a pipe end: what fits of the
 * `count` bytes at `buf`, waiting only before the first byte, and not at
 * all with PUT_NONBLOCK in `how`. Returns the bytes written, or, before a
 * transfer, -EBADF for a descriptor not open for writing, -EFAULT for a bad
 * buffer, -EAGAIN for a full pipe that may not be waited on, -EINTR when a
 * signal ends the wait, or -ENOMEM when the heap has no room for the ring;
 * each of these after a transfer returns the count written instead. With no
 * reader left the write raises SIGPIPE in the calling thread, as Linux sends
 * it, and returns -EPIPE or the count written. Readers are woken for data at
 * every transfer, so an edge-triggered epoll registration of the read end is
 * told of each, as Linux tells one of a pipe.
 *
 * The bytes take a buffer for each page of the source they come from, whose
 * first byte lies `src` bytes into its page (`pipe_buf_add_pages`), and a
 * transfer stays within one such page. The pipe's lock is held throughout
 * but for the wait for room, as Linux's `vmsplice_to_pipe` and
 * `splice_file_to_pipe` hold `pipe_lock` (fs/splice.c:1558-1562 and
 * 1295-1299 in 6.12), so no other reader or writer uses the ring while a
 * transfer brings its page in; the readers and the ring are judged again
 * after that sleep. The splice stops where Linux's ring of a buffer for
 * each page of the pipe's size is full (`pipe_put_room`), however few bytes
 * its buffers hold, and waits for room only before its first byte, in
 * `pipe_wait_writable`, returning what fitted once the ring fills, as
 * Linux's `wait_for_space` and `iter_to_pipe` do. With PUT_PINNED, a source
 * page that Linux's get_user_pages refuses, such as `[vvar]`, ends the copy
 * with -EFAULT or the count written, judged before the page is brought in,
 * since a page of a mapping without PROT_READ passes only when it was there
 * already, and again after the last sleep before its copy.
 */
static int32_t pipe_put(fd_entry_t *e, const char __user *buf, uint32_t count, uint32_t how,
                        uint32_t src)
{
    pipe_t *p = (pipe_t *)e->priv;
    uint32_t done = 0, last;
    int32_t err = 0;
    int fresh = 1, opens;

    if (!(e->flags & FD_FLAG_WRITE)) {
        return -EBADF;
    }
    if (count == 0) {
        return 0;
    }
    if (!pipe_span_ok((__force uintptr_t)buf, count)) {
        return -EFAULT;
    }

    pipe_lock(p, NULL);
    pipe_note_writer(p);
    last = p->tail;
    while (done < count) {
        uint8_t *at;
        uint32_t chunk;
        int32_t rc;
        uintptr_t addr = (__force uintptr_t)buf + done;

        if (!p->read_open) {
            err = -EPIPE;
            break;
        }
        if (pipe_ring_ready(p) < 0) {
            err = -ENOMEM;
            break;
        }

        opens = pipe_put_opens(p, src + done, fresh, p->tail != last);
        if (!pipe_put_room(p, opens)) {
            rc = pipe_put_wait(p, how, done, opens);
            if (rc < 0) {
                err = rc;
                break;
            }
            continue;
        }

        if (!pipe_put_fault(addr, count - done, how)) {
            err = -EFAULT;
            break;
        }
        if (!p->read_open) {
            continue;
        }
        if (p->tail != last) {
            fresh = 1;
        }
        if (!pipe_put_room(p, pipe_put_opens(p, src + done, fresh, 0))) {
            continue;
        }
        chunk = pipe_ring_write_span(p, &at);
        if (chunk == 0) {
            continue;
        }
        if (chunk > count - done) {
            chunk = count - done;
        }
        chunk = page_piece(src + done, page_piece(addr, chunk));
        rc = pipe_put_copy(at, (const char __user *)addr, chunk, how);
        if (rc < 0) {
            err = rc;
            break;
        }
        pipe_buf_add_pages(p, chunk, src + done, fresh);
        fresh = 0;
        pipe_ring_produce(p, chunk);
        last = p->tail;
        done += chunk;
        pipe_wake(p, &p->read_wq, WQ_DATA);
    }
    pipe_unlock(p, NULL);
    if (err == -EPIPE) {
        pipe_sigpipe();
    }
    return (done > 0) ? (int32_t)done : err;
}

/** The address of the next byte of `iov` for a `pipe_put_vec`, `*off`
 * bytes into element `*k`, which moves past the elements used up or empty,
 * and in `*left` the bytes left in that element. A byte must remain. */
static uintptr_t pipe_iov_at(const struct kernel_iovec *iov, int *k, uint32_t *off, uint32_t *left)
{
    while (*off == iov[*k].iov_len) {
        (*k)++;
        *off = 0;
    }
    *left = (uint32_t)iov[*k].iov_len - *off;
    return iov[*k].iov_base + *off;
}

/**
 * Copy the `n` bytes of `iov` from `*off` bytes into element `*k` into the
 * ring of `p` at its tail, which has room for them, moving `*k` and `*off`
 * past them, a page of the source at a time; the tail does not move.
 * Returns 0, or -EFAULT when a byte cannot be read, with the bytes before
 * it in the ring past the tail, where no reader sees them. Each piece is
 * brought in and copied under the regions' lock (`copy_from_user`), which
 * may sleep with the pipe's lock held.
 */
static int32_t pipe_iov_copy(pipe_t *p, const struct kernel_iovec *iov, int *k, uint32_t *off,
                             uint32_t n)
{
    uint32_t at = p->tail;

    while (n > 0) {
        uint32_t left, slot = at & (p->size - 1);
        uintptr_t addr = pipe_iov_at(iov, k, off, &left);

        left = page_piece(addr, left < n ? left : n);
        if (left > p->size - slot) {
            left = p->size - slot;
        }
        if (copy_from_user(&p->buf[slot], (const void __user *)addr, left) != 0) {
            return -EFAULT;
        }
        *off += left;
        at += left;
        n -= left;
    }
    return 0;
}

/**
 * Copy the `n` bytes at the head of the ring of `p` into `iov` from `*off`
 * bytes into element `*k`, moving `*k` and `*off` past them, a page of the
 * destination at a time; the head does not move. Returns 0, or -EFAULT
 * when a byte cannot be written, with the bytes before it copied. Each
 * piece is brought in for writing and copied under the regions' lock
 * (`copy_to_user`), which may sleep with the pipe's lock held.
 */
static int32_t pipe_iov_fill(const pipe_t *p, const struct kernel_iovec *iov, int *k, uint32_t *off,
                             uint32_t n)
{
    uint32_t at = p->head;

    while (n > 0) {
        uint32_t left, slot = at & (p->size - 1);
        uintptr_t addr = pipe_iov_at(iov, k, off, &left);

        left = page_piece(addr, left < n ? left : n);
        if (left > p->size - slot) {
            left = p->size - slot;
        }
        if (copy_to_user((void __user *)addr, &p->buf[slot], left) != 0) {
            return -EFAULT;
        }
        *off += left;
        at += left;
        n -= left;
    }
    return 0;
}

/** The unread bytes of the oldest buffer of `p`, which holds bytes, as
 * Linux's `buf->len`: up to the next buffer's start, or all the bytes when
 * there is no next buffer. A pipe that keeps no buffers (`buf_pages`) is
 * read to the end of the head's page of the ring. */
static uint32_t pipe_buf_head_len(const pipe_t *p)
{
    uint32_t avail = pipe_available(p), len;

    if (!p->buf_pages) {
        len = PAGE_SIZE - (p->head & (PAGE_SIZE - 1));
    } else if (p->bufs > 1) {
        len = pipe_buf_at(p, 1) - p->head;
    } else {
        len = avail;
    }
    return len == 0 || len > avail ? avail : len;
}

/**
 * The read of a pipe end into the `n` elements of `iov`, a vector
 * iov_import has checked and cut, as Linux's `pipe_read` reads an iov_iter:
 * read(2) is a vector of one. Returns the bytes read; 0 for a vector of no
 * bytes, or for an empty pipe with no writer left; or, before any byte,
 * -EBADF for a descriptor not open for reading, -EFAULT for a buffer past
 * user space or one that cannot be written, -EAGAIN for an empty pipe with
 * O_NONBLOCK, or -EINTR when a signal ends the wait. The wait, the end of
 * input and -EAGAIN come before any copy, so a vector whose first buffer
 * cannot be written reads 0 from an ended pipe, as Linux's does.
 *
 * Each buffer of the pipe (`pipe_buf_head_len`) is copied whole, or as much
 * of it as the vector has room for, or not at all, as Linux's `pipe_read`
 * advances a buffer only once `copy_page_to_iter` has copied all it asked:
 * a destination page that cannot be written ends the read with the bytes of
 * the buffers before it, or -EFAULT, and the buffer keeps every byte. The
 * read goes on through the buffers while bytes are buffered, and waits only
 * before its first byte; a blocking read spins once for the next write
 * while the last writer runs on another processor (`pipe_read_spin`) and
 * sleeps if none comes. An empty buffer a faulted write left is let go of
 * when the read reaches it with room left (`pipe_buf_skip_empty`), so a
 * read of a pipe that holds only such buffers waits, or is -EAGAIN, as
 * Linux's.
 *
 * The pipe's lock is held from the start to the return but for the wait for
 * bytes, the spin's and the sleep's (`pipe_sleep`), as Linux's `pipe_read`
 * holds `pipe->mutex` across its copies' faults and drops it only to wait
 * (fs/pipe.c:264, 361, 393 and 399 in 6.12), so while a destination page comes
 * in no other reader takes the next buffer and no writer grows the one being
 * read: the bytes a read returns are adjacent in the stream. Each page of a
 * copy is brought in for writing and copied under the address space's regions'
 * lock, so another thread's unmap comes before or after it and a page it took
 * away ends the copy with -EFAULT (`pipe_iov_fill`); the head moves past a
 * buffer once its copy is whole. The lock is the pipe's alone: the read takes
 * no big lock, and nothing but the lock holder moves the ring. The writers are
 * woken once, after the lock is given back, as Linux's `pipe_read` wakes them
 * (fs/pipe.c:399-402), and not at each buffer: a writer woken at the first
 * would run on another processor, fill the one slot and wait again, at the cost
 * of a wake for each page.
 */
static int32_t pipe_read_vec(fd_entry_t *e, const struct kernel_iovec *iov, int n)
{
    pipe_t *p = (pipe_t *)e->priv;
    uint32_t count = 0, done = 0, off = 0;
    int32_t err = 0;
    int i, k = 0, room = 0, spun = 0;

    if (!(e->flags & FD_FLAG_READ)) {
        return -EBADF;
    }
    for (i = 0; i < n; i++) {
        if (!pipe_span_ok(iov[i].iov_base, (uint32_t)iov[i].iov_len)) {
            return -EFAULT;
        }
        count += (uint32_t)iov[i].iov_len;
    }
    if (count == 0) {
        return 0;
    }

    pipe_lock(p, NULL);
    while (done < count) {
        uint32_t len;

        pipe_buf_skip_empty(p);
        if (pipe_available(p) == 0) {
            if (done > 0 || !p->write_open) {
                break;
            }
            if (fd_is_nonblock(e)) {
                err = -EAGAIN;
                break;
            }
            if (!spun) {
                spun = 1;
                if (pipe_read_spin(p)) {
                    continue;
                }
            }
            if (pipe_sleep(p, NULL, &p->read_wq, pipe_readable, p, "pipe_read") < 0) {
                err = -EINTR;
                break;
            }
            continue;
        }
        if (p->spliced) {
            if (done > 0) {
                break;
            }
            if (pipe_wait_unspliced(p, NULL) < 0) {
                err = -EINTR;
                break;
            }
            continue;
        }
        len = pipe_buf_head_len(p);
        if (len > count - done) {
            len = count - done;
        }
        if (pipe_iov_fill(p, iov, &k, &off, len) < 0) {
            err = -EFAULT;
            break;
        }
        pipe_ring_consume(p, len);
        pipe_buf_drop(p);
        done += len;
        room = 1;
    }
    pipe_unlock(p, NULL);
    if (room) {
        pipe_wake(p, &p->write_wq, WQ_ROOM);
    }
    return (done > 0) ? (int32_t)done : err;
}

/**
 * Leave in `p` the empty buffer of a write's page group whose source faulted,
 * as Linux's `pipe_write` moves `pipe->head` past an empty buffer before its
 * copy and leaves it in the ring after a fault (fs/pipe.c:521-545 in 6.12):
 * poll reports it as data, it takes a slot, a later write's first `count` mod
 * 4096 bytes join it (`pipe_buf_merge_room`), and a read lets go of it
 * (`pipe_buf_skip_empty`). The readers are woken, as Linux wakes them for an
 * empty pipe (fs/pipe.c:601-602). Nothing is opened when no slot is free, since
 * a sleep in the fault may have let other writers fill the ring.
 */
static void pipe_buf_fault(pipe_t *p)
{
    uint32_t bufs = p->bufs;

    if (pipe_slots_free(p) == 0) {
        return;
    }
    pipe_buf_open(p, p->tail, 0, 1);
    if (p->bufs != bufs) {
        pipe_wake(p, &p->read_wq, WQ_DATA);
    }
}

/**
 * The write of a pipe end: the `n` elements of the vector `iov`, whose
 * ranges iov_import or the caller has cut, `count` bytes in all, as one
 * write, which waits whenever the ring is full until the last byte is in,
 * unless `nonblock`. Returns the bytes written, or, before a byte is
 * written, -EBADF for a descriptor not open for writing, -EFAULT for a bad
 * buffer, -EAGAIN for a full pipe with `nonblock`, -EINTR when a signal ends
 * a wait, or -ENOMEM when the heap has no room for the ring; each of these
 * after bytes are written returns the count written instead. With no reader
 * left the write raises SIGPIPE in the calling thread, as Linux sends it,
 * and returns -EPIPE or the count written. Readers are woken for data at
 * every buffer the write commits, the ring empty before it or not, so an
 * edge-triggered epoll registration of the read end is told of each, as
 * Linux tells one of a pipe.
 *
 * A vector is one write, as Linux runs a writev through one `pipe_write`.
 * The newest buffer takes the write's first `count` mod 4096 bytes when they
 * fit in its page (`pipe_buf_merge_room`), and each page buffer after it
 * fills across the elements; a buffer is opened only while a slot is free,
 * as Linux's `pipe_write` opens one only while `pipe_full` is false, and the
 * write waits in `pipe_write`, as Linux's does, for a slot when its next
 * bytes open a buffer.
 *
 * The pipe's lock is held from the start to the return but for the wait for
 * room, as Linux's `pipe_write` holds `pipe->mutex` across its copies'
 * faults and drops it only to wait (fs/pipe.c:455, 575, 580 and 587 in
 * 6.12), so no other writer's bytes come between a write's own while its
 * source pages come in, and no reader takes them half made.
 *
 * Each of those groups, the merged bytes and each page, is committed whole
 * or not at all, as Linux commits a page only once it is full or the vector
 * is spent: the group is copied past the tail, each page of its source
 * brought in and copied under the regions' lock (`pipe_iov_copy`), and only
 * then does the tail move past it. A
 * page the caller cannot read ends the write with the bytes of the groups
 * before it, or -EFAULT, and no byte of its own group is readable. A page
 * group that faults leaves its buffer open and empty (`pipe_buf_fault`), as
 * Linux's `pipe_write` moves the head past a buffer before its copy and
 * leaves it after a fault; merged bytes that fault open nothing. A write of
 * PIPE_BUF bytes or fewer is one group, so no other writer's bytes come
 * between its own.
 *
 * A free slot means a page of the ring is free, since each buffer holds a
 * page at most and the ring has a page for each slot.
 *
 * The readers are woken once for the bytes the call put in, before it
 * waits for room and after it gives the lock back, as Linux's `pipe_write`
 * wakes them (fs/pipe.c:575-577, 587-602), and not at each page: a reader
 * woken at the first would run on another processor, take one page and wait
 * again, at the cost of a wake for each page. An edge-triggered epoll
 * registration of the read end is told once for each write, as Linux's.
 */
static int32_t pipe_put_vec(fd_entry_t *e, const struct kernel_iovec *iov, int n, uint32_t count,
                            int nonblock)
{
    pipe_t *p = (pipe_t *)e->priv;
    uint32_t how = PUT_MERGE | (nonblock ? PUT_NONBLOCK : 0u);
    uint32_t done = 0, fill, last, off = 0;
    int32_t err = 0;
    int i, k = 0, data = 0;

    if (!(e->flags & FD_FLAG_WRITE)) {
        return -EBADF;
    }
    if (count == 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (!pipe_span_ok(iov[i].iov_base, (uint32_t)iov[i].iov_len)) {
            return -EFAULT;
        }
    }

    pipe_lock(p, NULL);
    pipe_note_writer(p);
    fill = pipe_buf_merge_room(p, count);
    last = p->tail;
    while (done < count) {
        uint32_t len;
        int32_t rc;

        if (!p->read_open) {
            err = -EPIPE;
            break;
        }
        if (pipe_ring_ready(p) < 0) {
            err = -ENOMEM;
            break;
        }
        if (p->tail != last || p->bufs == 0 || pipe_free_space(p) < fill) {
            fill = 0;
        }
        len = fill != 0 ? fill : PAGE_SIZE;
        if (len > count - done) {
            len = count - done;
        }
        if (len > pipe_free_space(p)) {
            len = pipe_free_space(p);
        }
        if (!pipe_put_room(p, fill == 0)) {
            if (data) {
                pipe_wake(p, &p->read_wq, WQ_DATA);
                data = 0;
            }
            rc = pipe_put_wait(p, how, done, fill == 0);
            if (rc < 0) {
                err = rc;
                break;
            }
            continue;
        }
        if (pipe_iov_copy(p, iov, &k, &off, len) < 0) {
            if (fill == 0) {
                pipe_buf_fault(p);
            }
            err = -EFAULT;
            break;
        }
        pipe_buf_add(p, len, &fill);
        pipe_ring_produce(p, len);
        last = p->tail;
        done += len;
        data = 1;
    }
    pipe_unlock(p, NULL);
    if (data) {
        pipe_wake(p, &p->read_wq, WQ_DATA);
    }
    if (err == -EPIPE) {
        pipe_sigpipe();
    }
    return (done > 0) ? (int32_t)done : err;
}

/** The write of a pipe end (`pipe_put_vec`), waiting for room unless the
 * description has O_NONBLOCK. */
static int32_t pipe_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct kernel_iovec one;

    one.iov_base = (__force uintptr_t)buf;
    one.iov_len = count;
    return pipe_put_vec(e, &one, 1, count, fd_is_nonblock(e));
}

/* poll(2) event bits, the Linux values. */
#define PIPE_POLLIN  0x001
#define PIPE_POLLOUT 0x004
#define PIPE_POLLERR 0x008
#define PIPE_POLLHUP 0x010

/**
 * The poll of a pipe end, with `*wq` set to the end's own queue: the read
 * end's `read_wq`, which data and a writer's close wake, or the write end's
 * `write_wq`, which freed room and a reader's close wake. A FIFO opened for
 * reading and writing is both ends, and its pollers wait on `rdwr_wq`, which
 * every change of either wakes (`pipe_wake`).
 *
 * A poller wakes for what ends the wait of a read or a write. The read end
 * is POLLIN while it holds data or a buffer, an empty one a faulted write
 * left among them (`pipe_holds`), as Linux's tests `pipe_empty`, and
 * POLLHUP once the writers are gone, so
 * an empty pipe with no writers, whose read returns 0, reports POLLHUP
 * alone, as on Linux. A FIFO reader opened nonblocking with no writer
 * reports no hangup until a writer has opened since that read open.
 * The write end is writable with room or the readers
 * gone, and POLLERR once the readers are gone (the write then fails with
 * EPIPE). POLLHUP and POLLERR are reported whatever `events` asks.
 *
 * It runs without the big lock and the pipe's lock (FILE_OPS_POLL_UNLOCKED),
 * reading each word of the pipe once, as Linux's pipe_poll reads the ring
 * after poll_wait (fs/pipe.c:656-705): every change a poller waits for is
 * made before the wake of the queue named here, whose lock the poller's
 * record went on under before this read, so the read sees the change or the
 * wake finds the poller. The first poll marks the pipe polled, and passes
 * a barrier before it reads, so that every change after it wakes and
 * counts (`pipe_wake`).
 */
static short pipe_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    pipe_t *p = (pipe_t *)e->priv;
    const volatile pipe_t *v = p;
    uint32_t used, bufs, size;
    int readers, writers;
    short revents = 0;

    if (!p->polled) {
        p->polled = 1;
        sched_mb();
    }
    if (wq) {
        if ((e->flags & FD_FLAG_READ) && (e->flags & FD_FLAG_WRITE)) {
            *wq = &p->rdwr_wq;
        } else {
            *wq = (e->flags & FD_FLAG_READ) ? &p->read_wq : &p->write_wq;
        }
    }
    used = v->tail - v->head;
    bufs = v->bufs;
    size = v->size;
    readers = v->read_open;
    writers = v->write_open;

    if (e->flags & FD_FLAG_READ) {
        if (used != 0 || bufs != 0) {
            revents |= (short)(events & PIPE_POLLIN);
        }
        if (!writers && (!p->fifo || e->ofd->fifo_write_opens != v->write_opens)) {
            revents |= PIPE_POLLHUP;
        }
    }
    if (e->flags & FD_FLAG_WRITE) {
        if ((used < size && bufs < size / PAGE_SIZE) || !readers) {
            revents |= (short)(events & PIPE_POLLOUT);
        }
        if (!readers) {
            revents |= PIPE_POLLERR;
        }
    }
    return revents;
}

/** The ioctls of a pipe end: FIONREAD stores the bytes buffered, an int, at
 * `arg`, or fails with -EFAULT, counting them under the pipe's lock, as
 * Linux's `pipe_ioctl` does (fs/pipe.c:622-632 in 6.12), so a read or a
 * write that sleeps in a fault ends first; any other request is -ENOTTY. */
static int32_t pipe_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    pipe_t *p = (pipe_t *)e->priv;

    switch (request) {
    case FIONREAD: {
        int n;

        pipe_lock(p, NULL);
        n = (int)pipe_available(p);
        pipe_unlock(p, NULL);
        if (copy_to_user((void __user *)arg, &n, sizeof(n)) != 0) {
            return -EFAULT;
        }
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

/**
 * Close the end `e` when its description's last reference goes
 * (`fd_release_backing`): count the end closed, wake the other end's queue
 * for every kind of change, so that a waiting reader sees the end of file,
 * or a waiting writer the broken pipe, once no end of the closed kind is
 * left, and free the pipe once no description of it is left
 * (`pipe_info_put`). A FIFO opened for reading and writing is an end of
 * both kinds. The counts change under the pipe's lock, as Linux's
 * `pipe_release` changes them under `pipe->mutex` (fs/pipe.c:722-743 in
 * 6.12), so a read or a write that holds it sees them change only while it
 * waits, and judges them again when it wakes. May sleep.
 */
static void pipe_close(fd_entry_t *e)
{
    pipe_t *p = (pipe_t *)e->priv;

    if (!p) {
        return;
    }
    pipe_lock(p, NULL);
    if (e->flags & FD_FLAG_READ) {
        p->read_open--;
        pipe_wake(p, &p->write_wq, WQ_ALL);
    }
    if (e->flags & FD_FLAG_WRITE) {
        p->write_open--;
        pipe_wake(p, &p->read_wq, WQ_ALL);
    }
    if ((e->flags & FD_FLAG_READ) && (e->flags & FD_FLAG_WRITE)) {
        p->rdwr_open--;
    }
    pipe_unlock(p, NULL);
    pipe_info_put(p);
}

/* Every operation but poll runs under the pipe's own lock without the big
 * lock, which it takes, with the pipes' locks given back, only to post
 * SIGPIPE (`pipe_sigpipe`) and to ask `signal_pending` once the signal-work
 * flag ends a wait (`pipe_signal_pending`); poll takes neither. A FIFO's
 * operations but poll keep the big lock (`file_ops_unlocked`). */
static const struct file_ops pipe_ops = {
    pipe_read,  pipe_write, pipe_poll, pipe_ioctl,
    pipe_close, NULL,       NULL,      FILE_OPS_UNLOCKED | FILE_OPS_POLL_UNLOCKED};

int pipe_is(const fd_entry_t *e)
{
    return e->ops == &pipe_ops;
}

int32_t pipe_splice_write(fd_entry_t *e, const char __user *buf, uint32_t count, int nonblock,
                          uint32_t src)
{
    return pipe_put(e, buf, count, nonblock ? PUT_NONBLOCK : 0u, src);
}

int32_t pipe_writev(fd_entry_t *e, const struct kernel_iovec *iov, int count)
{
    uint32_t total = 0;
    int i;

    for (i = 0; i < count; i++) {
        total += (uint32_t)iov[i].iov_len;
    }
    return pipe_put_vec(e, iov, count, total, fd_is_nonblock(e));
}

int32_t pipe_readv(fd_entry_t *e, const struct kernel_iovec *iov, int count)
{
    return pipe_read_vec(e, iov, count);
}

int32_t pipe_vmsplice_write(fd_entry_t *e, const char __user *buf, uint32_t count, int nonblock)
{
    return pipe_put(e, buf, count, PUT_PINNED | (nonblock ? PUT_NONBLOCK : 0u),
                    (uint32_t)((__force uintptr_t)buf & (PAGE_SIZE - 1)));
}

int32_t pipe_splice_room(fd_entry_t *e, int nonblock, uint32_t src)
{
    pipe_t *p = (pipe_t *)e->priv;
    int32_t rc;

    if (!(e->flags & FD_FLAG_WRITE)) {
        return -EBADF;
    }
    pipe_lock(p, NULL);
    for (;;) {
        if (!p->read_open) {
            rc = -EPIPE;
            break;
        }
        if (pipe_slot_room(p)) {
            uint32_t room = pipe_slots_free(p) * PAGE_SIZE - (src & (PAGE_SIZE - 1));

            rc = (int32_t)(room < pipe_free_space(p) ? room : pipe_free_space(p));
            break;
        }
        if (nonblock) {
            rc = -EAGAIN;
            break;
        }
        if (pipe_sleep(p, NULL, &p->write_wq, pipe_slot_writable, p, NULL) < 0) {
            rc = -EINTR;
            break;
        }
    }
    pipe_unlock(p, NULL);
    if (rc == -EPIPE) {
        pipe_sigpipe();
    }
    return rc;
}

/**
 * Wait until the pipe `p` has bytes, or has no writer left, with its lock
 * held and given back for the wait (`pipe_sleep`), for a splice to a file
 * or a socket and for vmsplice: the bytes buffered, 0 for none and no
 * writer, -EAGAIN for none with `nonblock`, or -EINTR when a signal ends
 * the wait. The empty buffers at the front, which faulted writes left, are
 * let go of first, as a read lets go of them (`pipe_buf_skip_empty`) and as
 * Linux's `splice_from_pipe_next` eats them (fs/splice.c:495-507 and 551 in
 * 6.12), so the splice waits for bytes.
 */
static int32_t pipe_wait_data(pipe_t *p, int nonblock)
{
    for (;;) {
        pipe_buf_skip_empty(p);
        if (pipe_available(p) != 0) {
            break;
        }
        if (!p->write_open) {
            return 0;
        }
        if (nonblock) {
            return -EAGAIN;
        }
        if (pipe_sleep(p, NULL, &p->read_wq, pipe_readable, p, NULL) < 0) {
            return -EINTR;
        }
    }
    return (int32_t)pipe_available(p);
}

/**
 * Copy the first `n` bytes buffered in the ring of `src` to `dst` when that
 * is not NULL, or otherwise into the ring of `to` at its tail, a contiguous
 * span of each at a time. The caller has checked that `src` buffers `n`
 * bytes and that `to` has room for them; neither ring's counters move.
 */
static void pipe_ring_copy(const pipe_t *src, uint8_t *dst, pipe_t *to, uint32_t n)
{
    uint32_t done = 0;

    while (done < n) {
        uint32_t s = (src->head + done) & (src->size - 1), chunk = src->size - s;
        uint32_t t = to ? (to->tail + done) & (to->size - 1) : 0;

        if (to && chunk > to->size - t) {
            chunk = to->size - t;
        }
        if (chunk > n - done) {
            chunk = n - done;
        }
        k_memcpy(dst ? dst + done : &to->buf[t], &src->buf[s], chunk);
        done += chunk;
    }
}

int32_t pipe_get_size(const fd_entry_t *e)
{
    return (int32_t)((const pipe_t *)e->priv)->size;
}

/**
 * Charge the pages of a ring of `want` bytes to the user of `p` in place of
 * its present ring's, for a resize: the pages charged before, to give back
 * with `pipe_uncharge` if the resize fails, or -EPERM for a growth past
 * `pipe_max_size` or the user's limits that an unprivileged caller,
 * `privileged` 0, may not make. The check and the charge are one hold of
 * `pipes_lock`, as Linux's `pipe_set_size` charges the user's count and
 * reads it in one step before it judges the limits and reverts the charge
 * on a refusal (fs/pipe.c:1363-1381 in 6.12), so two resizes of one user's
 * pipes cannot both pass a limit that only one fits under.
 */
static int32_t pipe_charge(pipe_t *p, uint32_t want, int privileged)
{
    uint32_t irq = spin_lock_irqsave(&pipes_lock);
    uint32_t was = p->nr_accounted;

    if (want > p->size && !privileged) {
        unsigned long pages = (unsigned long)pipe_user_pages(p->user) - was + want / PAGE_SIZE;

        if (want > pipe_max_size || pipe_pages_over(pages, pipe_user_pages_soft) ||
            pipe_pages_over(pages, pipe_user_pages_hard)) {
            spin_unlock_irqrestore(&pipes_lock, irq);
            return -EPERM;
        }
    }
    p->nr_accounted = want / PAGE_SIZE;
    spin_unlock_irqrestore(&pipes_lock, irq);
    return (int32_t)was;
}

/** Charge `p`'s user again with `was` pages, its charge before a resize
 * that failed (`pipe_charge`). */
static void pipe_uncharge(pipe_t *p, int32_t was)
{
    uint32_t irq = spin_lock_irqsave(&pipes_lock);

    p->nr_accounted = (uint32_t)was;
    spin_unlock_irqrestore(&pipes_lock, irq);
}

/**
 * Give `p` a ring of `size` bytes for F_SETPIPE_SZ (`pipe_set_size`). A new
 * ring is allocated, whether or not the pipe had one, as Linux's
 * `pipe_resize_ring` allocates its slots, so that a size the heap cannot
 * give fails here and not at every write; the bytes buffered are copied to
 * the slots their counters name in it, so `head` and `tail` keep counting
 * as they did. A growth wakes the writers, which may now have room, once
 * the new size is stored, since a poller reads it without the pipe's lock
 * and would otherwise take the wake, judge the old size full and sleep
 * again, as Linux's `pipe_resize_ring` sets `max_usage` before its wake
 * (fs/pipe.c:1322 and 1329 in 6.12). The limits are Linux's
 * `pipe_set_size`'s, judged as the new size is charged (`pipe_charge`), and
 * a shrink passes them even for a user over the soft limit; a resize that
 * fails gives the charge back. A shrink below the page buffers fails with
 * EBUSY, as in Linux, however few bytes they hold, an empty one a faulted
 * write left among them (`pipe_buf_fault`): a write opens a page unless its
 * first `count` mod 4096 bytes fit in the newest buffer's
 * (`pipe_buf_merge_room`), and tee, splice and vmsplice open one for each
 * buffer or source page their bytes come from (`pipe_buf_link`,
 * `pipe_buf_add_pages`). Called with the pipe's lock held.
 */
static int32_t pipe_resize(pipe_t *p, uint32_t size, int privileged)
{
    uint32_t want = pipe_round_size(size), done = 0, n = pipe_available(p), i, was;
    struct pipe_page *slots;
    uint8_t *ring;
    int32_t rc;

    if (want == 0) {
        return -EINVAL;
    }
    if (want == p->size) {
        return (int32_t)want;
    }
    rc = pipe_charge(p, want, privileged);
    if (rc < 0) {
        return rc;
    }
    if (pipe_ring_new(want, &ring, &slots) < 0) {
        pipe_uncharge(p, rc);
        return -ENOMEM;
    }
    if (n > want || p->bufs > want / PAGE_SIZE) {
        kfree(ring);
        kfree(slots);
        pipe_uncharge(p, rc);
        return -EBUSY;
    }
    for (i = 0; i < p->bufs; i++) {
        slots[i] = *pipe_buf_page(p, i);
    }
    while (done < n) {
        uint32_t s = (p->head + done) & (p->size - 1), t = (p->head + done) & (want - 1);
        uint32_t chunk = p->size - s;

        if (chunk > want - t) {
            chunk = want - t;
        }
        if (chunk > n - done) {
            chunk = n - done;
        }
        k_memcpy(&ring[t], &p->buf[s], chunk);
        done += chunk;
    }
    kfree(p->buf);
    kfree(p->buf_pages);
    p->buf = ring;
    p->buf_pages = slots;
    p->buf_cap = pipe_buf_slots(want);
    p->buf_first = 0;
    was = p->size;
    p->size = want;
    if (want > was) {
        pipe_wake(p, &p->write_wq, WQ_ROOM);
    }
    return (int32_t)want;
}

/** F_SETPIPE_SZ of `e` (`pipe_resize`) under the pipe's lock, as Linux's
 * `pipe_fcntl` holds it (fs/pipe.c:1408-1422 in 6.12), so a read or a write
 * that sleeps in a fault ends before the ring moves. */
int32_t pipe_set_size(fd_entry_t *e, uint32_t size, int privileged)
{
    pipe_t *p = (pipe_t *)e->priv;
    int32_t rc;

    pipe_lock(p, NULL);
    rc = pipe_resize(p, size, privileged);
    pipe_unlock(p, NULL);
    return rc;
}

int32_t pipe_splice_peek(fd_entry_t *e, uint8_t *dst, uint32_t n, int nonblock)
{
    pipe_t *p = (pipe_t *)e->priv;
    int32_t avail;

    if (!(e->flags & FD_FLAG_READ)) {
        return -EBADF;
    }
    pipe_lock(p, NULL);
    for (;;) {
        avail = pipe_wait_data(p, nonblock);
        if (avail <= 0) {
            pipe_unlock(p, NULL);
            return avail;
        }
        if (!p->spliced) {
            break;
        }
        if (pipe_wait_unspliced(p, NULL) < 0) {
            pipe_unlock(p, NULL);
            return -EINTR;
        }
    }
    if (n > (uint32_t)avail) {
        n = (uint32_t)avail;
    }
    pipe_ring_copy(p, dst, NULL, n);
    p->spliced = 1;
    pipe_unlock(p, NULL);
    return (int32_t)n;
}

void pipe_splice_consume(fd_entry_t *e, uint32_t n)
{
    pipe_t *p = (pipe_t *)e->priv;

    pipe_lock(p, NULL);
    if (n > pipe_available(p)) {
        n = pipe_available(p);
    }
    if (n > 0) {
        pipe_ring_consume(p, n);
        pipe_buf_drop(p);
        pipe_wake(p, &p->write_wq, WQ_ROOM);
    }
    p->spliced = 0;
    wq_wake(&p->splice_wq, WQ_ALL);
    pipe_unlock(p, NULL);
}

/**
 * Wait, with the locks of `a` and `b` held and given back for the waits
 * (`pipe_sleep`), until a tee or a splice from `a` to `b` can link a
 * buffer, as Linux's `ipipe_prep` and `opipe_prep` wait
 * (fs/splice.c:1668-1735 in 6.12): `a` holds a buffer, an empty one a
 * faulted write left among them (`pipe_holds`), no splice holds `a`'s
 * bytes, and `b` has a slot and a byte free. Returns 1 then, or 0 for an
 * `a` that holds nothing and has no writer, -EAGAIN for a wait `nonblock`
 * forbids, -EPIPE for a `b` with no reader, for which the caller raises
 * SIGPIPE once it has let the locks go, or -EINTR.
 * A `b` with no reader is -EPIPE before an `a` with neither bytes nor writer
 * is 0, as Linux's `link_pipe` and `splice_pipe_to_pipe` check
 * `opipe->readers` first in their loops (fs/splice.c:1778-1783 and
 * 1902-1907 in 6.12), which `ipipe_prep` reaches with 0 for such an `a`.
 */
static int32_t pipe_link_ready(pipe_t *a, pipe_t *b, int nonblock)
{
    for (;;) {
        if (!pipe_holds(a)) {
            if (!a->write_open) {
                if (!b->read_open) {
                    return -EPIPE;
                }
                return 0;
            }
            if (nonblock) {
                return -EAGAIN;
            }
            if (pipe_sleep(a, b, &a->read_wq, pipe_readable, a, NULL) < 0) {
                return -EINTR;
            }
            continue;
        }
        if (a->spliced) {
            if (pipe_wait_unspliced(a, b) < 0) {
                return -EINTR;
            }
            continue;
        }
        if (!b->read_open) {
            return -EPIPE;
        }
        if (pipe_slot_room(b)) {
            return 1;
        }
        if (nonblock) {
            return -EAGAIN;
        }
        if (pipe_sleep(a, b, &b->write_wq, pipe_slot_writable, b, NULL) < 0) {
            return -EINTR;
        }
    }
}

/**
 * The buffers of `a` from its head that a link into `b` of at most `len`
 * bytes takes, as Linux's `link_pipe` and `splice_pipe_to_pipe` walk them
 * (fs/splice.c:1819-1856 and 1916-1949 in 6.12): each whole, or the part of
 * it `len` or `b`'s free bytes leave room for, while `b` has a slot for it
 * and `len` is not spent, an empty buffer among them, which takes a slot
 * and no byte. Sets `*n` to the bytes, `*k` to the buffers, and `*whole` to
 * whether the last is taken whole. A pipe that keeps no buffers gives the
 * bytes its ring and `b`'s slots allow, `*k` 0.
 */
static void pipe_link_plan(const pipe_t *a, const pipe_t *b, uint32_t len, uint32_t *n, uint32_t *k,
                           int *whole)
{
    uint32_t slots = pipe_slots_free(b), room = pipe_free_space(b), i;

    *n = 0;
    *k = 0;
    *whole = 1;
    if (!a->buf_pages) {
        *n = pipe_available(a);
        if (*n > room) {
            *n = room;
        }
        if (*n > slots * PAGE_SIZE) {
            *n = slots * PAGE_SIZE;
        }
        if (*n > len) {
            *n = len;
        }
        return;
    }
    for (i = 0; i < a->bufs && i < slots && *n < len; i++) {
        uint32_t end = pipe_buf_end(a, i) - a->head, take = end - *n;

        if (take > len - *n) {
            take = len - *n;
        }
        if (take > room - *n) {
            take = room - *n;
        }
        if (take == 0 && end > *n) {
            break;
        }
        *whole = *n + take == end;
        *n += take;
        *k = i + 1;
        if (!*whole) {
            break;
        }
    }
}

/**
 * A tee links the buffers of `a` into `b` once and returns, as Linux's
 * `link_pipe`, 0 when it linked only empty buffers. A splice, `move`, takes
 * the buffers it links from `a`, the last only in part when `len` or `b`
 * cuts it; one that moved only empty buffers goes on, as Linux's
 * `splice_pipe_to_pipe` retries, since 0 would read as the end of input:
 * it waits for more, or returns -EAGAIN with `nonblock`, or 0 once `a`
 * holds nothing and has no writer.
 */
int32_t pipe_tee(fd_entry_t *in, fd_entry_t *out, uint32_t len, int nonblock, int move)
{
    pipe_t *a = (pipe_t *)in->priv;
    pipe_t *b = (pipe_t *)out->priv;
    uint32_t n, k, i;
    int32_t rc;
    int whole;

    if (a == b) {
        return -EINVAL;
    }
    pipe_lock(a, b);
    for (;;) {
        rc = pipe_link_ready(a, b, nonblock);
        if (rc <= 0) {
            break;
        }
        if (pipe_ring_ready(b) < 0) {
            rc = -ENOMEM;
            break;
        }
        pipe_link_plan(a, b, len, &n, &k, &whole);
        pipe_ring_copy(a, NULL, b, n);
        pipe_buf_link(a, b, k, n, move, whole);
        pipe_ring_produce(b, n);
        if (n > 0) {
            pipe_note_writer(b);
            pipe_wake(b, &b->read_wq, WQ_DATA);
        }
        if (move) {
            pipe_ring_consume(a, n);
            for (i = 0; i < (whole ? k : k - 1); i++) {
                pipe_buf_shift(a);
            }
            pipe_wake(a, &a->write_wq, WQ_ROOM);
        }
        rc = (int32_t)n;
        if (n > 0 || !move) {
            break;
        }
        if (!pipe_holds(a) && !a->write_open) {
            break;
        }
        if (nonblock) {
            rc = -EAGAIN;
            break;
        }
    }
    pipe_unlock(a, b);
    if (rc == -EPIPE) {
        pipe_sigpipe();
    }
    return rc;
}

/** Make the caller's reserved descriptor `fd` an end of `p` with the access
 * `access`, close-on-exec and O_NONBLOCK as pipe2's `flags` ask. */
static void pipe_setup_end(int fd, pipe_t *p, uint32_t access, int flags)
{
    fd_entry_t *e = fd_slot(current_proc->fds, fd);

    e->type = FD_TYPE_PIPE;
    e->node = NULL;
    e->sock = NULL;
    e->ops = &pipe_ops;
    e->priv = p;
    e->flags = access;
    if (flags & O_CLOEXEC_FLAG) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    if ((flags & O_NONBLOCK_FLAG) && e->ofd) {
        e->ofd->status_flags |= OFD_NONBLOCK;
    }
    fd_install(current_proc->fds, fd);
}

/* An open of a FIFO waiting for the other end: the pipe, whether it waits
 * for a reader, as a writer's open does, or for a writer, and that end's
 * count of opens when the wait began. */
struct fifo_wait {
    const pipe_t *p;
    int for_reader;
    uint32_t seen;
};

/** The wait condition of an open of a FIFO, `arg` its `struct fifo_wait`:
 * the end it waits for has been opened since the wait began. */
static int fifo_partner_came(void *arg)
{
    const struct fifo_wait *w = (const struct fifo_wait *)arg;

    return (w->for_reader ? w->p->read_opens : w->p->write_opens) != w->seen;
}

/** The pipe of the FIFO file that `mnt` and `ino` identify, or NULL when no
 * end of it is open. Called under `pipes_lock`. */
static pipe_t *fifo_find(const void *mnt, uint64_t ino)
{
    pipe_t *p;

    for (p = pipes; p && !(p->fifo && p->fifo_mnt == mnt && p->fifo_ino == ino); p = p->next) {
    }
    return p;
}

/**
 * The pipe of the FIFO file that `mnt` and `ino` identify, with a count in
 * its `files` for the caller's open, made when no end of it is open; NULL
 * when the heap has no room or the caller's user is at
 * `pipe_user_pages_hard`. The look and the count are one hold of
 * `pipes_lock`, and a pipe made while the lock was let go is dropped for
 * one another open published meanwhile, as Linux's `fifo_open` finds or
 * makes `inode->i_pipe` under `i_lock` (fs/pipe.c:1117-1135 in 6.12), so
 * every open of a file reaches one pipe.
 */
static pipe_t *fifo_get(const void *mnt, uint64_t ino)
{
    uint32_t irq = spin_lock_irqsave(&pipes_lock);
    pipe_t *p = fifo_find(mnt, ino), *made;

    if (p) {
        p->files++;
        spin_unlock_irqrestore(&pipes_lock, irq);
        return p;
    }
    spin_unlock_irqrestore(&pipes_lock, irq);
    made = pipe_alloc();
    if (!made) {
        return NULL;
    }
    made->read_open = 0;
    made->write_open = 0;
    made->files = 1;
    made->fifo = 1;
    made->fifo_mnt = mnt;
    made->fifo_ino = ino;
    irq = spin_lock_irqsave(&pipes_lock);
    p = fifo_find(mnt, ino);
    if (p) {
        p->files++;
    } else if (pipe_publish(made) == 0) {
        p = made;
        made = NULL;
    }
    spin_unlock_irqrestore(&pipes_lock, irq);
    if (made) {
        pipe_free(made);
    }
    return p;
}

/** Take back the ends counted by an open of `p` that did not finish,
 * `reads` and `writes` of them, under the pipe's lock, which the caller
 * holds and which is given back here, and let go of the open's count in
 * `files`, which frees the pipe when it was the last. */
static void fifo_unopen(pipe_t *p, int reads, int writes)
{
    p->read_open -= reads;
    p->write_open -= writes;
    pipe_unlock(p, NULL);
    pipe_info_put(p);
}

/**
 * A blocking open waits for the other end's count of opens to change, not
 * for an end of that kind to be open, so an open of the other end that
 * closes again while this one sleeps still ends the wait, as Linux's
 * counters have it. The counts change under the pipe's lock, which the
 * wait gives back, as Linux's `fifo_open` holds `pipe->mutex` and lets it
 * go in `wait_for_partner` (fs/pipe.c:1081-1100 and 1140 in 6.12); the
 * open's count in `files` keeps the pipe while it waits, and becomes its
 * description's.
 */
int32_t fifo_attach(fd_entry_t *e, const void *mnt, uint64_t ino, int flags)
{
    int acc = flags & 3;
    int nonblock = (flags & O_NONBLOCK_FLAG) != 0;
    int reads = acc != 1;
    int writes = acc != 0;
    pipe_t *p;

    if (acc == 3) {
        return -EINVAL;
    }
    p = fifo_get(mnt, ino);
    if (!p) {
        return -ENOMEM;
    }
    pipe_lock(p, NULL);
    p->read_open += reads;
    p->write_open += writes;
    if (acc == 1 && nonblock && p->read_open == 0) {
        fifo_unopen(p, reads, writes);
        return -ENXIO;
    }
    p->read_opens += (uint32_t)reads;
    p->write_opens += (uint32_t)writes;
    wq_wake_all(&p->open_wq);
    if (!nonblock && acc != 2 && (acc == 0 ? !p->write_open : !p->read_open)) {
        struct fifo_wait w;

        w.p = p;
        w.for_reader = acc == 1;
        w.seen = w.for_reader ? p->read_opens : p->write_opens;
        if (pipe_sleep(p, NULL, &p->open_wq, fifo_partner_came, &w, "wait_for_partner") < 0) {
            fifo_unopen(p, reads, writes);
            return -EINTR;
        }
    }
    if (acc == 2) {
        p->rdwr_open++;
    }
    e->type = FD_TYPE_PIPE;
    e->node = NULL;
    e->sock = NULL;
    e->ops = &pipe_ops;
    e->priv = p;
    e->flags = (reads ? FD_FLAG_READ : 0u) | (writes ? FD_FLAG_WRITE : 0u);
    if (flags & O_CLOEXEC_FLAG) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    if (e->ofd) {
        e->ofd->status_flags |= OFD_OPEN_STATUS(flags);
    }
    e->ofd->fifo_write_opens = p->write_opens;
    if (acc != 0 || !nonblock || p->write_open) {
        e->ofd->fifo_write_opens--;
    }
    pipe_unlock(p, NULL);
    return 0;
}

/**
 * pipe2(2) of the user array at `fds_ptr` with `flags`: a new pipe, the
 * read end's descriptor stored first and the write end's second, each with
 * O_CLOEXEC and O_NONBLOCK as `flags` ask. Returns 0, or -ESRCH with no
 * current process, -EINVAL for any other flag, -ENFILE when the heap has no
 * room for a pipe or the caller's user is at `pipe_user_pages_hard`, as on
 * Linux, -EMFILE or -ENOMEM from the descriptor table, or -EFAULT for a bad
 * array.
 *
 * Both descriptors are reserved, their numbers copied to the caller, and
 * only then given their ends, as Linux's do_pipe2 does: the copy can sleep,
 * and a reserved slot has no type, so a thread sharing the table can neither
 * use nor duplicate an end before the call succeeds, and a copy that fails
 * frees the slots and the pipe, which nothing else can reach.
 */
static int32_t pipe_open(uintptr_t fds_ptr, int flags)
{
    pipe_t *pipe;
    int kfds[2];

    if (!current_proc) {
        return -ESRCH;
    }
    if (flags & ~(O_CLOEXEC_FLAG | O_NONBLOCK_FLAG)) {
        return -EINVAL;
    }

    pipe = pipe_alloc();
    if (pipe) {
        uint32_t irq = spin_lock_irqsave(&pipes_lock);
        int over = pipe_publish(pipe) < 0;

        spin_unlock_irqrestore(&pipes_lock, irq);
        if (over) {
            pipe_free(pipe);
            pipe = NULL;
        }
    }
    if (!pipe) {
        return -ENFILE;
    }

    kfds[0] = fd_alloc(current_proc->fds);
    kfds[1] = kfds[0] >= 0 ? fd_alloc(current_proc->fds) : kfds[0];
    if (kfds[1] < 0 || copy_to_user((void __user *)fds_ptr, kfds, sizeof(kfds)) != 0) {
        if (kfds[1] >= 0) {
            fd_free(current_proc->fds, kfds[1]);
        }
        if (kfds[0] >= 0) {
            fd_free(current_proc->fds, kfds[0]);
        }
        pipe_info_put(pipe);
        pipe_info_put(pipe);
        return kfds[1] < 0 ? kfds[1] : -EFAULT;
    }
    pipe_setup_end(kfds[0], pipe, FD_FLAG_READ, flags);
    pipe_setup_end(kfds[1], pipe, FD_FLAG_WRITE, flags);
    return 0;
}

/** pipe2(2) takes the array and the flags (`pipe_open`). */
long sys_pipe2(struct isr_params *params)
{
    return pipe_open(SC_ARG(params, 0), (int)SC_ARG(params, 1));
}

/** pipe(2) takes the array alone and is pipe2 with no flags. The register
 * of pipe2's second argument is not changed to pass them: it goes back to
 * the program as it came, as every register but the result's does. */
long sys_pipe(struct isr_params *params)
{
    return pipe_open(SC_ARG(params, 0), 0);
}
