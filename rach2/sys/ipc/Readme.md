# `ipc/`: anonymous pipes

The pipe is the original Unix inter-process channel and the thing that makes a shell pipeline `a | b` work: a bounded byte FIFO with a read end and a write end, each handed to user space as a file descriptor. Bytes written at one end come out, in order, at the other; the slower side sets the pace, because a reader blocks while the FIFO is empty and a writer blocks while it is full.

## The end-of-stream contract

This is what lets a pipeline terminate cleanly rather than hang:

* `read()` returns `0` (EOF) once every write-end descriptor is closed and the buffer has drained, so the reader learns the producer is done.
* `write()` raises `SIGPIPE` and returns `-EPIPE` once every read-end descriptor is closed, since there is no longer anyone to read the bytes, so the producer is told to stop (the default `SIGPIPE` action terminates it, which is why `yes | head` exits instead of spinning forever).

`O_NONBLOCK` turns the two blocking cases into `-EAGAIN` instead.

## How it sits on the kernel

Each pipe end is built on the file-backing layer:

* each end is a descriptor whose `ops` is `pipe_ops` (a `struct file_ops` vtable: `read`, `write`, `poll`, `ioctl`, `close`) and whose `priv` is the `struct pipe`. `read`, `write`, `ioctl`, and `close` dispatch through that vtable, so no pipe-specific code lives in the generic syscall layer;
* read and write block on the wait queues (`read_wq`, `write_wq`), so other processes run while a pipeline stage waits, and a blocked stage is signal-interruptible (`-EINTR`);
* the ends ride the refcounted open-file description: `dup`/`dup2`/`fork` share an end and bump the OFD refcount, and `pipe_close` (called from `fd_release_backing`) fires only when an end's last descriptor closes. The `struct pipe` returns to its pool once both ends are gone.

The byte FIFO itself is a power-of-two ring with monotonically increasing head and tail counters (occupancy is the unsigned `tail - head`, the physical slot is `pos & (SIZE-1)`); the wrap arithmetic lives in `pipe_ring.c` so it can be exercised natively under ASan in `tests/host/pipe_ring_hosttest.c`.

## Compared to Linux

Linux `fs/pipe.c`: a pipe is an in-kernel object behind two `struct file`s with a `pipe_fops` operations table, readers and writers sleep on wait queues, and `pipe_release` tears an end down on the last close. Rach2's version is the same shape with a single fixed-size ring instead of Linux's chained page buffers.

## Honest simplifications

* one fixed page (`PIPE_BUF_SIZE` = 4096) per pipe, from a small fixed pool;
* no `F_SETPIPE_SZ` resizing, no packet mode (`O_DIRECT`), no splice, tee, or vmsplice;
* writes are not guaranteed atomic below `PIPE_BUF` under concurrent writers (a single writer per end is the common shell case).

## Verify

* `tests/pipeprobe.c` (freestanding): round-trip and EOF, `O_NONBLOCK` returning `-EAGAIN`, `SIGPIPE` and `-EPIPE`, blocking across a fork (5000 bytes through a 4 KB pipe), and a no-leak loop of create/close cycles past the pool size. Prints `PIPEPROBE OK`.
* `tests/host/pipe_ring_hosttest.c`: the ring wrap arithmetic under ASan and UBSan.
* `tests/musl/pipemusl.c` (installed `mpipe`): a real musl `pipe()` and `fork()` round-trip end to end.
