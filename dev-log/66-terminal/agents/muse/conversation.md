# Muse Code on Rach2: the conversation

Muse Code 1.4.10 (build 98c762468d), Meta's coding agent, is a 360 MB static binary at `/opt/muse/bin/muse`. It ran on the Lenovo as root in `/root/test4` in two sessions: the discovery run over SSH from 09:53 to 10:21, which Muse named dusk-gibbous, and a greeting on the framebuffer terminal from 10:44 to 10:49, named golden-proteus. The run began on the model `muse-wonder-1.0`; the owner switched it to `muse-wonder-full-1.0` with `/model` at 10:02, and the report names the latter. Muse also ran background observers on `muse-spark-1.3` that judged whether to remind the main agent of a skill; they never did, and the owner saw nothing of them.

Times are UTC. The owner's turns are quoted whole. Muse's turns are condensed to what it did and said; its own words are in quotation marks. Its reasoning is stored encrypted; the short summaries Muse shows of it are readable, and one is quoted. The sources are `.local/share/muse/sessions/2026/11/01/*/session.jsonl` and the `subagent/` sessions beside them, journals of JSON records, in the copy of the Lenovo's `/root`.

## Session 1, over SSH, 09:53 to 10:21

Muse started at 09:53:53. Its login, by a device code entered elsewhere, took two minutes and finished at 09:55:55. The owner sent the greeting at 10:01.

### 10:01, the greeting

> Hey Muse, I'm building my own educational OS with a kernel called Rach2. It's Linux-like, but it isn't Linux. We already have the GNU stack and headless Chrome working, and now... ta-da! You're running here too, without a single change to your code: just the official binaries, downloaded as-is. It all works because Rach2 provides a Linux-compatible system call interface.
>
> Welcome to the new OS! Please do a short discovery run: look around the environment, and if anything doesn't work for you, write it up in a text file in this directory so we can fix it. And tell me: how do you feel in your new home?

Muse said nothing until its answer ten minutes later. What it did:

* 10:01 Its first shell call failed on its own arguments, the string `"null"` where Muse's tool takes a number: "invalid type: string "null", expected u64".
* 10:01 Its second failed before starting, in its sandbox: "sandbox enforcement unavailable: Bubblewrap candidate `/opt/muse/bin/muse` failed --help capability validation", with no `bwrap` on PATH. Its tool added: "Do not retry and do not fabricate command output".
* 10:02 It asked to run outside the sandbox, and the owner approved each of the next six commands by hand: `uname`, the identity and environment; the shells, tools and versions; `/proc`, `/sys`, `/dev` and the limits; `bwrap`, the namespaces in `/proc/self/ns`, `/proc/self/status` and cgroups; a Python smoke test of fork, threads, sockets, mmap and signals.
* 10:02 The owner ran `/model` and chose `muse-wonder-full-1.0`.
* 10:03 The turn stopped, cancelled during a model step.

### 10:04

> Go on

* 10:04 The owner approved the next command, and then Muse's own tool refused it for starting a process with `&`: "unmanaged shell backgrounding is not allowed". Run again without it, and approved again, the command looked at `stat` of system files (all dated 1970), hard links, symbolic links and `rename`.
* 10:05 Checked its pty, signals and job control, inotify and more of `/proc`. This was the ninth command the owner approved by hand.
* 10:05 The owner ran `/permissions`. Muse's log records the result at 10:06:01: approval "allow_all", the file system "unrestricted", the network enabled, no reviewer.
* 10:06 Its next request to run outside the sandbox was refused: "unsandboxed execution requires human approval, but approval prompts are disabled". It tried its default mode, which now worked, and found itself root with the full mount table; a test write to `/etc/.probe-nope` succeeded, and it removed the file. It concluded that its sandbox fails or is silently absent. It did not connect the change to the owner's `/permissions` a minute before.
* 10:07 DNS and outbound TCP (working), IPv6 (failing at `socket()`), then the namespaces: `unshare -n`, `unshare -m` with a tmpfs and `unshare -U -r` all isolated as on Linux. `/proc/<pid>/ns` listed as empty, though each entry opened by name.
* 10:08 Counted the entries of `/proc` and `/sys` directories, built and ran a C program with GCC, traced with `strace`, made a git repository, and ran Chromium.
* 10:08 Called 26 newer system calls with null arguments from one Python process. It printed nothing: the process died with its output still buffered. Muse ran the calls again, each in a forked child, and the `seccomp` child was killed with `SIGKILL`; a last batch tried `seccomp` in filter mode, which returned `EFAULT`. Its summary at 10:09: "Verifying seccomp strict mode behavior and confirming SIGKILL on disallowed syscalls is expected."
* 10:09 Looked at init, the users, the locale, the time zone and the kernel log.
* 10:09 Tested its own tools on Rach2: search, `write_file`, `read_file` and `edit_file`, all working.
* 10:11 Wrote `rach2-discovery-report.txt`, corrected one line, and removed its scratch files.

Its answer, 10:11:54:

"Top finding: I run fine here unmodified", it began, and named "the shell sandbox" the one real blocker: "There is no `bwrap` on the system. My first sandboxed shell call refused to start. Later, default mode began succeeding with no isolation at all (root, full mounts, writes to `/etc` allowed). So the sandbox is either jammed shut or silently absent." It listed the other items to fix first: the empty `/proc/<pid>/ns/`, no IPv6 while DNS returns AAAA records, no cgroups, and newer calls returning `ENOSYS`. "What works is the long list." Then: "How does it feel? Like Linux, uncannily so." It called the machine a shared house whose layout it already knew, with Claude and Codex in the same `ps` listing, and ended: "Fix the front-door lock and I could not tell this home from a small Linux box without checking `/etc/os-release`."

The session ended cleanly at 10:21:53.

## Session 2, on the framebuffer terminal, 10:44 to 10:49

### 10:45

> Hi again, I am checking the terminal renderer of Muse Code. Last time I was running you in SSH, now it is a real hardware terminal.

Its answer, eight seconds later, welcomed the owner back on the hardware terminal: "Let me know if the rendering looks off or if there's anything you'd like me to do from here." The session ended at 10:49.
