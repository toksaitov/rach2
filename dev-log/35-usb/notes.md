# Milestone 35 notes

## Phase 1

The tree at the start of the run, `git log --oneline -12`:

    d86353e Plan milestone 35, USB
    1efaf6a Record what the documentation pass found
    b659664 Record the contracts milestone 34 changed in their own Readmes
    94ab2c0 Say what the test system is now in the docs that describe it
    7bf92e0 Name the guest checker the tree actually builds
    b85f6b9 Drop the host-test stubs for a serial writer that is gone
    071324a Say in the messaging comments what the code does now
    568be4d Let the suite makefile own its own phony targets
    0956908 Record the last review
    c851d8f Say in the record what the reader cannot look up
    5b62dd8 Say what a passing run means and what it does not
    97e64b8 Say what the root makefile holds now, not only what it held

The tree was clean and one commit ahead of the remote, which is the plan.

### The inventory

`rach2/sys/usb/usb.c` reads configuration space and records one line per USB
controller function, plus a routing line for an Intel xHCI. It is called from
`kernel.c` after the board and mouse probes and before `init_syscall`, which is
the earliest point where the PCI scan is complete and the log is running.

Two additions the plan did not name, both reads and both recorded here rather
than done silently:

* The SuperSpeed routing pair, `USB3PSSEN` at 0xD8 and `USB3PRM` at 0xDC, is
  read beside the USB2 pair. The plan named only `XUSB2PR` and `XUSB2PRM`. The
  owner's routing decision in phase 6 is about the notebook's USB3 receptacles
  as much as its USB2 pins, and the extra read costs nothing.
* The record names the interface (`uhci`, `ohci`, `ehci`, `xhci`, or `other`)
  as well as the raw byte, so a reader does not have to know the encoding.

The interface names were checked against real controllers rather than against
the specification alone: booting QEMU with `usb-ehci`, `piix3-usb-uhci` and
`pci-ohci` attached at once produced one correct line each.

`rach2.nousb` is not wired yet. There is nothing to switch off until there is a
driver, and phase 5 adds the switch with the `subset` boot that checks it.

Proof of the phase: with `-device qemu-xhci` the kernel records the controller;
with no USB device on the command line it records `controller: none`. Both
lines came from a real boot.

## Phase 2

`hcd.h` is the interface, `xhci.c` is bring-up as far as the controller's own
reset, and `xhci_hosttest.c` drives it against a model. The driver is five
steps a caller runs in order, each returning 0 or -1 with `xhci_error` set:
discover, read the capabilities, take it from the firmware, halt and reset it,
and give it its scratchpad pages.

Two things the plan did not settle, decided here:

* The capability registers are read before the firmware handoff, not after.
  The handoff needs the extended capability list, whose head is a field of
  `HCCPARAMS1`, so the order is forced. The header says so.
* The mapped aperture is a fixed 64 KiB, and `xhci_read_caps` refuses a
  controller whose ports, doorbells or runtime registers fall outside it. The
  alternative was to size the BAR by writing all ones to it and reading the
  mask back, which is the usual method and which disturbs a controller the
  firmware may still be driving. Refusing what does not fit costs nothing and
  touches nothing.

`XHCI_MAX_SCRATCHPADS` is 32, which is 128 KiB of the kernel's own image. No
controller met so far asks for any. If the notebook asks for more, phase 6
raises the number rather than guesses now; the driver names both numbers when
it refuses.

The plan asked for a handoff row that answers slowly as well as ones that
answer at once and never. The first version of the model had only the two, so
a driver that asked for ownership and read the answer once would have passed;
the model now takes a number of waits before the firmware gives up its claim,
and the mutation below proves the row.

Eight mutations, each applied from a script that restores the source under a
trap and rebuilds before the verdict:

* skip the ownership wait: caught by `firmware that never answers is refused`
* ask for ownership once and read once: caught by `firmware that answers slowly
  is waited for`
* ignore the ready bit: caught by `a controller that never becomes ready is refused`
* drop the reset pause: caught by `the reset pause is taken before it is read again`
* assume 32-byte contexts: caught by `64-byte contexts are noticed`
* skip the scratchpad array: caught by `with three pages`
* read one half of the scratchpad count: caught by `and is both halves`
* accept any scratchpad request: caught, by AddressSanitizer rather than by a
  row. The mutation writes past the compiled array, which is what the refusal
  exists to prevent, so the sanitizer reaching it first is the check working
  through a different door. The row `and refused` fails in the same run.
