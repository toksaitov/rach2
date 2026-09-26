# CLAUDE.md

This file is the project policy for Rach2, and Codex reads it through `AGENTS.md`. It states the rules in force, not the history behind them; that history is in commit messages and `dev-log/`. When a new rule replaces or tightens an old one, remove the old text.

## Project

Rach2 is an educational 32-bit x86 operating system. Its readers are students and developers, and the code and its prose are meant to teach. The kernel is freestanding C89 (`-ansi`, no libgcc), cross-built with `i686-elf-gcc`. The same sources also build on Rach2 itself with the staged TinyCC and pdpmake, so kernel code must stay inside the dialect both compilers share. The userland is the ported musl libc, the BusyBox shell, the Dropbear SSH server, and the TinyCC compiler with pdpmake to drive it.

## Agents

Codex does the work, in units of one task each, and this session directs it. Give Codex the phase, the files, the claims the work must make true, and what would prove it done; read what comes back, check every claim against the code, apply it, and report progress. Write code here only when a unit comes back wrong or when handing the work over would cost more than doing it, and say in the report which it was.

Codex also writes and checks the prose: documents, manual pages, Readmes, the proofreading of anything written here, the contextual half of the register rule, and, in milestones about documents and structure, the comments in source and support files. Prepare the facts and the claims here, and check every claim in what comes back against the code before applying it. Codex reviews code once per milestone, at its end, as Milestones describes.

The limits on agents:

* Claude: up to ten Opus 5.5 instances at xhigh effort may work on a task at once, counting every subagent. They may do work the Codex allowance does not cover, prose included. A subagent that writes or judges code runs on Opus 5 or stronger; a weaker model needs a written reason in the plan.
* Codex: at most three units run at the same time. The limit is on how many run at once, not on how many run in total. The allowance is used up only when Codex answers with its usage-limit message, and it is separate from the Claude one.
* A Codex unit, like any delegated review, covers at most six files and runs at most ten minutes. These limits are fixed. One reviewer is assigned to a diff.
* State a fixed total of agents before starting any. An earlier result does not decide how many agents a later stage starts.

Running a Codex unit:

* `~/.codex/config.toml` sets the model, `gpt-6-astra`, and the reasoning effort, `xhigh`. Pass no `--model` and no `--effort` unless the owner names others, and spell them as the configuration or a milestone's record does. A refusal is a refusal of that request, not a fact about the account: `--model astra` is refused with `the 'astra' model is not supported when using Codex with a ChatGPT account`, though only the name is wrong.
* A unit that changes files needs `--write`; without it the sandbox is read-only and the unit only reports.
* A unit that changes files runs with `--cwd` in a git worktree of its own, because units that share a checkout overwrite each other's objects in the middle of a build. `status`, `result` and `cancel` for that unit need the same `--cwd`.
* Ask a reviewer to state the most serious findings first. Do not ask it to write or tee a file; its sandbox refuses. Do not launch it where a tool timeout can kill its process group.
* Never report that a delegated job is running without reading its state in the same turn.

## Hosts

Code is written, reviewed and committed on the Mac mini, in this tree. Every suite, every full set (`tests/run all`) and every analysis runs on the testing host, reached as `ssh testing`: a Windows machine with an AMD Ryzen 9 7950X3D, 32 threads and 200 GB of memory, whose Windows Subsystem for Linux (WSL2) distribution, Ubuntu 26.04, is given about half of that memory and has a copy of this tree at `/home/testing/rach2`.

Reach that copy through `tools/dev/testing.sh`. `testing.sh sync` replaces the copy with this tree, `testing.sh run COMMAND` syncs and then runs the command there, as in `testing.sh run tests/run all`, and `testing.sh sh` opens a shell there. The copy is never edited on the host, and it is synced before every run: `testing.sh run` does that itself, and a run started from `testing.sh sh` needs a `testing.sh sync` first. What `.gitignore` names is not sent, except the Doom WAD, so the host keeps its own `build/` and vendor trees. `tools/Readme.md` and the script's header say how the script reaches WSL and which rsync it needs.

The Mac builds only what editing needs: a compile or one host test. A run a person waits for, such as a suite, the host tests (`tests/run host`), a full set or `make analyze`, is never started on the Mac. The rules under Verification apply on the testing host unchanged, and the tool versions a milestone's Readme records (compilers, analysers, formatter and QEMU) are the testing host's. The Lenovo is on the Mac's network, not the testing host's, so hardware sittings (sessions with the owner at the machine) and stick writes run from the Mac. `tools/Readme.md` describes how to set up a host.

## Hardware

The owner has authorized writes to two targets: the development flash drive on the Mac mini, and any disk of the Lenovo. Both have nothing on them but this work, and both are a sandbox: write them as the work needs, without asking and without naming them to the owner first, and take no backup, restore or hash for their safety. Report afterwards what was written and where. Any other target, such as a second flash drive, another machine or hardware of another kind, needs the owner's authorization.

The Mac mini's internal SSD, an APPLE SSD AP0512Z with serial `0ba02f1140bcba20`, is never written.

A check of the target runs before every write, in place of asking. A target on the Mac must be external, removable and on USB, and must report the flash drive's serial and size: a SanDisk 3.2Gen1 of 61,524,148,224 bytes with USB serial `00014101091624031318`. Its device node changes between connections, so the serial and the size identify it. A target that is internal, or that reports the SSD's serial, is refused and the write does not run. The system itself needs no host device path: the kernel probes its own disk controller and chooses its root from its boot options, as `rach2/sys/block/Readme.md` describes.

The Lenovo's disk stays in its bay, since removing and refitting it is an hour of physical work. A machine left unbootable is reinstalled from the flash drive, which stays plugged into it.

The installed Lenovo is updated over SSH: a kernel through `bootctl` on the running system; a root through the recovery system with `rach2-install --update`, which keeps the machine's accounts; a partition the installer does not know about through `reflash`. The manual has the commands: `rach2-install(8)` for an install, `recovery(7)` for the recovery system and the three update paths, `bootctl(8)` and `reflash(8)` for the two writes, and `disk(7)` for the device nodes they write through, each limited to one region of the disk. The machine's own offsets are in `dev-log/43-machine/hardware.md`, and the credential check before a root write is in `tools/Readme.md`, under Working an installed machine from the host. Take a path from these; do not derive one.

## Build

Use GNU Make on the host. `make` builds the kernel, the host tools, the guest programs, five of the vendored projects (musl, BusyBox, Dropbear, TinyCC and pdpmake), Doom, and the test, os-simple and os-real RAM images. The first build compiles the vendored projects and takes minutes; a later `make` rebuilds only what changed, and header dependencies rebuild the objects they affect. The build checks its tools before it compiles anything with the cross toolchain, and `sh tools/setup/doctor.sh` reports on the wider machine setup.

* `make test` runs the style checks, the host tests and the core tier of suites.
* `tests/run NAME` runs a named suite.
* `make check-sources` compares the kernel's source list with the source tree.
* `make clean` and `make distclean` clean up. Do not use `make clean` as a routine build step, and do not run a `build_*.sh` script by hand.

Every in-house component writes its objects, `.d` files and binaries under the top-level `build/`. Shared host Make rules are in `mk/`. Image targets do not run QEMU: the run scripts under `rach2/` boot an image, and the test scripts start QEMU for the suites.

The guest build uses three POSIX `guest.mk` files, for the kernel, the utilities and ISH, which pdpmake reads on Rach2. Each component's host Makefile and its `guest.mk` include the same source manifest, `rach2/sources.mk`, `utils/programs.mk` or `ish/sources.mk`, so the two builds cannot drift apart. `tools/verify/guestcheck.c` checks the derived lists and the guest limits, and `tests/run selfbuild` builds all three components on Rach2 under QEMU.

### Ports

Six projects are vendored as pinned tarballs, checked by SHA-256 and built by their `build_*.sh` scripts, with stamp files that record a finished build: musl, BusyBox, Dropbear, TinyCC, pdpmake, and e2fsprogs, whose `e2fsck` and `resize2fs` the recovery image stages. The five besides musl link against the ported musl through the `muslgcc` wrapper. Vendored projects use their upstream build systems. Do not commit vendored build output or churn the vendored trees. The one committed exception is `vendor/grub/`: six reviewed, pinned GRUB boot images with their provenance manifest, committed because the toolchain that makes them does not build on every supported host. They change only through `vendor/grub/regen_grub.sh` and its review procedure.

When a capability is missing, look in BusyBox before writing it. That is the owner's standing decision, and it is why the account tools (`adduser`, `addgroup`, `chpasswd`, `deluser` and `delgroup`) are BusyBox's.

## Verification

A test is trusted only after it has been made to fail, in three steps: the unmodified test passes; a mutation, a deliberate fault compiled into the artifact the test runs, makes it fail; with the source restored and the artifact rebuilt, the test passes again. Drive the mutation from a script that restores the source in a trap, since a mutation can hang the suite and outlive the command that applied it. Stop the test processes before restoring a source they share. Copying the source back does not make `make` rebuild it, so touch the file or rebuild explicitly before believing the result. One mutation shows that one fault is caught, not that the whole test is sound.

A suite judges what the kernel counts, never the clock. Seconds in a suite are completion bounds, which `RACH2_TIMEOUT_FACTOR` scales for a slow host, and a time is printed for the record, not judged. What would make a row, one check in a suite, slow (a spin, a wait, a walk over a list, a call per page) is counted by the kernel, and the row checks each count against a bound that is the same on every machine. Where the kernel has no counter for it yet, a multiple of a baseline measured in the same boot is the last resort, and each such row is listed with its reason.

A step that costs the owner time, allowance or a device is checked before it starts: a build from clean, a full set, a Codex round, a hardware sitting, or any run a person waits for. Read what would stop it: `dev-log/50-shortcomings/plan.md`, the milestone's notes and the code the step depends on. Where a form is known to work, use it. A step whose conditions were not checked is not started.

Running tests:

* Try a new row, suite or tool small first, on the smallest inputs that exercise every step: a 64 MiB disk where the real one is 232 GiB. Start it at full size only after it passes there.
* Never build while a run is in flight. The suites read the kernel and the images from `build/`, so a build part way through changes what the remaining suites boot and what the finished ones were judged on, and the run is void. Check with `ps` for a run in progress before `make`, and wait for it or stop it deliberately.
* Watch a run in flight. Stream its log and act on the first failing line, since most failures come in the first seconds. Do not sleep until the run ends to find out how it ended.
* Read a run's rate at its first stage. A copy, a hash or a boot running at a small fraction of a similar step's earlier rate is a symptom: the emulator's USB storage moves tens of kilobytes a second where its IDE moves megabytes, a wrong block size makes a call per sector, a wait runs until its timeout expires. Stop the run, find and remove the cause, and only then start it again.
* Keep the whole output of a long run in a file on the host that runs it, and read all of it. Never pipe it through `tail` or `head`, which throws the failure away.
* Stop a run with SIGTERM, so its cleanup releases what it has taken. Before starting it again, wait until every process of the old run has exited, since the old run's cleanup would otherwise remove files of the new one, and look for anything it left, such as a lock file or a mounted image.
* Resume a failed run at the failing step: rebuild what changed, run the suite that failed, and run the others again only when the change could have touched them. Start work that does not depend on a running job beside it.
* Believe a number only with the binary that produced it. `make` does not rebuild the recovery kernel or the install stick, so a measurement on a stick, an ISO or a recovery boot first checks that the artifact has the change: its timestamp against the source's, or a string the change added.

When a failure repeats in a new form, write the general rule here and record the incident in `dev-log/`.

## Milestones

A milestone is run for speed: all of its work is done first, and it is checked once, by the closing sequence below. Nothing runs along the way but a compile: no suite, full set, host tests, Codex review or mutation test after a change or a phase. Run `tests/run style host` at the end before committing a change to a kernel header or a host test source. A Readme whose contract a commit changes gets its facts in that commit and its prose from Codex, without a review of the phase.

A problem found along the way that the milestone did not set out to fix goes into `dev-log/50-shortcomings/plan.md` the same day, with its severity, how to reproduce it, the contract it breaks, what it depends on, and the test that will show it fixed. The milestone that found it fixes it, and the entry says so. Nothing is reported at the end as found and unfixed.

At the end of the milestone, once, in this order:

1. Build from clean: empty `build/` of everything but `build/vendor/`, and remove the vendor outputs as well only when a port or its toolchain changed.
2. Run `tests/run all` and the mutation tests of the milestone's new checks, and fix everything they find. The style suite runs `prosecheck`, so the mechanical half of the register rule is checked before the review.
3. Give the whole milestone's diff to Codex for a harsh review, in units grouped by contract, each of at most six files and sized to finish in ten minutes. Each unit gets the shared interface changes, may read what its diff depends on, and states its most serious findings first. The review covers the contextual half of the register rule. An expired review is incomplete, not clean.
4. Fix every finding, or reject it in the milestone's `notes.md` with the source that shows it wrong.
5. Run `tests/run all` again.
6. Only in a milestone whose plan has a machine phase, work on the machine. A destructive write on the machine comes only after the review of the code that performs it.

The machine phases are gathered so that the owner is needed for one sitting. Every step is prepared before the owner is called, the steps run one after another, and the sitting ends with the owner free. Such a milestone runs the sequence above once before the sitting, since the write comes after the review, and has the fixes the machine produced reviewed once more at its close. Other milestones test their work in QEMU and do not touch the machine. A kernel installed through `bootctl` over SSH when the machine happens to be on is permitted, never required.

Before a milestone or a plan is called done, or nearly done, audit its whole scope: every phase of its plan, every entry of the shortcomings list, and every record, read for what is still owed. Report the list of open items the audit gives; nothing is called done that the audit has not shown done. The milestone's Readme records what was accepted: the commit, the build options, the image hashes, the test command and its result, and the hardware arrangement.

## Writing

All written language reads as if one careful author wrote it: code comments, documents, commit messages, reviewer prompts and chat replies. Write in the voice of Kernighan and Ritchie, Kernighan and Pike, W. Richard Stevens and Knuth; of the Unix manual pages for reference documents; and of Lions' Commentary for explaining kernel code. Be terse, exact and concrete. State the claim first in plain words (the component, what it does, why), explain the part that is not obvious, and stop. No slogans, no marketing words, no development diary, no notes to oneself.

Chat replies are held to the same standard as committed prose: short sentences, ordinary words, and no em-dashes. Scratch files and internal reasoning may use any register, and none of it is committed or shown to the user.

* ASCII only in prose and comments: no Unicode dashes, smart quotes or ellipsis.
* No em-dash, whether written as U+2014 or as a spaced `--`. Recast the sentence with a period, colon, semicolon or parentheses. The `--flag` and end-of-options `--` forms stay legal in code and command examples.
* Markdown: do not wrap prose to a column, write bullets as `*` and not `-`, do not lean on bold, and avoid tables unless the data is tabular and stays aligned. Code comments wrap at 80 columns.

### Register rule

The Claude models used here tend to write figurative phrases as private shorthand. Do not use that register in chat replies, code comments, documentation, Readmes, commit messages or reviewer prompts. `tools/verify/prosecheck.c` rejects the terms that have no valid use here; review decides the cases that depend on context.

These terms are wrong at any frequency, and `prosecheck` rejects them: "load-bearing", "worth stating plainly", "worth noting", "full stop", "blast radius", "tripwire", "timebox" and "seam". Write the ordinary word: a test, a stage, a time limit. There are no sanctioned exceptions: the tests under `tests/host` are the host tests, and the check suites are named by their targets, `tests/run style` and `tests/run host`. This section, the checker's word lists and its test fixtures are the only places these terms may appear.

A term goes on that list only when it has no valid use in this domain. "Trap", "bench", "gate", "era", "surface" and "sight" stay off it, since each has one here: a shell trap, a test bench, hardware gates, controller generations, an attack surface, and phrases such as "anywhere in sight". Review still rejects each of them as invented shorthand: "trap" for a difficulty, "bench" for a work session, "gate" for a set of checks, "era" for a stage, "surface" for an area of code, "sight" for a view.

Review decides the cases that depend on context:

* Reject "honest" or "fiction" applied to code or data.
* Reject a "not X, it's Y" reversal, and fragments written for effect, such as "Not a detail. A design decision."
* Do not give hardware or code intentions: a clock does not believe, refuse or get asked. "The register says X" is ordinary datasheet language and stays.
* Prefer a concrete subject and verb: "the header has a magic number", not "the header carries one". Review "holds", "names", "spells", "sits" and "pins" for the same fault. Established technical metaphors stay: a listening socket, a held lock, a warm cache.
* Do not build a sentence toward a turn of phrase.
* Define a term of art at first use, and put a qualification beside the claim it limits.

Repeating an ordinary phrase can produce the same private register. `tools/verify/prose_report.sh` compares each watched phrase with the human comments of the pinned upstream tarballs and the Doom sources, and prints the rate and the ratio for a person to read. A phrase fails the build only after the reason is recorded. Do not change the baseline in the same commit as the prose that changes it.

## Comments

Explain a function in a block comment before it, and let names and structure make the body clear; do not narrate the body in comments.

The block is a Doxygen-compatible `/**` comment. Its first sentence is the summary, and prose paragraphs follow for the contract and the part of the reason that is not obvious. Use `@param` and `@return` only where the meaning is not obvious, keep Markdown light (mostly backticks around identifiers), and do not pile up tags.

A comment inside a body is allowed only for a deliberate hack, or for a hardware or application binary interface (ABI) fact that cannot be named away. It is one line, written as documentation, never as a reasoning trace.

## Messages

Messages from tools and code have one format. Diagnostics go to stderr as `component: detail`, in lowercase, with no banners; progress is `component: action`, and an outcome is `component: result: detail`. Kernel and user code use `subsystem: event: detail`. A probe's success token, such as `VMPROBE OK`, is a test contract and does not change.

## Documentation

Each component has one Readme. A change that alters a contract rewrites the owning Readme in the same commit, and does not append to it. Documents state the current contract; history is in commit messages, and the records of how each milestone was planned and built are in `dev-log/`. The facts of a document are gathered here and its prose is Codex's, as Agents says.

The manual under `man/` covers the machine. Every program a system image stages or `utils/programs.mk` builds, every BusyBox applet and every system call the kernel registers has a page or an alias in its section; a program that only test images stage needs none. `tools/verify/mancheck.c`, run by `tests/run style`, refuses a tree that lacks one, a page `mandoc -T lint` warns about, an alias that names no page, or a recovery page the recovery manifest does not stage. A commit that adds a program, an applet or a call adds its page or its alias line in the same commit, and a commit that changes what one does changes its page. A page states what the code does on this kernel, from a fact sheet gathered from the source, and says where that differs from Linux or from the program's upstream documentation.

## Git

Never push. The remote is the owner's to update, and a push cannot be taken back once anyone has fetched it; ask, and let the owner do it.

Commit in scoped units, one logical change each, directly on the branch, with no attribution of any kind. The message is one line with no trailing period, begins with a capitalised verb in the imperative, and says what the commit does, not how it was arrived at; `git log --oneline -20` shows the style. A text-only cleanup changes no behavior and no built artifact. Keep the vendored trees out of the diff. `tools/git-hooks/commit-msg` refuses a message with an attribution line. `.claude/settings.json` turns off Claude Code's own attribution and turns the hook on when a session starts; `git config core.hooksPath tools/git-hooks` turns it on in any other clone.
