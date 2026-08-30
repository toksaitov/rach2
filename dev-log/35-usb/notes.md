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

### Review 1 of 6

Seven findings against `hcd.h`, `xhci.h`, `xhci.c` and the host test. Six were
right and are fixed; one was right about this kernel and wrong about the
defect.

* The interface could not express an asynchronous command. `device_open`,
  `device_address`, `endpoint_open` and `endpoint_reset` each need a command
  the controller executes, and each returned a value as if the work were done.
  `hcd.h` now says one command may be in flight and adds `command_done` beside
  `poll`, which is what lets the core in phase 4 be the state machine the plan
  requires. This was the most serious finding: the interface is this phase's
  deliverable and the shape was wrong.
* The controller may not be written to while it says it is not ready. A
  controller coming out of D3, or out of a reset the firmware began, can report
  `CNR` for milliseconds, and the driver wrote `USBCMD` at once. There is a
  bounded wait before the halt now.
* Ownership is two bits. The driver waited for the firmware's claim to clear
  and never checked that its own had been taken. A controller that drops the
  request would have been driven by a kernel that did not own it.
* The capability list was walked without a bound. A next-pointer only moves
  forward, so the mapped window is the bound: each step is now checked against
  it and a list that reaches the end is refused. The first version of this fix
  also counted steps, which is unreachable given a forward-only list, so the
  counter went.
* The aperture check checked where each register set began and not where it
  ended. The doorbell array is a word per slot and the interrupters are 32
  bytes each; both ends are checked now.
* Several rows could not fail for the reason they named: the PCI stub ignored
  the class it was asked for, nothing recorded that the driver enabled memory
  space and bus mastering, and the model answered registers normally for a
  function still in D3. The model does all three now, and a controller in D3
  returns all ones as the hardware does.
* The interface version and the event-ring segment limit were read and never
  checked. They are checked.

The finding that was wrong about the defect: `map_mmio`'s result was said to be
discarded in favour of the physical address. It is, and in this kernel that is
correct, because `map_mmio` identity-maps and `ahci.c` does the same. The
driver now uses the returned address anyway: it costs nothing and it stops the
driver depending on a promise made in another file.

Six further mutations, all caught: skip the power-up, check only the firmware's
own ownership bit, write the command register before the controller is ready,
check the head of the capability list and no step after it, check only the
start of each register set, and assume the interface version.

## Phase 3

The rings, the ports and the commands. `xhci_start` programs the context array,
the command ring, the event ring and its one-entry segment table, tells the
controller how many slots this kernel will use, starts it, and powers any port
the controller does not power itself. `xhci_events` drains the event ring from
the timer, `xhci_command_issue` and `xhci_command_done` are one command at a
time with a deadline, and `xhci_port_status`, `xhci_port_reset_begin` and
`xhci_port_reset_done` are the ports.

Decisions the plan left open:

* The driver keeps its own clock in milliseconds, advanced by `xhci_advance`:
  one from the boot pump, ten from the timer. Deadlines are counted against it.
  The alternative was to read `kernel_ticks`, which would have made the driver
  untestable off a machine.
* `XHCI_SLOTS` is four and `CONFIG.MaxSlotsEn` is the smaller of that and what
  the controller offers. QEMU offers 64. Asking for all of them would have the
  controller reserve context memory for devices this kernel has no table for.
* The boot path waits, once, for the hundred milliseconds the USB
  specification asks for between a connect and a reset. It is the only wait,
  and it is in bring-up where the plan allows one.

What QEMU reports, now in `hardware.md`: version 1.0.0, eight ports, 64 slots,
32-byte contexts, no scratchpad pages, and no legacy-support capability, which
is why the firmware handoff can only be proved in the model.

Proof from four boots: a high-speed keyboard and mouse are recorded on ports 5
and 6; the same keyboard attached with `usb_version=1` is recorded as
full-speed, which is what says the speed is read rather than assumed; a
controller with nothing plugged in records `port: nothing connected`; and a
machine with no controller records `absent`.

Ten mutations, each caught: enable every slot the controller offers, write the
command and not ring the doorbell, link back without the toggle bit, keep one
cycle value on the command ring, keep one cycle value on the event ring, read
the events and not say how far, write the port register back as it was read,
leave the ports unpowered, let a command run for ever, and let a port reset run
for ever.

The suites that touch what changed all pass: `default`, `console`, `reset`,
`shutdown`, `subset` and `metal`, beside `style` and `host`. `usb_stop` sits
beside `netdev_stop` in `kernel_shutdown`, which is why `reset` and `shutdown`
are in that list.
