# Milestone 30 checkpoint

This file holds only the resume state for the item in progress. Replace its contents instead of appending.

## Position

* Active phase: 4, shared source manifests.
* Item in progress: kernel manifest done: `rach2/sources.mk` included by both builds, `guest.mk` derives `OBJS` and writes the response file from it in one builtin printf (1587 bytes expanded, budget 2572), `check_guest.py` learned includes and substitution references. Proofs: host kernel hash unchanged (148594d07c3b105210a0), planted nonexistent source fails both descriptions by name, header edit rebuilds 23 of 83 objects, self-build hash equals the milestone 29 record (fba1c906...). Next: the utils manifest, then ISH.
* Exact next action: read `utils/Makefile` and `utils/guest.mk`, move the program map and companion sources into a shared file, and prove the same way.
* Phase 3 completed at fa0436e: interface is make, make test, tests/run NAME, clean, distclean, run.sh; retired names print notices; preflight cached; FUSE optional; compile-commands alias retires in phase 6 with the script move.

## Phase record

* Phase 1 complete at e4d3187. Baselines: cold `make distclean && make full` 60 s; no-op `make full` 3 s; style 23 s; host 37 s; `make test` 96 s.
* Phase 2 complete at 6ef65d2: compilation database generator fixed, selftest in check 12, both restored defects failed the selftest.
* Phase 3 in progress: FUSE split at 131bd4a (default KRFFS build needs neither FUSE nor pkg-config; `make -C krffs mount` is the optional tool). Runner landed: `tests/run` reads `tests/registry` (43 suites, five fields), `make test` delegates to `tests/run default`, `verify-style` and `verify-host` are retirement aliases for `tests/run style` and `tests/run host`, and 26 current documents name the new commands. Proofs: unknown suite exits 2 with the valid list; style and host pass through the runner; a planted banned term made `tests/run style` fail and the restore passed again; `make test` boots 63 of 63 through the runner.

## Sequencing note

The `*-test` targets keep their recipes and print no retirement notice while the runner dispatches through them; their bodies move to `tests/system/suites/` in phase 6, and only then do the old names become notice-printing aliases. The registry's prerequisites field stays `-` until then.
