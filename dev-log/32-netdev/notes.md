# Milestone 32 notes

Deviations from `plan.md` and defects found but not fixed in the phase that
found them. Each entry says what and why.

## Phase 1

* `AGENTS.md` ends with "Readme.md has the fresh Mac bootstrap instructions".
  Milestone 31 deleted that script and moved machine setup to
  `tools/Readme.md`, so the sentence names a procedure that is not there. Left
  for phase 5, which is where this milestone's document corrections land.

## Phase 3

* `rach2/sys/lib/kstring.c` cannot be compiled natively at `-O1` or above
  without a sanitizer. It defines `memset` as a wrapper around `k_memset`, and
  the native compiler recognises `k_memset`'s loop as the memset idiom and
  emits a call to `memset`, which is the wrapper: the pair becomes an infinite
  loop. The host suite always passes `-fsanitize=address,undefined`, whose
  interceptor breaks the recursion, so no suite run has ever shown this. It
  appeared here only because a mutation script compiled a host test without
  those flags. The e1000 driver therefore keeps to plain byte loops and lists
  no `host-deps:`. Fixed later in the milestone: the host runner compiles with
  `-ffreestanding`, which is what these sources are and what stops the
  rewrite. The per-function `-fno-builtin-memset` is not enough on clang,
  whose loop-idiom pass is gated on the whole of `-fno-builtin`; that was
  found by disassembling `k_memset` and seeing it still tail-jump to
  `memset`. The cross compiler makes no such transformation at any
  optimization level, checked by disassembly, so the kernel was never at
  risk.

## Phase 4

* `k_puts` writes to the VGA console only; it never reached serial. The
  shutdown path used it for "Stopping the system. Goodbye!", so a machine read
  over netconsole, which taps serial, was never told it was stopping. Found
  while writing the wire contract for `netdev_stop`, whose comment had claimed
  that line was the last one on the wire. The line is now printed to both, as
  the boot messages already are.

* The network card really was transmitting through `power_stop`. The mutation
  that removes `netdev_stop` entirely puts `poweroff: powering off` on the
  captured wire, which is the evidence that the card kept mastering the bus and
  sending while the machine was resetting around it.

* `model_reset` in the e1000 host test did not clear `e1000_mmio` or the PCI
  record, so a case ran against a driver that still remembered the previous
  case's card. Found by a check that failed for the right reason; the reset was
  corrected rather than the expectation.

* `make` alone does not build the test checkers under `build/tests/system/`;
  they are prerequisites of the suites that use them. An edit to `pcapcheck.c`
  followed by `make` therefore leaves the old binary in place. Not a defect,
  but it is not obvious, and a checker edit needs `tests/run NAME` or the
  checker's own target to take effect.

## Phase 6

* The e1000 driver's rings were `volatile` and the other two drivers' were
  not. The reason recorded here at first was wrong: it said the e1000 was
  different because its host model compiles the file optimized, but
  `rtl8168_hosttest.c` compiles `rtl8168.c` the same way, so that driver had
  the same exposure. Its rings are volatile now too. The RTL8139 keeps plain
  buffers: it has no descriptor the card writes ownership back into, and its
  receive loop is ordered by a port read in another translation unit.

## The independent reviews

Two reviewers, six files each, stated as a fixed total before either started:
the drivers and the interface, and the consumers and the proofs. Both returned
findings; the most serious was found by both independently.

* The transmit ring could publish itself as empty. All 32 descriptors were
  initialised with `DD` set and all 32 could be submitted, so the 32nd wrapped
  `TDT` onto a `TDH` the card had not moved. A card reads head equal to tail
  as nothing to do, so 32 frames it had not sent would have become invisible
  to it and the link would have stopped for good. The host test blessed it,
  asserting `e1000_tx_packets <= E1000_RING_SIZE`. Now the driver counts
  submissions and reclaims and keeps one descriptor unsubmitted, as
  `rtl8168.c` does.

* The reset used the memory window. Intel's specification update records that
  on this part a memory write of `CTRL.RST` can draw a bridge padding write
  that takes a master abort during the reset, which some bridges promote to a
  machine check; the reference driver uses the I/O window on every member
  before the 82545 revision 3. QEMU exposes the I/O BAR, so the correct path
  is both taken and exercised on every boot.

* The reset did not quiesce a card that arrived running. The comment claimed
  no transfer could be in flight because this driver had not enabled bus
  mastering, which is false: a global reset preserves PCI configuration, so a
  card left running by firmware or a network boot arrives still fetching
  descriptors.

* `rtl8139_stop` skipped revoking DMA in the one case it existed for. `init`
  enabled bus mastering before validating BAR0, and the revoke sat inside the
  `iobase != 0` guard, so a card whose window was unusable kept the bus with
  no way to be told to stop. BAR0 is now checked before the bus is given, and
  the revoke is guarded on the PCI record instead.

* `rtl8168_stop` ran GU-specific sequences on revisions the probe had
  rejected, a hazard introduced by calling `stop` after `NETDEV_FAILED`.
  `MISC` at 0xf0 is the receive gate on this member and something else on
  older ones, so a rejected chip now gets only the interrupt mask and the bus.

* An interrupt line above 15 was accepted. The vector is `32 + irq` in a byte,
  so a large enough line wraps onto an exception vector and installs a network
  handler there.

* A split frame's tail was delivered as though it were a whole frame, and the
  test that claimed otherwise supplied only one fragment.

* `ethernet_rx` returned without counting for a short frame and for a
  downed interface, so `/proc/net/dev` understated receive drops.

* `FRCSPD` and `FRCDPLX` were left as the EEPROM set them while auto-speed
  detection was requested, which is only permitted while both are clear.
  `TCTL.RTLC` was unset, so a late collision on a half-duplex link was
  reported as a success rather than retried.

* The shutdown contract was not true as written. `NETDEV_TX_OK` means the
  descriptor was posted, not that the frame left, so the goodbye line could be
  discarded in the ring it had just been placed in. `stop` now drains what the
  card accepted before silencing it, and the host model has a transmit engine
  that finishes late so the drain has something to wait for.

* Three test defects beyond those already named: the receive model wrote into
  the ring without consulting `RDT`, so it would feed a driver that had
  stopped returning descriptors; the stop-order check read a flag rather than
  the write order, so revoking DMA first would have passed; and "no driver is
  called at all" checked three of the eight things it could have.

Three of the mutations written to prove the fixes failed to prove anything at
first. Two named behaviour that had been implemented but never asserted, and
one did not actually violate the order it claimed to. All three were the
mutation's fault rather than the code's, and finding that is what the exercise
is for.

## Found while finishing, not this milestone's work

* The `crash` suite leaks its machines. After a run that reports `PASS in 12s`
  it leaves three orphaned `qemu-system-i386` processes with `crash-test.disk`,
  parented to init and burning about one percent of a processor each,
  indefinitely. Fifteen were alive on this machine when the milestone
  finished: five runs' worth, the oldest nearly three days old and predating
  this work. `crash.sh` has a cleanup trap and kills `$QPID`, but that holds
  one pid while the suite starts a machine per phase, so the earlier ones are
  forgotten. Nothing in milestone 32 touched that suite; the leak was found
  because a check for stray processes at the end of the run turned up more
  than it expected. Fixed: `qemu_disk` backgrounds a shell function, so `$!`
  held the subshell's pid and never QEMU's; killing the subshell left QEMU to
  init. The function now `exec`s, so the backgrounded shell is the machine.
  `tests/run` also censuses the running machines either side of every suite
  and fails one that leaves a new machine behind, which turns this class of
  defect from a silent leak into a failing run. Both directions are proved:
  putting the defect back makes the runner report three leaked machines.
