# `ipc/`: anonymous pipes

Rach2 implements anonymous byte pipes through `pipe` and `pipe2`. A pipe has a read end, a write end, and a 4096-byte ring. It preserves byte order but has no message boundaries.

## Creation and lifetime

`pipe(int pipefd[2])` writes the read descriptor to `pipefd[0]` and the write descriptor to `pipefd[1]`. `pipe2` accepts only `O_CLOEXEC` and `O_NONBLOCK`; it applies either flag to both new descriptors and rejects any other bit with `EINVAL`.

The two ends start with separate open-file descriptions. `dup`, `dup2`, `dup3`, `F_DUPFD`, `F_DUPFD_CLOEXEC`, and `fork` share the open-file description for the end they copy. Its status flags are shared too, so `O_NONBLOCK` changes made through `fcntl` or `FIONBIO` are visible through duplicates of that end. `FD_CLOEXEC` remains a descriptor flag.

Closing a descriptor releases an end only when the last descriptor sharing its open-file description closes. Closing the last writer wakes readers to drain buffered data and observe EOF. Closing the last reader wakes writers for `SIGPIPE`. The pipe returns to its fixed pool after both ends close. The pool has 16 entries, and a process can hold at most 16 descriptors.

Each live pipe has a nonzero inode distinct from every other live pipe. Both ends report that inode as a FIFO with permissions 0666 through `fstat`, and `/proc/<pid>/fd/<fd>` renders it as `pipe:[inode]`.

## Reads and writes

A read of an empty pipe waits while a writer remains open. It returns `EAGAIN` instead when the read end is non-blocking, and returns `0` once the last writer has closed. Buffered data is returned before EOF. One call reads the bytes available at that point, up to the requested count; it does not wait to fill the caller's buffer.

A blocking write continues until it has copied the requested count while a reader remains. If the pipe fills, the writer waits for space. A non-blocking write uses the space currently available and returns a short count; it returns `EAGAIN` only when it cannot copy a byte. If the last reader has closed, the kernel posts `SIGPIPE` and returns `EPIPE` when no byte has been written by that call. A reader closure, signal, or user-copy fault after a partial transfer returns the partial count.

Pipe waits are signal-interruptible. A wait interrupted before any transfer returns `EINTR`; the kernel does not implement `SA_RESTART`. See the [signal documentation](../proc/signal.Readme.md) for delivery details. Reads wake blocked writers after freeing space, writes wake blocked readers after adding data, and either final close wakes the other end. The wait queues admit every process in the fixed process table and wake all sleepers to recheck the condition.

The 4096-byte `PIPE_BUF_SIZE` is the ring capacity, not an atomic-write limit. A blocking write uses whatever space is free before it waits. Concurrent writers can therefore interleave even when each request is no larger than 4096 bytes. There is no POSIX `PIPE_BUF` atomicity guarantee.

## Descriptor operations

Pipe descriptors use a `struct file_ops` table for `read`, `write`, `poll`, `ioctl`, and final `close`. The generic descriptor layer owns duplication, fork inheritance, close-on-exec, and last-reference release.

The read end reports `POLLIN` when data is buffered or no writer remains, and always reports `POLLHUP` after the last writer closes. The write end reports `POLLOUT` when space exists or no reader remains, and always reports `POLLERR` after the last reader closes. These conditions match the ones that stop `read` and `write` from waiting.

`FIONREAD` reports the buffered byte count. `FIONBIO` sets or clears `O_NONBLOCK` on that end's open-file description. Other pipe ioctls return `ENOTTY`. `lseek` and `_llseek` return `ESPIPE`.

The ring uses monotonically increasing 32-bit head and tail counters. Occupancy is unsigned `tail - head`; a physical index is the counter masked by `PIPE_BUF_SIZE - 1`. The pure ring operations are in `pipe_ring.c` so the host suite can test the same code as the kernel.

The implementation has no pipe resizing, packet mode, `splice`, `tee`, or `vmsplice`.

## Verification

`make test` runs two pipe-specific freestanding probes:

* `tests/pipeprobe.c` checks byte order, EOF, the 4096-byte capacity, non-blocking empty and full cases, `SIGPIPE` and `EPIPE`, a 5000-byte transfer that blocks across `fork`, 100 create-close cycles, and agreement between the two ends' `fstat` and `/proc` identities.
* `tests/pollprobe.c` checks `POLLIN` readiness, timeout, `POLLHUP`, `select` translation, and a poller woken by a child writing after a delay.

`tests/run host` runs `tests/host/pipe_ring_hosttest.c` under AddressSanitizer and UndefinedBehaviorSanitizer. It checks empty and full rings, bounded partial operations, byte order, and a transfer across the physical end of the array. `tests/host/wait_queue_hosttest.c` checks fixed-array insertion, removal, capacity, and wake-all behavior; the guest probes above exercise the actual scheduling path.

`tests/musl/pipemusl.c` is built and staged at `/musl/pipemusl` for a manual libc `pipe`, `fork`, read, write, close, and wait round trip. It is not an entry in the default `make test` runner.
