# Milestone 34 notes

Deviations from `plan.md`, measurements, defects found and what was done about
them, and every decision the plan left to the run.

## Phase 1

Tree clean at `aee2cd6`, no stray machines, build current. Recent history:

```text
aee2cd6 Recast two sentences in the milestone 34 plan
f1f85e8 Rewrite the milestone 34 plan around the review
e66831c Plan milestone 34, the messaging and the tests
48431a7 Write down what the suite review found
2396530 Record the fast-suite milestone
deb2d50 Wait for the console, schedule the long suites first, and stop the credential race
bce5b2c Set the worker count from the processors, measured
9579f5c Run the second card's suites beside the first card's
c090f9c Run the suites through a pool and name each machine's owner
0d040f9 Stop the suites writing each other's images and ports
0b4d0f4 Open the milestone 33 record
115e793 Rewrite the fast-suite plan around what the review found
```

Baseline adopted rather than re-measured: milestone 33's warm run, 465s across
44 suites, recorded in `dev-log/33-fast/notes.md`. The only commits since are
this milestone's plan and findings, which change no code.

## Phase 2

* My first splitter treated a comment line inside a recipe as the end of the
  block. Every recipe with an embedded comment was truncated: the tail stayed
  in the root `Makefile` and attached itself to whatever target preceded it,
  and `run-e1000` was left with prerequisites and no body at all. A target
  with no recipe is legal Make, so `make -n run-e1000` still succeeded; only
  reading the file found it. The split was redone with a rule that treats a
  comment as part of a recipe when further recipe lines follow, and checked by
  requiring every `run-` target to have a body and by reconciling the line
  counts: 756 plus 1056 equals the original 1789 plus the 23-line header.

* `selfbuild` failed in the first full run after the split, and the cause
  predates this milestone. The suite waits for `HASHONE=` and then reads
  `SIZEONE` from the same console line, but the size is printed after the hash
  on that line, so the value is read while the machine is still typing it. It
  returned 3979 for a kernel of 397980 bytes. Under a twenty-worker pool the
  guest's output is slow enough to widen that window, which is why milestone
  33 saw the same suite fail at 51 seconds with an empty size and why the
  cause was then misread as the credential race. The guest now prints a marker
  after the value and the suite waits for that marker. Proved twice: a kernel
  reported as 1234 bytes is still rejected, and with the old wait restored a
  guest that pauses mid-number reproduces the truncation.

## The runner self-test, 2026-08-30

`tests/system/selftest.sh` drives `tests/run` against a registry and a makefile
of its own in a temporary directory: a passing suite, a failing one, one that
needs a command that does not exist, one that leaks a machine, an unknown name,
a tier, and two bad worker counts. Eighteen checks, about two seconds, no
machine booted and nothing read from the product. `tests/run runner` runs it,
and `sh tests/system/selftest.sh` runs it on a tree that does not build.

Proved by six mutations of `tests/run`, each restored from a trap: skipping the
tool check (3 checks fail), dropping the machine check (2), inverting the tier
filter (1), accepting an unknown name (2), turning a suite failure into a pass
(4), and dropping the worker-count validation (2).

The first attempt at the mutation run reported two mutations caught by nothing.
They were not: `tests/run` checked the name and the tools twice, once before
the build and once inside `run_one`, so breaking one copy left the other. The
duplicate is now gone. Every name and tool is checked once, before `prepare`.

Open: `prepare-all` rebuilds `build/images/rach2-sealed.disk` on every run,
about 25 seconds, whether or not anything it stages has changed. It is a
product rule rather than a test rule, so it stays out of this phase; recorded
here for phase 8.
