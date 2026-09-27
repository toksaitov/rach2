# `io/`: `poll`, `select`, `epoll`, `eventfd` and `timerfd`

Rach2 implements `poll`, `ppoll`, `select`, `_newselect`, and `pselect6` over one readiness scanner, and `epoll` over the same scanner and wait. These calls report which descriptors can make progress without waiting. They do not copy file, terminal, pipe, or socket payloads. `eventfd` and `timerfd` are descriptors whose readiness these calls report, a counter and a timer.

## Readiness

Each descriptor reports a subset of the requested `POLLIN`, `POLLOUT`, and `POLLPRI` bits. `POLLERR`, `POLLHUP`, and `POLLNVAL` are returned without being requested.

An operations-backed descriptor calls its `file_ops.poll` method and supplies the wait queue whose wakeup can change its answer. Pipes, devfs files, procfs snapshots, and pseudoterminals use this path. The console calls the tty poller. Sockets call `socket_poll`. Regular KRFFS files and directories report requested read and write readiness without a wait queue.

The main backing rules are:

* A pipe uses its buffered byte count and peer lifetime. Its complete readiness contract is in the [pipe documentation](../ipc/Readme.md).
* A tty is readable when its line discipline has input ready and is always writable. Console readiness is restricted to the descriptor's access mode: `POLLIN` requires read access and `POLLOUT` requires write access. A read-only console descriptor polled only for output reports no readiness. A pseudoterminal also reports peer hangup.
* Datagram and protocol-bearing raw sockets are readable when a packet is queued and always writable. A listening stream socket is readable when an accepted connection is queued. A connected stream socket is readable with buffered data or a received FIN; it is writable in `ESTABLISHED` or `CLOSE_WAIT` while its send ring has space. Socket errors and a closed stream report `POLLERR` and `POLLHUP` as applicable. See the [network documentation](../network/Readme.md).
* Procfs snapshots report read readiness, including at EOF. Devfs character devices report requested read and write readiness.
* An eventfd is readable while its counter is not zero and writable while another 1 fits; a timerfd is readable while it has expirations to read; an epoll set is readable while a descriptor of its set is ready.

The framebuffer, mouse, and any remaining descriptor type use the fallback that reports requested read and write readiness immediately. Mouse readiness is not tied to packet availability, so `poll` cannot wait for a mouse packet.

For `poll`, a negative descriptor is ignored. A closed or out-of-range nonnegative descriptor receives `POLLNVAL` and contributes to the return count. The return value is the number of `pollfd` entries with a nonzero result.

## Scan and wait

The wait, `poll_wait`, takes a scan function, which `poll` and `select` give their descriptors and `epoll` its set. The core disables interrupts and scans every entry once. If one is ready, a signal that is neither blocked nor ignored is pending, or the deadline has passed, it returns. Otherwise it adds the process to every non-null wait queue, arms its timer deadline when needed, marks it blocked, and schedules another process.

A pipe operation, socket path, terminal path, or signal can wake the process. The 100 Hz timer wakes a finite wait at its deadline. After any wake, the process removes itself from every queue and scans again. The scan and all queue insertions happen while interrupts are disabled, so a readiness change cannot occur between the test and the sleep.

If no descriptor is ready, a pending signal that is neither blocked nor ignored returns `EINTR`; the kernel does not restart the call for `SA_RESTART`, nor an `epoll` wait, as Linux does not. A finite timeout is rounded up to a 10 ms tick, and the tick in progress is not counted, so a timeout never expires before the time asked, up to the limits below. A zero timeout performs one scan without waiting. `poll` with no entries and a finite timeout acts as a sleep. A negative `poll` timeout or a null timeout pointer for `select`, `_newselect`, `ppoll`, or `pselect6` waits indefinitely, until readiness or a pending signal that is neither blocked nor ignored.

Finite waits saturate without an error. `select` and `_newselect` convert any seconds value above 2147482 to 2147483647 milliseconds before tick rounding, about 24.86 days. `ppoll` and `pselect6` cap seconds at 21474836, round nanoseconds up to ticks, and cap the sum at 2147483646 ticks, about 248.55 days. Larger requested intervals are shortened. These kernel limits apply even when the timeout fits the 32-bit ABI; they are separate from musl's time64 fallback described below.

## `select` translation

`select` translates its read, write, and exception bitmaps into a dense `pollfd` array. One selected descriptor becomes one entry whose event bits record every set that contained it. The result is projected back as follows:

* `POLLIN`, `POLLHUP`, or `POLLERR` satisfies a requested read bit.
* `POLLOUT` or `POLLERR` satisfies a requested write bit.
* `POLLPRI` satisfies a requested exception bit.

Projection is limited to sets in which the caller selected the descriptor. A hangup on a write-only selection does not create a read result. The return value counts ready set memberships, so one descriptor ready for both read and write contributes two.

An `fd_set` has 1024 bits, matching the maximum number of descriptors a process may hold. `nfds` may be as large as 1024. Every selected descriptor below `nfds` must be open, or the call returns `EBADF`; `nfds` outside 0 through 1024 returns `EINVAL`.

## System-call ABI and limits

The implemented i386 calls are old packed-argument `select(82)`, `_newselect(142)`, `poll(168)`, `pselect6(308)`, and `ppoll(309)`; `epoll_create(254)`, `epoll_ctl(255)`, `epoll_wait(256)`, `epoll_pwait(319)`, `epoll_create1(329)` and `epoll_pwait2(441)`; `eventfd(323)` and `eventfd2(328)`; and `timerfd_create(322)`, `timerfd_settime(325)`, `timerfd_gettime(326)` and their time64 forms, 411 and 410. The `pselect6_time64(413)` and `ppoll_time64(414)` calls are absent and return `ENOSYS`.

The ported i386 musl calls `poll(168)` directly. It remaps ordinary `select` to `_newselect(142)`. `pselect` and `ppoll` use calls 308 and 309 while the timeout seconds fit in 32 bits. Only a larger timeout makes musl try call 413 or 414 before falling back to the 32-bit call with a clamped timeout.

`poll` accepts at most 1024 entries, the maximum number of descriptors a process may hold. Its descriptor, wait-queue and waiter arrays are allocated from the heap for each call. `select` reads and writes a complete 128-byte `fd_set` for each non-null set. `timeval` and `timespec` fields must be nonnegative and normalized; an invalid value returns `EINVAL`. This implementation does not write the remaining `select` time back to the caller's `timeval`.

`ppoll`, `pselect6` and `epoll_pwait` install the signal mask they are given for the wait (`poll_mask_set`); the old mask comes back when the wait ends, or, when a signal ended it, after that signal's handler runs. No backing reports `POLLPRI`, so an exception-only `select` cannot become ready.

## `epoll`

An epoll set (`epoll.c`) keeps registrations of a descriptor number and the open file it named when it was added, with the events it is watched for and a 64-bit word of data. A wait scans the set with the poll core's descriptor readiness and reports at most `maxevents` ready registrations with their data. A registration is level-triggered, or with `EPOLLET` edge-triggered: it is reported when an event appears in its readiness that was not there at its last report, or has gone since, or when its descriptor's wait queue has been woken since that report, which is how a pipe drained and filled again between two waits is reported again. A wait queue counts its wakes (`wakes` in `proc/wait_queue.h`), and the registration keeps the queue and the count of its last report; a wake for a change of another kind counts too, so an edge may be reported where Linux, whose wakeup callback looks at the events of the change, reports none. `EPOLLERR` and `EPOLLHUP` are reported whatever a registration asks, a registration of no events included. `EPOLLONESHOT` disables a registration at its first report, errors and hangups included, until `EPOLL_CTL_MOD`. A registration whose number is closed, or names another open file, is dropped at the next scan; Linux keeps it while its open file stays open. An open file is told from one made later at the same address by the serial number its description has (`struct open_file`). A regular file or directory cannot be added (`EPERM`). An epoll set inside another is reported ready by what it holds, but a change in it does not wake a wait on the outer set.

## `eventfd` and `timerfd`

An eventfd (`eventfd.c`) is a 64-bit counter: a write of eight bytes adds, a read takes the whole counter or one with `EFD_SEMAPHORE`, a read of zero waits, and a write that would carry the counter past `0xfffffffffffffffe` waits. A read takes its value before the copy to the caller, which can sleep in a page fault, so a second reader meanwhile finds it gone; a copy that fails loses the value, as in Linux. A read of an eventfd or a timerfd shorter than eight bytes is `EINVAL`, and `read` sends one of zero bytes to the descriptor's read method for that answer, ahead of the zero-length shortcut every other descriptor takes; `readv` of nothing returns 0, as Linux's does. A timerfd (`timerfd.c`) is armed with a first expiration, relative or absolute on its clock, and an interval; the timer tick (`timerfd_tick`, from the timer interrupt) counts expirations of the armed timers and wakes their readers, and a read of eight bytes returns the count since the last read. The read takes the count with interrupts off, since the tick adds to it, and before the copy to the caller, which can sleep: an expiration during the copy is kept for the next read, and a copy that fails loses the count, as in Linux. Times are 10 ms ticks: an expiration is rounded up and the tick in progress is not counted. A timer keeps its times as whole seconds and ticks below a second, since the kernel has no 64-bit division, up to 4294967295 seconds, about 136 years, taking a longer time as that; it waits a time longer than a span of 10737417 seconds, which keeps the signed comparison of ticks valid, as the part below a span and then whole spans, and `timerfd_gettime` reports the whole time. The descriptors of all three have type `FD_TYPE_ANON`, `/proc/PID/fd` names them `anon_inode:[eventfd]`, `anon_inode:[eventpoll]` and `anon_inode:[timerfd]`, and `fstat` reports mode 0600 with no type bits, as Linux's anonymous inodes.

## Verification

`make test` runs `tests/pollprobe.c`. It checks a timeout on two empty pipes, `POLLIN`, `POLLHUP`, `_newselect`, a child write waking a blocked poller, invalid selected descriptors, the 1024-bit `nfds` bound, and invalid `timeval` normalization. The same default suite runs the musl-linked `addusermusl`, `passwdmusl`, and `loginmusl` programs, which use libc `poll` while collecting data from child-process pipes.

`tests/run host` runs `tests/host/poll_select_hosttest.c` under AddressSanitizer and UndefinedBehaviorSanitizer. It checks bitmap word boundaries, dense translation, multi-set descriptors, array-capacity failure, error and hangup projection, direction gating, and ready-set counting.

`tests/musl/pollmusl.c` is built and staged at `/musl/pollmusl` for manual use. Its short timeouts exercise musl's direct `poll(168)` and `_newselect(142)` paths over a pipe, including timeout, readiness, and a cross-process wake. It does not exercise the time64 fallback.

`tests/musl/epollmusl.c` checks the eventfd counter and its semaphore form, and an epoll set's level-triggered, edge-triggered and one-shot reports, its data word, deletion, a closed descriptor dropped, the refusals, a child's write waking a wait, a registration of no events told of a hangup, the registration of a closed eventfd not reporting the one made next at its number, and `epoll_pwait2` with a timeout of 2^31 seconds, and with negative seconds, `EINVAL`. `tests/musl/timerfdmusl.c` checks a blocking read of an expiration, the interval reported back, a disarmed timer, a nonblocking read, an absolute time already past, a timer waking an epoll wait, and a timer and interval of 200 days reported whole.

The specialized guest targets extend socket coverage: `tests/run packet` checks `poll` and `select` over a packet socket, `tests/run raw` polls a raw socket, `tests/run netif` checks immediate `POLLERR` on an ioctl-only socket, and `tests/run dhcp` drives the in-house DHCP client's libc `poll` waits.
