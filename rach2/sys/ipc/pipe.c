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

/** A new pipe on the live list, with one end of each kind counted open and
 * no ring yet; NULL when the heap has no room. */
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
 * The write of a pipe end: all `count` bytes, waiting whenever the ring is
 * full. Returns the bytes written, or, before a transfer, -EBADF for a
 * descriptor not open for writing, -EFAULT for a bad buffer, -EAGAIN for a
 * full pipe with O_NONBLOCK, -EINTR when a signal ends a wait, or -ENOMEM
 * when the heap has no room for the ring; each of these after a transfer
 * returns the count written instead. With no reader left the write raises
 * SIGPIPE in the calling thread, as Linux sends it, and returns -EPIPE or
 * the count written. Readers are woken for data at every transfer, the ring
 * empty before it or not, so an edge-triggered epoll registration of the
 * read end is told of each write, as Linux tells one of a pipe.
 *
 * Each transfer makes its page present as `pipe_read`'s does, and judges
 * the readers and the ring again after that sleep.
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
                /* Linux's SIGPIPE: SI_USER, from the writer, to its thread. */
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
 * The poll of a pipe end, with `*wq` set to the end's own queue: the read
 * end's `read_wq`, which data and a writer's close wake, or the write end's
 * `write_wq`, which freed room and a reader's close wake. A FIFO opened for
 * reading and writing is both ends, and its pollers wait on `rdwr_wq`, which
 * every change of either wakes (`pipe_wake`).
 *
 * Readiness matches the wait conditions of a read and a write, so a poller
 * wakes for what ends the wait of a read or a write: the read end is
 * readable with data buffered or the writers gone (the read then returns 0),
 * and the write end is writable with room or the readers gone (the write
 * then fails with EPIPE). POLLHUP on the read end once the writers are gone,
 * and POLLERR on the write end once the readers are gone, are reported
 * whatever `events` asks.
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

static const struct file_ops pipe_ops = {pipe_read, pipe_write, pipe_poll, pipe_ioctl, pipe_close};

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
 * pipe2(2) of the user array at `fds_ptr` with `flags`: a new pipe, the
 * read end's descriptor stored first and the write end's second, each with
 * O_CLOEXEC and O_NONBLOCK as `flags` ask. Returns 0, or -ESRCH with no
 * current process, -EINVAL for any other flag, -ENFILE when the heap has no
 * room for a pipe, -EMFILE or -ENOMEM from the descriptor table, or -EFAULT
 * for a bad array.
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
