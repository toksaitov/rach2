# CLAUDE.md

Project policy for Rach2. This file is normative and lean. A new rule replaces or tightens an old one; it does not pile on. Codex follows it through `AGENTS.md`.

## Project

Rach2 is an educational 32-bit x86 operating system. The kernel is freestanding C89 (`-ansi`, no libgcc), cross-built with `i686-elf-gcc`; the same sources also build on Rach2 itself with the staged TinyCC and pdpmake, so kernel code must stay inside both compilers' shared dialect. The userland is the ported musl libc, the BusyBox shell, the Dropbear SSH server, and the TinyCC compiler with pdpmake to drive it. The readers are students and developers; the code and its prose are meant to teach.

## Build

GNU Make only on the host. `make doctor` checks the toolchain, `make` builds the base OS, `make full` adds the BusyBox, Dropbear, TinyCC, pdpmake, and Doom capstones and the SSH image, and `make help` lists the rest. The guest build is separate: three POSIX `guest.mk` files (kernel, utils, ish) that pdpmake reads on the machine, held equal to the host lists by `tools/check_guest.py` and proved by `make selfbuild-test`. The shared make logic lives in `mk/`. Every component builds out of tree into one repo-root `build/` directory that mirrors the source tree: the objects, the `.d` files, and the final binaries alike, so the source tree never holds a build product. The kernel source list is explicit (`C_SRC` in `rach2/Makefile`) and guarded by `make check-sources`. There is no manual `build_*.sh` step and no `make clean` ritual: a header edit rebuilds only its dependents. The build does not run QEMU; the scripts in `rach2/` do.

## Writing

All written language reads as if one careful author wrote it. The model for voice is the prose of Kernighan and Ritchie, Kernighan and Pike, W. Richard Stevens, and Knuth, the Unix manual pages for reference docs, and Lions' Commentary for explaining kernel code. Terse, exact, concrete. Explain the part that is not obvious and stop. No slogans, no marketing words, no development diary, no notes to oneself.

* ASCII only in prose and comments. No Unicode dashes, smart quotes, or ellipsis.
* No em-dash, written as U+2014 or as a spaced `--`. Recast with a period, colon, semicolon, or parentheses. The `--flag` and end-of-options `--` forms stay legal in code and command examples.
* Markdown: do not wrap prose to a column, bullets are `*` and not `-`, do not lean on bold, and avoid tables unless the data is genuinely tabular and stays aligned. Code comments do wrap, at 80 columns.

### Register rule for Claude Opus 5 and Fable 5

These models drift into a figurative register that reads as private slang. It is banned from everything a reader sees: chat replies, code comments, docs, Readmes, commit messages, and prompts sent to reviewers. The named tics, and everything of their kind: "load-bearing", "the trap", "the point", "honest" or "fiction" applied to code or data, "worth stating plainly", "full stop", "not X, it's Y" reversals, fragments for drama ("Not a detail. A design decision."), and hardware or code given intentions (a clock that "believes", "refuses", or "is asked"; "the register says X" is fine, that is how datasheets speak). Do not build a sentence toward a turn of phrase; state the claim first in plain words: the component, what it does, why. A metaphor is allowed only when it is the established technical term (a socket listens, a lock is held, a cache is warm). In scratch files and internal reasoning any register is fine; none of it may be committed or shown to the user.

The rule also covers invented shorthand nouns that bury the meaning: "seam", "bench", "gate", "era", "surface", "sight", "timebox", and their kind. Write the ordinary word instead: a test, a laptop session, the checks, a stage, a time limit. Two proper names already established in this repository stay, and only as those names: the verify gate (the `make verify-*` suites) and the seam tests under `tests/host`. Chat replies are held to the same standard as committed prose: short sentences, ordinary words, a term of art defined when first used, and no em-dashes there either.

## Comments

The explanation lives in a block comment before the function, not inside the body. The body is made clear by naming and structure, not narrated.

The header is a Doxygen-compatible `/**` block: the first sentence is the summary, then prose paragraphs for the contract and the non-obvious why, with `@param` and `@return` only where the meaning is not obvious. Light Markdown, mostly backticks around identifiers. No tag soup.

An in-body comment is allowed only for a deliberate hack or a hardware or ABI fact that cannot be named away. One line, written as documentation, never as a reasoning trace.

## Messages

Output from tools and code uses one shape. Diagnostics go to stderr as `component: detail`, lowercase, with no banners. Progress is `component: action`; an outcome is `component: result: detail`. Kernel and user code use `subsystem: event: detail`. Probe success tokens such as `VMPROBE OK` are test contracts and do not change.

## Ports

musl, BusyBox, Dropbear, TinyCC, and pdpmake are vendored as pinned tarballs, verified by sha256, extracted and built by their `build_*.sh` behind stamp files. BusyBox, Dropbear, TinyCC, and pdpmake link against the ported musl through the `muslgcc` wrapper. Do not commit vendored build output or churn the vendored trees.

## Hardware

Any operation on real hardware that cannot be undone by rebuilding requires the owner's approval at the time, every time. Present what the host reports about the target, enough that the owner can recognise it, and wait for an approval that names it. A previous approval for the same target does not carry forward to the next run, and neither does one given on another machine, because the name a host gives a device belongs to that host and that moment.

For a disk write, what is presented is the node, its size in bytes, its bus, whether it is internal or external, whether it is a whole disk, and the table it currently carries. Other hardware has other properties; the rule is that the owner is shown enough to recognise the thing, not that these particular fields exist everywhere.

Nothing in this system is bound to a particular drive: the kernel probes for whatever disk is present and finds its root by scanning partition entries. Device identity is therefore a guard on the host's own destructive commands, never an operating requirement, and a person confirming the drive in front of them is the guard that counts.

## Agents

Do the work directly. Reading a file, judging a claim against the code, and writing prose this project will keep are the work itself, and handing them out costs more than it saves: the writing rules above exist to produce one voice, and a fan-out does not have one.

Delegation is for an independent opinion, not for volume. One reviewer per diff is the shape; the review is worth having because it is independent, not because it is parallel. Where more than one agent is genuinely wanted, the total is decided before any of them starts and stated. A stage whose agent count depends on what an earlier stage found is forbidden: the number that comes back is never the number that was budgeted, and a run that exhausts a session buys nothing, because the work still has to be done afterwards by hand.

## Verification

Every change passes `make verify-style` and `make verify-host`, and a real boot plus the SSH capstone when it touches anything they exercise. Substantial work gets a harsh independent Codex review with every finding fixed before commit. Voice, which no checker can judge, is part of that review.

## Git

Commit in scoped units, one logical change each, directly on the branch in the repo's one-line house style, with no attribution of any kind. A text-only cleanup changes no behavior and no built artifact. Keep the vendored trees out of the diff.

## Documentation

One Readme per component. A change that alters a contract rewrites the owning Readme in the same commit; it does not append. Docs state the current contract; history lives in commit messages. The records of how each milestone was planned and built live in `dev-log/`.
