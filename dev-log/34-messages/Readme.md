# Milestone 34: the kernel log and the test system

The kernel said things in places nothing read, and the suite that was supposed to catch that had four checks which could not fail. Both halves are fixed, and each was proved by making it fail.

## Why the two together

They are one problem seen from two sides. A test can only check what a machine says, and this machine said the same thing in two places or in none: 695 calls across 39 files, split between a screen writer and a serial writer that no caller could reach in one call, so `diag.c` named both printers on every line and `kernel.c` printed each boot component's name twice and its outcome twice again. A test written against that reads a fragment of a line and hopes. Fixing the test system first would have written those hopes down; fixing the messages first would have broken every suite that reads them, with no measurement of what broke. So the test system was made trustworthy first, then the log was built, then the four suites that could not fail were fixed against the records the log now emits, and only then were the callers converted.

## The kernel log

One call for a diagnostic and one for the console. `klog(level, subsystem, format, ...)` builds a whole record and offers it to four destinations in a fixed order:

```text
<3>[12.34] ahci: command: timed out on port 0
```

The ring is first and always takes it, because a serial port that stopped draining or an Ethernet controller that never came up must not cost the machine its own copy. The screen takes `INFO` and above, without the prefix a person does not need. The serial port takes every level through a transmit wait that gives up rather than spinning. Netconsole takes every record from the first one, before there is a device to send through, because it holds what it cannot send yet and the records worth having on the wire are the early ones.

`console_putc`, `console_puts` and `console_printf` are the other half. The debug shell's prompts and command output are a user interface, not a diagnostic: they are not in the machine's log and they are not broadcast to whoever is on the segment. That is checked, not asserted: `tests/run netconsole` refuses the shell's prompt in the capture, and `tests/run procfiles` counts what `/dev/bootlog` holds on the machine.

The parallel paths are gone. `serial_putc`, `serial_puts`, `serial_printf` and the netconsole serial tap are deleted; `sys/bootlog.c` and its second copy of everything printed are deleted; `/dev/bootlog` hands out the log's own ring. `serial_out` is the only way to the port and does nothing but write a byte.

## The four suites that could not fail

The kernel ran a dozen self-tests at every boot and printed `[FAIL]` when one failed, in 23 places; nothing in the tree looked for the word. They are records now, and the probe runner requires the summary and refuses any error record between the group's first record and it. That check found a real fault the moment it could see one: `test_udelay` required 190 to 230 ms for a 200 ms wait, and of sixteen machines booted at once here, nine measured outside it. The band is 150 to 350 ms now, a quarter wider than the measurement at each end and no wider.

`subset` booted with each `rach2.no*` option and asserted only that a shell appeared. Each probe now says `started` or `skipped: rach2.noX`, and each boot requires the one and forbids the other, and requires the other three probes to run.

`reset` announced the restart and could then hang, because the harness killed the machine itself. `--must-exit` fails a run whose machine had to be killed, and the suite forbids a `CRIT` record so a crash is not read as a reset.

`addressed` re-ran every probe at the address SLIRP hands out anyway, which a kernel ignoring the boot line would also pass. It is one boot at an address nothing else would produce, checked from the capture: three packets from it, and an ARP request sent as it.

## The test system

The 44 suite recipes left the root `Makefile`, which went from 1789 lines to 756 and holds no test rule; `make` builds no test tool. The runner is 200 lines of shell over a three-field registry, and it has a test of its own that boots nothing: 25 checks, two seconds. `tests/run style` checks the registry against the targets in both directions, which is how a one-line edit that silently deleted the `default` suite would have been caught.

Ten guest probes were compiled, staged and run by nothing; nine had never been run by any target in the tree's history. Four joined the shared boot, and three suites were written for the rest: `fault` for the probes that must be killed, `console` for the two that block on typed input, `socket` for the guest opening a connection of its own and being answered, which nothing covered.

## What is new to check

The kernel's fixed limits are reached and their refusals required: the process table (`EAGAIN`), a process's descriptors (`EMFILE`), the socket pool (`ENOMEM`, which needs two processes), the TCP control blocks, and the physical frames, where a child is killed with `SIGSEGV` and the parent's next allocation must succeed. A full file system returned `EIO` for both a full disk and a bad argument; it returns `ENOSPC` now, and `diskfull` is what proves it.

Three host tests reach what no boot produces. `ahci_hosttest` runs the driver against a model of a controller, through a register interface `ahci.c` gained for it: a reset that never completes, firmware that never answers the handoff, a port whose link comes up only on reset, a command that never completes, a device that reports a fatal status. `tcp_hosttest` drives the state machine segment by segment with the clock a variable. `packet_hosttest` hands the receive parsers frames no peer would send, each in an allocation of exactly its length so a read past one is a read past the allocation.

`tests/run audio` measures what the machine played rather than what it was asked to play, from the recording QEMU's wav backend writes.

## What running it found

The kernel dropped a SYN it had no control block for, in silence, leaving the caller to retransmit for six seconds and report a timeout, which is what a machine that is not there looks like. It answers with a reset now.

`krffs_write_node` returned a bare -1 whether the file system was full or the caller passed a bad argument, and the write syscall turned both into `EIO`.

The self-build suite read a kernel size while the machine was still typing it, which is the 51-second failure milestone 33 recorded and attributed to something else.

`k_vprintf` read past the end of a format string ending in its own `%`.

Every newline the terminal wrote reached the serial port as carriage return, carriage return, newline, for as long as the discipline and the console call both expanded it. That one was introduced here and found by the review of the change that introduced it.

## What was refused

`procfiles` is not in the core tier: it is 125 seconds and the tier is under two minutes. The `e1000` suites were not trimmed, for the reason milestone 33 gave. `hda_probe_log`'s 55 call sites were not rewritten one at a time; the trace gathers its fragments and emits one record per line instead. A guest probe for the control-block pool cannot check a block coming back, because a closed connection holds its two through TIME_WAIT; the host test checks that directly.

## Verification

Cold from `make distclean`: 124 seconds to build everything. `make test`, which is the source checks, the native tests and the core tier: 172 seconds warm, 178 after a cold build. `tests/run all`: 471 seconds, 59 suites, no machine left running.

The core tier is 21 system suites chosen by two rules, that between them every subsystem is exercised and that the tier finishes in under two minutes: 107 seconds.

Every check added here was proved by a mutation that makes it fail, driven from a script that restores the source under a trap: sixty-two of them, each caught by the check it was aimed at. Twelve reviews were planned and five were run, each of at most six files; between them they returned 58 findings. The most serious in each: an unchecked `mktemp` that would have put the QEMU shim at `/bin/qemu-system-i386`; a worker count of `00` that hung the pool; a format ending in `%` reading past its string; the terminal's double carriage return; and nine rows of the new coverage that could pass while the thing they name was broken, including a disk-policy check that could not fail at all.
