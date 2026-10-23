# The old runner's programs and their conversion

T89 of [the plan](plan.md), #678, removed the old runner's programs that had no user left. Six stayed, since `tests/run`, proofs and cases still called them: the harness (`tests/system/runner/harness.c`), runtests (`runtests.c`), runutil (`runutil.c`), deadline (`deadline.c` with `tests/system/deadline.sh`), `tests/system/drive.sh`, and the tree lock (`tests/system/runlock.sh` and `runlock_exec.sh`). On 2026-10-22 the owner decided to convert their users to etude in this phase and then remove the programs, #875, on two conditions:

* Etude's core does not grow into a program that does everything. What the old programs did, where it is still needed, becomes small helper programs deployed with etude, each doing one job, as `etude-guest`, `etude-tooling` and `etude-bridge` are.
* Each helper builds and runs on Linux, on macOS and on native Windows, built with MinGW-w64, with no MSYS or WSL runtime.

This is the plan of that conversion: which helpers etude gains, which successor takes over each use of each old program, and the tasks that do it.

## The helpers

Etude gains three helpers. Each is `etude-NAME`, alike in layout and build to `etude-tooling` and to the volume helper of #889: its sources are under `tools/etude/` and listed in `sources.mk`, the top-level Makefile builds it into `build/tools/etude/`, `version.mk` builds it into a cached version's `bin/`, and `wincheck.sh` builds it for Windows and checks it with `pecheck`.

* `etude-deadline` runs one command as a process tree of its own (a process group on POSIX systems, a Job Object on Windows) under a bound, and ends the whole tree when the bound passes or a stop arrives. The bound is elapsed seconds, as deadline's, or, with `-i`, seconds without output or processor time, as runutil's `watch`; a bound of 0 is none, which is what tests/run needs for itself and its makes. #898.
* `etude-hold` holds a tree's run lock while one command runs: the record lock on `build/.run.guard` and the directory `build/.run.lock` with its owner record, unchanged on disk, so that etude-proof, etude-legacy, `tools/flow/test_status.py` and an older checkout read them as before. A command started under the hold of an ancestor runs under it. #899.
* `etude-ports` takes the host lock and the lowest free block of host ports, in the files and by the protocol that runutil and `etude-bridge` use, and holds them while one command runs. The protocol's code moves out of `tools/etude/posix/bridge.c` into one source that the bridge and the helper both link. #900.

The harness, runtests and drive.sh get no successor. Their users are proofs, and the scripts the proofs run, that boot machines for probes and rows that etude's cases already run, through `etude-guest` and `libetude-case`. Those proofs move to the cases, and `tests/run` gains a choice of cases, `RACH2_CASES`, so that a proof runs only the cases its faults reach (#901).

## Each use and its successor

| Program | Use | Users | Successor | Task |
| --- | --- | --- | --- | --- |
| harness | boot a probe image and judge its console | 13 catalogues of guest probes, with `guest.sh boot`, `xsave.sh`, `rng.sh` and `ptrace.sh` | the probes' cases, through `guest.sh run` | #905 |
| harness | drive a machine for an old suite's script | 8 catalogues, with `probe_roots.sh`, `ext2_cost_rows.sh`, `usbdisk_hold.sh`, `usb_latency.sh`, `machine_grow_space.sh` and `sb16.sh` | the machine cases, through `guest.sh run` | #906 |
| harness | a target the build names but nothing runs | `fdlock`, `install-keys` and `recovery-disk-key` | none | #905 |
| runtests | boot the test image and run probes | `default_probe.sh`, which 63 catalogues run: `rows-default` and the `probe-*` catalogues but `probe-roots` | the cases of `guest.default` | #904 |
| runtests | the list `PROBES` | `tools/verify/probecheck.sh` | `tests/cases/guest.default.json` | #904 |
| runtests | its self-test of the judge in `tests/clockref.h` | `clock-scorer` | a host test of that judge | #904 |
| runutil `lock`, `young` | the tree lock | tests/run, `runlock_exec.sh` | etude-hold | #899, #903 |
| runutil `group`, `lead`, `killgroup` | tests/run's process groups and its makes | tests/run | etude-deadline with no bound | #903 |
| runutil `pgid`, `identity` | the lock owner's liveness, the status record | tests/run, `test_status_test.py` | etude-hold's owner record | #899, #903 |
| runutil `ports` | the host lock and a port block | tests/run, `ci_test.sh`, `test_status_test.py`, `etude_launch_test.py` | etude-ports | #900, #903 |
| runutil `sleep` | the nap of tests/run's watcher of the scripts | tests/run | sleep(1) | #903 |
| runutil `now` | the clock of drive.sh | drive.sh | none | #906, #875 |
| runutil `watch` | stop a command that does nothing | `tools/dev/poll_kernels.sh`, `accept/su_command.sh` | etude-deadline `-i` | #898 |
| deadline | a time limit on a command | three mutation scripts of `tests/host`, `host_suite_test.sh` | etude-deadline | #898 |
| deadline.sh | the factor of a script's bounds | `agent_fakeapi.sh`, `agent_rows.sh`, `accept/agents_accept.sh` | the bounds of the cases' steps | #907 |
| drive.sh | type at a console when it is ready | `machine_grow_space.sh` | `machine.grow.space` | #906 |
| runlock.sh, runlock_exec.sh | a script's hold of the tree | `tools/etude/proof.sh`, `legacy.sh`, `tests/cases/host/host.sh` | etude-hold | #899 |
| runlock.sh, runlock_exec.sh | a script's hold of the tree | 32 other scripts | etude-hold | #902 |

The old programs' own tests go with them: the cases of `tests/cases/host.runner.json` but the reader's, which #900 moves to a family that stays; `host.drive.json` with `drivetest.sh`; `runutil_cpu.sh`, `runutil_group.sh` and its fixture; `harness_input_mutate.sh`; the catalogue `krffs-inode-harness`; and the style suite's self-tests and the host suite's sanitizer builds of runtests and the harness.

## Order

The three helpers and the choice of cases start at once. The scripts move to etude-hold (#902) after it is in, and tests/run moves to all three (#903) after they are in. The proofs (#904, #905, #906) wait only for the choice of cases, and the agents' bounds (#907) for nothing. The Mac, which only the owner's accounts reach, checks the helpers and tests/run on them (#908). #875 removes the programs and their tests last, when `git grep` finds no other user.

## What stays

`hostpeer`, `ptydrive` and `screentext` stay in `tests/system/runner/`, since etude's cases run them. The kernel built with `HARNESS=1` stays, since `machine.exitline.harness` boots it. Bisection runs an older commit's `tests/run`, which compiles that commit's runutil; since the helpers keep the files and protocols of the lock and the ports, an old run and a new one keep each other out of the tree and the ports as before.

## Outcome

The removal, #875, followed this plan except where the owner's decision of 2026-10-23 changed it: no new program runs a command under a fixed number of seconds, and a test is no longer proven by a mutation. So `etude-deadline` was not built (#898), and the proofs that #904, #905 and #906 were to move were deleted with their driver instead. What each use became:

* runutil `watch`: `accept/su_command.sh` runs its session under its caller's watchdog alone, and `tools/dev/poll_kernels.sh` runs its builds with no idle limit.
* deadline and deadline.sh: the scripts that used them wait for their events with no bound (#907 and the commits before it).
* runtests: its list `PROBES` became `tests/default-probes`, which `probecheck.sh` and the list of the selfbuild cases read, and the self-test of the judge in `tests/clockref.h` became the host test `clockref_hosttest.c`.
* The harness and drive.sh: `probe_roots.sh`, whose callers had gone, went with them. Three scripts that a person ran by hand still drove the harness and were not in this plan: `grow_machine.sh` and `machine_grow_space.sh` became cases of the `grow` family (#934), and `rehearse_sitting.sh` a case of `installer-rehearse` (#935).
* `tools/verify/suitecheck.sh` lost its rules for the harness's `wait=` and `send=` steps and drive.sh's typed steps.

`tests/system/fixtures/old_ports.py` still takes the host lock and a port block as an older checkout's runutil does, since bisection runs older commits beside new ones.
