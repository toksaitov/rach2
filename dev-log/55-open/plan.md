# Milestone 55: the open items of milestone 54

## The goal

Every item that `dev-log/54-gnu/notes.md` leaves open after the two sittings on the Lenovo is fixed: the kernel does what the programs that met each gap asked of it, the programs a Debian system has and the GNU root lacks are ported, and the musl system stages the tools the notes found missing. The owner chose on 2026-10-02 to take every item, the ports and the namespaces among them.

## The items

From the first sitting's list and the second's, in `dev-log/54-gnu/notes.md`:

* `logger`'s messages go nowhere, since no syslog daemon runs, and syslogd cannot make `/dev/log` in the read-only `/dev`.
* `last` has no history and BusyBox's `w` lists no session: musl's login records and glibc's differ on i386, and util-linux's `last` and procps-ng's `w` are not ported.
* BusyBox's `ps` and `pgrep` lack `-p`, `-u` and `-c`; procps-ng and util-linux are not in the GNU root, so `watch -e`, `uptime -p`, `flock -w`, `vmstat`, `prlimit`, `ionice`, `lsblk`, `findmnt`, `column`, `script -e`, `sg` and `newgrp` are missing.
* zip, zstd, rsync, tmux, nano, strace, lsof, cron, info, GnuPG and Python are not ported.
* `mke2fs`, `mkdosfs` and BusyBox's `mkfs` applets are not staged.
* A route other than the default and the subnet's, a second address on an interface and an ARP entry added by hand are refused.
* System V IPC and POSIX message queues are not calls of the kernel.
* `unshare` answers `ENOSYS`, since the kernel has no namespaces, and the headless browser runs for an ordinary user only with `--no-sandbox`.
* `/sys/class/net` is missing.
* `timer_create` refuses the processor-time clocks.
* `fstat` of a descriptor open on a `/proc/PID` file reports root as its owner.
* `rtcwake` finds no wake alarm and no sleep state.
* iproute2's `rtstat` and `ctstat` find no `/proc/net/stat`.
* BusyBox's `devmem` finds no `/dev/mem`.

## Decisions

* The ports follow `vendor/gnu`'s rules: a pinned tarball checked by SHA-256 whose digest was verified against the upstream's signature, a recipe in `vendor/gnu/ports`, the upstream manual pages, and a row of the gnu suite for each. An upstream that signs nothing is taken from Debian's archive, whose signed `Release` file gives the digest of the source index that gives the tarball's.
* procps-ng and util-linux install into the GNU root. Their programs answer first by the path, as the GNU tools do; BusyBox's applets stay under `/bin/busybox` and on the musl system.
* The login records stay glibc's on the GNU root, which OpenSSH writes and util-linux's `last` and procps-ng's `w` read.
* The namespaces are what `unshare(1)` and Chromium's namespace sandbox use: user, PID, mount, network, UTS and IPC, with `setns` and the `/proc` files that map ids. The sandbox also needs a seccomp filter run by a classic BPF interpreter, `PR_SET_NO_NEW_PRIVS` and `SIGSYS` with its siginfo, which come with them.
* An interface holds a list of addresses, the stack a routing table of prefixes with gateways and metrics, and the neighbour cache permanent entries beside the learned ones.
* The sleep state is suspend to idle (`freeze`): the processes stop and the machine waits in its idle loop for a wake event, the RTC's alarm among them. ACPI's S3 is not attempted.

## How the work runs

* In batches, as the Batches section of `AGENTS.md` describes, each checked before the next starts: compile and style, the review by hand (Codex is not used before 2026-10-04, by the owner's quota), the build from clean with `tests/run all` and the mutation tests of new checks.
* A program that meets a gap of the kernel while it is ported gets the gap fixed in the same batch.

## Phases

### Phase 1: the kernel's small gaps

`fstat`'s owner of `/proc/PID` files; `/sys/class/net`; `/dev/mem`; `/dev/log` and a syslog daemon at boot; the RTC's alarm and suspend to idle; `timer_create` on the processor-time clocks; `/proc/self/mountinfo` and `ioprio_get` and `ioprio_set`, which util-linux reads; the staged `mke2fs` and `mkfs` applets.

### Phase 2: inter-process communication

System V shared memory, semaphores and message queues, through `ipc` and the direct calls, with `/proc/sysvipc`; POSIX message queues with their descriptors, `mq_notify` and `poll`.

### Phase 3: the network's tables

Several addresses on an interface, a routing table with `ip route add` and `del`, and permanent neighbour entries, with the route socket and the ioctls answering for them; and `/proc/net/stat`, whose counters are those tables'.

### Phase 4: the system's tools

procps-ng and util-linux, and with them `last`, `w` and the programs the BusyBox applets lack; and `/sys/block` and `/sys/dev/block`, which `lsblk` reads, made to what the ported `lsblk` asks.

### Phase 5: the small ports

zstd, zip and unzip, rsync, nano, strace, lsof, texinfo's `info`, cron, and tmux with libevent.

### Phase 6: Python and GnuPG

Python with what its standard library builds on, and GnuPG with its libraries.

### Phase 7: namespaces and the sandbox

`unshare`, `setns` and the `clone` flags for the six namespaces, seccomp filters, and Chromium running its sandbox for an ordinary user.

### Phase 8: the close

The GNU root rebuilt and installed on the Lenovo, each item tried there as a user, the last full set, and the record.
