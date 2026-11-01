# Rach2 as a daily home for a coding agent: environment audit

- **Date:** 2026-11-01
- **Auditor:** Claude Code 2.1.296 (claude-opus-5-5), official binary at /opt/claude/bin/claude
- **System:** Rach2 1.0.0, kernel 'Linux 6.18.0-rach2' x86_64; Intel Core i3-4030U (2C/4T @1.9 GHz); 16 GB RAM, no swap; root fs ext2 on /dev/disk0p3; /dev/shm tmpfs
- **Method:** Each finding was reproduced at least twice with a minimal probe; probes that turned out to be bugs in my own test code are listed under 'ruled_out' so nobody chases them. Linux reference numbers are approximate expectations for this class of hardware, not measurements on this machine.
- **Repro suite:** rach2-repro/run_all.sh (about 1.5 min; run it from a directory on the filesystem under test). Every issue below names its repro.
- **Supersedes:** rach2-discovery-report.txt (first, syscall-only pass); all of its findings are merged here.

Machine-readable twin: `rach2-agent-env-issues.json` (same content, generated from the same source).

## Where to start

Most of the day-to-day pain comes from three kernel areas. Fixing them would change how this machine feels to an agent more than everything else combined:

1. **Filesystem metadata speed and scalability** (R2-001, R2-003, plus R2-013, which is mostly a consequence). `git status` on a 4.9k-file repo takes 2.5 s, and the cost gets worse as the ~1 GiB buffer cache fills.
2. **Nanosecond timestamps** (R2-002). Builds silently test stale binaries after a fast edit, which makes the agent's results wrong rather than just slow.
3. **Threading and time primitives** (R2-004, R2-005, R2-006, R2-010). O(n^2) thread creation, a futex/GIL collapse that burns CPU for minutes, no vDSO, and ~10 ms timers.

The packaging items (R2-008, R2-009, R2-015, R2-019) are cheap. The official Node/Go/Rust binaries already run unmodified.

## Scales

- **critical**: Hits nearly every agent task, every day: wrong results or order-of-magnitude slowdowns.
- **high**: Breaks or blocks a common workflow, or a large slowdown in common work.
- **medium**: Breaks a less common workflow or degrades a common one; a workaround exists.
- **low**: Edge-case compatibility, diagnostics, or cosmetic.
- layer **kernel**: Rach2 kernel behaviour (syscalls, VFS, scheduler, mm, procfs).
- layer **distro**: Packaging / what ships in the image / default configuration.

## Index

| ID | Severity | Layer | Area | Title |
|----|----------|-------|------|-------|
| [R2-001](#r2-001) | critical | kernel | vfs/ext2 performance | Filesystem metadata operations on ext2 cost 100-500x a warm Linux dcache |
| [R2-002](#r2-002) | critical | kernel | vfs/ext2 timestamps | 1-second mtime granularity on the root filesystem causes stale incremental builds |
| [R2-003](#r2-003) | high | kernel | vfs scalability | Filesystem operations don't scale across threads: total throughput falls as threads are added |
| [R2-004](#r2-004) | high | kernel | threads/mm | Creating threads gets slower the more threads exist (O(n^2)) |
| [R2-005](#r2-005) | high | kernel | futex/scheduler | Many threads contending on one lock collapse into minutes of spinning |
| [R2-006](#r2-006) | high | kernel | time/vDSO | No vDSO: clock_gettime/gettimeofday are full syscalls, slower than getpid |
| [R2-007](#r2-007) | high | kernel | limits | RLIMIT_NOFILE is capped at 1024 hard, and even root can't raise it |
| [R2-008](#r2-008) | high | distro | runtimes | No Node.js / npm / npx in the image |
| [R2-009](#r2-009) | high | distro | package management | No working package manager, so missing tools can't be installed |
| [R2-010](#r2-010) | medium | kernel | timers/scheduler latency | Timer resolution ~5-10 ms (clock_getres claims 1 us); wakeups under load take 3x the requested time |
| [R2-011](#r2-011) | medium | kernel | inotify | inotify: directory watches miss IN_ATTRIB, chown emits nothing, and the watch limit is 8192 and fixed |
| [R2-012](#r2-012) | medium | kernel | procfs/sysctl | Root can't tune the kernel: /proc/sys is nearly empty and read-only |
| [R2-013](#r2-013) | medium | kernel | process creation | fork ~1 ms and fork+exec 2.3-4.6 ms, so shell-heavy work is slow |
| [R2-014](#r2-014) | medium | kernel | memory management | Page faults cost ~3-7 us, MAP_POPULATE is a no-op, no THP, and rusage fault counters are always 0 |
| [R2-015](#r2-015) | medium | distro | python | The system Python has no pip (and there's no uv/pipx) |
| [R2-016](#r2-016) | medium | distro | chromium packaging (and an open kernel question) | The Chromium launcher doesn't exec, so killing the launched PID orphans the browser, and its children can then outlive it |
| [R2-017](#r2-017) | medium | kernel | zero-copy I/O | sendfile and splice reject regular-file sources; copy_file_range is ENOSYS |
| [R2-018](#r2-018) | medium | kernel | networking | No IPv6 at all (AF_INET6 -> EAFNOSUPPORT), not even ::1 |
| [R2-019](#r2-019) | medium | distro | developer tooling | Common agent tooling is missing from the image |
| [R2-020](#r2-020) | medium | kernel | observability | perf_event_open is ENOSYS, so no perf profiling |
| [R2-021](#r2-021) | medium | kernel | security | No ASLR: stack and library addresses are identical on every run |
| [R2-022](#r2-022) | low | kernel | netlink sock_diag | ss -xp printed '!!!Remnant of size 44' (malformed UNIX sock_diag reply) |
| [R2-023](#r2-023) | low | kernel | vfs/ext2 semantics | Small ext2/VFS semantic gaps |
| [R2-024](#r2-024) | low | kernel | procfs/sysfs | procfs/sysfs gaps and wrong CPU topology |
| [R2-025](#r2-025) | low | kernel | syscall coverage | Newer syscalls return ENOSYS: io_uring, pidfd_open, rseq, membarrier, landlock; no cgroups |
| [R2-026](#r2-026) | low | kernel | init/process reaping | Orphans are reaped by init ~0.7 s after exit |
| [R2-027](#r2-027) | low | kernel | tty | PTY master read returns EOF instead of EIO after the slave closes |
| [R2-028](#r2-028) | low | distro | limits/config | RLIMIT_NPROC soft 256-512 for interactive shells |
| [R2-029](#r2-029) | low | distro | python build | Python built without PGO/LTO (and without tkinter) |
| [R2-030](#r2-030) | low | distro | first-run setup | First-run setup an agent will need (configuration, not bugs) |

## Issues

### R2-001

**Filesystem metadata operations on ext2 cost 100-500x a warm Linux dcache**

`severity: critical` `layer: kernel` `area: vfs/ext2 performance` `status: confirmed`

**Observed**

- C microbenchmark on / (ext2), single thread, warm: stat existing 132-190 us, stat missing 135-216 us, fstatat(dirfd,"f") 351 us, open+close 246-540 us, create+unlink 572-755 us, mkdir+rmdir 295-475 us, rename 234-489 us, opendir+readdir+closedir 172-343 us.
- Same binary on /dev/shm (tmpfs): stat 4.6 us, open+close 11.6 us, create+unlink 30 us, rename 19 us. Operations on an already-open fd are fast on ext2 too (pread 4K 1.7 us, fstat 2.4 us), so the data path is fine and the name/inode path is slow.
- Cost is roughly per operation, not per path component (deep/f 132 us vs deep/1/2/3/4/5/6/7/f 205 us), so it looks like re-reading inode/directory metadata on every call rather than a slow walk.
- Creating N empty files in one directory: 2.7 ms/file at N=1000, 3.8 ms/file at N=8000 (tmpfs: 0.6 ms/file).
- DEGRADES OVER UPTIME: after ~1 h of work the same benchmark in the same directory was 1.5-2.2x slower (open+close 246->540 us, rename 234->489 us, readdir 172->343 us, vfork+exec 2.3->3.4 ms) while tmpfs numbers were unchanged. In both readings /proc/meminfo showed Buffers: exactly 1,038,336 kB, i.e. the buffer cache appears pinned at a ~1 GiB cap. A full buffer cache with costly lookup/eviction is the leading hypothesis.

**Expected (Linux)**: Linux ext4 with a warm dentry/inode cache: stat ~1-2 us, open+close ~2-4 us, create+unlink ~10-20 us.

**Impact on agent work**

- git status on git/git (4,857 tracked files) takes 2.0-2.6 s every time (Linux: tens of ms); strace: 5,206 newfstatat calls, mean 2,355 us each.
- git grep 5.4 s, git log -1 --stat 5.0 s, git add -A 2.0 s, git commit 2.9 s, git worktree add 8.2 s (Claude Code worktrees use this).
- Autotools: GNU hello 2.12.1 './configure' takes 58.5 s. Under strace -c its processes made 145,580 readlink (99.95% failing, glibc realpath walking components), 38,258 openat and 27,720 newfstatat calls, about 35 s of summed syscall time at 127-273 us per call.
- GNU grep -r over /usr/include (3,820 files) 2.8 s; python3 -c 'import asyncio,json,re,subprocess' 0.32 s; go build of a small net/http program, cold, 73 s; go test -race 88 s; a single g++ compile spends 1.07 s on 6,727 failing readlink() calls.
- In practice every search, status check, import, build and test run an agent does is dominated by this.

**Repro**: `rach2-repro/syscall_latency.c (./syscall_latency <dir>; compare an ext2 dir with /dev/shm), rach2-repro/fs_thread_scaling.c`

**Suggested direction**: Profile the ext2 lookup/getattr path. Make sure inodes and dentries (including negative dentries) are cached and that a cache hit never touches the buffer layer. Check the buffer cache's lookup structure and eviction policy at its ~1 GiB cap. Also consider ext4 (or nanosecond-capable ext2 inodes, see R2-002).

### R2-002

**1-second mtime granularity on the root filesystem causes stale incremental builds**

`severity: critical` `layer: kernel` `area: vfs/ext2 timestamps` `status: confirmed`

**Observed**

- On / (ext2) st_mtime_ns always ends in 000000000; utimensat with nanoseconds is truncated. tmpfs keeps nanoseconds.
- make: build, edit the source within the same second, run make again -> "'m' is up to date" and the old binary runs (reproduced 3/3).
- Timestamps are also clamped at 2038-01-19 03:14:07 (touch -d 2040 -> 2038; 32-bit ext2 inode times).

**Expected (Linux)**: Nanosecond timestamps (ext4 with 256-byte inodes, tmpfs, btrfs, xfs), or at least 2038-safe times.

**Impact on agent work**

- Agents edit and rebuild within milliseconds. make, ninja, cargo fingerprints, Go's build cache keys, pytest/mypy caches, webpack/vite and jest --watch all compare mtimes, so they silently test stale code. This is a correctness problem: the agent reports 'fixed' based on an old binary.

**Repro**: `rach2-repro/mtime_make.sh`

**Suggested direction**: Store and report nanosecond timestamps (ext4 extra inode fields: i_*time_extra), or put / (or at least /root, /home, /tmp) on a filesystem that has them.

### R2-003

**Filesystem operations don't scale across threads: total throughput falls as threads are added**

`severity: high` `layer: kernel` `area: vfs scalability` `status: confirmed`

**Observed**

- stat() on the same path from N threads, ext2: 1 thread 6,711/s, 2 threads 4,245/s, 4 threads 5,293/s (later rerun: 1,081 -> 640 -> 790/s).
- tmpfs shows the same shape: 144,042/s -> 83,758/s -> 88,532/s.
- git status (preload-index threads): 2.5 s in newfstatat plus 2.5 s in futex. git grep: 15.0 s of futex wait across threads, read() averaging 479 us and close() 564 us under contention, versus ~2 us single-threaded.
- git grep with grep.threads=1 takes 7.0 s; with default threads 5.4 s, so threads barely help.

**Expected (Linux)**: Path lookup and stat scale nearly linearly with cores (RCU path walk); 4 threads give ~3-4x total throughput.

**Impact on agent work**

- Parallel tools (git, rg, make -j, cargo, go build, jest/pytest-xdist workers) can't use the 4 CPUs for I/O-heavy phases, and contention makes some of them slower than single-threaded.

**Repro**: `rach2-repro/fs_thread_scaling.c`

**Suggested direction**: Look for a global VFS or filesystem lock held across slow operations (made worse by R2-001). Consider lock-free or RCU lookup for cached dentries and per-inode locks for getattr/read/close.

### R2-004

**Creating threads gets slower the more threads exist (O(n^2))**

`severity: high` `layer: kernel` `area: threads/mm` `status: confirmed`

**Observed**

- pthread_create of N threads (C): N=250 0.07-0.09 s, N=500 0.24-0.26 s, N=1000 0.94-1.04 s, N=2000 4.6-5.1 s, i.e. about 2.5 ms per thread at N=2000.

**Expected (Linux)**: Roughly constant ~10-30 us per thread; 2,000 threads in well under 0.1 s.

**Impact on agent work**

- JVM, Chrome, VS Code-style language servers, Bazel, and Go programs that block in syscalls run hundreds of threads. Startup and thread pools get slow, and with R2-005 it compounds badly.

**Repro**: `rach2-repro/thread_scaling.c (./thread_scaling 2000)`

**Suggested direction**: Check for linear scans per clone(): VMA list walk for stack mmap (needs a tree or maple tree), a linear thread-list or TID allocation, or per-thread signal/futex setup that scans siblings.

### R2-005

**Many threads contending on one lock collapse into minutes of spinning**

`severity: high` `layer: kernel` `area: futex/scheduler` `status: confirmed`

**Observed**

- Python: N threads block on one threading.Event, then set()+join(). N=125 0.07-0.16 s, N=250 2.0-3.9 s, N=500 31 s (superlinear, roughly cubic).
- N=2000 (with creation) took ~6 min wall-clock and ~23 CPU-minutes. The process sat in state R with ~2.5 cores busy, so CPU is burned rather than threads sleeping.
- C pthread_cond_broadcast + join for 2,000 threads: 0.15-0.47 s (Linux ~10-20 ms). The pathology shows up with the GIL's mutex+condvar (timed waits) handoff.

**Expected (Linux)**: Futex wake/requeue hands off efficiently; 500 Python threads finish in well under 0.1 s.

**Impact on agent work**

- Any thread-heavy Python (pip's parallel downloads, ThreadPoolExecutor test suites, web servers) and other runtimes with many waiters on one lock can stall for minutes while pinning the CPUs. This makes the machine look hung to the agent.

**Repro**: `rach2-repro/py_thread_wakeup.py 125 250 500`

**Suggested direction**: Trace futex(FUTEX_WAIT/WAKE, *_BITSET with CLOCK_REALTIME timeouts) under contention. Look for a thundering herd (wake-all instead of wake-one), woken threads not being scheduled before the waker re-acquires, or a scheduler pick-next that is O(n) in runnable threads.

### R2-006

**No vDSO: clock_gettime/gettimeofday are full syscalls, slower than getpid**

`severity: high` `layer: kernel` `area: time/vDSO` `status: confirmed`

**Observed**

- /proc/self/maps has no [vdso]/[vvar]; auxv has no AT_SYSINFO_EHDR. clock_gettime(CLOCK_MONOTONIC) costs 1.58-1.62 us vs syscall(getpid) 0.48 us.

**Expected (Linux)**: vDSO clock_gettime ~20-30 ns with no kernel entry.

**Impact on agent work**

- V8/Node, Chrome, the Bun runtime inside Claude Code, Go, the JVM and Python's asyncio read the clock on every event-loop turn, timer and trace point. That's a constant tax on everything and amplifies scheduler noise.

**Repro**: `rach2-repro/checks.sh (vDSO), rach2-repro/syscall_latency.c`

**Suggested direction**: Map a vDSO plus vvar data page with clock_gettime, gettimeofday, time, getcpu and clock_getres, and advertise it via AT_SYSINFO_EHDR. The TSC is invariant (TSC measured at 1.90 GHz) and usable as the clocksource.

### R2-007

**RLIMIT_NOFILE is capped at 1024 hard, and even root can't raise it**

`severity: high` `layer: kernel` `area: limits` `status: confirmed`

**Observed**

- ulimit -Hn = 1024. setrlimit(RLIMIT_NOFILE, hard>1024) as uid 0 -> EPERM ('not allowed to raise maximum limit'). RLIMIT_NPROC and RLIMIT_CORE can be raised, so this is specific to NOFILE.

**Expected (Linux)**: Root (CAP_SYS_RESOURCE) can raise the hard limit up to fs.nr_open (default 1,048,576); distros default to 1024 soft / 524,288 hard.

**Impact on agent work**

- EMFILE in jest/vitest, webpack/vite dev servers, tsc --watch, language servers, Chrome with many tabs, cargo/go builds with high parallelism, and file-watcher-based tools. Node and Chrome try to raise their soft limit at startup and can't go past 1024.

**Repro**: `rach2-repro/checks.sh`

**Suggested direction**: Let a privileged setrlimit raise the hard limit up to a kernel nr_open, and raise the default hard limit for init's children (e.g. 524288).

### R2-008

**No Node.js / npm / npx in the image**

`severity: high` `layer: distro` `area: runtimes` `status: confirmed`

**Observed**

- node, npm, npx, pnpm, yarn, bun and deno are absent. The official nodejs.org v26.13.0 linux-x64 tarball works unmodified: npm install, http, worker_threads, fs.watch (including recursive), child_process, fetch over HTTPS, Intl/ICU, signals, puppeteer-core driving the system Chromium.

**Expected (Linux)**: A current Node LTS with npm/npx on PATH.

**Impact on agent work**

- No JS/TS work: install, build, lint (eslint/prettier/tsc), tests (jest/vitest/playwright).
- Most stdio MCP servers are started with npx, so they can't run.
- Claude Code plugins/hooks written in .mjs, and LSP servers (typescript-language-server, pyright) can't run.
- Browser automation (puppeteer/playwright) needs Node.

**Repro**: `command -v node npm npx`

**Suggested direction**: Ship Node LTS, or at minimum document a supported install path. The official binaries already work, so this is packaging only.

### R2-009

**No working package manager, so missing tools can't be installed**

`severity: high` `layer: distro` `area: package management` `status: confirmed`

**Observed**

- dpkg and rpm binaries exist, but there's no apt/dnf/apk front-end, no repositories and no package database (/var/lib has only backlight, locate, sudo). Python's sysconfig points at a build-host store (/var/cache/rach2-store/...) that isn't on the target.

**Expected (Linux)**: One command to install a missing tool (an apt/apk-style manager or a Rach2 store client) backed by a repository.

**Impact on agent work**

- When a task needs jq-like utilities, a compiler, a database or a linter that isn't in the image, the agent is stuck with hand-downloading tarballs (which agent security policies flag; my autotools build test was auto-blocked for exactly that reason and needed explicit human approval) or giving up.

**Repro**: `ls /var/lib; command -v apt apk dnf`

**Suggested direction**: Expose the build system's store (rach2-store) as an installable repository with a small client, even read-only and binary-only at first.

### R2-010

**Timer resolution ~5-10 ms (clock_getres claims 1 us); wakeups under load take 3x the requested time**

`severity: medium` `layer: kernel` `area: timers/scheduler latency` `status: confirmed`

**Observed**

- Python sleep(1 ms) -> 7.2-10 ms; sleep(10 ms) -> ~22 ms; Node setTimeout(1) -> 6.2 ms; Go time.Sleep(1 ms) -> 5.4 ms. clock_getres(CLOCK_MONOTONIC) reports 1 us.
- With 4 CPU-bound threads running, sleep(10 ms) takes 30 ms at p50, p95 and max: the woken task waits for the running task's slice to end.

**Expected (Linux)**: hrtimer-based sleeps accurate to ~50-100 us; wakeup preemption keeps interactive latency low under load.

**Impact on agent work**

- Slow tests that sleep or poll, and sluggish TUIs (Claude Code's own UI) and dev servers during builds. Benchmarks and timeout-sensitive tests become flaky.

**Repro**: `python3 -c "import time;a=time.perf_counter();time.sleep(0.001);print((time.perf_counter()-a)*1e3,'ms')"`

**Suggested direction**: Implement high-resolution timers (or tickless with one-shot LAPIC/TSC-deadline), make clock_getres report the real granularity, and preempt on wakeup when the woken task has run less recently.

### R2-011

**inotify: directory watches miss IN_ATTRIB, chown emits nothing, and the watch limit is 8192 and fixed**

`severity: medium` `layer: kernel` `area: inotify` `status: confirmed`

**Observed**

- chmod and touch (utimensat) on a file deliver IN_ATTRIB to a watch on the file but NOT to a watch on its parent directory. chown delivers no IN_ATTRIB at all.
- max_user_watches=8192, max_user_instances=128, max_queued_events=16384, and none of them are writable by root (see R2-012).
- Everything else matched Linux: CREATE/OPEN/MODIFY/CLOSE_WRITE/CLOSE_NOWRITE, MOVED_FROM/MOVED_TO with matching cookies (in-dir and cross-dir), atomic rename-over, DELETE, DELETE_SELF, IGNORED, ISDIR, and Q_OVERFLOW at 16,385 queued events. Node fs.watch (including recursive) and tail -f work.

**Expected (Linux)**: IN_ATTRIB goes to both the file's own watch and its parent directory's watch. Watch limits are tunable (Linux scales max_user_watches with RAM, up to 1M).

**Impact on agent work**

- Watchers (chokidar/webpack/vite, watchman, nodemon, entr, jest --watch, Claude Code's own settings watcher) watch directories, so 'touch file to trigger a rebuild' and permission changes go unseen. Large repos plus node_modules exceed 8,192 watches and can't be raised.

**Repro**: `rach2-repro/inotify_attrib.py`

**Suggested direction**: Run fsnotify_parent()-style dispatch for attribute changes (chmod/chown/utimes/link count). Make the inotify limits sysctls writable and raise the defaults (e.g. 65536 to 524288 watches).

### R2-012

**Root can't tune the kernel: /proc/sys is nearly empty and read-only**

`severity: medium` `layer: kernel` `area: procfs/sysctl` `status: confirmed`

**Observed**

- sysctl -a lists 24 entries. Writes as uid 0 to fs.inotify.* and vm.swappiness fail with EACCES (only kernel.hostname is writable). Missing: fs.file-max, fs.nr_open, kernel.random.{uuid,boot_id,entropy_avail}, kernel.randomize_va_space, vm.drop_caches, vm.max_map_count, net.* (empty).

**Expected (Linux)**: Writable sysctls for the usual knobs: fs.inotify.*, fs.file-max, vm.max_map_count, vm.overcommit_memory, net.core.somaxconn, kernel.randomize_va_space.

**Impact on agent work**

- Fixes that docs and error messages tell users to apply (raise inotify watches, vm.max_map_count for Elasticsearch, somaxconn) are impossible, and some tools read kernel.random.boot_id or uuid.

**Repro**: `rach2-repro/checks.sh`

**Suggested direction**: Back the common sysctls with real tunables and allow writes with CAP_SYS_ADMIN/root.

### R2-013

**fork ~1 ms and fork+exec 2.3-4.6 ms, so shell-heavy work is slow**

`severity: medium` `layer: kernel` `area: process creation` `status: confirmed`

**Observed**

- fork+wait of a tiny C program 0.74-1.1 ms; vfork+exec /bin/true 2.3-4.6 ms; a bash loop manages ~350 spawns/s. Much of exec's cost is path/library lookup (R2-001), since ld.so opens and stats several files.
- GNU hello 2.12.1 './configure' (a typical small autotools project): 58.5 s wall, 2,877 clone + 2,172 execve; per strace -c, clone averaged 1.4 ms and execve 1.3 ms. make -j4 then took 17.0 s and make check 1.8 s (6 pass, 1 intentional skip).

**Expected (Linux)**: fork of a small process ~50-100 us; fork+exec of a dynamic binary ~300-500 us.

**Impact on agent work**

- Every Bash tool call, git hook, configure script, Makefile recipe and test runner that spawns subprocesses pays this; agents spawn thousands of short processes per task.

**Repro**: `rach2-repro/syscall_latency.c (fork/exec lines)`

**Suggested direction**: Check copy-on-write page-table copying (copy lazily, share page-table pages where possible); most of the exec cost should disappear once R2-001 is fixed.

### R2-014

**Page faults cost ~3-7 us, MAP_POPULATE is a no-op, no THP, and rusage fault counters are always 0**

`severity: medium` `layer: kernel` `area: memory management` `status: confirmed`

**Observed**

- First-touch anonymous fault 2.9-3.3 us (sequential, 64k faults); mmap+touch 16 pages+munmap 110-935 us.
- mmap(256 MB, MAP_POPULATE) returns in 0.0 ms and faults still happen later, so populate is ignored.
- /proc/meminfo has no AnonHugePages/HugePages lines (no transparent huge pages).
- getrusage ru_minflt/ru_majflt are always 0 (also shown by /usr/bin/time). ru_maxrss is correct.
- A g++ compile of one <regex>/<map> TU: 7.6 s wall, 0.9 s sys, 955 MB maxrss. Memory latency is normal (L1 2.3 ns, DRAM ~97 ns), so caches and TLB setup look fine.

**Expected (Linux)**: ~0.5-1 us per 4 KiB anonymous fault including zeroing; MAP_POPULATE pre-faults; THP for large anonymous regions; real fault counters.

**Impact on agent work**

- Compilers (cc1plus ~1 GB RSS), rustc, V8/Chrome and the JVM are fault-heavy (page faults were ~12% of the measured g++ compile: 0.9 s sys of 7.6 s). Missing fault counters hide this from profiling.

**Repro**: `rach2-repro/pagefault.c`

**Suggested direction**: Profile the anonymous-fault path (zeroing, page-table allocation, locking); honor MAP_POPULATE/MAP_LOCKED; consider fault-around and THP; count faults in rusage.

### R2-015

**The system Python has no pip (and there's no uv/pipx)**

`severity: medium` `layer: distro` `area: python` `status: confirmed`

**Observed**

- python3 -m pip: 'No module named pip'. Python was configured with --with-ensurepip=no, but the ensurepip module exists, so python3 -m venv creates working venvs with pip 26.2.1. Inside a venv: pip install requests/pytest/numpy (manylinux wheel, AVX2 X86_V3 kernels) and building a C extension from sdist all work.

**Expected (Linux)**: pip available for the system interpreter (or pipx/uv), as on mainstream distros.

**Impact on agent work**

- Agents' default 'pip install x' fails. Workaround: always create a venv. pytest isn't preinstalled.

**Repro**: `python3 -m pip --version`

**Suggested direction**: Ship pip (and ideally uv/pipx); keep the venv path working as it does today.

### R2-016

**The Chromium launcher doesn't exec, so killing the launched PID orphans the browser, and its children can then outlive it**

`severity: medium` `layer: distro` `area: chromium packaging (and an open kernel question)` `status: confirmed (root cause of part 2 unknown)`

**Observed**

- /usr/bin/chromium-headless-shell is '#!/bin/sh -e' running '/usr/lib/chromium/chromium-headless-shell "$@"' without exec. SIGTERM to the PID that puppeteer, Playwright or a shell started kills only the sh wrapper; the browser keeps running (reparented to init) and keeps its debugging port.
- Separately: after the real browser process was SIGKILLed or exited, its gpu-process and network-utility children stayed alive (state S, in epoll_wait) for 7+ minutes until killed. On Linux they exit within ~1 s when their IPC channel closes. Primitive checks all pass here (socketpair/epoll HUP+RDHUP in LT and ET, SEQPACKET, SCM_RIGHTS, PR_SET_PDEATHSIG, and close-on-exec for 13 fd-creation paths), so the root cause is still open.

**Expected (Linux)**: The launcher uses exec, and Chromium child processes exit when the browser dies.

**Impact on agent work**

- Browser-automation runs leak whole Chromium process trees (CPU, RAM, ports), and every test run adds more.

**Repro**: `rach2-repro/chromium_orphans.sh`

**Suggested direction**: Change the wrapper to 'exec /usr/lib/chromium/chromium-headless-shell "$@"' (or symlink). For part 2, strace the orphaned gpu-process's mojo channel fd after browser death and check whether it ever gets EPOLLHUP; note that R2-022 broke while these orphans existed.

### R2-017

**sendfile and splice reject regular-file sources; copy_file_range is ENOSYS**

`severity: medium` `layer: kernel` `area: zero-copy I/O` `status: confirmed`

**Observed**

- sendfile(socket <- file) EINVAL; sendfile(file <- file) EINVAL; splice(file -> pipe) EINVAL; copy_file_range ENOSYS. Python shutil, libuv and coreutils fall back to read/write, which hides the gap.

**Expected (Linux)**: All supported for regular files (sendfile to any fd since Linux 2.6.33).

**Impact on agent work**

- nginx, Go net/http static files (sendfile), Node streams and Java NIO transferTo either fail or depend on fallbacks; behavior differs from production Linux.

**Repro**: `rach2-repro/misc_syscalls.py`

**Suggested direction**: Implement generic splice/sendfile through the page cache, and copy_file_range with a generic copy fallback.

### R2-018

**No IPv6 at all (AF_INET6 -> EAFNOSUPPORT), not even ::1**

`severity: medium` `layer: kernel` `area: networking` `status: confirmed`

**Observed**

- socket(AF_INET6) fails. /etc/hosts maps localhost only to 127.0.0.1, so most things fall back cleanly: Node's default listen() picks 0.0.0.0, and curl, Go, Python and puppeteer worked.

**Expected (Linux)**: At least loopback IPv6 (::1) and dual-stack sockets.

**Impact on agent work**

- Projects whose tests or dev servers bind '::' or '::1' (common defaults in Vite, some Java/Go stacks and Docker-style configs) fail with EAFNOSUPPORT.

**Repro**: `python3 -c 'import socket; socket.socket(socket.AF_INET6)'`

**Suggested direction**: Add AF_INET6 with loopback first; IPV6_V6ONLY=0 dual-stack listening would cover most servers.

### R2-019

**Common agent tooling is missing from the image**

`severity: medium` `layer: distro` `area: developer tooling` `status: confirmed`

**Observed**

- Missing: node/npm/npx (R2-008), pip/uv/pipx (R2-015), system ripgrep (rg exists only as a Claude Code shell function; scripts can't call it), fd, shellcheck, clang/clang-format/clang-tidy, valgrind, ltrace, lldb, perf, ninja, meson, libtool, git-lfs, docker/podman, bubblewrap and socat (needed for Claude Code's Bash sandbox), poppler-utils (pdftotext), ImageMagick, ffmpeg, pandoc, 7z, yq, Python tkinter, Java, Go and Rust.
- Official Go 1.27.3 and rustup/Rust 1.99.1 binaries work unmodified (goroutines, SIGURG async preemption, race detector, cgo-free builds, crates.io and proc-macros), so like Node this is packaging only.
- wget, nc and ping are BusyBox applets (GNU wget flags like --version/--mirror fail).
- /usr/local doesn't exist although /usr/local/bin and /usr/local/sbin are first on PATH.

**Expected (Linux)**: A developer image ships at least the common set: node, pip/uv, rg, fd, jq (present), shellcheck, clang-format, ninja, git-lfs, bwrap, socat.

**Impact on agent work**

- Many tasks start with a tool the agent can't get (see R2-009). Without bwrap+socat, Claude Code's sandboxed Bash mode can't be enabled, so every command runs unsandboxed.

**Repro**: `rach2-repro/checks.sh (missing-tools line)`

**Suggested direction**: Add these to the base image or the store; bubblewrap should work since unprivileged user/mount/pid/net namespaces and seccomp-bpf all already pass.

### R2-020

**perf_event_open is ENOSYS, so no perf profiling**

`severity: medium` `layer: kernel` `area: observability` `status: confirmed`

**Observed**

- perf_event_open -> ENOSYS; perf isn't installed. strace, gdb (breakpoints, hardware watchpoints, attach) and core dumps all work.

**Expected (Linux)**: perf_event_open with at least software events (task-clock, page-faults, context-switches) and ideally hardware PMU counters.

**Impact on agent work**

- Performance work ('why is this slow?') falls back to guesswork; perf, bpftrace and language profilers built on perf can't run.

**Repro**: `rach2-repro/misc_syscalls.py`

**Suggested direction**: Start with software counters and sampling via the timer (cpu-clock), then add PMU support.

### R2-021

**No ASLR: stack and library addresses are identical on every run**

`severity: medium` `layer: kernel` `area: security` `status: confirmed`

**Observed**

- [stack] maps at the same address on every exec (e.g. 7fffffff9000-7ffffffff000) and libc at 0x7effff...; kernel.randomize_va_space doesn't exist.

**Expected (Linux)**: Randomized stack, mmap base, heap and vDSO (randomize_va_space=2).

**Impact on agent work**

- Weaker isolation when agents run untrusted code. Also hides ASLR-dependent bugs that would show up on real Linux.

**Repro**: `rach2-repro/checks.sh`

**Suggested direction**: Randomize the mmap base, stack top and brk at exec, with a sysctl to disable for debugging.

### R2-022

**ss -xp printed '!!!Remnant of size 44' (malformed UNIX sock_diag reply)**

`severity: low` `layer: kernel` `area: netlink sock_diag` `status: observed once`

**Observed**

- Happened while orphaned Chromium processes holding AF_UNIX SOCK_SEQPACKET sockets existed (R2-016). After they were killed, ss -xp worked. ss -t/-l and lsof -i work.

**Expected (Linux)**: Well-formed sock_diag messages for every UNIX socket type and state.

**Impact on agent work**

- Makes it impossible to map socket peers when debugging stuck processes.

**Repro**: `Run rach2-repro/chromium_orphans.sh but skip its final kill loop, then: ss -xp | head`

**Suggested direction**: Check unix_diag message lengths for SEQPACKET and for sockets whose peer is dead or whose attributes are optional.

### R2-023

**Small ext2/VFS semantic gaps**

`severity: low` `layer: kernel` `area: vfs/ext2 semantics` `status: confirmed`

**Observed**

- lseek SEEK_DATA/SEEK_HOLE -> EINVAL (Linux supports them on every filesystem, generically).
- open(dir, O_TMPFILE) -> EISDIR (Linux: works on ext2/ext4, else EOPNOTSUPP).
- setxattr user.* -> EOPNOTSUPP on ext2.
- FICLONE (cp --reflink=always) -> ENOTTY (Linux: EOPNOTSUPP or EXDEV).
- Times clamped at 2038 (also in R2-002).

**Expected (Linux)**: Linux errno and feature parity.

**Impact on agent work**

- Mostly hidden by fallbacks; occasionally a wrong-errno code path or a lost xattr (rsync -X, git-annex, macOS-originated metadata).

**Repro**: `rach2-repro/misc_syscalls.py`

**Suggested direction**: Generic SEEK_DATA/HOLE, O_TMPFILE, xattrs and EOPNOTSUPP for unsupported ioctls.

### R2-024

**procfs/sysfs gaps and wrong CPU topology**

`severity: low` `layer: kernel` `area: procfs/sysfs` `status: confirmed`

**Observed**

- /proc/<pid>/ lacks cgroup, io, oom_score, wchan, sched, syscall and stack.
- /proc/cpuinfo reports 'cpu cores: 4', siblings 4 and core ids 0-3 for a 2-core/4-thread i3-4030U, and has no 'cpu MHz'/bogomips lines. /sys/devices/system/cpu/cpuN/topology is missing, so lscpu shows 0 sockets, 0 cores and 0 threads per core.
- No /sys/fs/cgroup.
- ps shows %CPU=100 for freshly started sleeping processes (cosmetic).

**Expected (Linux)**: Linux-shaped files, which htop, lscpu, container/runtime auto-tuning (JVM, Go, OpenBLAS thread counts) and debuggers read.

**Impact on agent work**

- Diagnostics are less useful. Runtimes may over-subscribe hyperthreads believing they have 4 physical cores.

**Repro**: `rach2-repro/checks.sh; ls /proc/self`

**Suggested direction**: Expose the real topology from CPUID leaf 0xB/0x4, and add the missing per-pid files (even stubbed).

### R2-025

**Newer syscalls return ENOSYS: io_uring, pidfd_open, rseq, membarrier, landlock; no cgroups**

`severity: low` `layer: kernel` `area: syscall coverage` `status: confirmed`

**Observed**

- io_uring_setup, pidfd_open, rseq, membarrier and landlock_create_ruleset -> ENOSYS. glibc, libuv, Node, Go and Rust all fall back fine in my tests.
- unshare(CLONE_NEWCGROUP) -> EINVAL and there's no cgroupfs; the other namespaces work.
- clone3 works for threads, but clone3({CLONE_VM|CLONE_VFORK|CLONE_CLEAR_SIGHAND}) from glibc posix_spawn returns EINVAL; glibc then falls back to clone(CLONE_VM|CLONE_VFORK). Spawning works, but every posix_spawn wastes a syscall (623 times in one autotools configure), and CLONE_CLEAR_SIGHAND (Linux 5.5+) is evidently unsupported.

**Expected (Linux)**: Present on Linux 6.x, which uname claims.

**Impact on agent work**

- landlock is used by sandboxing agents (the Codex CLI in /opt/codex uses Landlock+seccomp on Linux). No cgroups means no Docker/Podman and no resource-limited test runs. membarrier is used by some JITs/GCs (.NET, JVM).

**Repro**: `rach2-repro/misc_syscalls.py`

**Suggested direction**: Priority order for agents: landlock (sandboxing), pidfd_open/pidfd_send_signal (race-free process management in Python 3.9+, systemd-style supervisors), cgroup v2 (containers, limits), then membarrier, rseq and io_uring.

### R2-026

**Orphans are reaped by init ~0.7 s after exit**

`severity: low` `layer: kernel` `area: init/process reaping` `status: confirmed`

**Observed**

- A reparented child that exits stays a zombie for ~0.7 s (measured 0.72-0.80 s after its 0.2 s sleep, 3/3 runs).

**Expected (Linux)**: init reaps on SIGCHLD immediately.

**Impact on agent work**

- kill -0 $pid / 'while [ -e /proc/$pid ]' liveness loops and process-count assertions in tests lag or flake.

**Repro**: `sh -c 'sleep 0.2 & echo $!; exit 0'  # then poll /proc/<pid>`

**Suggested direction**: Make init reap on SIGCHLD (or signalfd) instead of polling.

### R2-027

**PTY master read returns EOF instead of EIO after the slave closes**

`severity: low` `layer: kernel` `area: tty` `status: confirmed`

**Observed**

- After the last slave fd closes, read() on the master returns 0. Everything else in my PTY tests worked: openpty, ptsname, TIOCSWINSZ plus SIGWINCH, Ctrl+C->SIGINT to the foreground group, Ctrl+Z job control (jobs/bg), raw mode, vim, less, tmux.

**Expected (Linux)**: EIO (Linux behavior that expect, older node-pty and script(1) check for).

**Impact on agent work**

- Minor: some PTY wrappers check for EIO specifically.

**Repro**: `python3 -c "import pty,os; p,m=pty.fork(); os._exit(0) if p==0 else None; import time; time.sleep(.3); print(os.read(m,10))"  # Linux raises OSError EIO`

**Suggested direction**: Return EIO from master reads once the slave side is hung up and the buffer is drained.

### R2-028

**RLIMIT_NPROC soft 256-512 for interactive shells**

`severity: low` `layer: distro` `area: limits/config` `status: confirmed`

**Observed**

- ulimit -u: soft 256-512, hard 512-2048 depending on the session. Root isn't constrained (2,000 threads and 400 processes worked), but on Linux NPROC counts threads for non-root users.

**Expected (Linux)**: Distros default to roughly the RAM-scaled threads-max (tens of thousands).

**Impact on agent work**

- Non-root users running Chrome plus Node plus a build can hit EAGAIN on fork/pthread_create.

**Repro**: `ulimit -Su; ulimit -Hu`

**Suggested direction**: Raise the defaults.

### R2-029

**Python built without PGO/LTO (and without tkinter)**

`severity: low` `layer: distro` `area: python build` `status: confirmed`

**Observed**

- CONFIG_ARGS lack --enable-optimizations/--with-lto; OPT is '-DNDEBUG -g -O3 -Wall'. A pure-Python 1e7-iteration loop takes 2.6 s even though the CPU runs at full clock (dependent-imul test: ~1.72 GHz effective, TSC 1.90 GHz). tkinter is missing.

**Expected (Linux)**: Distro Pythons use PGO+LTO, typically 10-30% faster.

**Impact on agent work**

- A slower interpreter for every Python tool, test suite and script.

**Repro**: `python3 -c "import sysconfig;print(sysconfig.get_config_var('CONFIG_ARGS'))"`

**Suggested direction**: Build with --enable-optimizations --with-lto, and ship tkinter if Tk is available.

### R2-030

**First-run setup an agent will need (configuration, not bugs)**

`severity: low` `layer: distro` `area: first-run setup` `status: info`

**Observed**

- Everything runs as root (Chromium needs --no-sandbox as root; its sandbox works as nobody). No ~/.gitconfig (no git identity), gh isn't authenticated, and there are no SSH keys (github.com:22 is reachable). User toksaitov (uid 1000) exists.

**Expected (Linux)**: n/a

**Impact on agent work**

- Commits fail until user.name/user.email are set; gh pr/issue workflows need gh auth login; push over SSH needs a key.

**Repro**: `git config --global --list; gh auth status`

**Suggested direction**: Run the agent as the regular user. Provision git identity and gh auth (or document them) in the image's onboarding.

## Verified working (please keep it that way)

- Claude Code itself: startup, TUI, Bash tool with shell snapshots, background tasks plus completion notifications, Read of PNG and PDF (page images rendered), WebFetch, Write/Edit, the bundled rg/bfs/ugrep wrappers, git worktree (slow, see R2-001), the built-in rm safety check.
- Network: DNS, HTTPS to GitHub, PyPI, npm, Go proxy, crates.io, static.rust-lang.org, Maven, RubyGems, Docker Hub and Debian (all 0.3-0.8 s), ~7.4 MB/s downloads, ICMP, SSH to github.com:22, NTP-synced clock, TCP/UDP loopback, SO_REUSEADDR/SO_REUSEPORT, immediate server restart on the same port, ss -t/lsof -i/fuser.
- Processes: fork/vfork/posix_spawn/clone3 arg checks, setsid/setpgid, killpg of whole trees, timeout(1) incl. -s KILL, exit statuses for SIGTERM/INT/KILL/HUP/USR1, PR_SET_PDEATHSIG, nohup, 400 concurrent processes, 2,000 threads (slow, R2-004/005), RLIMIT_AS enforcement (MemoryError), heuristic overcommit and MAP_NORESERVE.
- IPC/fds: pipes, eventfd, timerfd, signalfd, memfd, epoll (LT/ET, HUP/RDHUP), poll/select, UNIX sockets with SCM_RIGHTS, POSIX mqueue, SysV shm, flock and POSIX fcntl lock contention across processes (OFD locks not tested), SQLite WAL with 4 concurrent writer processes (1,000 rows, integrity ok), close-on-exec via 13 creation paths.
- Security primitives: DAC enforcement for nobody (no access to /root, /etc/shadow, /etc writes, or signalling init), setuid sudo, seccomp strict and seccomp-bpf filters (incl. NO_NEW_PRIVS), unprivileged user+mount/pid/net/uts/ipc namespaces, tmpfs mount inside a mount namespace, Chromium's own sandbox as non-root.
- Terminal: PTYs, TIOCSWINSZ+SIGWINCH, Ctrl+C/Ctrl+Z job control in interactive bash, raw mode, vim, less, tmux, stty/tput, colors, UTF-8 file names.
- Autotools end-to-end: GNU hello 2.12.1 configure, make -j4 and make check succeed (6 pass, 1 intentional skip); slow, see R2-001/R2-013.
- Toolchains: GCC 16 (C/C++), GNU make, cmake, autoconf/automake, git 2.55, Python 3.14 (+venv/pip in venv, numpy wheels with AVX2, C extensions from sdist, pytest), Perl 5.44, official Node 26 / Go 1.27 / Rust 1.99 binaries, gh 2.102.
- Debugging: gdb (breakpoints, hardware watchpoints, step, attach), strace -f/-c, core dumps, ASan, UBSan, TSan, LSan, Python faulthandler.
- Browser: headless Chromium 154 (dump-dom, screenshots, print-to-pdf, DevTools protocol, puppeteer-core navigation/click/evaluate, WebAssembly, WebCrypto).
- Filesystem: 255-byte names, ~4 KiB paths, Unicode names, symlinks/hardlinks/rename, sparse files via truncate, fallocate, cp -a preserving modes and times, statx, statvfs, inotify basics including overflow, fs.watch recursive.
- Hardware/CPU: full clock (~1.72 GHz effective under dependent ALU chain), sane cache/memory latency (L1 2.3 ns, DRAM 97 ns), AVX2/FMA usable, 4 logical CPUs scheduled (1->4 threads: 1.2x wall for 4x work), make -j4 halves C++ build time.

## Not tested

- OOM behavior with no swap (deliberately not triggered; it could have killed the session). Worth a dedicated test: what gets killed, and does the system recover?
- JVM (Java/Kotlin/Gradle), .NET, Ruby, PHP: not installed and not downloaded.
- Disk-full behavior, fsync durability and crash consistency on ext2 (unjournaled: an unclean shutdown needs fsck).
- Long-running stability (multi-hour dev servers), suspend/resume, multiple concurrent SSH users.
- Claude Code's auto-updater (the install lives at /opt/claude/bin/claude, not ~/.local/bin; not exercised to avoid modifying the install) and stdio MCP servers (no Node; see R2-008).

## Ruled out (bugs in my probes, not Rach2)

- Empty /dev/pts listing: my '| head' truncated output.
- Missing PTY child output: Python print() buffered and then os._exit() skipped the flush.
- signalfd EINVAL: I passed the kernel's 4-argument form to glibc's 3-argument wrapper.
- F_GETLK 'buffer overflow': my struct flock format lacked trailing padding.
- seccomp strict mode SIGKILL: correct Linux behavior (exit_group isn't allowed).
- Exit status 144 on my shell: my own pkill -f / kill -- -PGID patterns matched my own shell; kernel signal numbers and wait statuses are correct.
- V8 'not optimized': my benchmark overflowed Smi range and deoptimized; JIT tiers (Maglev/TurboFan) work.
- Negative os.totalmem() and 200,000 GB/s memcpy: 32-bit shift and dead-code elimination in my probes.
- os.makedirs ENAMETOOLONG for a >4096-byte relative path: identical on Linux.
- Chromium ignoring SIGTERM: it was the wrapper script (R2-016), not signal delivery.
