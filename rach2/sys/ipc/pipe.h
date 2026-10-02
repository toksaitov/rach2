#ifndef _PIPE_H_
#define _PIPE_H_

#include "../lib/kint.h"
#include "../proc/wait_queue.h"

/*
 * Anonymous pipe: the original Unix IPC primitive. A pipe is a bounded FIFO
 * with a read end and a write end, each handed to user space as a descriptor.
 * Bytes written at the write end are read, in order, at the read end; a reader
 * blocks while the FIFO is empty and a writer blocks while it is full, so a
 * pipeline `a | b` flows at the rate the slower side can keep up with.
 *
 * The end-of-stream contract is what makes pipelines terminate cleanly:
 *   * read() returns 0 (EOF) once every write-end descriptor is closed and the
 *     buffer has drained;
 *   * write() raises SIGPIPE and returns -EPIPE once every read-end descriptor
 *     is closed (nobody will ever read the bytes).
 *
 * The pipe rides the file-backing layer: each end is a descriptor whose `ops`
 * is pipe_ops and whose `priv` is the struct pipe; dup/dup2/fork share the end
 * through the refcounted open-file description, and the end is torn down (via
 * pipe_ops->close) only when its last descriptor closes. read/write block on
 * the wait queues, so other processes run while a pipe stage waits.
 *
 * The byte FIFO is a power-of-two ring with monotonically increasing
 * head/tail counters (count = tail - head); the index math is pure and
 * host-tested. The ring holds 64 KiB, Linux's default capacity, so a write of
 * that much to an empty pipe does not wait, as a program that writes a
 * child's input before it reads the child's output needs. It comes from the
 * heap at the pipe's first write, so a pipe that carries nothing costs only
 * its record.
 */
#define PIPE_BUF_SIZE 65536 /* a power of two, for the ring mask */

typedef struct pipe {
    struct pipe *next; /* the list of live pipes */
    int in_use;
    uint32_t inode;        /* its own; never reused within a boot */
    uint8_t *buf;          /* the ring, NULL until the first write */
    uint32_t head;         /* total bytes consumed by the read end */
    uint32_t tail;         /* total bytes produced by the write end */
    int read_open;         /* descriptions open on the read end */
    int write_open;        /* descriptions open on the write end */
    wait_queue_t read_wq;  /* readers blocked waiting for data */
    wait_queue_t write_wq; /* writers blocked waiting for space */
    /* A FIFO's: the file it is the pipe of, the opens of each end so far,
     * which an open waiting for the other end watches, and the queue it
     * waits on. The pipe lasts while an end is open, as in Linux. */
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
} pipe_t;

/** Allocate the pipe pool from kernel memory; called once from init. */
void pipe_init_pool(void);

/**
 * Make the fresh descriptor `e` an end of the pipe of the FIFO file (`mnt`,
 * `ino`), as open(2) with `flags` asks, making the pipe at the first open.
 *
 * A reader waits for a writer and a writer for a reader, as in Linux: with
 * O_NONBLOCK a reader does not wait and a writer with no reader fails with
 * ENXIO; O_RDWR opens both ends and never waits. Returns 0, or -errno with
 * `e` as it was, for the caller to free.
 */
struct fd_entry;
int32_t fifo_attach(struct fd_entry *e, const void *mnt, uint64_t ino, int flags);

/*
 * sys_pipe2(int pipefd[2], int flags): create a pipe and return its read-end fd
 * in pipefd[0] and write-end fd in pipefd[1]. flags may set O_CLOEXEC and
 * O_NONBLOCK on both ends. Returns 0 on success, -errno otherwise. sys_pipe is
 * the same with flags == 0.
 */
struct isr_params;
int32_t sys_pipe(struct isr_params *params);
int32_t sys_pipe2(struct isr_params *params);

/* Pure ring helpers; pipe_ring_put/pipe_ring_get are contracted in
 * pipe_ring.c. */
uint32_t pipe_available(const pipe_t *p);
uint32_t pipe_free_space(const pipe_t *p);
uint32_t pipe_ring_put(pipe_t *p, const uint8_t *src, uint32_t n);
uint32_t pipe_ring_get(pipe_t *p, uint8_t *dst, uint32_t n);
/* The spans: the longest run of readable bytes from the head, or of free
 * slots from the tail, that does not cross the ring's end, and the counters
 * advanced after a caller copied straight from or into them. */
uint32_t pipe_ring_read_span(pipe_t *p, const uint8_t **at);
uint32_t pipe_ring_write_span(pipe_t *p, uint8_t **at);
void pipe_ring_consume(pipe_t *p, uint32_t n);
void pipe_ring_produce(pipe_t *p, uint32_t n);

#endif /* _PIPE_H_ */
