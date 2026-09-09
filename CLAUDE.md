# CLAUDE.md

This file is the project policy for Rach2. When a new rule replaces or tightens an old rule, remove the old text. Codex reads this policy through `AGENTS.md`.

## Project

Rach2 is an educational 32-bit x86 operating system. The kernel is freestanding C89 (`-ansi`, no libgcc), cross-built with `i686-elf-gcc`; the same sources also build on Rach2 itself with the staged TinyCC and pdpmake, so kernel code must stay inside both compilers' shared dialect. The userland is the ported musl libc, the BusyBox shell, the Dropbear SSH server, and the TinyCC compiler with pdpmake to drive it. The readers are students and developers; the code and its prose are meant to teach.

## Build

Use GNU Make on the host. The build checks its own tools before compiling; `sh tools/setup/doctor.sh` is the wider machine-setup report. `make` builds everything: the kernel, the host tools, the guest programs, the vendored userland (musl, BusyBox, Dropbear, TinyCC, pdpmake), Doom, and the test, os-simple, and os-real RAM images. The first build compiles the five vendor projects and takes minutes; a later `make` rebuilds only what changed. `make test` runs the style checks, the host tests and the core tier, `tests/run NAME` runs a named suite, and `make clean` and `make distclean` clean up.

The guest build uses three POSIX `guest.mk` files for the kernel, utilities, and ISH. pdpmake reads them on Rach2. Each component's source lists live in one shared manifest (`rach2/sources.mk`, `utils/programs.mk`, `ish/sources.mk`) included by both its host Makefile and its `guest.mk`, so the two builds cannot drift apart; `tools/verify/guestcheck.c` checks the derivations and the guest limits, and `tests/run selfbuild` builds all three on the machine. Shared host Make rules are under `mk/`. Every in-house component puts its objects, `.d` files, and binaries in the repo-root `build/` directory. Vendored projects use their upstream build systems as described under Ports. `make check-sources` compares the kernel source list with the source tree. Do not run a `build_*.sh` script by hand or use `make clean` as a routine build step. Header dependencies rebuild the affected objects.

Image build targets do not run QEMU. The run scripts under `rach2/` boot an image directly, while test targets invoke QEMU through their test scripts.

## Writing

All written language reads as if one careful author wrote it. The model for voice is the prose of Kernighan and Ritchie, Kernighan and Pike, W. Richard Stevens, and Knuth, the Unix manual pages for reference docs, and Lions' Commentary for explaining kernel code. Terse, exact, concrete. Explain the part that is not obvious and stop. No slogans, no marketing words, no development diary, no notes to oneself.

* ASCII only in prose and comments. No Unicode dashes, smart quotes, or ellipsis.
* No em-dash, written as U+2014 or as a spaced `--`. Recast with a period, colon, semicolon, or parentheses. The `--flag` and end-of-options `--` forms stay legal in code and command examples.
* Markdown: do not wrap prose to a column, bullets are `*` and not `-`, do not lean on bold, and avoid tables unless the data is genuinely tabular and stays aligned. Code comments do wrap, at 80 columns.

### Register rule for Claude Opus 5 and Fable 5

These models tend to use figurative phrases as private shorthand. Do not use that register in chat replies, code comments, documentation, Readmes, commit messages, or reviewer prompts. `tools/verify/prosecheck.c` enforces terms that have no valid use here. Review handles choices that depend on context.

Decided by `tools/verify/prosecheck.c`. These terms are wrong at any frequency: "load-bearing", "worth stating plainly", "worth noting", "full stop", "blast radius", "tripwire", "timebox", and "seam". Write the ordinary word: a test, a stage, a time limit. There are no sanctioned exceptions: the tests under `tests/host` are the host tests, and the check suites are named by their targets, `tests/run style` and `tests/run host`. This rule's own text, the checker's word lists, and its test fixtures are the only places these terms may appear.

Do not add "trap", "bench", "gate", "era", "surface", or "sight" to the mechanical list. Each has a valid use in this tree: a shell trap, a test bench, hardware gates, an attack surface, controller generations, and ordinary phrases such as "anywhere in sight". Review must still reject any of them when used as invented shorthand. A mechanical ban is reserved for a term with no valid use in this domain.

Review decides the contextual cases. Reject "honest" or "fiction" when applied to code or data. Reject a "not X, it's Y" reversal and fragments written for effect, such as "Not a detail. A design decision." Do not give hardware or code intentions: a clock does not believe, refuse, or get asked. "The register says X" is ordinary datasheet language and remains valid.

Prefer a concrete subject and verb. Write "the header has a magic number", not "the header carries one". Review "holds", "names", "spells", "sits", and "pins" for the same fault. Keep established technical metaphors, such as a listening socket, a held lock, or a warm cache. Define a term of art at first use. Put a qualification beside the claim it limits. Do not use "trap" for a difficulty, "bench" for a work session, "gate" for a set of checks, "era" for a stage, "surface" for an area of code, or "sight" for a view.

Repeating an ordinary phrase can produce the same private register. `tools/verify/prose_report.sh` compares each watched phrase with human comments from the pinned upstream tarballs and Doom sources, then prints the rate and ratio. A person reads that report. A phrase fails the build only after the reason is recorded. Do not change the baseline in the same commit as the prose that changes it.

Do not build a sentence toward a turn of phrase; state the claim first in plain words: the component, what it does, why. In scratch files and internal reasoning any register is fine; none of it may be committed or shown to the user. Chat replies are held to the same standard as committed prose: short sentences, ordinary words, and no em-dashes there either.

## Comments

Put an explanation in a block comment before the function. Use names and structure to make the body clear; do not narrate it with comments.

The header is a Doxygen-compatible `/**` block: the first sentence is the summary, then prose paragraphs for the contract and the non-obvious why, with `@param` and `@return` only where the meaning is not obvious. Light Markdown, mostly backticks around identifiers. No tag soup.

An in-body comment is allowed only for a deliberate hack or a hardware or ABI fact that cannot be named away. One line, written as documentation, never as a reasoning trace.

## Messages

Output from tools and code uses one shape. Diagnostics go to stderr as `component: detail`, lowercase, with no banners. Progress is `component: action`; an outcome is `component: result: detail`. Kernel and user code use `subsystem: event: detail`. Probe success tokens such as `VMPROBE OK` are test contracts and do not change.

## Ports

musl, BusyBox, Dropbear, TinyCC, and pdpmake are vendored as pinned tarballs, verified by SHA-256, extracted and built by their `build_*.sh` behind stamp files. BusyBox, Dropbear, TinyCC, and pdpmake link against the ported musl through the `muslgcc` wrapper. Do not commit vendored build output or churn the vendored trees. The one exception is `vendor/grub/`: six reviewed, pinned GRUB boot artifacts with their provenance manifest, committed because the generating toolchain does not build on every supported host. They change only through `vendor/grub/regen_grub.sh` and its review procedure.

When a capability is missing, look in BusyBox before writing it. That is the owner's standing decision, and it is why the account tools (`adduser`, `addgroup`, `chpasswd`, `deluser`, `delgroup`) are BusyBox's rather than this project's.

## Hardware

Any operation on real hardware that cannot be undone by rebuilding requires the owner's approval at the time, every time. Present what the host reports about the target, enough that the owner can recognise it, and wait for an approval that names it. A previous approval for the same target does not carry forward to the next run, and neither does one given on another machine, because the name a host gives a device belongs to that host and that moment.

For a disk write, what is presented is the node, its size in bytes, its bus, whether it is internal or external, whether it is a whole disk, and the partition table it currently has. Other hardware has other properties; the rule is that the owner is shown enough to recognise the thing, not that these particular fields exist everywhere.

The Lenovo's disk does not leave its bay. Removing and reinstalling it is an hour of physical work, and no procedure may depend on it. So before any write to that disk that is not a kernel install through `bootctl`, a USB stick carrying the recovery kernel and `reflash` is booted on the machine and shown to work, a copy of every sector about to be overwritten is saved on the host and on that stick, and recovery is the saved boot default until the write has been verified. A procedure that cannot be undone from that stick is not run.

Updating the installed Lenovo is done over SSH and is written down: a kernel through `bootctl` from the running system, a root through the recovery system and `rach2-install --update`, which keeps the machine's accounts, or through `reflash` for a partition the installer does not know about. The Readme section "Installing and updating a machine" has the commands, the disk offsets they depend on, and the credential check that comes before a root write. Read it rather than deriving a path; taking that disk out and putting it back is an hour of physical work.

The system does not depend on a host device path. The kernel probes its supported ATA or AHCI controller and scans the MBR entries for a KRFFS root. Device identity protects the host's destructive commands; it is not an operating requirement. A person must confirm the physical drive before such a command runs.

## Agents

Do the work directly: read the file, check its claims against the code, write the code, review it yourself. The main session is the most capable and the most expensive model here, and its reasoning is what the owner is paying for; a subagent is started only when the work cannot be done here, never for volume. On the Claude side at most two sessions exist, this one and one subagent, and the subagent starts none of its own; a subagent that judges or writes code runs on Opus 5 or stronger, and a weaker model needs a written reason in the plan. On the Codex side at most two units run at once, and the owner's Codex allowance is separate from the Claude one. Do not set Codex's model or reasoning effort in the plugin: the owner adjusts the default as the limits burn. The owner set these limits on 2026-09-05, short of money and of both allowances, after an earlier limit of one Claude subagent and before that a run of five reviewers that consumed a session.

Codex is the writer of prose and the checker of it. Documents, manual pages, Readmes, the proofreading of anything written here, the register rule's contextual half, and, in the milestones on documents and structure, the comments in source and support files: give these to Codex, with the facts and the claim inventory prepared here, and apply what comes back after checking every claim against the code. Codex also reviews code, once per milestone at the end, as the Verification section says. Assign one reviewer to a diff; a delegated review covers at most six files and runs for at most ten minutes. Reviews of two and five files returned findings in eight and ten minutes; a review of twenty-six files and a procedure failed three times without a result. State a fixed total of agents before starting any.

Never report that a delegated job is running without reading its state in the same turn. Ask the reviewer to state the most serious findings first. Do not ask it to write or tee a file because its sandbox refuses that operation. Do not launch it where a tool timeout can kill its process group. Do not let an earlier result determine how many agents a later stage starts; one such run started two hundred agents, exhausted the session, and still required a manual review.

## Verification

A test is proved by making it fail, in three steps: the unmodified test passes; the mutation, compiled into the artifact the test runs, makes it fail; the source restored and rebuilt passes again. Drive the mutation from a script that restores the source in a trap, because a mutation can hang the suite and outlive the command that applied it, leaving the tree mutated, and stop the test processes before restoring a source they share. Restoring the source is not enough on its own: a `cp` back does not make `make` rebuild, so several runs here re-tested a stale binary and reported PASS for a mutation that had never been compiled. Touch the file, or rebuild explicitly, before believing the result. One mutation proves that one fault is caught, not the whole test.

A step that costs the owner time, allowance or a device is checked before it is started. That covers a build from clean, a full set, a Codex round, a hardware sitting, and any run a person waits for. The check is to read what would stop it: `dev-log/50-shortcomings/plan.md`, the milestone's notes, and the code the step depends on. Where a form is known to work, use that form. A minute of reading is worth an hour of running, and a step whose conditions were not checked is not started.

A build while a run is in flight voids that run. The suites read the kernel and the images from `build/`, so a rebuild part way through replaces what the remaining suites boot and the finished ones were judged on. Before `make`, check for a running set with `ps` and either wait or stop it deliberately. On 2026-09-07 two full sets of about thirty-five minutes each were lost to this, both times by rebuilding the moment a fix was written.

A new row, suite or tool is proved small before it is run at size. A row that will take hours at the machine's size is run first on the smallest inputs that exercise every step, a 64 MiB disk where the real one is 232 GiB, and only a row that has passed there is started at size; the first run at size is never the first run of the code. A run that is killed is not restarted until every process of it is gone, since a killed run's shell goes on to its cleanup and removes what the next run made. A killed run gets the ordinary signal, so what it holds is released, and what it may have left, a lock file or a mounted image, is looked for before the next run. A commit that changes a kernel header or a host test source is preceded by `tests/run style host`, not followed by it. The owner set this on 2026-09-09, after three days of complaints and a day in which a rehearsal row was started at size three times and failed each time on a fault of its own that a small run would have shown in two minutes.

A run's rate is read at its first stage and a slow one is stopped and explained, not paid for. A copy, a hash or a boot that goes at a small fraction of the rate a similar step had before is a symptom: the emulator's USB storage moves data at tens of kilobytes a second where its IDE moves megabytes, a wrong block size makes a call per sector, a deadline waits out its whole period. The run is stopped, the cause found and removed, and only then started again; a run left to crawl for hours because the number was written down as the emulator's rate is the waste this rule is for. The owner set this on 2026-09-09, after a rehearsal was left to copy an image at 75 KB a second for three hours.

A run in flight is watched, not waited on. Stream its log and react to the first failing line, since most failures arrive in the first seconds and the rest of the run is then wasted. Do not sleep until a run ends to find out how it ended.

The whole output of a long run is kept. Write it to a file and read the file; never pipe it through `tail` or `head`, which throws away the failure and forces the run again. On 2026-09-07 a full set was piped through `tail -22`, its five failures were lost, and the suites were started a second time to see what the first run had already said.

A number is believed only with the binary that produced it. `make` does not rebuild the recovery kernel or the install stick, so a measurement of a fix on a stick, an ISO or a recovery boot first checks that the artifact carries the change: its timestamp against the source's, or a string the change added. On 2026-09-09 a fix was measured three times on a stick whose kernel was the old one, and each run reported the old rate as the fix's.

A run that fails part way is resumed at the failing step. Rebuild what changed, run the suite that failed, and run the others again only when the change could have touched them. Work that does not depend on a running job is started beside it. When a failure repeats in a new form, write down the general rule and keep the instance as its example.

On 2026-09-07 this was ignored three times in one hour. A clean build was started twice in the two forms entry 5 of the shortcomings list says fail, and then a whole set was restarted from its first suite rather than resumed at the failure.

A milestone is run for speed. Write the code of every phase, reviewing it here as it is written, and run only what a change needs on the way: a compile, the test rows or the smallest suite that covers the change, and a new check's mutation once, when it is written. A Readme whose contract a commit changes gets its facts in that commit and its prose from Codex without a review of the phase. Do not run the full set, the host tier or a Codex review after a phase. At the end of the milestone, once, in this order: build from clean, which is `build/` removed and the vendor projects rebuilt only when a port or its toolchain changed; `tests/run all`; everything that comes up fixed; the whole milestone's diff to Codex for a harsh review, in units grouped by contract and sized so a unit finishes in ten minutes, six files at most, each unit given the shared interface changes and free to read what its diff depends on, most serious findings first; every finding fixed, or rejected in `notes.md` with the source that shows it wrong; `tests/run all` again; then the machine, only in a milestone whose plan has a machine phase, and a destructive write on the machine only after the review of the code that performs it. The machine phases are gathered so that the owner is needed for one sitting in such a milestone: every step is prepared before the owner is called, the steps run one after another, and the sitting ends with the owner free. A milestone with a machine phase runs that sequence once before the sitting, since the write comes after the review, and reviews the fixes the machine produced once more at its close. The other milestones prove their work in QEMU and do not touch the machine. A kernel installed through `bootctl` over SSH when the machine happens to be on is permitted, never required. An expired review is incomplete, not clean. `tools/verify/prosecheck.c` runs before the review; the register rule's contextual half is part of it. The milestone's Readme records what was accepted: the commit, the build options, the image hashes, the test command and its result, and the hardware arrangement.

A problem found on the way that the milestone did not set out to fix is written into `dev-log/50-shortcomings/plan.md` the same day, with its severity, how to reproduce it, the contract it breaks, what it depends on, and the test that will show it fixed. Then it is fixed in the milestone that found it, with its entry saying so, when the milestone cannot close without it or when leaving it would slow every later run, review or sitting: a disk path that turns a suite into hours is fixed now, not carried to 50. Only a problem that costs nothing to carry is left for milestone 50. The owner set this on 2026-09-07 after a slow disk path was deferred by the letter of the earlier rule and cost a day. That milestone fixes every entry before the documents are written, and the list stays open through the final acceptance: an entry made during the manuals or the audit reopens the technical work before the document that describes it is accepted. Nothing is left to be reported at the end as found and unfixed.

## Git

Commit in scoped units, one logical change each, directly on the branch, with no attribution of any kind. Never push. The remote is the owner's to update, and a push cannot be taken back once anyone has fetched it; ask, and let the owner do it. The house style is one line, no trailing period, beginning with a capitalised verb in the imperative, saying what the commit does and not how it was arrived at; `git log --oneline -20` is the reference. A text-only cleanup changes no behavior and no built artifact. Keep the vendored trees out of the diff.

## Documentation

One Readme per component. A change that alters a contract rewrites the owning Readme in the same commit; it does not append. Docs state the current contract; history lives in commit messages. The records of how each milestone was planned and built live in `dev-log/`. The facts of a document are gathered here; the prose is Codex's, as the Agents section says.
