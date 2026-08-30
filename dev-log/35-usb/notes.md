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

### Review 3 of 6

Eight findings. Six were right and are fixed; two are recorded with what was
done about them.

* A slot was forgotten before the controller gave it back. `xhci_device_close`
  cleared the slot and its context pointer and then asked for a Disable Slot,
  so a controller that still held the slot was pointed at a context this kernel
  had let go. Closing is now two halves: the endpoints stop taking work at
  once, and the slot and its context stay the controller's until the command
  completes. `core.c` has a closing state that waits for it.
* A ring reused by a second device still held the first device's blocks, with
  the cycle value the controller was looking for and addresses belonging to
  nobody. Rings and device contexts are cleared before they are handed over.
* A device that was unplugged stayed in the table, kept its slot and blocked
  the port. Every step now notices a port that has gone and takes the device
  down. A connect that never settles gives its entry back after five seconds
  rather than holding it for ever.
* The shared input context was filled before the command ring was known to be
  free, so a refusal could rewrite what a command in flight had given the
  controller. Every filler asks `xhci_command_busy` first. The command in
  flight also remembers what it was, so a completion is read as the answer to
  that command rather than by what it happens to carry: a Disable Slot
  completion could otherwise have been taken for an Enable Slot's answer and
  resurrected a slot.
* A control transfer reported nothing about how much arrived, because the
  event was asked for on the status stage. It is asked for on the data stage
  now, which is the stage whose event carries the residual, and every step that
  reads a descriptor refuses one that came back short. The configuration is
  walked only as far as it actually arrived.
* A completion for a slot the controller no longer holds is dropped rather than
  reported, so a slot handed to the next device cannot be advanced by the last
  one's transfer.
* The endpoint's packet size took all sixteen bits of a field whose low eleven
  are the size. An interface on an alternate setting, a boot protocol that is
  neither keyboard nor mouse, a configuration numbered zero and an endpoint
  numbered zero are all refused now, each with a row.
* The pure test had a row that changed a byte to the value it already had, so
  it tested nothing. It steps over an unknown descriptor by its length now, and
  there are rows for each of the refusals above.
* `usbprobe` searched the whole file for the words it wanted, so a file whose
  first line held all of them and whose second was empty would have passed. It
  reads line by line now.

Two findings are recorded rather than closed. Matching a completion to the
exact block that was submitted, rather than to the device and endpoint, needs a
per-transfer tag this driver has nowhere to put yet; the slot-reuse case it
guards against is closed by dropping completions for slots the controller no
longer holds. And the endpoint recovery is still two primitives rather than the
whole sequence: `CLEAR_FEATURE(ENDPOINT_HALT)` and Stop Endpoint belong with
the first transfer that can stall, which is phase 5.

## Phase 5

The input layer first, with no USB in it. What was inside the PS/2 interrupt
handler is now `sys/input.c`: the modifier state, the Ctrl+C interception, the
scrollback keys, the translation and the choice between the line discipline and
the debug shell. The handler keeps what is particular to the 8042, which is
reading the port. The shell's own three pieces stayed in `kernel.c` behind
`shell_reads_input`, `shell_input_char` and `shell_interrupt`, because the
buffer they touch is the shell's. `ttyprobe`, `stdintest`, `jobprobe`, the
`console` suite, `exitline` and the default boot all passed unchanged, which is
what says the move changed nothing.

Then `hid.c`. A report is a state and a scancode is an event, so each report is
compared with the one before it and the difference is what the input layer is
given, in the set-1 codes the PS/2 keyboard already produces. The mouse's
vertical movement is turned over, because a USB mouse's Y grows downward and
the queue it joins grows upward.

The host test found a defect the moment it existed: a report of all zeros, which
is a keyboard saying nothing is down, was being dropped as if it were the
rollover report. Every key held would have stayed held for ever. The rollover
report is the one whose slots are filled with 0x01, and that is what is checked
now.

The mouse device is opened by the bare name `mouse` rather than a path under
`/dev`, which is how Doom opens it and the only way the kernel offers. The
probe does the same, and the Readme says so.

The suite is six boots. Three of them take the 8042 away with QEMU's
`i8042=off`, which is what makes the answers mean anything: with a PS/2
keyboard present, a typed key could have arrived by either road.

* a high-speed keyboard and mouse enumerate and are listed
* the same at full speed, which is the speed a real keyboard uses
* a key typed through the emulator's monitor reaches the shell, on a machine
  with no other keyboard: `sendkey h e l p` and the shell prints its help
* a mouse moved through the monitor arrives at the mouse device
* a keyboard plugged in after the boot is enumerated, and pulled out is taken
  down
* with `rach2.nousb` the file is there and empty

The harness gained the monitor connection this needs: a unix socket beside the
serial line, and the steps `key=`, `mouse=`, `usb=add:` and `usb=del:`. Nothing
else can produce those events, because they come from the emulated hardware
rather than from anything the guest can be asked to do. The socket is opened
only for a run whose steps need it, so every other suite's command line is
unchanged. `harness --selftest` covers the mouse step's parsing.

Mutations, all caught: send make codes and no break codes; translate the
rollover report; forget the previous report; keep the vertical movement as the
device sends it; let a device go without releasing what it held; and cut the
USB path out of `input_key` entirely, which fails the typing boot and is what
says that boot measures the path this milestone built.

### Review 4 of 6

Seven findings. Four are fixed, three are recorded.

* A USB keystroke never reached the ring a graphics-mode reader drains, so a
  USB keyboard could not have played Doom. The push moved into `input_key`,
  where both keyboards pass, and out of the PS/2 handler.
* The rollover report threw away the modifier byte with the keys. A shift
  pressed as the keyboard lost count stayed pressed for ever. The modifiers are
  read first and the key array alone is dropped.
* It recognised only the first of the three error usages. All three mean the
  same thing and all three are recognised.
* The suite proved the debug shell's road and not the terminal's, which is the
  one every program on the machine reads through. There is a boot for it now:
  `stdintest` reads three lines typed through the emulator's monitor.
* The mouse check accepted any movement on either axis, and the suite moved the
  mouse by the same amount on both, so swapped axes or a turned sign would have
  passed. The movement is now different on each axis and the probe requires the
  direction of both.
* The host test counted codes where it should have named them. It names them.

Three recorded rather than closed:

* The modifier state is one, not one per keyboard. Two keyboards would share a
  shift: releasing it on either would release it for both. This kernel holds
  one keyboard, which is one of its stated limits, and the header says so.
* A key held while the keyboard is unplugged is released by `hid_gone`, and
  that is proved in the host test rather than on a machine. QEMU's `sendkey`
  presses and releases in one command, so no boot can hold a key across an
  unplug.
* A held USB key does not repeat. The PS/2 keyboard's repeat comes from the
  hardware; a USB host is supposed to make its own, and this one does not. It
  is a real gap and it is named in the Readme.

Two further mutations, both caught: drop the modifiers with the rollover, and
notice only the first error usage. A third, keeping the raw ring for the port
keyboard alone, is not caught by anything: no test reads that ring, and the
only reader is Doom. Recorded rather than left implied.

## Phase 7

The documents, the measurements and the record.

A defect in the test system came out of the full run rather than out of a
review: `run-usb` built the harness kernel in its own recipe, the way
`run-exitline` already did, and with two suites doing it the pool ran two
`make`s in `rach2/` at once. They wrote over each other's link response file
and the kernel failed to link with `EN_us` and `kbd_push_scancode` undefined,
which failed `selfbuild` and, through a half-built image, two `dhcp` suites.
The harness kernel is built by `prepare-all` now, before the pool starts, and
neither suite builds anything.

The `usb` suite joined the core tier. The tier's rule is that every subsystem is
exercised at least once, and nothing else in it touches USB; the suite is 39
seconds and runs beside the others, so `make test` is 172 seconds either way.

The documentation pass, with a Codex read of the five reader-facing documents,
returned ten findings. Seven were mine and are fixed: the kernel Readme still
said USB was not implemented; the limits table said four devices where the
controller's slot zero is its own, so three; it said one keyboard and one mouse
where the real limit is the slots and the shared modifier state; it said
`endpoint_open` is generic over control as well, where the control endpoint is
made while the device is addressed; the root-program count and the suite count
were both a milestone behind; and the tests Readme said the `usb` suite runs
both probes at both speeds with the 8042 off, where only three of its boots
switch it off. One finding was about `rach2.resetskip`, which claims to skip the
first N reset methods when the last of the three always runs; that is older than
this milestone and the line is corrected. The comments on the scancode ring said
the IRQ handler fills it, which is the input layer now.

Two full runs each had one failure that did not reproduce alone, and the third
was clean. Both are timing failures under twenty workers: `musl/pwauthmusl`
inside `selfbuild`, and a sixty-second wait in `raw-e1000`. Recorded in the
milestone record rather than left out.

## What is still open

Phase 6, the notebook, and with it phase 1's last item and reviews 5 and 6 of
6. Nothing about it can be done from here: it needs the machine, a USB keyboard
and mouse plugged into it, and the owner's decision about the chipset's port
routing with the real register values in front of them. Everything it needs is
ready and is listed in `current.md`.

The plan says this is the owner's to close, not the milestone's: with the
evidence in front of them they decide whether it closes with USB working in
QEMU alone or stays open for another attempt.

### Review 6 of 6

Ten findings against the milestone record, and two of them were about the code
rather than the prose.

The slot tables were a slot short. `CONFIG.MaxSlotsEn` asked the controller for
four, and every bound in the driver read `device >= XHCI_SLOTS`, so slot 4, which
the controller is entitled to hand out, would have been refused as if it did not
exist. Slots are numbered from one; the tables have one more entry than the
count now, and a row proves that the last slot is addressed like any other.

The model never drove a transfer. It answered commands and posted port changes,
and `xhci_submit` and `xhci_poll` were exercised only by QEMU, while the record
claimed the control transfer, the short packet and the stall were proved
against it. The model has endpoint rings now: it walks them on a doorbell the
way a controller does, finds the block that asked for an event, and answers
with a completion code and a residual of the row's choosing. Twelve rows and
three mutations followed, including the one that matters most: asking for the
event on the status block rather than the data block, which is what a driver
does when it does not care how much arrived.

The rest were the record claiming more than the tests give, and are corrected in
it: low speed is not enumerated anywhere, only derived from; the port routing is
read and reported and has never been written or tested; `endpoint_open` does not
take a control endpoint; bring-up does wait, and it is the timer's path that
does not; and the two full-run failures are described as what was observed
rather than as a cause nobody established. Six known limits that were in
`notes.md` and not in the record are now in the record.

Four phrases went for the register rule: a fragment, an effect sentence, an
invented shorthand, and a controller given the intention to lie.

### Review 5 of 6

The plan gave this review to phase 6. The machine is not available, so it was
run against what phase 6 will meet: the hardware path in the driver, the
acceptance script, and the checklist for the session. Eight findings.

Three were code and are fixed.

* The handoff had no way back. The driver took the controller from the
  firmware, turned the firmware's own interrupts off, and if anything after
  that failed it kept both. On a notebook whose firmware is emulating a PS/2
  keyboard out of a USB one, what it kept was the keyboard. `xhci_release`
  restores the legacy-support registers to what they held, and it is called
  when bring-up fails and when the machine stops.
* `xhci_stop` wrote the command register whenever a controller had been found,
  including one that refused its own reset or was never started. It writes
  nothing now unless the controller is running, and it says so when a halt does
  not take.
* A function taken out of D3 was read immediately. It gets the ten milliseconds
  the specification allows and its power state is read back.

The raw register words a session needs are recorded now: the two
legacy-support registers before and after the handoff, and the three capability
words with the scratchpad count and context size beside them. A person at that
machine takes the log away, and reading numbers out of prose is not the same as
having them.

The acceptance script could report `PASS` on a machine whose controller was
refused. It fails on a refusal now, says which of the four things happened, and
keeps `usb: controller: none` as an ordinary answer rather than a fault.

The checklist gained three corrections. The step that asks whether the firmware
already types for you booted the kernel with the stack running, so a key that
appeared might have come through it; it boots with `rach2.nousb` now. VT-d is a
stop rather than a note: this kernel gives the controller physical addresses and
installs no mappings, so with translation on it hangs rather than explains. And
the routing step said the owner decides whether the driver may write the
register, which implied there is a writer; there is none, and building one is a
code change with its own review, so the step is a decision about whether to
build it.

Two findings are recorded rather than closed. The BAR's decoded size is never
established: the driver maps 64 KiB and refuses a controller whose registers do
not fit, which is not the same as knowing the aperture is that large, and
sizing a BAR the firmware may still be driving is not something to do during a
first session. And with VT-d enabled the driver has no answer at all; the
checklist stops the session instead.
