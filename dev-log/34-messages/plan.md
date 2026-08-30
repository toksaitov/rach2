# Milestone 34: the kernel log and the test system

## The goal

Two pieces of work with one cause, done in one milestone with nothing deferred.

The kernel gets one logging call with a severity and a subsystem name, four fixed destinations that each apply their own level filter, and a separate console call for the debug shell's interactive output. Every one of the roughly 670 places that print today is read, classified and converted, one file at a time.

The test system leaves the product build and becomes small enough to read: the suite recipes live under `tests/system/`, the runner is plain POSIX shell with every mechanism justified in its Readme, the registry carries only what varies, and every probe in the tree is either run by a suite or deleted. The four suites whose checks cannot fail are made to fail for the right reason, the coverage that is missing is written, and a core tier that runs in under two minutes becomes what `make test` runs.

The milestone record ends with a trust statement: what a passing run proves, subsystem by subsystem, and what it does not.

This plan is the authority for the run. A conflict between this plan and the current tree is resolved by the decisions here; a conflict between this plan and `CLAUDE.md` is resolved by `CLAUDE.md`. The plan was revised once after an independent review; `findings.md` records the evidence it was written from.

## Why these two together, and in this order

The test findings share a cause. The kernel prints `[FAIL]` in 23 places and no test reads it; `subset` cannot tell whether a device probe was skipped; the shutdown message reached the screen and not the serial port. In each case there is no single stream with severity for a test to read. The proper test fixes need the logger, and the logger changes what the tests assert on, so the two are one milestone.

The order is decided by what is safe at each step:

1. The test system restructure first. It is a pure refactor: every suite asserts the same text before and after. Done first, the assertions the logger will later change are in one place.
2. The logger second, as infrastructure with no callers, so it can be proved in isolation.
3. The four broken oracles third, before the bulk migration. Two of them need no logger at all, and the other two need only the self-tests and the device probes converted. Converting 670 print sites while four checks cannot fail would leave the migration unguarded exactly where guarding matters.
4. The migration fourth, protected by working oracles.
5. The destination cutover fifth, once no caller remains on the old path.
6. The new coverage sixth, logging through the finished logger.
7. The core tier last, chosen from the suite set as it finally stands.

Pruning is the one part of the test work done early: a dead probe deleted before the migration is a file that need not be read.

## Decisions on the logger

### One call for records

```c
klog(KLOG_ERR, "ahci", "command: timed out on port %d", port);
```

Five levels with the conventional syslog numbers: `KLOG_CRIT` 2, `KLOG_ERR` 3, `KLOG_WARN` 4, `KLOG_INFO` 6, `KLOG_DEBUG` 7. `EMERG`, `ALERT` and `NOTICE` are not offered because no destination here would treat them differently from their neighbours. The subsystem is a required lowercase string literal. The logger writes `subsystem: ` itself, and the format begins with `event: detail`, the shape `CLAUDE.md` already requires; the call enforces what was a convention.

One call is one record: formatted into a stack-local buffer of at most 256 bytes, ended with exactly one newline, with truncation marked by a trailing `...` when the text did not fit. Formatting uses the kernel's own `%d`, `%u`, `%x`, `%s` and `%c` conversions, which are what `serial_printf` supports today; no new conversion is added.

### One call for the console

```c
console_printf("Type 'help' for the command list\n");
```

The debug shell prints menus, prompts and command output. That is a user interface, not a diagnostic, and today it reaches the network because the terminal writes through the serial port and netconsole taps every serial byte. `console_*` writes to the screen and, through a private raw UART path, to the serial port. It never enters the ring and never leaves the machine. It exists from phase 3 with that contract in force, which is why the raw UART path is built in phase 3 and not later.

There are two classes of output and two calls. A message whose text is a test contract, such as the process-exit line the probe runner scores at the start of a line, is a record with a required substring; it is emitted through `klog` and its text is preserved. There is no third call.

### The record

```text
<3>[12.34] ahci: command: timed out on port 0
```

The `<priority>` prefix is the level with the kernel facility implied. It is the form BusyBox's `klogd` parses, so a later userland logger needs no kernel change. The bracket is uptime in seconds and hundredths from the counter the kernel already keeps without the 497-day wrap; before the timer starts it reads `0.00`. Calendar time is added by whatever stores or forwards the record later, not by the kernel.

No facilities: a kernel is one facility and the subsystem string is the classification that matters. No RFC 3164 or 5424 framing in the kernel. No writable `/dev/kmsg`. No runtime filters, rate limiting, duplicate suppression or binary format.

### The four destinations, in order

The order matters because a destination can fail.

1. The ring. 16 KiB, always on, every level. Whole records are evicted oldest first, so the ring never begins mid-line. `/dev/bootlog` hands it out unchanged and `/bin/dmesg` stays its reader; the content becomes kernel records instead of every byte that crossed the serial port. The ring is written first, before any other destination, so a failed serial port or Ethernet path cannot lose the machine's own copy.
2. The screen, `INFO` and above, coloured by level, without the prefix and timestamp, which a person in front of the machine does not need. The boot narrative stays on the screen: for a teaching kernel it is part of the explanation, and a quiet screen would remove the one place a failed network probe's diagnostic can appear when the network is what failed. Register dumps and probe detail become `DEBUG` and leave the screen.
3. The serial port, every level, when a UART exists, through a transmit wait bounded at a fixed count of polls so a wedged UART costs a bounded delay per record and never a hang.
4. Netconsole, every level, when `rach2.netconsole` was requested. It keeps its EtherType, header, boot id, sequence, checksum, broadcast destination, early buffer and recursion guard, all of which `netconsole_hosttest.c` and `pcapcheck` hold it to. It takes whole records from the logger instead of bytes from the serial tap. It stays because it works below IP and can therefore report a broken address configuration, which nothing addressed can do; syslog over UDP is a possible later addition, not a replacement.

The four are a fixed table in `klog.c`, not a subscriber registry.

The logger runs with interrupts as it finds them and takes no lock. A record from interrupt context and a record from a syscall may interleave in the ring only at record boundaries, because each record is formatted whole before the ring is touched and the ring append is a single copy with interrupts disabled around it.

### Panic

`k_panic` appends one `CRIT` record to the ring, draws its screen, and only then delivers the record to the serial port and netconsole through their bounded paths. Today it prints through the screen API only and one caller compensates by printing to the serial port first.

### What stays out, and why it is a decision rather than a deferral

BusyBox's `syslogd`, `klogd`, `logger` and `logread` are built and staged but cannot run on this kernel: `klogd` calls `klogctl`, which does not exist; `syslogd` needs an `AF_UNIX` datagram socket at `/dev/log` and the kernel accepts only `AF_INET` and `AF_PACKET`; `logread` needs System V shared memory; `/var/log` is not in the image. The owner has decided that userland syslog is not wanted now. The record format above is chosen so that when it is wanted, the kernel side is already right.

## Decisions on the test system

### Where the suites live

Every `run-*` recipe, every test-only variable and every test tool build rule moves from the root `Makefile` into `tests/system/suites.mk`, and the root `Makefile` includes that file with one line. The dependency graph stays whole, which is what keeps `tests/run authority` correct on a fresh clone: the metal, recovery, harness and sealed variants that only suites need are built by targets only suites depend on, and a standalone test makefile would have to rebuild all of them or miss one. The root `all` target stops building the test tools, so `make` at the root builds the product and nothing else; a suite target names the tools it invokes and builds them when run.

`tools/verify/check.sh` stops compiling `runtests.c` on its own and asks make for it.

`tests/Makefile`, which builds the guest probe programs staged into the product image, stays what it is. Probes are part of what the machine ships; suites are not.

### The registry

Three fields: `name|tools|tier`. The make target was always `run-` plus the name and the prerequisites field has held `-` since it was introduced. The last field held `yes` for every suite; it becomes `core` or `full`.

The second-card network runs become registry entries of their own, `ping-e1000` through `ssh-e1000`, tier `full`, so the pool schedules them beside their first-card twins; `e1000` keeps only the three card-selection boots. That removes the inner `make -j4` and with it the only suite whose cost the pool could not see, which is why the registry needs no weight field.

Schedule order is registry order, longest first, stated in the registry's comment. The durations file goes; phase 8 measures the full run with and without it and records the difference.

### The runner

Plain POSIX shell that a reader can follow top to bottom. It keeps the pool, the `PATH` shim that names a leaked machine to its suite, the several-names form, and the tool check, which moves ahead of any build. It validates `RACH2_WORKERS` as a positive integer. Every mechanism that remains is named in `tests/system/Readme.md` with its reason. A runner self-test covers the registry parser, the pool's failure reporting and the leak attribution. 150 lines is a review warning, not a gate; control flow is not compressed to meet a count.

### The tools

Seven in-house C programs, 2994 lines: `harness`, `runtests`, `pcapcheck`, `ptydrive`, `mkfixture`, `deadline`, `holdport`. Each does one job nothing else does and each is kept. The external runtime tools stay what they are: QEMU, `ssh`, four mtools commands for the GRUB disks, and three style checkers.

### Every probe runs or goes

106 guest probe sources; 63 run in the default boot and about ten in dedicated suites. Each remaining file is read and then given a suite or deleted, and `notes.md` records which and why. `winprobe`, which reproduces a kernel panic that happened, runs. `budgetmusl`, which passes with both its sockets failed, goes. `tests/echoprobe.c`, an empty file, goes.

### Duplication: two decisions and one change

`e1000`'s nine second-card runs and `stress-ahci` are kept. Both are pooled and neither is on the critical path, so removing them buys no time, and milestone 33's review showed the second-card runs exercise stack paths the host model does not. `addressed` becomes a single-probe boot: one probe reads the interface address and the suite requires it, rather than 63 probes that never look at it. `procfiles` gets a small probe that reports the mount source in place of three full `procprobe` runs.

### The four oracles, with their mutations

* Boot self-tests. Each emits one record and the group ends with `selftest: result: passed: N` or `failed: K of N`. The probe runner requires the summary and rejects any `ERR` or `CRIT` record between the group's first record and its summary, and nowhere else, because the fault probes that run later produce error records on purpose. Mutation: force one self-test false.
* `subset`. Each device probe logs a unique `started` record when it starts and `skipped: rach2.noX` when told not to. For each option the suite requires the skipped record and forbids that device's started record; the boot that disables every device at once is kept. Mutation: make one probe ignore its option.
* `reset`. The harness reports whether the machine exited on its own or was killed at the deadline. The suite requires an exit, requires the restart record, and forbids any `CRIT` record, so a crash that also ends the machine does not pass as a reset. Mutation: make every reset mechanism return without resetting.
* `addressed`. A probe reads the interface address through `SIOCGIFADDR` and prints it. The suite passes an address that is not the compiled default and requires that address in the probe's output; the expected string appears in no `send=` step, so the console echo of typed input cannot satisfy it. Mutation: make the kernel ignore the boot-line address.

### The coverage that is missing

Guest probes, on their own boots where they need one. Before any is written, the exact error each refusal returns is read from the code and recorded in `notes.md`; the probes assert those errors.

* `procfull`: fork until the process table refuses, count the slots, reap one, fork again. Refusal is `EAGAIN` as the code stands.
* `fdfull`: open until `EMFILE`, count, close one, open again.
* `sockfull`: two processes, because a process has sixteen descriptors and three are taken, so one process reaches `EMFILE` before the sixteen-slot socket pool is full. The parent forks, both fill sockets until the pool refuses, one closes one, the other allocates. Refusal is `ENOMEM` as the code stands.
* `tcbfull`: dropped. Both pools hold sixteen and every control block is owned by a socket, so the socket pool refuses first from userland; the one shape that would reach the control-block limit first is a connection to this machine's own listener, and a loopback handshake does not complete on this kernel (see `notes.md`). Control-block exhaustion is covered in `tcp_hosttest` instead, and the loopback handshake is recorded as a defect this milestone found and did not fix.
* `oomprobe`: a child maps memory and touches every page until physical frames run out. The documented behaviour is that the child dies with `SIGSEGV`; the probe asserts that, then asserts the parent's next allocation and touch succeed, which is the frame recovery.
* `diskfull`: fill a small KRFFS through a file, require `ENOSPC` from create and from write, delete, write again. The kernel today turns a failed KRFFS growth into `EIO` at the write syscall; that becomes `ENOSPC` in the same change, and the probe is what proves it. Truncation growth has no syscall and is tested natively.

Host tests with a software model, in the shape `e1000_hosttest.c` established:

* `ahci_hosttest`. `ahci.c` has plain read and write wrappers and no injectable interface; it gets one covering register access, PCI configuration reads, the delay, and DMA completion, so the model can change register values between reads. The model is driven through this matrix: HBA reset that completes, that never completes; BIOS handoff present and absent; port link up, down, and lost after start; a command that completes, that times out, that returns a fatal task-file status; a port that recovers after a fatal status and one that does not; a byte count short of the request. Each row names the record the driver must log and the state it must leave.
* `tcp_hosttest`. `tcp_rx`, `tcp_tick`, `tcp_send`, `tcp_connect`, `tcp_listen` and `tcp_close` are driven against a stub that captures every segment `ip_tx` would send and a stub socket layer that records every callback. The matrix: for each of the eleven states, the arrival of `SYN`, `FIN`, `RST`, and a data segment in order, duplicated, reordered and with a gap; retransmission until the seventh and the timeout that ends the connection; a zero window held and reopened; a close with the send ring full; a segment for a port nobody listens on. Each row names the state after, the segments emitted, and the callback made.
* `packet_hosttest`. Malformed Ethernet, IPv4, UDP and TCP frames through the parser chain under the sanitizers: short headers, wrong versions, bad lengths in every field, options past the end, fragments. The name is `packet_` because `frame_hosttest.c` already exists and tests the physical-frame allocator.

Audio. A boot with `-device intel-hda -device hda-duplex` and `-audiodev wav,id=audio0,path=...`, because the PC-speaker fallback cannot play samples. The debug shell command `audio test scale` plays five notes at 262, 294, 330, 349 and 392 Hz. A small host program, `wavcheck`, reads the file and requires energy at each of the five frequencies in order and silence outside them. Mutations: a kernel that plays silence, and one that plays the wrong frequency.

Every new test is proved by its mutation before it is believed.

### The core tier

System suites only, chosen at the end by two rules: every kernel subsystem is exercised at least once, and the tier runs in under two minutes through the pool. `style` and `host` stay outside the pool and outside the tier because they share a scratch directory and `style` rebuilds the kernel; `make test` runs `style`, then `host`, then the core tier, and the whole of `make test` is measured and recorded. The full set is what precedes a commit that touches more than one subsystem.

## How the work is kept track of

Five files beside this plan, all tracked, all committed at each checkpoint:

* `progress.md`: every item in this plan as a checklist. One glance answers how far along the milestone is. Updated in the commit that completes an item.
* `current.md`: the resume state only. The active phase, the item in progress, the evidence already gathered, the exact next action, and the file being read when the state was written. Replaced, never appended.
* `notes.md`: deviations, measurements, defects found and what was done about them, and every decision this plan left to the run.
* `messaging.md`: the migration ledger. Every file that prints, found by the tool, then read in full; every call site classified as record or console by reading, with its level and subsystem, and marked when its text is a test contract; converted yes or no. Written before each batch, not after.
* `findings.md`: what the reviews found before the run. Input evidence, not tracking state.

After every phase, every compaction recovery and every batch of commits, one line is reported: `M34: phase N of 8; X of Y items done; HEAD abc1234; tree clean|dirty`.

## Standing rules for the run

* Work directly. Reading a file, deciding what each message is, and writing the conversion require one judgement; none of it is delegated. The only delegation is an independent review, one reviewer per diff, at most six files, at most ten minutes. There are twelve reviews in this milestone and no more: one after each of phases 2, 3, 4, 6, 7 and 8, and one after each of the six component groups of phase 5. The count is fixed now; no result of one review starts another.
* Commit in scoped units in the house style, directly on the branch, no attribution, never push.
* Every commit passes `tests/run style` and `tests/run host`. A commit that touches a subsystem runs that subsystem's suites singly. The whole set runs at the end of each phase and nowhere else.
* A test is proved by making it fail. A mutation is driven from a script that restores the source in a trap and rebuilds before the verdict is believed.
* A change that alters a contract rewrites the owning Readme in the same commit.
* Nothing is deferred. A thing discovered mid-run that the plan did not foresee is written into `notes.md` with the phase that will do it. Phase 8 reads `notes.md` from the top; every open item is completed, or removed from scope by a decision of the owner recorded in `notes.md`. There is no third outcome.
* Messages follow `CLAUDE.md`. During the migration, every converted message keeps the exact text a test asserts on until that test is changed in the same commit.

## The phases

### Phase 1: arrival

Read `AGENTS.md`, `CLAUDE.md`, `findings.md` and this plan in full. Require a clean tree and record `git log --oneline -12`. The baseline is milestone 33's warm run, 465 seconds, 44 suites; adopt it. Create `progress.md`, `current.md`, `notes.md` and `messaging.md` in the first commit.

### Phase 2: the test system

Move the suite recipes, test-only variables and test tool rules into `tests/system/suites.mk` and include it from the root. Drop the test tools from the root `all` target. Reduce the registry to three fields and add the `-e1000` entries. Rewrite the runner to the rules above with its self-test. Fix the three stale document references and delete the empty probe. Read every probe that nothing runs and give each a suite or delete it. Replace `procfiles`' three full `procprobe` boots with the mount-source probe.

Proof: every suite passes singly; `make distclean` followed by `tests/run authority` builds what that suite needs from nothing; a touched kernel source rebuilds the kernel before the next suite runs it; `make` at the root builds no test tool; the root `Makefile`'s diff is removals and one `include`; the sixteen-suite batch from milestone 33 passes together; the runner self-test passes; the leak attribution still names `crash` when its defect is put back.

### Phase 3: the logger

`rach2/sys/klog.c` and `klog.h`: the call, the formatter, the ring with whole-record eviction, the four destinations in their order with the bounded serial wait, the interrupt bracket around the ring append, `console_*` with its private raw UART path, and the panic sequence. No existing caller is converted. The ring content is not yet what `/dev/bootlog` serves; that switch is phase 6.

Proof: a host test proves the formatter for every conversion and for truncation, the priority prefix and timestamp, whole-record eviction at the wrap, each destination's level filter, the destination order under a serial path that never becomes ready, that `console_*` output never reaches the ring, and that a record from a simulated interrupt does not tear one from the foreground.

### Phase 4: the oracles

Convert only what the four fixes need: the boot self-tests and their summary record, each device probe's started and skipped records, and `k_panic`. Add to the harness the exit-or-killed report and the bounded-interval level rejection. Then fix the four suites as decided, each proved by its named mutation.

Proof: the four mutations each fail their suite; a deliberate panic under the harness produces the `CRIT` record on the serial port and the panic screen afterwards; a boot log parsed end to end shows exactly one summary record.

### Phase 5: the migration

Build `messaging.md` first: the tool lists every file with a print call, then each file is read in full and every call is classified before any is converted. Convert in six component groups, each a review unit: boot and shutdown in `kernel.c`; storage; memory and processes; interrupts, timers and the rest of `sys/`; audio; network. Then the debug shell to `console_*`. Each component's Readme is updated in the commit that converts it.

Proof: after each commit the suites that assert on that component's text pass, unchanged or updated in the same commit with the reason; `/dev/bootlog` after a boot shows the converted component's records with their levels and subsystems and shows no debug-shell output; the screen filter is proved in the host test rather than live, since the harness cannot read the VGA buffer.

### Phase 6: the cutover

Netconsole takes records from the logger; the serial tap and the boot-log capture inside `serial_putc` are removed; `/dev/bootlog` serves the logger's ring; the ring, the serial port and netconsole render the full record while the screen renders without the prefix and timestamp; the public legacy print calls are removed and the screen and UART byte writers become private to their drivers.

Proof: `pcapcheck` holds netconsole to its unchanged frame contract and additionally reassembles one complete record from the capture; the early-buffer drop policy is unchanged in `netconsole_hosttest.c`; a character typed at the console during a netconsole capture does not appear in the capture; `/dev/bootlog` after a boot holds records and no console text; a repository search finds no caller of the removed calls.

### Phase 7: the coverage

The six guest probes, the three host models with their matrices, and the audio check, each with its mutation. The exact refusal errors are read from the code and recorded before the first probe is written. `ahci_hosttest` and `tcp_hosttest` are the largest pieces of new code in the milestone; each matrix row is a checklist item in `progress.md`.

### Phase 8: the tier and the record

Choose the core tier by the two rules and measure `make test`. Measure the full run with registry order and with the durations file restored, and record both. Read `notes.md` from the top and close every line. Measure cold from `make distclean` and warm. Write `dev-log/34-messages/Readme.md` with the trust statement. Add `34-messages` to `dev-log/Readme.md` in the same commit. The tree is clean and nothing is pushed.

## Recovery after a compaction

Read `AGENTS.md`, `CLAUDE.md`, this plan's decisions and the current phase, `progress.md`, `current.md` and `notes.md`; in phase 5, `messaging.md` as well. Run `git status --short --branch` and `git log --oneline -15`, inspect every uncommitted path, and resume the current phase's first unfinished item. Report the one line.
