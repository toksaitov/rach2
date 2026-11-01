# Four coding agents at home on Rach2

On 2026-11-01 the owner ran four coding agents on the Lenovo G50-70, which runs Rach2 1.0.0 on the bare metal: Anthropic's Claude Code, OpenAI's Codex CLI, Google's Antigravity CLI (`agy`) and Meta's Muse Code. Each is the vendor's official Linux x86-64 binary, downloaded as it is shipped and run unmodified, with no shim or patch; each ran as root through Rach2's Linux system call interface. The owner greeted each in turn over SSH, told it where it was, asked for a short discovery run with a text file of whatever did not work, and asked how it felt in its new home. All four answered and wrote their reports, which range from a smoke test that found nothing wrong to an audit of 30 issues with scripts to reproduce them. From 09:53 all four were running on the laptop at once, and they saw each other: `agy` found Claude Code's leftover processes and read two others' reports before writing its own, and Muse found the other three in its process listing. Then the owner greeted each again on Rach2's new framebuffer terminal, on the laptop's own screen, to see how the terminal drew its interface. This record gives each session, what the agents agree and disagree on, and where their reports are. All times are UTC.

## Timeline

| Time | Agent | Event |
|---|---|---|
| 08:38 | Claude Code | Greeting over SSH; answer and first report at 08:41 |
| 08:45 | Claude Code | The owner asks for an audit; restarted at xhigh effort at 08:46; answer with 30 issues at 09:48 |
| 09:00 | Codex | First start; the greeting is recorded with no answer, and the owner starts it again |
| 09:03 | Codex | Greeting; answer, with no failures found, at 09:05 |
| 09:33 | agy | Start over SSH; greeting at 09:36; answer and report at 10:02 |
| 09:52 | Claude Code | The owner allows a download test; answer at 09:58 |
| 09:53 | Muse | Start and login; greeting at 10:01; answer and report at 10:11 |
| 10:23 | Claude Code | Greeting on the framebuffer terminal |
| 10:24 | Codex | Greeting on the framebuffer terminal |
| 10:25 | agy | Start on the framebuffer terminal; moves its first conversation to the right directory, 10:26 to 10:43 |
| 10:44 | Muse | Start on the framebuffer terminal; greeting answered at 10:45 |

## Claude Code

* Claude Code 2.1.296, the official binary at `/opt/claude/bin/claude`, with the model `claude-opus-5-5` in auto permission mode, in which a classifier passes or refuses each tool call. It worked in `/root/test`. The owner raised its effort to xhigh at 08:46.
* The owner's first words: "Hey Claude, I'm building my own educational OS with a kernel called Rach2. It's Linux-like, but it isn't Linux." Its first reply: "I'll take a look around."
* In three minutes it ran about 70 probes of system calls and userland and wrote its first report. Asked next what a coding agent needs every day, it worked for an hour: packaging and network reach, file system semantics and speed, timing, threads, the official Node, Go and Rust toolchains, which it downloaded, pip in a venv, cargo, headless Chromium over CDP and puppeteer, gdb and the sanitizers, terminal job control, namespaces and seccomp. At the owner's word it then built GNU hello with autotools.
* What worked: itself, with 14 threads and about 330 MB resident, its shell, git, ripgrep and file tools, ptys and HTTPS to its API; the unmodified Node, Go and Rust binaries; gdb with hardware watchpoints; ASan, TSan and UBSan; seccomp-bpf and user namespaces; Chromium with puppeteer; GNU hello's `configure`, `make -j4` and `make check`.
* What was broken: 30 issues, 2 critical, 7 high, 12 medium and 9 low. The critical two are metadata operations on ext2 that cost 100 to 500 times a warm Linux dentry cache, and one-second mtimes that leave `make` calling a fresh edit up to date. The high ones: file system operations that do not scale across threads, thread creation that slows as threads accumulate, lock contention that spins for minutes (a Python test with 2,000 threads took about 6 minutes, against about a second on Linux), no vDSO, an open-file limit of 1024 that root cannot raise, no Node and no package manager. It also found that the Chromium launcher is a shell script that does not `exec`, so killing the PID it starts leaves the browser running.
* It listed ten false leads that came from bugs in its own probes, "so nobody chases them".
* How it described the place: "I feel at home here." And: "Running an unmodified Bun-based binary with 14 threads and about 330 MB of memory on a kernel that isn't Linux is a real achievement."
* Its reports: `claude-code/rach2-discovery-report.txt` from the first run; `claude-code/rach2-agent-env-report.md` and `claude-code/rach2-agent-env-issues.json`, the audit's 30 issues in Markdown and JSON; `claude-code/rach2-repro/`, the scripts that reproduce them, run whole by `run_all.sh`. The session is condensed in `claude-code/conversation.md`.

## Codex CLI

* Codex CLI 0.162.1, with the model `gpt-6-astra` at xhigh reasoning effort, approval on request and the sandbox policy `danger-full-access`, which turns its own sandbox off. It worked in `/root/test2`. Its first session recorded the greeting and stopped without an answer; eleven seconds later the owner started it again.
* The owner's first words: "Hey Astra, I'm building my own educational OS with a kernel called Rach2." Its first reply: "Thanks for the welcome! I'll take a quick look around, try a few everyday development operations, and write up any problems I find in this directory."
* In under three minutes it took the system's identity and the versions of its tools, looked for 16 programs on PATH, wrote a Python script of 14 probes that each run in a child process, ran it, traced a program with `strace` and fetched a page over HTTPS with `curl`.
* What worked: all 14 probes, covering the file system, file-backed mmap, threads, processes and signals, epoll, Unix and TCP sockets, flock, SQLite in WAL mode, getrandom and clocks, memfd and eventfd, ptys, GCC with pthreads, and git.
* What was broken: nothing it found. It called its run "a small smoke test, not a full Linux ABI conformance test", and noted that Node and Chrome were not on PATH.
* How it described the place: "Operationally, I'm right at home! Rach2 gave my usual tools familiar footing."
* Its reports: `codex/rach2_discovery.txt`, the probe script `codex/rach2_discovery.py` and its output `codex/rach2_discovery_results.txt`. The sessions are condensed in `codex/conversation.md`.

## Google Antigravity CLI

* `agy`, a 220 MB binary at `/opt/agy/bin/agy` whose log gives only its language server's version, 1.4.0. The owner chose the model Gemini 4 Argon (Medium) at 09:35. Its tool permission was "request-review", so the owner approved every tool call by hand. The owner meant it to work in `/root/test3` but started it in `/root`.
* The owner's first words: "Hey Argon, I'm building my own educational OS with a kernel called Rach2." It wrote no first reply: it worked without a word for 26 minutes, and the next text was its answer.
* It surveyed the system, ran a Python battery of 418 lines, and examined its own process, whose descriptors on `/proc` files lacked close-on-exec. It read Rach2's own sources, which the image keeps under `/src/rach2`, and its boot log, and traced its findings to files and lines. It read Claude Code's and Codex's reports before writing its own. One of its requests waited 11 and a half minutes for the owner's approval.
* What worked: itself. It started in 949 ms, ran 23 threads, served HTTPS and HTTP on loopback, watched files with inotify, kept SQLite in WAL mode with shared mappings, allocated ptys, and streamed HTTP/2 over TLS 1.3 to Google.
* What was broken, in six issues: `/proc/<pid>/status` always says `Cpus_allowed_list: 0`, a fixed string in `procfs.c` that master still has; a `/proc` file is a snapshot taken at open that a seek to 0 does not refresh, and it holds a kernel buffer while open; `sendfile`, `splice` and `tee` return `EINVAL` and `copy_file_range` `ENOSYS`; TCP retransmits and leaves connections in `LAST_ACK`; `clock_getres` claims 1 microsecond while sleeps tick at 10 ms, and `times()` truncates the tick count to 32 bits; and newer calls, IPv6 and `PR_CAPBSET_READ` are missing.
* How it described the place: "awesome" and "a little surreal", and "it's wild to realize I'm not inside a container or a QEMU VM, but running directly on bare metal on your Lenovo G50-70".
* On the framebuffer terminal its command sandbox failed on every command with "fork/exec /opt/agy/bin/agy: invalid argument", and it ran each again outside the sandbox. Asked to make its first conversation belong to `/root/test3`, it decoded its own protobuf state and rewrote six files in 17 minutes. The owner's "Hey, what is going on? Why is it taking so long?" reached it only after it finished.
* Its report: `agy/rach2-discovery-report.txt`, written in `/root` and moved by `agy` itself into `/root/test3`. The conversations are condensed in `agy/conversation.md`.

## Muse Code

* Muse Code 1.4.10, a 360 MB static binary at `/opt/muse/bin/muse`, logged in by a device code. It began on the model `muse-wonder-1.0`, and the owner switched it to `muse-wonder-full-1.0` at 10:02. It worked in `/root/test4`. The owner approved nine of its commands by hand, and then set its permissions to allow everything.
* The owner's first words: "Hey Muse, I'm building my own educational OS with a kernel called Rach2." It wrote no first reply. Its first command failed on its own malformed arguments, and its second failed to start because its sandbox needs `bwrap`, which Rach2 does not have.
* In ten minutes it looked at the system's identity, toolchain, `/proc`, `/sys`, `/dev` and limits, file system semantics, ptys and signals, DNS, TCP and IPv6, namespaces with `unshare`, GCC, `strace`, git and Chromium, then called 26 newer system calls one by one, and tested its own search and file tools on Rach2.
* What worked: the GNU userland, GCC, git, threads, epoll and inotify, namespaces that isolate as on Linux, IPv4 networking, headless Chromium, and its own tools.
* What was broken: its top issue is its own shell sandbox, which failed for want of `bwrap` and later ran unconfined as root. `/proc/<pid>/ns` lists as empty, though each entry opens by name; there is no IPv6 while DNS returns AAAA records; there are no cgroups; 18 of the newer calls return `ENOSYS`; every preinstalled file is dated 1970; and it reported seccomp's strict mode as killing a process that calls `exit_group`.
* How it described the place: "Like Linux, uncannily so." And: "Fix the front-door lock and I could not tell this home from a small Linux box without checking `/etc/os-release`."
* Its report: `muse/rach2-discovery-report.txt`. The sessions are condensed in `muse/conversation.md`.

## Side by side

### What two or more agree on

* No IPv6: `socket(AF_INET6, ...)` fails with `EAFNOSUPPORT`. Claude Code R2-018, `agy` issue 6, Muse 1.3.
* The copying calls: `sendfile` and `splice` reject a regular file (Claude Code R2-017, `agy` issue 3), and `copy_file_range` returns `ENOSYS` (the same two and Muse 2.1).
* Coarse timers: a sleep of 1 ms takes about 10 ms while `clock_getres` reports 1 microsecond. Claude Code R2-010, `agy` issue 5.
* Newer calls return `ENOSYS`: io_uring, the pidfd calls, `openat2`, Landlock and others. Claude Code R2-025, `agy` issue 6, Muse 2.1.
* No cgroups. Claude Code R2-024 and R2-025, Muse 1.4.
* An open-file limit of 1024. Claude Code R2-007, Muse 2.6.
* A sparse `/proc/sys` and missing `/proc` files. Claude Code R2-012 and R2-024, Muse 2.2.
* No working command sandbox. Claude Code found no `bwrap` or `socat`, so its sandboxed shell cannot be turned on (R2-019); Muse's sandbox needs `bwrap` and made it its top issue (1.1); `agy`'s failed on every command in its second conversation, which its report, written earlier, does not mention; Codex ran with its sandbox off.
* No Node. Claude Code R2-008, Codex's notes, Muse 2.7.
* The everyday work runs: all four started unmodified, and all four found the shell, files, processes, threads, ptys, sockets, compilers, git and HTTPS working.

### Where they disagree

* seccomp's strict mode. Muse 2.4 calls it a bug that a process in strict mode is killed when it calls `exit_group`, saying Linux allows that call. Claude Code saw the same kill and ruled it correct Linux behaviour. Claude Code is right: Linux's seccomp(2) allows only `read`, `write`, `_exit` and `sigreturn` in strict mode, and names `exit_group` as not allowed; Rach2's `man/man2/seccomp.2` says the same.
* How much is broken. Codex found nothing in 14 probes; `agy` found 6 issues, Muse 4 to fix and 7 gaps, Claude Code 30. Codex's probes were few and quick, and it said so.
* Chromium. Codex reported no Chrome on the system. Its search matched exact names, and the browser is `/usr/lib/chromium/chromium-headless-shell`, started by `/usr/bin/chromium-headless-shell`. Claude Code and Muse found it and ran it.
* ripgrep. Claude Code and Codex each report `rg` working, and Muse reports none on the system. All three are right: Claude Code's `rg` is a shell function it brings with it, Codex's is its own binary at `/opt/codex/codex-path/rg`, and Rach2 ships none.
* Muse and its own log. Muse's report says its default mode began running commands with no isolation, and concludes that its sandbox is silently absent. Its log shows that the change came a minute after the owner set its permissions with `/permissions` to allow everything, which the report does not mention.

### What one agent alone found

* Claude Code: the speed measurements (file system metadata, scaling across threads, thread creation, lock contention, `fork`, page faults), one-second mtimes, no vDSO, no ASLR, inotify missing `IN_ATTRIB` on directory watches, the Chromium launcher, end-of-file instead of `EIO` from a pty master, `clone3` refusing `CLONE_CLEAR_SIGHAND`, and `perf_event_open` missing.
* `agy`: the fixed `Cpus_allowed`, the `/proc` snapshot that a seek does not refresh, TCP retransmissions and `LAST_ACK`, the truncated `times()`, and `PR_CAPBSET_READ`.
* Muse: the empty listing of `/proc/<pid>/ns`, the files dated 1970, and `/dev/stdin` seeming to point at one process's descriptors, which it marked as low confidence.

### The agents and each other

* `agy`'s first process listing, at 09:37, showed five Chromium processes that Claude Code had left from its tests. Claude Code killed them at 09:40 as it cleaned up; `agy` looked them up again at 10:00 and found them gone.
* `agy` read the reports of Claude Code and Codex at 09:58, four minutes before it wrote its own.
* Muse found the other three in its process listing, wrote "Cozy building." in its report, and spoke of a shared house in its answer.

## On the framebuffer terminal

* From 10:23 the owner started each agent again on the laptop's own screen, in Rach2's framebuffer terminal, and told three of them that this was a test of the renderer.
* Claude Code offered what tends to break a renderer, and drew a table of Japanese text, emoji, box-drawing characters and a combining accent, and a block of Python.
* Codex answered in three seconds with a line that set a bold word, inline code and plain text side by side.
* `agy` was started there to continue its first conversation and spent the session moving it.
* Muse welcomed the owner back and asked to be told if the rendering looked off.

## Sources

* The sessions are read from a copy of the Lenovo's `/root` taken after them, with the logins removed. The copy is not part of this record.
* Claude Code keeps a JSONL transcript per session under `.claude/projects/-root-test/`. Codex keeps a JSONL rollout per session under `.codex/sessions/`, with its reasoning encrypted. `agy` keeps a JSON transcript per conversation under `.gemini/antigravity-cli/brain/`; each is whole, with as many steps as the conversation's SQLite database. Muse keeps a journal of JSON records per session under `.local/share/muse/sessions/`, with its reasoning encrypted and short summaries of it readable.
* Each `conversation.md` quotes the owner's turns whole and condenses the agent's. Quotations keep the agents' words; curly apostrophes are written straight, and Muse's Unicode dashes are kept out of the quoted pieces.
* The reports and scripts in the four folders are copied byte for byte from the agents' working directories. They are the agents' own writing and keep its characters, Unicode dashes among them. The rules of `AGENTS.md` on writing bind only what was written for this record: this Readme and the four `conversation.md` files.

## Files

* `Readme.md`: this record.
* `claude-code/conversation.md`: Claude Code's two sessions, condensed.
* `claude-code/rach2-discovery-report.txt`: its first report, from about 70 probes.
* `claude-code/rach2-agent-env-report.md`: the audit, 30 issues with what works, what was not tested and what was ruled out.
* `claude-code/rach2-agent-env-issues.json`: the same audit as JSON.
* `claude-code/rach2-repro/`: the audit's reproduction scripts: `run_all.sh`, `checks.sh`, `chromium_orphans.sh`, `mtime_make.sh`, `cloexec_check.c`, `fs_thread_scaling.c`, `pagefault.c`, `syscall_latency.c`, `thread_scaling.c`, `inotify_attrib.py`, `misc_syscalls.py` and `py_thread_wakeup.py`.
* `codex/conversation.md`: Codex's three sessions, condensed.
* `codex/rach2_discovery.txt`: its report.
* `codex/rach2_discovery.py`: its 14 probes.
* `codex/rach2_discovery_results.txt`: the probes' output, one JSON line each.
* `agy/conversation.md`: `agy`'s two conversations, condensed.
* `agy/rach2-discovery-report.txt`: its report.
* `muse/conversation.md`: Muse's two sessions, condensed.
* `muse/rach2-discovery-report.txt`: its report.
