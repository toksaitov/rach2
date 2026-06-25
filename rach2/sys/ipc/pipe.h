#ifndef _PIPE_H_
#define _PIPE_H_

#include "../lib/kint.h"
#include "../proc/wait_queue.h"

/*
 * Anonymous pipe: the original Unix IPC primitive. A pipe is a bounded FIFO with
 * a read end and a write end, each handed to user space as a descriptor. Bytes
 * written at the write end are read, in order, at the read end; a reader blocks
 * while the FIFO is empty and a writer blocks while it is full, so a pipeline
 * `a | b` flows at the rate the slower side can keep up with.
 *
 * The end-of-stream contract is what makes pipelines terminate cleanly:
 *   * read() returns 0 (EOF) once every write-end descriptor is closed and the
 *     buffer has drained;
 *   * write() raises SIGPIPE and returns -EPIPE once every read-end descriptor is
 *     closed (nobody will ever read the bytes).
 *
 * The pipe rides the file-backing layer: each end is a descriptor whose `ops` is
 * pipe_ops and whose `priv` is the struct pipe; dup/dup2/fork share the end
 * through the refcounted open-file description, and the end is torn down (via
 * pipe_ops->close) only when its last descriptor closes. read/write block on the
 * wait queues, so other processes run while a pipe stage waits.
 *
 * The byte FIFO is a power-of-two ring with monotonically increasing head/tail
 * counters (count = tail - head); the index math is pure and host-tested.
 */
#define PIPE_BUF_SIZE 4096 /* one page; must stay a power of two for the ring mask */

typedef struct pipe {
    int in_use;
    uint8_t buf[PIPE_BUF_SIZE];
    uint32_t head;         /* total bytes consumed by the read end */
    uint32_t tail;         /* total bytes produced by the write end */
    int read_open;         /* a read-end descriptor still references this pipe */
    int write_open;        /* a write-end descriptor still references this pipe */
    wait_queue_t read_wq;  /* readers blocked waiting for data */
    wait_queue_t write_wq; /* writers blocked waiting for space */
} pipe_t;

/** Allocate the pipe pool from kernel memory; called once from init. */
void pipe_init_pool(void);

/*
 * sys_pipe2(int pipefd[2], int flags): create a pipe and return its read-end fd
 * in pipefd[0] and write-end fd in pipefd[1]. flags may set O_CLOEXEC and
 * O_NONBLOCK on both ends. Returns 0 on success, -errno otherwise. sys_pipe is
 * the same with flags == 0.
 */
struct isr_params;
int32_t sys_pipe(struct isr_params *params);
int32_t sys_pipe2(struct isr_params *params);

/* Pure ring helpers; pipe_ring_put/pipe_ring_get are contracted in pipe_ring.c. */
uint32_t pipe_available(const pipe_t *p);
uint32_t pipe_free_space(const pipe_t *p);
uint32_t pipe_ring_put(pipe_t *p, const uint8_t *src, uint32_t n);
uint32_t pipe_ring_get(pipe_t *p, uint8_t *dst, uint32_t n);

#endif /* _PIPE_H_ */
