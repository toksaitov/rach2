# Milestone 30 checkpoint

This file holds only the resume state for the item in progress. Replace its contents instead of appending.

## Position

* Active phase: 3, the interface.
* Item in progress: preflight done and proved (missing tool named before compilation; warmed cache detects a hidden tool; script change invalidates; 53 ms cached hit; clean and distclean bypass it). `doctor` is a notice alias for the direct setup report, FUSE and pkg-config moved to doctor's optional group. Next: the `help` and `compile-commands` retirement notices, closing phase 3.
* Exact next action: alias `help` (the Readme is the interface) and add the `compile-commands` notice, then run style, host, and `make test` and commit.

## Phase record

* Phase 1 complete at e4d3187. Baselines: cold `make distclean && make full` 60 s; no-op `make full` 3 s; style 23 s; host 37 s; `make test` 96 s.
* Phase 2 complete at 6ef65d2: compilation database generator fixed, selftest in check 12, both restored defects failed the selftest.
* Phase 3 in progress: FUSE split at 131bd4a (default KRFFS build needs neither FUSE nor pkg-config; `make -C krffs mount` is the optional tool). Runner landed: `tests/run` reads `tests/registry` (43 suites, five fields), `make test` delegates to `tests/run default`, `verify-style` and `verify-host` are retirement aliases for `tests/run style` and `tests/run host`, and 26 current documents name the new commands. Proofs: unknown suite exits 2 with the valid list; style and host pass through the runner; a planted banned term made `tests/run style` fail and the restore passed again; `make test` boots 63 of 63 through the runner.

## Sequencing note

The `*-test` targets keep their recipes and print no retirement notice while the runner dispatches through them; their bodies move to `tests/system/suites/` in phase 6, and only then do the old names become notice-printing aliases. The registry's prerequisites field stays `-` until then.
