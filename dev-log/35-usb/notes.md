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
