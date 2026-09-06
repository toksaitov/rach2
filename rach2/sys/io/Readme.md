# `io/`: `poll` and `select`

Rach2 implements `poll`, `ppoll`, `select`, `_newselect`, and `pselect6` over one readiness scanner. These calls report which descriptors can make progress without waiting. They do not copy file, terminal, pipe, or socket payloads.

## Readiness

Each descriptor reports a subset of the requested `POLLIN`, `POLLOUT`, and `POLLPRI` bits. `POLLERR`, `POLLHUP`, and `POLLNVAL` are returned without being requested.

An operations-backed descriptor calls its `file_ops.poll` method and supplies the wait queue whose wakeup can change its answer. Pipes, devfs files, procfs snapshots, and pseudoterminals use this path. The console calls the tty poller. Sockets call `socket_poll`. Regular KRFFS files and directories report requested read and write readiness without a wait queue.

The main backing rules are:

* A pipe uses its buffered byte count and peer lifetime. Its complete readiness contract is in the [pipe documentation](../ipc/Readme.md).
* A tty is readable when its line discipline has input ready and is always writable. A pseudoterminal also reports peer hangup.
* Datagram and protocol-bearing raw sockets are readable when a packet is queued and always writable. A listening stream socket is readable when an accepted connection is queued. A connected stream socket is readable with buffered data or a received FIN; it is writable in `ESTABLISHED` or `CLOSE_WAIT` while its send ring has space. Socket errors and a closed stream report `POLLERR` and `POLLHUP` as applicable. See the [network documentation](../network/Readme.md).
* Procfs snapshots report read readiness, including at EOF. Devfs character devices report requested read and write readiness.

The framebuffer, mouse, and any remaining descriptor type use the fallback that reports requested read and write readiness immediately. Mouse readiness is not tied to packet availability, so `poll` cannot wait for a mouse packet.

For `poll`, a negative descriptor is ignored. A closed or out-of-range nonnegative descriptor receives `POLLNVAL` and contributes to the return count. The return value is the number of `pollfd` entries with a nonzero result.

## Scan and wait

The core disables interrupts and scans every entry once. If one is ready, a deliverable signal is pending, or the deadline has passed, it returns. Otherwise it adds the process to every non-null wait queue, arms its timer deadline when needed, marks it blocked, and schedules another process.

A pipe operation, socket path, terminal path, or signal can wake the process. The 100 Hz timer wakes a finite wait at its deadline. After any wake, the process removes itself from every queue and scans again. The scan and all queue insertions happen while interrupts are disabled, so a readiness change cannot occur between the test and the sleep.

If no descriptor is ready, a pending signal returns `EINTR`; the kernel does not restart the call for `SA_RESTART`. A finite timeout is rounded up to a 10 ms tick. A zero timeout performs one scan without waiting. `poll` with no entries and a finite timeout acts as a sleep.

## `select` translation

`select` translates its read, write, and exception bitmaps into a dense `pollfd` array. One selected descriptor becomes one entry whose event bits record every set that contained it. The result is projected back as follows:

* `POLLIN`, `POLLHUP`, or `POLLERR` satisfies a requested read bit.
* `POLLOUT` or `POLLERR` satisfies a requested write bit.
* `POLLPRI` satisfies a requested exception bit.

Projection is limited to sets in which the caller selected the descriptor. A hangup on a write-only selection does not create a read result. The return value counts ready set memberships, so one descriptor ready for both read and write contributes two.

An `fd_set` has 1024 bits, matching the maximum number of descriptors a process may hold. `nfds` may be as large as 1024. Every selected descriptor below `nfds` must be open, or the call returns `EBADF`; `nfds` outside 0 through 1024 returns `EINVAL`.

## System-call ABI and limits

The implemented i386 calls are old packed-argument `select(82)`, `_newselect(142)`, `poll(168)`, `pselect6(308)`, and `ppoll(309)`. The `pselect6_time64(413)` and `ppoll_time64(414)` calls are absent and return `ENOSYS`.

The ported i386 musl calls `poll(168)` directly. It remaps ordinary `select` to `_newselect(142)`. `pselect` and `ppoll` use calls 308 and 309 while the timeout seconds fit in 32 bits. Only a larger timeout makes musl try call 413 or 414 before falling back to the 32-bit call with a clamped timeout.

`poll` accepts at most 1024 entries, the maximum number of descriptors a process may hold. Its descriptor, wait-queue and waiter arrays are allocated from the heap for each call. `select` reads and writes a complete 128-byte `fd_set` for each non-null set. `timeval` and `timespec` fields must be nonnegative and normalized; an invalid value returns `EINVAL`. This implementation does not write the remaining `select` time back to the caller's `timeval`.

`ppoll`'s signal-mask arguments and `pselect6`'s sixth mask block are ignored. There is no atomic signal-mask replacement around the wait. No backing reports `POLLPRI`, so an exception-only `select` cannot become ready. The kernel does not implement `epoll`.

## Verification

`make test` runs `tests/pollprobe.c`. It checks a timeout on two empty pipes, `POLLIN`, `POLLHUP`, `_newselect`, a child write waking a blocked poller, invalid selected descriptors, the 1024-bit `nfds` bound, and invalid `timeval` normalization. The same default suite runs the musl-linked `addusermusl`, `passwdmusl`, and `loginmusl` programs, which use libc `poll` while collecting data from child-process pipes.

`tests/run host` runs `tests/host/poll_select_hosttest.c` under AddressSanitizer and UndefinedBehaviorSanitizer. It checks bitmap word boundaries, dense translation, multi-set descriptors, array-capacity failure, error and hangup projection, direction gating, and ready-set counting.

`tests/musl/pollmusl.c` is built and staged at `/musl/pollmusl` for manual use. Its short timeouts exercise musl's direct `poll(168)` and `_newselect(142)` paths over a pipe, including timeout, readiness, and a cross-process wake. It does not exercise the time64 fallback.

The specialized guest targets extend socket coverage: `tests/run packet` checks `poll` and `select` over a packet socket, `tests/run raw` polls a raw socket, `tests/run netif` checks immediate `POLLERR` on an ioctl-only socket, and `tests/run dhcp` drives the in-house DHCP client's libc `poll` waits.
