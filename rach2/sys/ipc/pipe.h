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
 * child's output needs. It comes from the heap at the pipe's first write, so
 * a pipe never written costs only its record.
 */
#ifndef _PIPE_H_
#define _PIPE_H_

#include "../lib/kint.h"
#include "../lib/kuser.h"
#include "../proc/wait_queue.h"

#define PIPE_BUF_SIZE 65536 /* a power of two, for the ring mask */

typedef struct pipe {
    struct pipe *next;     /* the list of live pipes */
    int in_use;            /* 1 from allocation on; no code reads it */
    uint32_t inode;        /* nonzero, unlike every other live pipe's */
    uint8_t *buf;          /* the ring, NULL until the first write */
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
    /* Set while a splice holds the bytes at the front of the ring that it
     * copied out (`pipe_splice_peek`) and is writing, until it takes them
     * (`pipe_splice_consume`); every other reader waits on `splice_wq`
     * meanwhile, so no byte goes to two readers. */
    int spliced;
    wait_queue_t splice_wq;
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
 * -ENFILE when the heap has no room for a pipe, -EINTR when a signal ends
 * the wait. May sleep.
 */
int32_t fifo_attach(struct fd_entry *e, const void *mnt, uint64_t ino, int flags);

/* The operations splice(2), tee(2) and sendfile(2) use on a pipe end, in
 * pipe.c. */

/** Whether the descriptor `e` is an end of a pipe or of a FIFO. */
int pipe_is(const struct fd_entry *e);

/**
 * Write `count` bytes of `buf`, a user buffer or one the thread opened to
 * the user-copy checks, to the write end `e`, as a write(2) of it does but
 * without waiting for room when `nonblock`, the descriptor's O_NONBLOCK or
 * not; the bytes written or a negative errno, -EAGAIN, -EPIPE with SIGPIPE
 * raised, -EINTR, -EBADF, -EFAULT or -ENOMEM.
 */
int32_t pipe_splice_write(struct fd_entry *e, const char __user *buf, uint32_t count, int nonblock);

/**
 * Wait for room in the pipe of the write end `e` unless `nonblock`: the
 * bytes free, or -EAGAIN, -EINTR, -EBADF, or -EPIPE with SIGPIPE raised when
 * no reader is left. A splice into a pipe reads its source only for the room
 * there is, as Linux's does, so that it never waits with bytes in hand.
 */
int32_t pipe_splice_room(struct fd_entry *e, int nonblock);

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
 * and leaving them (tee). Waits for a byte in `in` and then for room in
 * `out` unless `nonblock`, and copies what both allow at once, nothing
 * between that sleeps. Returns the bytes copied, 0 for an empty `in` with no
 * writer left, or a negative errno: -EINVAL for one pipe at both ends,
 * -EAGAIN, -EINTR, -EPIPE with SIGPIPE raised when `out` has no reader, or
 * -ENOMEM when the heap has no room for `out`'s ring.
 */
int32_t pipe_tee(struct fd_entry *in, struct fd_entry *out, uint32_t len, int nonblock, int move);

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
