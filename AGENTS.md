# AGENTS.md

The project policy for Rach2, for every coding agent. Codex reads this file directly; Claude Code reads it through the `@AGENTS.md` import in `CLAUDE.md`, which adds the rules Claude Code alone needs. Procedures needed only for some tasks are skills in `.agents/skills/`, which Codex runs as `$name` and Claude Code as `/name` through `.claude/skills/`:

* `milestone`: working through a milestone of `dev-log/` and closing it.
* `testing`: running suites on the testing host, and writing suites and mutation tests.
* `machine`: writing the flash drive or a Lenovo disk, and updating the installed Lenovo.

This file states the rules in force; their history is in commit messages and `dev-log/`. When a rule changes, replace the old text. When a failure repeats in a new form, write the general rule here or in the skill it belongs to, and record the incident in `dev-log/`.

## Project

* Rach2 is an educational 32-bit x86 operating system. Its readers are students and developers, and the code and its prose are meant to teach.
* The kernel is freestanding C89 (`-ansi`, no libgcc), cross-built with `i686-elf-gcc`. The same sources also build on Rach2 with the staged TinyCC and pdpmake, so kernel code stays inside the dialect both compilers accept.
* The userland is the ported musl libc, the BusyBox shell, the Dropbear SSH server, and TinyCC with pdpmake to drive it.
* Read `Readme.md` first, and a component's `Readme.md` before changing its code. `dev-log/` records how each milestone was planned and built; [its index](dev-log/Readme.md) lists them in order, and the last acceptance is `dev-log/06-review/results-2026-09-20.md`.
* For a new machine, `dev-log/26-portable/arrival.md` lists what a clone does not provide, and `tools/Readme.md` has the setup for macOS, Debian and Ubuntu.

## Roles

* Claude Code does the work: code, tests and prose. Codex does work only when the owner asks for it.
* Codex reviews at the end of each batch, as Batches says, and otherwise when the owner asks.
* Claude Code works alone, with no subagents of any kind. Its one helper is the Codex plugin, one Astra instance at xhigh effort at a time, used for review. Codex, when it runs, does its work itself and starts no subagents.

## Batches

* Work comes in batches: one phase of a plan, or several phases the owner names as one batch.
* Build the whole batch first. Write every item, compile as you go, and check a detail by hand only when reading and reasoning cannot settle it. Start no suite, full set, review or other long run while the batch is being built.
* Work on the real machine comes last in the building, after every item is written and before any check.
* Then check the batch once, ordering the steps by cause and effect, so that nothing is checked before a later step changes it:
  1. The compile and `tests/run style`, which are fast and catch mechanical faults.
  2. The Codex review of the whole batch, and its fixes.
  3. The build from clean and `tests/run all`, with the mutation tests of new checks, and their fixes.
  4. For every fix after that, only the checks it could affect and a review of its diff, repeated until a round finds nothing.
  5. If the checks changed what the machine runs, the machine gets the final build.
  6. A last `tests/run all` on the final tree closes the batch.

## Build

* `make` builds the kernel, the host tools, the guest programs, five of the vendored projects (musl, BusyBox, Dropbear, TinyCC, pdpmake), Doom, and the test, os-simple and os-real RAM images. The first build takes minutes; later ones rebuild only what changed, header dependencies included.
* `make test` runs the style checks, the host tests and the core tier of suites; `tests/run NAME` runs one suite; `make clean` and `make distclean` clean up. The build checks its tools before it compiles with the cross toolchain, and `sh tools/setup/doctor.sh` reports on the wider machine setup.
* Do not run a `build_*.sh` script by hand, and do not use `make clean` as a routine step.
* Every in-house component writes its objects, `.d` files and binaries under the top-level `build/`; shared host Make rules are in `mk/`. Image targets run no QEMU: `rach2/run*.sh` boot an image, and the test scripts start QEMU for the suites.
* A source file is listed once, in its component's manifest (`rach2/sources.mk`, `utils/programs.mk` or `ish/sources.mk`), which both the host Makefile and the POSIX `guest.mk` that pdpmake reads on Rach2 include. `make check-sources` and `tools/verify/guestcheck.c` check the lists, and `tests/run selfbuild` builds all three components on Rach2 under QEMU.
* musl, BusyBox, Dropbear, TinyCC, pdpmake and e2fsprogs (whose `e2fsck` and `resize2fs` the recovery image stages) are vendored as pinned tarballs checked by SHA-256, built by their `build_*.sh` scripts with their upstream build systems, behind stamp files; the five besides musl link against the ported musl through the `muslgcc` wrapper. Do not commit their build output or churn their trees.
* The one committed exception is `vendor/grub/`: six reviewed, pinned GRUB boot images with their provenance manifest, committed because the toolchain that makes them does not build on every supported host. They change only through `vendor/grub/regen_grub.sh` and its review procedure.
* When a capability is missing, look in BusyBox before writing it. That is the owner's standing decision, and it is why `adduser`, `addgroup`, `chpasswd`, `deluser` and `delgroup` are BusyBox's.

## Testing

* Outside a plan's batches, check a change with the tests that cover what it changed, and run a Codex review only when the owner asks for one.
* Code is written, reviewed and committed on the Mac mini, which runs a compile or one host test. Every suite, full set (`tests/run all`) and analysis runs on the testing host, as the `testing` skill describes.
* A test is trusted only after it has been made to fail: the unmodified test passes; a mutation, a deliberate fault compiled into what it runs, makes it fail; with the source restored and the artifact rebuilt, it passes again. The `testing` skill has the mechanics.
* A suite judges what the kernel counts, never the clock; seconds in a suite are completion bounds.

## Hardware

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

* Explain a function in a Doxygen-compatible `/**` block before it. Its first sentence is the summary; prose paragraphs follow for the contract and the part of the reason that is not obvious. Use `@param` and `@return` only where the meaning is not obvious, keep Markdown light (mostly backticks around identifiers), and do not pile up tags.
* Let names and structure make a body clear; do not narrate it in comments. A comment inside a body is allowed only for a deliberate hack, or for a hardware or application binary interface (ABI) fact that cannot be named away, and is one line written as documentation, never as a reasoning trace.

## Messages

* Diagnostics go to stderr as `component: detail`, in lowercase, with no banners; progress is `component: action`, and an outcome is `component: result: detail`. Kernel and user code use `subsystem: event: detail`. A probe's success token, such as `VMPROBE OK`, is a test contract and does not change.

## Documentation

* Each component has one Readme. A change that alters a contract rewrites the owning Readme in the same commit, and does not append to it. Documents state the current contract; history is in commit messages and `dev-log/`.
* The manual under `man/` covers the machine. Every program a system image stages or `utils/programs.mk` builds, every BusyBox applet and every system call the kernel registers has a page or an alias in its section; a program that only test images stage needs none. `tools/verify/mancheck.c`, run by `tests/run style`, refuses a tree that lacks one, a page `mandoc -T lint` warns about, an alias that names no page, or a recovery page the recovery manifest does not stage.
* A commit that adds a program, an applet or a call adds its page or its alias line in the same commit, and a commit that changes what one does changes its page. A page states what the code does on this kernel, from a fact sheet gathered from the source, and says where that differs from Linux or from the program's upstream documentation.

## Git

* Never push. The remote is the owner's to update, and a push cannot be taken back once anyone has fetched it; ask, and let the owner do it.
* Commit in scoped units, one logical change each, directly on the branch, with no attribution of any kind. The message is one line with no trailing period, begins with a capitalised verb in the imperative, and says what the commit does, not how it was arrived at; `git log --oneline -20` shows the style.
* A text-only cleanup changes no behavior and no built artifact. Keep the vendored trees out of the diff.
* `tools/git-hooks/commit-msg` refuses a message with an attribution line. `.claude/settings.json` turns off Claude Code's own attribution and turns the hook on when a session starts; `git config core.hooksPath tools/git-hooks` turns it on in any other clone.
