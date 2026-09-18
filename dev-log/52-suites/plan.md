# Milestone 52: the suites

## The goal

A test set that a refactor can be trusted to: every suite passes on every machine it is run on, the same way each time, because nothing in it measures the clock, waits a number of seconds, or shares a file with another suite. The audit that follows rewrites the tree against this set, so this milestone comes first. The milestone is complete when `tests/run all` passes three times in a row under the emulator and three times under KVM on the testing host with no failure and no suite run alone, when every pass criterion in the set is a count, an order or an equality the kernel reports, when a mechanical check refuses the forms this milestone bans, and when the set's cost is recorded with the reason for every suite over five minutes.

## What the tree settles before this plan

* On 2026-09-18 the set met two new machines. Under the emulator on the testing host, 84 of 87 suites passed once the host's toolchain faults were fixed; the three that failed were diskrate, whose bounds were seconds measured on the Mac, mount, whose held-file step let a fork race an unmount, and imagerace, which found a Makefile fault. Under KVM on the same host eight failed: queue, ssh, ssh-e1000, exitline, audio, mount, conload and fatwrite, then loopdev and installer-repair at the runner's silence limit.
* The failures under KVM are of three kinds. Times written for one machine: `tests/ahciprobe.c` holds the fast reader under 200 ms and one read under 60 ms, `tests/conloadprobe.c` holds stage A under 3 s, `tests/lagprobe.c` holds deadlines in windows of 1.2, 5 to 6.9 and 4.5 to 6.9 s with 80 ms floors, `tests/loadprobe.c` holds latencies under 0.2, 5 and 1 s, `tests/loopprobe.c` a factor and a slack, `tests/timeprobe.c` 130 ms and 2 s, `tests/pingtest.c` 3 to 8 s and 2 s, and `tests/system/suites/ssh.sh` wants five connections established. Synchronization by waiting: nineteen suites `sleep`, `exitline.sh` stopped the machine on the probe's exit line and read the kernel's next line from the log, and `mount.sh` forked a sleeper to hold a file and let the unmount race it. Kernel faults the emulator hides: the serial port's interrupt stops after the first under an edge-triggered controller whose UART is refilled as fast as it is drained, the HD Audio controller counts no interrupts under KVM and `audio test scale` never prints `Done.`, and the ATA driver moves data by port I/O and polls the status port after every sector it writes, entry 97 of milestone 50's list, so a 64 MiB write costs 236 s under nested KVM and 25 s emulated.
* diskrate was reworked that day to judge each row against a baseline the same boot measures, reads against the raw read and writes against the exFAT write, with room recorded from both hosts; it is the model for what a multiple can do and the proof that a multiple is still a bet: the room is the odds.
* The kernel already counts what several proxies need: `/proc/disk` has disk commands, cache hits, flush and read bytes, `/proc/stat` has interrupt drains against tick drains, ring and queue high-water marks, uart bytes and overruns, and latency lines, `/proc/interrupts` counts per vector. What it does not count: polls per command and ticks waited per request in the block layer and the drivers, deadline releases with their tick, and a round trip's ticks in the socket path.
* The runner runs `style` and `host` serially, `usb`, `diskrate`, `install` and `imagerace` alone after the pool, and stops a suite after `RACH2_IDLE_TIMEOUT` seconds of silence, 300 by default; the silence limit is never scaled and its self-test asserts so. The emulated set on the testing host costs 9417 suite-seconds; grow 714, sealed 628, selfbuild 530, installer 477, account 406, installer-repair 395, diskrate 393, selfbuild-ext2 389, large 386, dhcp 355, installer-rehearse 337 and runner 295 are two thirds of it. The four installer suites and the two self-builds rehearse one procedure six ways; eight suites exist twice, once per network card.
* The serial fix, the handler dropping and setting the UART's enable bit when data remain so the controller sees an edge, and the exitline fix, waiting for the hand-back line before the stop, are in the tree unproved: the proof is conload and exitline under KVM and under the emulator on the testing host.

## Decisions

### A suite asserts on what the kernel counts, never on the clock

Time is never a pass criterion and never a synchronization. What a timed row was a proxy for is counted by the kernel and asserted exactly: a spin or a wait in a driver is polls per command and ticks slept per request; a reader not held behind a slow disk is its completion ordered before the slow one's and the completions on the controller's vector; a deadline is the kernel's tick on the log line, the waiter released on the same tick as the quarantine; console input under load is bytes drained through the interrupt against the tick; a sleep's correctness is ticks, with its lower bound exact; a flood is the count the server reports, equal, not a floor; a round trip that must not wait for the timer is fewer ticks than round trips. Where a proxy has no counter, the counter is added to the kernel, read through `/proc`, and given a host test; the expected value is recorded from the emulator and from KVM on the testing host, in the suite beside the assertion. Time survives only as the run's end: generous, scaled by `RACH2_TIMEOUT_FACTOR`, and a failure to be explained, never a pass. A multiple of a measured baseline, diskrate's form today, is the last resort for a row whose proxy cannot be counted, and each such row is listed with the reason.

### A suite waits for a token, never for a number of seconds

A suite waits for what the guest prints, under a completion limit, and never stops a machine on one line to read the next from its log. `sleep N` and `wait=N` leave the vocabulary of the suites and the harness; a deliberate interval, a load applied for a period, is marked `# interval:` with its reason and listed in a reasons file. A race is settled in the guest with an order the guest enforces, the shell opening the file before the sleeper is forked, and never with a delay on the host.

### The standard is enforced

`tests/run style` rejects a `sleep` with a number in a suite, a `wait=` harness step, and a millisecond or second constant compared in a probe, unless the line carries a reason the checker knows; the reasons file is reviewed by a person and its every entry names what the interval is for. The checker is proved by mutation like every checker here.

### Pooled suites share nothing

Every image, socket, port, fixture and temporary a suite makes carries the suite's name and the builder's process number; each suite declares what it touches, and the runner refuses a suite that reaches an artifact it did not declare or one another suite declared. The declarations are the inventory of the pool, read by a person once and by the runner every time.

### No suite runs alone

The lone list exists because four suites cannot stand a loaded host, and a suite that cannot stand load is one whose criteria are times or whose state is shared: `usb` waits for the emulator's monitor to answer, `diskrate` measured seconds, `install` builds under a completion bound, `imagerace` touches the shared images. Each is made to stand in the pool by removing what made it fragile, `usb` by waiting for the guest's own report of a device's arrival and removal, `diskrate` by keeping its counts, `install` by having no time criterion, `imagerace` by mutating targets of its own, and the list is deleted from the runner so it cannot fill again. `style` and `host` keep their place before the pool because they rebuild the kernel with their own flags.

### Hung means idle

The runner stops a suite whose machine has consumed no processor time and printed nothing over its interval, not one that has been silent. A slow host writes for an hour without a word; a dead one burns nothing. The silence limit and its self-test are replaced by an activity limit and its self-test.

### Every suite's cost is justified or cut

A suite's header states what only it proves and what that proof costs, and a reviewer reads the two against each other. A row that proves what a shorter row of another suite already proves is cut; two suites that prove one contract with two device models or two disk layouts are one suite with a loop; the four installer suites and the two self-builds are examined first, since they are the same procedure rehearsed six ways. What survives is the set, and its total is recorded in this milestone's Readme with the reason for each suite over five minutes.

### The kernel faults KVM showed are fixed here

The serial interrupt lost under an edge-triggered controller, the HD Audio controller silent under KVM, and the ATA driver's polling are kernel faults and are fixed in this milestone, because the KVM proof needs them and because real hardware with those devices behaves as KVM does, not as the emulator does. The ATA driver waits for the controller's interrupt, or moves data by bus-master DMA, one request per transfer, and its polls per command become a counter the suites read.

### Two machines or it is not done

A suite is accepted when it passes under the emulator and under KVM on the testing host, three times each, with its counts recorded from both. A failure on either is a defect of the suite or a fault in the kernel and is fixed, never attributed to the machine.

## Phases

### Phase 1: the counters and the kernel faults

Polls per command and ticks waited per request in the block layer and the ATA, AHCI and USB drivers; deadline releases with their tick; a round trip's ticks in the socket path; each read through `/proc`, each with a host test. The serial edge fix and the exitline fix proved. The HD Audio fault found and fixed. The ATA driver made interrupt-driven or DMA, proved by diskrate under KVM inside the runner's limits. Proof: the host tier, and conload, exitline, audio, queue and diskrate under both accelerators.

### Phase 2: the harness and the checker

The harness keeps one waiting primitive; `wait=` is removed. The checker in `style` with its reasons file and its mutation. The runner's activity limit and its self-test; the lone list deleted; the declarations of what each suite touches and the runner's refusal. Proof: `tests/run runner` and the checker's mutation.

### Phase 3: the probes

Every probe in the inventory rewritten to the counted or ordered form, one commit each, the expected values recorded from both accelerators. Proof: each suite under both, three times.

### Phase 4: the suites

Every suite that sleeps or thresholds rewritten, one commit each; the four lone suites made to stand in the pool. Proof: each suite under both, three times, in the pool.

### Phase 5: the cuts

The cost review, the merges of the installer family, the self-builds and the per-card pairs, the rows cut, the survivors' cost recorded. Proof: the set under both accelerators three times, and its total in the Readme.

### Phase 6: the close

Once: build from clean on the testing host; `tests/run all` under the emulator and under KVM, three times each; everything that comes up fixed; the milestone's diff to Codex in units of six files; the findings fixed; the sets again. The Readme records the commit, the judges' versions, the set's cost with each suite's reason, and the counts recorded from both machines.

## Risks

* A counter added to a hot path costs what it measures; each is a plain increment on a path that already touches the structure, and the host tier's timing rows say if it is not.
* Making the ATA driver interrupt-driven touches the boot path of every image and the Lenovo's disk; it is proved under both accelerators and on the machine's own controller before the milestone closes, through a kernel install with `bootctl`, never a root write.
* A merged suite that fails names a stage, not a suite; the stage names must be as exact as the suite names were.
* The activity limit can call a machine hung that is waiting on the network; the interval is long and the limit reports what it saw.
* A row cut as redundant may have been the only one that caught a fault; each cut names the row that still proves it, and the reviewer checks that row.
