# Milestone 30 checkpoint

This file holds only the resume state for the item in progress. Replace its contents instead of appending.

## Position

* Active phase: 3, the interface.
* Item in progress: default scope done (clean `make` builds everything in 59 s, no-op 3 s, aliases print notices, style and host pass). Next: the cached preflight replacing `doctor`, then the `help` retirement.
* Exact next action: write `tools/preflight.sh` with the fingerprint cache, wire it into `make` and `tests/run`, alias `doctor`, and prove the missing-tool, warmed-cache, invalidation, cheap-hit, and clean-bypass cases.

## Phase record

* Phase 1 complete at e4d3187. Baselines: cold `make distclean && make full` 60 s; no-op `make full` 3 s; style 23 s; host 37 s; `make test` 96 s.
* Phase 2 complete at 6ef65d2: compilation database generator fixed, selftest in check 12, both restored defects failed the selftest.
* Phase 3 in progress: FUSE split at 131bd4a (default KRFFS build needs neither FUSE nor pkg-config; `make -C krffs mount` is the optional tool). Runner landed: `tests/run` reads `tests/registry` (43 suites, five fields), `make test` delegates to `tests/run default`, `verify-style` and `verify-host` are retirement aliases for `tests/run style` and `tests/run host`, and 26 current documents name the new commands. Proofs: unknown suite exits 2 with the valid list; style and host pass through the runner; a planted banned term made `tests/run style` fail and the restore passed again; `make test` boots 63 of 63 through the runner.

## Sequencing note

The `*-test` targets keep their recipes and print no retirement notice while the runner dispatches through them; their bodies move to `tests/system/suites/` in phase 6, and only then do the old names become notice-printing aliases. The registry's prerequisites field stays `-` until then.
