# Milestone 34 notes

Deviations from `plan.md`, measurements, defects found and what was done about
them, and every decision the plan left to the run.

## Phase 1

Tree clean at `aee2cd6`, no stray machines, build current. Recent history:

```text
aee2cd6 Recast two sentences in the milestone 34 plan
f1f85e8 Rewrite the milestone 34 plan around the review
e66831c Plan milestone 34, the messaging and the tests
48431a7 Write down what the suite review found
2396530 Record the fast-suite milestone
deb2d50 Wait for the console, schedule the long suites first, and stop the credential race
bce5b2c Set the worker count from the processors, measured
9579f5c Run the second card's suites beside the first card's
c090f9c Run the suites through a pool and name each machine's owner
0d040f9 Stop the suites writing each other's images and ports
0b4d0f4 Open the milestone 33 record
115e793 Rewrite the fast-suite plan around what the review found
```

Baseline adopted rather than re-measured: milestone 33's warm run, 465s across
44 suites, recorded in `dev-log/33-fast/notes.md`. The only commits since are
this milestone's plan and findings, which change no code.

## Phase 2

* My first splitter treated a comment line inside a recipe as the end of the
  block. Every recipe with an embedded comment was truncated: the tail stayed
  in the root `Makefile` and attached itself to whatever target preceded it,
  and `run-e1000` was left with prerequisites and no body at all. A target
  with no recipe is legal Make, so `make -n run-e1000` still succeeded; only
  reading the file found it. The split was redone with a rule that treats a
  comment as part of a recipe when further recipe lines follow, and checked by
  requiring every `run-` target to have a body and by reconciling the line
  counts: 756 plus 1056 equals the original 1789 plus the 23-line header.

* `selfbuild` failed in the first full run after the split, and the cause
  predates this milestone. The suite waits for `HASHONE=` and then reads
  `SIZEONE` from the same console line, but the size is printed after the hash
  on that line, so the value is read while the machine is still typing it. It
  returned 3979 for a kernel of 397980 bytes. Under a twenty-worker pool the
  guest's output is slow enough to widen that window, which is why milestone
  33 saw the same suite fail at 51 seconds with an empty size and why the
  cause was then misread as the credential race. The guest now prints a marker
  after the value and the suite waits for that marker. Proved twice: a kernel
  reported as 1234 bytes is still rejected, and with the old wait restored a
  guest that pauses mid-number reproduces the truncation.

## The runner self-test, 2026-08-30

`tests/system/selftest.sh` drives `tests/run` against a registry and a makefile
of its own in a temporary directory: a passing suite, a failing one, one that
needs a command that does not exist, one that leaks a machine, an unknown name,
a tier, and two bad worker counts. Eighteen checks, about two seconds, no
machine booted and nothing read from the product. `tests/run runner` runs it,
and `sh tests/system/selftest.sh` runs it on a tree that does not build.

Proved by six mutations of `tests/run`, each restored from a trap: skipping the
tool check (3 checks fail), dropping the machine check (2), inverting the tier
filter (1), accepting an unknown name (2), turning a suite failure into a pass
(4), and dropping the worker-count validation (2).

The first attempt at the mutation run reported two mutations caught by nothing.
They were not: `tests/run` checked the name and the tools twice, once before
the build and once inside `run_one`, so breaking one copy left the other. The
duplicate is now gone. Every name and tool is checked once, before `prepare`.

`prepare-all` rebuilt `build/images/rach2-sealed.disk` on every run, about 25
seconds, whether or not anything it stages had changed. Closed in phase 8: the
disk was a phony target whose prerequisites are phony too, so it was always
out of date. The name is now an alias that builds the components and then asks
for the disk, and the disk is a file target naming the files it is made of. A
second build takes no time at all.

## The orphaned probes, 2026-08-30

Ten guest probes were built, staged in the test image, and run by nothing:
cowprobe, forktest, sockettest, dnstest, stdintest, ttyprobe, vmfault, vmro,
winprobe, udptest and tcptest. Nine had never been run by any target in the
history of the tree; vmfault appeared in `exitline` only as material for
checking the line the kernel prints, not as a probe with a result. The header
of `runtests.c` said they were "run by hand under the harness instead", and
that was not true of any of them.

Each was read and given a suite:

* cowprobe, forktest, sockettest and dnstest now run in the shared probe boot.
  All four exit 0 on success, which is all that boot requires; the header's
  claim that cowprobe and forktest carry the child's status was wrong. dnstest
  had one block that resolves real names through SLIRP, so the live block now
  runs only with `--live`, which the suite does not pass. 63 probes to 67.
* `tests/run fault`: vmfault, vmro and winprobe in one boot. They are killed on
  purpose, so they have no status; the harness gained `--never TEXT`, which
  fails a run if a text ever appears, and the text is the line each probe
  prints when it was not killed.
* `tests/run console`: stdintest and ttyprobe. Both block until something is
  typed. The harness gained `sendraw=TEXT`, which writes with no newline after
  it and reads `\xNN`, because a terminal in raw mode would otherwise read the
  newline of a `send=` as the byte under test.
* `tests/run socket`: udptest and tcptest against a host echo server. This is
  the guest opening a connection of its own and being answered, which nothing
  else in the suite does: the SSH suites have the guest listening, and the
  capture suites read what it sent with no one answering.

`holdport` became `hostpeer`, with `hold` and `echo` jobs. The tool count stays
at seven.

Proved by mutation, each restored from a trap: a read-only page made writable
(fault), a read that ignores the buffer size and a `sendraw` that stops
decoding a byte (console), and an echo server that drops a byte or answers with
other bytes (socket). All caught.

## The registry check, 2026-08-30

A one-line edit to `tests/registry` replaced `default` with two rows, because
`fault|qemu-system-i386|full` is a substring of `default|qemu-system-i386|full`.
Nothing noticed until `tests/run default` said the name was unknown: make
accepts an unknown goal only by failing, on the day someone asks for that
suite, and a target no row names is silent forever. `tests/run style` now
checks the registry against the `run-` targets in both directions, and refuses
a name written twice.

## Two suites that were paying for nothing, 2026-08-30

`procfiles` booted three times to read one line out of each boot, and ran the
whole 1130-line `procprobe` on each. `procprobe mount` now reads the root's
line from `/proc/mounts`, prints it, and stops; the rest of the probe runs once,
in the shared probe boot, where it already ran. 152s to 125s. Proved by making
the probe read a line that is not the root's: the ram boot's comparison fails.

`addressed` re-ran every probe with `rach2.ip=10.0.2.15/24` on the boot line,
which is the address SLIRP hands out anyway, so a kernel that ignored the boot
line passed. It is now one boot at 10.0.2.77 (not .99, which pingtest requires
to stay unreachable), and the capture is read from outside: three packets sent
from .77, and an ARP request sent as .77. Nothing on that network ever asks the
machine for its address, so `--owns` cannot be used and `pcapcheck` gained
`--arp-from`, which reads the sender of a request instead. Proved by making
`netconf.c` ignore `rach2.ip`: no packet leaves from .77 and the suite fails.

## Phase 2 proofs, 2026-08-30

`tests/run all`: 57 suites, all passed, 465s, no machine left running. The 44
suites of milestone 33 took the same 465s, so the thirteen added here cost no
wall-clock time: they fill slots the pool was leaving idle around the four long
suites.

A touched kernel source is rebuilt before the next suite: `touch
rach2/sys/serial.c` then `tests/run fault` rebuilt `serial.o` and the kernel
one second after the touch, and the suite ran on the new kernel.

Leak attribution names the suite: with `crash.sh` stopped from killing its
machines, `tests/run crash` failed with "run: crash: left 76756 76763 76792
running", which is the defect milestone 33 fixed, restored and caught.

## Review 1 of 12, 2026-08-30

Codex read `tests/run`, `selftest.sh`, the two harness additions, `hostpeer.c`,
`socket.sh` and the four new suite targets. Twelve findings, all fixed:

* `mktemp` was unchecked in both scripts. An empty value would have put the
  QEMU shim at `/bin/qemu-system-i386` and removed `/` on the way out.
* A leaked machine was named and left running. It is now stopped as well.
* `RACH2_WORKERS=00` passed the digit test and hung the pool, which waits for a
  worker that a limit of zero never frees. The value is made a number and
  required to be at least one.
* `tests/run core socket` ran core and dropped `socket` without a word. A set
  and a name together are now refused.
* The same name twice shared one ledger and one held output, so the second
  result overwrote the first. Refused.
* The shim's path to the real QEMU was unquoted.
* `socket.sh` waited for the server by asking whether the process existed,
  which is true before it has run `hostpeer` at all. It now waits for a
  connection to succeed, which `hostpeer hold PORT 0` is.
* `--never ""` was accepted and could never fail, because the matcher returns
  false for an empty text. Refused.
* A `sendraw=` whose write failed was counted as a step that ran.
* The TCP echo assumed one `write` took the whole buffer and died on SIGPIPE if
  the guest reset. It writes to the end and ignores the signal.
* `hostpeer echo 7777 junk` was a valid run of zero seconds. Both numbers are
  now whole or refused.
* The `addressed` comment still said .99 in one sentence after the address
  moved to .77.

Three oracle weaknesses it found, and what was done:

* The runner self-test never checked that `prepare-all` runs once for a whole
  run, nor that the worker limit is a limit. Both are checked now: the fixture
  counts preparations, and two suites that take two seconds each take four
  seconds at one worker and under four at two. 18 checks to 25.
* `sendraw`'s decoding was exercised only for `\xNN` and `\n`. `harness
  --selftest` now checks thirteen cases including every escape and four that
  must be refused, and `tests/run style` runs it beside the probe scorer.
* `stdintest` believed the count the kernel returned. It now fills the buffer
  first and requires the bytes past the count to be untouched. Proved: a
  `copy_to_user` of `n + 8` in the console read fails the suite. A mutation
  inside `n_tty_read` does not, because the syscall copies out exactly the
  count it returns from a kernel buffer; the check is on the copy-out length,
  which is the reachable half.

Two it raised that are recorded rather than changed:

* `run-fault` cannot tell a fault handler that kills only the faulting process
  from one that kills others as well. The debug shell is the only other process
  on that boot, and the suite ends by having the shell exec a program that
  prints a token, so a handler that took the shell with it fails here.
* `run-addressed` does not check `rach2.gateway=`. Every destination in reach
  is on the subnet, so nothing is routed and a wrong gateway would not show.
  Routing through a gateway is what `dhcp` and `netif` check.

## The logger, 2026-08-30

`rach2/sys/klog.c` and `klog.h`, with three supporting changes:

* `k_vprintf` moved out of `kio.c` into `sys/lib/kfmt.c`. It is the one
  formatter and it has no screen in it; leaving it in the file that holds the
  VGA console meant a caller that formats but does not draw pulled the console
  in behind it, and it could not be compiled for a host test without the
  terminal interpreter coming too.
* `sys/lib/kirq.c` holds the interrupt bracket, which `tty.c` had a private
  copy of. Both use the shared one now.
* `serial.c` gained `serial_out`, a write with no boot-log copy, no tap, and a
  transmit wait that gives up after a fixed number of polls. `serial_putc`
  calls it, so the old unbounded wait is gone from every path. A kernel that
  hangs while reporting a fault reports nothing.

Two decisions differ from the plan, both recorded here rather than quietly:

* The plan gave the panic path its own two-step delivery, ring first and the
  screen in between. `k_panic` draws its picture and then raises one `CRIT`
  record, because drawing clears the screen and a record printed before it
  would be wiped. The property the two-step existed for still holds: the
  record reaches the ring before the serial port and the wire, and both of
  those are bounded now.
* `klog` is a thin variadic wrapper over `klog_v`, which takes the argument
  words. Reaching a variadic function's arguments by walking the stack from
  the last named one is an i386 fact, and the host test does not run on i386.
  The split is the same one `k_printf` and `k_vprintf` already have.

The host test drives the logger against recorders in place of its four
destinations, which is how the cases a boot will not produce on demand get
checked: a ring that wraps mid-record, a port that never becomes ready, a
record longer than its buffer, and a record raised from inside the delivery of
another. Proved by nine mutations of `klog.c`, all caught: a dropped priority
prefix, a broken timestamp digit, an unmarked truncation, a record allowed to
end in two newlines, a screen filter that takes every level, the serial port
written before the ring, a ring that drops bytes instead of records, a console
that writes to the wire, and a ring append with no interrupt bracket.

No caller is converted yet beyond `k_panic`. That is phase 5, and the ledger
for it is `messaging.md`.

## Review 2 of 12, 2026-08-30

Codex read the logger, the two files moved out from under it, `serial.c` and
the host test. Its verdict was that the subsystem was not ready for callers,
and it was right on four counts. All fixed:

* A format ending in its own `%` read past the end of the string. `k_vprintf`
  consumed the terminator looking for a conversion character and the loop then
  read the byte after it. That defect is older than this milestone; the
  formatter was moved here, not written here. It now writes the sign out and
  stops.
* `klog_read` walked the ring with no interrupt bracket, so an append could
  evict and overwrite under a copy in progress. It takes the bracket now. What
  cannot be fixed inside one call is that offsets shift between calls when the
  ring evicts; that is the contract `/dev/bootlog` already has, and it is now
  written down in `klog.h` for the reader phase 6 writes.
* Records were dropped for netconsole while it had no device. That threw away
  exactly the records worth having on the wire: netconsole holds what it
  cannot send yet and sends it when a device arrives, which is what the serial
  tap used to feed it. It is offered every record now.
* A newline inside a record's text made a second delimiter, and the ring
  evicts by finding one, so such a record would have been dropped in halves
  and left a tail with no header. A newline in the text is kept as a space;
  the record's own newline is written past the sink that does that.

One design point it raised is now decided rather than left: the three
destinations past the ring write byte by byte with interrupts as the caller
had them, so a record raised by an interrupt handler could put its bytes
inside another record's line, and on the screen inside a terminal escape the
shell was part way through. A record raised while another is being delivered
now goes to the ring and no further. One record in the log only is better than
two shredded into each other, and the ring is exact either way.

It also found the host test weaker than its own descriptions in eight places.
The test now checks the exact truncation point and the exact fit beside it,
that the ring already holds the record when the screen is written, that every
byte is offered to a port that accepts none, the ring's contents byte for byte
against the records that should be in it, that the ring is within one record
of full, that a nested record reaches the ring and nothing else, that
netconsole is offered records before it has a device, an interior newline, a
format ending in `%`, and the reader's own bracket. 25 checks to 41.

Proved by fifteen mutations of `klog.c` and `kfmt.c`, all caught.

Two of its style points are taken: the function explanations are Doxygen
blocks, and the reasoning that was in function bodies has moved into those
blocks. The third, the `/* ---- section ---- */` markers, is left: the tree
uses that form in `harness.c` and `runtests.c`, and `textcheck.sh`, which
decides the dash rule, passes them.

## The oracles, 2026-08-30

Four suites that could not fail, three of them fixed here and one in phase 2.

`default` now reads the kernel's own checks. The boot self-tests were 227
lines of near-duplicate printing to two outputs, ending in `[PASS]` or
`[FAIL]`; nothing in the tree looked for the word, so a boot whose checks all
failed passed the suite. They are records now, one per check, ending in
`selftest: result: passed: N` or `failed: K of N`. The probe runner requires
the summary and refuses any error record between the group's first record and
it, which is the interval that makes the rule usable at all: the fault probes
later in the same run print error records on purpose.

That check found a real fault the moment it could see one. `test_udelay`
required 190 to 230 ms for a 200 ms wait, and under the pool's own load that
is not what an emulated 8254 gives: of sixteen machines booted at once here,
nine measured outside the band, between 180 and 280 ms. The band is now 100 to
400 ms, which catches a delay wrong by a factor (what a device bring-up would
notice) and not one wrong by a fraction on a loaded host. Sixteen at once pass
now. The check had been failing on and off for as long as it has existed, and
the boot said so every time.

`subset` booted with each `rach2.no*` option and asserted only that a shell
appeared, which a kernel ignoring every option would also do. Each device
probe now says `started` when it begins and `skipped: rach2.noX` when it is
told not to, and the suite requires the second and forbids the first, one
option at a time, then all four at once, then a boot with none of them where
every probe must run. Proved by two mutations: a probe that ignores its
option, and a kernel that skips everything whatever it is told.

`reset` announced the restart and could then hang, because the harness killed
the machine itself and passed on the token. The harness now reports whether
QEMU ended by itself, `--must-exit` requires that, and the suite forbids a
`CRIT` record so a crash that also ends the machine is not read as a reset.
Proved by a kernel that announces the restart and halts.

Fixing the harness for that found a second defect in it: `pump` returns the
moment the serial pipe ends, which is before the machine that closed it has
finished going away, so the first attempt read "still running" on every clean
restart. The exit is now waited for, bounded by the same grace period.

Three suites read boot text that these conversions changed: `netconsole` and
`metal` wanted `Network... OK`, and `diskdiag` wanted `Disk diagnostic...
none`. The last one is the interesting one: the text was still printed, but
`block: started` now landed in the middle of that half-finished line, because
a console print and a record cannot share a line. The disk diagnostic became a
record too. That is the argument for phase 5 in one line of output.

`serial_out` skips the boot-log copy that `serial_putc` makes, so records were
missing from `/dev/bootlog`, which `tests/run metal` fetches over SSH. The
serial destination feeds the boot log itself until phase 6 gives `/dev/bootlog`
the ring.

## Group 2 of the migration: storage, 2026-08-30

Seven files, 43 calls, all read in place before any was converted. The reading
settled the rule the rest of the migration follows: a call inside a function
the debug shell invokes by name is console output; everything else is a
record. The shell calls `ahci_dump`, `ahci_selftest` and `krffs_list_files`,
and those three keep their exact text through `console_printf`. The other 28
became records: DEBUG for bring-up detail nobody reads on a screen, WARN for a
refused command or a port that did not come up, ERR for an image that cannot
be mounted or a controller that cannot be used, INFO for the one line per
device that says what answered.

No test in the tree matches any of these texts, which was checked before the
conversion rather than discovered by a failing suite.

One of them was `%p`, which the kernel formatter has no conversion for: it
printed the letter `p` and left the address unread, so every argument after it
was read from the wrong stack slot. It is `%x` on the address now. Nothing
noticed because the line only appears when a mount is refused.

## Review 3 of 12, 2026-08-30

The first attempt died without a result: the delegated process ended after
reading the files and before answering. A second, narrower run answered, and
the first one's answer arrived later as well, so there are two reviews of the
same change. Between them, nine findings, all fixed:

* The self-test check accepted any summary, so a kernel that printed
  `result: passed: 0` without running a check passed. The number in the
  summary must now equal the records in the group: one per check and one for
  the group's start.
* That check failed the moment it was added, and the reason was the same fault
  the `diskdiag` line showed: the round-robin test's two processes print `A`
  and `B` with no newline, so the record after them began in the middle of
  that line and a reader looking at line starts could not see it. The letters
  go through `console_putc` now and the line is closed before the record.
* The summary was searched for from the start of the log rather than from the
  group, so an older summary could pair with a newer group and leave the
  interval reversed, in which case nothing between them was ever read.
* Neither record was required to be at a line start, so a probe printing the
  text of one inside its own output would have counted.
* `find_level_between` required four bytes where three are read, missing a
  `<2>` at the very end of the interval.
* `find_text` could read one byte past the buffer if a caller passed an empty
  tail. No caller does; it is refused now.
* Each `subset` boot checked only that its own probe was skipped, so a kernel
  where any one option switched all four off would have passed every boot.
  Each boot now requires the other three to start. Proved by that exact
  mutation.
* The all-four boot checked `mouse: skipped` and not the reason, so a probe
  skipped for another reason would have satisfied it.
* `--must-exit` accepted a machine ended by a signal. It requires an exit now,
  and says which of the two happened.
* The wait for that exit ran for the grace period rather than the run's own
  timeout, so a slow halt could be cut short. It runs to the deadline, and
  only where an exit is required, so a run whose machine is meant to stay up
  does not wait out its timeout.

The band on `test_udelay` is 150 to 350 ms rather than 100 to 400: still a
quarter wider at each end than sixteen machines at once measured, and no
wider, because half or double is what a bring-up sequence would notice.

One finding is recorded rather than fixed. A triple fault also resets the
machine, and with `-no-reboot` QEMU exits for it exactly as it does for a
reset the kernel asked for, so `tests/run reset` cannot tell those apart from
outside. The kernel prints a `CRIT` record for a kernel exception and the
suite refuses that, so what is left is a triple fault that printed nothing.
The suite says so where it is checked.

## The migration and the cutover, 2026-08-30

Every call in the kernel that printed is now either a record or a console
call. 695 of them, read in six groups, each group's classification written
down in `messaging.md` before any of it was converted.

The cutover that followed removes the parallel paths the old arrangement
needed:

* `/dev/bootlog` serves the log's ring. `sys/bootlog.c` and its second copy of
  everything printed are gone.
* `serial_putc`, `serial_puts`, `serial_printf` and `serial_set_tap` are gone.
  What reaches COM1 is decided by `klog` and the console calls; `serial_out`
  is the only way out of the port and it does nothing but write a byte.
* Netconsole is a destination of the log rather than a tap on the serial
  stream. Console output no longer reaches the wire at all, which is checked:
  the netconsole suite refuses the shell's prompt and banner in the capture,
  and requires each expected text with the `] ` that ends a record's prefix,
  so what it proves is a whole record reassembled from frames. Proved by
  making `console_putc` write to netconsole: caught.
* `tests/run procfiles` counts what `/dev/bootlog` holds on the machine: fifty
  records and no console text.

Fourteen suites read text that these conversions moved, and every one of them
was found by running the suite rather than by reading it. Two were more than a
text change: `sealed` and `install` rewrite the kernel's banner inside the
built artifact to mark a kernel, and `binfind --replace` requires the mark to
be the same length as what it replaces, so both marks were re-cut for the
shorter text. `install` also counts the lines its `sed` changed on the machine,
and there is one line to change now where there were two.

## What each limit refuses with, 2026-08-30

Read from the code before any probe was written, so the probes assert what the
kernel does and not what the plan guessed.

* The process table: `alloc_process` returns NULL and `sys_fork` returns
  `-EAGAIN` (`syscall.c`). 16 slots, one of them the debug shell's.
* A process's descriptors: `fd_alloc` fails and every caller returns `-EMFILE`
  (`syscall.c`, `devfs.c`). 16 per process, three of them stdio.
* The socket pool: `socket_alloc` returns NULL and `do_socket` returns
  `-ENOMEM`, not `-ENFILE` (`socket.c`, `syscall.c`). 16 sockets for the
  machine.
* The TCP control blocks: `tcp_connect` and `tcp_listen` return `-ENOBUFS`
  (`tcp.c`). A loopback connection takes two.
* Physical frames: a process that touches a page with none left takes a page
  fault the handler cannot serve, and `terminate_current_process(SIGSEGV)`
  kills it. The parent survives and its frames come back.
* A full file system: `krffs_write_node` returned a bare -1 whether the file
  system was full or the caller passed a bad argument, and the write syscall
  turned both into `-EIO`. A full disk is what a program writing a large file
  meets and it has to be able to tell that apart, so the two are now
  `KRFFS_ENOSPACE` and `KRFFS_EBAD`, and the syscall returns `-ENOSPC` and
  `-EIO`.

## The control-block probe, twice, 2026-08-30

The plan called for `tcbfull`, a guest probe that fills the TCP control-block
pool over loopback connections, each of which takes two blocks. The first
attempt did not work and I recorded two reasons for it. One of them was wrong,
and finding that out fixed a defect in the kernel.

The arithmetic reason was right: every block is owned by a socket and both
pools hold sixteen, so a process filling either one alone meets the socket
pool first. Only a connection to this machine's own listener takes two blocks
for one socket pair.

The second reason I gave was that a loopback handshake does not complete on
this kernel, because the probe's connections retransmitted and timed out. That
was wrong, and a five-line probe that makes one loopback connection settled
it: `bind` to no address, `connect` to 127.0.0.1, and the handshake completes
in the tick it is delivered. What actually happened is that the eighth
connection is the one that cannot be served: the connecting side takes the
fifteenth block, the side being connected to finds none left, and `tcp_rx`
dropped that SYN in silence. The caller then retransmitted for six seconds and
reported a timeout, which is what a machine that is not there looks like.

A machine with no room should say so, and there was already a reset for a port
nobody listens on. That path now answers with one, so the caller is refused at
once with `ECONNREFUSED`. `tcbfull` is in the tree and checks exactly that: a
listener and seven connections, and the eighth refused rather than ignored.
Proved by removing the reset again: the probe hangs and the suite says so.

What the probe does not check is a block coming back, because a closed
connection holds its two through the teardown and TIME_WAIT, which is the
protocol. `tests/host/tcp_hosttest.c` checks that directly.

## The limits, 2026-08-30

Five probes, each filling a fixed pool and requiring the refusal the kernel
documents, then requiring the resource back:

* `procfull`: 14 of the 16 process slots (the shell and the probe hold two),
  refused with `EAGAIN`, and a reaped slot reused.
* `fdfull`: 13 of a process's 16 descriptors (stdio holds three), refused with
  `EMFILE`, and a closed one reused.
* `sockfull`: two processes, because one runs out of descriptors at thirteen
  before the sixteen-socket pool is full. The parent fills its table, the child
  closes three of the inherited descriptors and opens three more, and the next
  open is refused with `ENOMEM`.
* `oomprobe`: a child maps and touches until the frames run out, is killed with
  `SIGSEGV`, and the parent then maps and touches successfully, which is the
  frames coming back.
* `diskfull`: 7 MiB written into the test image, refused with `ENOSPC`, the
  file removed and the write repeated. It has a boot of its own because it
  fills the image every other probe is staged in.

Proved by six mutations, all caught: fork refusing with `ENOMEM` instead of
`EAGAIN`, `close` not freeing the descriptor, the socket pool refusing with
`ENFILE`, a full file system reporting `EIO` again, and a dead process keeping
its frames.

## Review 10 of 12, 2026-08-30

Codex read the cutover. It found a regression I had introduced and four
smaller faults, all fixed:

* Every newline the terminal wrote reached the port as carriage return,
  carriage return, newline. The line discipline expands the newline itself,
  and `console_putc`, which the discipline now writes through, expanded it
  again. There is a raw console call for a writer that has already done it,
  and the discipline uses that one.
* One flag said whether the console had left a line open, and both the screen
  and the port cleared it. A `DEBUG` record reaches the port and not the
  screen, so it closed the screen's line without writing to the screen, and
  the next record on screen continued the prompt. There are two flags now,
  each cleared by the destination that wrote.
* `/dev/bootlog` reported bytes copied before a fault without advancing its
  cursor, so the next read repeated them.
* `serial_port_exists` and `serial_getc` had no callers left, and the header
  described a blocking read this kernel must not have. Both are gone.
* Four comments described the arrangement that this milestone replaced.

## The three host tests, 2026-08-30

`packet_hosttest` hands the receive parsers frames no peer would send: an
Ethernet header cut short, a version that is not 4, a header length below the
minimum and past the end, a total length past the frame and below the header,
a checksum that does not sum, a fragment, and a datagram for another machine.
Each frame is copied into an allocation of exactly its length, so a read one
byte past it is a read past the allocation and the sanitizer says so. Proved
by five mutations, all caught, one of them by the sanitizer rather than by a
check.

Writing it found that three of the first five mutations were caught by the
wrong check: the case changed a header field after the checksum was computed,
so every one of them was being dropped for the checksum. The header is
restamped per case now.

`tcp_hosttest` drives the state machine segment by segment against a recorder
in place of IP, with the clock a variable. It covers what no guest probe can:
the control-block pool filling and refusing (which is where `tcbfull` was
meant to go), a reset for a closed port and the absence of one for a reset,
data in order, the same data twice, a segment past a hole, the hole filled, a
window closing and reopening, a FIN, and a peer that stops answering until the
retransmission gives up. Proved by four mutations.

`ahci_hosttest` needed a change to the driver first: `ahci.c` reached the
controller through two static functions, and now reaches it through a table of
three, so a test can supply a model. The rows are the ones no boot produces: a
reset that never completes, firmware that never answers the handoff, a port
with nothing on the link, a port whose device is not a disk, a command that
never completes, and a device that reports a fatal status.

The first version of that test passed every row for the wrong reason. Each row
called `ahci_init` and asserted it refused, and on a model that cannot answer
an IDENTIFY it always refuses; three mutations of the driver changed nothing.
Each row now drives the step it is about and the positive case is checked
first, which is what made the mutations fail. The model also had to learn that
a status register clears on a write of one rather than storing it, and that a
controller reports the bytes it moved.

Two smaller things fell out of it: the driver's cache instructions are x86's
and are now compiled only where they exist, and `net_core.c` had a third copy
of the interrupt bracket, which is the shared one now.

## The audio path, 2026-08-30

`tests/run audio` boots with an Intel HDA controller and a codec, plays the
five-note scale the debug shell has, and measures the recording QEMU's wav
backend writes. The driver's own report says what it was asked to play; the
recording says what came out, and only the second can be checked from outside.

`wavcheck` is the eighth test tool and the only one that links the maths
library. It sweeps the recording in fifty-millisecond windows, labels each with
whichever named frequency is loudest in it, and requires those labels to visit
every note in the order named. The first attempt divided the recording into
equal fifths, which is wrong: the device opens during the first note and the
machine is stopped during the last, so the notes' shares differ by three to
one. That version failed two of five notes on a machine that played the scale
correctly.

Proved by two mutations: a sample generator that returns silence (caught: four
notes are never the loudest) and a scale whose five notes are all the same
(caught: the second note sounds where the second was asked for).

## Review 11 of 12, 2026-08-30

Codex read the new coverage and asked of each row whether it could pass while
the thing it names is broken. Nine of the twelve findings were that it could.
All fixed:

* The AHCI disk-policy row was `refused || policy_allows` with the policy
  always allowing, so it could not fail. It refuses now, and the port must see
  no command.
* The AHCI model kept `GHC.AE` across a host reset. A real reset clears it,
  which is why the driver sets it again; a driver that forgot would have
  passed. Proved by removing that write: caught.
* The model answered `SSTS.DET 3` whatever the driver did, so a driver that
  never reset the link would have passed. `SCTL.DET` now takes the link down
  and releasing it brings the link up, and there is a row for a port whose
  link comes up only on reset.
* The TCP zero-window row read the stored window and sent nothing. It offers
  data while the window is closed, requires none of it to go out, and requires
  it to go out when the window reopens.
* The retransmission row counted attempts, which a stack retrying every tick
  would also pass. The gaps between attempts are recorded and each must be
  longer than the first.
* `sockfull` required "at least eight" and `procfull` "between two and
  thirty-one", so a pool of another size passed. Both pin the number: thirteen
  descriptors and fourteen slots, which is what sixteen of each leaves. Proved
  by shrinking the socket pool to fourteen: caught.
* `procfull` ignored every reap after the first and could report success while
  leaving a zombie, which would take a slot from the next probe in the same
  boot. A failed reap is now a failure.
* `packet_hosttest` checked the raw layer on one row of three, and its
  Ethernet-header-only row omitted the packet sockets, which are deliberately
  offered that frame.
* `wavcheck` labelled every window with a frequency however quiet it was, so a
  machine playing almost nothing kept the same labels. A window below a
  threshold now belongs to no note. It also read `2 * channels` bytes into a
  sixteen-byte buffer and accepted any channel count; more than eight is
  refused.

Its four simplifications are taken: the model state no row used is gone.
