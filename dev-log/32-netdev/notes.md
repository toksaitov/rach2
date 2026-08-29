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
  no `host-deps:`, as `rtl8168.c` and its model already do. Any future host
  test that names `kstring.c` as a dependency inherits the same constraint.

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

* The e1000 driver's rings are `volatile` and the other two drivers' are not.
  The kernel builds every driver without optimization, so none of them can
  lose a descriptor read to a register. The e1000 is different only because its
  host model compiles the same file at -O1 and -O2, where the guarantee comes
  from the type rather than from a flag. Making the other two match would be a
  change to drivers this milestone was not asked to alter, and neither is
  wrong as built.
