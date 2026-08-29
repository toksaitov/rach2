# Milestone 33: a fast suite

## The goal

Bring `tests/run all` from 81 minutes to under 20 without weakening a single check. At the end, a developer runs the whole suite while making a cup of tea instead of while making other plans, and every check that could fail before can still fail, for the same reason, on the same input.

This plan is the authority for the run. A conflict between this plan and the current tree is resolved by the decisions here; a conflict between this plan and `CLAUDE.md` is resolved by `CLAUDE.md`.

## Why

The suite is not slow. It is asleep.

A full boot to the kernel debug shell takes 4.76 seconds and one fifth of one core. The machine has ten cores and 24 GB, and one emulated machine costs 71 MB. During a complete run, one QEMU process sits at about three percent of one processor and the other nine cores do nothing.

The time goes into waiting for things that have already happened. The harness step `wait=SECS` is `pump(now() + SECS)`, an unconditional drain that never looks at whether the success pattern has arrived; the `--ok` test happens once, after every step. The root `Makefile` has 45 such steps totalling 988 seconds, and the suite scripts have 112 `sleep` calls totalling 1384 seconds. Measured on one suite: `tests/run ping` sees `PINGTEST OK` at 11 seconds and exits at 66. Fifty-five seconds of nothing, in a suite whose whole job took eleven.

The second cost is that all 44 suites run one after another. They need not. Two measurements settle it: six network suites take 462 seconds in series and 143 in parallel, and sixteen mixed suites take 760 in series and 98 in parallel. In both, the parallel time is the slowest member's, which is what independence looks like.

## The decided end state

### The two rules about waiting

These are mechanical and they are the whole of the first phase's judgement:

* `wait=SECS` means timed observation. It never consults `--ok`. It stays wherever the interval itself is the assertion, or wherever work follows the token.
* `expect=TOKEN` means positive completion. The harness already returns the moment the pattern matches (`expect_pattern` in `tests/system/runner/harness.c`), so this needs no new machinery and no change to the harness. It replaces a trailing `wait=` only where the token is the last required event and no host checker needs output produced after it.

The failure allowance that the old `wait=` provided is preserved by `--steptmo`, so a token that never arrives still fails in the same time it used to.

The two steps do not match alike, and a conversion that ignores this weakens what it converts. `--ok` is a literal substring test (`contains`), while `expect=` compiles a regular expression, and the matcher refuses a pattern containing a parenthesis outright. Six success tokens contain parentheses. Three of them are `cred-persist`'s and stay for their own reason; two more sit on invocations with no `wait=` to convert; the sixth, `PASSWDMUSL OK (fail-closed)`, keeps its wait because no shorter substring of it is both metacharacter-free and unambiguous. Elsewhere a token's dots stop being literal, which loosens the match without breaking it, and that is accepted. No conversion invents a pattern: it uses a substring of the token the invocation already declares.

`pump` is not changed. A global early exit on `--ok` was considered and rejected: `expect_pattern` calls `pump` to read later output, so a check there would stop a recipe whose `--ok` token appears early from ever receiving the input its later steps need.

### The waits that stay

Each of these is a case where the token is not the end of the work. They keep their timed `wait=` and the plan names them so that a later reader does not mistake them for oversights:

* `cred-persist`, first boot: `CREDDISK OK (wrote)` is printed and flushed before `_exit(0)`. The drain is what proves the program took that path.
* `dhcp`, the two lease boots: the token is followed by `poweroff -f`, and the wait is the machine stopping.
* `dhcp`, the unattended boot: the lease line is the `--ok`, and init and the SSH server come after it.
* `dhcp`, the static-address boot and the renewal boot: a capture checker reads packets sent after the token, and the renewal case asserts that exactly one address request appears, which is an absence assertion over the interval.
* `netconsole`, the shutdown boot: `--ok` matches the goodbye line and the interval exists to see whether `powering off` also reaches the wire, which `pcapcheck --reject` then denies.
* `bootline`: the suite waits after a refusal diagnostic and then rejects any debug shell or login prompt. The interval is the assertion.

A conversion that cannot be shown to be outside this set is not made.

### The pacing in the shell suites

Four suites pace a console feed with fixed sleeps rather than waiting for the machine: `account` (twelve seconds before the first line and six between each), `tcc` (the same shape), `procfiles` (sixteen sleeps between commands that already print status markers), and `ssh` (twenty-five seconds before a connection that could be retried until it answers). They are converted to wait on the marker each step already produces. Seven suites already have such a helper under the names `wait_serial`, `wait_token`, or `wait_count`; the conversions use the one their file already has rather than introducing another.

The long sleep that holds a FIFO open is not a delay and is not converted. `account`'s 240-second tail is one of those: it keeps the console writable while the test polls, and the suite kills the machine when its condition is met.

### The e1000 suite

`tests/run e1000` re-runs nine complete suites on the second card and then makes three boots of its own. At 914 seconds it is the most expensive suite in the tree, and most of that is a second copy of work the first card already proved.

It keeps ping, packet, dgram, netconsole, and ssh, plus the three card-selection boots. It drops the second-card repeats of default, netif, raw, and dhcp. What remains covers card selection, ordinary receive and transmit, unsolicited input, broadcast, the shutdown path, and one complete TCP session; the descriptor contract, ring wrap, transmit stalls, stop, and link reporting are covered by the host model, which does not need a boot.

This trade is coverage for time and it is the only item in this plan that is. It is accepted only if four mutations still fail the retained set: one in the e1000 receive path, one in its transmit path, one in its stop, and one in the driver-table selection. If any survives, the dropped suite that would have caught it goes back.

### Preparation and execution

Every `run-NAME` target today carries its build prerequisites and creates its own fixtures, so running two of them at once runs two builds against one `build/` tree. The end state separates the two:

* `prepare-NAME` builds everything the suite needs: components, tools, images, kernels, and fresh fixtures. It is the only thing that writes under `build/`.
* `run-NAME` boots machines and reads results. It has no build prerequisites and creates no artifact.
* `tests/registry` gains the preparation target in its third field, which has held `-` for every suite since the field was introduced.
* `tests/run NAME` runs the suite's preparation and then its run. `tests/run all` collects every preparation target into one `make -j` invocation, so GNU Make sees one dependency graph and no two component sub-makes overlap, and then runs the suites through a bounded pool.

### The collisions

Found by running suites together and reading what broke, not by reading alone:

* `run-disk-ahci` and `run-disk-q35` both create and boot `build/images/disk-test-ahci.disk`. Under parallelism one renames it into place while the other is checking it, which is the failure `check-setid: FAIL: disk-test-ahci.disk.new: No such file or directory` reports. The q35 suite gets its own path.
* `run-dhcp` restages `rach2-metal.krffs`, which `diskdiag` and `metal` also boot, and `run-account` restages `os-simple.krffs`, which `dhcp` also boots. Both moves go into preparation.
* Two host ports are used by two suites each: 2224 by `dhcp` and `diskdiag`, and 2298 by `procfiles` and `drill`. The second of each pair is given its own port through a Makefile variable. A dynamic port broker is not built; the suite list is fixed and short.
* `style` and `host` both use `/tmp/rach2_verify`, and `style` invokes a kernel build directly. They run outside the parallel pool.
* `e1000` invokes other run targets recursively and reuses their captures and disks. It runs alone, after the ordinary network suites.

### The worker count

Four. Three is the reasonable interactive setting and four is the measured ceiling for a machine whose guests are emulated rather than virtualised; beyond that the workers compete for the same four performance cores and add memory and scheduling pressure without finishing sooner. The count is a variable with four as its default, not a constant.

### The machine census

`tests/run` currently lists the QEMU processes before and after each suite and fails one that leaves a new machine behind. Under parallelism each worker would see every other worker's machines and report them as its own leak.

For `tests/run NAME` the present per-suite census is kept, since it attributes a leak to the suite that caused it. For `tests/run all` the census is taken once before any worker starts and once after all workers have been waited for, and a new machine fails the run as a whole. Attribution is lost in that mode and the report says so. Per-suite attribution under parallelism would need every invocation to name its machine, which is a change to the harness and to every shell suite, and it is not worth it to say which of four suites leaked when the run has already failed and `tests/run NAME` will name it.

### What is not done

No change to `pump`. No dynamic port allocation. No per-worker copy of the repository, no container, and no new language runtime. No reduction in the number of boots: a 4.76-second boot is small beside the waits, and boots that look alike often differ in kernel, disk, or command line. No attempt to run the guests under hardware acceleration; the host is ARM and the guest is x86, so there is none to be had.

## Standing rules for the run

* Work directly. Delegation is only for an independent review, one reviewer per diff, with the limits in `CLAUDE.md`.
* Commit in scoped units in the house style, directly on the branch, no attribution, never push.
* Every commit passes `tests/run style` and `tests/run host`.
* A suite that gets faster must still fail for its own reasons. Every phase that changes how a suite waits proves at least one of that suite's checks by making it fail, and the conversion is reverted if the check cannot be made to fail.
* Measure, do not estimate. Each phase records the wall time of `tests/run all` before and after it, on a warm tree, and `notes.md` keeps the numbers. A phase that does not pay for itself is reverted rather than kept for tidiness.
* A timing measurement is taken with nothing else running on the machine, and the census confirms no machine survived it.

## The phases

Do the phases in order. Within a phase, land the smallest self-contained commits that keep every intermediate tree passing the required checks. Record deviations and measurements in `notes.md`.

### Phase 1: arrival and the baseline

Read `AGENTS.md`, `CLAUDE.md`, and this plan in full. Require a clean tree, record `git log --oneline -12`, and run `tests/run all` on a warm tree with nothing else running, recording the total and every per-suite time as the baseline every later phase is measured against. Create `notes.md` and `current.md` beside this plan in the first commit.

### Phase 2: the waits that are already over

Convert each trailing `wait=` whose token is the last required event into `expect=`, keeping the old interval as `--steptmo` so a token that never comes still fails in the same time. Leave every wait named under "the waits that stay". Work one suite per commit, and for each, record the suite's time before and after.

Proof: for each converted suite, mutate the token the suite waits for and confirm the suite still fails, in the step timeout rather than instantly.

### Phase 3: the pacing in the shell suites

Convert `account`, `tcc`, `procfiles`, and `ssh` from fixed pacing to waiting on the marker each step already prints, using the helper each file already has. Leave the sleeps that hold a pipe open.

Proof: for each converted suite, mutate one marker and confirm the suite fails on the missing marker rather than passing on a lucky interval.

### Phase 4: the e1000 repeat set

Reduce the suite to ping, packet, dgram, netconsole, ssh, and the three selection boots. Then prove the retained set: one mutation in the e1000 receive path, one in its transmit path, one in its stop, and one in the driver-table selection must each fail it. Any mutation that survives puts its dropped suite back, and `notes.md` records which and why.

### Phase 5: preparation and execution

Split every suite into `prepare-NAME` and `run-NAME`, fill the registry's third field, and move every artifact write into preparation, including the two cross-suite restagings. Give the q35 suite its own disk and the second suite of each port pair its own port. `tests/run NAME` runs preparation then execution, and the behavior of a single named suite does not change.

Proof: with the tree already built, `make run-NAME` writes nothing under `build/`, checked by comparing a listing of the tree before and after. Every suite still passes when run singly.

### Phase 6: the pool

`tests/run all` runs one `make -j` over every preparation target, then the suites through a pool of four workers, with `style`, `host`, and `e1000` outside it. Replace the per-suite census with the batch census for this mode, keeping the per-suite one for a named suite.

Proof: the leak guard still works, shown by putting the `crash` suite's defect back and requiring the run to fail; a suite that fails still fails the run and is named; and the run is repeated three times to show the result does not depend on timing.

### Phase 7: completion

Run `make distclean`, then `make`, then `tests/run all`, and record the cold and warm numbers against the phase 1 baseline. Confirm no machine survives. Write `dev-log/33-fast/Readme.md` and add `33-fast` to `dev-log/Readme.md` in the same commit. The tree is clean and nothing is pushed.

## Recovery after a compaction

Read `AGENTS.md`, `CLAUDE.md`, this plan's end state and the current phase, `notes.md`, and `current.md`. Run `git status --short --branch` and `git log --oneline -15`, inspect every uncommitted path, and resume the current phase's first unfinished item. `current.md` names the active phase, the item in progress, the measurements already taken, and the exact next action; replace its contents instead of appending. After each phase and after each recovery, report one line: `fast: phase N; item; tree clean|dirty`.
