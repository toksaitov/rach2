# Milestone 33: a fast suite

## The goal

Bring `tests/run all` from 81 minutes to a small fraction of that without weakening a single check. At the end, every check that could fail before can still fail, for the same reason, on the same input, and a developer runs the whole suite while making a cup of tea instead of while making other plans.

Nothing in this plan trades coverage for time. Where the two conflict, coverage wins and the time stays.

This plan is the authority for the run. A conflict between this plan and the current tree is resolved by the decisions here; a conflict between this plan and `CLAUDE.md` is resolved by `CLAUDE.md`.

## Why

The suite is not slow. It is asleep, and it is serial.

A full boot to the kernel debug shell takes 4.76 seconds and one fifth of one core. The machine has ten cores and 24 GB, and one emulated machine costs 71 MB. During a complete run, one QEMU process sits at about three percent of one processor and the other nine cores do nothing.

Two measurements settle what to do about it. Six network suites take 462 seconds in series and 143 in parallel. Sixteen mixed suites take 760 in series and 98 in parallel. In both, the parallel time is the slowest member's, which is what independence looks like. The sixteen-suite run also produced the first real conflict: `disk-ahci` and `disk-q35` both create and boot `build/images/disk-test-ahci.disk`, so one renamed it into place while the other was checking it.

There is a second, smaller cost. The harness step `wait=SECS` is `pump(now() + SECS)`, an unconditional drain that never consults the success pattern; `tests/run ping` sees `PINGTEST OK` at 11 seconds and exits at 66. That idle time is worth removing, but only where it is on a worker's critical path once the suites run together. Removing it everywhere first would spend the highest-risk work before knowing which of it matters.

## The decided end state

### Parallel execution

`tests/run all` builds everything once and then runs the suites through a bounded pool. `tests/run NAME` behaves exactly as it does today.

The preparation is one `make -j` over the union of every suite's reusable prerequisites, so GNU Make sees one dependency graph and no two component sub-makes overlap. A full split of all 44 targets into `prepare-NAME` and `run-NAME` is not done: concurrent no-op component sub-makes are not a race once everything is current, because the component make runs no compile or link recipe when nothing has changed and `mk/fingerprint.mk` rewrites its tree only when the fingerprint is missing or changed. Suite-private fixture creation stays in the run recipes, where a suite writes only its own paths.

What must change for that to be true:

* `run-disk-ahci` and `run-disk-q35` both create and boot the same disk. The q35 suite gets its own path.
* `run-dhcp` rebuilds `rach2-metal.krffs`, which `diskdiag` and `metal` also boot, and `run-account` rebuilds `os-simple.krffs`, which `dhcp` also boots. Both writes move into preparation.
* The shared image builders that are phony targets rebuild on every invocation despite the up-front build. They become real file targets, or their consumers are serialised.
* Two host ports are used by two suites each: 2224 by `dhcp` and `diskdiag`, 2298 by `procfiles` and `drill`. The second of each pair gets its own port through a Makefile variable. No dynamic port broker is built; the suite list is fixed and short.
* `style` and `host` both use `/tmp/rach2_verify`, and `style` invokes a kernel build directly. They run outside the pool.
* `run-disk-q35` invokes `$(HARNESS)` without depending on it, which happens to work only because another suite built it first. It gets the dependency it uses.

### The e1000 suite joins the pool rather than shrinking

`tests/run e1000` re-runs nine suites on the second card and then makes three boots of its own, taking 914 seconds. Run as one serial block it would be the pool's critical path and would set the floor for the whole run.

Trimming it was considered and rejected. The host model proves the driver in isolation; it does not run the stack through the device, so dropping the second-card `netif`, `raw`, and `dhcp` runs would drop live interface reconfiguration, the raw socket with its hop limits and capture, and packet-socket address reconfiguration on that card. That is a coverage loss, and this milestone does not buy time with coverage.

Instead the second-card runs become ordinary pool members. Each needs the card selection and the private paths and ports its first-card twin has, so the two can run at once. The suite name stays; what changes is that its members are scheduled rather than sequenced.

### The worker count

Unknown until measured. Four guests at the measured cost would use about eight tenths of a core and 284 MB, which is nowhere near this machine, and the sixteen-suite run showed useful overlap well past four. But `selfbuild` compiles inside its guest and is far heavier than an idle boot, and the mix changes as idle time is removed.

The count is a variable. The run measures 4, 8, 12, and 16 workers on the finished pool and takes the best, with 8 as the starting default. A number is not written into this plan as though it had been measured.

### Attribution of a leaked machine

The census added in milestone 32 lists QEMU processes before and after each suite. Under a pool, each worker would see every other worker's machines.

Attribution is kept rather than abandoned, through one mechanism at the runner level. `tests/run` resolves the real `qemu-system-i386` once, gives each worker a private ledger file, and puts a shim first on `PATH` that records its own pid and then execs the real binary. The harness reaches QEMU through `execvp` and the shell suites resolve it through `PATH`, so both are covered without touching either. After a suite finishes, any pid in its ledger that is still alive is that suite's leak, even after the process was re-parented.

### The two rules about waiting

For the phase that removes idle time, and only where a suite is on the critical path:

* `wait=SECS` means timed observation. It never consults `--ok`. It stays wherever the interval itself is the assertion, or wherever work follows the token.
* `expect=TOKEN` means positive completion. The harness already returns the moment the pattern matches, so this needs no change to the harness and none to `pump`. A global early exit on `--ok` was considered and rejected: `expect_pattern` calls `pump` to read later output, so a check there would stop a recipe whose token appears early from ever receiving the input its later steps need.

The failure allowance the old `wait=` provided is preserved by `--steptmo`.

A conversion reproduces the token exactly. `--ok` is a literal substring test and `expect=` compiles a regular expression, so every metacharacter in the token is escaped: the matcher takes a backslash-escaped literal, and `expect=CREDDISK OK \(wrote\)` matches what `--ok "CREDDISK OK (wrote)"` matches. Unescaped dots in an address would match any byte, which is a looser check than the one being replaced, and looser is not acceptable here.

### The waits that stay

Each is a case where the token is not the end of the work. A conversion that cannot be shown to be outside this set is not made.

* `reset`: the token `reboot: restarting` is printed before the reset is attempted, and the six-second wait is what lets the forbidden `the platform did not reset` fallback reach the log, which the suite then rejects. Converting this would kill the machine before the fallback could appear and pass a broken reset silently. Better still, this one waits for the machine to exit and keeps the rejection.
* `dhcp`, the two lease boots: the token is followed by `poweroff -f`, and the wait is the machine stopping.
* `dhcp`, the unattended boot: the lease line is the token, and init and the SSH server come after it.
* `dhcp`, the static-address and renewal boots: a capture checker reads packets sent after the token, and the renewal case requires exactly one address request over the interval, which is an absence assertion.
* `netconsole`, the shutdown boot: the token is the goodbye line and the interval exists to see whether `powering off` also reaches the wire, which `pcapcheck --reject` then denies.
* `bootline`: the suite waits after a refusal diagnostic and then rejects any debug shell or login prompt. The interval is the assertion.
* `drill`, the recovery no-DHCP case: it waits 45 seconds after the login prompt before checking that the fixed address was kept and the client stopped. Not a trailing wait, so no phase touches it, but it belongs on this list.

`cred-persist` is not on this list, and the reason it was once given is wrong. `wait=15` only drains, and the verdict only searches for the token, so replacing `_exit(0)` with an endless loop would still pass. The drain proves nothing about the exit path. Only its first boot uses `_exit` at all; the other two return normally. If the exit path is worth proving, it is proved by expecting the diagnostic that follows it, not by waiting.

### The pacing in the shell suites

Four suites pace a console feed with fixed sleeps rather than waiting for the machine: `account`, `tcc`, `procfiles`, and `ssh`. They are converted only if they are on the critical path after pooling, and they wait on the marker each step already prints, using the helper their own file already has.

Two cautions. A long sleep that holds a FIFO open is not a delay: `account`'s 240-second tail and `shutdown`'s 180 keep a console writable while the test polls, and the machine is killed when the condition is met. And `procfiles` has an independent 42-second timer that opens the host connection `netstat` and `arp` later read; speeding the console commands without tying that peer to the `SSHD-UP` marker would let `netstat` run before the connection exists.

The count of 112 sleeps totalling 1384 seconds is a count of the text, not of elapsed time, and no saving is estimated from it.

### What is not done

No change to `pump`. No dynamic port allocation. No per-worker copy of the repository, no container, and no new language runtime. No reduction in coverage anywhere. No reduction in the number of boots: a 4.76-second boot is small beside the waits, and boots that look alike often differ in kernel, disk, or command line. No attempt to run the guests under hardware acceleration; the host is ARM and the guest is x86, so there is none to be had.

## Standing rules for the run

* Work directly. Delegation is only for an independent review, one reviewer per diff, with the limits in `CLAUDE.md`.
* Commit in scoped units in the house style, directly on the branch, no attribution, never push.
* Every commit passes `tests/run style` and `tests/run host`.
* A suite that gets faster must still fail for its own reasons. Every change to how a suite waits is proved by making that suite's own check fail, and where the check is an absence assertion the proof introduces the thing that must be rejected, not merely a missing token.
* Measure, do not estimate. Each phase records the wall time of `tests/run all` before and after it, and `notes.md` keeps the numbers. A phase that does not pay for itself is reverted rather than kept for tidiness.
* A timing measurement is taken with nothing else running, and the census confirms no machine survived it.

## The phases

Do the phases in order. Within a phase, land the smallest self-contained commits that keep every intermediate tree passing the required checks. Record deviations and measurements in `notes.md`.

### Phase 1: arrival and the baseline

Read `AGENTS.md`, `CLAUDE.md`, and this plan in full. Require a clean tree, record `git log --oneline -12`, and run `tests/run all` warm with nothing else running, recording the total and every per-suite time. Create `notes.md` and `current.md` in the first commit.

### Phase 2: the collisions

Give the q35 suite its own disk and `$(HARNESS)` as a dependency; give the second suite of each port pair its own port; move the two cross-suite restagings into preparation; make the shared phony image builders real file targets. Nothing runs in parallel yet.

Proof: every suite still passes singly, and the sixteen-suite batch that previously failed on the shared disk now passes when run together.

### Phase 3: the pool

`tests/run all` runs one `make -j` over the union of prerequisites, then the suites through a pool, with `style` and `host` outside it. Add the `PATH` shim ledger and use it for per-suite attribution in both modes.

Proof: the leak guard still names the suite that leaked, shown by putting the `crash` suite's defect back and requiring the run to fail with that suite named; a failing suite still fails the run and is named; and the run is repeated three times to show the result does not depend on timing.

### Phase 4: the worker count

Measure the finished pool at 4, 8, 12, and 16 workers, three runs each, and take the best. Record every number. Set the default to what was measured, not to what was expected.

### Phase 5: the e1000 members

Give the second-card runs the private paths and ports their first-card twins have, so they schedule alongside everything else instead of running as one serial block. Coverage is unchanged: the same nine suites still run on that card.

Proof: the e1000 members pass when run concurrently with their first-card twins, and the whole-run time drops by roughly the block's former length.

### Phase 6: the waits, where they still matter

With the pool in place, identify which suites are on the critical path and convert only those. Apply the two rules and the exact-token escaping. Leave every wait named under "the waits that stay".

Proof: for each converted suite, mutate the token and confirm the suite fails in the step timeout rather than instantly; and where the suite also has an absence assertion, introduce the thing that must be rejected and confirm it is still rejected.

### Phase 7: completion

Measure cold and warm properly: `make distclean`, then `tests/run all` (cold, since it builds what it needs), then `tests/run all` again (warm). Record both against the phase 1 baseline. Confirm no machine survives. Write `dev-log/33-fast/Readme.md` and add `33-fast` to `dev-log/Readme.md` in the same commit. The tree is clean and nothing is pushed.

## Recovery after a compaction

Read `AGENTS.md`, `CLAUDE.md`, this plan's end state and the current phase, `notes.md`, and `current.md`. Run `git status --short --branch` and `git log --oneline -15`, inspect every uncommitted path, and resume the current phase's first unfinished item. `current.md` names the active phase, the item in progress, the measurements already taken, and the exact next action; replace its contents instead of appending. After each phase and after each recovery, report one line: `fast: phase N; item; tree clean|dirty`.
