# Milestone 6 review: results

The plan in `plan.md` was executed end to end. This records what it found and what
was done; the per-fix detail lives in the commit messages.

## Method

Phase 0 fixed a green baseline from a from-scratch `make distclean && make full`: the
three images at 10/10/16 MB, `make test` at 27/0, `make verify-style` 8/8, `make
verify-host` at 11 seam tests, three clean boots at 10/10 self-tests, and the os-real
SSH capstone over pubkey auth. Every later result is judged against it.

The code review (areas: kernel-core, io/fs, drivers, network, userland, build/host)
and the comments/docs/support review each ran as a fan-out: a finder per area, then an
independent skeptic that tried to refute every finding before it counted. The
manual-test phases were run by hand against the baseline (boots, the syscall probes,
the musl integration programs, the ping/udp/tcp/socket/dns network probes against a
host echo server and live DNS, the host KRFFS tools, the ISH shell and utilities, and
the entropy fail-closed path). The fixes were then reviewed once more by an independent
Codex pass; every finding it raised was fixed or consciously resolved before commit.

## Findings

The reviews surfaced 63 findings that survived refutation: 36 from the code review (1
critical, 1 high, 12 medium, 22 low), 25 from the comments/docs/support review (5
medium, 20 low), and 2 from the manual tests (the test image staged no `/etc`, so three
musl probes failed silently; the entropy boot message carried a spaced em-dash the gate
could not see).

## Fixed

Fourteen scoped commits landed the safe fixes:

* httpd: a crafted request could overflow the fixed page buffer through the unbounded
  `put_*` append helpers; every byte store now routes through a bounded output cursor.
* KRFFS: renaming a file onto a directory freed the directory and orphaned its subtree;
  it is now refused.
* UDP: an explicit `bind` leaked a socket's auto-bound ephemeral port, and `bind(0)`
  leaked port 0; both are handled.
* execve: more than 15 arguments truncated silently rather than returning E2BIG.
* /proc/<pid>/stat reported the pid for pgrp and session instead of the real pgid/sid.
* ISH leaked a descriptor per redirected command (the parent never closed the
  redirection fds).
* Doom: `__divdi3` looped forever for INT64_MIN; `calloc` did not check for overflow;
  the PC-sound driver trusted a lump's sample count past the lump size.
* fsck printed "completed successfully" even after a structural fault broke the walk.
* The vendored build scripts now apply `patches/` as their Readmes promised (a no-op
  until a patch exists).
* The text gate now catches em-dashes in make `$(error)` directives and the Python
  tools, and the diagnostics it flagged (plus the entropy message and two banner
  words) were recast.
* The test image stages a minimal `/etc`, so the musl file and auth probes pass.
* The kernel, KRFFS, network, and Doom Readmes were corrected to the current contract;
  stale, oversized, and copied comments across the tree were tidied.

After the fixes the gate, the three boots, and the SSH capstone are green and match the
baseline, with the musl suite now at 16 of 16.

## Deferred

Three items were left as backlog, each larger than a safe one-commit fix:

* The kernel tree still has about 645 comment lines past 80 columns; wrapping them is a
  normalization pass of its own, like the one the tests, utils, and host tools already
  had.
* The older driver, network, audio, and fs diagnostics use a Title-Case `Component:`
  prefix instead of the lowercase house form; the two with banner words were fixed, the
  rest is a tree-wide casing pass.
* A process that exits without reaping its children does not reparent them, so an
  orphan can leak its frames and slot. A correct fix needs a persistent reaping init,
  which the current model (init is the first user process and exits) does not have.
