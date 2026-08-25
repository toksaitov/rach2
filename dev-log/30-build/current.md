# Milestone 30 checkpoint

This file holds only the resume state for the item in progress. Replace its contents instead of appending.

## Position

* Active phase: 2, the compilation database.
* Item in progress: reading `tools/gen_compile_commands.sh` to locate the wrapped-recipe selection and tab-escaping defects.
* Exact next action: fix the generator, add its self-test with the wrapped Homebrew command, prove the self-test by restoring the defect, wire the self-test into the style checks, and rewrite `mk/Readme.md` and `tools/Readme.md` in the same commit.

## Phase 1 baselines, recorded 2026-08-26 at commit f234cf9

* `make doctor`: pass, all required tools present.
* Green run, every command exit zero: `make full` (warm, 3 s), `make verify-style` (23 s, 13 of 13), `make verify-host` (37 s, 36 tests), `make test` (96 s, 63 of 63 probes and the clock).
* Cold baseline `make distclean && make full`: 60 s, with every vendor verified, extracted, patched, and rebuilt.
* No-op baseline `make full`: 3 s (the phony image targets restage on every run; phase 5 item 1 addresses this).
