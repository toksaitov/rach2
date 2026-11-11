/*
 * pipe.c: anonymous pipes and FIFOs, as pipe.h describes them: the
 * operations of a pipe end, pipe2(2), and the open of a FIFO file.
 */

#include "pipe.h"
#include "../irq.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../proc/process.h"
#include "../mm/usercopy.h"
#include "../mm/paging.h"
#include "../mm/kheap.h"
#include "../lib/kstring.h"
#include "../lib/kerrno.h"
#include "../lib/ksignal.h"

/* Linux ioctls a pipe answers. */
#define FIONREAD 0x541B /* bytes available to read */

/* The flags pipe2 takes, Linux's O_CLOEXEC and O_NONBLOCK. */
#define O_CLOEXEC_FLAG  02000000
#define O_NONBLOCK_FLAG 04000

/* The live pipes, each allocated from the heap when made and freed when
 * both ends are closed. */
static pipe_t *pipes;

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
 * pipe has.
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
 * is allocated yet.
 */
static uint32_t pipe_user_pages(uint32_t user)
{
    const pipe_t *p;
    uint32_t pages = 0;

    for (p = pipes; p; p = p->next) {
        if (p->user == user) {
            pages += p->size / PAGE_SIZE;
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
 * A new pipe on the live list, with one end of each kind counted open and
 * no ring yet; NULL when the heap has no room or, for an unprivileged
 * caller, when its user's pipes would pass `pipe_user_pages_hard`. It is
 * charged to the caller's real user, and its size is Linux's
 * `alloc_pipe_info`'s: PIPE_BUF_SIZE, but for an unprivileged caller at
 * most `pipe_max_size`, and PIPE_MIN_PAGES once the user's pipes would pass
 * `pipe_user_pages_soft`.
 */
static pipe_t *pipe_alloc(void)
{
    uint32_t user = current_proc ? current_proc->ruid : 0;
    int privileged = !current_proc || proc_privileged(current_proc);
    uint32_t size = PIPE_BUF_SIZE, held;
    pipe_t *p;

    if (!privileged) {
        held = pipe_user_pages(user);
        if (size > pipe_max_size) {
            size = pipe_max_size;
        }
        if (pipe_pages_over((unsigned long)held + size / PAGE_SIZE, pipe_user_pages_soft)) {
            size = PIPE_MIN_PAGES * PAGE_SIZE;
        }
        if (pipe_pages_over((unsigned long)held + size / PAGE_SIZE, pipe_user_pages_hard)) {
            return NULL;
        }
    }
    p = kzalloc(sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->in_use = 1;
    p->size = size;
    p->user = user;
    p->inode = pipe_next_inode();
    p->read_open = 1;
    p->write_open = 1;
    wait_queue_init(&p->read_wq);
    wait_queue_init(&p->write_wq);
    wait_queue_init(&p->rdwr_wq);
    wait_queue_init(&p->splice_wq);
    p->next = pipes;
    pipes = p;
    return p;
}

/** Take `p` off the live list and free it with its ring, once no end of it
 * is open or before any descriptor has it. */
static void pipe_free(pipe_t *p)
{
    pipe_t **link = &pipes;

    while (*link && *link != p) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = p->next;
    }
    kfree(p->buf);
    kfree(p->buf_pages);
    kfree(p);
}

/** The wait condition of a read of the pipe `arg`: bytes are buffered, or
 * no writer is left, when the read returns 0. */
static int pipe_readable(void *arg)
{
    pipe_t *p = (pipe_t *)arg;
    return pipe_available(p) != 0 || !p->write_open;
}

/** The wait condition of a write to the pipe `arg`: the ring has room, or
 * no reader is left, when the write fails with EPIPE. */
static int pipe_writable(void *arg)
{
    pipe_t *p = (pipe_t *)arg;
    return pipe_free_space(p) != 0 || !p->read_open;
}

/** The wait condition of a reader of the pipe `arg` that a splice's hold
 * stopped: the hold has ended. */
static int pipe_unspliced(void *arg)
{
    return !((pipe_t *)arg)->spliced;
}

/** Wait until no splice holds the bytes of `p` (`pipe_splice_peek`): 0, or
 * -EINTR when a signal ends the wait. A reader waits even with O_NONBLOCK,
 * as Linux's waits for the pipe's lock, since the hold ends without any
 * other process acting. */
static int32_t pipe_wait_unspliced(pipe_t *p)
{
    while (p->spliced) {
        if (wq_wait_event(&p->splice_wq, pipe_unspliced, p) < 0) {
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

/** Wake the waiters on one end's queue `wq` for a change of the kinds
 * `kinds` (`wq_wake`), and the pollers of a FIFO's descriptions open on both
 * ends, whose readiness any change can move. */
static void pipe_wake(pipe_t *p, wait_queue_t *wq, uint32_t kinds)
{
    wq_wake(wq, kinds);
    if (p->rdwr_open) {
        wq_wake(&p->rdwr_wq, kinds);
    }
}

/*
 * Linux keeps a pipe's bytes in page buffers, one to a slot of its ring, and
 * refuses to shrink a pipe below the buffers that hold bytes, however few
 * bytes they hold. The functions below keep the same buffers over the byte
 * ring, `buf_pages` in pipe.h, so that `pipe_set_size` can apply the rule: a
 * write opens a page when the last buffer cannot take its bytes, vmsplice and
 * splice from a file open one for each page of their source, and a buffer
 * leaves once read whole.
 */

/** The slots of `buf_pages` for a ring of `size` bytes: twice its pages and
 * two, as many as writes can open, since of two neighbouring buffers past
 * the first that writes opened the second opened only because the first's
 * page had no room for its bytes. Tee, splice and vmsplice can open more,
 * which `pipe_buf_open` folds into the newest. */
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

/** Let go of the buffers of `p` that its reads have emptied; called after
 * `head` moves. */
static void pipe_buf_drop(pipe_t *p)
{
    uint32_t avail = pipe_available(p);

    if (avail == 0) {
        p->bufs = 0;
        p->buf_first = 0;
        return;
    }
    while (p->bufs > 1 && p->tail - pipe_buf_at(p, 1) >= avail) {
        p->buf_first = (p->buf_first + 1) % p->buf_cap;
        p->bufs--;
    }
}

/**
 * The bytes of a write of `count` that the newest buffer of `p` takes, as
 * Linux's `pipe_write` merges them: the write's first `count` mod 4096
 * bytes, when the pipe holds bytes, the newest buffer takes merges and they
 * fit in its page after its offset and the bytes written to it.
 */
static uint32_t pipe_buf_merge_room(const pipe_t *p, uint32_t count)
{
    uint32_t chars = count & (PAGE_SIZE - 1);
    const struct pipe_page *pg;

    if (chars == 0 || p->bufs == 0 || pipe_available(p) == 0) {
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
 * after another writer's.
 */
static void pipe_buf_add_pages(pipe_t *p, uint32_t n, uint32_t src, int fresh)
{
    uint32_t at = p->tail;

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
 * Open in `b` a buffer for each buffer of `a` that the first `n` bytes
 * buffered in `a` reach, at the same offset in its page, as Linux's tee and
 * splice between pipes link each buffer, or a part of it, into the other
 * pipe; called before `b`'s tail moves past the `n` bytes. A buffer that a
 * splice, `move`, carries whole keeps taking later writes' bytes if it did,
 * as Linux's `splice_pipe_to_pipe` moves it with its flags; a part of one,
 * and every buffer tee links, takes none.
 */
static void pipe_buf_link(const pipe_t *a, pipe_t *b, uint32_t n, int move)
{
    uint32_t i, from = 0;

    if (!a->buf_pages) {
        pipe_buf_add_pages(b, n, 0, 1);
        return;
    }
    for (i = 0; i < a->bufs && from < n; i++) {
        const struct pipe_page *pg = pipe_buf_page(a, i);
        uint32_t read = i == 0 ? a->head - pg->start : 0;
        uint32_t end = i + 1 < a->bufs ? pipe_buf_at(a, i + 1) - a->head : pipe_available(a);

        pipe_buf_open(b, b->tail + from, pg->offset + read, move && end <= n && pg->merge);
        from = end;
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

/**
 * The read of a pipe end: up to `count` bytes, and at most those buffered
 * when the first transfer begins, waiting only before it. Returns the bytes
 * read; 0 for an empty pipe with no writer left; or, before a transfer,
 * -EBADF for a descriptor not open for reading, -EFAULT for a bad buffer,
 * -EAGAIN for an empty pipe with O_NONBLOCK, or -EINTR when a signal ends
 * the wait. A fault after a transfer returns the count read, and the bytes
 * meant for the page that faulted stay buffered.
 *
 * Each transfer makes one page of the buffer present first, which can
 * sleep, and only then takes its span of the ring, so the ring is judged
 * after the sleep; copying the span and advancing the counter have no sleep
 * between them. The checked copy repeats the prefault, which finds the page
 * present and allocates nothing: nothing between the two sleeps, and an
 * interrupt does not preempt a system call, so no other thread can unmap
 * the page meanwhile.
 */
static int32_t pipe_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    pipe_t *p = (pipe_t *)e->priv;
    uint32_t done = 0;
    uint32_t to_read = count;

    if (!(e->flags & FD_FLAG_READ)) {
        return -EBADF;
    }
    if (count == 0) {
        return 0;
    }
    if (!user_range_ok((__force uintptr_t)buf, count)) {
        return -EFAULT;
    }

    while (done < to_read) {
        const uint8_t *at;
        uintptr_t addr = (__force uintptr_t)(buf + done);
        uint32_t chunk;

        if (pipe_available(p) == 0) {
            if (done > 0 || !p->write_open) {
                break;
            }
            if (fd_is_nonblock(e)) {
                return -EAGAIN;
            }
            if (wq_wait_event_as(&p->read_wq, pipe_readable, p, "pipe_read") < 0) {
                return -EINTR;
            }
            continue;
        }

        if (!vm_prefault_range(addr, page_piece(addr, to_read - done), 1)) {
            return (done > 0) ? (int32_t)done : -EFAULT;
        }
        if (p->spliced) {
            if (done > 0) {
                break;
            }
            if (pipe_wait_unspliced(p) < 0) {
                return -EINTR;
            }
            continue;
        }
        chunk = pipe_ring_read_span(p, &at);
        if (chunk == 0) {
            continue;
        }
        if (done == 0 && to_read > pipe_available(p)) {
            to_read = pipe_available(p);
        }

        if (chunk > to_read - done) {
            chunk = to_read - done;
        }
        chunk = page_piece(addr, chunk);
        if (copy_to_user((void __user *)(buf + done), at, chunk) != 0) {
            return (done > 0) ? (int32_t)done : -EFAULT;
        }
        pipe_ring_consume(p, chunk);
        pipe_buf_drop(p);
        done += chunk;
        pipe_wake(p, &p->write_wq, WQ_ROOM);
    }

    return (int32_t)done;
}

/** Linux's SIGPIPE for a write to a pipe no reader is left on: SI_USER,
 * from the writer, to its thread. */
static void pipe_sigpipe(void)
{
    struct k_siginfo info;

    k_memset(&info, 0, sizeof(info));
    info.code = SI_USER;
    info.pid = current_proc->tgid;
    info.uid = current_proc->ruid;
    signal_post_thread(current_proc, SIGPIPE, &info);
}

/**
 * The write of a pipe end: all `count` bytes, waiting whenever the ring is
 * full unless `nonblock`. Returns the bytes written, or, before a transfer,
 * -EBADF for a descriptor not open for writing, -EFAULT for a bad buffer,
 * -EAGAIN for a full pipe with `nonblock`, -EINTR when a signal ends a wait,
 * or -ENOMEM when the heap has no room for the ring; each of these after a
 * transfer returns the count written instead. With no reader left the write
 * raises SIGPIPE in the calling thread, as Linux sends it, and returns
 * -EPIPE or the count written. Readers are woken for data at every
 * transfer, the ring empty before it or not, so an edge-triggered epoll
 * registration of the read end is told of each write, as Linux tells one of
 * a pipe.
 *
 * Each transfer makes its page present as `pipe_read`'s does, and judges
 * the readers and the ring again after that sleep.
 *
 * A write, `merge`, fills Linux's page buffers as `pipe_buf_add` does, and
 * otherwise the bytes are a splice's, which take a buffer for each page of
 * the source they come from, whose first byte lies `src` bytes into its page
 * (`pipe_buf_add_pages`).
 */
static int32_t pipe_put(fd_entry_t *e, const char __user *buf, uint32_t count, int nonblock,
                        int merge, uint32_t src)
{
    pipe_t *p = (pipe_t *)e->priv;
    uint32_t done = 0, fill = merge ? pipe_buf_merge_room(p, count) : 0, last = p->tail;
    int fresh = 1;

    if (!(e->flags & FD_FLAG_WRITE)) {
        return -EBADF;
    }
    if (count == 0) {
        return 0;
    }
    if (!user_range_ok((__force uintptr_t)buf, count)) {
        return -EFAULT;
    }

    while (done < count) {
        uint32_t space;
        uint32_t chunk;
        uintptr_t addr = (__force uintptr_t)(buf + done);

        if (!p->read_open) {
            pipe_sigpipe();
            return (done > 0) ? (int32_t)done : -EPIPE;
        }

        space = pipe_free_space(p);
        if (space == 0) {
            if (nonblock) {
                return (done > 0) ? (int32_t)done : -EAGAIN;
            }
            if (wq_wait_event_as(&p->write_wq, pipe_writable, p, "pipe_write") < 0) {
                return (done > 0) ? (int32_t)done : -EINTR;
            }
            continue;
        }

        if (!vm_prefault_range(addr, page_piece(addr, count - done), 0)) {
            return (done > 0) ? (int32_t)done : -EFAULT;
        }
        if (!p->read_open) {
            continue;
        }
        if (pipe_ring_ready(p) < 0) {
            return (done > 0) ? (int32_t)done : -ENOMEM;
        }
        if (p->tail != last) {
            fill = 0;
            fresh = 1;
        }
        {
            uint8_t *at;

            chunk = pipe_ring_write_span(p, &at);
            if (chunk == 0) {
                continue;
            }
            if (chunk > count - done) {
                chunk = count - done;
            }
            chunk = page_piece(addr, chunk);
            if (copy_from_user(at, (const void __user *)(buf + done), chunk) != 0) {
                return (done > 0) ? (int32_t)done : -EFAULT;
            }
            if (merge) {
                pipe_buf_add(p, chunk, &fill);
            } else {
                pipe_buf_add_pages(p, chunk, src + done, fresh);
            }
            fresh = 0;
            pipe_ring_produce(p, chunk);
            last = p->tail;
            done += chunk;
            pipe_wake(p, &p->read_wq, WQ_DATA);
        }
    }

    return (int32_t)done;
}

/** The write of a pipe end (`pipe_put`), waiting for room unless the
 * description has O_NONBLOCK. */
static int32_t pipe_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    return pipe_put(e, buf, count, fd_is_nonblock(e), 1, 0);
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
 * is POLLIN while data is buffered and POLLHUP once the writers are gone, so
 * an empty pipe with no writers, whose read returns 0, reports POLLHUP
 * alone, as on Linux. A FIFO reader opened nonblocking with no writer
 * reports no hangup until a writer has opened since that read open.
 * The write end is writable with room or the readers
 * gone, and POLLERR once the readers are gone (the write then fails with
 * EPIPE). POLLHUP and POLLERR are reported whatever `events` asks.
 */
static short pipe_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    pipe_t *p = (pipe_t *)e->priv;
    short revents = 0;

    if (wq) {
        if ((e->flags & FD_FLAG_READ) && (e->flags & FD_FLAG_WRITE)) {
            *wq = &p->rdwr_wq;
        } else {
            *wq = (e->flags & FD_FLAG_READ) ? &p->read_wq : &p->write_wq;
        }
    }

    if (e->flags & FD_FLAG_READ) {
        if (pipe_available(p) != 0) {
            revents |= (short)(events & PIPE_POLLIN);
        }
        if (!p->write_open && (!p->fifo || e->ofd->fifo_write_opens != p->write_opens)) {
            revents |= PIPE_POLLHUP;
        }
    }
    if (e->flags & FD_FLAG_WRITE) {
        if (pipe_free_space(p) != 0 || !p->read_open) {
            revents |= (short)(events & PIPE_POLLOUT);
        }
        if (!p->read_open) {
            revents |= PIPE_POLLERR;
        }
    }
    return revents;
}

/** The ioctls of a pipe end: FIONREAD stores the bytes buffered, an int, at
 * `arg`, or fails with -EFAULT; any other request is -ENOTTY. */
static int32_t pipe_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    pipe_t *p = (pipe_t *)e->priv;

    switch (request) {
    case FIONREAD: {
        int n = (int)pipe_available(p);
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
 * left, and free the pipe once no end is open. A FIFO opened for reading and
 * writing is an end of both kinds.
 */
static void pipe_close(fd_entry_t *e)
{
    pipe_t *p = (pipe_t *)e->priv;

    if (!p) {
        return;
    }
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
    if (!p->read_open && !p->write_open) {
        pipe_free(p);
    }
}

static const struct file_ops pipe_ops = {pipe_read,  pipe_write, pipe_poll, pipe_ioctl,
                                         pipe_close, NULL,       NULL};

int pipe_is(const fd_entry_t *e)
{
    return e->ops == &pipe_ops;
}

int32_t pipe_splice_write(fd_entry_t *e, const char __user *buf, uint32_t count, int nonblock,
                          uint32_t src)
{
    return pipe_put(e, buf, count, nonblock, 0, src);
}

int32_t pipe_splice_room(fd_entry_t *e, int nonblock)
{
    pipe_t *p = (pipe_t *)e->priv;

    if (!(e->flags & FD_FLAG_WRITE)) {
        return -EBADF;
    }
    for (;;) {
        if (!p->read_open) {
            pipe_sigpipe();
            return -EPIPE;
        }
        if (pipe_free_space(p) != 0) {
            return (int32_t)pipe_free_space(p);
        }
        if (nonblock) {
            return -EAGAIN;
        }
        if (wq_wait_event(&p->write_wq, pipe_writable, p) < 0) {
            return -EINTR;
        }
    }
}

/**
 * Wait until the pipe of the read end `e` has bytes, or has no writer left:
 * the bytes buffered, 0 for none and no writer, -EAGAIN for none with
 * `nonblock`, or -EINTR when a signal ends the wait.
 */
static int32_t pipe_wait_data(fd_entry_t *e, int nonblock)
{
    pipe_t *p = (pipe_t *)e->priv;

    while (pipe_available(p) == 0) {
        if (!p->write_open) {
            return 0;
        }
        if (nonblock) {
            return -EAGAIN;
        }
        if (wq_wait_event(&p->read_wq, pipe_readable, p) < 0) {
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
 * A new ring is allocated, whether or not the pipe had one, as Linux's
 * `pipe_resize_ring` allocates its slots, so that a size the heap cannot
 * give fails here and not at every write; the bytes buffered are copied to
 * the slots their counters name in it, so `head` and `tail` keep counting
 * as they did. A growth wakes the writers, which may now have room. The
 * limits are Linux's `pipe_set_size`'s, and a shrink passes them even for a
 * user over the soft limit. A shrink below the page buffers that hold bytes
 * fails with EBUSY, as in Linux, however few bytes they hold: a write opens
 * a page unless its first `count` mod 4096 bytes fit in the newest buffer's
 * (`pipe_buf_merge_room`), and tee, splice and vmsplice open one for each
 * buffer or source page their bytes come from (`pipe_buf_link`,
 * `pipe_buf_add_pages`).
 */
int32_t pipe_set_size(fd_entry_t *e, uint32_t size, int privileged)
{
    pipe_t *p = (pipe_t *)e->priv;
    uint32_t want = pipe_round_size(size), done = 0, n = pipe_available(p), i;
    struct pipe_page *slots;
    uint8_t *ring;

    if (want == 0) {
        return -EINVAL;
    }
    if (want > p->size && !privileged) {
        unsigned long pages =
            (unsigned long)pipe_user_pages(p->user) - p->size / PAGE_SIZE + want / PAGE_SIZE;

        if (want > pipe_max_size || pipe_pages_over(pages, pipe_user_pages_soft) ||
            pipe_pages_over(pages, pipe_user_pages_hard)) {
            return -EPERM;
        }
    }
    if (want == p->size) {
        return (int32_t)want;
    }
    if (pipe_ring_new(want, &ring, &slots) < 0) {
        return -ENOMEM;
    }
    if (n > want || p->bufs > want / PAGE_SIZE) {
        kfree(ring);
        kfree(slots);
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
    if (want > p->size) {
        pipe_wake(p, &p->write_wq, WQ_ROOM);
    }
    p->size = want;
    return (int32_t)want;
}

int32_t pipe_splice_peek(fd_entry_t *e, uint8_t *dst, uint32_t n, int nonblock)
{
    pipe_t *p = (pipe_t *)e->priv;
    int32_t avail;

    if (!(e->flags & FD_FLAG_READ)) {
        return -EBADF;
    }
    for (;;) {
        avail = pipe_wait_data(e, nonblock);
        if (avail <= 0) {
            return avail;
        }
        if (!p->spliced) {
            break;
        }
        if (pipe_wait_unspliced(p) < 0) {
            return -EINTR;
        }
    }
    if (n > (uint32_t)avail) {
        n = (uint32_t)avail;
    }
    pipe_ring_copy(p, dst, NULL, n);
    p->spliced = 1;
    return (int32_t)n;
}

void pipe_splice_consume(fd_entry_t *e, uint32_t n)
{
    pipe_t *p = (pipe_t *)e->priv;

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
}

int32_t pipe_tee(fd_entry_t *in, fd_entry_t *out, uint32_t len, int nonblock, int move)
{
    pipe_t *a = (pipe_t *)in->priv;
    pipe_t *b = (pipe_t *)out->priv;
    uint32_t n;

    if (a == b) {
        return -EINVAL;
    }
    for (;;) {
        int32_t avail = pipe_wait_data(in, nonblock);

        if (avail <= 0) {
            return avail;
        }
        if (a->spliced) {
            if (pipe_wait_unspliced(a) < 0) {
                return -EINTR;
            }
            continue;
        }
        if (!b->read_open) {
            pipe_sigpipe();
            return -EPIPE;
        }
        if (pipe_free_space(b) != 0) {
            break;
        }
        if (nonblock) {
            return -EAGAIN;
        }
        if (wq_wait_event(&b->write_wq, pipe_writable, b) < 0) {
            return -EINTR;
        }
    }
    if (pipe_ring_ready(b) < 0) {
        return -ENOMEM;
    }
    n = pipe_available(a);
    if (n > pipe_free_space(b)) {
        n = pipe_free_space(b);
    }
    if (n > len) {
        n = len;
    }
    pipe_ring_copy(a, NULL, b, n);
    pipe_buf_link(a, b, n, move);
    pipe_ring_produce(b, n);
    pipe_wake(b, &b->read_wq, WQ_DATA);
    if (move) {
        pipe_ring_consume(a, n);
        pipe_buf_drop(a);
        pipe_wake(a, &a->write_wq, WQ_ROOM);
    }
    return (int32_t)n;
}

/** Make the caller's reserved descriptor `fd` an end of `p` with the access
 * `access`, close-on-exec and O_NONBLOCK as pipe2's `flags` ask. */
static void pipe_setup_end(int fd, pipe_t *p, uint32_t access, int flags)
{
    fd_entry_t *e = &current_proc->fds->entries[fd];

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
 * end of it is open. */
static pipe_t *fifo_find(const void *mnt, uint64_t ino)
{
    pipe_t *p;

    for (p = pipes; p && !(p->fifo && p->fifo_mnt == mnt && p->fifo_ino == ino); p = p->next) {
    }
    return p;
}

/** Take back the ends counted by an open that did not finish, `reads` and
 * `writes` of them, and free the pipe when no end is left. */
static void fifo_unopen(pipe_t *p, int reads, int writes)
{
    p->read_open -= reads;
    p->write_open -= writes;
    if (!p->read_open && !p->write_open) {
        pipe_free(p);
    }
}

/**
 * A blocking open waits for the other end's count of opens to change, not
 * for an end of that kind to be open, so an open of the other end that
 * closes again while this one sleeps still ends the wait, as Linux's
 * counters have it.
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
    p = fifo_find(mnt, ino);
    if (!p) {
        p = pipe_alloc();
        if (!p) {
            return -ENOMEM;
        }
        p->read_open = 0;
        p->write_open = 0;
        p->fifo = 1;
        p->fifo_mnt = mnt;
        p->fifo_ino = ino;
        wait_queue_init(&p->open_wq);
    }
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
        if (wq_wait_event_as(&p->open_wq, fifo_partner_came, &w, "wait_for_partner") < 0) {
            fifo_unopen(p, reads, writes);
            return -EINTR;
        }
        e = fd_entry_now(current_proc->fds, e); /* another sharer may have grown the table */
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
    if (nonblock && e->ofd) {
        e->ofd->status_flags |= OFD_NONBLOCK;
    }
    e->ofd->fifo_write_opens = p->write_opens;
    if (acc != 0 || !nonblock || p->write_open) {
        e->ofd->fifo_write_opens--;
    }
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
        pipe_free(pipe);
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
