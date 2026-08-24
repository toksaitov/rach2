# Milestone 6 review: results

The plan in `plan.md` was completed. This file records its findings and changes; the commit messages contain the details of each fix.

## Method

Phase 0 established a baseline with `make distclean && make full`: three images at 10/10/16 MB, `make test` at 27/0, `make verify-style` at 8/8, `make verify-host` at 11 host tests, three clean boots at 10/10 self-tests, and the `os-real` end-to-end SSH test with public-key authentication. Every later result was compared with this baseline.

The code review covered the kernel core, I/O and file systems, drivers, network, userland, and host build tools. The comments, documentation, and support files received a separate review. Each area had one finder and an independent reviewer who tried to refute every finding before it counted. The manual-test phases compared boots, syscall probes, musl integration programs, network probes, host KRFFS tools, the ISH shell and utilities, and the entropy refusal path with the baseline. The ping, UDP, TCP, socket, and DNS probes used a host echo server and live DNS. An independent Codex pass reviewed the fixes. Every resulting finding was fixed or explicitly resolved before commit.

## Findings

The reviews produced 63 findings that independent review confirmed: 36 from the code review (1 critical, 1 high, 12 medium, 22 low), 25 from the comments, documentation, and support review (5 medium, 20 low), and 2 from the manual tests. The test image staged no `/etc`, so three musl probes failed silently. The entropy boot message contained a spaced em dash that the checks did not inspect.

## Fixed

Fourteen scoped commits applied the selected fixes:

* httpd: a crafted request could overflow the fixed page buffer through the unbounded `put_*` append helpers; every byte store now uses a bounded output cursor.
* KRFFS: renaming a file onto a directory freed the directory and orphaned its subtree; the operation is now refused.
* UDP: an explicit `bind` leaked a socket's auto-bound ephemeral port, and `bind(0)` leaked port 0; both are handled.
* execve: more than 15 arguments truncated silently rather than returning E2BIG.
* /proc/<pid>/stat reported the pid for pgrp and session instead of the actual pgid/sid.
* ISH leaked a descriptor per redirected command because the parent never closed the redirection fds.
* Doom: `__divdi3` looped forever for INT64_MIN; `calloc` did not check for overflow; the PC-sound driver trusted a lump's sample count past the lump size.
* fsck printed "completed successfully" even after a structural fault broke the walk.
* The vendored build scripts now apply `patches/` as their Readmes promised. This is a no-op until a patch exists.
* The text checks now detect em dashes in make `$(error)` directives and Python tools. The diagnostics they identified, the entropy message, and two banner words were recast.
* The test image stages a minimal `/etc`, so the musl file and auth probes pass.
* The kernel, KRFFS, network, and Doom Readmes were corrected to the current contract. Outdated, overlong, and copied comments across the tree were revised.

After the fixes, the checks, three boots, and end-to-end SSH test pass and match the baseline. The musl suite now passes 16 of 16 tests.

## Deferred

Three items were deferred because each required more than one contained commit:

* The kernel tree still has about 645 comment lines longer than 80 columns. Wrapping them requires a separate normalization pass like the earlier pass over the tests, utilities, and host tools.
* The older driver, network, audio, and file-system diagnostics use a title-case `Component:` prefix instead of the lowercase house form. The two containing banner words were fixed; the rest require a tree-wide casing pass.
* A process that exits without reaping its children does not reparent them, so an orphan can leak its frames and slot. A correct fix requires a persistent reaping init, which the current model lacks because init is the first user process and exits.
