# Milestone 30 checkpoint

This file holds only the resume state for the item in progress. Replace its contents instead of appending.

## Position

* Active phase: 4, shared source manifests.
* Item in progress: phase 4 complete. All three components build from shared manifests (`rach2/sources.mk` at 391c901, `utils/programs.mk`, `ish/sources.mk`), `check_guest.py` checks derivations, includes, and substitution references, and the docs name the new contract. Proofs per component: unchanged output hashes (kernel 148594d0..., utils set f0da6d2d..., ish eaf64ab0...), planted nonexistent sources failing both descriptions by name, and two full self-builds (259 to 261 s) with the guest kernel hash still equal to the milestone 29 record.
* Exact next action: commit the utils and ISH conversions, then start phase 5, incremental correctness, item 1: real image file targets with the staging closure.
* Phase 3 completed at fa0436e: interface is make, make test, tests/run NAME, clean, distclean, run.sh; retired names print notices; preflight cached; FUSE optional; compile-commands alias retires in phase 6 with the script move.

## Phase record

* Phase 1 complete at e4d3187. Baselines: cold `make distclean && make full` 60 s; no-op `make full` 3 s; style 23 s; host 37 s; `make test` 96 s.
* Phase 2 complete at 6ef65d2: compilation database generator fixed, selftest in check 12, both restored defects failed the selftest.
* Phase 3 in progress: FUSE split at 131bd4a (default KRFFS build needs neither FUSE nor pkg-config; `make -C krffs mount` is the optional tool). Runner landed: `tests/run` reads `tests/registry` (43 suites, five fields), `make test` delegates to `tests/run default`, `verify-style` and `verify-host` are retirement aliases for `tests/run style` and `tests/run host`, and 26 current documents name the new commands. Proofs: unknown suite exits 2 with the valid list; style and host pass through the runner; a planted banned term made `tests/run style` fail and the restore passed again; `make test` boots 63 of 63 through the runner.

## Sequencing note

The `*-test` targets keep their recipes and print no retirement notice while the runner dispatches through them; their bodies move to `tests/system/suites/` in phase 6, and only then do the old names become notice-printing aliases. The registry's prerequisites field stays `-` until then.
