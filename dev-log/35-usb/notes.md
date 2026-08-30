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

### Review 2 of 6

Seven findings against phase 3, all of them right, all fixed.

* The command abort was not the state change the specification describes. The
  driver wrote the abort bit, called the command finished and let the next one
  be issued as soon as that bit read clear. The bit is write-only and reads
  zero, so the guard never fired. An abort is a request: the controller stops
  the ring and says so by clearing the running bit, and only then is there a
  ring to use. The driver waits for that and then points the ring at the block
  it will use next.
* A completion event was accepted whatever it named. A late answer to a command
  already abandoned would have completed the next one. The event names the
  block its command was in, and that is matched now.
* A controller with more than sixteen root ports would have written past the
  per-port tables. The count is capped at what this kernel has state for, with
  a record naming both numbers.
* The debounce interval was defined and never used, and the port-change flags
  were set and never read. There is a per-port connect time now, and
  `xhci_port_settled` is what the layer above asks before it resets a port.
* Nothing separated filling a block from ringing the doorbell, or reading a
  cycle bit from reading the rest of the event. There are compiler barriers at
  both points and before the controller is started. x86 gives the ordering; the
  barrier is against the compiler, which cannot see the other party.
* The command-wrap rows passed for the wrong reason. The model took the block
  before the driver's enqueue pointer to be the command just written, which
  after a wrap is the link block. It follows a dequeue pointer of its own now,
  through the link block and the cycle change, the way a controller does.
* Deadlines were compared with `>=` against a clock that wraps after seven
  weeks. They are signed differences now.

The model also gained: a command ring register whose abort and stop bits are
write-only and whose running bit is the controller's, an abort that can be made
not to complete, and rows for the event ring segment table's contents, the
dequeue pointer's address rather than only its handler bit, the port count cap
and the debounce.

Four further mutations, all caught: answer the command in flight with any
completion, call the abort finished the moment it is asked for, let a connect
be acted on at once, and trust the controller's port count.

## Phase 4

Enumeration. `core.c` is the state machine and knows no register; `xhci.c`
gained the device, endpoint and transfer half it drives. A device goes from a
connected port to a configured one in twelve steps, none of which waits: settle,
reset, Enable Slot, Address Device, the first eight bytes of the device
descriptor, Evaluate Context where the control endpoint turns out to be a
different size, the whole device descriptor, nine bytes of the configuration,
the whole of it, Configure Endpoint, `SET_CONFIGURATION`, running.

Two decisions the plan left open:

* One device is enumerated at a time. The controller takes one command at a
  time and this driver keeps one command in flight, so a second device waits at
  its port. That costs a hundred milliseconds and no complexity.
* The core is given the time that passed rather than reading a clock, so its
  deadlines are the caller's units: one millisecond from the boot loop and ten
  from the timer.

The defect this phase found, and it was mine: the boot loop and the timer were
both draining the event ring and both stepping the state machine. The timer
interrupts the boot loop, so two drains of one ring ran inside each other and
two devices were enumerated over the top of one another. One device was
addressed with the other's input context; which one failed changed from boot to
boot. The boot loop now waits and nothing more, and the timer does all of it.
The same class of fault was waiting one line further on: `usb_poll` runs from
the first tick of the boot, long before there is a controller, and the event
ring it would have read is a zeroed array whose cycle bits look like the
controller's. There is a flag for that now.

`/proc/usb` is one line per configured device: port, speed, identifiers, class
and protocol. A machine with no controller has the file and it is empty.
`usbprobe` reads it and requires a keyboard and a mouse. Adding the file
touched five places, which is what a synthetic file costs here: the classifier,
the vnode kind, the inode, the renderer and the directory listing.

Proof in QEMU, at both speeds QEMU can produce: a keyboard on port 5 and a
mouse on port 6 are recorded with their identifiers, class and protocol; the
count is two; `usbprobe` exits zero from the guest.

`tests/host/usb_hosttest.c` is the pure half: the descriptor walk over
truncated, overlong, zero-length and self-shortened descriptors, in buffers
allocated to the exact length under AddressSanitizer. The interval and packet
derivations are rows in `xhci_hosttest` instead, because that is the file the
code is in.

Mutations, all caught: read a descriptor past the end of what arrived (by the
sanitizer), take any interface that says HID, take an endpoint whatever
direction it is.

### Phase 4's mutations

Nine, each caught by the check it was aimed at:

* read a descriptor past the end of what arrived: by AddressSanitizer
* take any interface that says HID
* take an endpoint whatever direction it is
* take the slot the command named rather than the one the controller answered
* point a device's context nowhere
* reset an endpoint on a device the controller never gave out
* derive every interval as if the device were high-speed
* change the whole context instead of the one field
* send `SET_CONFIGURATION` before Configure Endpoint: caught in QEMU rather
  than in the model. `usbprobe` fails, and the log says the device offers no
  interface this kernel can drive, because the endpoint was never configured.
  The order is not visible to the controller model, which sees commands and
  not the transfers between them; phase 5 gives the model transfer rings and
  the row can be written there.

Two limits recorded rather than closed:

* The core's table of four devices is not filled by any test. The controller
  refuses a device beyond its own table, which has a row, but the core's own
  refusal needs five devices on one controller and QEMU's has eight ports;
  phase 5's suite runs two. A row for it needs a driven state machine with a
  model host controller, which is more machinery than the limit is worth here.
* The Evaluate Context step is exercised in the model but not on a machine:
  QEMU's HID devices report a control endpoint of eight bytes at every speed,
  which is the size assumed before asking, so nothing needs correcting. A
  device that reports otherwise would take that path on the notebook.
