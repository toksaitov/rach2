# Claude Code on Rach2: the conversation

Claude Code 2.1.296, the official binary at `/opt/claude/bin/claude`, ran on the Lenovo as root in `/root/test` in two sessions. The first, over SSH from 08:38 to 09:59, held the greeting, the audit and the autotools test; the second, at 10:23, was a greeting on the framebuffer terminal. Every reply came from `claude-opus-5-5`. The permission mode was auto, in which a classifier passes or refuses each tool call. The owner raised the effort to xhigh at 08:46.

Times are UTC. The owner's turns are quoted whole. Claude Code's turns are condensed to what it said and did; its own words are in quotation marks. The source is `.claude/projects/-root-test/586d3db4-38f8-4e84-acb9-e5fb68ba2422.jsonl` (1.7 MB) and `af47fa60-6df8-4423-84a9-5af74604313f.jsonl` in the copy of the Lenovo's `/root`.

## Session 1, over SSH

### 08:38, the greeting

> Hey Claude, I'm building my own educational OS with a kernel called Rach2. It's Linux-like, but it isn't Linux. We already have the GNU stack and headless Chrome working, and... ta-da! Even you are running here without a single change to your code (just the official binaries, downloaded as-is). It all works because Rach2 provides a Linux-compatible system call interface.
>
> Welcome to the new OS! Please do a short discovery run: look around the environment, and if anything doesn't work for you, write it up in a text file in this directory so we can fix it. And tell me, how do you feel in your new home?

"I'll take a look around."

* 08:38 Surveyed the system: `uname`, os-release, `id`, CPUs, memory, disks, `ps`, `/proc/self`, mounts, `/sys`, `/dev`, the versions of 21 everyday tools (Node was missing), Perl, ptys, file systems, the network (`curl` to its API, `ping`).
* 08:38 Listed `/dev/pts` again three times and found the earlier empty listing was its own fault: "That earlier empty `/dev/pts` was my own `| head` truncation, not a bug."
* 08:39 Ran a Python battery of system call probes ("Mostly green"), then checked the failures again: signalfd, pty, sendfile, timers. Two more of its failures were its own: a signalfd call with the wrong signature, and a pty child whose buffered output `_exit` discarded.
* 08:40 Ran git, C probes (atomics across 16 threads, statx, clone3, close_range, seccomp), and a fork rate test. Of the seccomp probe it said: "that `Killed` is correct behavior. My probe put the process in seccomp strict mode, and Linux kills `exit_group` in that mode."
* 08:40 Looked at itself: 14 threads, about 330 MB resident; checked `/proc/sys`, the time zone, pipe and disk throughput, and the units of `/proc/<pid>/stat` against `ps`.
* 08:41 Wrote `rach2-discovery-report.txt`.

Its answer, 08:41:

"I feel at home here. I started up, logged in, reached the API over HTTPS, and every tool I use (shell, git, ripgrep, file I/O, subprocesses, PTYs) behaved as it would on Linux. Running an unmodified Bun-based binary with 14 threads and about 330 MB of memory on a kernel that isn't Linux is a real achievement."

It reported about 70 checks and listed, by how likely each is to affect real software: `sendfile` failing with `EINVAL` to a socket or a file, and `splice` from a regular file; sleeps of 1 ms taking 7 to 10 ms while `clock_getres` reports 1 microsecond; no IPv6, not even `::1`; then `copy_file_range` missing, the wrong error for `O_TMPFILE`, no extended attributes on ext2, end-of-file instead of `EIO` from a pty master, missing `/proc` files, missing newer calls (rseq, membarrier, io_uring, pidfd), an open-file limit of 1024, and `ps` showing 100% CPU for new processes. "None of these blocked me." It added that `bwrap` and `socat` are not installed, so its optional command sandbox cannot be turned on, and that user namespaces work.

### 08:45, the audit

> Great write-up! Now think very hard about this: as a coding agent, what do you expect to be available and working well so you can do your job every day? Check how all of it actually behaves here. Then document everything that doesn't work, or doesn't work well, in a structured way in this directory, so our AI factory can review it and improve the environment for you.

Claude Code inventoried the tools ("Big gaps already: no `pip`, no Node/npm, no Chrome on PATH") and started two more checks. The owner stopped the turn, refusing both pending calls, ran `/effort` to set xhigh, and wrote:

> Whoops, sorry, forgot to turn xhigh effort here. Start again, please, think very hard.

It started again and worked for an hour, speaking only in short notes between steps:

* 08:48 Packaging and reach: no pip in the system Python, pip inside a venv, the CA store, no OS package manager; found Chromium at `/usr/bin/chromium-headless-shell`; identified which programs are GNU and which BusyBox; timed DNS and HTTPS to GitHub, PyPI, npm, the Go proxy, crates.io and others.
* 08:49 Downloaded the official Node 26, Go 1.27 and Rust 1.99 toolchains into its scratchpad in the background (1.1 GB in all).
* 08:50 File system semantics: one-second mtimes on ext2, so `make` called a freshly edited target up to date; inotify event by event; flock and fcntl locks across processes; SQLite in WAL mode; directories of 1,000 to 8,000 files on ext2 and tmpfs; a C benchmark of system call latency.
* 08:57 Found no vDSO (no `AT_SYSINFO_EHDR` in the auxiliary vector), timed everyday commands, measured scaling over 1 to 8 threads, cloned git/git (4,857 files) and timed `git status` (2.5 s) and `git grep`, profiled both with `strace -c`, and measured path lookup per component and lock contention.
* 09:01 Runtimes: a Node battery (HTTP server and client, child processes, the V8 tiers), the effective CPU clock, Go build, test and race detector, pip installs in a venv (requests, pytest, numpy, and markupsafe built from source), a cargo project with serde. At 09:07 Claude Code's own safety check refused an `rm -rf ./*` in its scratch directory, and it ran that step again without it.
* 09:15 Headless Chromium over CDP and with puppeteer (it read the screenshot it took), gdb with breakpoints and hardware watchpoints, strace, ASan, TSan and UBSan, core dumps.
* 09:16 The terminal: Ctrl+C, Ctrl+Z and `bg`, raw mode, vim, tmux and less in a pty, all working.
* 09:17 Dev servers on reused ports, process tree cleanup, DAC as `nobody`, setuid, seccomp-bpf, the namespaces bwrap needs, ASLR. An exit status of 144 turned out to be its own `pkill -f` matching its shell: "Signals and exit statuses are correct and process-group kills work."
* 09:18 A batch hung. One Python test with 2,000 threads spun: "It finished eventually, after about 6 minutes of wall time and 23 minutes of CPU time. On Linux this takes about a second." It then measured thread creation and wakeup in C and Python.
* 09:30 Chromium: `SIGTERM` to the launched PID left the browser running and its children alive. It checked `PR_SET_PDEATHSIG`, peer death on socket pairs and epoll's HUP and RDHUP, all correct ("Every low-level mechanism checks out"), and then found the cause: `/usr/bin/chromium-headless-shell` is a `#!/bin/sh -e` script that does not `exec`.
* 09:32 Printed a PDF with Chromium and read it back, and fetched a web page with its WebFetch tool.
* 09:33 The auto mode classifier refused downloading and running GNU hello's `configure` as code from outside. It went on: `/proc/sys` writability, raising limits as root, CPU topology, C++ builds at `-j1` (61 s), `-j2` and `-j4` (30.5 s), page-fault cost, memory latency, inotify `IN_ATTRIB`, close-on-exec on 13 ways of making a descriptor (all honoured).
* 09:39 Looked at the sockets of an orphaned Chromium GPU process, then killed its own leftover Chromium processes, 19283, 19286, 19348, 19418 and 19421.
* 09:42 Wrote `rach2-repro/` and ran it whole (about 1.5 minutes). A rerun of its first benchmark was 1.5 to 2 times slower than an hour earlier. It fixed two bugs in the repro scripts.
* 09:48 Wrote a generator for the JSON and Markdown reports, first correcting two claims: "the OFD-lock test never actually completed, and I'll state the page-fault share as the measured 12% of one compile." The generator wrote 30 issues: 2 critical, 7 high, 12 medium, 9 low.

Its answer, 09:48:

"The audit is done. I wrote 30 issues into this directory, each with what I observed, the Linux behavior I expected, the impact on agent work, a repro command and a suggested fix. Every issue was reproduced at least twice."

What hurts most, in its order: file system metadata is slow and grows slower as the 1 GiB buffer cache fills; builds go stale on one-second timestamps; file system operations do not scale across threads, which points to a global lock; threads break down past a few hundred; timing is coarse and every clock read is a system call; root cannot raise the open-file limit or write `/proc/sys`; and the packaging gaps: no Node, no package manager, no pip, no bwrap or socat, and the Chromium launcher. What works is long: the official Node, Go and Rust binaries run unmodified, and gdb, the sanitizers, seccomp-bpf, namespaces, pty job control, Chromium with puppeteer, pip in a venv, SQLite, inotify basics, and its own PDF, image and web reading. Not covered: out-of-memory, which might have killed the session; the GNU hello build the classifier refused; why Chromium's children outlive the browser by seven minutes and more. "Ten false leads came from bugs in my own probes. They're listed in the report so nobody chases them."

### 09:52, the download test

> Do the download test, I will accept it here.

"I'll run it now. The tarball gets its own empty directory, and the build happens in a separate one."

* 09:53 Downloaded GNU hello 2.12.1 and ran `configure`, `make -j4` and `make check`.
* 09:54 Profiled `configure` under `strace -c`, and traced `clone3` to see why it failed.
* 09:58 Folded the results into its generator and wrote both reports again.

Its answer, 09:58: "I ran the autotools build: it works on Rach2, but it's slow." `configure` took 58.5 s, `make -j4` 17.0 s, and `make check` passed 6 tests and skipped one, which GNU hello skips on purpose ("not a full moon night"). `configure` started 2,877 processes and made 145,580 `readlink` calls at 127 to 273 microseconds each. One new finding: glibc's `posix_spawn` asks `clone3` for `CLONE_CLEAR_SIGHAND`, gets `EINVAL` and falls back, 623 times in that one run; it went into R2-025. Still 30 issues.

## Session 2, on the framebuffer terminal

### 10:23

> Hi, again, trying out the new fullscreen renderer here not from ssh, but on the real machine itself

Its answer, ten seconds later: "Hi again! Good idea to test it on the machine itself as well as over SSH, since terminal emulator, font rendering, and input handling can all behave differently locally." It offered things that tend to break a renderer: long scrolling output, rich Markdown, wide Unicode characters, resizing during output, and its permission dialogs. It drew a sample table of Japanese text, two emoji, box-drawing characters and a combining accent, and a short Python block. The session ended there.
