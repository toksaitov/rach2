# Milestone 61: coding agents on Rach2, unmodified

## Goal

A person signs in to Rach2 over SSH and works with a coding agent there: Claude Code, OpenAI's Codex, Google's Antigravity CLI (`agy`) or Meta's Muse Code (`muse`). The agent reads and edits files, runs shell commands, uses git and talks to its vendor's service. The project's own loops (`tools/flow/`) run on Rach2 as well, with GitHub's `gh`. Every program is the vendor's published build, run unmodified and under its license; where a program and Rach2 disagree, the kernel changes.

## The owner's decisions

These were made on 2026-10-17.

* No program is modified, patched, preloaded or wrapped to hide a kernel fault. A fault the programs find is a kernel fault and is fixed in the kernel.
* The first release is one agent session at a time, over SSH and in tmux, on the x86-64 GNU root, as an ordinary user. Each agent runs with its own sandbox off, by the setting its vendor documents; the sandboxes, which need bubblewrap, are a later milestone.
* The build fetches each program for the person who builds the image, by a recipe with a pinned version and digest; nothing is redistributed, and each person signs in with their own account. The owner's image enables them.
* The owner's accounts are used for every program. The owner signs in on the Lenovo after the release; no task of this milestone needs a sign-in or a real credential.

## What was found

On 2026-10-17 Claude Code 2.1.296 and Codex 0.162.1 ran on the Lenovo (Core i3-4030U, 16 GB, release `01acc22d`) and in QEMU on the testing host.

* Both start on the x86-64 GNU root. The guest resolves names, its curl reaches both APIs over TLS, and Codex completes a request to OpenAI end to end: with an invalid key it gets the same 401 as on Linux, over WebSocket and then HTTPS. Codex's screen draws in tmux and offers sign-in by a device code.
* Claude Code stops at once with `SyntaxError: Invalid character: '\0'`. Bun, its runtime, calls `madvise(MADV_DONTNEED)` over 47 MB of the `.bun` section of its own writable `PT_LOAD` segment. Linux maps that segment privately from the file and reads a dropped page back from it; Rach2's `elf_load_image_at` copies each segment into anonymous frames, so the page comes back zeroed. A preload that ignores the call made Claude Code behave as on Linux, which proves the cause; it is not a fix.
* With that preload, Claude Code's Bash tool fails: Rach2 follows a `/proc/self/fd/N` link only as the last component of a path, so `mkdir("/proc/self/fd/17/x")` gives `ENOTDIR`.
* Codex's sandbox runs its bundled `bwrap` through `/proc/self/fd/3`, and Rach2 refuses `execve` of a `/proc/self/fd/N` path with `EACCES`. `bwrap` then needs `PR_CAPBSET_READ`, `/proc/sys/kernel/overflowuid` and `pivot_root`, which Rach2 lacks; that is the later milestone.
* Exec copies the whole file of a program into private frames, so each process costs its file's size: 290 MB for each Codex process, three of them a session. Two sessions filled a 2 GiB guest.
* Under TCG the clock lost about 60 s in 75 minutes of load: `pit.c` does not catch up lost ticks under a hypervisor. Nothing syncs the clock.
* Rach2 has no vDSO, so `clock_gettime` is a system call, and the kernel runs under one lock. Both cost speed, not correctness, and wait for measurements.

## Rules for the work

* A program comes from its vendor's published build: a pinned version, its SHA-256 or SHA-512 as the vendor publishes it, and the vendor's signature where it signs one. Downloads go to an ignored directory and are checked again at each use, as `vendor/gnu/chromium/fetch_chromium.sh` does; only the recipe is committed.
* Updates are off by the vendor's own settings, so each program runs the pinned version. A daemon or helper that a program installs or starts counts as part of it.
* A credential never goes into an image, the repository, a log or an issue.
* The tests run offline. A fake API server on the host speaks the vendor's documented public protocol, behind a test certificate authority that the program is told to trust by its documented setting, and replays a script: read a file, run a command, edit a file, commit with git. The host judges the files, the commit and the tool results, never the agent's last sentence. A program whose vendor documents no public protocol or no way to name a server is tested offline as far as it can be (start, help, its screen, its sign-in menu) and in the live acceptance.
* Every suite of this milestone is x86-64 only and declares its memory; kernel changes are checked on both architectures.
* A new test is proven by a mutation, as AGENTS.md says.

## The phase

One phase, its tasks sub-issues of the phase issue. Most start at once; a task lists the tasks it waits for.

Kernel:

* K1. Load ELF segments as private file mappings, so a dropped page comes back from the file.
* K2. Refuse writes to a running program's file, with `ETXTBSY`.
* K3. Follow `/proc/PID/fd/N`, `cwd` and `root` in the middle of a path.
* K4. Run a program through `/proc/PID/fd/N`.
* K5. Keep the clock when the emulator delays the timer's ticks.
* K6. Keep time with BusyBox's `ntpd` on the GNU system.

Packages, each a recipe in one agents layer under `vendor/gnu/`:

* L0. The agents layer, with GitHub's `gh` as its first package.
* L1. Claude Code, from Anthropic's signed release manifest. Waits for L0.
* L2. Codex, from its GitHub release. Waits for L0.
* L3. Antigravity CLI, from Google's release manifest. Waits for L0.
* L4. Muse Code, from Meta's release. Waits for L0.

Fake API servers:

* F0. The fake API server and its test certificate authority.
* F1. Anthropic's Messages protocol, proven against Claude Code on Linux. Waits for F0.
* F2. OpenAI's Responses protocol, proven against Codex on Linux. Waits for F0.
* F3. Antigravity CLI's protocol, where Google documents one. Waits for F0.
* F4. Muse Code's protocol, where Meta documents one. Waits for F0.

Suites:

* S1. Claude Code on Rach2. Waits for K1, K3, L1 and F1.
* S2. Codex on Rach2. Waits for L2 and F2.
* S3. Antigravity CLI on Rach2. Waits for L3 and F3.
* S4. Muse Code on Rach2. Waits for L4 and F4.
* S5. The loops' own tests on Rach2.
* S6. One worker session on Rach2, against a fake board and a fake API. Waits for S1 and S5.
* A1. The live acceptance script for the Lenovo. Waits for S1 and S2.

## After the release

The release installs the phase on the Lenovo. The owner then signs in to each program and to `gh`, and a session of the owner's runs A1's script: each agent, over SSH and in tmux, reads a disposable repository, changes it, runs its test and commits; the session is cancelled, resized, reconnected and reopened, the machine reboots and the credentials are still there, and no program changed its version. A worker loop then runs one session on the Lenovo. The milestone's Readme records the result, and the owner closes the milestone.

## Later

* The agents' sandboxes: capability bounding sets, the overflow IDs, `pivot_root`, the mounts `bwrap` makes, a loopback interface in a new network namespace, and `socat` for Claude Code's relay.
* Pages shared between processes that map the same file, a vDSO, and a finer kernel lock, each when a measurement asks for it.
* The agents on Rach2's own console, on the musl system, and on i686.
