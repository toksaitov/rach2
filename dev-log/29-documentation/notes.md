# Milestone 29 notes

This is the persistent record for information needed after a compaction or between documentation files. It is not evidence for a factual claim. Recheck every item against the repository before using it in an edit.

## Cross-document contradictions

* `CLAUDE.md` and `Readme.md` described all build output as living under `build/`. The root Makefile and `mk/build.mk` make that true for in-house components; vendored projects also build extracted trees and musl's install under `vendor/`. `CLAUDE.md` is corrected. Recheck the root Readme in its own pass.
* `CLAUDE.md` and `Readme.md` said the build does not run QEMU without distinguishing image builds from test targets. Image build targets do not boot QEMU. The run scripts and the test harnesses do. `CLAUDE.md` is corrected. Recheck the root Readme in its own pass.
* Both root-document contradictions above are corrected in `Readme.md`.

## Non-document defects

* The comment at the top of `rach2/guest.mk` says the response file avoids putting 87 objects on one command line. `OBJS` now has 88 entries, confirmed by `tools/check_guest.py`. This milestone does not edit source comments.

## Facts for subsystem passes

* `block_probe` tries ATA by default. With `rach2.disk=ahci`, it tries AHCI first and falls back to ATA. Disk-root discovery scans the four MBR entries and mounts the first entry whose first sector is a valid KRFFS root; it does not depend on a host device path.
* The current vendor patch count is one for musl, two for BusyBox, none for Dropbear, one for TinyCC, and one for pdpmake. Each build script verifies its pinned tarball by SHA-256.
* `tools/check_guest.py` reports 88 kernel sources, 29 utility programs, five ish objects, and three staged `/src` trees. `tools/selfbuild_test.sh` checks 18 userland smoke tokens.
* `make test` checks each probe's declared outcome; it is not accurate to say that every special probe simply exits zero.
* `make os-real-disk` creates a partitioned disk-image file. `rach2/run.sh` can attach that image through ATA or AHCI and mount it as the persistent root.
* The fresh-Mac manifest uses the Homebrew cask `fuse-t` and the `coreutils` formula, which provides `gtimeout`. The direct commands are `brew install --cask fuse-t` and `brew install coreutils`.

## Decisions and deferred questions

None recorded yet.
