# Milestone 30 checkpoint

This file holds only the resume state for the item in progress. Replace its contents instead of appending.

## Position

* Active phase: 7, small dependency removals.
* Item in progress: not started.
* Exact next action: replace `gtimeout` with a C process-group deadline tool and prove it with a hanging child and a hanging descendant; then move the SSH password and pty exchange into the retained Python runner and remove Expect; then fold the Perl, host SHA-256, and `strings` uses into the project checkers with differential fixtures. The FUSE split already landed in phase 3 (131bd4a).

## Phase record

* Phases 1 to 5 complete; see the log from e4d3187 through 710b9fe.
* Phase 6 complete: setup c2d2eac, dev df90c21, verify 23724b2, inspect e9af87b, image 3fba431, tests/system 2fd741f, suite-name retirement c2bca4f. Every mover's repository-root computation gained one level; extensionless callers such as `tests/run` need sweeps without include filters; path rewrites widen comment lines, so `check_comments` runs after every role. Proofs: all four setup tools, the generator, and every checker re-proved from their new homes; 17 dynamic dispatch proofs (temporary exit 97 per moved suite reached through `tests/run`); the four Docker-disk suites statically wired; the SSH capstone passed through the moved suite in 29 s; strict shellcheck covers the moved suites again.
