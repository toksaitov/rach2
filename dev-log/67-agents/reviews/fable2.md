**Review of `build/flow/coordinator/fixup67/plan.md`, second round, against `origin/milestone/66-terminal` (143b25719).**

**1. OpenCode, tasks 1 to 3**

1. The recipe pattern is as the research says (`fetch_agents.sh:14-35`; `agy.sh:35-38` for profile.d; `claude.sh:75-83` for a Bun check skipped off x86). One correction: no recipe fetches a checksum file. `gh.sh:4-10` and `codex.sh:9,26` pin the digest the file lists, and `fetched` checks the tarball against it at every use (`fetch_agents.sh:118-123`). A `package_verify` that checks the tarball's line in a pinned `SHA256SUMS` adds nothing. Drop the download; pin the tarball's digest in FILES as the others do.

2. Task 3 cannot pass as written. The acceptance keeps each credential file's SHA-256 before the sessions and requires it unchanged after the reboot (`agents_accept.sh:686-689,733-737`), takes every run of 16 or more token characters in the file as a secret to strip from each screen it reads (`agents_guest.sh:150-156,158-193`), and requires the log to hold none (`:764-767`). OpenCode v2's "credential file", `opencode.db`, is the server's SQLite store, which every session writes, and a database yields thousands of such runs, among them words the screens show. Change: give each agent a credential source, a command that prints its secrets, in place of a file, and digest that output; make the rewrite of `agents_accept.sh:35-60` and `agents_guest.sh:125-156` a named subtask of 3, with the Readme paragraph (`vendor/gnu/agents/Readme.md:20`). This is the one argument for v1.19.5 (`auth.json`); the contract fix is cheaper than a legacy line. Keep v2.2.5.

3. Task 2 omits the host verifier. `agent_native.c:517-534` knows each agent's digest by name (claude, agy, muse written in; codex from the `codex-digest` artifact that `mk/gnu-agents.mk:54-68` makes), and `:291-293,726,760-761` take any name that is not codex, claude or agy for Muse. Add `tests/cases/machine/agent_native.c` and a digest rule in `mk/gnu-agents.mk` to task 2's Owns.

4. Kernel gaps, settled by code: robust process-shared mutexes have robust lists (`futex.c:2,31-49`) and shared futexes keyed by physical page (`futex.c:119-142`); `set_robust_list`, `statx`, `flock`, `memfd_create`, `getrandom`, `prlimit64`, `close_range`, `epoll_pwait2`, `splice` and `sendfile` are in the table (`arch/x86_64/systab.c:165-384`); `/tmp` is a 1777 directory of the ext2 root (`os-real-gnu.manifest:245`), not a noexec tmpfs, so Bun's extraction runs and that risk falls away; the tree has 262 directories, so two recursive watchers take some 500 of 8192 watches (`inotify.c:28-30`), lower than the research's "High"; 1024 descriptors (`fd.h:24`, `syscall_proc.c:5385`) is Linux's default soft limit. Nothing in tasks 1 to 3 need wait on a kernel task; keep `OPENCODE_DISABLE_FFF=1` as the fallback.

5. "On term" in task 3's Done: nothing reads the Lenovo's screen. `agent_accept.c` reads tmux over SSH; only `termconsole_scenario.c:6-15` reads term, through QMP in QEMU. term already answers DA1, XTVERSION, DECRQM 2026 and the Kitty query (`machine.termconsole.json:8`), the queries OpenTUI sends. Make "on term" an owner's look, not a Done.

6. Task 1's Done records start time and memory in the Readme, but those come from the rows' `sample_memory` (`muse.sh:93-104`), which is task 2's. Move it.

7. Pin and service: no objection to v2.2.5 and the vendor's default. v1 is legacy, downloads LSPs and has no checksum file; seeding standalone needs a per-user file or a wrapper, against "unmodified".

**2. The split**

8. `rlimit_set` is one function: task 7 replaces its `proc_list` walk (`syscall_proc.c:5388-5393`), task 9 its NOFILE cap (`:5385`). Give the function to 9, or let 9 wait on 7 for that line.

9. The sysctl table is one array whose parents are indices, with a writable list (`synthfs.c:190-240`; `:190` says the `SYNTH_SYS_` names stay). Task 9 (`fs.nr_open`, `fs.file-max`) and task 16 (`kernel.randomize_va_space`) append to it while 10 rewrites it, and neither waits on 10. Let 10 own every entry, the backing variables declared by 9 and 16, or let 9 and 16 wait on 10 for the sysctl line as 12 waits for the proof.

10. Task 8's list of 100 Hz assumptions misses `futex.c:84,103` (`futex_ticks`, `futex_ticks_until`), which task 6 owns. Let 6 make them read `TICK_HZ` first, or 8 waits on 6.

11. Tasks 13 and 17 both own the Chromium launcher. Give it to 13; 17 traces orphans only, after 13's link, since the wrapper is the orphan the report shows (`chromium_orphans.sh:3-5`).

12. Task 9's Done needs task 13's Node. Judge with `prlimit`, `ulimit -n` and the limitprobe instead.

13. Task 6's Done: `py_thread_wakeup.py:5-8` prints `create` and `set+join` apart; creating 500 threads is task 7's cost. Judge `set+join`.

14. Task 14's Done cannot hold: `misc_syscalls.py` also probes AF_INET6 (task 18), utime 2040 (task 5), pidfd_open (task 15), xattrs, perf_event_open, io_uring, membarrier and landlock (set aside), and prints FAIL on ENOSYS (`misc_syscalls.py:1-7,13,16,19-27`). Name the lines 14 owns and the expected errno of the rest.

15. Too large: task 11 is the 7-8 subtasks the first round split into procfs and topology; split again (`cpuinfo.c`, sysfs). Task 12 is 3-4 kernel subtasks, two ports, three agents' settings, denial tests and a Lenovo proof, nine or more; split into 12a (kernel, bubblewrap, QEMU proof) and 12b (socat, settings, man pages, Readme Limits, Lenovo), 12b waiting on 12a.

16. Three tasks edit `vendor/gnu/agents/Readme.md`: 1 (table, Design, Limits), 12 (Limits `:118-122`, Design `:73,91`), 13 (table, Design). Name one owner of Limits; the others append. Add to the hot-file list: `futex.c` (6, 8), the sysctl table (9, 10, 16), `rlimit_set` (7, 9), `agent_native.c` (2).

**3. Now wrong**

17. "Fifteen start at once; five wait, each on one task": 14 start; six wait (2, 3, 8, 12, 16, 19), and 12 waits on two.

18. Task 20 cites `rach2-install:1113`, the put-back side; the keep side is `:984-994`, where only `.ssh` of a home outside `/home` is kept. Both change. `machine_logins.sh` must name opencode's store beside the five files of `agents_guest.sh:140-143`.

19. Task 15's `syscall_proc.c:2550` is now `:2542` (the `>> 31 >> 1` check, which also refuses `CLONE_PIDFD`). Cosmetic; every other citation holds at 143b25719.

No objection to the rest.