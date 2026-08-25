# Milestone 30 checkpoint

This file holds only the resume state for the item in progress. Replace its contents instead of appending.

## Position

* Active phase: 3, the interface.
* Item in progress: not started.
* Exact next action: land the FUSE split in `krffs/Makefile` (core objects without FUSE flags, mount tool off the default graph), then build `tests/run` with the complete suite registry from the plan's end state.

## Phase record

* Phase 1 complete at e4d3187. Baselines: cold `make distclean && make full` 60 s; no-op `make full` 3 s; `verify-style` 23 s; `verify-host` 37 s; `make test` 96 s, 63 of 63.
* Phase 2 complete: the generator joins wrapped recipes, selects commands by first word, escapes control characters, validates before publication, and its 5-case `--selftest` runs in check 12. Both restored defects (no join, no tab escape) made the selftest fail; the published database parses as JSON with 203 entries.
