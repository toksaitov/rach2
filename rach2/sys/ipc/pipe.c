/**
 * Anonymous pipes.
 *
 * A pipe is a bounded byte FIFO with a read end and a write end. Each end is a
 * descriptor carrying pipe_ops + the struct pipe as priv; read/write block on
 * the pipe's wait queues (so other processes run while a stage waits);
 * dup/dup2/fork share an end through the refcounted open-file description, and
 * pipe_close fires (from fd_release_backing) only when an end's LAST descriptor
 * closes. See pipe.h for the EOF/SIGPIPE contract.
 */

#include "pipe.h"
#include "../isr.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../proc/process.h"
#include "../mm/usercopy.h"
#include "../mm/kheap.h"
#include "../lib/kstring.h"
#include "../lib/kerrno.h"
#include "../lib/ksignal.h"

/* Linux ioctls a pipe answers. */
#define FIONREAD 0x541B /* bytes available to read */
#define FIONBIO  0x5421 /* set/clear non-blocking */

/* O_* and OFD status bits used here (mirrors syscall.c / fd.h). */
#define O_CLOEXEC_FLAG  02000000
#define O_NONBLOCK_FLAG 04000

/* The live pipes, each allocated from the heap when made and freed when
 * both ends are closed. */
static pipe_t *pipes;

/* The pure ring helpers (pipe_available/pipe_free_space/pipe_ring_put/
 * pipe_ring_get) live in pipe_ring.c so the host-host test can exercise the
 * wrap arithmetic natively under ASan without pulling in the syscall glue. */

void pipe_init_pool(void)
{
    pipes = NULL;
}

static uint32_t next_pipe_inode = 1;

/**
 * The next inode to give a pipe: rising, nonzero, and not one a live pipe has.
 *
 * Two pipes must not be one thing to a program reading `/proc/<pid>/fd`, and a
 * reused slot must not read as the pipe that had it, so the number rises
 * rather than being the slot. Rising is not enough by itself: the counter is
 * 32 bits and wraps, so the pool is checked and a number in use is stepped
 * over. Zero is skipped because it is what an unset field reads as.
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

/**
 * Read from the read end. Blocks until at least one byte is available or every
 * writer has closed (EOF -> 0). Returns the byte count read (may be < count),
 * -EAGAIN if O_NONBLOCK and empty, -EINTR if a signal interrupts the wait.
 */
static int32_t pipe_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    pipe_t *p = (pipe_t *)e->priv;
    uint8_t kbuf[512];
    uint32_t done = 0;
    uint32_t to_read;

    if (!(e->flags & FD_FLAG_READ)) {
        return -EBADF; /* the write end is not readable */
    }
    if (count == 0) {
        return 0;
    }
    if (!user_range_ok((uint32_t)(__force uintptr_t)buf, count)) {
        return -EFAULT;
    }

    /* Block until data arrives or the last writer closes. */
    while (pipe_available(p) == 0) {
        if (!p->write_open) {
            return 0; /* EOF: all write ends closed and the buffer is drained */
        }
        if (fd_is_nonblock(e)) {
            return -EAGAIN;
        }
        if (wq_wait_event(&p->read_wq, pipe_readable, p) < 0) {
            return -EINTR;
        }
    }

    /* Return what is available now, up to count (POSIX pipe read semantics:
     * a single batch, not a fill-to-count loop). */
    to_read = pipe_available(p);
    if (to_read > count) {
        to_read = count;
    }
    while (done < to_read) {
        uint32_t chunk = to_read - done;
        if (chunk > sizeof(kbuf)) {
            chunk = sizeof(kbuf);
        }
        pipe_ring_get(p, kbuf, chunk);
        if (copy_to_user((void __user *)(buf + done), kbuf, chunk) != 0) {
            /* Bytes already removed from the ring are reported; the rest are
             * lost to the bad pointer, matching the return-count convention. */
            wq_wake_all(&p->write_wq);
            return (done > 0) ? (int32_t)done : -EFAULT;
        }
        done += chunk;
    }

    wq_wake_all(&p->write_wq); /* space freed: wake any blocked writer */
    return (int32_t)done;
}

/**
 * Write to the write end. Blocks while the FIFO is full (until space frees or
 * every reader closes). A write with no readers raises SIGPIPE and returns
 * -EPIPE. Returns the byte count written, -EAGAIN if O_NONBLOCK and full.
 */
static int32_t pipe_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    pipe_t *p = (pipe_t *)e->priv;
    uint8_t kbuf[512];
    uint32_t done = 0;

    if (!(e->flags & FD_FLAG_WRITE)) {
        return -EBADF; /* the read end is not writable */
    }
    if (count == 0) {
        return 0;
    }
    if (!user_range_ok((uint32_t)(__force uintptr_t)buf, count)) {
        return -EFAULT;
    }

    while (done < count) {
        uint32_t space;
        uint32_t chunk;

        /* A vanished reader is fatal to the write regardless of buffer
         * space. */
        if (!p->read_open) {
            signal_post(current_proc, SIGPIPE);
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

        chunk = count - done;
        if (chunk > space) {
            chunk = space;
        }
        if (chunk > sizeof(kbuf)) {
            chunk = sizeof(kbuf);
        }
        if (copy_from_user(kbuf, (const void __user *)(buf + done), chunk) != 0) {
            return (done > 0) ? (int32_t)done : -EFAULT;
        }
        pipe_ring_put(p, kbuf, chunk);
        done += chunk;
        wq_wake_all(&p->read_wq); /* data available: wake any blocked reader */
    }

    return (int32_t)done;
}

/**
 * Poll readiness for one end. POLLIN if the read end has data or the writers
 * are gone; POLLOUT if the write end has space or the readers are gone;
 * POLLHUP when the peer end is closed.
 */
#define PIPE_POLLIN  0x001
#define PIPE_POLLOUT 0x004
#define PIPE_POLLERR 0x008
#define PIPE_POLLHUP 0x010

static short pipe_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    pipe_t *p = (pipe_t *)e->priv;
    short revents = 0;

    /* A readiness change for this end is signaled on its own wait queue: the
     * read end's read_wq (data arrives / writer closes), the write end's
     * write_wq (space frees / reader closes). The poll core parks on it. */
    if (wq) {
        *wq = (e->flags & FD_FLAG_READ) ? &p->read_wq : &p->write_wq;
    }

    /* Readiness must match the read/write wait predicates so a poller wakes for
     * exactly what unblocks read/write: a read end is readable when data is
     * buffered OR the writers are gone (read then returns 0/EOF, not blocks); a
     * write end is "writable" when space exists OR the readers are gone (the
     * write would immediately EPIPE). POLLHUP/POLLERR are reported regardless
     * of the requested events. */
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

static int32_t pipe_ioctl(fd_entry_t *e, uint32_t request, uint32_t arg)
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
    case FIONBIO: {
        int on;
        if (copy_from_user(&on, (const void __user *)arg, sizeof(on)) != 0) {
            return -EFAULT;
        }
        if (e->ofd) {
            if (on) {
                e->ofd->status_flags |= OFD_NONBLOCK;
            } else {
                e->ofd->status_flags &= ~(uint32_t)OFD_NONBLOCK;
            }
        }
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

/**
 * Release one end when its last descriptor closes (called from
 * fd_release_backing at OFD refcount 1). Mark the end closed, wake the peer
 * so a blocked reader sees EOF or a blocked writer sees the broken pipe, and
 * return the pipe to the pool once both ends are gone.
 */
static void pipe_close(fd_entry_t *e)
{
    pipe_t *p = (pipe_t *)e->priv;

    if (!p) {
        return;
    }
    if (e->flags & FD_FLAG_READ) {
        p->read_open = 0;
        wq_wake_all(&p->write_wq); /* writers now get SIGPIPE */
    } else {
        p->write_open = 0;
        wq_wake_all(&p->read_wq); /* readers now get EOF */
    }
    if (!p->read_open && !p->write_open) {
        pipe_free(p);
    }
}

static const struct file_ops pipe_ops = {pipe_read, pipe_write, pipe_poll, pipe_ioctl, pipe_close};

/** Install a pipe end onto a freshly allocated descriptor. */
static void pipe_setup_end(int fd, pipe_t *p, uint32_t access, int flags)
{
    fd_entry_t *e = &current_proc->fds.entries[fd];

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

int32_t sys_pipe2(struct isr_params *params)
{
    uint32_t fds_ptr = params->ebx;
    int flags = (int)params->ecx;
    pipe_t *pipe;
    int rfd, wfd;
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

    /* Set up the read end before allocating the write end: fd_alloc leaves a
     * slot type==FD_TYPE_NONE (the caller assigns the type), so reserving rfd
     * via pipe_setup_end is what stops the second fd_alloc from handing back
     * the very same slot. */
    rfd = fd_alloc(&current_proc->fds);
    if (rfd < 0) {
        pipe_free(pipe);
        return -EMFILE;
    }
    pipe_setup_end(rfd, pipe, FD_FLAG_READ, flags);

    wfd = fd_alloc(&current_proc->fds);
    if (wfd < 0) {
        fd_free(&current_proc->fds, rfd);
        pipe_free(pipe);
        return -EMFILE;
    }
    pipe_setup_end(wfd, pipe, FD_FLAG_WRITE, flags);

    kfds[0] = rfd;
    kfds[1] = wfd;
    if (copy_to_user((void __user *)fds_ptr, kfds, sizeof(kfds)) != 0) {
        /* The user array is unwritable: tear the just-created pipe down rather
         * than leaking the descriptors. Both ends are fresh (OFD refcount 1,
         * nothing blocked), so free the pipe slot and the two fds directly. */
        pipe_free(pipe);
        fd_free(&current_proc->fds, rfd);
        fd_free(&current_proc->fds, wfd);
        return -EFAULT;
    }
    return 0;
}

int32_t sys_pipe(struct isr_params *params)
{
    params->ecx = 0; /* pipe() == pipe2() with no flags */
    return sys_pipe2(params);
}
