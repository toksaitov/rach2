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

Updating the installed Lenovo is done over SSH and is written down: a kernel through `bootctl` from the running system, a root through the recovery system and `reflash`. The Readme section "Updating the installed machine" has the commands, the disk offsets they depend on, and the credential check that comes before a root write. Read it rather than deriving a path; taking that disk out and putting it back is an hour of physical work.

The system does not depend on a host device path. The kernel probes its supported ATA or AHCI controller and scans the MBR entries for a KRFFS root. Device identity protects the host's destructive commands; it is not an operating requirement. A person must confirm the physical drive before such a command runs.

## Agents

Do the work directly. Reading a file, checking its claims against the code, and writing the final prose require one consistent judgement and voice. Do not delegate them for volume.

Use delegation only for an independent opinion. Assign one reviewer to a diff. If a task genuinely needs more than one agent, state the fixed total before starting any of them. A delegated review may cover at most six files and run for at most ten minutes. Reviews of two and five files returned findings in eight and ten minutes; a review of twenty-six files and a procedure failed three times without a result.

Never report that a delegated job is running without reading its state in the same turn. Ask the reviewer to state the most serious findings first. Do not ask it to write or tee a file because its sandbox refuses that operation. Do not launch it where a tool timeout can kill its process group. Do not let an earlier result determine how many agents a later stage starts; one such run started two hundred agents, exhausted the session, and still required a manual review.

## Verification

A test is proved by making it fail. Drive a mutation from a script that restores the source in a trap, because a mutation can hang the suite and outlive the command that applied it, leaving the tree mutated. Restoring the source is not enough on its own: a `cp` back does not make `make` rebuild, so several runs here re-tested a stale binary and reported PASS for a mutation that had never been compiled. Touch the file, or rebuild explicitly, before believing the result.

Every change passes `tests/run style` and `tests/run host`, and a real boot plus the SSH capstone when it touches anything they exercise. Substantial work gets a harsh independent Codex review with every finding fixed before commit. The half of the register rule that a person decides is part of that review; `tools/verify/prosecheck.c` decides the other half before the review starts.

## Git

Commit in scoped units, one logical change each, directly on the branch, with no attribution of any kind. Never push. The remote is the owner's to update, and a push cannot be taken back once anyone has fetched it; ask, and let the owner do it. The house style is one line, no trailing period, beginning with a capitalised verb in the imperative, saying what the commit does and not how it was arrived at; `git log --oneline -20` is the reference. A text-only cleanup changes no behavior and no built artifact. Keep the vendored trees out of the diff.

## Documentation

One Readme per component. A change that alters a contract rewrites the owning Readme in the same commit; it does not append. Docs state the current contract; history lives in commit messages. The records of how each milestone was planned and built live in `dev-log/`.
