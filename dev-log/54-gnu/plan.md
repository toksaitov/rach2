# Milestone 54: GNU libc and the GNU system

## The goal

Rach2 runs GNU libc and a GNU userland beside its musl system: bash, coreutils, the GNU tools that configure scripts expect, GNU make, Vim, Perl, Git, OpenSSH with its server, client and libraries, GCC with binutils, and GDB. The kernel gets far enough towards a headless browser that Debian's i386 `chromium-headless-shell` loads a page on Rach2. The browser itself is not ported in this milestone.

## What the discovery found

The discovery of 2026-09-27 ran static glibc programs on Rach2 in QEMU and read the sources of glibc, the ports and both browsers.

* No glibc program runs on Rach2, not even a static "hello". glibc applies read-only protection to its relocated data (RELRO) with `mprotect` at startup, in static programs too, and the kernel has no `mprotect`. `execve` refuses the ET_DYN type that dynamic loaders and PIE programs use.
* Linked without RELRO, static glibc programs run, and about 40 of 80 coreutils commands tried did what they should. The rest failed on a short list: glibc's `fstat` is `fstatat64` with an empty path and `AT_EMPTY_PATH`, which the kernel refuses; `mkdirat`, `unlinkat`, `renameat`, `renameat2`, `mknodat` and `fchdir` are missing; glibc's `fork` is `clone`; glibc calls `futex` in a program with one thread; `sleep` is `clock_nanosleep`; `time` reads `CLOCK_REALTIME_COARSE`; `touch` is `utimensat` with a NULL path.
* Signals deliver a frame of the kernel's own layout with a zero `siginfo` and `ucontext`, never restart a call, and never reach a handler for a processor fault.
* glibc's `posix_spawn`, `system` and `popen` start the child with `clone(CLONE_VM|CLONE_VFORK)` and have no fork path. `execve` accepts 63 entries and 3072 bytes of strings. Paths are resolved in 256-byte buffers. `pselect6` and `ppoll` ignore their signal mask.
* Mozilla ended 32-bit Linux builds with Firefox 144 and lists Linux x86 as a tier-3 platform kept by distributions; Chromium's build refuses the 32-bit Linux target. Debian builds both for i386: `chromium-headless-shell` 154 is in trixie and sid, Firefox 156 in sid. Either needs threads, real `mmap` and `mprotect`, shared file mappings, dynamic loading and `/proc/self/exe`; Chromium also needs `epoll` and `eventfd`.

## Decisions

* The system stays 32-bit. Debian's i386 packages are the browser target.
* glibc goes beside musl. Every musl program is static, so the two do not meet.
* The versions: binutils 2.47, GCC 16.2.0 with GMP 6.3.0, MPFR 4.2.2 and MPC 1.3.1, glibc 2.44 against the Linux 6.18 headers, libxcrypt 4.5.2 (glibc 2.44 has no `crypt`), zlib 1.3.2, OpenSSL 4.0.2, OpenSSH 10.5p1, ncurses 6.6, bash 5.3, coreutils 9.12, make 4.4.1, Vim 9.2.1132, Perl 5.44.0, curl 8.22.0, Git 2.55.0 and GDB 18.1, with grep, sed, gawk, findutils, diffutils, patch, tar, gzip, xz and less at their releases current when phase 3 starts.
* The tarballs of this layer are not committed: GCC's is 107 MB, over GitHub's 100 MB limit for one file, and the set is about twice the repository. The build fetches each at its pinned version, checks its SHA-256, and keeps it outside Git. The musl system still builds from a clean clone without a network.
* The GNU layer builds on the Linux hosts, which run i686 Linux programs and so run the configure tests of glibc, Perl and the rest. The Mac edits and builds the kernel and the musl images.
* The GNU system is installed on the ext2 root, since KRFFS has no links.
* OpenSSH replaces Dropbear on os-real. Dropbear stays in the recovery image and os-simple.
* bash is `/bin/bash`. `/bin/sh` stays BusyBox's ash, which the system scripts and the test harness use.
* Ported programs install their upstream manual pages. This milestone writes the pages of the system calls it adds.
* No PAE in this milestone.

## How the work runs

* The milestone is one batch, as the Batches section of `AGENTS.md` describes. Each item is written and compiled, run once to see that it does its basic job, and its tests are written with it. The tests run at the end.
* All testing on Rach2 waits for phase 8, and the machine for phase 7.

## Phases

### Phase 0: the toolchain

* Fetching and checking the pinned tarballs.
* An i686-linux-gnu cross toolchain on the testing host: binutils, a first GCC, the Linux headers, glibc, libxcrypt, and the final GCC with C and C++.

The basic check: a static and a dynamic "hello" build, and the static one runs on the testing host.

### Phase 1: static glibc programs

* `mprotect`, with the memory work under it: regions that split and merge, freed address space used again, the address hint honoured, `MAP_FIXED` that replaces what it covers, copy-on-write that respects protection, and `brk` over a heap that protection split.
* `clone` in the form glibc's `fork` uses (`CLONE_CHILD_SETTID`, `CLONE_CHILD_CLEARTID`, `SIGCHLD`), and `futex` `WAIT`, `WAKE` and `WAIT_BITSET` within one address space.
* `fstatat64` with `AT_EMPTY_PATH`; `utimensat` with a NULL path; clocks 2 to 7 with the CPU time they count, `clock_getres`, `clock_nanosleep`; `gettid`; `rt_sigpending`; `sigprocmask`; `getpriority`, `setpriority`; `sched_yield`, `sched_getaffinity`; `getrusage`; `waitid`; `prctl`; `pread64`, `pwrite64`; `truncate64`; `mkdirat`, `unlinkat`, `renameat`, `renameat2`, `fchdir`; `O_PATH`; `SOCK_CLOEXEC` and `SOCK_NONBLOCK`; `RLIMIT_STACK` kept; `execve` limits sized as Linux sizes them; paths up to 4096 bytes.
* Signals in Linux's i386 frames, with a `siginfo` and a `ucontext` that `rt_sigreturn` restores; `SA_RESTART`; `sigaltstack`; processor faults delivered to handlers; `int3` from user mode; the masks of `pselect6`, `ppoll` and `rt_sigsuspend`; pending signals kept across `execve`.
* `lchown` and `fchownat` with `AT_SYMLINK_NOFOLLOW` change the link, not its target; `getdents64` reports a symbolic link as one.

The basic check: static glibc builds of coreutils, make and bash run the commands the discovery ran.

### Phase 2: sharing and IPC

* Address spaces, open files and directories held by reference; `execve` replaces the caller's address space instead of emptying it; `clone(CLONE_VM|CLONE_VFORK)` for `posix_spawn`, `system` and `popen`.
* FIFOs through `mknodat`; `flock` and POSIX record locks that lock; `chroot`.
* AF_UNIX: `socketpair`, stream, datagram and sequenced-packet sockets, `sendmsg` and `recvmsg` with `SCM_RIGHTS` and `SO_PASSCRED`, and sockets named in the file system.

The basic check: `system` and `popen` run a command, and a descriptor passes from one process to another.

### Phase 3: dynamic linking and the ports

* File-backed `MAP_PRIVATE` mappings; ET_DYN programs with `PT_INTERP`; the rest of the auxiliary vector; `/proc/self/exe` and `/proc/self/maps`; `dlopen`.
* glibc as the shared system library, with its locale archive; then zlib, libxcrypt, OpenSSL, OpenSSH, ncurses, bash, the GNU tools, coreutils, make, Vim, Perl, curl and Git, installed on the ext2 root of os-real.

The basic check: each program starts and does its main job, and `ssh` logs in to os-real through OpenSSH.

### Phase 4: threads and the native compiler

* Threads: a process becomes a group of threads, each with its own TLS descriptor, mask and pending signals; `futex` with keys shared across address spaces; `exit` for one thread and `exit_group` for all; robust lists; `/proc/self/task`.
* GCC and binutils built to run on Rach2.
* The SpiderMonkey shell from Firefox's tree.

The basic check: a program's threads start and join; `gcc` compiles and links a C and a C++ program on Rach2; the shell runs a script with its JIT on.

### Phase 5: debugging

* `ptrace` as GDB uses it, the debug registers, `/proc/PID/mem`, `auxv` and `status`, and core files.
* gdbserver and GDB.

The basic check: GDB runs a program, stops at a breakpoint, steps, prints a variable and prints a backtrace.

### Phase 6: what the browser needs

* `eventfd`, `epoll`, `timerfd`, `memfd_create` with seals, a tmpfs for `/dev/shm`, shared file mappings, `madvise` and `mremap`, and the `/proc` and `/sys` files Chromium reads.

The basic check: Debian's `chromium-headless-shell` for i386 starts on Rach2 with its libraries.

### Phase 7: the machine

The system of phases 0 to 6 installed on the Lenovo, with the owner there once.

### Phase 8: the close

The checks of the Batches section, once and in their order: the compile and `tests/run style`; the Codex review of the whole milestone; the build from clean, `tests/run all` with this milestone's tests, and their mutations; the fixes; the machine again if the fixes changed what it runs; the last `tests/run all`. The acceptance adds the ports doing their jobs on Rach2, the SpiderMonkey shell, and Debian's `chromium-headless-shell` loading a page.
