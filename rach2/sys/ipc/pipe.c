/**
 * Anonymous pipes.
 *
 * A pipe is a bounded byte FIFO with a read end and a write end. Each end is a
 * descriptor whose ops are pipe_ops and whose priv is the struct pipe;
 * read/write block on the pipe's wait queues (so other processes run while a
 * stage waits); dup/dup2/fork share an end through the refcounted open-file
 * description, and pipe_close runs (from fd_release_backing) only when an
 * end's LAST descriptor closes. See pipe.h for the EOF/SIGPIPE contract.
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

/* The O_* flags pipe2 takes (the values in syscall.c and fd.h). */
#define O_CLOEXEC_FLAG  02000000
#define O_NONBLOCK_FLAG 04000

/* The live pipes, each allocated from the heap when made and freed when
 * both ends are closed. */
static pipe_t *pipes;

/* The pure ring helpers (pipe_available/pipe_free_space/pipe_ring_put/
 * pipe_ring_get and the spans) live in pipe_ring.c so the host test can
 * exercise the wrap arithmetic natively under ASan without pulling in the
 * syscall glue. */

void pipe_init_pool(void)
{
    pipes = NULL;
}

static uint32_t next_pipe_inode = 1;

/**
 * The next inode to give a pipe: rising, nonzero, and not one a live pipe has.
 *
 * Two pipes must not be one thing to a program reading `/proc/<pid>/fd`, and a
 * new pipe must not read as one that has closed, so the number rises. Rising
 * is not enough by itself: the counter is 32 bits and wraps, so the list of
 * live pipes is checked and a number in use is stepped over. Zero is skipped
 * because it is what an unset field reads as.
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

static pipe_t *pipe_alloc(void)
{
    pipe_t *p = kzalloc(sizeof(*p));

    if (!p) {
        return NULL;
    }
    p->in_use = 1;
    p->inode = pipe_next_inode();
    p->read_open = 1;
    p->write_open = 1;
    wait_queue_init(&p->read_wq);
    wait_queue_init(&p->write_wq);
    wait_queue_init(&p->rdwr_wq);
    p->next = pipes;
    pipes = p;
    return p;
}

/* Both ends closed, or the pipe never handed out: off the list and back to
 * the heap. Nothing refers to it after this. */
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
    kfree(p);
}

/**
 * Wait predicate: a reader can stop blocking; data is available, or every
 * writer has gone (EOF).
 */
static int pipe_readable(void *arg)
{
    pipe_t *p = (pipe_t *)arg;
    return pipe_available(p) != 0 || !p->write_open;
}

/**
 * Wait predicate: a writer can stop blocking; space is available, or every
 * reader has gone (the write will fail with EPIPE).
 */
static int pipe_writable(void *arg)
{
    pipe_t *p = (pipe_t *)arg;
    return pipe_free_space(p) != 0 || !p->read_open;
}

static int fd_is_nonblock(fd_entry_t *e)
{
    return e->ofd && (e->ofd->status_flags & OFD_NONBLOCK);
}

/* Wake the waiters of one end's queue `wq`, and the pollers of a FIFO's
 * descriptions open on both ends, whose readiness any change moves, for a
 * change of the kinds `kinds` (`wq_wake`). */
static void pipe_wake(pipe_t *p, wait_queue_t *wq, uint32_t kinds)
{
    wq_wake(wq, kinds);
    if (p->rdwr_open) {
        wq_wake(&p->rdwr_wq, kinds);
    }
}

/**
 * The bytes of `n` from `addr` that lie in `addr`'s own page: a copy into
 * user memory goes a page at a time, so a page that faults leaves the ones
 * before it moved and reported.
 */
static uint32_t page_piece(uintptr_t addr, uint32_t n)
{
    uint32_t left = PAGE_SIZE - (uint32_t)(addr & (PAGE_SIZE - 1));

    return n < left ? n : left;
}

/**
 * Read one batch of available bytes, waiting only before the first transfer.
 *
 * An empty pipe returns EOF when its writers close, -EAGAIN in nonblocking
 * mode, or -EINTR on an interrupted wait. A later empty ring or user-copy
 * fault returns the count already copied. A fault leaves its bytes unread.
 *
 * Resolve each user page before selecting a ring span. Recheck the ring
 * after any operation that can sleep; copying a span and advancing its
 * counter must not be separated by a context switch.
 *
 * The checked copy's repeated prefault finds the page present and needs no
 * allocation. User pages are not evicted or shared between threads, and
 * interrupt preemption is deferred until the syscall returns.
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
            if (wq_wait_event(&p->read_wq, pipe_readable, p) < 0) {
                return -EINTR;
            }
            continue;
        }

        if (!vm_prefault_range(addr, page_piece(addr, to_read - done), 1)) {
            return (done > 0) ? (int32_t)done : -EFAULT;
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
        done += chunk;
        pipe_wake(p, &p->write_wq, WQ_ROOM);
    }

    return (int32_t)done;
}

/**
 * Write to the write end. Blocks while the FIFO is full (until space frees or
 * every reader closes). A write with no readers raises SIGPIPE and returns
 * -EPIPE. Returns the byte count written, -EAGAIN if O_NONBLOCK and full, or
 * -EINTR if a signal cut a wait short; either returns the count already
 * written instead when there is one. A user-copy fault returns -EFAULT or the
 * count already written. Readers are woken for data at every transfer, the
 * ring empty before it or not, so an edge-triggered epoll registration of the
 * read end is told of each write, as Linux tells one of a pipe it polls.
 *
 * Resolve each user page before selecting a ring span. Recheck the ring
 * after any operation that can sleep; copying a span and advancing its
 * counter must not be separated by a context switch.
 *
 * The checked copy's repeated prefault finds the page present and needs no
 * allocation. User pages are not evicted or shared between threads, and
 * interrupt preemption is deferred until the syscall returns.
 */
static int32_t pipe_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    pipe_t *p = (pipe_t *)e->priv;
    uint32_t done = 0;

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
            {
                /* As Linux sends it: SI_USER, from the writer itself, to
                 * the writing thread. */
                struct k_siginfo info;

                k_memset(&info, 0, sizeof(info));
                info.code = SI_USER;
                info.pid = current_proc->tgid;
                info.uid = current_proc->ruid;
                signal_post_thread(current_proc, SIGPIPE, &info);
            }
            return (done > 0) ? (int32_t)done : -EPIPE;
        }

        space = pipe_free_space(p);
        if (space == 0) {
            if (fd_is_nonblock(e)) {
                return (done > 0) ? (int32_t)done : -EAGAIN;
            }
            if (wq_wait_event(&p->write_wq, pipe_writable, p) < 0) {
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
        if (!p->buf) {
            p->buf = kmalloc(PIPE_BUF_SIZE);
            if (!p->buf) {
                return (done > 0) ? (int32_t)done : -ENOMEM;
            }
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
            pipe_ring_produce(p, chunk);
            done += chunk;
            pipe_wake(p, &p->read_wq, WQ_DATA);
        }
    }

    return (int32_t)done;
}

/* poll(2) event bits, the Linux values. */
#define PIPE_POLLIN  0x001
#define PIPE_POLLOUT 0x004
#define PIPE_POLLERR 0x008
#define PIPE_POLLHUP 0x010

/**
 * Poll readiness for one end, on that end's own wait queue: the read end's
 * read_wq, which data and a writer's close wake, and the write end's
 * write_wq, which freed space and a reader's close wake. A FIFO opened for
 * reading and writing is both ends, and its pollers wait on rdwr_wq, which
 * every change of either wakes (`pipe_wake`).
 *
 * Readiness matches the read and write wait predicates, so a poller wakes for
 * exactly what unblocks a read or a write: the read end is readable with data
 * buffered or the writers gone (the read then returns 0), and the write end
 * is writable with space free or the readers gone (the write then fails with
 * EPIPE). POLLHUP on the read end once the writers are gone, and POLLERR on
 * the write end once the readers are gone, are reported whatever `events`
 * asks.
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
        if (pipe_available(p) != 0 || !p->write_open) {
            revents |= (short)(events & PIPE_POLLIN);
        }
        if (!p->write_open) {
            revents |= PIPE_POLLHUP; /* the peer (writer) hung up */
        }
    }
    if (e->flags & FD_FLAG_WRITE) {
        if (pipe_free_space(p) != 0 || !p->read_open) {
            revents |= (short)(events & PIPE_POLLOUT);
        }
        if (!p->read_open) {
            revents |= PIPE_POLLERR; /* broken pipe: a write would fail with EPIPE */
        }
    }
    return revents;
}

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
 * Release one end when its last descriptor closes (called from
 * fd_release_backing at OFD refcount 1). Count the end closed, wake the peer
 * so a blocked reader sees EOF or a blocked writer sees the broken pipe once
 * no end of its kind is left, and free the pipe once no end is open. A FIFO
 * opened for reading and writing is an end of both kinds.
 */
static void pipe_close(fd_entry_t *e)
{
    pipe_t *p = (pipe_t *)e->priv;

    if (!p) {
        return;
    }
    if (e->flags & FD_FLAG_READ) {
        p->read_open--;
        pipe_wake(p, &p->write_wq, WQ_ALL); /* writers now get SIGPIPE */
    }
    if (e->flags & FD_FLAG_WRITE) {
        p->write_open--;
        pipe_wake(p, &p->read_wq, WQ_ALL); /* readers now get EOF */
    }
    if ((e->flags & FD_FLAG_READ) && (e->flags & FD_FLAG_WRITE)) {
        p->rdwr_open--;
    }
    if (!p->read_open && !p->write_open) {
        pipe_free(p);
    }
}

static const struct file_ops pipe_ops = {pipe_read, pipe_write, pipe_poll, pipe_ioctl, pipe_close};

/** Install a pipe end onto a freshly allocated descriptor. */
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

/* An open of a FIFO waiting for the other end: the opens of that end so far
 * when the wait began. */
struct fifo_wait {
    const pipe_t *p;
    int for_reader;
    uint32_t seen;
};

static int fifo_partner_came(void *arg)
{
    const struct fifo_wait *w = (const struct fifo_wait *)arg;

    return (w->for_reader ? w->p->read_opens : w->p->write_opens) != w->seen;
}

static pipe_t *fifo_find(const void *mnt, uint64_t ino)
{
    pipe_t *p;

    for (p = pipes; p && !(p->fifo && p->fifo_mnt == mnt && p->fifo_ino == ino); p = p->next) {
    }
    return p;
}

/* Take back an open of `reads` and `writes` that did not finish, and free a
 * pipe with no end left. */
static void fifo_unopen(pipe_t *p, int reads, int writes)
{
    p->read_open -= reads;
    p->write_open -= writes;
    if (!p->read_open && !p->write_open) {
        pipe_free(p);
    }
}

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
            return -ENFILE;
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
        if (wq_wait_event(&p->open_wq, fifo_partner_came, &w) < 0) {
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
    if (nonblock && e->ofd) {
        e->ofd->status_flags |= OFD_NONBLOCK;
    }
    return 0;
}

/**
 * pipe2(2) of the user array at `fds_ptr` with `flags`. Both descriptors
 * are reserved, their numbers copied to the caller, and only then given
 * their ends, as Linux's do_pipe2 does: the copy can sleep, and a reserved
 * slot has no type, so a thread sharing the table can neither use nor
 * duplicate an end before the call succeeds, and a copy that fails frees
 * the slots and the pipe, which nothing else can reach.
 */
static int32_t pipe_open(uintptr_t fds_ptr, int flags)
{
    pipe_t *pipe;
    int kfds[2];

    if (!current_proc) {
        return -ESRCH;
    }
    /* Only O_CLOEXEC and O_NONBLOCK are valid pipe2 flags. */
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

long sys_pipe2(struct isr_params *params)
{
    return pipe_open(SC_ARG(params, 0), (int)SC_ARG(params, 1));
}

/* pipe() is pipe2() with no flags. The register of a second argument is
 * not one of its arguments, and comes back as it went, as every register
 * but the result's does. */
long sys_pipe(struct isr_params *params)
{
    return pipe_open(SC_ARG(params, 0), 0);
}
