# Milestone 34: how the machine speaks, and how it is checked

## The goal

Two pieces of work that share a cause, done in one milestone with nothing deferred.

The kernel gets one way to say things: a logging call with a severity and a subsystem name, four fixed destinations that decide for themselves what they show, and a separate call for the debug shell's interactive output, which is not a log. Every one of the roughly 680 places that print today is read and converted or reclassified, one file at a time.

The test system gets moved out of the product build and made small enough to read: the suites live under `tests/system/`, the runner is under 150 lines, the registry carries only what varies, and every probe in the tree is either run by something or gone. The four suites that cannot fail are made to fail for the right reasons, the coverage that is missing is written, and a core tier under two minutes becomes what `make test` runs.

At the end the milestone record states plainly what a passing suite proves and what it does not.

This plan is the authority for the run. A conflict between this plan and the current tree is resolved by the decisions here; a conflict between this plan and `CLAUDE.md` is resolved by `CLAUDE.md`.

## Why these two together, and in this order

The test findings in `findings.md` keep running into the same wall: the kernel prints `[FAIL]` in 23 places and nothing reads it, `subset` cannot tell whether a device was skipped, the shutdown line went to the screen only. Each is a symptom of there being no single stream with severity that a test can watch. Fixing the tests properly means fixing the messaging first, and fixing the messaging changes what the tests assert on. So neither can be finished without the other, and the order is decided by which changes are safe.

The test system restructure is a pure refactor: every suite passes before and after, asserting the same text. It goes first, so that when the messaging work starts changing what is printed where, the test assertions it must update live in one place rather than scattered through a build file. The messaging work goes second and produces the record format. The test fixes go third because the four broken oracles are fixed with that format, and the new coverage logs through it. The core tier goes last because it is chosen from the suite set as it finally stands.

Pruning is the one part of the test work done early: a dead probe deleted before the migration is a file that never needs reading.

## Decisions on the messaging

The design came out of a discussion recorded in `findings.md`. These are the decisions.

### One call for records

```c
klog(KLOG_ERR, "ahci", "command: timed out on port %d", port);
```

Five levels: `KLOG_CRIT`, `KLOG_ERR`, `KLOG_WARN`, `KLOG_INFO`, `KLOG_DEBUG`, with the conventional syslog numbers 2, 3, 4, 6, 7. `EMERG`, `ALERT` and `NOTICE` are not offered because nothing here would treat them differently, and a level that changes nothing is a level nobody chooses on purpose. The subsystem name is a required lowercase literal; the logger writes `subsystem: ` itself, and the format begins with `event: detail`, which is the shape `CLAUDE.md` already requires. That makes the shape a property of the call rather than a convention.

Each call is formatted into a stack-local record of at most 256 bytes, ends in exactly one newline, and marks its own truncation. One call is one record. The record is appended to the ring before any external destination sees it, so a failed serial port or Ethernet path cannot erase the machine's own copy.

### One call for the console

```c
console_printf("Type 'help' for the command list\n");
```

The kernel debug shell prints menus, prompts and command output. Those are a user interface, not diagnostics, and today they reach the network because the terminal writes through the serial port and netconsole taps it. `console_*` writes to the screen and the serial port and nowhere else. It never enters the ring and never leaves the machine. This distinction is the reason the migration cannot be mechanical: each call site has to be read to know which it is.

### The record

```text
<3>[12.34] ahci: command: timed out on port 0
```

The `<priority>` prefix is the level with the kernel facility implied; it is what BusyBox's `klogd` and every syslog daemon already parse, so a later userland logger needs no change on the kernel side. The bracket is uptime in seconds and hundredths, from the counter the kernel already keeps without the 497-day wrap; it says when during boot a thing happened and it needs no clock. Calendar time is not the kernel's business and is left to whatever stores or forwards the record later.

No facilities: a kernel is one facility, and the subsystem string is the classification that means something. No RFC 3164 or 5424 in the kernel: those are wire formats for a daemon that does not exist here. No writable `/dev/kmsg`. No runtime filters, rate limiting, duplicate suppression or binary format.

### The four destinations

* The ring, 16 KiB, always on, every level. Whole records are evicted oldest first, so the ring never begins in the middle of a line. `/dev/bootlog` hands it out unchanged and `/bin/dmesg` stays its reader; what changes is that the content is kernel records rather than every byte that crossed the serial port.
* The serial port, every level, when a UART exists.
* Netconsole, every level, when `rach2.netconsole` was asked for. It keeps its EtherType, header, boot id, sequence, checksum, broadcast address, early buffer and recursion guard, all of which `netconsole_hosttest.c` and `pcapcheck` already hold it to. What changes is that it takes records from the logger instead of bytes from the serial tap. It exists because it works below IP, so it can report a broken address configuration, which nothing addressed can; syslog over UDP is a possible later addition and not a replacement.
* The screen, `INFO` and above, coloured by level, without the priority prefix and timestamp because those are noise to a person standing in front of the machine. The boot narrative stays on the screen. For a teaching kernel it is part of the explanation, and a quiet screen would let a failed network probe erase the only place its diagnostic could appear. Register dumps and probe detail become `DEBUG` and leave the screen.

These four are a fixed table, not a subscriber registry. There are four; a list is clearer than a mechanism.

### Panic

`k_panic` emits one `CRIT` record through the logger first, then draws its screen. Today it prints through the screen API only and one caller compensates by printing to serial first.

### What stays out, and why it is not a deferral

BusyBox's `syslogd`, `klogd`, `logger` and `logread` are built and staged but cannot run on this kernel. `klogd` calls `klogctl`, which does not exist; `syslogd` needs an `AF_UNIX` datagram socket at `/dev/log`, and the kernel accepts only `AF_INET` and `AF_PACKET`; `logread` needs System V shared memory; `/var/log` is not in the image; and the two daemons would take two of sixteen process slots. Making them run is a feature with its own design, beginning with Unix-domain sockets, and the owner has said it is not wanted now. The record format above is chosen so that when it is wanted, the kernel side is already right.

## Decisions on the test system

### Where the suites live

Every `run-*` recipe, every test-only variable and every test tool build rule moves from the root `Makefile` to `tests/system/Makefile`. The root keeps the product: the kernel, the userland, the three bootable images, the validators, `clean` and `distclean`, and one `test` target that runs the core tier. A shared `mk/paths.mk` names the build layout so both makefiles agree on where `build/rach2/kernel` is without either including the other.

Suite targets stop listing product prerequisites. `tests/run` builds the product through the root `make` and the test tools and fixtures through `tests/system/Makefile` before any suite runs, so a suite target names only its own fixtures and the tools it invokes. The ten targets that each repeat a fourteen-word prerequisite list lose it.

`tools/verify/check.sh` stops compiling `runtests.c` on its own and asks the test makefile for it.

`tests/Makefile`, which builds the guest probe programs staged into the product image, stays what it is. Probes are part of what the machine ships; suites are not.

### The registry

Three fields: `name|tools|tier`. The make target was always `run-` plus the name and the prerequisites field has held `-` since it was introduced, so both go. The last field held `yes` for every suite and so encoded nothing; it becomes the tier, `core` or `full`. `tests/run core` runs the core tier and `tests/run all` runs everything.

Schedule order is registry order, longest first, stated in the registry's comment. The runner stops keeping a durations file and sorting by it; that machinery bought about thirty seconds and a person can put the long suites at the top by hand.

### The runner

Under 150 lines. It keeps the pool, because that is the speedup; the `PATH` shim, because that is how a leaked machine is named to its suite while twenty others run; and the several-names form, because that is how a person runs what their change touched. It validates `RACH2_WORKERS`, checks every suite's tools before it builds anything, and accounts for the `e1000` suite's inner jobs. Every mechanism that remains is named in `tests/system/Readme.md` with its reason, and one that cannot justify itself in a sentence is removed.

### The tools

Seven in-house C programs, 3181 lines, no external dependency: `harness`, `runtests`, `pcapcheck`, `ptydrive`, `mkfixture`, `deadline`, `holdport`. Each is read and its reason confirmed; none is expected to go, since each does one job nothing else does and replacing any with an external tool would add a dependency. The external runtime tools stay what they are: QEMU, `ssh`, four mtools commands for the GRUB disks, and three style checkers.

### Every probe runs or goes

The tree has 106 guest probe sources. 63 run in the default boot and about ten in dedicated suites. Of the rest, one is an empty file and the others are staged into every image and run by nothing. Each is read; each is then given a suite or deleted, and `notes.md` records which and why. `winprobe`, which reproduces a kernel panic that happened, runs. `budgetmusl`, which can pass with both its sockets failed, goes.

### The four oracles

* Boot self-tests: each emits one record and the group ends with `selftest: result: passed: N` or `failed: K of N`. The probe runner requires the summary and rejects any `ERR` or `CRIT` record between the first line and the shell prompt. It does not reject later error records, because the fault probes that follow produce them on purpose.
* `subset`: each device probe logs `skipped: rach2.noX` when told to skip and `OK` when it starts. For each option the suite requires the skipped record and forbids the started one.
* `reset`: the harness reports whether the machine exited on its own or was killed at the deadline, and the suite requires an exit. A kernel that announces the restart and then hangs fails.
* `addressed`: a probe reads the interface address through `SIOCGIFADDR` and prints it; the suite passes an address that is not the compiled default and requires the probe to report that one.

Each is proved by a mutation that makes the kernel misbehave in the way the oracle exists to catch.

### The coverage that is missing

Guest probes, each on its own boot where it needs one:

* `procfull`: fork until the table refuses, count the slots, reap one, fork again.
* `fdfull`: open until `EMFILE`, count, close one, open again.
* `sockfull` and `tcbfull`: the socket and TCP pools, the same shape.
* `oomprobe`: a child maps memory until `ENOMEM`; the parent survives and a later allocation works.
* `diskfull`: fill a small KRFFS, require `ENOSPC` from create, grow and truncate, delete, allocate again.

Host tests with a software model, in the shape `e1000_hosttest.c` established:

* `ahci_hosttest`: the HBA's reset, ownership, link, timeout, fatal-status and port-recovery paths. `ahci.c` gets the register indirection the two network drivers have if it lacks one. Milestone 10's record says this driver's worst defects were in paths QEMU never reaches.
* `tcp_hosttest`: `tcp.c` driven with crafted segments through a stub below it: loss, duplication, reordering, retransmission exhaustion, `SYN`, `FIN` and `RST` in every state, zero-window persistence, backpressure under close. Milestone 12 found its most serious defects in the closing states.
* `frame_hosttest`: malformed Ethernet, IPv4, UDP and TCP frames through the parser chain under the sanitizers.

Audio: a boot with QEMU's `wav` audio backend, a tone played through the debug shell, and a small host program that reads the file and requires energy at the tone's frequency. Today the only audio test proves a boot with `rach2.noaudio` reaches a shell.

Every new test is proved by a mutation before it is believed.

### The core tier

Chosen at the end, by two rules: every kernel subsystem is exercised at least once, and the tier runs under two minutes through the pool. `style` and `host` are in it. `make test` runs it. The full set is what a change to a shared piece runs and what precedes a commit that touches more than one subsystem.

## How the work is kept track of

Five files beside this plan, all tracked, all committed at each checkpoint:

* `progress.md`: every item in this plan as a checklist. One glance answers how far along the milestone is. Updated in the commit that completes an item.
* `current.md`: the resume state only. The active phase, the item in progress, the evidence already gathered, the exact next action. Replaced, never appended.
* `notes.md`: deviations, measurements, defects found and what was done about them, and every decision this plan left to the run.
* `messaging.md`: the migration ledger. Every file that prints, found by the tool, then read in full; every call site classified as record, console or test protocol by reading, with its level and subsystem; converted yes or no. It is the record of the file-by-file reading, and it is what makes the migration resumable after a compaction.
* `findings.md`: what the reviews found before the run. Read only.

After every phase, every compaction recovery and every batch of commits, one line is reported: `M34: phase N of 8; X of Y items done; tree clean|dirty`.

## Standing rules for the run

* Work directly. Reading a file, deciding what each message is, and writing the conversion require one judgement; none of it is delegated. The only delegation is an independent review, one reviewer per diff, at most six files, at most ten minutes. Each phase ends with one such review of its highest-risk files. Phase 4 touches more than six files and gets one review per batch of six, with the count of batches stated before the first starts. The total for the milestone is at most twelve reviews.
* Commit in scoped units in the house style, directly on the branch, no attribution, never push.
* Every commit passes `tests/run style` and `tests/run host`. A commit that touches a subsystem runs that subsystem's suites singly. The whole set runs at the end of each phase and nowhere else; it costs eight minutes, and the phase is the unit it should judge.
* A test is proved by making it fail. A mutation is driven from a script that restores the source in a trap and rebuilds before the verdict is believed.
* A change that alters a contract rewrites the owning Readme in the same commit.
* Nothing is deferred. A thing discovered mid-run that the plan did not foresee is written into `notes.md` and given a home in a later phase or closed where it is found. Phase 8 reads `notes.md` from the top and closes every open line or states why it cannot be closed here.
* Messages follow `CLAUDE.md`. During the migration, every converted message keeps the exact text a test asserts on until that test is changed in the same commit.

## The phases

### Phase 1: arrival

Read `AGENTS.md`, `CLAUDE.md`, `findings.md` and this plan in full. Require a clean tree and record `git log --oneline -12`. The baseline is milestone 33's warm run, 465 seconds, 44 suites; adopt it. Create `progress.md`, `current.md`, `notes.md` and `messaging.md` in the first commit.

### Phase 2: the test system

Move the suites out of the root `Makefile` into `tests/system/Makefile` with `mk/paths.mk` shared, drop the product prerequisites from every suite target, and move the test tool rules with them. Reduce the registry to three fields and the runner to under 150 lines. Fix the three stale document references and delete `tests/echoprobe.c`. Then read every probe that nothing runs and give each a suite or delete it. Replace `procfiles`' three full `procprobe` boots with a small probe that reports the mount source.

Proof: every suite passes singly; `make` at the root builds no test tool; the root `Makefile`'s diff is removals and one `test` target; the sixteen-suite batch from milestone 33 passes together; the runner's leak attribution still names `crash` when its defect is put back.

### Phase 3: the logger

`rach2/sys/klog.c` and `klog.h`: the call, the record formatter, the ring, the four destinations, `console_*`. A host test proves the formatter, the truncation mark, the ring wrap on whole records, the level filtering and the priority prefix. No caller is converted. The logger reaches the ring and netconsole through the existing serial fan-out for now, so nothing observable changes.

### Phase 4: the migration

Build `messaging.md`: the tool lists every file with a print call, and then each file is read in full and every call is classified before any is converted. Convert in this order, each in its own commit or small group: panic; the boot self-tests with their summary record; shutdown; the boot sequence in `kernel.c`; then storage, memory, processes, interrupts, audio and network, one component at a time; then the debug shell to `console_*`. Each component's Readme is updated in the commit that converts it.

Proof: after each commit the suites that assert on that component's text pass, either unchanged or updated in the same commit with the reason.

### Phase 5: the destinations

Netconsole takes records from the logger; the serial tap and the boot-log capture inside `serial_putc` go; the ring, serial and netconsole render the full record and the screen renders without prefix and timestamp; the legacy public print calls are removed and the screen and UART byte writers become private to their drivers.

Proof: `pcapcheck` holds netconsole to its unchanged frame contract; a new check shows that a character typed at the console is no longer broadcast; `/dev/bootlog` holds records; a repository search finds no caller of the removed calls.

### Phase 6: the oracles

The four fixes above, each with its mutation. The harness gains what two of them need: a report of whether the machine exited or was killed, and a rejection of records at or above a level within a bounded interval.

### Phase 7: the coverage

The six guest probes, the three host models, and the audio check, each with its mutation. `ahci_hosttest` and `tcp_hosttest` are the largest single pieces of new code in the milestone and are sized as such in `progress.md`.

### Phase 8: the tier and the record

Choose the core tier by the two rules and measure it. Point `make test` at it. Read `notes.md` from the top and close every open line. Measure cold from `make distclean` and warm. Write `dev-log/34-messages/Readme.md` with the trust statement: what a passing run proves, subsystem by subsystem, and what it does not. Add `34-messages` to `dev-log/Readme.md` in the same commit. The tree is clean and nothing is pushed.

## Recovery after a compaction

Read `AGENTS.md`, `CLAUDE.md`, this plan's decisions and the current phase, `progress.md`, `current.md` and `notes.md`; in phase 4, `messaging.md` as well. Run `git status --short --branch` and `git log --oneline -15`, inspect every uncommitted path, and resume the current phase's first unfinished item. Report the one line.
