# Milestone 62: a test system that is fast, portable and judged by what happens

## Goal

The tests check what the system does, finish in minutes, and run on Linux, macOS and Windows hosts. One runner, `rtest`, written in C, replaces `tests/run`, the runner suite and the shell wrappers. A test runs where the code it tests runs: guest behavior is checked inside the guest, and the host boots machines and collects results. Each host says in a configuration file which environments it offers and how much of its processors, memory, disk space and disk speed the tests may use, and the runner plans its work within those limits. Time decides only when to stop waiting, never whether a test passed.

## What is wrong now

Line references are to `master` at `53b165d3`.

* The tests are larger than the kernel: about 240,000 lines of C and shell against the kernel's 170,000. `tests/host` alone is 112,000 lines.
* A system suite is a host shell script that types commands into the guest's serial line and searches the output (`tests/system/suites/doom.sh:44-47`). That ties the suites to a Linux-like host and puts a bound in seconds on every step.
* The bounds are seconds measured on one host and scaled (`tests/system/runner/runutil.c:108`, and again in `tests/system/suites/agents.sh:179`), 2,331 timing expressions in all. Under nested KVM the ext2 zombie row fails at 45 s against a bound of 40 s; under TCG the GNU autotools row takes 1,400 to 1,700 s for work that KVM does in 50 (`dev-log/58-team/Readme.md:23-24`). The set passes under neither accelerator for that reason alone.
* The test machinery tests itself at length: the runner suite takes 313 s of every ci run (`dev-log/58-team/Readme.md:66`), 102 mutation scripts and some 35 other scripts edit sources with sed, and the agents suite runs four loop tests in the guest that already run as host tests.
* Some suites duplicate others or have no oracle: diskrate's timings, the guest manual sweep beside the host's `mancheck`, `selfbuild-ext2`, the eight `-e1000` copies, Doom's survival sleep.
* The release runs 12 suites at a time, i686 and then x86-64, on a host with 32 processor threads. The last full run took 3 h 39 min, and the agents suite alone 6,573 s of it. In September the set ran in 18 minutes with 64 at a time (`dev-log/53-audit/notes.md:60`), with fewer and older suites.
* Nothing measures what a test costs in processors, memory or disk, so the runner cannot plan by it, and nothing refuses a new slow test.

## The design

### Principles

* A test runs where what it tests runs. A guest test runs inside the guest and writes its own result; the host checks only what the host alone can see.
* A test judges what happened. Running out of time gives the result incomplete, which is never a pass and is judged by its evidence.
* What a test costs is measured on every run, recorded, and limited.
* One runner program, in C99 that TinyCC builds, for Linux, macOS, Windows and Rach2.
* Guest tests share a boot unless a test says it cannot, and the runner checks what each one leaves behind.
* A test file carries its own description, so there is no registry to keep in step.

### The test file

A test file, C or shell, has a block of `key: value` comment lines that begins at `test:`. The checker refuses an unknown key, a missing required key, a duplicate id, a glob that matches no file, and a name of a profile or fault that does not exist.

* `test`: the stable id, such as `lib/kstring`.
* `checks`: one sentence, the contract the test checks.
* `runs`: `host` (a native program), `guest` (inside a booted Rach2), `init` (as the debug shell's first process, several in one boot), `machine` (a C program that drives machines: reboots, power loss, the installer) or `native` (on Rach2 itself, without QEMU).
* `needs`: what the test requires: `arch=`, `host=`, `accel=`, `profile=` (a QEMU machine and firmware profile under `tests/profiles`), `image=`, `memory=`, `cpus=`, `disk=` (MiB of scratch), `io=heavy`, `net`, `gnu`, `tool=`.
* `matrix`: a parameter whose every value is run, such as `nic=ne2k e1000`.
* `state`: `shared` (the default), `dirty` (leaves state on purpose and runs last in its boot) or `alone` (a fresh kernel and disks).
* `deps`: source globs that select the test for a push check.
* `cost`: `small`, `medium`, `large`, or `exception` with an entry in `tests/exceptions.list`.
* `tier`: `push`, `ci` or `release`.
* `fault`: the entry in `tests/faults.list` that proves the test fails when the code is wrong.

Doom's recorded demo, from `tests/system/suites/doom.sh`, becomes:

```sh
#!/bin/sh
# test: apps/doom-record-mode
# checks: a demo recorded through Doom's fopen honours the umask
# runs: guest
# needs: arch=any image=doom
# deps: ports/doom/** musl/** rach2/sys/syscall_fs.c
# cost: medium
# tier: release
# fault: doom-fopen-mode
. /test/lib.sh
umask 022
doom -record "$T/rec" -maxdemo 1 > "$T/doom.log" 2>&1
t_equal engine-exit 255 $?
t_equal recorded-mode 644 "$(stat -c %a "$T/rec.lmp")"
```

### Building and running are separate

Images are built where the build works: the cross toolchain on Linux and macOS, and the GNU root on x86-64 Linux only. ci publishes each commit's images and kernels as a bundle with a manifest of their SHA-256, and `rtest` runs tests from a tree's build or from a bundle, so a host that cannot build can still test.

### Hosts and their limits

Each host has a configuration file, `tests/hosts/NAME.conf`, committed for the project's hosts (the testing PC, its ci runners, the Mac mini) and chosen by `RTEST_HOST` or the host's name. A person may override it with `~/.config/rach2/rtest.conf`. It uses the same `key: value` lines:

```
host: testing
side: wsl
accel: kvm tcg
profiles: pc-bios pc-uefi q35-uefi
cpus: 28
memory: 96
disk-free: 40
scratch: /home/testing/rtest
io: 400
share: default 8 16
share: rach2-ci-1 12 24
share: rach2-ci-2 12 24
```

* `side`: where the tests run on that host: on the testing PC, all in WSL or all on native Windows, never both, since a lock on one side cannot be seen from the other.
* `accel` and `profiles`: the environments the host offers, in order of preference.
* `cpus` and `memory`: the processor threads and GiB that all tests on the host may use together.
* `disk-free`: the GiB that must stay free on the scratch disk; below it the runner starts nothing new, as `tests/run` refuses below 40 GiB today.
* `io`: the MB/s of disk writes the runner may plan for.
* `share`: an account's part of `cpus` and `memory`, as each account's `.bashrc` gives today (8 workers and 16 GiB each, 12 and 24 for each ci runner).

`rtest probe` measures what the host has: its processors, memory, free disk, the QEMU binaries and their version, which accelerators open, the disk's rate for one large write and for small synchronous writes in `scratch`, and the tools on the PATH. It boots each configured profile once, and a profile that fails to boot is reported and not offered. The runner uses the smaller of each configured and measured value, and keeps the measurement, with its date, until it is a week old or the file changes. A test whose needs nothing meets is unsupported, with the missing need named; an unsupported test of the release tier blocks the release.

The runner records, for every test on every run, its processor seconds, the peak resident memory of its QEMU process, the bytes its disks wrote (QMP's `query-blockstats`) and its seconds. It starts a test only while the running tests' processors, memory and disk writes, by their recorded medians, stay within the host's limits and the account's share, and the free disk stays above `disk-free` plus the test's declared scratch. A test without history counts as its declared `memory=`, `cpus=` and `disk=` and its cost class's defaults.

### The runner

`rtest` has these parts: the catalog reader and checker; the probe and the host configuration; a planner that packs guest and init tests into boots by architecture, profile and image, dirty tests last and alone tests in boots of their own, and boots onto the host's resources; a supervisor, with process groups on POSIX and a Job Object per test on Windows, so that a dead runner takes its machines with it; QMP and the serial line over loopback TCP, which QEMU offers on every host; and the result writer. `rtest-guest`, a small program in the test image, and a library for shell tests run the guest side. The estimate is 3,500 to 5,000 lines with its tests.

The result protocol is fixed before any code: one record per line, `case`, `assert`, `progress` and `end`, each with the run, boot and test ids. A test is finished only by its `end` record; a boot is complete only when every planned test has one; a duplicate, malformed or unplanned record fails the boot. `rtest-guest` writes the records to the serial line and to the test disk, and the runner checks both.

`rtest-guest` reads the kernel's counters before and after each test (free frames, processes, open files, mounts) and fails a test that left a difference it did not declare. When a test fails in a shared boot, the runner runs it again alone in a fresh boot and reports both results, so a fault that depends on the order of tests is not hidden by a clean second run.

Several runs share a host through a directory of lock files, one per slot, taken with `flock` or `LockFileEx`, which the system releases when the holder dies. A build takes slots too. A `drain` file stops new acquisitions, so that the release can take the host and ci can go before the workers.

### Time

* A test body has no time in it. The checker refuses sleep, timeout and deadline calls in bodies, except a declared use whose meaning is time, such as a timer interval under test. A wait for readiness becomes a wait for an event.
* A test may write `progress` records; a long build writes one per step.
* The watchdog stops a test when neither a `progress` nor an `end` record has come within its allowance: five times its own median on this host and profile, or, without history, five times its class's reference cost scaled by the host's speed. The host's speed is the median time to a boot's first record on this host against the reference profile, measured from every boot.
* An incomplete result keeps the last records, the console's last lines, the QMP status and registers, the disks, and the command that repeats the boot. The test is run again alone, and both results are kept. An incomplete result blocks acceptance.
* A test may check what the kernel computes about time by the kernel's own clock and counters. The clock suite keeps an outside clock as its oracle.

### Budgets

Cost classes are seconds on the reference profile, the ci host with its ci accelerator, recorded in `tests/profiles/reference` with the date of the measurement:

* `small`: a host test under 1 s, or a guest test under 2 s of its boot.
* `medium`: a guest test under 30 s.
* `large`: a test with a boot of its own, under 180 s with the boot.
* `exception`: `tests/exceptions.list` gives the test, the reason and its allowance. A new entry is the owner's decision.

A test whose median over its last three ci runs is more than twice its class fails ci for its cost, apart from its result. Each tier has a total, per architecture, set from the first measurement; families of tests are totalled too, so splitting a test does not get past it.

### What runs when

* A push check: the build, style of the changed files, and the host and guest tests whose `deps` match, both architectures at once. Target: 5 minutes.
* ci: every test of the ci tier on both architectures. Target: 10 minutes.
* The release: every test, both architectures, the testing PC drained, then the Lenovo. Target: 60 minutes, which holds only with a hardware accelerator for the heavy tests (gnu, agents, selfbuild).

These targets are checked against the measurements below and published with the time split into waiting, builds, fixtures, tests and cleanup. While the conversion runs, ci runs both runners, so it is slower before it is faster.

### Rules for the agents, enforced by the checker and ci

* A new test file has every key, a fault entry, and its proof: it passes; the fault makes it fail at an assertion, or, for a fault that hangs or crashes, end incomplete or failed; with the fault removed it passes again.
* A new shell test body is at most 60 lines.
* A commit that removes a test, loosens an assertion or raises a cost class says so in its message, and the reviewer reads every such commit.
* The first line of a failure gives the test id, the assertion, what was expected, what was seen, and the command that runs it again.
* The day report counts tests and reference seconds per tier and their change.

## Measure first

On the testing PC under TCG and KVM in WSL and under WHPX with QEMU for Windows, and on the Mac mini under TCG, on one pinned commit:

* The time to a boot's first record for each architecture and profile.
* The number of tests at once at which the host stops getting faster, with today's suites, and the processors, memory and disk writes of each suite.
* The GNU autotools row, the agents suite and the sealed suite's fault run.
* The disk rate under nested KVM and under WHPX, now that the IDE driver uses DMA.

These set the reference profile, the release's parallelism, the accelerator of each tier, the first host files, and the owner's first decision below.

## What is deleted, and when

At once, since each duplicates a check that stays: the agents suite's guest copies of the four loop tests, `selfbuild-ext2`, Doom's survival sleep, and the runner suite in every ci run, which moves to the push check of changes to the runner.

With the conversion of their family, once the replacement runs: diskrate, the guest manual sweep (its image checks move to the host), the eight `-e1000` copies (a matrix of the network tests), the mutation scripts once `tests/faults.list` holds their faults, and `tests/run`, the registry, the runner suite and `drive`. The twelve installer suites become eight scenarios that share fixtures.

## The owner's decisions

Each is asked on the board as an issue labelled `owner`, with a recommendation.

* Where the testing PC runs its tests: all in WSL with KVM, all in WSL with TCG, or all on native Windows with WHPX, which needs the accounts to reach Windows directly. Recommended: decide after the measurements.
* The release drains the testing PC for its hour, and ci goes before the workers. Recommended.
* The exceptions list: selfbuild, gnu, agents and the installer scenarios. Recommended.
* The deletions and their timing as listed. Recommended.

## The phases

Three phases, each released on its own, so the first gains arrive before the runner exists. A task names the tasks it waits for; the others start at once.

### Phase 1: faster releases now

1. Take the measurements on one pinned commit and record them in this milestone's `notes.md`.
2. Delete the duplicates listed under "At once".
3. Give the release the measured parallelism, and the accelerator the measurements support for the heavy suites. Waits for 1.

### Phase 2: the runner

4. Write the test file grammar, the host file grammar, the result protocol and `rtest`'s internal interfaces, with the checker, `tests/faults.list` and its fault tool.
5. Write the catalog, the probe with the host files and the disk measurement, the planner by processors, memory and disk writes, the result writer and the host executor on POSIX. Waits for 4.
6. Write the Windows supervisor and the TCP transport. Waits for 4.
7. Write `rtest-guest`, the guest library, the check of what a test leaves behind, and the guest and init executors. Waits for 5.
8. Write the slot locks, the account shares, `drain` and the port blocks. Waits for 5.
9. Write the machine executor and its C interface. Waits for 7.
10. Write the watchdog, incomplete results, the recording of cost and resources, and the cost check. Waits for 7.
11. Prove the first slice on Linux, macOS and native Windows: the host tests, the default probes in one init boot, one guest shell test; a hang put in on purpose gives incomplete with the QMP state, and a fault put in on purpose fails at its assertion. Waits for 6, 8, 9 and 10.
12. Run `rtest` in ci and in the push check beside `tests/run`, and publish each commit's images from ci as a bundle with their SHA-256. Waits for 11.

### Phase 3: the conversion

Each task moves a family of tests and deletes its old suites in the same commit, so a test is in the old registry or the new catalog, never both.

13. The host tests, the checkers, the mutation regressions, fuzz replay and style, as host tests.
14. The raw and musl probes.
15. Block devices, durability and file systems, with diskrate's counted checks.
16. The network, with the card as a matrix.
17. The console, USB, audio and input, with a PS/2 mouse test.
18. The clock, and the rest of the timing allowlist. Waits for 14.
19. The installer, as eight scenarios. Waits for 15.
20. Accounts, services and manuals.
21. The native build, GNU, Doom and agents. Waits for 15.
22. The Lenovo's acceptance as machine tests in `rtest`'s results. Waits for 19.
23. The missing tests: `preadv`, `pwritev`, `pselect6`'s signal mask, a ChaCha known answer, an NVMe boot.
24. Move the callers to `rtest`: `tools/flow/session.pl` and `status.sh`, `tools/ci/run.sh` and `bisect.sh`, the three workflows and the ci issue titles; rewrite the check rules of `AGENTS.md` and the testing skill. Waits for 13 to 22.
25. Remove `tests/run`, the registry, the runner suite and `drive`; the day report counts tests and reference seconds. Waits for 24.
