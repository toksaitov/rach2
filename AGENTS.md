# AGENTS.md

The project policy for Rach2, for every coding agent. Codex reads this file directly; Claude Code reads it through `CLAUDE.md`, which adds what Claude Code alone needs. Procedures needed only for some work are skills in `.agents/skills/`, which Codex runs as `$name` and Claude Code as `/name`:

* `milestone`: the manager's procedure, from a milestone's plan to its release.
* `testing`: the runs at the end of a milestone and in its release, and writing a test.
* `machine`: writing the flash drive or a Lenovo disk, and updating the installed Lenovo.

The rules are kept short on purpose; their history is in commit messages and `dev-log/`.

## Project

* Rach2 is an educational operating system for 32-bit and 64-bit x86 PCs, i686 and x86-64. Its readers are students and developers, and the code and its prose are meant to teach.
* The kernel is freestanding C89 (`-ansi`, no libgcc), cross-built with `i686-elf-gcc` or `x86_64-elf-gcc` as `ARCH` selects. The same sources build on Rach2 with the staged TinyCC and pdpmake, so kernel code stays inside the dialect both accept.
* The userland is the ported musl libc, BusyBox, Dropbear, and TinyCC with pdpmake. `vendor/gnu/` builds a larger root beside it, with glibc and GNU programs.
* Read `Readme.md` first, and a component's `Readme.md` before changing its code. `dev-log/` records how each milestone was planned and built.

## How everyone works

* Every agent works as a sensible engineer, not a script. These rules describe the normal path. When something unusual happens, understand it, fix it or work around it, tell whoever needs to know, and keep going. Cooperate instead of waiting on each other. Aim for the fastest correct result: speed, never at the cost of correctness or safety.
* The manager above all is a pragmatic engineer. It clears whatever is in the way by the quickest sound fix, without waiting for a procedure: a stuck agent, a hung machine, service or tool, a broken build. It handles small incidents itself; a big one, which stops the milestone, puts data or hardware at risk or needs the owner's decision, it handles as far as it safely can and sends to the chat.

## Roles

* The owner sets the goals, approves each milestone's plan and makes the owner's decisions: a choice of taste or scope between outcomes that are each correct, the owner's hardware, repository settings, spending, and deleting what Resources does not allow.
* The chat is the owner's own Claude Code session on the Mac. It answers the owner and carries out the owner's orders, and does no long work. It starts the manager, passes the owner's orders on, and tells the owner of big incidents in the chat and on the owner's phone.
* The manager is a Claude Code session on the Mac that the chat starts. It plans each milestone, watches the workers, writes its share of the code through up to three worker loops of its own, and integrates, tests and releases. It reports to the chat by message.
* The workers are the worker loops, `tools/flow/worker.sh`: one in each of the five research assistants' accounts on the testing host, and on the Mac the owner's Codex loop and the manager's own. They take tasks from the board by themselves.
* Codex and a Claude Fable subagent review each milestone's plan; Codex reviews the whole milestone at its end.

## Milestones

| Word | On GitHub | Branch | Card on the board | Closed when |
|---|---|---|---|---|
| Milestone | a GitHub milestone | `milestone/NN-name` | no; cards are grouped by it | it is released |
| Task | an issue in the milestone | `task/N` | yes | its worker merges it into the milestone branch |
| Subtask | an issue under its task | none | yes | its worker finishes it |

* The manager writes each milestone's plan in `dev-log/NN-name/plan.md`: the goal as a user sees it, the owner's decisions, and a few large tasks, each with its subtasks (its tests and documents among them), the files it owns and what done means, ordered so that one rarely waits for another. What the plan needs from a machine is measured first.
* Codex and a Claude Fable subagent each review the plan on its own: whether the goal can be reached this way, whether workers would wait on each other or collide in the same files, and what is missing. The manager checks what they raise against the code, revises the plan, and gives it to the owner with what was raised and what was changed or set aside.
* Once the owner approves, the manager makes the GitHub milestone, whose description says `Branch: milestone/NN-name`, cuts that branch from master, files the tasks and subtasks, and sets the tasks Ready.
* The board is the GitHub project "Rach2" of `toksaitov/rach2`, with the Status Backlog, Ready, In progress or Done. It holds the open milestones' tasks and subtasks, the owner's decisions (label `owner`) and bugs reported from outside (label `bug`), and nothing else.
* Master moves only when a milestone is released. Several milestones may be open at once: when one has nothing left to take, workers take the next one's tasks, on its branch.

## Work

* A worker loop takes the top Ready task with `sh tools/flow/board.sh next` and `take`, which keep one writer on each task. An issue labelled `owner-only` needs the owner's accounts, such as a step on the Lenovo, and goes to them alone.
* The worker writes the whole task on its task branch, compiles what it changed and commits; the commit hook formats the code. It pushes the task branch as it goes, merging in what reaches the milestone branch meanwhile so that the task branch only grows, closes each subtask when it is done, and fixes a bug it meets on the spot, naming it in the commit message instead of filing it.
* When the task is done, the worker rebases it onto the milestone branch, compiles, pushes with `git push origin HEAD:milestone/NN-name` (rebasing again if the branch moved), closes the task and deletes its branch; the rebased task goes only to the milestone branch. A task that needs another task's code is marked on GitHub as blocked by that task, and `next` offers it once the blocker is closed.
* During a milestone nothing else is checked: no suites, no host tests, no style or document checks and no CI. A worker builds and boots the whole system only to see behaviour that reading the code cannot settle: in QEMU, and on the Lenovo through the owner's accounts.
* Wait for a long command in the foreground, or in the background with the tool's own notice. Never by polling the process table.
* The manager reads every account's state at least every half hour, with `sh tools/flow/status.sh`. When an account runs out of its limit or stalls, the manager stops its loop, commits its unfinished work to the task branch, pushes it, and gives the task to a free account; the closed subtasks show where to go on. It restarts a stopped loop, folds the owner's new requests into tasks, and keeps every account busy.
* At the end of a milestone the manager rebases its branch onto master if master moved, builds it after `make clean`, which keeps the vendor and GNU builds, and runs `tests/run style`, the host tests and every suite on both architectures, while Codex reviews the whole diff. The failures and findings go in one list, fixed in one batch; then only the failed cases run again.
* `tools/flow/release.sh M SHA` then runs every suite on Ubuntu in WSL, on Windows and on the Mac, installs SHA on the Lenovo and runs its native cases and acceptance on a trial boot. On a pass it moves master to SHA, closes the milestone and archives its cards; `-r` runs again only the last release's failures. The Lenovo takes no commit that has not passed everywhere.
* If the owner's limit runs out, the chat, the manager and its worker sessions stop together and start again when it is back.

## Build

* `make` builds, for `ARCH` (i686 by default), the kernel, host tools, guest programs, six vendored projects, Doom, the RAM images and os-real's ext2 root, rebuilding only what changed. `tests/run NAME` runs one suite; `sh tools/setup/doctor.sh` checks a machine's setup.
* Do not run a `build_*.sh` script by hand, or `make clean` as a routine step. Everything in-house builds under the top-level `build/`.
* A source file is listed once, in its component's manifest (`rach2/sources.mk`, `utils/programs.mk` or `ish/sources.mk`), which the host Makefile and the `guest.mk` that pdpmake reads on Rach2 both include.
* The vendored projects are pinned tarballs checked by SHA-256 and built behind stamp files; do not commit their build output. `vendor/grub/` keeps eight reviewed GRUB images, which change only through `vendor/grub/regen_grub.sh`.
* The GNU root's build takes some 35 GiB for one architecture and over an hour to remake; keep it between milestones.
* When a capability is missing, look in BusyBox before writing it. That is the owner's standing decision.

## Testing

* Tests run at the end of a milestone and in its release, on the testing host, in the owner's copy that `tools/dev/testing.sh` syncs, and in the release on Windows and the Mac too. The `testing` skill describes the machines.
* Whoever stops a run on the testing host confirms that its claim has left the admission ledger, `/var/lib/rach2-etude/ledger/claims`, before going on.
* A suite judges what the kernel counts, never the clock, and sets no time limit of its own.
* A test removes what it makes on every exit path, from an exit trap installed first, unless its output names a directory it keeps as evidence. Scratch goes under `TMPDIR`, which `tests/run` sets to `build/tmp`.

## Resources

* Everything the tools, tests and agents write stays under the `build/` of the clone they work in: scratch in `build/tmp`, runs in `build/runs`, caches in `build/cache`, the state of the loops and sessions in `build/flow`, and further copies of the tree in `build/worktrees`. Nothing goes in a home directory, a drive's root, a system directory or another project's. The machine-wide setup that `tools/Readme.md` lists, the test runner's site, ledger, locks and the testing host's shared caches, stays where it is.
* `sh tools/flow/status.sh` reads every disk and says low below 20% free, critical below 10% or 40 GiB. A full disk stops everyone.
* Each machine has one build copy for the end runs and the release, kept between milestones, whose unchanged vendor and GNU builds come from the machine's cache, so that no milestone builds everything from nothing. A worker builds in its own clone, and only what its task needs.
* Whoever makes something removes it: a worker its task's worktree and branch once merged, a test its scratch, the release its own. At the end of each milestone, and whenever space runs low, the manager sweeps every machine, the testing host and the Mac with `sh tools/flow/sweep.sh` and Windows by hand: merged branches, worktrees and copies whose work is on master or a milestone branch, scratch older than a day, logs older than two weeks. It keeps the build copies, the caches and the evidence of anything open.
* Never delete source that is on no branch, a stash, a credential or key, the Doom WAD, a pinned tarball, the GRUB images, a release's record, or anything a process uses. Do not run `git clean -fdx` or `make distclean` as cleanup.

## Hardware

* The hardware is the owner's: the Mac mini, the development flash drive and the Lenovo, which is on the Mac's network. Only the owner's sessions work on it.
* The Mac mini's internal SSD, an APPLE SSD AP0512Z with serial `0ba02f1140bcba20`, is never written.
* The owner has authorized writes to the development flash drive and to any disk of the Lenovo, without asking first and without backups; report afterwards what was written. Any other target needs the owner's authorization. The `machine` skill has the identity check that runs before every write.

## Writing

* All written language reads as if one careful author wrote it: comments, documents, commit messages, prompts and chat replies, in the voice of Kernighan and Ritchie, Stevens and Knuth, and of the Unix manual pages. Be terse, exact and concrete: the claim first, then the part that is not obvious.
* Chat replies use short sentences, ordinary words and no em-dashes, and give times in the owner's UTC+6. The owner sees only the reply, so each makes sense alone, in plain words instead of project terms. Issues and comments on GitHub are written the same way, the subject first and the evidence after.
* ASCII only in prose and comments, and no em-dash written as a spaced `--`. Markdown: no wrapped prose, `*` bullets, little bold, tables only for tabular data. Code comments wrap at 80 columns.

### Register rule

* Write the ordinary word. `tools/verify/prosecheck.c` rejects "load-bearing", "worth stating plainly", "worth noting", "full stop", "blast radius", "tripwire", "timebox" and "seam" in committed text. Do not call code "honest" or a "fiction", write "not X, it's Y" reversals or fragments for effect, or give intentions to hardware or code.

## Comments

* A source file begins with a comment of a line or two on what it is for.
* A function has a Doxygen-compatible `/**` block before it: a one-sentence summary, then the contract (arguments whose meaning is not obvious, return values and errors, ownership, locking and context) and the reason where it is not obvious.
* Comment a structure member whose units, invariants or special values are not obvious. Inside a body, comment only a deliberate hack or a hardware or ABI fact, in one line.
* A comment states what is true now; history belongs in the commit message.

## Messages

* Diagnostics go to stderr as `component: detail`, in lowercase; progress is `component: action`, an outcome `component: result: detail`; kernel and user code use `subsystem: event: detail`. A probe's success token, such as `VMPROBE OK`, does not change.

## Documentation

* Each component has one Readme. It opens with a summary paragraph, and its `##` sections come from this list, in this order, as it needs them: Use or Running, Results, Interfaces, Sources and license, Local changes, Design, Building, Updating, Adding tests, Limits, Tests.
* A document states what the code does now, and a fact has one home. A task that changes a contract rewrites the Readme that owns it.
* Every program a system image stages, every BusyBox applet and every system call has a page or an alias under `man/`; a task that adds or changes one changes its page.

## Git

* Commit in scoped units with no attribution of any kind. The message is one line, with no trailing period, that begins with a verb in the imperative and says what the commit does.
* Workers push their task branches, and the milestone branch by fast-forward; only the release moves master. Never force-push, with one exception: at a milestone's end, once its tasks are closed and nobody writes its branch, the manager rebases that branch onto a master that moved and pushes it with `--force-with-lease`, since master takes only linear history. Never pass `--no-verify`.
* The hooks in `tools/git-hooks` run on every commit: `commit-msg` refuses an attribution line and `pre-commit` formats the staged C files. `.claude/settings.json` turns them on for Claude Code; `git config core.hooksPath tools/git-hooks` does in any other clone.
