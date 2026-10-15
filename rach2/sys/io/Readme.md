# `io/`: `poll`, `select`, `epoll`, `eventfd`, `timerfd`, POSIX timers and `inotify`

The `io/` directory has the system calls that wait until one of several descriptors is ready, `poll`, `select` and `epoll` with their signal-mask forms, and four kinds of descriptor that refer to no file, whose readiness those calls report: an eventfd, a 64-bit counter; a timerfd, a timer read through a descriptor; an epoll set; and an inotify instance, a queue of changes to watched files. POSIX timers share the timerfd's timer. Every wait runs in `poll_wait`. The calls follow Linux's ABI on both architectures, for programs such as Dropbear, whose session loop waits in `select`.

## Interfaces

### System calls

| Call                | i686 | x86-64 |
|---------------------|------|--------|
| `select` (old form) | 82   | none   |
| `_newselect`        | 142  | 23     |
| `poll`              | 168  | 7      |
| `pselect6`          | 308  | 270    |
| `ppoll`             | 309  | 271    |
| `epoll_create`      | 254  | 213    |
| `epoll_ctl`         | 255  | 233    |
| `epoll_wait`        | 256  | 232    |
| `epoll_pwait`       | 319  | 281    |
| `epoll_create1`     | 329  | 291    |
| `epoll_pwait2`      | 441  | 441    |
| `eventfd`           | 323  | 284    |
| `eventfd2`          | 328  | 290    |
| `timerfd_create`    | 322  | 283    |
| `timerfd_settime`   | 325  | 286    |
| `timerfd_gettime`   | 326  | 287    |
| `timerfd_gettime64` | 410  | none   |
| `timerfd_settime64` | 411  | none   |
| `timer_create`      | 259  | 222    |
| `timer_settime`     | 260  | 223    |
| `timer_gettime`     | 261  | 224    |
| `timer_getoverrun`  | 262  | 225    |
| `timer_delete`      | 263  | 226    |
| `timer_gettime64`   | 408  | none   |
| `timer_settime64`   | 409  | none   |
| `inotify_init`      | 291  | 253    |
| `inotify_add_watch` | 292  | 254    |
| `inotify_rm_watch`  | 293  | 255    |
| `inotify_init1`     | 332  | 294    |

`arch/i686/sysnr.h` and `arch/x86_64/sysnr.h` have the numbers, and each architecture's `systab.c` enters the handlers. x86-64's call 23 is named `select` and takes `_newselect`'s arguments; both reach `sys__newselect`. The old i686 `select` reads its five arguments through one pointer. On i686 a `long` is 32 bits and the `*_time64` calls take 64-bit times; x86-64's `long` is 64 bits, and it has no `*_time64` calls. i686 has no `pselect6_time64` (413) or `ppoll_time64` (414); those numbers return `ENOSYS`, and musl falls back to the 32-bit calls. `inotify_add_watch` resolves a path, so it is in `syscall_fs.c`, which passes the file's mount, inode number and kind to `inotify_watch`.

The manual gives the user contracts, errors, timeout limits and differences from Linux: [poll(2)](../../../man/man2/poll.2), [select(2)](../../../man/man2/select.2), [epoll(2)](../../../man/man2/epoll.2), [eventfd(2)](../../../man/man2/eventfd.2), [timerfd(2)](../../../man/man2/timerfd.2), [timer_create(2)](../../../man/man2/timer_create.2) and [inotify_init(2)](../../../man/man2/inotify_init.2).

### Inside the kernel

* `file_ops.poll` (`fs/file.h`) reports a backing's readiness: the requested `POLLIN` and `POLLOUT` that are true now, `POLLERR` and `POLLHUP` always, and the wait queue whose wake means the answer may have changed, or NULL. Without the method, `poll_fd` calls `socket_poll` for a network socket and `tty_poll` for the console, and reports a regular file, a directory, the framebuffer and the mouse ready at once. The rules of each backing are in [the ipc Readme](../ipc/Readme.md) for pipes, [the network Readme](../network/Readme.md) for sockets and poll(2) for the rest.
* A waker passes the kinds of change to `wq_wake`, `WQ_DATA`, `WQ_ROOM` or `WQ_HANGUP`, or all three through `wq_wake_all` for a close or a shutdown (`proc/wait_queue.h`). Each queue counts its wakes of each kind for edge-triggered epoll registrations; a wake of no kind is an edge for none.
* The timer interrupt calls `timerfd_tick` (`kernel.c`) and `sched_account_tick` calls `ptimer_cpu_tick` (`proc/sched.c`); exec (`syscall_proc.c`) and the exit of a process's last thread (`proc/exit.c`) call `ptimer_delete_all`. `syscall.c` has the clocks' part, `clock_now` and `clock_cpu_target`.
* `struct anon_obj` (`anon.h`) begins the object of every `FD_TYPE_ANON` descriptor: the eventfd, epoll, timerfd and inotify objects, the signalfd (`syscall_proc.c`), a POSIX message queue descriptor (`ipc/mqueue.c`) and a namespace file (`proc/ns.c`). `/proc/PID/fd` prints its name after `anon_inode:`, or alone for a namespace file; a message queue's link is instead `/NAME`, the path of its file in the mqueue file system, with ` (deleted)` once `mq_unlink` has taken the name (`fs/procfs.c`).

## Design

### The wait

`poll_wait` runs a scan function, which fills an array with the wait queue of each object it looked at and returns the number ready. It allocates a waiter record (`struct wq_waiter`) for each queue from the heap, so entering a queue allocates nothing, and loops with interrupts disabled. It returns the count if an object is ready, `-EINTR` if a signal that is neither blocked nor ignored is pending, or 0 at the deadline; otherwise it puts a record on every queue, sets `wakeup_tick` for a finite timeout, blocks, and after a wake takes its records off and scans again. The scan and the enqueue run with interrupts disabled, under the big lock every entry to the kernel takes (`proc/smp.h`), so no change of readiness falls between the test and the sleep. An epoll set adds a queue for each descriptor it watches. A scan that finds more queues than the wait has records, with nothing ready, returns 1 with the number in `grow`, and the caller waits again with that many and the same deadline, which keeps a tick `deadline_passed` moved it to.

A deadline (`struct poll_deadline`) is a tick of the 100 Hz timer and the end of the timeout by time since boot, the `CLOCK_MONOTONIC` reading at the call plus the timeout. `tick_deadline` ends a wait at the first tick at or after its end, counted from where in the tick in progress the call began (`rtc_tick_phase`), so without a lost tick a timeout ends at most a tick late; `deadline_reached` compares the signed difference, which the wrap of `kernel_ticks` does not disturb. At that tick `deadline_passed` reads the clock. A tick the timer lost while the processor could not take it leaves the count behind the clock, which keeps its last reading (`rtc_monotonic`), and the deadline then moves to the tick at which the count reaches the end, so a timeout never ends early by the clock. A `ppoll` or `pselect6` timeout of 21474836 seconds or more, which the tick clamps, ends at its tick alone. Each call converts its own timeout and saturates at the limit its manual page gives; on x86-64 `ppoll` and `pselect6` compare their 64-bit seconds with the limit before they narrow them. No wait here restarts after a signal (`syscall_restartable` in `syscall.c`). `poll_mask_set` installs the mask of `ppoll`, `pselect6`, `epoll_pwait` and `epoll_pwait2`; `poll_mask_done` restores the old one, or, when a signal ended the wait, signal delivery restores it after the handler.

### `poll` and `select`

`poll_scan` sets `revents` to 0 for a negative descriptor and to `POLLNVAL` for one that is not open or was opened with `O_PATH`, and counts the second as ready. It polls any other for its events and `POLLERR` and `POLLHUP`, and masks the answer to those and `POLLNVAL`. `poll` takes at most `FD_TABLE_LIMIT` (1024) entries.

`select` is `poll` over a translated array. `poll_select.c` turns the three `fd_set`s of `FDSET_BITS` (1024) bits into one `pollfd` per selected descriptor, with `POLLIN`, `POLLOUT` and `POLLPRI` for the sets it was in, and writes each result back into those sets alone: `POLLIN`, `POLLHUP` or `POLLERR` as readable, `POLLOUT` or `POLLERR` as writable, `POLLPRI` as exceptional. The scan counts a descriptor ready by the same rule (`pollfd_select_ready`). By `poll`'s rule, a descriptor selected only for writing that reports `POLLHUP` would end the wait with every set empty, and a program that calls `select` in a loop would spin. `poll_select.c` calls no other kernel code, so the host test compiles it unchanged.

### `epoll`

A set (`struct epoll`) has a list of registrations (`struct epitem`), its own wait queue, and `holders`, the registrations of the set in other sets. A registration keeps the number it was added by, the open file description that number had and the description's serial number, which tells it from a later description at the same address, the events, the data word, and the state of its reports (`off`, `seen`, `at`, `wakes`).

A wait (`epoll_wait_common`) runs `poll_wait` with `scan_set`, which polls each registration through the caller's descriptor at its number for its events and `EPOLLERR` and `EPOLLHUP` (`item_ready`, `target_poll`). When that descriptor is another description, or none, the registration reports nothing, and the scan frees it once its description is closed in every process (`fd_ofd_live`); a registration of a set lasts until the set's last close. The scan uses the caller's descriptors alone because the wait sleeps on the queues of what it polled, and only they keep those objects open while it sleeps. A registration that reports is taken at once: its report state changes and it moves to the end of the list, so waits with room for fewer events than are ready report the ready descriptors in turn, and a second waiter does not report an edge-triggered or one-shot event again. The copy to the caller follows the wait, since it can sleep in a page fault; an event that cannot be copied goes back to its registration (`untook`).

An edge-triggered registration is reported when an event is ready that was not at its last report or that a scan has found gone since, or when its descriptor's queue has counted a wake of a kind its events watch since that report (`kinds_of`): data for the input events, room for the output events, and a hangup always. A pipe drained and filled again between two waits is as ready as before, and its count of data wakes shows the edge; data that comes to a socket watched for output alone is no edge, as in Linux. `EPOLL_CTL_ADD` and `EPOLL_CTL_MOD` give a registration a new id and clear its report state.

Sets nest. `set_ready` reports whether a wait on a set would report an event, taking none, and adds the queues of the set and of what it watches to the wait's; `poll` and `select` use it too (`epoll_poll_set`). The set's count of wakes (`sig`) sums its own queue's and those its registrations watch, so a registration of a set sees a change anywhere inside it. A scan looks into each set once (`scan_marks`), which ends a cycle, and at most `EP_MAX_NESTS` (4) sets deep, which bounds the kernel stack a scan uses. `EPOLL_CTL_ADD` of a set fails with `ELOOP` when it would make a cycle or a chain of more than five sets (`nests_badly`), counted through every registration that lasts, whichever process made it, since a fork's child that closed a number shares the sets with its parent. A set's last close (`epoll_close`) frees its registrations and those of it in other sets.

### `eventfd`

An eventfd is a 64-bit counter and a wait queue. A read takes the value, the whole counter or 1 with `EFD_SEMAPHORE`, and wakes the writers before it copies the value to the caller, so a second reader during a copy that sleeps in a page fault finds it gone; a failed copy loses the value, as in Linux. A read wakes the queue for room, and a write of a nonzero value for data. `sys_read` sends a zero-byte read of an anonymous descriptor to its read method (`syscall_fs.c`), so a zero-byte read of an eventfd or a timerfd fails with `EINVAL`, as a short one does.

### Timers

`struct ktimer` is the timer of a timerfd and of a POSIX timer. An armed timer is on `armed`, which `timerfd_tick` walks at each tick, or, on a clock of processor time, on `cpu_armed`, which `ptimer_cpu_tick` walks. `timer_lock`, a spinlock taken with interrupts off after the big lock, covers both lists, each timer's count and arming, and each process's list of POSIX timers.

The kernel has no 64-bit division, so a time is whole seconds and ticks below a second, up to `TIME_MAX` seconds (4294967295, about 136 years). A setting rounds up to a tick and counts from `rtc_ticks_now() + 1`, leaving out the tick in progress, so a timer never expires early. A wait of `SPAN_SEC` (10737417) seconds or more runs as the part below a span and then whole spans, which keeps the signed comparison of ticks in `timerfd_tick` valid. At its tick a timer counts its expiration and the intervals it missed, is armed again or disarmed, and wakes its readers or posts its signal. A read takes the count under the lock before the copy, so an expiration during the copy is kept for the next read.

A process's POSIX timers are on `timers` in `struct tgroup`, by id from 0, at most `PTIMER_MAX` (64). `timer_create` stores the id in the caller's memory before it lists the timer, since the store can sleep in a page fault and the lock cannot be kept across it. A timer on a clock of processor time keeps a reference to the process it counts (`clock_cpu_target`), and its `due` is the ticks left, of which each tick charged to that process or thread takes one. `ptimer_fire` posts the signal with `SI_TIMER`, the timer's id, its value and its overrun where Linux's siginfo has them; while that signal is still pending, later expirations add to its overrun.

### `inotify`

An instance (`struct inotify`) has a queue of events and a wait queue. Every watch of the machine is on one list (`watches`) and is found by the base mount of its file system (`vfs_mount_base`) and the inode number, a key an open file description has too, so no hook needs a path. `inotify_watches` counts the watches, and every path to a hook tests it first, so a machine with no watch pays one comparison per call. `fs/vfs.c` reports what its namespace operations and `vfs_setattr` change (`inotify_child`, `inotify_file`, `inotify_gone`), and the descriptor calls in `syscall_fs.c` and `fs/fd.c` report reads, writes, truncations, opens and last closes through the open file description (`inotify_desc`). While a watch exists, `open_done` records in the description the directory and last component of the name it was opened by (`open_record_parent`), so the directory's watches hear of those events under that name.

`queue_event` drops an event equal to the last unread one, as Linux merges it. Past `MAX_QUEUED_EVENTS` (16384), or when the heap has no room, it queues the one `IN_Q_OVERFLOW` record kept in the instance, so the overflow needs no memory. A read takes each record off the queue before it copies it. An unmount gives each watch on the mount `IN_UNMOUNT` and `IN_IGNORED` and frees it (`inotify_unmount`), since a watch keeps no reference to its mount. A user may have `MAX_USER_INSTANCES` (128) instances and `MAX_USER_WATCHES` (8192) watches, Linux's defaults, which `/proc/sys/fs/inotify` reports.

## Limits

* No backing reports `POLLPRI`, so a wait for it alone ends only at its timeout or a signal.
* `select` and `pselect6` do not write the time left back.
* An epoll wait reports a registration only while the caller's descriptor at its number is the registered description.
* `EPOLLEXCLUSIVE` changes nothing: a wake wakes every waiter on a queue.
* `TFD_TIMER_CANCEL_ON_SET` changes nothing: a change of the real-time clock does not move an armed timer.
* A node of `/dev` or `/proc` can be watched with inotify and reports nothing.

## Tests

`tests/run host` compiles `poll_select_hosttest.c`, `poll_deadline_hosttest.c`, `epoll_hosttest.c`, `eventfd_hosttest.c`, `timerfd_hosttest.c` and `wait_queue_hosttest.c` from `tests/host/` natively under AddressSanitizer and UndefinedBehaviorSanitizer. [The host tests' Readme](../../../tests/host/Readme.md) gives their rows. `sh tests/host/deadline_mutate.sh` proves the deadline and wake-count rows with faults.

`tests/run default`, in the core tier, runs these on both architectures:

* `tests/pollprobe.c`: `poll` and `select` over pipes: a timeout, `POLLIN`, `POLLHUP`, `_newselect`, a child's write waking a poller, `select`'s errors, on x86-64 a `ppoll` of 2^32 seconds, and the console.
* `tests/musl/epollmusl.c`: the eventfd counter; epoll's level-triggered, edge-triggered and one-shot reports, nesting and `ELOOP`, a fork's child that closed a nested set's number, reports in turn, an event a wait could not store, and the edges of AF_UNIX, pipe, UDP, TCP and pseudoterminal wakes.
* `tests/musl/timerfdmusl.c`: reads, intervals, a disarmed timer, an absolute time already past, a timer waking an epoll wait, and settings of 200 days.
* `tests/musl/inotifymusl.c`: the events of a watched directory and a watched file through a file's life, reads, `FIONREAD`, `poll`, and the errors and flags of the watch calls.
* `tests/musl/callsmusl.c`: a POSIX timer's signal and overrun, a deleted id, and timers on clocks of processor time.

The faults in `tests/system/probe_faults.txt` for the files here are caught by those rows and by `limitprobe`; `sh tests/system/probe_faults.sh` runs them. The `packet`, `raw`, `netif` and `dhcp` suites poll sockets. `tests/musl/pollmusl.c` is staged as `/musl/pollmusl` for a person to run; no suite runs it.
