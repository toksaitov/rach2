# The agents' home

Milestone 67 makes Rach2 a better home for the coding agents that already run on it, and adds a fifth, OpenCode. A user who signs in to the Lenovo finds `opencode` beside `claude`, `codex`, `agy` and `muse`; `git status` on a large tree answers at once instead of in seconds; a build sees a source edited in the same second; Claude Code's and Muse's sandboxed shells really are sandboxed; Node, npm, pip, rg, fd and shellcheck are there; the machine takes an IPv6 address from the network and speaks IPv6 as well as IPv4; term leaves no colour behind a program that exits and rings its bell as the owner sets it; and a reinstall keeps the agents' logins. The work comes from the four agents' own reports of 2026-11-01, in `dev-log/66-terminal/agents/`.

## The owner's decisions

* OpenCode: pin v2.2.5, the current line, not the legacy v1.19.5 that the vendor's installer still serves. Keep v2's background service as the vendor ships it (unmodified is the goal), document `opencode service stop`, and use `--standalone` in the tests.
* The new tools (Node, ripgrep, fd, shellcheck, ninja, git-lfs) go in the agents layer as official x86-64 binaries, beside their users; pip comes from the GNU root's own Python.
* Full IPv6, not loopback alone: addresses, ICMPv6, neighbour discovery, router advertisements and SLAAC, MLD, routing, fragments, TCP and UDP over IPv6, dual-stack sockets, netlink, procfs and the tools. The owner's words: a serious OS needs a serious networking stack. The Lenovo's network advertises a unique-local prefix (`fd91:381e:e667:4a0c::/64`, seen on the Mac's en0) and has no global IPv6 route, so the proof is on the LAN, and programs must fall back to IPv4 for the Internet.
* term's bell: never a flash by default. A setting with four values, audible, visual, both and none; the default is a short, soft beep (as the Linux console's, shorter), and visual is there for whoever wants it. Until term can sound one, the Lenovo's installed system has `set bell-style none` in `/etc/inputrc`, `set belloff=all` in `/usr/share/vim/vimrc` and `LESS=-q` in `/etc/profile.d/bell.sh`, set by the chat on 2026-11-01; the next install replaces them with the image's files.
* A license, the owner's decision of 2026-11-01: GPLv2 for all of Rach2's own code, GPL-2.0-only, with a syscall note as Linux's; the kernel derives in part from Linux's r8169, which is GPL-2.0-only. No SPDX sweep.
* The vDSO and ASLR are in, ordered last; they are the first to cut if the milestone must be shorter.
* Set aside: a package manager, perf_event_open, cgroups, io_uring, bpf, the new mount API, userfaultfd, futex_waitv, transparent huge pages, Python with PGO and LTO, tkinter, Landlock (the pinned Codex sandboxes with its bundled bubblewrap, `vendor/gnu/agents/packages/codex.sh:6`), openat2, membarrier, execveat, O_TMPFILE, user xattrs, rseq, `net.core.somaxconn` (`listen` ignores its backlog, `syscall_net.c:1207`), and lock scaling across CPUs, which the one big kernel lock (`proc/smp.c:38`) decides and which goes in a Limits section instead.
* Rejected as reported: seccomp strict mode killing on exit_group (Muse; Linux allows only `exit`), `/dev/stdin` naming the wrong descriptors (Muse; `synthfs.c:537` is right), and "chown emits nothing" (Claude Code; the probe changed nothing, and `syscall_fs.c:7886` rightly skips it).

## Measured

* The reports' figures, measured on the Lenovo under milestone 66's trial system: stat 132-190 us on ext2 against 4.6 us on tmpfs; git status on 4,857 files 2.0-2.6 s; 2,000 pthread_create 4.6-5.1 s; 500 Python threads woken 31 s; clock_gettime 1.6 us; sleep(1 ms) 7-10 ms. Each task measures its own number again on the Lenovo before and after; a test judges the kernel's counters, never the clock.
* In milestones 65 and 66 a task of 3 to 7 subtasks took a loop 10 to 60 minutes; builds, boots, the GNU root's ports and the release took the most time.

## The split

Forty-one tasks for nine loops, which take them from one queue with milestone 68's twenty. Twenty-three start at once. The longest chains are 6, 8, 19 and the Lenovo's 23a (wakeups, the tick, the vDSO, then the kernel's figures on the machine), and 18a, 18b, 18e and 23b, and 18a, 18b, 18d and 18f (IPv6's core, neighbour discovery, then the cross-host proof or the tools and the transports' files): each 4.25 to 6 hours of loop time. 7a and 18a are the riskiest and head the queue.

Before any task, the manager makes one commit of groundwork, which changes no behaviour, on 67's branch, and cuts 68's branch from it, so that both milestones have it and 68's rebase onto the released 67 keeps it: `TICK_HZ` moves from `platform.c:58` to `timer.h`, so that 67's 7b, 8 and 17 and 68's 1c and 4 count by it; and `procfs_gen_audio` and `audio_field` move from `procfs.c:4813-4823` to a new `rach2/sys/audio/audio_proc.c`, with `hda_hosttest.c` including `audio_proc.c` in place of `procfs.c` and `synthfs.c` (`hda_hosttest.c:56,125`), so that 67's additions to those two files cannot break 68's host test.

A task labelled `linux-host` builds the GNU layer, which builds only on an x86-64 Linux host (`vendor/gnu/Readme.md:150`), and `board.sh next` offers it only to a loop that can: the team's five and the owner's Codex loop, not the manager's three on the Mac. In 67 they are 1a, 1b, 2, 3, 12b, 12c, 13a, 13b, 13c and 23c. A task that changes a GNU port builds that port alone in its copy with `build_port.sh NAME`, as the GNU Readme's Updating says, before it closes; a change to `sources.txt` rebuilds every port at the end runs (`vendor/gnu/Readme.md:146`).

Appends to the same list (`rach2/sources.mk`, the manifests, `vendor/gnu/sources.txt`, `man/programs.md`, the system tables) conflict on a rebase when two tasks append at the same place; the worker resolves them by keeping both, which is expected, not a bug.

Each task's Done is shown in QEMU or by the kernel's counters on any machine. The Lenovo's figures and proofs are collected in three owner-only tasks at the end, 23a to 23c, each waiting only on the tasks it measures, so that no task waits for the machine and the machine, which one task holds at a time by the `machine` skill's lock, is taken in a few sittings instead of fifteen. The Lenovo's figures before the change are the reports' (Measured).

Shared files are split by function, and each function has one owner, across both milestones:

* `syscall_fs.c`: 9 select, 10 the setattr callers, 12a mounts, 14 copy, lseek and ioctl, 18c `readv_dgram`.
* `syscall_proc.c`: 7a `fork_nproc`, 9 `rlimit_set`'s descriptor limit, 12a `prctl`, `capset` and the capabilities `exec_ids` gives (`:7441`, `:1456`), 15 clone3, getrusage and mmap populate, 16 and then 19 `exec_build`, 8 the tick conversion at `:2610`. `process.c`'s `alloc_process` (`:106`): 7a, then 9 its initial limits.
* `procfs.c`: 8 the stat fields, 10 the sysctl writes, 18d `if_inet6` and `ipv6_route`, 18f `tcp6`, `udp6`, `raw6` and `snmp6`, 11b the topology's files, 11a the rest. The groundwork moves `/proc/audio`'s generator out, to 68.
* `synthfs.c`: 10 the sysctl table and its entries, 10b the entries for 9 and 16, 18d the `net.ipv6` entries, 11a `ns` readdir.
* `ext2.c`: 4a `node_for`, 5 inode times and feature admission.
* `vfs.c`: 4b lookup and unmount, 10 `vfs_setattr` (`:1785`), 12a mount and detach after 4b. 4b's cache checks permissions on every hit, so it adds nothing to `vfs_setattr`.
* `futex.c`: 6, then 8. `mm/vm.c`: 7a the gap search, 10 `VMA_LIMIT`, 16 the mmap base. `process.h`: 7a, 9 `fd_held`.
* `netlink.c`: 17 unix diag, 18d addresses, routes and neighbours, 18f inet_diag.
* `tcp.c`, `tcp.h` and `udp.c`: 17, then 18c; 17 owns the timeout constants (`tcp.h:68-72`) and writes them in milliseconds through `TICK_HZ`, so 8 leaves `tcp.h` alone. `raw.c`: 18a its IPv6 side, 18c the queue's width, 18e the filter.
* `ethernet.c`, `netdev.h`, `netconf.c`, the e1000 and ip.c's loopback ring: 18a; fragments, extension headers and the path MTU: 18a2. `route.c` and the neighbour cache: 18b; 18a declares the IPv6 route lookup, answering `::1` and link-local, and 18b gives it the table.
* `syscall_net.c` and `socket.c/.h`: 18a `socket_kind`'s admission of AF_INET6 raw ICMPv6 and of packet type 0x86DD (`syscall_net.c:384`) and `utils/pingcall.h`'s address; 18c the rest, then 18e the options.
* `utils/netup.c`, `netcfg.c` and the manifests' `/etc/hosts`: 18d.
* `term/` and libtsm's patches: 21a the alternate screen (`tsm-vte.c` modes 1049 and 1048, `tsm-screen.c`, `term/draw.c`), 21b the bell (`tsm-vte.c` `do_csi`, `term/main.c`, `screen.c`, `term.h`, `/etc/term.conf`). Each is its own patch in the series; whichever lands second makes its patch again on top of the first's, and both apply together with the build's `-F 0`.
* `syscall.c`: 8 sleeps and times, 21b KIOCSOUND and KDMKTONE; 68 changes nothing there. `speaker.c`: 21b adds a divisor entry beside `speaker_on(hz)`, which keeps its unit. `ports/doom/i_pcsound.c`: 21b `play_tone` alone; the rest is 68's.
* The tick in `rach2/kernel.c`: 8; 18b and 21b each append one call.
* `agent_native.c`: 2. `vendor/gnu/agents/Readme.md`: 1a owns Limits; 1b, 12b and 13a add rows and Design sections.
* `Readme.md`'s license sentence, `man/programs.md`'s two license sentences, Doom's `sha1.c` and its Readme's license section: 22.

The system tables, manifests, `man/programs.md`'s rows and `vendor/gnu/sources.txt` take appends from any task.

| Task | Waits on | Owns | Loop time |
|---|---|---|---|
| 1a OpenCode package (linux-host) | none | `vendor/gnu/agents/packages/opencode.sh`, `opencode.1`, the agents Readme's Limits | 20-40 min |
| 1b ripgrep package (linux-host) | none | `vendor/gnu/agents/packages/ripgrep.sh`, `rg.1` | 15-30 min |
| 2 OpenCode in the agents suite (linux-host) | 1a, 1b | suite rows, fake server script, `agent_native.c`, the digest rule in `mk/gnu-agents.mk` | 45-75 min |
| 3 OpenCode acceptance and credential sources (linux-host) | 1a | `agents_accept.sh`, `agents_guest.sh`, `agent_accept.c` | 45-75 min |
| 4a The block cache's and ext2's hashes | none | `bcache.c`, `ext2.c` `node_for` | 30-45 min |
| 4b The name cache | none | `vfs.c` lookup and unmount, new `dcache.c` | 60-90 min |
| 5 ext2 nanosecond and post-2038 times | none | `ext2.c` inode load, encode and feature admission, `mkext2.sh`, host test | 45-75 min |
| 6 Wakeups: futex, init, klog | none | `futex.c`, `utils/init.c` reaping, klog stamps | 45-60 min |
| 7a Process table and counts | none | `process.c/.h`, `syscall_proc.c` `fork_nproc`, `mm/vm.c` gap search | 60-90 min |
| 7b Per-CPU ready lists | 7a | `sched.c` | 60-90 min |
| 8 The tick at 1000 Hz and a 64-bit tick count | 6 | `platform.c`, `pit.c`, `clock.c`, `timer.h`, `timerfd.c`, `syscall.c` sleeps, times and getres, `kernel.c`'s second count (`:283`), the tick conversions in `futex.c`, `poll.c`, `request.c`, `msc.c`, `syscall_proc.c:2610`, `procfs.c` stat fields | 75-105 min |
| 9 Descriptors and limits | 7a | `fd.c/.h`, `process.h` `fd_held`, `alloc_process`'s initial limits, `poll.c` scans, select, `rlimit_set`'s descriptor limit, `su.c`, `sudo.c`, the limits manifest line | 75-105 min |
| 10 sysctl, inotify to parents, overflowuid | none | `synthfs.c` sysctl table, `procfs.c` sysctl writes, `inotify.c` limits, `vm.c` `VMA_LIMIT`, the setattr callers in `syscall_fs.c` | 45-60 min |
| 10b sysctl entries for descriptors and ASLR | 9, 10, 16 | `fs.nr_open`, `fs.file-max`, `kernel.randomize_va_space` in 10's table | 15-30 min |
| 11a procfs | none | `procfs.c` status, snapshots, maps; `synthfs.c` `ns` readdir | 45-60 min |
| 11b CPU topology | none | `cpuinfo.c`, sysfs topology | 30-45 min |
| 12a Sandbox system calls | 4b | `syscall_fs.c` mounts, `vfs.c` mount and detach, `ns.c`, `syscall_proc.c` `prctl`, `capset` and exec's capabilities | 75-105 min |
| 12b Agents' sandboxes (linux-host) | 10, 11a, 12a, 12c | the agents' sandbox settings, man pages, the agents Readme's Design rows | 45-60 min |
| 12c bubblewrap and socat ports (linux-host) | none | `vendor/gnu/ports/bubblewrap.sh`, `socat.sh`, their pins and pages | 30-60 min |
| 13a Developer tools (linux-host) | none | agents-layer recipes for Node, fd, shellcheck, ninja, git-lfs | 45-60 min |
| 13b pip (linux-host) | none | `vendor/gnu/ports/python.sh` | 20-40 min |
| 13c /usr/local and Chromium (linux-host) | none | `install_root.sh` | 30-60 min |
| 14 File system calls | none | `syscall_fs.c` copy, lseek and ioctl sections; `pty.c` | 60-90 min |
| 15 Process system calls | none | `syscall_proc.c` clone3, getrusage, mmap populate; new pidfd file | 60-90 min |
| 16 ASLR | none | `syscall_proc.c` `exec_build`, `vm.c` mmap base, `elf.c` | 60-75 min |
| 17 TCP close and unix diag | none | `tcp.c`, `tcp.h` RTO and orphan timer, `netlink.c` unix diag | 45-75 min |
| 18a IPv6 core | none | new `network/ip6/` (input, output, ICMPv6 echo and errors, the route and path MTU lookups' interfaces), `ethernet.c`, `netdev.h` addresses and groups, `netconf.c`, ip.c's loopback ring, the e1000's multicast bit, `raw.c` IPv6 side, `socket_kind`, `pingcall.h`, `utils/ping.c -6` | 60-90 min |
| 18a2 Fragments, extension headers and the path MTU | 18a | `network/ip6/` fragments, extension headers, Packet Too Big and the path MTU per destination | 45-75 min |
| 18b Neighbour discovery, SLAAC and MLD | 18a | new `nd6.c` and `mld6.c` with their own queue and cache, `route.c` IPv6 table, address expiry hooks | 90-120 min |
| 18c IPv6 transports and sockets | 17, 18a | the address width in `socket.h`, `tcp.h/.c`, `udp.c`, `raw.c` and `readv_dgram`; AF_INET6 in `syscall_net.c` | 90-120 min |
| 18d IPv6 addresses in netlink, start-up and tools | 10, 18b | `netlink.c` family-aware address, route and neighbour dumps, `if_inet6`, `ipv6_route`, `net.ipv6` entries, `netup.c`, `netcfg.c`, `/etc/hosts`, BusyBox `udhcpc6`, `network/Readme.md`, man pages | 60-90 min |
| 18e IPv6 options, raw filter, the network proof in QEMU | 18a2, 18b, 18c | `IPV6_*` options and ancillary data in `syscall_net.c`, the ICMPv6 filter in `raw.c` | 60-90 min |
| 18f IPv6 sockets in inet_diag and /proc/net | 18c, 18d | `netlink.c` inet_diag for IPv6, `tcp6`, `udp6`, `raw6`, `snmp6` | 45-60 min |
| 19 The vDSO on x86-64 | 8, 16 | TSC calibration, new vDSO image and vvar page, auxv, maps | 90-120 min |
| 20 Logins kept across a reinstall | none | `utils/rach2-install`, `tools/dev/machine_logins.sh`, the machine image's staging | 45-60 min |
| 21a term: colours after the alternate screen | none | the libtsm patch for modes 1049 and 1048, `term/draw.c` | 45-75 min |
| 21b term's and the console's bell | none | the libtsm patch for `CSI 10;n]` and `CSI 11;n]`, `term/main.c`, `screen.c`, `term.h`, `/etc/term.conf`, `term.1`; `syscall.c` KIOCSOUND and KDMKTONE, `speaker.c`, one call in the tick, Doom's `play_tone` | 60-90 min |
| 22 License | none | `COPYING`, `LICENSES/`, `Readme.md`'s license sentence, `man/programs.md`'s license sentences, `rtl8168.c/.h`'s header | 15-30 min |
| 23a The kernel's figures on the Lenovo (owner-only) | 4a, 4b, 6, 7a, 7b, 8, 9, 11b, 19 | `dev-log/67-agents/lenovo.md` | 60-90 min |
| 23b IPv6 on the Lenovo (owner-only) | 18a2, 18b, 18e | the same record | 45-60 min |
| 23c The agents and term on the Lenovo (owner-only, linux-host) | 3, 5, 12b, 13a, 20, 21a, 21b | the same record; the root it installs | 60-105 min with the owner |

Sizes: 34 to 51 loop-hours of work in all.

## Tasks

Each task rewrites the Readme and pages of every contract it changes. Besides those named: the process and scheduler Readme for 7a, 7b and 15; the file system and VFS Readmes for 4a, 4b, 5, 9, 10 and 14; the namespace and memory Readmes for 12a and 16; the pages of the calls of 9, 12a, 14, 15, 16 and 19; `rach2-install(8)` and the recovery document for 20.

### 1a. OpenCode package

* `vendor/gnu/agents/packages/opencode.sh`: v2.2.5 `opencode-linux-x64-baseline.tar.gz`, its digest in FILES (`09f44302...614e`, which the release's `SHA256SUMS` lists); `/opt/opencode`, `/usr/bin/opencode`, `/etc/profile.d/opencode.sh` with `OPENCODE_DISABLE_AUTOUPDATE=1`; a Bun check skipped off x86 as `claude.sh:75-83` does.
* `man/man1/opencode.1`: sign-in over SSH (device code, API key; ChatGPT through the device-code method), where credentials live, the service and `--standalone`, no sandbox under `--auto`.
* The agents Readme: table, Design, Limits (this task owns that section), Updating (the pin moves fast).
* Done: the recipe builds and its checks run; `opencode --version` runs in QEMU.

### 1b. ripgrep package

* `vendor/gnu/agents/packages/ripgrep.sh`: ripgrep 15.2.0, the musl tarball's digest in FILES as `gh.sh` and `codex.sh` pin theirs, licenses as `codex.sh` pins them; `/usr/bin/rg`; `rg.1`; the agents Readme's row.
* Done: the recipe builds; `rg --version` runs in QEMU.

### 2. OpenCode in the agents suite

* `tests/system/agents/opencode.sh`: version, path, license, `rg`, the extracted libraries mapped from TMPDIR and matching the pinned package, the client's and the server's memory and start time sampled, and the service's life (`--standalone` leaves nothing; a default run leaves one service that `opencode service stop` ends).
* `tests/system/agents/opencode-run.sh` with `tests/system/fakeapi/opencode.json`: `opencode run --standalone --auto` reads, searches the tree with FFF on, runs a script, edits and commits against the fake server, with `OPENCODE_DISABLE_MODELS_FETCH=1`, and makes no request the script does not expect.
* FFF's LMDB under contention and after its owner is killed; the watchers on a tree near the inotify limit. A kernel fault they show is a bug that task 2 fixes on the spot, and `OPENCODE_DISABLE_FFF=1` is the fallback.
* `tests/cases/machine/agent_native.c` (`:291,517-534,726,760` take an unknown agent for Muse or Codex) and a digest rule in `mk/gnu-agents.mk`; `tests/cases/machine.agents.json`, `tests/native/rows.cases`.
* Done: both rows behave as written in QEMU.

### 3. OpenCode acceptance and credential sources

* The acceptance digests each agent's credential file and strips every long token run of it from the screens (`agents_accept.sh:686-689,733-737`, `agents_guest.sh:150-193`); v2's `opencode.db` is a SQLite store in WAL mode that every session writes. Give each agent a credential source, a command that prints its secrets (for opencode, a consistent read of the auth records), and digest and redact that output, binary-safe.
* `agents_accept.sh`, `agents_guest.sh`, `agent_accept.c` screen strings measured on the first run, `machine.agentsaccept.json`, `accept/agents/opencode.json`, and the Readme paragraph on credentials (`vendor/gnu/agents/Readme.md:20`).
* Done: in QEMU the TUI draws in tmux over SSH, answers, survives a cancelled turn, keeps its sign-in across a reboot, and leaves no process after quitting with `--standalone`. The Lenovo's run is 23c's.

### 4a. The block cache's and ext2's hashes

* bcache: size the hash from the cache's limit (`bcache.c:118` has 4096 buckets for some 262k buffers) and bound the victim search at the cap (`bcache.c:711`).
* ext2: a hash for the mount's node list (`node_for`).
* Tests: the report's `syscall_latency.c` as a case that judges buffers and nodes searched per warm lookup.
* Done: the case passes in QEMU with the cache full. The Lenovo's figures are 23a's.

### 4b. The name cache

* VFS: a name cache with negative entries, bounded, invalidated at every mutator that already emits inotify, at rename and at unmount (`vfs.c:3925,3987`, which this task owns until it closes), and checked against permissions.
* Tests: the report's `fs_thread_scaling.c` as a case that judges directory reads per warm lookup, and rows for invalidation at each mutator and for a denied lookup that a cached entry must not let through.
* Done: the cases pass in QEMU. The Lenovo's `git status` figure is 23a's.

### 5. ext2 nanosecond and post-2038 times

* Read and write the 256-byte inode's extra fields with `i_extra_isize` checked (`ext2.c:405`, `:1677`); admit the extra_isize feature bit, which today makes the mount read-only (`ext2.c:5528`); keep 128-byte volumes working with their present limits.
* `mkext2.sh`: `-I 256` with extra_isize, and `E2FSPROGS_FAKE_TIME` from the commit's date instead of 1 (Muse's 1970 files).
* The ext2 host test, `ext2.Readme.md`, the man pages that state the limits.
* Done: `mtime_make.sh` rebuilds after a same-second edit; `touch -d 2040` holds across a remount; e2fsck is clean; an old image still mounts.

### 6. Wakeups: futex, init, klog

* `futex.c:281` wakes the whole bucket; give each waiter its own wait queue so a wake-one wakes one. Make `futex_ticks` and `futex_ticks_until` (`futex.c:84,103`) read `TICK_HZ`.
* `utils/init.c:2554` polls `waitpid` and sleeps a second; reap on SIGCHLD.
* The kernel log's fractions (`[6594.000073]`).
* Done: a case judges wakeups per wake-one by the kernel's count; an orphan is reaped within a tick, in QEMU. `py_thread_wakeup.py 500` on the Lenovo is 23a's.

### 7a. Process table and counts

* A pid table, per-user counts and a sleepers list in place of walks of `proc_list` (`process.c:133,242,300`, `syscall_proc.c:2122`); measure the VMA gap search (`mm/vm.c:358`) and fix it if it shows.
* The report's `thread_scaling.c` as a case judging work per creation, every `pthread_create` result checked.
* Done: the case passes in QEMU. The Lenovo's 2,000 `pthread_create` is 23a's.

### 7b. Per-CPU ready lists

* Ready lists per CPU in place of the process list as the run queue (`sched.c:24-26,112-152`), with stealing.
* Tests: a case with many blocked threads and a few runnable ones counts the entries a pick examines, which master's list walk (`sched.c:112`) makes grow with the blocked ones.
* Done: four CPUs scheduled in QEMU, each running work by the kernel's per-CPU counts, and the case's count stays flat as blocked threads are added. The Lenovo's 4-thread figure is 23a's.

### 8. The tick at 1000 Hz and a 64-bit tick count

* `TICK_HZ`, in `timer.h` since the groundwork, to 1000, with every place that assumes 100 converted: `timerfd.c:45`, `clock.c:73`, `syscall.c:2346`, `pit.c:155`, `kernel.c:283`, `futex.c` (after 6), `poll.c:388`, the storage deadlines (`request.c:134`, `msc.c:159`), `syscall_proc.c:2610`, and any other the first subtask's search of the tree finds; TCP's constants are 17's, and 68's HDA period and jack poll count by `TICK_HZ`.
* `kernel_ticks` widened to 64 bits (`timer.h:13`) with the accounting, the process deadlines and the wait queues that read it; `clock_t` kept at USER_HZ 100 where a program sees it (`times`, `/proc/PID/stat`); `times()` full width (`syscall.c:2299`); `clock_getres` the clock's real resolution.
* Done: in QEMU a 1 ms sleep takes one or two ticks by the kernel's count; `times` and `/proc/stat` in Linux's units. The Lenovo's `sleep(0.001)` is 23a's.

### 9. Descriptors and limits

* Let root raise RLIMIT_NOFILE to a kernel nr_open (1,048,576) and give init's children a hard 524,288: a table that grows (`fd.c:347`) and stays one contiguous array, reallocated and copied under the table's lock, so that the 70 places outside `fd.c` that index `entries[]` (`procfs.c:4798` among them) stay as they are, after a check that none keeps a slot's address across a sleep; no per-thread bitmap sized to the maximum (`process.h:354`), bounded scans in `poll.c` and select, `rlimit_set`'s descriptor limit (`syscall_proc.c:5371-5393`) and the initial limits in `alloc_process` (`process.c:141`), after 7a's change to it.
* The variables behind `fs.nr_open` and `fs.file-max`, which 10b enters in the table.
* RLIMIT_NPROC: the line in `krffs/manifests/os-real-gnu.manifest:150`.
* Done: `prlimit`, `ulimit -n` and limitprobe show high descriptors and shared tables working in QEMU.

### 10. sysctl, inotify to parents, overflowuid

* The sysctl table (`synthfs.c:190-240`) and its writes (`procfs.c:5324`), each entry backed by a variable its owner reads: `fs.inotify.*` (`inotify.c:29`), `vm.max_map_count` (`vm.c:119`), `kernel.random.boot_id` and `uuid` (read-only, generated), `kernel.overflowuid` and `overflowgid` (65534), which bubblewrap reads before it parses its arguments. 10b and 18d add theirs.
* IN_ATTRIB to the parent's watch from chmod, chown and utimensat (`vfs.c:1807`).
* Done: `sysctl -w` sticks and takes effect; `inotify_attrib.py` passes.

### 10b. sysctl entries for descriptors and ASLR

* `fs.nr_open` and `fs.file-max` over task 9's variables, `kernel.randomize_va_space` over task 16's, in task 10's table.
* Done: `sysctl` reads and writes each, and a write takes effect, in QEMU.

### 11a. procfs

* `Cpus_allowed` from the real mask (`procfs.c:1561`); a file regenerated when read again from offset 0 (`:4597`); `/proc/PID/ns` listed by readdir (`synth_open_dir`, and the chdir case); maps with device and inode (`:1245`).
* Done: `ls /proc/self/ns` lists six; pread at 0 sees fresh maps.

### 11b. CPU topology

* cpuinfo's cores and siblings from CPUID 0xB and 0x4 (`cpuinfo.c:310`); `/sys/devices/system/cpu/cpuN/topology`.
* Done: `lscpu` in QEMU with `-smp 2,cores=2` shows one socket and two cores. The Lenovo's two threads are 23a's.

### 12a. Sandbox system calls

* `pivot_root`, `umount2` with `MNT_DETACH`, proc mounted anywhere, `prctl` capability bounding reads and drops.
* Capabilities are all or none today, and exec gives them back (`syscall_proc.c:7441`, `:1456`): a bounding set per process, inherited across fork, that exec's capabilities are masked by, and `capset` of single capabilities.
* Tests that check denial as well as success, in C: a write outside a pivoted root fails, a detached mount is gone from every view, and a dropped capability does not come back through fork and exec.
* Done: the cases pass in QEMU.

### 12b. Agents' sandboxes

* `bwrap --unshare-all --proc /proc` runs and confines: a write outside the sandbox fails, the network is gone under `--unshare-net`.
* Each agent's own sandbox invocation, as it runs it: Claude Code's managed sandbox, Codex's `sandbox_mode` (`codex.sh:74`) with the bubblewrap it bundles; Muse's default mode left as shipped; the man pages and the agents Readme's Design rows.
* Done: in QEMU bubblewrap confines, and Claude Code's sandboxed Bash runs under it. The agents' own runs on the Lenovo are 23c's.

### 12c. bubblewrap and socat ports

* The bubblewrap 0.8.0 port (autotools) and the socat port, each pinned, built in the task, with their pages.
* Done: `bwrap --version` and `socat -V` run in QEMU.

### 13a. Developer tools

* Agents-layer recipes: Node LTS with npm and npx, fd, shellcheck, ninja, git-lfs, each pinned with its license; man pages and `man/programs.md` rows.
* Done: each `--version` runs in QEMU.

### 13b. pip

* `python.sh`: install pip; check the build-host paths `sysconfig` reports.
* Done: `python3 -m pip install` of a pure-Python wheel from a local file works outside a venv in QEMU.

### 13c. /usr/local and Chromium

* `install_root.sh`: `/usr/local` and its `bin`; the Chromium launcher as a link (made near `install_root.sh:213`); then one bounded trace of Chromium's children that outlive a killed browser, fixed if it shows a cause.
* Done: killing the launched Chromium PID ends the browser and its children in QEMU.

### 14. File system calls

* sendfile and splice to and from regular files with their pipe contracts kept, tee between pipes only, copy_file_range by a generic copy (`syscall_fs.c:9683`); faccessat2; SEEK_DATA and SEEK_HOLE; FICLONE's errno; a pty master's read gives EIO once the slave hangs up and the buffer drains (`pty.c:213`).
* Done: `misc_syscalls.py`'s sendfile, splice, copy_file_range, SEEK_DATA, SEEK_HOLE and FICLONE lines pass, and its other lines give the errno this plan expects of them.

### 15. Process system calls

* clone3 with `CLONE_CLEAR_SIGHAND`, `CLONE_PIDFD` and the flags glibc's posix_spawn passes (`syscall_proc.c:2542`); pidfd_open, pidfd_send_signal and pidfd_getfd; getrusage fault counts (`:3187`); MAP_POPULATE honoured.
* Done: posix_spawn makes one clone call; a pidfd polls readable at exit; `/usr/bin/time` shows faults.

### 16. ASLR

* Random stack, mmap base, brk, PIE base and interpreter on x86-64, the variable behind `kernel.randomize_va_space` (10b enters it), `ADDR_NO_RANDOMIZE` honoured (`syscall_proc.c:1563,1584,7125`); i686 kept fixed where it has no room.
* Done: two runs differ; `setarch -R` gives equal addresses; gdb disables it as on Linux.

### 17. TCP close and unix diag

* A QEMU capture of the LAST_ACK stall first; then TCP's timeout constants (`tcp.h:68-72`) in milliseconds through `TICK_HZ`, RTO_MIN 200 ms, an orphan timeout, quieter logs.
* unix_diag messages sized and aligned (`netlink.c:364,1433`).
* Done: 100 curl downloads close with nothing in the log; `ss -xp` clean with Chromium running.

### 18a. IPv6 core

* `network/ip6/`: IPv6 input and output of unfragmented packets with no extension headers, ICMPv6 echo and errors.
* The interfaces of the IPv6 route lookup, answering `::1` and link-local destinations, which 18b gives a table, and of the path MTU, answering the link's, which 18a2 fills; 18c calls both.
* The interface: addresses in `struct netdev` (`netdev.h:163-173`) with scope, state and lifetimes, set through `netconf.c`; a link-local address made at link up; multicast group membership as an interface operation; `::1` on ip.c's loopback ring, which carries IPv4 alone today (`ip.c:431`); Ethernet type 0x86DD in dispatch and in the transmit check (`ethernet.c:49,86`); the IPv4 callers of routing and `ip_source` kept working (`ip.c:351,404`, `tcp.c:1501`, `syscall_net.c:1458`).
* The e1000 receives multicast (`E1000_RCTL_MPE`; it clears its table, `e1000.c:586`); the two Realtek drivers already pass every group.
* Raw ICMPv6 sockets: their IPv6 side in `raw.c`, their admission in `socket_kind` (`syscall_net.c:384` refuses AF_INET6 and packet type 0x86DD today), a 128-bit address in `utils/pingcall.h:20`, and `utils/ping.c -6`.
* The packet probe stops asserting IPv6's absence (`packetprobe.c:249,781`).
* Done: `ping -6 ::1` answers and a packet socket sees EtherType 0x86DD, in QEMU.

### 18a2. Fragments, extension headers and the path MTU

* Extension headers skipped or refused as RFC 8200 says, fragment reassembly with its limits and timeouts, fragmenting output, Packet Too Big and a path MTU per destination behind 18a's interface.
* Done: a fragmented echo over `::1` reassembles, an overlapping fragment is dropped, and a Packet Too Big lowers the destination's MTU, in QEMU.

### 18b. Neighbour discovery, SLAAC and MLD

* `nd6.c`: neighbour solicitation and advertisement, a neighbour cache and a bounded queue of its own (ARP's queue holds 32-bit next hops and emits IPv4, `arp.c:214`, `ip.c:175,226`), duplicate address detection, router solicitation and advertisements with prefixes and MTU, a default router list, SLAAC addresses stable by RFC 7217 from `net.ipv6.conf.*.stable_secret`, which 18d's start-up keeps across boots as Linux's does, and their lifetimes.
* `mld6.c`: MLDv2 reports, queries and leaves for every group joined.
* `route.c`: the IPv6 table behind 18a's lookup, longest prefix first, on-link prefixes and default routes from advertisements; a failed resolution and an address that expires or is removed reach their users, as `netconf.c:504` does for IPv4.
* `net.ipv6.conf.*` enforced: disable_ipv6, accept_ra, autoconf, forwarding, use_tempaddr, as variables that 18d enters in the table. The tick gets one call for ND's timers.
* Done: in QEMU the guest takes slirp's `fec0::/64` by SLAAC and pings `fe80::2` and `fec0::2`. The Lenovo's address is 23b's.

### 18c. IPv6 transports and sockets

* One address type of either family, with its scope id, through `struct socket` and its receive queues (`socket.h:61,81,138,187-190,250,305,310`), `struct tcb` and `tcp_row` (`tcp.h:116-119,366-369`), `tcp_find` (`tcp.c:1087,1102`), UDP's bindings (`udp.c:55,184,195`), raw's queue (`raw.c:28,85-88`) and `readv_dgram` (`syscall_fs.c:4699-4733`).
* TCP over IPv6: the 40-byte pseudo-header, MSS and PMTU without IPv4's sizes (`tcp.h:26`, `tcp.c:59,842`), an address that ends its lifetime (`tcp.c:1156`). UDP's checksum sent and verified, as IPv6 requires (`udp.c:158,248`).
* AF_INET6 in every socket call (`syscall_net.c:386`), v4-mapped addresses, `IPV6_V6ONLY`, binding conflicts across families, the ephemeral ports shared.
* A connect with no usable route fails at once, so programs fall back to IPv4.
* Done: in QEMU a server bound to `::` accepts IPv4 and IPv6 over loopback, TCP and UDP carry data over `::1` and a link-local address, and a connect to an address with no route fails at once.

### 18d. IPv6 addresses in netlink, start-up and tools

* Netlink by family: addresses, routes and neighbours for AF_INET6 (the dumps answer IPv4 to any family today, `netlink.c:1217-1238`), so iproute2's `ip -6 addr`, `ip -6 route` and `ip -6 neigh` work in the GNU root.
* `/proc/net/if_inet6` and `ipv6_route` (`synthfs.c:240` and `procfs.c`); the `net.ipv6` entries in task 10's table, over 18b's variables.
* Start-up: `netup.c` and `netcfg.c` bring IPv6 up beside DHCP, write the stable secret, made once and kept in `/var/lib`, before the link comes up, and read `inet6` stanzas (`netcfg_hosttest.c:139-147`); BusyBox `udhcpc6` enabled (`vendor/busybox/rach2.config:1016`) with a lease script, for networks that give addresses by DHCPv6 alone; `::1 localhost` in the three manifests' `/etc/hosts` (`os-real.manifest:151`, `os-real-gnu.manifest:159`, `grub-test.manifest:149`).
* `network/Readme.md` (it says "IPv4 only" and "no fragments", `:123-124`), ipv6(7), icmp6(7), and the pages of the calls that name a family.
* Done: in QEMU the tools show the guest's addresses, routes and neighbours.

### 18e. IPv6 options, raw filter, the network proof in QEMU

* The common `IPV6_*` options (hop limits, multicast, `IPV6_RECVPKTINFO`, `IPV6_TCLASS`) with their ancillary data (`recvmsg` drops control data today, `syscall_net.c:1890`), and raw ICMPv6's filter.
* Done: the options' cases pass in QEMU, and curl from the guest to `[fec0::2]` reaches a server on the host. The Lenovo's proof is 23b's.

### 18f. IPv6 sockets in inet_diag and /proc/net

* inet_diag for IPv6 (`netlink.c:1533`), so `ss -6` works; `/proc/net/tcp6`, `udp6`, `raw6` and `snmp6`; the procfiles case's IPv6 lines (`procfilesmusl.c:208`).
* Done: in QEMU `ss -6` and the files list a listening and a connected socket of each kind.

### 19. The vDSO on x86-64

* Calibrate the TSC against the active reference even when an HPET or PM timer exists (`pit.c:314`), check it is invariant and monotonic across CPUs, and fall back to the system call where it is not.
* A vDSO image with clock_gettime, gettimeofday, time, getcpu and clock_getres, a vvar page with the TSC's scale, `AT_SYSINFO_EHDR`, `[vdso]` in maps, gdb loading it.
* Done: in QEMU a process's clock_gettime makes no system call by the kernel's count where the TSC is valid, and falls back where it is not. The Lenovo's figure is 23a's.

### 20. Logins kept across a reinstall

* `rach2-install --update` keeps `/root` as it keeps `/home`, on the keep side (`rach2-install:984-994`) and the put-back side (`:1113`), and keeps the machine's `/etc/vconsole.conf` (the Lenovo's has `KEYMAP=ru`, `KEYMAP_TOGGLE=alt-shift` since 2026-11-01) as it keeps the accounts.
* `tools/dev/machine_logins.sh pull|push`: the agents' login files (the five of `agents_guest.sh:140-143` and opencode's store, copied consistently from its SQLite by its backup command) from and to a running Lenovo, modes kept, contents never printed; a failed pull stops a replacement.
* The machine image (not the test images) stages a login directory named on the command line, such as `~/Desktop/rach2-agent-creds`.
* Done: in QEMU an `--update` keeps `/root` and `/etc/vconsole.conf`, and the image stages a named directory. The Lenovo's reinstall is 23c's.

### 21a. term: colours after the alternate screen

* First capture the bytes term receives in the owner's case (term's `-H` mode): vim, `:colorscheme blue`, quit, Ctrl+L. Vim resets SGR before it leaves (`\e[m` before `\e[?1049l`), so the cause is found there before it is fixed.
* Then make modes 1049 and 1048 save and restore as xterm's DECSC and DECRC, in the main screen's slot of patch 0024 (libtsm 4.8.0 saves only the cursor's position, `tsm-vte.c:1764,1782`), and restore the main screen's erase attributes on leaving, which `tsm-screen.c` stores on entry and never restores (`:997`, `:1024-1030`). The case writes the bytes with printf and reads the grid's colours from the screen dump, since the termconsole scenario has no vim.
* Done: the colour case passes in QEMU.

### 21b. term's and the console's bell

* The bell: `audible`, `visual`, `both` or `none` in `/etc/term.conf`, which term already reads and rewrites (`term/main.c:155-170`); a short soft beep by default and never a flash unless asked for. The Linux console's `CSI 10;n]` and `CSI 11;n]` (pitch in Hz, length in ms; length 0 is silent), which need a `]` case in libtsm's `do_csi` and an interface to term (`term/screen.c:35`).
* The PC speaker has no volume (`speaker.c:25`), so soft means short and low: a 40 ms tone at about 750 Hz by default. Whether the Lenovo sounds it, and how loud, is 23c's to find; if it is silent there, the setting stays `none` on the Lenovo and the case says so.
* The kernel: KDMKTONE in Linux's encoding (high 16 bits milliseconds, low 16 bits the PIT divisor, the tone ended by the kernel), and KIOCSOUND corrected to take the PIT divisor as Linux's does, where `syscall.c:2084` passes it as Hz; cancellation and replacement of a sounding tone; one call in the tick for the tone's end; `ioctl(2)` and `console.4`.
* Doom's `play_tone` (`ports/doom/i_pcsound.c:167`) passes hertz to KIOCSOUND today; it converts to the divisor in the same commit as the kernel.
* `term.1` and term's Readme section on `/etc/term.conf`.
* Done: in QEMU each bell setting does what it says, by the speaker's counts in the kernel, and Doom's speaker effects still sound. The owner's ear on the Lenovo is 23c's.

### 22. License

Rach2's own code is GPL-2.0-only, the owner's decision of 2026-11-01 ("GPLv2 for all our code").

* `COPYING`: a notice that Rach2's own code is licensed under the GNU General Public License version 2 only, that each vendored project keeps its upstream license as `man/programs.md` lists it, and that the syscall note applies; then the GPLv2 text verbatim, from the FSF's `gpl-2.0.txt`.
* `LICENSES/exceptions/Linux-syscall-note`: a note in the words of Linux's, naming Rach2: a user program that uses the kernel's services by system calls is not a derived work of the kernel; `COPYING` points to it.
* `Readme.md:63`'s sentence ("declares no license") and `man/programs.md:243`'s ("declares no license for its own code") say GPL-2.0-only and point to `COPYING`.
* `rtl8168.c` and `rtl8168.h`: a header note that they derive from Linux's GPL-2.0-only r8169 driver (`r8169_main.c`, `r8169_phy_config.c`, v6.16), whose license they keep.
* Doom: `ports/doom/src/sha1.c` is GnuPG's, GPL-3.0-or-later (`sha1.c:9-11`), and is linked with Doom's in-house files (`ports/doom/Makefile:48`), which a GPL-2.0-only license cannot be combined with. Replace it with an in-house SHA-1 of the same interface (`sha1.h`, used by `w_checksum.c` and `d_net.c`), checked against FIPS 180's test vectors in a host row; the rest of Doom is GPL-2.0-or-later and LGPL-2.1-or-later, which combine. The Doom Readme's license section (`ports/doom/Readme.md:34-40`) says so and keeps the upstream notices.
* No SPDX lines in other files.
* Done: the files are in place, Doom builds with the new SHA-1, and its host row passes.

### 23a. The kernel's figures on the Lenovo

Owner-only; the Lenovo by its lock, on a trial boot of the milestone's head, as the `machine` skill says.

* Warm stat on ext2 against tmpfs, `git status` on a 5,000-file tree, 2,000 `pthread_create`, `py_thread_wakeup.py 500`, the 4-thread case, `sleep(0.001)`, `clock_gettime` with the TSC's verdict, and `lscpu`'s socket, cores and threads, each against the reports' figure in Measured.
* The kernel alone goes on the trial, by `bootctl kernel` over the release's root, as the `machine` skill says, and the idle share of the ticks (`sched_idle_ticks`) on an idle machine is recorded at 1000 Hz beside the release's at 100.
* Targets: warm stat within a few times tmpfs; `git status` well under 0.5 s; 2,000 `pthread_create` under 0.1 s; set and join of 500 threads under 0.5 s; the 4-thread case no slower than before; `sleep(0.001)` under 2 ms; clock_gettime under 100 ns on a valid TSC; one socket, two cores, two threads.
* A figure that misses goes back to its task as a bug, fixed on the spot.
* Done: the figures are in `dev-log/67-agents/lenovo.md` and the release is back in both slots.

### 23b. IPv6 on the Lenovo

Owner-only; the Lenovo by its lock, on a trial boot.

* The machine takes an `fd91:381e:e667:4a0c::/64` address and the Mac pings it; `ssh -6` from the Mac and curl from the Lenovo to a server on the Mac over IPv6 work; an Internet name with AAAA and A records is reached over IPv4 at once; a fragmented echo to the Mac reassembles.
* Done: each step is recorded, and the release is back in both slots.

### 23c. The agents and term on the Lenovo

Owner-only, with the owner at the machine for the bell and the screen; the Lenovo by its lock. It installs a root, so its loop builds the milestone's x86-64 GNU root on the testing host and installs it through the recovery system as `release.sh` does; afterwards it puts back the release's root as well as its kernel in both slots, since both slots boot the one root (`rach2-install:1050`), and task 5's 256-byte inodes are mounted read-only by an older kernel (`ext2.c:5528`).

* OpenCode's acceptance on a trial boot, as task 3's in QEMU, and on term on the Lenovo's screen, which is the owner's look.
* Claude Code's sandboxed Bash and Muse's default shell confine.
* A reinstall with `machine_logins.sh` keeps all five agents signed in.
* The owner hears the bell and sees no flash; vim's colours leave nothing behind.
* Done: each step passes in the owner's words, is recorded, and the release is back in both slots.

## The reviews

Codex and a Fable reviewer each reviewed the draft triage against milestone 66's code, then this plan. Their answers are in `reviews/`.

* First round, taken: lock scaling cut (both); the bcache bucket count and ext2 node hash as the cheapest large win (Fable); overflowuid as the reason bubblewrap fails (Fable); bubblewrap's missing `pivot_root`, `MNT_DETACH` and proc-anywhere (Fable); Landlock cut (both); the futex fix apart from the scheduler (both); tee's and splice's pipe contracts (Codex); a growable descriptor table instead of a larger constant (both); the /dev/stdin and chown claims dropped (both); the repro programs as cases (Fable); denial tested in sandboxes (Codex).
* Second round, taken: OpenCode's digest pinned in FILES (Fable); acceptance's credential files replaced by credential sources, SQLite-aware (both); `agent_native.c` and the runtime mapping proof in task 2 (both); FFF exercised (Codex); the extra_isize feature bit (Codex); every 100 Hz assumption and a 64-bit tick count in task 8 (Codex, Fable); TSC calibration before the vDSO (Codex); one owner per shared function, with 9 and 16 waiting on 10, 12a on 4, 19 on 16 (both); tasks 7, 11 and 12 split (both); Chromium wholly in 13 (both); ports built in their tasks (Codex); Done clauses that need a later task rewritten (both).
* Third round, on full IPv6 and term, taken: 18a's Done kept to what 18a can show, the host ping moved to 18b (both); the interface's real records, `netdev.h` and `netconf.c`, and the e1000's one multicast bit (both); ND's own queue apart from ARP's, and expiry hooks (Codex); the address width given to one task, 18c, with UDP's checksum and TCP's sizes (both); 18c split into 18c and 18e (both); RDNSS and ICMP datagram sockets cut, since nothing here consumes them (Fable); iproute2 and `udhcpc6` placed rightly, and the cases that assert IPv6's absence given owners (Fable); the colour bug's capture first and `tsm-screen.c`'s unrestored erase attributes (Fable); the bell in `/etc/term.conf` instead of init (Fable); KDMKTONE's and KIOCSOUND's Linux encodings (Codex); the Lenovo steps owner-only (Fable).
* Set aside against a reviewer: the vDSO, IPv6 and ASLR, which Codex would defer; the vDSO and ASLR stay in, ordered last, and IPv6 grew to the full stack at the owner's word. `net.core.somaxconn` dropped rather than built, since `listen` has no backlog to raise.
* Joint review of 67 and 68 with their split, taken: the groundwork commit that both branches share, `TICK_HZ` in `timer.h` and `/proc/audio`'s generator out of `procfs.c`, since a task on 68's branch cannot keep 67's host test whole (both); the `linux-host` label and its test in `board.sh next`, since the manager's Mac loops cannot build the GNU layer (Fable); the Lenovo's lock in `status.sh` (Fable); 8 no longer waits on 7b, and 16 waits on nothing (Fable); 18a split into 18a and 18a2, with `socket_kind` and `pingcall.h` given to 18a so that `ping -6` is its own (both); one owner each for `alloc_process`, TCP's constants and `vfs_setattr` (Codex); 9 keeps one contiguous table, since 70 places index it (Codex); capabilities per process with a bounding set across exec in 12a (Codex); 7b's and 68's 1c's Dones made to prove the bound, not only the function (Codex); 8's storage deadlines (Codex); Doom's GPL-3.0-or-later `sha1.c` replaced in task 22 (Codex); the root installs of 23c and 68's 8, and the root put back with the kernel (both); the documents each task changes (Codex); 68's mixer already present, 5 split into 5a and 5b, the waits of 1c, 2d, 3a and 4 corrected, the verb path's bound, `audiotest -d` in place of `cat`, Doom's Makefile, FFmpeg's full measured configuration, its key and a converting Done, cmus's and sox_ng's pins (both); the queue's order with each chain first, the end runs one after the other, and the GNU ports' rebuild at each (Codex).
* Set aside: moving the groundwork to master, which only a release or a process fix moves (Fable's first choice; the shared commit on 67's branch does the same); converting the 70 direct users of the descriptor table (Codex), since reallocation under the table's lock keeps them correct; an integration task for sox_ng's and FFmpeg's device proofs (Codex), since 5b's cases and 8 already hold them.
