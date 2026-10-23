# Milestone 62: etude, one test runner that runs the whole collection anywhere

The [recovery acceptance and assignments](recovery.md) record the owner-authorized completion of the portable collection after the 23 October handoff.

## Goal

`tests/run` gives way to etude, a test runner written in C, and its guest helper `etude-guest`, which runs inside Rach2. Etude runs natively on Windows 11, on the latest macOS, on the current Ubuntu LTS and on Rach2 itself. The collection of tests passes in every one of those places, and its results do not depend on how fast the machine is or on whether the processor is emulated, virtualized or real. The phase runs the whole collection in each environment, finds which machine runs it fastest, and moves routine testing there.

A host test runs directly on the operating system of the machine that runs etude. A guest test boots Rach2 in QEMU. QEMU's TCG emulates the x86 processor in software; KVM on Linux and WHPX on Windows run the guest on the real processor with hardware virtualization.

## The owner's decisions

These were made on 2026-10-19.

* One phase, with one review and one release at its end.
* Outside Rach2, etude runs natively on Windows 11, on the latest stable macOS and on the current Ubuntu LTS, 26.04, which runs today in WSL on the testing PC.
* The whole collection works in every environment. A case is not bound to an environment and is not required to run in several. A test that cannot pass everywhere for reasons of timing, or of the differences between emulation, virtualization and native execution, is redesigned.
* Routine testing (the check before a push, ci and the release's full run) runs on one machine, the fastest, which this phase finds. The comparison of environments is work of this phase only.
* A full run on that machine aims at one hour, keeps a second hour for the tests of future subsystems, and never takes more than four. The ci limit of ten minutes and the push check's five stay. The first runs of this phase are measured and not held to the budget.
* The first run of etude covers the full collection on all four environments: the Mac under TCG, native Windows under WHPX, Ubuntu in WSL under TCG, and the Lenovo natively for every case that can run on real hardware.
* The rule for intermittent failures stays as it is: an `intermittent` issue stays open and does not block the release.
* Protection from stuck tests is part of the design, since a stuck test costs every worker time.
* Timing is recorded per case and per run in a structured history, for profiling and for finding a slowdown on the same machine.

## Research

The owner asked for [research](research.md) before this plan was written, and Codex finished it on 19 October. The coordinating session failed to act on it. It did not show the research to the owner whole, though the owner asked to see the results; it did not check this plan against it; and it did not raise the findings the plan left out until the owner found their effects on 23 October, too late to change the phase's design or the tasks already done. On that day the owner saw no point in further research. The findings, by the research's section, and what became of each:

* Taken: a test runner in portable C (C4), one catalog of cases with stable names (C1), one admission authority for each machine, shared by every account (C3), a run left unfinished reported apart from a failure (A4, C6), and limits measured from earlier runs on the same machine in place of fixed seconds (C6).
* Missed: an Apple Silicon Mac cannot accelerate an x86 guest, so it must emulate it under TCG or send the work to an x86 machine (A5), and each environment is measured before the design relies on it (C10). This plan put a whole run, the GNU build among it, on the Mac under TCG without measuring it. The build was estimated at 2 to 4 days when the owner found it on 23 October, and moved to a Linux virtual machine on the Mac (#873).
* Missed: 53 of the 101 suites were to become cases of shared families (B1, B2). Etude has one case file for each of the 103 suites.
* Missed: the cuts of B4, rows that check nothing and diagnostic work, out of the compulsory runs. On 23 October the owner decided that each suite's pointless tests are deleted in this phase.
* Missed: small reproducible subsets of the tests for an agent's quick checks, the practice of the parallel compiler project (A1). This plan has no such sample.
* Missed: the migration in 43 tasks (C9). This plan has 96.
* Set aside by the owner on 23 October: a deliberate fault to prove each new test (A1, C8, C9). No test is proven by a mutation.

## What the tree has now

* `tests/run` is a shell program. Its accelerator choice (`set_accel`) knows KVM, HVF and TCG, and not WHPX; ci, the release and every account on the testing host run TCG.
* One idle limit of 300 seconds, `RACH2_IDLE_TIMEOUT`, covers a whole suite (`tests/run`). It is not scaled to the machine and not kept per case.
* No timing history is kept. The runner prints `run: NAME: PASS in N` lines and nothing reads them later.
* Some tests depend on speed: a resize killed after a fixed six seconds (`grow.sh`), a guest clock compared with the host's within 30 seconds (`runtests.c`), deadlines raced against host disk flushes (`queue.sh`), spin counts and fixed polls in the USB, KRFFS, boot and console probes, and the sound test forced to TCG with `-icount` (`suites.mk`).
* The Lenovo runs only `metal_accept.sh` after a release. The harness reads COM1 and ends a test through QEMU's `isa-debug-exit` port, which a real machine lacks.

## Design

### The collection

A case states the behavior it tests, the architectures it applies to, the processor features, devices, tools and fixtures it needs, and the results it expects. It declares no obligation to run under several accelerators: there is no matrix of modes per case and no schedule that rotates accelerators.

A routine run uses one site profile: the chosen machine, its operating system and accelerator, its tool versions and its resource limits. Both Rach2 architectures go into one schedule, and each selected case runs once for each architecture it applies to, unless the test itself needs repetitions or several machine configurations.

Each case has stable case and assertion identities. Its manifest gives:

* the execution domain, the architectures and the workload;
* the processor features, devices, tools and fixtures it requires;
* isolation, privileges, process role and cleanup;
* the expected values, counts, states, exits or faults;
* the witnesses of progress, completion bounds and initial ceilings;
* the resources it declares and what must be prepared first;
* what a native run needs, or why no native equivalent exists.

The validator extends `tools/verify/suitecheck.sh` to the manifests, the scenario code, the musl probes and the host tests. It rejects a missing assertion result, a skip reported as a pass, a timing assumption not declared, and synchronization by sleeping. A static check cannot prove what a program does; the validator checks what can be read from the source.

### Tests that do not depend on speed

Tests wait for an event they can witness. A lock holder announces that it owns the lock before another process goes on. A receiver says it is ready. A test of a blocked process sees the blocked state before it applies the stimulus. A resize is interrupted after a disk change has been witnessed, while the real resizer is held at that point.

A rendering test looks at frames, demo progress or a program's completion. An input test counts delivered reports and the application's state, rather than requiring one interrupt per action. A disk deadline test uses controlled request state and logical time, not a race against the host's disk.

Clock correctness stays an assertion. It uses independent reference counts, known inputs and a stated uncertainty, and the arrival time of serial output is not the reference clock. A pause of the guest must produce the delayed or coalesced timer behavior the test means; a QMP `stop` (QEMU's machine protocol) is not taken to be the same as stopping the QEMU process.

The inventory of the tree gives separate repairs for:

* host assertions on retry durations, `pwauth_hosttest.c` among them;
* synchronization by sleeping in the mount, GNU utility and process checks;
* a resize interrupted after a real partial change;
* the readiness of Doom, the firmware and the services;
* controlled lag of block requests, and deadlines;
* spin counts and fixed polling;
* clock assertions and timer calibration;
* discovery of processor features, and a report of the branch a test exercised;
* the sound test's forced TCG;
* debug-entry checks that report success after an assertion that never ran.

`gnu_rows.sh` runs inside Rach2's GNU system, and its utility assertions stay guest tests. The APIC calibration already uses the measured reference interval, so its short window alone is no proof of a fault; tests under delayed execution decide which paths need repair.

### The environments of the first run

The phase shows the collection to be portable by running it whole in each environment:

| Environment | Etude built with | The collection runs |
| --- | --- | --- |
| Mac mini, Apple M4, macOS 27.0.1 | `cc` | in QEMU under TCG |
| Testing PC, Windows 11 | MinGW-w64 | in QEMU for Windows under WHPX |
| Testing PC, Ubuntu 26.04 in WSL | `cc` | in QEMU under TCG |
| Lenovo running Rach2 | TinyCC | natively, every case the hardware and kernel allow |

TCG on the testing PC is measured where it could change which machine wins. A TCG configuration wins only with a complete passing run measured over the whole collection, not from a few favorable cases.

The Windows executable depends on no MSYS or WSL runtime; a supporting tool may come from MSYS2. QEMU runs as a Windows process. An accelerator is requested explicitly, and a failed start does not fall back to another; QEMU accepts a list of accelerators to try in turn ([QEMU's invocation](https://www.qemu.org/docs/master/system/invocation.html)), so etude never passes one.

The first run is the full converted collection. The earlier compiler checks, the executor's own tests and the focused checks of each task are the work that leads to it; none of them stands in for it. The three virtual-machine environments run the whole collection on both architectures. The Lenovo runs every case that can run natively, on its own architecture; that smaller selection does not compete for the speed comparison.

Each environment's run fixes the source revision, the inventory of assertions, the hashes of the artifacts, the profiles, the tool versions, the state of the caches and the capacity declared, and its results keep them. Runs use checkouts of their own and immutable artifacts, so other work cannot change a comparison in progress.

The four environment runs wait for each other in one case only: a new kernel on the Lenovo waits for a complete pass of that kernel in a virtual machine. Windows and WSL share the testing PC, so each gets a measuring window of its own; jobs already running finish first, and the load from outside, memory pressure, paging and disk traffic are recorded.

Each run measures the cost of each case and family, the time of the whole run, the useful concurrency, the failures and the work left incomplete. Matched cases explain a difference; the whole run decides the winner. The recommendation is the fastest configuration that passes the whole collection under repeatable conditions, and a difference within the measured noise is reported as a tie.

### Native runs on the Lenovo

Which probes can run natively is found by reading every probe and scenario. The inventory records each one's process role, privileges, paths, fixtures, device assumptions, effects on the whole system, cleanup of children, and recovery.

The ordinary system-call, descriptor, signal, process and file-system checks are candidates; they get private fixtures and explicit cleanup. `auxvprobe` and `errnoprobe` are simple examples. A file-system probe that assumes `/vp`, a writable root or a particular mount is adapted first. A pipe probe that also examines the global heap has those assertions separated.

These cannot run natively, for reasons of what they test:

* QEMU monitor steps, removal of a virtual device, an injected NMI, the end of QEMU and QEMU's audio capture need QEMU.
* A test of an emulated card's driver cannot test that driver on the Lenovo, which has a different card. General network behavior uses configured peers and addresses.
* An exit of the designated init, forced boot profiles and tests of a fresh kernel's global state cannot be replaced by a child started over SSH.
* A test that exhausts a resource on purpose needs a machine of its own or a suitable native fixture.
* An assertion on a processor feature needs the feature, or a way to test its absence.
* Exact synthetic memory maps and firmware arrangements stay machine scenarios.

A failure under TCG, KVM or WHPX is no reason to leave that environment out.

The native transport is `etude-guest --stdio` over SSH without a terminal. The helper reads commands and writes results as frames on its standard input and output, and captures a child's output apart. The identities of the run, the attempt, the boot and the process keep a reconnection from being confused with the earlier connection. The helper keeps its own deadlines and cleans up after a lost connection. It needs neither COM1 nor `isa-debug-exit`.

The serial and standard-I/O transports have the same commands. An ordinary exit, an exit of the designated init, a panic, a reset and a shutdown stay distinct. `survivorprobe`, for example, keeps its designated-init contract; run as an ordinary child over SSH it would test something else, and could leave its intended child alive.

When the Lenovo already runs the exact candidate kernel, its native run goes on beside the others. Otherwise it waits for the first complete passing run of that candidate on both architectures in a virtual machine. An owner's loop then installs the candidate for a trial boot, by the documented identity check, installation and boot procedures, without making it the default, and after the run restores and checks the accepted system. `machine_update.sh` makes the boot the default, so the trial separates installation and checking from that step. Only the phase's release makes a candidate the default.

### Host tools on every machine

A pure C model runs natively wherever its contract allows; x86 subject matter alone does not need an x86 host. POSIX fixtures need a port or a declared local environment.

Production of the GNU root and checks that are specific to Linux namespaces use a declared local Linux virtual machine for tools where the Mac and Windows need one. Etude supervises it and charges its start, processor, memory, disk and run time to the run. Rach2 guests still run in the requested host's QEMU. Required work is neither sent to another machine nor left out.

The large-memory suite (`tests/system/suites/bigmem.sh`) has two obligations: an 8 GiB guest that writes and reads 5 GiB, and a 72 GiB boot that checks the 64 GiB mapping boundary and the memory accounting, without filling 72 GiB. The plan proves whether lazily allocated or file-backed guest memory gives that exact profile on the Mac within a conservative bound on physical memory. For that profile, the guest's configured memory, its resident memory and its disk backing become three separate quantities; every other guest still reserves its configured memory and overhead. The 72 GiB assertion is not reduced, skipped or run elsewhere. If a safe bound cannot be proved, the Mac's part stays unfinished and the owner gets a concrete proposal for capacity.

### Stuck tests

Every case has a progress watchdog. The worker tells apart a live transport, resource activity and progress of the case.

A heartbeat, repeated output, processor use or a duplicate record does not reset the watchdog. Progress is a completed step, an advancing counter of bounded work, or another witness the case declares. A quiet compiler or another opaque child gets a measured completion bound for its step, and is not made to print heartbeats.

The bounds come from successful runs of the same case on the same machine, accelerator, architecture and workload: the whole case's time and the longest gaps between progress records, with the measured variation and a margin, between a declared floor and ceiling, fixed when the run is planned. A case with no history uses its declared initial ceiling. A failed, cancelled or disrupted attempt never enters the history the bounds come from. A separate ceiling on the whole case keeps endless apparent progress from keeping it alive.

Boot, command, transfer, capture of evidence and cleanup each have a bounded stage of their own. The 300-second idle limit of a whole suite goes away as the families convert.

When a watchdog expires:

1. The worker marks the attempt incomplete and issues no more operations for the case.
2. It saves the current step, the age of the last progress, resource samples, the identities of the processes it owns, the case's output and the last lines of the console.
3. Under QEMU it tries, within a bound, to read the status and the registers of every virtual processor, and records any error. A broken QMP connection cannot delay the end.
4. It asks the guest or the native child to stop. After a bounded grace period it closes the machine through QMP where it can, then ends the processes it still owns.
5. On POSIX systems it sends a termination signal and then a kill. On Windows it ends the registered Job Object. Services and peers it owns are stopped after their diagnostics are collected.
6. It reaps the processes, closes private endpoints, removes the scratch of a successful run, and confirms that the machines and their descendants have ended before it releases their reservations.

QEMU is not suspended and then expected to answer QMP. A stopped POSIX process is resumed for a graceful end, or killed. The tests of failure cover these details.

A lost SSH connection, a missing process ID or an absent lock does not prove cleanup. A claim whose state is uncertain stays reserved, and the rest of the run goes on wherever confirmed capacity allows. A machine that cannot be recovered takes no more work until its state is established.

On the Lenovo the helper itself cleans up when the controller disappears. If the kernel is stuck, the result stays incomplete and an owner's loop uses the existing recovery procedure; a reboot is not a routine cancellation and never part of deployment.

A case that hangs again opens or updates one issue, named by the case and the place of the failure, with the identities of the attempts, the bounds, the last progress, the machine and the evidence. Etude neither raises the bound by itself nor removes the case from selection.

### Timing history

The history starts before the collection runs. Each machine keeps versioned JSON Lines records that a script can read. Separate writers use separate streams or one serialized writer, and recovery does not take an unfinished last record for a completed result.

| Part | Contents |
| --- | --- |
| Identity | run, case, attempt and step; commit; hashes of the runner and the artifacts |
| Environment | machine, operating system, accelerator, architecture, profile and tool versions |
| Time | UTC times and monotonic durations of queueing, build, transfer, boot, execution and cleanup |
| Resources | processor time of the whole process tree, peak memory, reservations and observed load |
| Steps | each step's identity, start, duration, witnesses of progress and outcome |
| Result | pass, failure, incomplete or cancelled; references to evidence and capture errors |
| Comparison | the baseline's identity, the workload, the concurrency and whether it may be compared |

Processor time includes ended descendants, and the guest's processor time is not counted again on top of its QEMU process. Peak memory is the combined peak where the platform reports one. The sampling method, interval and bounds are recorded, and so is a measurement that is not available; a missing value is never written as zero.

A run's record adds up its cases and keeps the whole run's time and peak resource use. The history is kept to a bounded size; accepted baselines and the evidence of open issues stay, as Resources in `AGENTS.md` says.

A comparison of performance matches a case by its identity and workload, not by the kernel commit, or no slowdown between commits could be found; the exact commits and artifacts are in the record. A changed workload starts a new baseline unless a review shows the two equivalent.

Once there are enough comparable successful samples, etude derives a robust baseline and a noise band from them. A result slower than the band produces a report with the baseline, the number of samples, the steps that changed and the commits in between. Too little history is reported as such.

The report does not fail a correctness check; a budget violation is reported apart. A person looking into a slow case selects its history, compares the boot and step costs, separates processor work from waiting, and runs the exact case again under a profiler with the command the record gives.

The same history gives the processor reservations, the watchdog bounds and the budget forecasts. These uses stay separate: a duration that creeps up does not enlarge a versioned budget.

### Admission and execution

A processor reservation is the median of a job's processor seconds divided by its wall seconds, with headroom, below a declared ceiling; the case's declaration gives the first value. Memory and disk reservations stay conservative. Builds, services, tooling virtual machines, transfers and growing caches are counted.

Each physical machine has one admission authority. Windows and WSL cannot each offer the whole testing PC. Routine testing uses only the selected endpoint, and builds or development work on the same machine still consume its capacity.

The scheduler puts both architectures together, uses compatible measured costs and the order of preparation, and is fair between accounts. It protects one blocked request per machine: running jobs finish, and new admissions keep free the capacity that request needs. There is no backfilling by time and no promise of an optimal schedule.

Etude is hosted C99 with POSIX, Win32 and Rach2 adapters; kernel code stays C89. A machine scenario is a separate executable linked with `libetude-case`, so a change to a scenario does not rebuild the worker.

Each independent Rach2 case boots afresh. A scenario with state may have several dependent steps or reboots, and the assertions of one program do not each need a boot. Shared boots and snapshots of virtual machines are not part of this work.

Artifacts are immutable and addressed by their content. Preparation is a dependency graph, and independent cases get private writable state. A cache keeps the products of setup, never the operation under test. A durability case uses its own copy of a disk.

`etude-guest` is staged in the test image and copied into a private workspace for a native run. The GNU and os-real images gain no permanent dependency on it. Tests of SSH keep their own authentication and transport assertions.

SSH uses bounded, versioned frames and checked transfers. A serial frame from the guest has the run and boot identities, a sequence number, a checksum and an acknowledgement. A short serial record fits in one console write: a write(2) to the console is copied in chunks of 256 bytes (`rach2/sys/syscall_fs.c`), each passed to `tty_write` between `console_write_begin` and `console_write_end` (`rach2/sys/tty/tty.c`), and a kernel record raised meanwhile, an interrupt's included, waits until the chunk ends (`rach2/sys/klog.h`). `etude-guest` is the only writer to the console while cases run. A repeated command cannot repeat its effects, and raw panic output overrides an ordinary success token.

A reservation is durable before execution. A POSIX child waits to be registered before `exec`. A Windows child starts suspended, enters a Job Object it cannot leave, is registered, and then resumes. Recovery checks the identities of the boot and of process creation.

A POSIX process group does not contain a daemon that escapes it. Test tools run in the foreground or report the children they start and clean them up. The tests of failure cover escape, the death of a worker, reuse of a process ID, lost connections and uncertain ownership.

### Deployment

Deployment starts from the owner's Mac. It checks the prerequisites, installs them where it is allowed to, sends etude's source, builds it with the target's compiler, prepares the configuration and shared state, and records what the machine can do. Where administrator rights are missing, it prints the commands that remain.

The cache of executables is keyed by etude's own source, its build recipe, the platform and the compiler, so a kernel commit does not rebuild it. Versions live side by side and share a compatible admission format; an incompatible version refuses admission rather than disturb running work.

Deployment creates no account or key, changes no clone, deletes nothing, writes no raw disk, restarts no machine and interrupts no test. The owner provides SSH access.

### The move from tests/run

A temporary adapter runs the old suites under etude while the families convert. It keeps the preparation, the GNU and recovery inputs, the QEMU shims, the machine ledgers, the console relay, the cleanup and the evidence of failure. It calls `make run-NAME` only as `tests/run` does, with the tree's run lock, a port block and the host lock held, since the target alone runs a suite without its locks, and it does not start the old scheduler inside etude.

Old and new runs share the host safely. Etude takes the existing lock files (`DIR/rach2-host.lock`, a write lock for `bigmem`) and the port blocks `/tmp/rach2-ports.N`, and connects them to its admission. While a machine enrolls, old runs that etude does not wrap finish before shared admission begins. An old checkout, as `bisect.yml` uses, runs through a wrapper outside its source tree. Setting `RACH2_PORT_OFFSET` skips the old lock, so the adapter sets it only once it owns the matching reservation; old suites get distinct port blocks, and native allocations avoid them.

The status readers (`tools/flow/test_status.py`) and the familiar failure lines (`run: NAME: PASS|FAIL in`, `run: result: failed:`, which `tools/ci/run.sh` and `tools/flow/release.sh` read) are in place before `tests/run` switches to etude. Suite names keep working, and a failure of etude itself is reported as `runner`, which `tools/ci/report.sh` treats as a reason for a hold. The structured readers for ci, reports, bisection and the release are written against fixtures while the families convert.

Bisection of an old commit uses that commit's runner under etude's admission. A missing case or an incomplete run leaves the range unresolved, never passed.

### Flaky results

Flakiness is looked for by repeated, shuffled runs with recorded seeds and controlled, reserved load. A case whose timing was repaired gets focused repetitions; the whole collection is not multiplied across environments at each release.

A result is possibly flaky when the same behavior, under valid declared conditions, sometimes passes and sometimes fails or does not finish. A seed reproduces the selection, the order and the injected disturbances, not the host's exact schedule. The original attempts stay visible.

The rule for intermittent failures does not change: an `intermittent` issue stays open and does not block a release, and a passing retry does not erase its history. Cases stay selected; nothing is quarantined or skipped.

### The budget

The budget applies to routine testing on the selected machine.

| Measure | Limit |
| --- | --- |
| A full run, the goal | 3,600 seconds |
| Growth kept for future tests | a further 3,600 seconds |
| A full run, the ceiling | 14,400 seconds |
| ci | 600 seconds |
| The check before a push | 300 seconds |
| The first runs of this phase | recorded, not held to the budget |

The measured time starts when the run first gets capacity and ends after cleanup. It includes ordinary builds, preparation, tooling virtual machines, transfers and execution. Waiting in an outside queue, installing prerequisites once and building the vendored toolchains the first time are reported apart. Installation on the Lenovo stays a separate step of the release.

The first runs' exemption names their manifest and attempts, and ci and the release cannot use it. The watchdogs stay on during it.

ci checks the versioned cost declarations against the selected machine's reference capacity and reports the goal, the growth used and the ceiling; it does not run the whole collection on each push to find out. Real ci and full runs check their own limits.

A hard deadline keeps time for cancellation and cleanup. An overrun fails the budget check and leaves the unfinished work incomplete. A lost machine or unconfirmed cleanup is never reported as a run completed within budget.

The selected machine runs each case once per architecture with its accelerator. There is no recurring run of the collection on the Mac, Windows, WSL and the Lenovo. A change specific to one platform still gets the focused check that proves it.

### Review and release

The reviewer loop reviews every commit of the phase, in consecutive ranges where one review cannot read them all; the manager writes the end of a range to the loop's `STATE/until`. The natural points of division are the commit that switches `tests/run` to etude, the removal of the old runner, and the phase's last commit.

The release runs the whole collection from clean on the selected machine, keeping the vendored outputs as the rules allow. The rerun of only the failed suites after their fixes, the handling of intermittent failures and the acceptance on the Lenovo stay as they are. The phase's native run of the collection on the Lenovo does not become a full native run required after every later phase.

## Rules for the work

* Every task takes the check that `AGENTS.md` gives for what it touches.
* A new or redesigned test gets a focused check; the full runs in the four environments show the collection portable. No case has an obligation per accelerator.
* Etude is hosted C99; kernel code stays C89 and is checked on both architectures.
* Nothing creates an account or a key, changes a clone, deletes data, writes a raw disk or restarts a machine, except the owner-only tasks on the Lenovo, which follow the `machine` skill.
* A fault found along the way that the task does not fix becomes a `bug` issue, made a member of this phase with `sh tools/flow/board.sh link N`.

## The phase

One phase, "Build etude and replace the test runner", with 96 tasks. Its longest chain of tasks that must run one after another has 17. The 24 conversions, T64 to T87, are independent of each other. Up to seven sessions work at once, two of them the owner's, as the machines allow. A task waits only for the tasks it names. Eight tasks are owner-only, since they need the owner's Mac, the Windows side of the testing PC, the Lenovo, or the shared configuration of the testing host and ci: T48, T49, T50, T56, T91, T92, T93 and T95.

In the files, `E` is `tools/etude/`, `C` is `tests/cases/` and `D` is `dev-log/62-etude/`. A family named in a task includes its scripts, recipes, helpers and proofs.

Contracts and records:

* T01. Map the existing assertions. Files: the coverage inventory and the migration map. Account for every suite, row, probe and proof. Done when each assertion has a purpose kept, a replacement or a removal with its reason.
* T02. Define the timing and comparison records. Files: the history schemas and `D/measurements.md`. Define identities, measured scopes, comparable workloads, noise and the first runs' exemption. Done when examples tell apart complete runs, missing data and comparisons that are not valid.
* T03. Prepare the platform prerequisites. Files: prerequisite recipes, tool fixtures and testing instructions. Prepare what Linux, macOS, Windows and Rach2 need. Done when each required tool has a concrete installation or build, and missing rights produce the exact commands.
* T04. Audit native eligibility. Waits for T01. Files: the native inventory and fixture descriptions. Examine privileges, process roles, paths, devices and global effects. Done when every probe has a native plan or a specific reason it needs a different fixture.
* T05. Define the portable execution contracts. Waits for T01, T02 and T04. Files: `E/Readme.md`, headers, protocols and schemas. Define cases, assertions, resources, progress and results without obligations per accelerator. Done when examples cover host programs, guest programs, native execution and machine scenarios.
* T06. Validate assertions and timing contracts. Waits for T05. Files: the manifest validator, `suitecheck.sh` and fixtures. Done when a missing result, a skip reported as a pass and a forbidden timing dependency are each rejected.
* T07. Record structured timing history. Waits for T02 and T05. Files: the history writer, collectors and schemas. Record each run's, case's and step's time, processor time, memory and identities. Done when concurrent writers, interrupted records and unavailable measurements cannot corrupt or falsify the history.

Executors:

* T08. Implement POSIX execution. Waits for T05. Files: the executor and the POSIX adapter. Capture output, supervise children and collect resource use. Done when concurrent success, failure and cancellation keep their identities and clean up registered descendants.
* T09. Implement native Windows execution. Waits for T05. Files: Win32 processes, Job Objects, pipes, sockets, paths and clocks. Build with MinGW. Done when the imports name no MSYS or Cygwin runtime, and fixtures cover suspended start, accounting of children and termination.
* T10. Implement QEMU execution. Waits for T05 and T08. Files: the QEMU launcher, profiles, serial capture and QMP. Select the accelerator explicitly. Done when both kernels boot where supported, and a failed accelerator, an unexpected exit and a cancellation are told apart.
* T11. Implement the guest transports. Waits for T05. Files: `E/guest/`, build rules and `test.manifest`. Provide serial and framed standard-I/O execution without the debug-exit port. Done when a pass, an intended failure, duplicate commands, an interruption and the designated-init exit keep distinct contracts.
* T12. Implement the Rach2 executor. Waits for T05 and T11. Files: the Rach2 adapter, accounting interfaces and TinyCC recipes. Done when it builds and its focused checks pass on Rach2 in QEMU, before it runs on the Lenovo.
* T13. Build cached source versions. Waits for T05. Files: the SSH bootstrap and the version cache. Build a requested etude source once, without an installed worker. Done when configured shells, interrupted builds, reuse by hash and versions side by side work.
* T14. Report performance regressions. Waits for T07. Files: history comparison, reports and profiling commands. Compare equal workloads on the same machine and accelerator. Done when noise, too little history and a real slowdown are told apart, and reports give the commits in between without failing a correctness check.
* T15. Implement per-case watchdogs. Waits for T07, T08, T09, T10 and T11. Files: progress tracking, bounds and termination. Done when a busy loop, a stream of heartbeats, quiet valid work, a hung QMP and a lost SSH connection each give the specified result.
* T16. Recover durable resource claims. Waits for T08 and T09. Files: the ledger, locking and recovery. Register execution before a child is released, and check identities after a failure. Done when the death of a worker, reuse of a process ID, escaped children and uncertain ownership cannot free live resources.
* T17. Implement remote execution. Waits for T05 and T13. Files: the remote protocol and checked transfers. Keep the attempt's identity across a reconnection. Done when truncation, duplicated commands, damaged content and incompatible protocols cannot produce a successful execution.
* T18. Schedule both architectures together. Waits for T07 and T16. Files: admission and scheduling. Done when work under contention makes progress without double allocation or backfilling by time.
* T19. Publish immutable artifacts. Waits for T08 and T17. Files: the artifact cache and the preparation graph. Done when races, failed preparation and interrupted publication cannot expose a partial artifact.
* T20. Implement machine scenarios. Waits for T10, T11 and T19. Files: `libetude-case`, QMP and disk operations. Done when dependent reboot steps keep their state and a lost transport cannot be taken for the expected event.
* T21. Supervise services and several machines. Waits for T20. Files: services, peers and several machines. Done when a failure at any stage of preparation or execution cleans up only the owning case's processes.
* T22. Implement repeatable deployment. Waits for T03, T13, T16 and T19. Files: deployment, configuration and permission fixtures. Done when a repeated or partial deployment succeeds without changing clones, keys or running tests.
* T23. Run host checks in parallel. Waits for T06, T08 and T19. Files: host selection, consumers of the compile commands, and metadata. Done when the whole selection stays present, outputs are isolated and sanitizer failures stay fatal.
* T24. Record shuffled and loaded repetitions. Waits for T07 and T18. Files: the repetition driver, seed records and load fixtures. Done when a seeded race is exposed, and retries do not erase the earlier failures.

Proofs and new checks:

* T25. Gather the mutation proofs in one driver. Waits for T19. Files: the common proof driver and mutation interfaces. Done when the unmodified, faulty and restored artifacts are told apart, and an incomplete restoration is rejected.
* T26. Move the process and protocol proofs to the driver. Waits for T25. Done when the catalogue keeps every distinct fault and its failing assertion.
* T27. Move the storage and image proofs to the driver. Waits for T25. Done when damaged and restored artifacts exercise the same assertions, and no tested operation is cached.
* T28. Add the missing random and system-call checks. Waits for T01. Files: tests of the random generator, and probes of `preadv`, `pwritev` and `pselect`. Done when deliberate faults fail the relevant assertions.
* T29. Exercise the whole PS/2 initialization. Waits for T01. Files: `ps2mouse_hosttest.c` and its existing controller model. Done when wrong controller sequences fail, with the existing model extended rather than a second one added.
* T30. Fuzz the ELF64 loader. Waits for T01. Files: an ELF64 host harness and corpus. Done when the sanitizer runs and a deliberate validation fault show invalid images rejected.
* T31. Remove the duplicate guest copies of the workflow checks. Waits for T01. Files: the agents rows and the mutation selections of the board and worker-loop scripts. Done when the host checks and short Rach2 integration assertions stay selected.

Tests that do not depend on speed:

* T32. Count retries in place of timing them. Waits for T01. Files: `pwauth_hosttest.c`, narrow clock interfaces and host fixtures. Done when wrong retry behavior fails every time and the real process-lock integration stays covered.
* T33. Replace synchronization by sleeping. Waits for T01. Files: the mount, GNU rows, wait, signal, epoll, FIFO and related probes. Done when delays of a producer keep the results, and absent states and wrong ordering fail.
* T34. Interrupt the resize at a witnessed change. Waits for T01. Files: `grow.sh`, its helpers and the resizer fixture. Done when fast and delayed execution produce the same repair obligation.
* T35. Observe rendering and service readiness. Waits for T01. Files: the Doom, UEFI and boot-service checks. Done when stalled work fails and a change of speed does not change a correct outcome.
* T36. Control the lag of block requests. Waits for T01. Files: the block-request test controls and their documents. Done when wrong ownership, completion and release are detected without relying on the host's disk speed.
* T37. Test disk deadlines through controlled state. Waits for T36. Files: the queue, AHCI and flush-deadline checks. Done when deadline behavior and the completion of independent requests stay covered beside the real driver.
* T38. Replace polling that depends on speed. Waits for T01. Files: the USB, KRFFS, boot, console and input probes. Done when report batching, device delay and host load do not change correct results.
* T39. Separate clock assertions from transport delays. Waits for T01. Files: `runtests.c`, the clock scenarios and reference helpers. Done when clock faults fail and serial latency cannot make a false failure or pass.
* T40. Prove the timer calibration under delayed execution. Waits for T01. Files: the PIT, APIC and TSC calibration tests and any repairs. Done when the arithmetic on the measured reference and the fallback paths stay correct under controlled descheduling.
* T41. Declare and observe processor features. Waits for T01. Files: processor profiles, the RNG and XSAVE probes, and assertion records. Done when the positive and negative paths report the branch they exercised.
* T42. Remove the sound test's dependence on TCG. Waits for T01. Files: the SB16 scenarios, capture analysis and mutation proof. Done when wrong samples or device behavior fail whatever the speed.
* T43. Repair the coverage of the debug-entry assertions. Waits for T01. Files: the ptrace debug probes and watchpoint proofs. Done when real debug-entry behavior and a deliberate fault give complete, distinct results.

Native and local tools:

* T44. Adapt every eligible probe for native runs. Waits for T04 and T11. Files: probe entry points, private fixtures and native metadata. Done when the assertions are kept and an ordinary process is not substituted for a designated-init or fresh-kernel contract.
* T45. Provide local tools on every comparison host. Waits for T03, T08, T09 and T19. Files: ports of host tests and recipes for a local Linux tooling machine. Done when every required command runs locally and its cost is recorded.
* T46. Prove bounded backing for the large-memory profile. Waits for T03, T10 and T16. Files: memory profiles, backing controls and resource declarations. Done when the boundary assertions stay unchanged and an unsafe allocation is refused; T48 proves it on the Mac.
* T47. Prove the Linux runtime and recovery. Waits for T15, T16, T17, T18, T19 and T21. Files: the executor's integration tests. Done when competing accounts, lost processes, full disks, blocked jobs and transport failures give the cleanup, evidence and retained claims of the design.
* T48. Prove the native Mac runtime. Owner-only. Waits for T15, T16, T17, T18, T19, T21, T22, T45 and T46. Done when the Mac runs every promised operation and bound: processes, the watchdog, deployment, the tooling machine and the memory backing.
* T49. Prove the native Windows runtime. Owner-only. Waits for T09, T15, T16, T17, T18, T19, T21, T22 and T45. Done when execution uses Windows processes and cleans up after failures of the controller and the children.
* T50. Prove the native Lenovo runtime. Owner-only. Waits for T12, T15, T17, T22 and T44. Done when a TinyCC build on the Lenovo passes, fails on purpose, cancels and survives a disconnection over framed SSH, without a debug-exit port.

The move from tests/run:

* T51. Bridge the old locks and ports. Waits for T16. Done when old runs, exclusive memory jobs and new execution cannot overlap unsafely.
* T52. Extract the whole legacy preparation. Waits for T19. Files: the preparation adapter and its Make integration. Done when the required artifacts exist before a suite starts.
* T53. Keep the supervision of legacy suites. Waits for T10, T15, T51 and T52. Files: the suite adapter, QEMU shims and `runutil`. Done when leftover machines and incomplete execution cannot pass.
* T54. Keep status and failure attribution. Waits for T05, T07 and T16. Files: the compatible formatter, `test_status.py` and the manager's fixtures. Done when the existing readers keep suite failures and `runner` attribution, and tell useful work from admission waits, stalled cases and uncertain claims.
* T55. Wrap old checkouts. Waits for T51, T53 and T54. Files: the installed wrapper and bisection's entry. Done when old revisions run under current admission without a nested-lock deadlock, and missing cases stay unresolved.
* T56. Enroll the shared Ubuntu endpoint. Owner-only. Waits for T22, T47, T51 and T55. Files: the private site configuration and access records. Done when every allowed launch is accounted for, without deciding the eventual winner. Accepted configuration and checks: [Ubuntu enrollment](ubuntu.md).
* T57. Route the test commands through etude. Waits for T06, T18, T21, T23, T53, T54, T55 and T56. Files: `tests/run` and compatible targets. Done when no nested old pool starts and status, cancellation and failure attribution stay correct.
* T58. Check the budgets from recorded costs. Waits for T07 and T18. Files: the cost model, budget policy and ci fixtures. Done when unknown costs, omitted work and hidden preparation cannot pass the budget.
* T59. Read structured ci results. Waits for T05, T06 and T07. Files: the ci driver and parser fixtures. Done when missing results, incomplete execution and runner faults cannot produce a successful ci result.
* T60. Keep issue reporting and the intermittent rule. Waits for T05 and T07. Files: ci reporting and issue fixtures. Done when intermittent issues stay open and do not block, and original failures stay visible.
* T61. Read structured results in bisection. Waits for T05 and T55. Done when missing cases, lost infrastructure and incomplete work leave a range unresolved.
* T62. Run the release on one selected machine. Waits for T05. Files: `release.sh`, `full.yml` and fixtures. Done when clean builds, reruns of fixes only, the intermittent rule and the Lenovo acceptance keep their contracts.
* T63. Update the operating instructions. Waits for T05 and T54. Files: Readmes, the worker and manager prompts, the skills and `testing.sh`. Done when the examples agree with the command fixtures and describe no recurring matrix of environments.

Conversions, each waiting for T57 and T24 and for the repairs named:

* T64. Convert the style suite. Done when deliberate source and manifest violations stay fatal.
* T65. Convert the GNU checks; also T32, T33, T39 and T45. Done when independent rows are split and the real configure, compile and run in the guest stay, with no tested operation replaced by a cached result.
* T66. Convert the agent sessions and fake services (`agents`, `agentsaccept`, `fakeapi`); also T31 and T33. Done when session, transport and application assertions survive concurrency and shuffled order.
* T67. Convert the host API checks (`codexapi`, `museapi`); also T32 and T45. Done when isolation, credentials and cleanup stay covered on each comparison host.
* T68. Convert the default and memory cases (`default`, `memory`, `pools`); also T38, T41, T43 and T44. Done when independent cases keep all assertions and correct native fixtures.
* T69. Convert the processor and large-memory cases (`smp`, `fpu`, `clock`, `noentropy`, `bigmem`); also T39, T40, T41 and T46. Done when actual features and memory bounds are recorded without weakening the 64 GiB boundary test.
* T70. Convert the process and account cases (`authority`, `account`, `login`, `cred-persist`, `procfiles`, `fdlock`, `textbusy`); also T32, T33, T43 and T44. Done when privilege, descriptor, process and persistence coverage stays complete.
* T71. Convert the terminal and manual cases (`console`, `conload`, `exitline`, `bootline`, `servicelog`, `man`); also T33 and T38. Done when lookup, pager, input and lifecycle assertions work without fixed waits.
* T72. Convert the audio and Doom cases (`audio`, `doom`); also T35 and T42. Done when capture and rendering assertions measure completed behavior rather than survival time.
* T73. Convert the USB cases (`usb`, `usbdisk`); also T38. Done when report batching and a changed device speed cannot change a correct outcome.
* T74. Convert the disk driver cases (`disk`, `disk-ahci`, `disk-q35`, `diskrate`, `diskdiag`, `diskpolicy`); also T36, T37 and T38. Done when controlled faults and real driver paths keep their distinct assertions.
* T75. Convert the storage pressure and persistence cases (`persist`, `persist-ahci`, `diskfull`, `fault`, `queue`, `load`, `stress`, `stress-ahci`); also T36 and T37. Done when private disks and controlled request states keep the persistence and pressure coverage.
* T76. Convert the writable file-system cases (`fatwrite`, `exfat`, `ext2`, `large`); also T33 and T38. Done when complete assertions no longer depend on narrow margins of wall time.
* T77. Convert the mount and virtual file-system cases (`mount`, `krffsmount`, `loopdev`, `krffscheck`, `subset`); also T33 and T38. Done when errors, permissions and cleanup stay complete, native runs included.
* T78. Convert the partition and boot-order cases (`parts`, `bootpart`, `disks`, `bus`, `bootorder`, `grub`); also T38 and T41. Done when every arrangement boots the intended artifact.
* T79. Convert the installation cases (`install`, `menu`, `sealed`, `drill`); also T33. Done when old and new runs compared keep every installation assertion and its cleanup.
* T80. Convert the repair and growth cases (`installer`, `installer-repair`, `installer-rehearse`, `grow`); also T34 and T37. Done when fast and delayed execution test the same recovery from a partial state.
* T81. Convert the uefi suite; also T35 and T41. Done when readiness by events boots and checks the intended artifacts: installation, wide disks, NVMe and direct kernel boot.
* T82. Convert the self-build and development-tool cases (`selfbuild`, `selfbuild-ext2`, `tcc`, `make`); also T32 and T33. Done when real compilation and execution of the produced bytes stay, and a smaller fixture is accepted only when it keeps the distinct obligations.
* T83. Convert the machine-lifecycle cases (`crash`, `reset`, `shutdown`); also T11. Done when a missing event, an unexpected exit and a lost transport cannot pass.
* T84. Convert the packet and socket cases (`socket`, `dgram`, `dgram-e1000`, `ping`, `ping-e1000`, `packet`, `packet-e1000`, `raw`, `raw-e1000`); also T33 and T38. Done when every variant stays isolated and complete.
* T85. Convert the network configuration cases (`dhcp`, `dhcp-e1000`, `netif`, `netif-e1000`, `addressed`); also T33 and T38. Done when leases, addressing, errors and cleanup stay covered with no leftover service.
* T86. Convert the SSH and network-console cases (`ssh`, `ssh-e1000`, `e1000`, `netconsole`, `netconsole-e1000`, `metal`); also T33 and T38. Done when real networking on the Lenovo is kept apart from the emulated cards' coverage.
* T87. Keep the image builders' concurrency checks (`imagerace`). Done when concurrent or interrupted publication cannot produce mixed artifacts.

Removal, the four environments and the move:

* T88. Remove the temporary suite adapter. Waits for T26 to T30, T59 to T62 and T64 to T87. Files: the adapter, the old run targets, the registry and references. Done when the whole inventory of assertions runs under etude, and old checkouts keep their wrapper.
* T89. Remove the obsolete runner programs. Waits for T26 to T30, T47 to T49 and T88. Files: the old harnesses and helpers, and the `runner` and `drive` tests. Done when the reference and coverage audits pass. It may overlap the environment runs, which use checkouts of their own. Six programs still had users then; [their conversion](old-programs.md) moved those users to etude, and #875 removed the six with their tests and the `runner` and `drive` suites.
* T90. Run the whole collection on Ubuntu. Waits for T24, T47, T58 and T88. Run both architectures under TCG. The owner dropped the run under KVM on 22 October 2026: Windows' October update keeps nested virtualization from WSL on the testing PC's AMD processor (#878). Done when every assertion, cost, width, failure and cleanup is recorded on the fixed candidate. Runs: [the whole collection on Ubuntu](ubuntu-run.md).
* T91. Run the whole collection on the Mac. Owner-only. Waits for T24, T48, T58 and T88. Run both architectures under TCG. Done when local tools and the large-memory backing keep every assertion, with no work skipped or sent elsewhere. Runs: [the whole collection on the Mac](mac-run.md).
* T92. Run the whole collection on Windows. Owner-only. Waits for T24, T49, T58 and T88. Run both architectures under WHPX, with the TCG measurements that bear on the choice. Done when native execution and the whole run's costs are recorded without load from WSL.
* T93. Run every eligible case on the Lenovo. Owner-only. Waits for T24, T50 and T88, and, when the Lenovo needs the candidate installed, for the first complete pass of T90, T91 or T92. Done when complete results and cleanup are recorded on the exact candidate kernel, and the accepted system is restored and checked.
* T94. Select the fastest test machine. Waits for T14, T90, T91 and T92. Files: the comparison report and the proposed site profile. Done when the recommendation follows the measured whole-collection performance, and the Lenovo's smaller selection is not scored against the full runs. Report: [the comparison of the test machines](machines.md).
* T95. Move routine testing to the selected machine. Owner-only. Waits for T22, T58 to T62 and T94. Done when push checks, ci and the release use one accounted machine with its limits, after the running jobs finish. A move that needs a new account or a hardware decision goes to the owner with the measured recommendation.
* T96. Audit the phase for its review. Waits for T01 to T95. Files: the plan, the coverage inventory, the comparison report and the acceptance record. Done when every obligation has evidence or an ordinary issue, and the review and release can establish completion. The part that does not wait for the runs: [the audit of the tasks on master](audit.md).

## After the release

The release runs the whole collection on the selected machine and installs the reviewed commit on the Lenovo. `dev-log/62-etude/Readme.md` records the four environments' results, the machine chosen and why, the budget measured, and the native coverage on the Lenovo.
