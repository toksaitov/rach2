/*
 * pipe.h: anonymous pipes, and the pipe behind each FIFO file.
 *
 * A pipe is a bounded byte FIFO with a read end and a write end, each a
 * descriptor. Bytes written at the write end are read, in order, at the read
 * end; a reader waits while the pipe is empty and a writer while it is full.
 * read() returns 0 once every descriptor of the write end is closed and the
 * buffer is empty, and write() raises SIGPIPE and returns -EPIPE once every
 * descriptor of the read end is closed.
 *
 * An end is a descriptor of type FD_TYPE_PIPE whose `ops` is pipe_ops and
 * whose `priv` is the struct pipe. dup, dup2 and fork share an end through
 * its open file description, and the end is closed (pipe_ops->close) when
 * the description's last reference goes.
 *
 * The bytes are in a power-of-two ring with two rising counters, whose
 * difference is the count buffered (pipe_ring.c). The ring has 64 KiB,
 * Linux's default capacity, so a write of that much to an empty pipe does
 * not wait, as a program that writes a child's input before it reads the
 * child's output needs; fcntl's F_SETPIPE_SZ makes it larger or smaller
 * (`pipe_set_size`). A new pipe's ring comes from the heap at its first
 * write, so a pipe never written costs only its record. Each pipe's pages
 * are charged to the real user that made it, as Linux charges them.
 */
#ifndef _PIPE_H_
#define _PIPE_H_

#include "../lib/kint.h"
#include "../lib/kuser.h"
#include "../fs/sleeplock.h"
#include "../proc/wait_queue.h"

#define PIPE_BUF_SIZE 65536   /* a new pipe's ring, a power of two */
#define PIPE_MAX_SIZE 1048576 /* fs/pipe-max-size at boot, Linux's */

/* fs/pipe-user-pages-soft at boot, Linux's. */
#define PIPE_USER_PAGES_SOFT 16384u
/* A new pipe's pages once its user's pipes pass `pipe_user_pages_soft`,
 * Linux's PIPE_MIN_DEF_BUFFERS. */
#define PIPE_MIN_PAGES       2u

/* The pages one real user's pipes may hold, each pipe's whole ring counted,
 * before an unprivileged caller's new pipe gets PIPE_MIN_PAGES and its
 * F_SETPIPE_SZ growth fails, and before its new pipe fails and its growth
 * fails: /proc/sys/fs/pipe-user-pages-soft and pipe-user-pages-hard, as on
 * Linux, where 0 is no limit. */
extern unsigned long pipe_user_pages_soft;
extern unsigned long pipe_user_pages_hard;

/* The largest ring F_SETPIPE_SZ gives an unprivileged caller, and the
 * largest a new pipe of one has: a power of two from a page to 2^31, which
 * /proc/sys/fs/pipe-max-size reads and sets. */
extern uint32_t pipe_max_size;

/* One of Linux's page buffers over a pipe's ring (`pipe_set_size`). */
struct pipe_page {
    uint32_t start;  /* the counter value of its first byte */
    uint16_t offset; /* that byte's offset in its page, Linux's buf->offset */
    uint16_t merge;  /* nonzero when a later write's bytes may join it */
};

/*
 * A pipe's fields are kept under its lock, `lock`, but for these: `next`,
 * `files` and `nr_accounted` are kept under the list's lock (`pipes_lock`
 * in pipe.c); `inode`, `user` and the FIFO's file are set before the pipe
 * is on the list and do not change; and poll and the wait conditions read
 * the counts and counters with no lock, as Linux's `pipe_poll` reads them
 * (fs/pipe.c:657-705 in 6.12), since a waiter is on its queue before it
 * reads them and every change under the lock wakes the queue after it.
 */
typedef struct pipe {
    struct pipe *next; /* the list of live pipes */
    int in_use;        /* 1 from allocation on; no code reads it */
    uint32_t inode;    /* nonzero, unlike every other live pipe's */
    uint32_t user;     /* the real user charged with its pages */
    /* The open file descriptions of the pipe's ends, one open for reading
     * and writing counting once, and the opens of a FIFO that have counted
     * themselves and not yet finished: Linux's `pipe->files`. The last to
     * go frees the pipe, after it has given the pipe's lock back
     * (`pipe_info_put`). */
    uint32_t files;
    /* The pages charged to `user`, the ring's size in pages, which a resize
     * charges before it allocates (`pipe_resize`): Linux's `nr_accounted`. */
    uint32_t nr_accounted;
    uint8_t *buf;          /* the ring, NULL until the first write */
    uint32_t size;         /* the ring's bytes, a power of two, a page or more */
    uint32_t head;         /* total bytes consumed by the read end */
    uint32_t tail;         /* total bytes produced by the write end */
    int read_open;         /* descriptions open on the read end */
    int write_open;        /* descriptions open on the write end */
    wait_queue_t read_wq;  /* readers and pollers of the read end */
    wait_queue_t write_wq; /* writers and pollers of the write end */
    /* A FIFO's pipe has `fifo` set and the file it belongs to, by mount and
     * number (`fifo_find`); the opens of each end so far, whose change a
     * blocking open of the other end waits for; and the queue that open
     * sleeps on. The pipe lasts while an end is open, as in Linux. */
    int fifo;
    const void *fifo_mnt;
    uint64_t fifo_ino;
    uint32_t read_opens;
    uint32_t write_opens;
    wait_queue_t open_wq;
    /* The descriptions of a FIFO open for reading and writing at once, and
     * the queue their pollers wait on, which every change of either end
     * wakes while there is one. */
    int rdwr_open;
    wait_queue_t rdwr_wq;
    /* Set by the first poll, select or epoll that asks about an end
     * (`pipe_poll`), and never cleared, as Linux's `poll_usage`: from then
     * on every change wakes its queues, which puts an epoll registration
     * of the end on its set's ready list; before, a change wakes only a
     * sleeper. */
    volatile int polled;
    /* Set while a splice holds the bytes at the front of the ring that it
     * copied out (`pipe_splice_peek`) and is writing, until it takes them
     * (`pipe_splice_consume`); every other reader waits on `splice_wq`
     * meanwhile, so no byte goes to two readers. */
    int spliced;
    wait_queue_t splice_wq;
    /* Linux's page buffers over the ring, which its shrink rule counts
     * (`pipe_set_size`): each buffer that still holds bytes, and each empty
     * one a write whose page faulted left, oldest first, `bufs` of them
     * from `buf_first` in the `buf_cap` slots of `buf_pages`, allocated with
     * the ring. A NULL `buf_pages` keeps none. */
    struct pipe_page *buf_pages;
    uint32_t buf_cap;
    uint32_t buf_first;
    uint32_t bufs;
    /* The thread of the last write and the processor it wrote on, which a
     * reader that finds the pipe empty spins on while that thread runs
     * there (`pipe_read_spin`). The thread is compared, never read, so it
     * may have exited; set under `lock`, read without it as a hint. */
    const struct process *writer;
    int writer_cpu;
    /* Linux's `pipe->mutex`: held by a read, a write, a splice's or a tee's
     * use of the ring, a resize, FIONREAD, a close and a FIFO's open from
     * their start to their return, across the page faults of their copies,
     * and given back only while they wait (`pipe_sleep`), so no other
     * reader or writer moves the ring while a copy sleeps. No call holds
     * the big lock for it: the operations of a pipe end run without the big
     * lock (FILE_OPS_UNLOCKED), and a tee takes two, the lower address
     * first, the second by subclass, as Linux's `pipe_double_lock`. */
    struct sleep_lock lock;
} pipe_t;

/** Empty the list of live pipes; called once at boot. Each pipe comes from
 * the heap when it is made. */
void pipe_init_pool(void);

struct fd_entry;
/**
 * Make the fresh descriptor `e` an end of the pipe of the FIFO file that
 * `mnt` and `ino` identify, as open(2) with `flags` asks, making the pipe at
 * the file's first open.
 *
 * A reader waits for a writer and a writer for a reader, as in Linux: with
 * O_NONBLOCK a reader does not wait and a writer with no reader fails with
 * ENXIO; O_RDWR opens both ends and never waits. Returns 0, or -errno with
 * `e` as it was, for the caller to free: -EINVAL for the access mode 3,
 * -ENOMEM when the heap has no room for a pipe or the caller's user is at
 * `pipe_user_pages_hard`, as Linux's `fifo_open` answers both, -EINTR when
 * a signal ends the wait. May sleep.
 */
int32_t fifo_attach(struct fd_entry *e, const void *mnt, uint64_t ino, int flags);

/* The operations splice(2), tee(2) and sendfile(2) use on a pipe end, in
 * pipe.c. */

/** Whether the descriptor `e` is an end of a pipe or of a FIFO. */
int pipe_is(const struct fd_entry *e);

/**
 * Write up to `count` bytes of `buf`, a user buffer or one the thread opened
 * to the user-copy checks, to the write end `e`, as a write(2) of it does,
 * but waiting for room only before the first byte and not at all when
 * `nonblock`, the descriptor's O_NONBLOCK or not, and returning what fitted
 * once the ring fills, as Linux's splice does; the bytes written or a
 * negative errno, -EAGAIN, -EPIPE with SIGPIPE raised, -EINTR, -EBADF,
 * -EFAULT or -ENOMEM. The bytes take a page buffer for each page of their
 * source they come from, as Linux's splice links the source's pages, the
 * first byte lying `src` bytes into its page, and no later write joins them.
 * The ring is full, as Linux's `pipe_full` judges it, once its buffers are
 * as many as its pages, however few bytes they hold.
 */
int32_t pipe_splice_write(struct fd_entry *e, const char __user *buf, uint32_t count, int nonblock,
                          uint32_t src);

struct kernel_iovec;

/**
 * Write the `count` elements of `iov`, a vector iov_import has checked and
 * cut, to the write end `e` as one write(2) of their bytes, as Linux runs a
 * writev(2) through one `pipe_write`: the newest buffer takes the vector's
 * first total mod 4096 bytes when it can, and each page buffer opened after
 * it fills across the elements. Each of those groups is committed whole
 * once it is full or the vector is spent, so a fault leaves none of its
 * group readable, as Linux leaves an unfinished page empty; a page group
 * that faults leaves its buffer in the pipe empty, as Linux's does, which
 * poll reports as data, which takes a slot and which a later small write
 * joins, until a read reaches it. Returns as a
 * write(2) of the pipe does: the bytes written, all of them unless
 * O_NONBLOCK or an error cuts the write, or -EAGAIN, -EPIPE with SIGPIPE
 * raised, -EINTR, -EBADF, -EFAULT or -ENOMEM before any byte. May sleep.
 */
int32_t pipe_writev(struct fd_entry *e, const struct kernel_iovec *iov, int count);

/**
 * Read the read end `e` into the `count` elements of `iov`, a vector
 * iov_import has checked and cut, as one read(2) of their bytes, as Linux
 * runs a readv(2) through one `pipe_read`: each page buffer of the pipe is
 * copied whole, or as much of it as the vector takes, or not at all, so a
 * destination that cannot be written leaves its buffer's bytes queued. The
 * wait, the end of input and -EAGAIN come before any copy. Returns as a
 * read(2) of the pipe does: the bytes read, 0 at the end of input or for a
 * vector of no bytes, or -EBADF, -EFAULT, -EAGAIN or -EINTR before any
 * byte. May sleep.
 */
int32_t pipe_readv(struct fd_entry *e, const struct kernel_iovec *iov, int count);

/**
 * Write up to `count` bytes of the caller's memory at `buf` to the write end
 * `e` for vmsplice(2), as `pipe_splice_write` writes them, the source's pages
 * being those of `buf`. A page that Linux's get_user_pages refuses
 * (`paging_pin_allows`), the vDSO's data page among them, ends the write
 * with -EFAULT, or with the count written before it.
 */
int32_t pipe_vmsplice_write(struct fd_entry *e, const char __user *buf, uint32_t count,
                            int nonblock);

/**
 * Wait for room in the pipe of the write end `e` unless `nonblock`: the
 * bytes a splice whose first byte lies `src` bytes into its source page can
 * write, a buffer for each page with a slot free (`pipe_splice_write`) and
 * no more than the bytes free, or -EAGAIN, -EINTR, -EBADF, or -EPIPE with
 * SIGPIPE raised when no reader is left. A splice into a pipe reads its
 * source only for the room there is, as Linux's does, so that it never
 * waits with bytes in hand.
 */
int32_t pipe_splice_room(struct fd_entry *e, int nonblock, uint32_t src);

/**
 * Copy up to `n` of the bytes buffered in the read end `e`'s pipe into the
 * kernel buffer `dst`, without taking them, after waiting for one unless
 * `nonblock`, and for any other splice's hold to end, and hold them: until
 * the caller's `pipe_splice_consume`, which it must make whenever this
 * returns above 0, no other reader takes a byte of the pipe. Returns the
 * bytes copied, 0 for an empty pipe with no writer left, or -EAGAIN, -EINTR
 * or -EBADF.
 */
int32_t pipe_splice_peek(struct fd_entry *e, uint8_t *dst, uint32_t n, int nonblock);

/**
 * Take from the read end `e`'s pipe the first `n` of the bytes that
 * `pipe_splice_peek` held, 0 for none when the write failed, end the hold,
 * and wake the writers for room and the readers that waited for the hold.
 */
void pipe_splice_consume(struct fd_entry *e, uint32_t n);

/**
 * Copy up to `len` bytes from the pipe of the read end `in` to that of the
 * write end `out`, ring to ring, taking them from `in` when `move` (splice)
 * and leaving them (tee). Waits for a buffer in `in`, an empty one a
 * faulted write left among them, and then for room in `out` unless
 * `nonblock`, and links what both allow at once, nothing between that
 * sleeps: the buffers of `in` from its head, each whole or the part `len`
 * leaves, one for each free slot of `out`, as Linux's link_pipe and
 * splice_pipe_to_pipe link them, an empty buffer too while `len` is not
 * spent. A tee that linked only empty buffers returns 0, as Linux's does; a
 * splice that moved only empty buffers goes on waiting, or returns -EAGAIN
 * when `nonblock`. Returns the bytes copied, 0 for an `in` that holds
 * nothing and has no writer left, or a negative errno: -EINVAL for one pipe
 * at both ends, -EAGAIN, -EINTR, -EPIPE with SIGPIPE raised when `out` has
 * no reader, or -ENOMEM when the heap has no room for `out`'s ring.
 */
int32_t pipe_tee(struct fd_entry *in, struct fd_entry *out, uint32_t len, int nonblock, int move);

/** The bytes the ring of the pipe end `e` holds, which F_GETPIPE_SZ gives. */
int32_t pipe_get_size(const struct fd_entry *e);

/**
 * Give the ring of the pipe end `e` at least `size` bytes, as F_SETPIPE_SZ
 * does: a power of two from a page up, the bytes buffered kept, in a ring
 * allocated now. Returns the size given, or -EINVAL for a size past 2^31,
 * -EPERM for a growth past `pipe_max_size`, or one that would take the
 * pages of the pipe's user past either limit, unless `privileged`,
 * -ENOMEM when the heap has no room for the ring, or -EBUSY for a size
 * below the bytes buffered.
 */
int32_t pipe_set_size(struct fd_entry *e, uint32_t size, int privileged);

/** Round `size` as Linux's `round_pipe_size` does: a page for less, the
 * next power of two otherwise; 0 for a size past 2^31. */
uint32_t pipe_round_size(uint32_t size);

struct isr_params;
/* The handlers of pipe(2) and pipe2(2), which arch_syscall_init enters in
 * the table on both architectures. */
long sys_pipe(struct isr_params *params);
long sys_pipe2(struct isr_params *params);

/* The ring operations, in pipe_ring.c. Those that copy or give a span need
 * `buf` allocated. */

/** The bytes buffered. */
uint32_t pipe_available(const pipe_t *p);

/** The bytes the ring has room for. */
uint32_t pipe_free_space(const pipe_t *p);

/**
 * Copy up to `n` bytes from `src` into the ring; the count stored, the
 * smaller of `n` and the room, so a short count means the ring filled.
 * Advances `tail`, which makes the bytes readable.
 */
uint32_t pipe_ring_put(pipe_t *p, const uint8_t *src, uint32_t n);

/**
 * Copy up to `n` bytes out of the ring into `dst`; the count removed, the
 * smaller of `n` and the bytes buffered, so a short count means the ring
 * emptied. Advances `head`, which frees the slots for a writer.
 */
uint32_t pipe_ring_get(pipe_t *p, uint8_t *dst, uint32_t n);

/**
 * The longest run of buffered bytes from the head that does not cross the
 * ring's end: its address in `*at` and its length, 0 on an empty ring. A
 * caller that copies out of it calls `pipe_ring_consume`; the bytes after
 * the wrap are the next span.
 */
uint32_t pipe_ring_read_span(pipe_t *p, const uint8_t **at);

/**
 * The longest run of free slots from the tail that does not cross the
 * ring's end: its address in `*at` and its length, 0 on a full ring. A
 * caller that copies into it calls `pipe_ring_produce`.
 */
uint32_t pipe_ring_write_span(pipe_t *p, uint8_t **at);

/** Remove `n` bytes the caller copied out of a read span. */
void pipe_ring_consume(pipe_t *p, uint32_t n);

/** Make readable `n` bytes the caller copied into a write span. */
void pipe_ring_produce(pipe_t *p, uint32_t n);

#endif /* _PIPE_H_ */
