# Milestone 30 checkpoint

This file holds only the resume state for the item in progress. Replace its contents instead of appending.

## Position

* Active phase: 4, shared source manifests.
* Active phase: 5, incremental correctness. Items 7 (incremental musl test builds, f946e0a) and 8 (credential timestamps, cec40ce) are committed. Item 1 is proved and ready to commit: the three RAM images are real file targets, the stager records every input (absent optional files included) into an image-side `.d`, absent inputs are represented by their nearest existing ancestor directory, and recorded paths are absolutized because a relative `rach2` names a phony target. Proof record: two no-ops restage nothing, an os-simple manifest touch restages only os-simple, a probe source touch restages only test, the Doom WAD's appearance and removal each restage only os-real, and the final no-op restages nothing.
* Exact next action: commit item 1, then item 2 (vendor patch manifests) and the remaining phase 5 items in the plan's order.
* Phase 3 completed at fa0436e: interface is make, make test, tests/run NAME, clean, distclean, run.sh; retired names print notices; preflight cached; FUSE optional; compile-commands alias retires in phase 6 with the script move.

## Phase record

* Phase 1 complete at e4d3187. Baselines: cold `make distclean && make full` 60 s; no-op `make full` 3 s; style 23 s; host 37 s; `make test` 96 s.
* Phase 2 complete at 6ef65d2: compilation database generator fixed, selftest in check 12, both restored defects failed the selftest.
* Phase 3 in progress: FUSE split at 131bd4a (default KRFFS build needs neither FUSE nor pkg-config; `make -C krffs mount` is the optional tool). Runner landed: `tests/run` reads `tests/registry` (43 suites, five fields), `make test` delegates to `tests/run default`, `verify-style` and `verify-host` are retirement aliases for `tests/run style` and `tests/run host`, and 26 current documents name the new commands. Proofs: unknown suite exits 2 with the valid list; style and host pass through the runner; a planted banned term made `tests/run style` fail and the restore passed again; `make test` boots 63 of 63 through the runner.

## Sequencing note

The `*-test` targets keep their recipes and print no retirement notice while the runner dispatches through them; their bodies move to `tests/system/suites/` in phase 6, and only then do the old names become notice-printing aliases. The registry's prerequisites field stays `-` until then.
