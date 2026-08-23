# Milestone 29 notes

This is the persistent record for information needed after a compaction or between documentation files. It is not evidence for a factual claim. Recheck every item against the repository before using it in an edit.

## Cross-document contradictions

* `CLAUDE.md` and `Readme.md` described all build output as living under `build/`. The root Makefile and `mk/build.mk` make that true for in-house components; vendored projects also build extracted trees and musl's install under `vendor/`. `CLAUDE.md` is corrected. Recheck the root Readme in its own pass.
* `CLAUDE.md` and `Readme.md` said the build does not run QEMU without distinguishing image builds from test targets. Image build targets do not boot QEMU. The run scripts and the test harnesses do. `CLAUDE.md` is corrected. Recheck the root Readme in its own pass.

## Non-document defects

None recorded yet.

## Facts for subsystem passes

* `block_probe` tries ATA by default. With `rach2.disk=ahci`, it tries AHCI first and falls back to ATA. Disk-root discovery scans the four MBR entries and mounts the first entry whose first sector is a valid KRFFS root; it does not depend on a host device path.

## Decisions and deferred questions

None recorded yet.
