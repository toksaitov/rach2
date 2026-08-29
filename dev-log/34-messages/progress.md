# Milestone 34 progress

Every item in `plan.md` as a checklist. `[ ]` open, `[x]` done, `[-]` closed by
a decision recorded in `notes.md`. Updated in the commit that completes an item.

## Phase 1: arrival (4 items)

* [x] Read `AGENTS.md`, `CLAUDE.md`, `findings.md`, `plan.md`
* [x] Clean tree, `git log --oneline -12` recorded in `notes.md`
* [x] Baseline adopted: milestone 33 warm run, 465s, 44 suites
* [x] `progress.md`, `current.md`, `notes.md`, `messaging.md` created

## Phase 2: the test system (12 items)

* [ ] Suite recipes, test-only variables and tool rules moved to `tests/system/suites.mk`
* [ ] Root `Makefile` includes it with one line
* [ ] Test tools dropped from the root `all` target
* [ ] `tools/verify/check.sh` asks make for `runtests` instead of compiling it
* [ ] Registry reduced to `name|tools|tier`
* [ ] `-e1000` registry entries added; `e1000` keeps the three selection boots
* [ ] Runner rewritten: pool, PATH shim, several names, tool check before build, `RACH2_WORKERS` validated
* [ ] Runner self-test: registry parser, pool failure reporting, leak attribution
* [ ] `tests/system/Readme.md` names every runner mechanism with its reason
* [ ] Three stale document references fixed; `tests/echoprobe.c` deleted
* [ ] Every orphaned probe read, then given a suite or deleted (`notes.md` records which)
* [ ] `procfiles` mount-source probe replaces three full `procprobe` boots
* [ ] `addressed` becomes a single-probe address boot

Proof items:

* [ ] Every suite passes singly
* [ ] `make distclean` then `tests/run authority` builds from nothing
* [ ] A touched kernel source rebuilds the kernel before the next suite runs it
* [ ] `make` at the root builds no test tool
* [ ] Root `Makefile` diff is removals and one `include`
* [ ] The sixteen-suite batch passes together
* [ ] Runner self-test passes
* [ ] Leak attribution names `crash` when its defect is restored
* [ ] Review 1 of 12

## Phase 3: the logger (10 items)

* [ ] `klog.h`: levels, call signature, `console_*`
* [ ] Record formatter: conversions, 256 bytes, truncation mark, one newline
* [ ] Ring with whole-record eviction, 16 KiB
* [ ] Interrupt bracket around the ring append
* [ ] Destination table in order: ring, screen, serial, netconsole
* [ ] Bounded serial transmit wait
* [ ] `console_*` with its private raw UART path
* [ ] Panic sequence: ring, screen, then bounded delivery
* [ ] Host test: formatter, truncation, prefix, timestamp, eviction at wrap, each filter, destination order under a wedged UART, `console_*` absent from the ring, no torn record from interrupt context
* [ ] Review 2 of 12

## Phase 4: the oracles (10 items)

* [ ] Boot self-tests emit one record each plus the summary
* [ ] Device probes emit unique `started` and `skipped` records
* [ ] `k_panic` converted
* [ ] Harness: exit-or-killed report
* [ ] Harness: bounded-interval level rejection
* [ ] `subset` fixed, mutation proves it
* [ ] `reset` fixed, mutation proves it
* [ ] `addressed` fixed, mutation proves it
* [ ] Self-test oracle fixed, mutation proves it
* [ ] Review 3 of 12

## Phase 5: the migration (9 items)

* [ ] `messaging.md` built: every printing file listed by tool, then read in full, every call classified before conversion
* [ ] Group 1: boot and shutdown in `kernel.c` (review 4)
* [ ] Group 2: storage (review 5)
* [ ] Group 3: memory and processes (review 6)
* [ ] Group 4: interrupts, timers, rest of `sys/` (review 7)
* [ ] Group 5: audio (review 8)
* [ ] Group 6: network (review 9)
* [ ] Debug shell converted to `console_*`
* [ ] Each component's Readme updated in the converting commit

## Phase 6: the cutover (9 items)

* [ ] Netconsole takes records from the logger
* [ ] Serial tap and boot-log capture removed from `serial_putc`
* [ ] `/dev/bootlog` serves the logger's ring
* [ ] Full record rendered to ring, serial, netconsole; screen without prefix and timestamp
* [ ] Legacy public print calls removed; byte writers private to their drivers
* [ ] `pcapcheck` reassembles one complete record from a capture
* [ ] Console keystrokes absent from a netconsole capture
* [ ] `/dev/bootlog` holds records and no console text
* [ ] Review 10 of 12

## Phase 7: the coverage (13 items)

* [ ] Refusal errors read from the code and recorded in `notes.md`
* [ ] `procfull` + mutation
* [ ] `fdfull` + mutation
* [ ] `sockfull` (two processes) + mutation
* [ ] `tcbfull` + mutation
* [ ] `oomprobe` (touch pages, child dies, parent recovers) + mutation
* [ ] `diskfull` + the `EIO` to `ENOSPC` fix + mutation
* [ ] `ahci.c` gains an injectable register interface
* [ ] `ahci_hosttest`: every matrix row a checklist line
* [ ] `tcp_hosttest`: every matrix row a checklist line
* [ ] `packet_hosttest`: malformed Ethernet, IPv4, UDP, TCP
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
