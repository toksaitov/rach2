# CLAUDE.md

Project policy for Rach2. This file is normative and lean. A new rule replaces or tightens an old one; it does not pile on. Codex follows it through `AGENTS.md`.

## Project

Rach2 is an educational 32-bit x86 operating system. The kernel is freestanding C89 (`-ansi`, no libgcc), cross-built with `i686-elf-gcc`. The userland is the ported musl libc, the BusyBox shell, the Dropbear SSH server, and the TinyCC compiler with pdpmake to drive it. The readers are students and developers; the code and its prose are meant to teach.

## Build

GNU Make only. `make doctor` checks the toolchain, `make` builds the base OS, `make full` adds the BusyBox, Dropbear, TinyCC, pdpmake, and Doom capstones and the SSH image, and `make help` lists the rest. The shared make logic lives in `mk/`. Every component builds out of tree into one repo-root `build/` directory that mirrors the source tree: the objects, the `.d` files, and the final binaries alike, so the source tree never holds a build product. The kernel source list is explicit (`C_SRC` in `rach2/Makefile`) and guarded by `make check-sources`. There is no manual `build_*.sh` step and no `make clean` ritual: a header edit rebuilds only its dependents. The build does not run QEMU; the scripts in `rach2/` do.

## Writing

All written language reads as if one careful author wrote it. The model for voice is the prose of Kernighan and Ritchie, Kernighan and Pike, W. Richard Stevens, and Knuth, the Unix manual pages for reference docs, and Lions' Commentary for explaining kernel code. Terse, exact, concrete. Explain the part that is not obvious and stop. No slogans, no marketing words, no development diary, no notes to oneself.

* ASCII only in prose and comments. No Unicode dashes, smart quotes, or ellipsis.
* No em-dash, written as U+2014 or as a spaced `--`. Recast with a period, colon, semicolon, or parentheses. The `--flag` and end-of-options `--` forms stay legal in code and command examples.
* Markdown: do not wrap prose to a column, bullets are `*` and not `-`, do not lean on bold, and avoid tables unless the data is genuinely tabular and stays aligned. Code comments do wrap, at 80 columns.

## Comments

The explanation lives in a block comment before the function, not inside the body. The body is made clear by naming and structure, not narrated.

The header is a Doxygen-compatible `/**` block: the first sentence is the summary, then prose paragraphs for the contract and the non-obvious why, with `@param` and `@return` only where the meaning is not obvious. Light Markdown, mostly backticks around identifiers. No tag soup.

An in-body comment is allowed only for a deliberate hack or a hardware or ABI fact that cannot be named away. One line, written as documentation, never as a reasoning trace.

## Messages

Output from tools and code uses one shape. Diagnostics go to stderr as `component: detail`, lowercase, with no banners. Progress is `component: action`; an outcome is `component: result: detail`. Kernel and user code use `subsystem: event: detail`. Probe success tokens such as `VMPROBE OK` are test contracts and do not change.

## Ports

musl, BusyBox, Dropbear, TinyCC, and pdpmake are vendored as pinned tarballs, verified by sha256, extracted and built by their `build_*.sh` behind stamp files. BusyBox, Dropbear, TinyCC, and pdpmake link against the ported musl through the `muslgcc` wrapper. Do not commit vendored build output or churn the vendored trees.

## Verification

Every change passes `make verify-style` and `make verify-host`, and a real boot plus the SSH capstone when it touches anything they exercise. Substantial work gets a harsh independent Codex review with every finding fixed before commit. Voice, which no checker can judge, is part of that review.

## Git

Commit in scoped units, one logical change each, directly on the branch in the repo's one-line house style, with no attribution of any kind. A text-only cleanup changes no behavior and no built artifact. Keep the vendored trees out of the diff.

## Documentation

One Readme per component. A change that alters a contract rewrites the owning Readme in the same commit; it does not append. Docs state the current contract; history lives in commit messages. The records of how each milestone was planned and built live in `dev-log/`.
