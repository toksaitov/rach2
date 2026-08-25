# Milestone 30 checkpoint

This file holds only the resume state for the item in progress. Replace its contents instead of appending.

## Position

* Active phase: 6, the layout.
* Item in progress: not started.
* Exact next action: move the scripts into `tests/system/` (runner, suites, checkers, fixtures, accept) and the `tools/` role directories (setup, verify, inspect, image, dev), one role per commit, updating every caller and the owning Readme in the same commit, with the renamed-path search from the standing rules. The compilation database generator lands in `tools/dev/` and its make alias joins the retirement list. For each moved suite, mutate its expected token and confirm the runner reaches the moved script.

## Phase record

* Phases 1 to 4 complete: baselines e4d3187; generator 6ef65d2; interface fa0436e (FUSE split 131bd4a, runner e9c5d80, default scope a49cdb9, preflight 88257a0); manifests 391c901, b7bb4bf, f241cdc.
* Phase 5 complete, all nine items: images from recorded inventories (03a03f2), patch series manifests (91c7287), parse-time output healing (ca5cd6a), validation stamps with builtin-rule delegation guards (c5dc1f5), configuration purge fingerprints (f026a72), real component edges only (5fb125b), incremental musl tests (f946e0a), credential timestamps (cec40ce), shared job budget with parallel-race fixes and progress messages (9f0c5f9).
* Closing measurements against the phase 1 baseline (cold 60 s, no-op 3 s): no-op `make` is 1 s; cold serial `make` is 118 s because the vendors now share the caller's job budget instead of choosing a private ten-way `-j` (decided in the design rounds); cold `make -j4` is 42 s with at most 3 concurrent compiles and 149 component interleavings.
* Notable fixes proved along the way: GNU make 3.81 compares whole seconds, so fingerprints purge at parse time instead of using mtime prerequisites; absent optional image inputs are represented by their nearest existing ancestor directory; relative inventory paths must be absolutized so they cannot name phony targets; musl installs legitimately empty headers, so healing tests existence, not size.
