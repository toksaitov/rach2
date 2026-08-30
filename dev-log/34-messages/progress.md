# Milestone 34 progress

Every item in `plan.md` as a checklist. `[ ]` open, `[x]` done, `[-]` closed by
a decision recorded in `notes.md`. Updated in the commit that completes an item.

## Phase 1: arrival (4 items)

* [x] Read `AGENTS.md`, `CLAUDE.md`, `findings.md`, `plan.md`
* [x] Clean tree, `git log --oneline -12` recorded in `notes.md`
* [x] Baseline adopted: milestone 33 warm run, 465s, 44 suites
* [x] `progress.md`, `current.md`, `notes.md`, `messaging.md` created

## Phase 2: the test system (12 items)

* [x] Suite recipes, test-only variables and tool rules moved to `tests/system/suites.mk`
* [x] Root `Makefile` includes it with one line
* [x] Test tools dropped from the root `all` target
* [x] `tools/verify/check.sh` asks make for `runtests` instead of compiling it
* [x] Registry reduced to `name|tools|tier`
* [x] `-e1000` registry entries added; `e1000` keeps the three selection boots
* [x] Runner rewritten: pool, PATH shim, several names, tool check before build, `RACH2_WORKERS` validated
* [x] Runner self-test: registry parser, pool failure reporting, leak attribution
* [x] `tests/system/Readme.md` names every runner mechanism with its reason
* [x] Three stale document references fixed; `tests/echoprobe.c` deleted
* [x] Every orphaned probe read, then given a suite or deleted (`notes.md` records which)
* [x] `procfiles` mount-source probe replaces three full `procprobe` boots
* [x] `addressed` becomes a single-probe address boot

Proof items:

* [x] Every suite passes singly
* [x] `make distclean` then `tests/run authority` builds from nothing
* [x] A touched kernel source rebuilds the kernel before the next suite runs it
* [x] `make` at the root builds no test tool
* [x] Root `Makefile` diff is removals and one `include`
* [x] The sixteen-suite batch passes together
* [x] Runner self-test passes
* [x] Leak attribution names `crash` when its defect is restored
* [x] Review 1 of 12

## Phase 3: the logger (10 items)

* [x] `klog.h`: levels, call signature, `console_*`
* [x] Record formatter: conversions, 256 bytes, truncation mark, one newline
* [x] Ring with whole-record eviction, 16 KiB
* [x] Interrupt bracket around the ring append
* [x] Destination table in order: ring, screen, serial, netconsole
* [x] Bounded serial transmit wait
* [x] `console_*` with its private raw UART path
* [x] Panic sequence: ring, screen, then bounded delivery
* [x] Host test: formatter, truncation, prefix, timestamp, eviction at wrap, each filter, destination order under a wedged UART, `console_*` absent from the ring, no torn record from interrupt context
* [x] Review 2 of 12

## Phase 4: the oracles (10 items)

* [x] Boot self-tests emit one record each plus the summary
* [x] Device probes emit unique `started` and `skipped` records
* [x] `k_panic` converted
* [x] Harness: exit-or-killed report
* [x] Harness: bounded-interval level rejection
* [x] `subset` fixed, mutation proves it
* [x] `reset` fixed, mutation proves it
* [x] `addressed` fixed, mutation proves it
* [x] Self-test oracle fixed, mutation proves it
* [x] Review 3 of 12

## Phase 5: the migration (9 items)

* [x] `messaging.md` built: every printing file listed by tool, then read in full, every call classified before conversion
* [x] Group 1: boot and shutdown in `kernel.c` (review 4)
* [x] Group 2: storage (review 5)
* [x] Group 3: memory and processes (review 6)
* [x] Group 4: interrupts, timers, rest of `sys/` (review 7)
* [x] Group 5: audio (review 8)
* [x] Group 6: network (review 9)
* [x] Debug shell converted to `console_*`
* [x] Each component's Readme updated in the converting commit

## Phase 6: the cutover (9 items)

* [x] Netconsole takes records from the logger
* [x] Serial tap and boot-log capture removed from `serial_putc`
* [x] `/dev/bootlog` serves the logger's ring
* [x] Full record rendered to ring, serial, netconsole; screen without prefix and timestamp
* [x] Legacy public print calls removed; byte writers private to their drivers
* [x] `pcapcheck` reassembles one complete record from a capture
* [x] Console keystrokes absent from a netconsole capture
* [x] `/dev/bootlog` holds records and no console text
* [x] Review 10 of 12

## Phase 7: the coverage (13 items)

* [x] Refusal errors read from the code and recorded in `notes.md`
* [x] `procfull` + mutation
* [x] `fdfull` + mutation
* [x] `sockfull` (two processes) + mutation
* [-] `tcbfull` + mutation (dropped; `notes.md` and `plan.md` say why)
* [x] `oomprobe` (touch pages, child dies, parent recovers) + mutation
* [x] `diskfull` + the `EIO` to `ENOSPC` fix + mutation
* [x] `ahci.c` gains an injectable register interface
* [x] `ahci_hosttest`: every matrix row a checklist line
* [x] `tcp_hosttest`: every matrix row a checklist line
* [x] `packet_hosttest`: malformed Ethernet, IPv4, UDP, TCP
* [ ] `wavcheck` + the audio boot + two mutations
* [ ] Review 11 of 12

## Phase 8: the tier and the record (8 items)

* [ ] Core tier chosen by the two rules
* [ ] `make test` runs style, host, core; measured
* [ ] Full run measured with registry order and with durations restored
* [ ] `notes.md` read from the top; every open item closed or removed by owner decision
* [ ] Cold from `make distclean` and warm measured
* [ ] `dev-log/34-messages/Readme.md` with the trust statement
* [ ] `34-messages` added to `dev-log/Readme.md` in the same commit
* [ ] Review 12 of 12
