# AGENTS.md

The project policy for Rach2, for every coding agent. Codex reads this file directly; Claude Code reads it through the `@AGENTS.md` import in `CLAUDE.md`, which adds the rules Claude Code alone needs. Procedures needed only for some tasks are skills in `.agents/skills/`, which Codex runs as `$name` and Claude Code as `/name` through `.claude/skills/`:

* `milestone`: working through a milestone of `dev-log/` and closing it.
* `testing`: running suites on the testing host, and writing suites and mutation tests.
* `machine`: writing the flash drive or a Lenovo disk, and updating the installed Lenovo.

This file states the rules in force; their history is in commit messages and `dev-log/`. When a rule changes, replace the old text. When a failure repeats in a new form, write the general rule here or in the skill it belongs to, and record the incident in `dev-log/`.

## Project

* Rach2 is an educational operating system for 32-bit and 64-bit x86 PCs, i686 and x86-64. Its readers are students and developers, and the code and its prose are meant to teach.
* The kernel is freestanding C89 (`-ansi`, no libgcc), cross-built with `i686-elf-gcc` or `x86_64-elf-gcc`, as `ARCH` selects (`mk/arch.mk`). The same sources also build on Rach2 with the staged TinyCC and pdpmake, so kernel code stays inside the dialect both compilers accept.
* The userland is the ported musl libc, the BusyBox shell, the Dropbear SSH server, and TinyCC with pdpmake to drive it. `vendor/gnu/` builds a larger root beside it, with glibc and GNU programs.
* Read `Readme.md` first, and a component's `Readme.md` before changing its code. `dev-log/` records how each milestone was planned and built; [its index](dev-log/Readme.md) lists them in order.
* For a new machine, `dev-log/26-portable/arrival.md` lists what a clone does not provide, and `tools/Readme.md` has the setup for macOS, Debian and Ubuntu.

## Roles

* The owner sets the work and its order on the board, and decides what the plans leave open. Everyone takes work from the board: the five research assistants, each with Claude Code in an account of their own on the testing host, and the owner, from a computer of the owner's own.
* Claude Code does the work: code, tests and prose. Codex does work only when the owner asks for it.
* The owner's Claude Code session is also the manager of the process, as the section of that name says. The workers are the Claude Code sessions of the five accounts.
* Codex reviews the whole diff of a phase once the phase's last issue has landed, and otherwise only when the owner asks. Only the owner's accounts are signed in to Codex, so the manager runs the review, one unit after another between its checks, and each finding it keeps becomes a `bug` issue. Nothing waits for the review: no push, no issue, no night's run and no milestone's close.
* Claude Code may run at most three helpers at once, each Claude Opus 5.5 in a git worktree of its own, at the effort `CLAUDE.md` gives; it starts no other subagents. Codex, one Astra instance at xhigh effort at a time, is used for review. A helper or Codex does its work itself and starts no subagents.

## Work

* The work is on the board, the GitHub project "Rach2" of `toksaitov/rach2`. A phase of a plan is an issue with the label `phase`, its tasks are its sub-issues, and a "blocked by" link orders two issues. The Status field is Backlog, Ready, In progress or Done. The board adds every new open issue as Backlog; the owner, or the manager for the owner, moves an issue to Ready, and the manager moves a new `bug` issue to Ready at its next check; and an issue is closed, and moved to Done, when a commit whose message says `Closes #N` reaches `master`. `gh` reaches the board with the `project` scope, which `gh auth refresh -s project` adds.
* Take the top Ready issue with no open blocker; an open issue that ci filed, whose title begins `ci:`, comes before any other, whatever its Status. Assign it to yourself and set it In progress, then read its timeline (`gh api repos/toksaitov/rach2/issues/N/timeline`): if another account was assigned before you, unassign yourself and take the next, so that no issue has two writers. A problem found along the way that the task did not set out to fix becomes an issue with the label `bug`, a sub-issue of the same phase.
* Several related issues may go in one change, each a commit of its own. Helpers may write parts of it; the session that pushes runs the check below once, for all of it.
* Work in your clone on a local branch named for the issue, such as `42-ahci-timeout`.
* Build the whole task first. Write every part, compile as you go, and check a detail by hand only when reading and reasoning cannot settle it. Start no suite or other long run while the task is being built.
* Then rebase onto `origin/master` and check the task once, ordering the steps by cause and effect, so that nothing is checked before a later step changes it:
  1. The compile and `tests/run style`, which are fast and catch mechanical faults.
  2. The suites the change could affect, as one `RACH2_WORKERS=4 tests/run A B C`, and again with `ARCH=x86_64` when the change touches code that differs between the architectures.
  3. For every fix after that, only the checks it could affect.
* A suite that fails in that check with the failure already recorded on `master`, in an open `ci:` issue for the same suite and architecture, is not yours. Push past it only when your change does not touch what the failing row tests; otherwise fix the failure, or show that your change passes a narrower check of that behavior. Either way, add your evidence to that issue.
* Before pushing, look for an open issue with the label `hold`. While one is open, push only a repair toward a passing core run, and name it in that issue. Push with `git push origin HEAD:master`, which `master` takes only as a fast-forward. If another push came first, rebase, look at what it changed, and repeat only the checks it could affect. Never force-push.
* ci checks `master` after the push. `.github/workflows/ci.yml` runs the build, `tests/run style`, `tests/run host` and `tests/run core` on both architectures, one run at a time, on the newest commit, and `night.yml` runs every suite at 19:00 UTC. A failure becomes an issue titled `ci: KIND: WHAT fails on ARCH`, with the commits since the last pass and the `gh run download` command that fetches the kept logs, and `bisect.yml` then comments with the commit that fails while its parent passes, or says that the range is unresolved or that the failure did not repeat. The next free worker takes the issue, whoever wrote the commit: fix forward when the fix is clear, otherwise revert the named commit and reopen the issue it closed, with the log. Nobody reverts on an unresolved range; narrow it first. A failure that did not repeat is labelled `intermittent` and stays open; nothing is quarantined.
* A core failure of the build, style, the runner or the `default` suite opens the issue labelled `hold`, and the next core run that passes on both architectures closes it. Pushes go on while any other suite fails.
* The owner reads what lands and reverts what they reject. The build from clean and the work on the Lenovo are the owner's, and the Lenovo takes only a commit that a night passed whole.

## The manager

* The manager works on its own schedule. Every 30 minutes it reads the worker sessions, the test runs on the host, ci, `master`, the board and the host's load, and writes one line to its log.
* It fixes what it finds: a lost ci job run again, leftover jobs and locks cleared, fewer workers writing at once while the host is overloaded, new `bug` issues moved to Ready, and the `hold` issue opened when a change on `master` has waited more than two core runs for a pass. A worker is stuck only when neither its session nor its test has written anything for twenty minutes, since a long suite is not a stuck one; the manager stops it, confirms that its processes are gone, and only then puts its issue back in Ready.
* Between its checks it takes work from the board as a worker does, with up to three helpers, and checks and pushes what they write.
* A major incident is `master` failing for more than two core runs, the host or the runners down, every worker stuck or out of tokens, or the same failure coming back. The manager notifies the owner with the cause and the fix it has chosen, waits ten minutes, applies the fix unless the owner stops it, and reports the result. It changes the rules in this file the same way; a worker reads this file when its session starts, so a change applies from each worker's next session. Repository settings, the hardware and anything that deletes data wait for the owner's yes.
* At the end of the day it reports, in an issue with the label `night-report`, what landed, what got stuck, where the time went and what it changed.

## Build

* `make` builds, for the architecture `ARCH` names (i686 by default), the kernel, the host tools, the guest programs, six of the vendored projects (musl, BusyBox, Dropbear, TinyCC, pdpmake, e2fsprogs), Doom, the test, os-simple and os-real RAM images, and os-real's ext2 root. The first build takes minutes; later ones rebuild only what changed, header dependencies included.
* `make test` runs the style checks, the host tests and the core tier of suites; `tests/run NAME` runs one suite; `make clean` and `make distclean` clean up. The build checks its tools before it compiles with the cross toolchain, and `sh tools/setup/doctor.sh` reports on the wider machine setup.
* Do not run a `build_*.sh` script by hand, and do not use `make clean` as a routine step.
* Every in-house component writes its objects, `.d` files and binaries under the top-level `build/`; shared host Make rules are in `mk/`. Image targets run no QEMU: `rach2/run*.sh` boot an image, and the test scripts start QEMU for the suites.
* A source file is listed once, in its component's manifest (`rach2/sources.mk`, `utils/programs.mk` or `ish/sources.mk`), which both the host Makefile and the POSIX `guest.mk` that pdpmake reads on Rach2 include. `make check-sources` and `tools/verify/guestcheck.c` check the lists, and `tests/run selfbuild` builds all three components on Rach2 under QEMU.
* musl, BusyBox, Dropbear, TinyCC, pdpmake and e2fsprogs (whose `e2fsck` and `resize2fs` the recovery image stages) are vendored as pinned tarballs checked by SHA-256, built by their `build_*.sh` scripts with their upstream build systems, behind stamp files; the five besides musl link against the ported musl through the `muslgcc` wrapper. Do not commit their build output or churn their trees.
* The one committed exception is `vendor/grub/`: eight reviewed, pinned GRUB boot images with their provenance manifest, committed because the toolchain that makes them does not build on every supported host. They change only through `vendor/grub/regen_grub.sh` and its review procedure.
* When a capability is missing, look in BusyBox before writing it. That is the owner's standing decision, and it is why `adduser`, `addgroup`, `chpasswd`, `deluser` and `delgroup` are BusyBox's.

## Testing

* Check a change with the suites that cover what it changed, as Work says; ci then checks `master`, and the night runs every suite.
* Builds and suites run on the testing host: each person's in a clone in their own account there, and the owner's in the copy `tools/dev/testing.sh` syncs from the owner's computer, which itself runs only a compile or one host test. The `testing` skill describes the host.
* A new test is trusted only after it has been made to fail: the unmodified test passes; a mutation, a deliberate fault compiled into what it runs, makes it fail; with the source restored and the artifact rebuilt, it passes again. A changed test needs no new proof. The `testing` skill has the mechanics.
* A suite judges what the kernel counts, never the clock; seconds in a suite are completion bounds.

## Hardware

* The hardware is the owner's: the Mac mini, the development flash drive and the Lenovo, which is on the Mac's network. Only the owner's sessions work on it.
* The Mac mini's internal SSD, an APPLE SSD AP0512Z with serial `0ba02f1140bcba20`, is never written.
* The owner has authorized writes to the development flash drive on the Mac mini and to any disk of the Lenovo. Both are a sandbox: write them as the work needs, without asking or naming them first, and take no backup, restore or hash for their safety. Report afterwards what was written and where. Any other target needs the owner's authorization.
* A check of the target's identity runs before every write, and hardware work runs from the Mac, since the Lenovo is on its network. The `machine` skill has the check and the procedures.

## Writing

* All written language reads as if one careful author wrote it: code comments, documents, commit messages, reviewer prompts and chat replies. Write in the voice of Kernighan and Ritchie, Kernighan and Pike, W. Richard Stevens and Knuth; of the Unix manual pages for reference documents; and of Lions' Commentary for explaining kernel code.
* Be terse, exact and concrete. State the claim first in plain words (the component, what it does, why), explain the part that is not obvious, and stop. No slogans, no marketing words, no development diary, no notes to oneself.
* Chat replies are held to the same standard: short sentences, ordinary words, and no em-dashes. Scratch files and internal reasoning may use any register, and none of it is committed or shown to the user.
* ASCII only in prose and comments: no Unicode dashes, smart quotes or ellipsis. No em-dash, whether written as U+2014 or as a spaced `--`; recast the sentence with a period, colon, semicolon or parentheses. The `--flag` and end-of-options `--` forms stay legal in code and command examples.
* Markdown: do not wrap prose to a column, write bullets as `*`, do not lean on bold, and use a table only for tabular data that stays aligned. Code comments wrap at 80 columns. `tests/run style` checks these rules.

### Register rule

* The Claude models used here tend to write figurative phrases as private shorthand. Do not use that register in chat replies, code comments, documentation, Readmes, commit messages or reviewer prompts.
* These terms are wrong at any frequency, and `tools/verify/prosecheck.c` rejects them in committed text: "load-bearing", "worth stating plainly", "worth noting", "full stop", "blast radius", "tripwire", "timebox" and "seam". Write the ordinary word: a test, a stage, a time limit. There are no sanctioned exceptions: the tests under `tests/host` are the host tests, and the check suites are named by their targets, `tests/run style` and `tests/run host`. This section, the checker's word lists and its test fixtures are the only places these terms may appear.
* A term goes on that list only when it has no valid use in this domain. "Trap", "bench", "gate", "era", "surface" and "sight" stay off it, since each has one here (a shell trap, a test bench, hardware gates, controller generations, an attack surface, "anywhere in sight"), and review rejects each as invented shorthand: "trap" for a difficulty, "bench" for a work session, "gate" for a set of checks, "era" for a stage, "surface" for an area of code, "sight" for a view.
* Review also rejects "honest" or "fiction" applied to code or data; a "not X, it's Y" reversal; fragments written for effect, such as "Not a detail. A design decision."; and intentions given to hardware or code: a clock does not believe, refuse or get asked, though "the register says X" is ordinary datasheet language.
* Prefer a concrete subject and verb: "the header has a magic number", not "the header carries one". Review "holds", "names", "spells", "sits" and "pins" for the same fault. Established technical metaphors stay: a listening socket, a held lock, a warm cache.
* Do not build a sentence toward a turn of phrase. Define a term of art at first use, and put a qualification beside the claim it limits.
* Repeating an ordinary phrase can produce the same private register. `tools/verify/prose_report.sh` compares each watched phrase with the human comments of the pinned upstream tarballs and the Doom sources, and prints the rate and the ratio for a person to read. A phrase fails the build only after the reason is recorded. Do not change the baseline in the same commit as the prose that changes it.

## Comments

* A source file begins with a comment of a line or two on what it is for.
* Explain a function in a Doxygen-compatible `/**` block before it. Its first sentence is the summary; prose paragraphs follow for the contract and the part of the reason that is not obvious. State the contract once: what the function does, the arguments whose meaning is not obvious, the return values and errors, ownership, and the locking and context it requires. Use `@param` and `@return` only where the meaning is not obvious, keep Markdown light (mostly backticks around identifiers), and do not pile up tags.
* Comment a structure member whose units, invariants or special values are not obvious.
* Let names and structure make a body clear; do not narrate it in comments. A comment inside a body is allowed only for a deliberate hack, or for a hardware or application binary interface (ABI) fact that cannot be named away, and is one line written as documentation, never as a reasoning trace.
* A comment states what is true now. It does not give the history of the code or argue with an earlier version; that is for the commit message.

## Messages

* Diagnostics go to stderr as `component: detail`, in lowercase, with no banners; progress is `component: action`, and an outcome is `component: result: detail`. Kernel and user code use `subsystem: event: detail`. A probe's success token, such as `VMPROBE OK`, is a test contract and does not change.

## Documentation

* `Readme.md` is the front page, for a reader who has never seen the project: what Rach2 is and for whom, what a person can do on it, what runs on it with links to the lists of programs, system calls and hardware, its status and major limits, the quick start to a booted system, and the top-level directories with their Readmes, in about 600 words. Licenses take one sentence. Detail belongs to the component that owns it.
* Each component has one Readme. It opens with a summary paragraph, and its `##` sections come from this list, in this order, as the component needs them: Use or Running, Results, Interfaces, Sources and license, Local changes, Design, Building, Updating, Adding tests, Limits, Tests. `dev-log/60-docs/plan.md` says what goes in each.
* A document states what the code does now, and a fact has one home: a count, a table or a contract is written where it is owned and linked from elsewhere. A change that alters a contract rewrites the owning Readme in the same commit, and does not append to it. History, milestone numbers and abandoned designs are in commit messages and `dev-log/`. A document that cannot be made true is deleted.
* The manual under `man/` covers the machine. Every program a system image stages or `utils/programs.mk` builds, every BusyBox applet and every system call the kernel registers has a page or an alias in its section; a program that only test images stage needs none. `tools/verify/mancheck.c`, run by `tests/run style`, refuses a tree that lacks one, a page `mandoc -T lint` warns about, an alias that names no page, or a recovery page the recovery manifest does not stage.
* A commit that adds a program, an applet or a call adds its page or its alias line in the same commit, and a commit that changes what one does changes its page. A page states what the code does on this kernel, from a fact sheet gathered from the source, and says where that differs from Linux or from the program's upstream documentation.

## Git

* `master` changes only by a fast-forward push of checked work, as Work says; the repository's ruleset refuses a force push to it and its deletion. Push no other branch; a helper does not push at all.
* Commit in scoped units, one logical change each, on your branch, with no attribution of any kind. The message is one line with no trailing period, begins with a capitalised verb in the imperative, and says what the commit does, not how it was arrived at; `git log --oneline -20` shows the style. A commit that finishes an issue adds a blank line and `Closes #N`.
* A text-only cleanup changes no behavior: every C file it changes is the same sequence of tokens before and after once its comments are removed, which the command in `dev-log/60-docs/plan.md` shows, and every other file it changes differs only in comment lines. Keep the vendored trees out of the diff.
* `tools/git-hooks/commit-msg` refuses a message with an attribution line. `.claude/settings.json` turns off Claude Code's own attribution and turns the hook on when a session starts; `git config core.hooksPath tools/git-hooks` turns it on in any other clone.
