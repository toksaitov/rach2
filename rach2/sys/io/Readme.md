# `io/`: I/O multiplexing (`select` and `poll`)

`poll` and `select` answer one question: of these many descriptors, which can I touch right now without blocking? They are what lets a single-threaded program (busybox's `ash` loop, an `httpd`, an `sshd`) wait on a keyboard, a socket, and a pipe at once and react to whichever is ready first, instead of blocking on one and going deaf to the others.

## Readiness, not data

Each pollable backing answers a tiny query, "which of `{readable, writable, error/hangup}` is true now", and names the wait queue whose wakeup means "my answer may have changed". That is the whole interface (`file_ops.poll` for `ops`-backed objects: pipes, ttys, and ptys; `socket_poll` for sockets; regular files are always ready):

* a pipe read end is readable when bytes are buffered or the writers are gone (so `read` would return data or `0`, not block), and reports `POLLHUP` once the writers are gone;
* a stream socket is readable with buffered data or a received FIN, writable once `ESTABLISHED`, and `POLLHUP` when the connection is fully closed;
* a regular file is always readable and writable; it never blocks.

The poll core never copies payload; it only collects these readiness bits.

## The core: scan, then park on all of them

`do_poll()` is the lost-wakeup-safe sleep generalized from one wait queue to many. With interrupts off (the single-CPU "lock"):

1. scan every descriptor once (`poll_scan`), recording each one's wait queue;
2. if any is ready (or a signal is pending, or the deadline passed), return;
3. otherwise enqueue the caller on every polled object's wait queue at once (`wq_poll_add`), mark it `BLOCKED`, and yield.

A waker (an IRQ delivering pipe bytes or a TCP segment) calls `wq_wake_all` on one of those queues and marks the poller `READY`; on return the poller leaves all the queues (`wq_poll_remove`) and re-scans. The scan and the enqueue happen under the same `cli`, so a readiness change can never slip into the gap between "saw nothing ready" and "went to sleep"; the lost-wakeup window is closed exactly as in `proc/wait_queue.c`. An always-ready backing (a regular file) reports ready in the scan and so never reaches the park.

## `select` is `poll` underneath

`select`'s three `fd_set` bitmaps are translated to a pollfd array and back (`fdsets_to_pollfds` and `pollfds_to_fdsets` in `poll_select.c`). That bit and word arithmetic is the fence-post-prone part, so it is kept pure and exercised natively under ASan in `tests/host/poll_select_hosttest.c`. The projection is direction-gated: a hangup or error is reported only on a set the caller actually selected the fd in (matching Linux's `POLLIN_SET`/`POLLOUT_SET`), so a `POLLHUP` on a write-only-selected fd is never invented into a read set, and the return value counts each ready direction (an fd ready for read and write counts 2). A selected bit naming an fd `>= MAX_FDS`, or a bit for a closed fd, is `-EBADF`, never a silent truncation.

## ABI: let the real binary pick

i386 musl uses the whole family, so all of them route to one core and whichever number the binary emits just works (confirmed by disassembling the vendored `libc-1.2.5`):

* `poll()` becomes `poll(168)`;
* `select()` tries `pselect6_time64(413)` first (the kernel answers `-ENOSYS`), then falls back to `_newselect(142)`;
* `select(82)` (the old single-pointer form), `pselect6(308)`, and `ppoll(309)` are implemented too. The `time64` variants `413` and `414` are intentionally not implemented; returning `-ENOSYS` is what triggers musl's time32 fallback.

`struct pollfd` is `{int fd; short events; short revents;}`; `fd_set` is musl's `FD_SETSIZE = 1024` bits while `MAX_FDS = 16`.

## Compared to Linux

Linux `fs/select.c`: `do_select` walks the fd_sets calling each file's `->poll`, which returns a readiness mask and registers the task on the file's wait queue through a poll table; if nothing is ready the task sleeps and any wake re-runs the walk. `do_sys_poll` is the same for `pollfd`s. Rach2's `do_poll` is that loop with a fixed per-call array instead of a poll-table allocation.

## Honest simplifications

* the `pselect6` and `ppoll` signal mask is accepted but not applied (there is no atomic pending-mask swap, and busybox and dropbear pass `NULL` or their normal mask here);
* timeouts are quantized to the 10 ms PIT tick, and a parked poller is also re-woken each tick (it re-scans and re-sleeps): correct, if not the tightest;
* `poll` is capped at `MAX_FDS` entries per call (the per-call stack array).

## Verify

* `tests/pollprobe.c` (freestanding): timeout returns `0`, readiness `POLLIN`, `POLLHUP` on a closed writer, the same fd through `select` and `_newselect`, a cross-process block-then-wake, and the fd contract (wide `nfds` works, an out-of-range or closed bit is `-EBADF`, `nfds` past `FD_SETSIZE` is `-EINVAL`). Prints `POLLPROBE OK`.
* `tests/host/poll_select_hosttest.c`: the `fd_set` and pollfd translation, including direction gating, under ASan and UBSan.
* `tests/musl/pollmusl.c` (installed `mpoll`): real musl `poll()` and `select()` over a pipe, end to end, exercising the `413` to `142` fallback.
