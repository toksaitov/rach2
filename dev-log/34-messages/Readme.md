# Milestone 34: the kernel log and the test system

The kernel printed diagnostics where nothing read them, and four suites asserted less than their names claimed. Both are fixed. Every check added here was then made to fail by a mutation of the code it watches, which is the only evidence that it watches anything.

## Why the two together

A suite can only read what the machine prints, and this machine printed the same line twice or in one place only: 695 calls across 39 files through a screen writer and a serial writer that no caller could reach in one call. `diag.c` named both printers on every line; the boot printed each component's label twice and its outcome twice again, in four calls. A suite written against that matches a fragment of a line.

The order was: the test system first, then the log, then the four suites, then the callers. Converting the callers first would have changed the text fourteen suites match on, with no measured suite to say what broke.

## The kernel log

There is one call for a diagnostic and one for the console. `klog(level, subsystem, format, ...)` builds a whole record and writes it to four destinations in a fixed order:

```text
<3>[12.34] ahci: command: timed out on port 0
```

The 16 KiB ring is written first and accepts every level, so a serial port that stopped draining or an Ethernet controller that never came up cannot cost the machine its own copy. The screen is written next, at `INFO` and above, without the prefix and timestamp. The serial port is written at every level, through a transmit wait bounded at a fixed number of polls. Netconsole is written last, also at every level, including the records raised before a network device exists: it buffers what it cannot yet send. `rach2/Readme.md` states the record format, the five levels and the ring's eviction rule.

`console_putc`, `console_puts` and `console_printf` carry the debug shell's prompts and command output. Those reach the screen and the serial port and nothing else: they are absent from the machine's log and from the wire, where netconsole broadcasts to whoever is on the segment. Two suites check it. `tests/run netconsole` fails if the shell's prompt appears in the capture, and `tests/run procfiles` counts the records and the console lines in `/dev/bootlog` on the machine: fifty records and no console text.

The parallel paths are gone. `serial_putc`, `serial_puts`, `serial_printf` and the netconsole serial tap are deleted; `sys/bootlog.c` and its second copy of everything printed are deleted; `/dev/bootlog` hands out the log's own ring. `serial_out` is the only way to the port and does nothing but write a byte.

## The four suites that could not fail

`default` is the first. The kernel runs a dozen self-tests at every boot and printed `[FAIL]` when one failed, in 23 places; nothing in the tree matched the word. They are records now, and the probe runner requires the summary, requires its count to equal the records in the group, and fails on any error record between the group's first record and the summary.

That check failed on the run that introduced it. `test_udelay` required 190 to 230 ms for a 200 ms wait; sixteen machines booted at once here measured between 180 and 280 ms, and nine of them were outside the band. It is 150 to 350 ms now, which is a quarter beyond the measured range at each end.

`subset` booted with each `rach2.no*` option and asserted only that a shell appeared. Each device probe now emits `started` or `skipped: rach2.noX`, and each boot requires the skipped record, fails on that probe's started record, and requires the other three probes to start.

`reset` accepted a machine that announced the restart and then hung, because the harness killed the machine itself and passed on the printed token. The harness option `--must-exit` fails a run whose machine had to be killed, and the suite fails on a `CRIT` record, so a crash that also ends the machine is not read as a reset.

`addressed` re-ran every probe with `rach2.ip=10.0.2.15/24`, which is the address SLIRP hands out anyway, so a kernel that ignored the boot line passed it. It is one boot at 10.0.2.77, checked from the packet capture rather than from inside the machine: three packets sent from that address, and an ARP request sent as it.

## The test system

The 44 suite recipes moved from the root `Makefile` to `tests/system/suites.mk`, which the root file includes with one line. The root file is 772 lines against 1789 and holds no `run-` target, and `make` builds no test tool. The runner is 200 lines of shell over a registry of three fields, described in `tests/system/Readme.md` with the reason for each of its six mechanisms. It has a test of its own that boots nothing: 25 checks in two seconds.

`tests/run style` compares the registry with the `run-` targets in both directions. A registry line that names no target is a suite nobody can run, and a target no line names is a suite nobody does run; neither shows up otherwise, and a one-line edit here deleted the `default` suite for an hour before that check existed.

Ten guest probes were compiled and staged in the test image, and no target ran them: `cowprobe`, `forktest`, `sockettest`, `dnstest`, `stdintest`, `ttyprobe`, `vmfault`, `vmro`, `winprobe`, `udptest` and `tcptest`. A search of the history found no target that ever had. Four now run in the shared probe boot, which needed only that they exit 0. Three suites cover the rest: `fault` runs `vmfault`, `vmro` and `winprobe`, which are killed on purpose and so report no status; `console` types at the terminal for `stdintest` and `ttyprobe`, which block until something is typed; and `socket` runs `udptest` and `tcptest` against a host echo server, which is the only place the guest opens a connection of its own and is answered.

## What is new to check

Five probes fill a fixed limit and require the refusal the kernel documents, then require the resource to come back. The process table holds 16 and refuses the 15th fork with `EAGAIN`; a process holds 16 descriptors and refuses the 14th open with `EMFILE`; the socket pool holds 16 and refuses with `ENOMEM`, which takes two processes because one runs out of descriptors first; the TCP control blocks hold 16 and a connection takes two, so the eighth loopback connection is refused; and a child that touches more pages than there are frames is killed with `SIGSEGV`, after which the parent's own allocation and touch must succeed.

A full file system returned `EIO`, which is what a bad argument returns, so a program could not tell a full disk from a failed write. `krffs_write_node` reports the two separately now and the write syscall returns `ENOSPC`; `tests/run diskfull` fills the image and requires it.

Three host tests reach cases a working QEMU does not produce.

`ahci_hosttest` drives the AHCI driver against a model of a controller, through a register interface `ahci.c` gained for it. Its rows: a controller reset that completes and one that never does, a BIOS handoff answered and one left unanswered, a port with a disk, a port with nothing on the link, a port whose link comes up only when reset, a port whose device is not a disk, a controller with no implemented port, the port engines starting, a command that completes, one that never completes, one that returns a fatal task-file status, and a command the disk policy refuses.

`tcp_hosttest` drives the state machine with the IP layer replaced by a recorder and the clock a variable: the control-block pool filling and refusing, a reset for a closed port and no reset for a reset, a handshake, data in order, the same data twice, a segment past a hole, the hole filled, a window closing and reopening, a peer's FIN, and a peer that stops answering until the retransmission gives up, where each wait must be longer than the one before.

`packet_hosttest` hands the receive parsers malformed frames: a short Ethernet header, a version that is not 4, a header length below the minimum and past the end of the frame, a total length past the frame and below the header, a checksum that does not sum, a fragment, and a datagram for another machine. Each frame is copied into an allocation of exactly its length, so a read one byte past the frame is a read past the allocation and AddressSanitizer reports it.

`tests/run audio` reads the recording QEMU's wav backend writes. The debug shell plays five notes at 262, 294, 330, 349 and 392 Hz; `wavcheck` sweeps the recording in fifty-millisecond windows, labels each with whichever of those frequencies has the most energy in it, and requires the labels to visit all five in that order. The driver's own report says what it was asked to play, and only the recording says what came out.

## What running it found

A SYN arriving with no control block left was dropped without a reply, so the caller retransmitted for six seconds and reported a timeout, the same result it gets from a machine that is not running. `tcp_rx` sends a reset there now, and the caller is refused at once with `ECONNREFUSED`. Removing the reset again makes `tcbfull` hang and the suite report it.

`krffs_write_node` returned -1 both when the file system was full and when the caller passed a bad argument, and the write syscall turned both into `EIO`. It returns `KRFFS_ENOSPACE` and `KRFFS_EBAD` now, and the syscall returns `ENOSPC` and `EIO`.

The self-build suite read a kernel size out of a line the machine had not finished writing, and got 3979 for 397980 bytes. That is the 51-second failure milestone 33 recorded and attributed to the credential race it fixed in the same run. The step emits a marker after the number and the wait matches the marker.

`k_vprintf` read past the end of a format string that ended in its own `%`: it consumed the terminator looking for a conversion character and the loop read the byte after it. It writes the sign out and stops now, and `harness --selftest` covers the decoder beside it.

Every newline the terminal wrote reached the serial port as carriage return, carriage return, newline, because the line discipline expands the newline and `console_putc`, which it now writes through, expanded it again. There is a raw console call for a writer that has already done the expansion, and the discipline uses that one. This defect was introduced in this milestone and found by the review of the change that introduced it.

## What was refused

`procfiles` is not in the core tier: it takes 125 seconds on its own and the tier's whole budget is two minutes.

The nine `e1000` suites were not trimmed. Milestone 33's reason still holds: the host model proves the driver in isolation and does not run the stack through the device, so dropping the second card's `netif`, `raw` and `dhcp` would drop live interface reconfiguration, the raw socket, and packet-socket address reconfiguration on that card.

`hda_probe_log`'s 55 call sites were not rewritten. The trace composes each line from fragments, so it gathers them and emits one record per line, which converts every site at once and keeps the text.

`tcbfull` does not check a control block coming back. A closed connection holds its two through the teardown and TIME_WAIT, which is the protocol, and a probe that waited it out would spend a minute doing so. `tcp_hosttest` frees a block and requires the next allocation to return one.

## Verification

All measurements are from the development machine: ten cores, 24 GB, macOS, QEMU 10, twenty pool workers.

Cold from `make distclean`, which compiles the five vendored projects and Doom: 124 seconds. `make test`, which is `tests/run style`, then `host`, then the core tier: 172 seconds warm and 178 after a cold build. `tests/run all`: 471 seconds for 59 suites, with no machine left running, repeated.

The core tier is 21 system suites and takes 107 seconds. They are `default`, `console`, `fault`, `socket`, `dgram`, `netconsole`, `e1000`, `disk`, `disk-ahci`, `persist`, `diskfull`, `subset`, `crash`, `reset`, `authority`, `grub`, `metal`, `audio`, `ssh`, `login` and `runner`. Between them they exercise processes, memory, the page cache, the file system, descriptors, pipes, signals, the terminal, `/proc`, both disk controllers, the network stack and two of the three cards, audio, the boot path, shutdown, restart, recovery, the log itself, and the userland. The third card is the laptop's and has no emulator.

Sixty-two mutations were applied, one at a time, each from a script that restores the source under a trap. Every one made the check it was aimed at fail, and no other. A mutation shows that a check is sensitive to one defect; it is not proof that the check is correct, and it is the strongest evidence available without a second implementation.

Five independent reviews were run against a planned twelve, each reading at most six files: the plan allotted one per migration group, and the six migration groups were reviewed together in the cutover review instead, because the groups are one change and a reviewer reading a sixth of it cannot see what the conversion moved. One further review returned nothing when the process ended before answering, and was rerun. Between them the reviews returned 58 findings. All are fixed except three, each recorded in `notes.md` with the reason: `fault` cannot distinguish a handler that kills only the faulting process from one that kills others as well, `addressed` does not check the gateway half of the boot line, and `reset` cannot separate a reset the kernel asked for from a triple fault that printed nothing.

The most serious finding in each review: an unchecked `mktemp` that would have put the QEMU shim at `/bin/qemu-system-i386`; a worker count of `00` that passed validation and hung the pool; a format string ending in `%` read past its terminator; the terminal's doubled carriage return; and nine rows of the new coverage that could pass while what they name was broken, one of which could not fail at all.

## What a passing run now means

`tests/run all` passing says the following, and each of these has a check that
was made to fail before it was believed:

* Every kernel self-test passed on the boot the suite scored, and the number of
  checks in the summary matches the records the group emitted. Before this
  milestone a boot whose checks all failed passed the suite.
* Every one of the 72 guest probes exited 0, and each of the four that carries
  a special contract (a fault, typed input, a host peer) ran under a suite
  built for it rather than being staged and forgotten.
* Each fixed limit was reached and refused with the error the kernel
  documents, and the resource came back.
* Each optional device probe honoured the option that switches it off, and ran
  when nothing switched it off.
* The machine restarted rather than announcing a restart, took the address its
  boot line named rather than the one the network hands out, mounted from each
  of the three roots, and wrote its disk only where the policy allows.
* What reached the wire and what reached the machine's own log are what the
  records say, read from a capture and from `/dev/bootlog` rather than from the
  code that wrote them.
* The three drivers, the TCP state machine and the receive parsers were driven
  through the cases a working QEMU never produces.

What it does not say. The suite runs on QEMU: the hardware paths for the
laptop's RTL8168GU, its AHCI controller and its HDA codec are exercised against
models and against QEMU's devices, not against that machine, and the hardware
procedure under `dev-log/24-metal/` is still what covers it. A triple fault
that prints nothing is indistinguishable from a reset the kernel asked for.
`tests/run addressed` does not check the gateway half of a boot line, which
`dhcp` and `netif` cover instead. And a passing run says nothing about the
parts of the kernel no probe reaches: the suite now names what it checks, which
is the change, but naming is not the same as covering everything.
