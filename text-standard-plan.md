# Rach2 text and documentation standard, and the refactor to reach it

## Status

Draft for review. Items marked OPEN are still being decided. Nothing here is applied to the tree yet, and `prompts/` and `PLAN.md` have not been moved.

## Purpose

Every piece of written language in the repository should read as if one careful author wrote it. A student opening any file should learn from it, not wade through slop. This document defines the standard for prose in documentation, comments in source, and messages printed by tools and code, then lays out a phased, verifiable refactor that brings the existing tree to that standard and enforces it so it cannot regress.

## Scope

In-house files only: the kernel (`rach2/`), the host tools (`krffs/`, `tools/`), the shell and utilities (`ish/`, `utils/`, `tests/`), the build files (`Makefile`, `*/Makefile`, `mk/`), and the in-house Markdown. Out of scope: the vendored trees (`libc/musl-*`, `busybox/*`, `dropbear/*`, `ports/doom`) and the recorded prompts.

## Voice

The target is the prose of the people who taught systems by writing it down well: Kernighan and Ritchie in The C Programming Language; Kernighan and Pike in The Practice of Programming and The Unix Programming Environment; W. Richard Stevens in Advanced Programming in the UNIX Environment and TCP/IP Illustrated; Knuth for precision. The Unix manual pages are the model for reference docs. Lions' Commentary on UNIX is the model for explaining kernel code to a student. The shared qualities: terse, exact, concrete; explain the part that is not obvious and stop; never reach for a slogan.

## Prose in documentation

Plain technical prose, present tense, third person. A document states what the component is, the contract it exposes, the constraints that matter, and how to build or test it. No development diary, no reasoning notes, no slogans, no repeated justification. History belongs in commit messages, not in stable docs. One Readme per component. When a change alters a contract, its Readme is rewritten in the same change, not appended to.

Formatting rules for Markdown:

* Do not wrap prose to a column. One paragraph is one line; the editor and the renderer wrap it. This applies to Markdown only. Code comments are wrapped, at the 80-column width the source uses.
* Bullet lists use `*`, never `-`.
* Do not lean on bold (`**`) for emphasis. Let the sentence carry the weight.
* Avoid tables. Prefer prose or a list. Use a table only for data that is genuinely tabular: short cells the reader scans across rows and down columns. When a table is used, its separators line up in the raw file and the cells stay short enough to read. A misaligned or overstuffed table is worse than the list it replaced.

## Punctuation and characters

ASCII only in in-house prose and comments: no U+2014 or other Unicode dashes, no smart quotes, no ellipsis character.

No em-dash, whether written as U+2014 or as a spaced `--`. Recast the sentence: split it, or use a colon, semicolon, or parentheses. The `--` and `--flag` forms stay legal inside command examples, shell option parsing, and quoted tool output, where they are code rather than prose.

## Vocabulary and voice

Filler, slogans, and changelog phrasing are not allowed in stable text: phrases like "the crux", "for free", "by design", "belt-and-suspenders", marketing adjectives used as filler, and diary phrasing like "the reason is" or "next phase".

This is not enforced by a banned-word list. Greping for phrases is fragile: it raises false positives on ordinary words, and it never covers the real space of bad writing. Voice is judged, not greped. The author applies judgment when writing, and a review pass (the model and Codex) flags text that reads like a machine wrote it. The mechanical gate is reserved for objective checks that cannot misfire, described under Enforcement.

## Messages from tools and code

One shape, so every tool and subsystem reads the same. Diagnostics go to stderr, prefixed with the component name, lowercase, no banners:

    component: detail                  a diagnostic or error
    component: action                  progress
    component: result: detail          an outcome

Kernel and user code use the same shape with the subsystem as the component:

    subsystem: event: detail

Probe success tokens such as `VMPROBE OK` are contracts read by the test harness and do not change.

## Comments

Default: the explanation lives in a block comment immediately before the function, never inside the body. The body is made clear by naming and structure, not narrated.

In-body comments are allowed only when something genuinely needs explaining at its exact line and cannot be named away: a deliberate hack or workaround, or a hardware or ABI fact. Even then the comment is written as documentation, one line, stating the fact. It never reads like a note to oneself or a reasoning trace. For example, `/* COM1 raises no receive interrupt; the timer polls it. */`, not a sentence about why the author chose to poll.

### Header format

The header is a Doxygen-compatible block comment using Markdown in the body. This is the chosen form. It is the one documentation-comment convention that tools for C actually parse (Doxygen, and through it Sphinx and Breathe), it reads as prose, and it keeps a generator possible without committing to one. The Linux kernel's `kernel-doc` is the stricter alternative, requiring a tag for every argument; it is not used here.

Shape:

    /**
     * One sentence saying what the function does. Doxygen takes this as the brief.
     *
     * Prose paragraphs for the contract: invariants, locking, ownership, the ABI or
     * hardware facts, the non-obvious why. Markdown is allowed but kept light, mostly
     * backticks around identifiers.
     *
     * @param name   meaning, only when it is not obvious from the name and prose
     * @return       meaning, only when it is not obvious
     */

Rules:

* The first sentence is the summary. Keep it to one line.
* Parameter and return tags are optional. Use them only where the meaning is not already clear. Do not pad a header with tags that restate the signature.
* No tag soup. The body is prose, not a form.

Example, rewritten from the current `schedule()` in `rach2/sys/proc/sched.c`, whose body today is narrated line by line while its header says nothing:

    /**
     * Select the next READY process in round-robin order and switch to it.
     *
     * Interrupts stay disabled across the publication of `current_proc` and the call
     * to `switch_context`. A timer interrupt in that window could observe the new
     * process while still running on the old stack and corrupt the saved register
     * frame.
     *
     * Returns without switching when scheduling is disabled, no process is current,
     * no other process is READY, or the chosen process is already current.
     */
    void schedule(void)

OPEN, doc generation: a generator is not built now. The structure above is enough for an index or an audit. Revisit a generator only if a real consumer appears.

## CLAUDE.md and AGENTS.md

`CLAUDE.md` at the repo root is normative policy, not a second Readme. It stays lean and capped (proposed budget: 120 lines). A new rule replaces or tightens an existing one rather than accreting. Sections:

    1. Project facts: C89, freestanding 32-bit x86 kernel, musl userland.
    2. Build: GNU Make, the tiers, .build/, explicit kernel C_SRC, no hidden rituals.
    3. Text style: the voice, with the named exemplars, and a pointer to this standard.
    4. Comments: header-first, the narrow in-body exception.
    5. Messages: the component-prefixed format.
    6. Ports: musl, BusyBox, Dropbear; pinned tarballs, sha gate, stamp files, muslgcc.
    7. Verification: make doctor, make verify-style, make verify-host; per-change review.
    8. Git: scoped commits, one logical unit, no attribution, no behavior change in a
       text-only cleanup, no vendored churn.
    9. Documentation discipline: docs describe current contracts; history lives in
       commits; one Readme per component, updated in the same change.

`AGENTS.md` is one short file pointing Codex at `CLAUDE.md`. The gate fails if it stops doing so.

## Enforcement

A dedicated checker, `tools/textcheck.sh`, called by `tools/check.sh`. It uses `grep`, `awk`, and `find` with explicit file lists (`rg` is not available on this machine). It checks only objective things that cannot misfire:

* Non-ASCII bytes in in-house `.c`, `.h`, `.sh`, `.mk`, and `.md`.
* U+2014 and other Unicode dashes.
* A spaced `--` used as prose, in comments and Markdown, with an allowlist for command examples.
* `//` comments in C (already checked).
* Markdown bullets that use `-` instead of `*`.
* Markdown source tables whose separators do not line up.

Two checks need a small parser rather than a grep:

* In-body comments: a C-aware scan for comments between a function's braces. During migration a reviewed exception may carry a `STYLE: local-invariant` marker, audited by hand.
* Header presence: enforced first on public (non-static) functions, later on subsystem-critical static ones.

Voice, vocabulary, and whether a comment earns its place are judged in review, not gated. The checker runs advisory (warning, non-failing) until the tree is clean, then becomes a hard gate.

## Phased refactor

Each phase changes text only, never behavior. Each ends with `make verify-style` and `make verify-host` green, a real boot and the SSH capstone where touched, a harsh independent Codex review of the phase with every finding fixed, and one scoped house-style commit with no attribution. Message-string changes are made in lockstep with any test or harness that reads them; probe success tokens are left alone. Voice is the one thing the gate cannot judge, so Codex review is its enforcement: every phase that touches prose or comments is read by Codex for slop and tone, not just correctness.

    0. Land CLAUDE.md, AGENTS.md, and tools/textcheck.sh in advisory mode. Do the
       housekeeping below.
    1. Punctuation and Markdown formatting: remove em-dashes, unwrap doc prose, switch
       bullets to *, strip bold abuse, align tables.
    2. Vocabulary and voice: rewrite slogan and diary phrasing across comments,
       Markdown, shell, and Makefiles, by judgment.
    3. Messages: remove slop from tool diagnostics and standardize the build-script
       progress to the prefixed `component: action` form. The numbered verify gate and
       the doctor status rows are deliberate aligned displays and keep their shape; the
       kernel boot log already reads as `subsystem: event` and carries no slop, so it
       stays. Probe success tokens and the [PASS]/[FAIL] markers are contracts, untouched.
    4. Docs: rewrite the high-traffic Readmes (root, mk/, libc/, the kernel subsystem
       Readmes).
    5. Foundation-era C comments: remove restating body comments, write real headers,
       subsystem by subsystem.
    6. Newer dense C (the musl and build era): move broad explanations to headers, keep
       the ABI and POSIX facts.
    7. Header audit: extend from public functions to subsystem-critical static functions.
    8. Flip the text checks from advisory to a hard gate.

## Housekeeping: the dev-log directory and the retired plan

`prompts/` holds only milestone 1, the foundation OS, recorded as the prompts that built it. Milestone 2 was driven from `PLAN.md` with no recorded prompts. The directory holds textual records of how each milestone was planned and built, not prompts as such.

Rename `prompts/` to `dev-log/`, one subdirectory per milestone:

    dev-log/
      01-foundation/                the existing 35 prompts, its Plan.md, its Readme.md
      02-networking-and-userland/   PLAN.md retired here as plan.md, plus a short Readme
                                    noting it was goal-driven with no recorded prompts
      03-text-standard/             this plan, once approved

Milestone 2 grew the kernel into a networked system running a musl userland that accepts SSH logins: the TCP/IP stack, the musl C library, BusyBox, Dropbear, and the build modernization. It is named `networking-and-userland`. Phase 0 retires the current root `PLAN.md` into `dev-log/02-networking-and-userland/plan.md`. This file keeps the name `text-standard-plan.md` at the repo root while the refactor runs, and retires into `dev-log/03-text-standard/plan.md` when the milestone is done.

## Open to your veto

Everything above is decided, including the names and the header form. Override any of it if you disagree: a name other than `dev-log` or `networking-and-userland`, `kernel-doc` headers instead of Doxygen, or a different message shape.
