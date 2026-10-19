# The inventory of assertions

This is the coverage inventory of milestone 62, task T01 of [the plan](plan.md): every test the tree had at commit `d5d580c9`, before etude replaced `tests/run`, with what it asserts, how its assertions depend on the speed of the machine, and what becomes of each under etude. [The migration map](migration.md) lists the same units by the task that takes them.

An assertion is a check whose failure fails a test: a probe's `FAIL` line, a script's `fail` call, a host test's check, a harness step's expected text, or a proof's requirement that a fault be caught. A unit is the smallest thing a reader can find by name that has assertions: a suite, a row, a probe, a musl program, a host test, a script, a checker, a style check, a proof, or a probe's group of faults in `tests/system/probe_faults.txt`. The units' names are their identities until T05 gives cases and assertions identities of their own.

## The parts

* [The suites and their scripts](inventory-suites.md): the 101 suites of `tests/registry`; the rows of `tests/system/gnu_rows.sh`, of the agents suite and the two other row scripts; and every other script, program, checker and fixture under `tests/system/` that is not a proof.
* [The guest programs](inventory-guest.md): the 103 probes of `tests/Makefile`, the 82 programs of `tests/musl/`, and the eleven checks that `tests/system/runner/runtests.c` makes beyond a program's exit status.
* [The host tests, style checks and proofs](inventory-host.md): the 148 host tests and the models they share, the checkers' self-tests, the eight fuzz harnesses, the checks of `tests/run style`, the 116 `*_mutate.sh` proofs and the other scripts with a fault mode, and the 502 faults of `tests/system/probe_faults.txt` by probe.

## The entries

Each unit is one line:

    * `UNIT` (what runs it; architectures if not both): ASSERTIONS. Timing: TIMING. Fate: FATE.

The assertions are given by group, concretely enough that a reader can tell whether a later case keeps each one. Timing is `none` when no assertion depends on speed, or names each dependence with its file and line: a sleep that orders events, a fixed poll or spin count, a wall-clock bound used as an assertion, a comparison of guest time with host time, a forced accelerator, a race with the host's disk. A time limit that only ends a hung run is a completion bound and is not a dependence on speed.

The fate is one of three:

* `kept, TNN`: the purpose stays, in the task TNN that takes the unit: the conversion of its suite (T64 to T87), the host selection (T23), the proof driver (T26 and T27), or the release (T62).
* `kept, TNN; replaced in TMM: WHAT`: kept, but the named assertions are replaced by the repair task TMM with what WHAT says, a witnessed event, a count or a controlled state. `extended in TMM` marks a test that a repair task adds to.
* `removed in TNN: REASON`: the unit goes in T31, as a guest copy of a check that stays on the host, or in T89, as a test or helper of the old runner whose purpose the named etude tasks take over.

A fate may end with `missing: WHAT (TNN)`, coverage that the plan's new checks T28 to T30 add, recorded where it belongs.

## The counts

| Part | Section | Units | Kept | Kept, part replaced | Removed |
| --- | --- | ---: | ---: | ---: | ---: |
| Suites | The suites | 101 | 62 | 37 | 2 |
| Suites | The rows | 88 | 65 | 18 | 5 |
| Suites | The scripts | 102 | 74 | 10 | 18 |
| Guest | The probes | 103 | 70 | 32 | 1 |
| Guest | The musl programs | 82 | 69 | 13 | 0 |
| Guest | The default suite's own checks | 11 | 3 | 6 | 2 |
| Host | The host tests | 168 | 166 | 2 | 0 |
| Host | The checkers' self-tests | 8 | 6 | 0 | 2 |
| Host | The fuzz harnesses | 9 | 9 | 0 | 0 |
| Host | The style checks | 42 | 41 | 0 | 1 |
| Host | The proofs | 152 | 140 | 7 | 5 |
| Host | The probe fault catalogue | 60 | 60 | 0 | 0 |
| All | | 926 | 765 | 125 | 36 |

Every unit has a fate; none is left open. The faults found in the tests while the inventory was made are in [the migration map](migration.md#faults-found).
