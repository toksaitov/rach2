# Milestone 35: USB

## The goal

The kernel gets a USB stack: one host-controller driver, a device core, and a human-interface class driver, so that a USB keyboard and a USB mouse work in QEMU and on the Lenovo G50-70. The stack is layered so that a second controller or a mass-storage class driver can be added later without changing what is written here. Mass storage is not written in this milestone, and that is a decision rather than a deferral.

The milestone is complete when the stack works in QEMU at both device speeds and on the laptop. The one case in which the laptop half may be closed without working is written under "If the laptop refuses", and only the owner closes it.

This plan is the authority for the run. A conflict between this plan and the current tree is resolved by the decisions here; a conflict between this plan and `CLAUDE.md` is resolved by `CLAUDE.md`. The plan was revised once after an independent review, which corrected the enumeration order, the input integration, the execution context, and what phase 1 can measure.

## Why xHCI alone

The laptop is Haswell-ULT with the Lynx Point-LP chipset: an xHCI at `00:14.0`, `8086:9c31`, and one EHCI at `00:1d.0`. QEMU offers `qemu-xhci` beside the legacy UHCI, OHCI and EHCI models. xHCI is therefore the only interface that exists on both targets, and it is the only one still put on new silicon.

It is also the only one that reaches a low-speed keyboard without a second mechanism. On xHCI the controller handles low, full, high and SuperSpeed alike: no companion controller, no transaction translator, no split transactions. On Lynx Point the EHCI reaches a low-speed device only through its integrated rate-matching hub, so an EHCI route to a keyboard costs a hub class driver and split transactions on top of EHCI's periodic schedule, to reach a device xHCI reaches directly.

UHCI is the simplest controller and the easiest to debug, and writing it first is a real option. It is refused because it exists only in QEMU: it would be a second schedule, a second addressing model and a second recovery path with no use on the machine this kernel is carried to. The debugging it would buy is bought instead by the host model in phase 2, which can make a controller fail on demand in ways no emulator will.

## Decisions on the stack

### The layers

Five files. Four of them never touch a register.

* `sys/usb/hcd.h`: what a host controller provides. `ports`, `port_status`, `port_reset_begin`, `port_reset_done`, `device_open`, `device_address`, `device_close`, `endpoint_open`, `endpoint_close`, `endpoint_reset`, `submit`, `poll`. `endpoint_open` is generic over control, interrupt and bulk from the first line, so a mass-storage class driver later adds no controller code.
* `sys/usb/xhci.c` and `xhci.h`: the one implementation.
* `sys/usb/core.c` and `core.h`: devices, interfaces, endpoints, the enumeration state machine, the standard requests, descriptor validation, configuration choice, class matching. Fixed tables, no allocation, no registers.
* `sys/usb/hid.c`: the boot-protocol keyboard and mouse, and the translation into the kernel's input layer.
* `sys/usb/Readme.md`: the contract, written in the phase that establishes it.

Addressing belongs to the controller, not to the core. xHCI's Address Device command replaces the `SET_ADDRESS` request, so the core asks the HCD to address a device and never sends that request itself. A UHCI driver added later sends it inside its own `device_address`.

### Nothing blocks

This is the decision the rest of the design follows from. The event ring is drained from the 100 Hz timer handler, which runs with interrupts off, so nothing on that path may wait: not a port reset, not a control transfer, not a command completion, not a millisecond delay.

So `submit` queues work and returns, `poll` harvests completions, and enumeration is a state machine that advances at most one step per call. A step is one of: issue a command, look for its completion, queue a transfer, look for its completion, begin a port reset, check whether a reset finished. Each device carries its state, its current step, and a deadline counted in ticks; a step that does not complete by its deadline fails the device with a record and frees its slot.

At boot the kernel pumps the same state machine in a bounded loop with interrupts on, so the devices present at reset are enumerated before the first process starts. Afterwards the timer pumps it, one step per tick, which is what makes hotplug work. There is one enumeration path, not two.

The only waits in the driver are in bring-up, which runs once at boot in thread context: the reset, the ownership handoff, and the fixed one-millisecond pause after `HCRST` that Intel controllers require before their registers may be read again.

### The controller, and what of it is written

Written: PCI discovery by class `0x0C`, subclass `0x03`, interface `0x30`; the function put in D0 through its power-management capability before any register is read; memory space and bus mastering enabled; BAR0 mapped as a whole aperture and refused if it is a 64-bit BAR above 4 GiB or too small for the register sets it declares. `CAPLENGTH`, `HCIVERSION`, `HCSPARAMS1`, `HCSPARAMS2`, `HCCPARAMS1`, `DBOFF`, `RTSOFF` and `PAGESIZE` read rather than assumed, and `PAGESIZE` refused if it does not offer 4 KiB. The extended capability list walked from `HCCPARAMS1.xECP`: capability 1 is the legacy-support handoff, capability 2 says which root ports are USB2 and which are USB3. Ownership taken from the firmware. Stop, reset, the one-millisecond pause, then `HCRST` and `CNR` waited out. `CONFIG.MaxSlotsEn`, the device-context base address array, the scratchpad pages and their pointer array, the command ring, and interrupter zero's event ring with a one-entry segment table. Ports powered where `PPC` says they must be, then reset after the debounce the USB specification requires between a connect and a reset.

Then, per device: Enable Slot; the slot and endpoint-zero input contexts; Address Device; the first eight bytes of the device descriptor; Evaluate Context where a full-speed device's endpoint-zero packet size turns out different from the guess; the whole device and configuration descriptors; Configure Endpoint; and only then `SET_CONFIGURATION`. That order is required: the controller allocates the endpoint and its bandwidth first, and the request that tells the device to use that configuration comes after the command that succeeded. A Configure Endpoint refused for bandwidth or resources refuses the device and says which, rather than sending the request anyway.

Configure Endpoint carries the fields the controller schedules from, each derived rather than copied: the interval converted from `bInterval` by the device's speed, the maximum packet size, the maximum burst, and the maximum payload per service interval.

Recovery is written, because an endpoint that stalls is an endpoint whose ring is unusable until it is reset: Reset Endpoint, `CLEAR_FEATURE(ENDPOINT_HALT)` for a non-control pipe, Set TR Dequeue Pointer to where the ring should resume, and Stop Endpoint for a transfer cancelled by a timeout or a disconnect. A command that does not complete by its deadline aborts the command ring; a command ring that will not abort fails the controller, which is recorded and leaves PS/2 alone.

Not written, each because nothing here needs it: MSI and INTx delivery, additional interrupters, more than one event-ring segment, streams, isochronous transfers, the Negotiate Bandwidth command, hubs, SuperSpeed device classes, and runtime power management.

Not skipped, whatever the temptation: a deadline on every wait, short-packet handling, stalls, disconnects during a transfer, and both context sizes. `HCCPARAMS1.CSZ` decides whether a context is 32 or 64 bytes, and QEMU and the laptop are not guaranteed to agree.

### Shutdown

The controller is halted on poweroff and on reboot, and any port routing this milestone changed is put back the way it was found. Linux marks this exact controller for spurious reboots and spurious wakeups if it is left running, and the kernel already has one place where a device is silenced before the machine stops: `netdev_stop` in `kernel_shutdown`. `usb_stop` goes beside it, and `tests/run reset` and `tests/run shutdown` are what prove the machine still stops and still restarts.

### Hotplug

A device plugged in after boot is enumerated and one unplugged is torn down. It is kept because a stack that enumerates only what was present at reset is not usable, and because `device_add` and `device_del` on the QEMU monitor make it the cheapest strong test in the milestone.

It is more than a Disable Slot command. A connect is debounced before the port is reset. A `PORTSC` write preserves the ordinary read-write fields and clears only the change bits it means to clear, which is a per-bit discipline and the easiest thing in the driver to get wrong. A removal cancels the endpoint's outstanding work, tolerates a Transfer Event that was already queued for a device that is gone, releases every key and button the device was holding, disables the slot, and only then frees the table entry. The suite covers a plug, an unplug, an unplug while a key is held, a replug on the same port, and a rapid replug.

### HID, and why boot protocol only

`SET_PROTOCOL(0)` and the two fixed report layouts: for a keyboard, eight bytes, being one modifier byte, one reserved byte and up to six key usages; for a mouse, three bytes of buttons and two signed movements, and a fourth wheel byte where the device sends one, which QEMU's `usb-mouse` does. No report-descriptor parser. That single decision removes the largest body of code in a USB HID stack, and it is what every firmware does. The cost is stated in the Readme: a device that offers no boot protocol is refused with a record saying so.

How many bytes actually arrived is the transfer's length less the residual the Transfer Event reports, not the endpoint's maximum packet size. A report shorter than its layout is dropped rather than read past.

### The input layer

The bridge is not two calls, because the path it must join does not exist yet. Today the PS/2 handler in `kernel.c` reads a scancode, pushes it to Doom's ring in `kbd.c`, and then does the shift, control and `0xE0` tracking, the table lookup, the Ctrl+C interception and the terminal delivery, all privately. The mouse driver injects packets into a queue only it can reach, and `/dev/mouse` opens only when the PS/2 mouse initialised.

So this milestone makes the input path shared, and that is real work in existing files rather than a claim that nothing changes.

* The translation and routing move out of the PS/2 handler into `sys/input.c` behind `input_key(uint8_t scancode)`. The PS/2 handler keeps the port reading and calls it. `hid.c` calls the same function.
* `hid.c` turns a report into scancodes: a make code for each usage that appeared since the previous report, a break code for each that went away, the modifier usages `0xE0` to `0xE7` mapped to the shift, control and alt codes, and the two-byte `0xE0` sequences for the navigation keys. The rollover and error usages `0x01` to `0x03` are dropped rather than translated. A device that disappears releases everything it was holding.
* `input_mouse(dx, dy, buttons)` accepts a packet from either driver, and `/dev/mouse` opens when any mouse exists rather than when the PS/2 one does.

`ttyprobe`, `stdintest`, `jobprobe`, the `console` suite and the Doom scancode path are the checks that this refactor did not change behaviour; they pass unchanged before the USB driver delivers its first key.

The bridge runs inside the timer handler, where the line discipline echoes through the console sink and that sink busy-waits on the UART. That is the wait `console_pump_irq` refuses to take for a serial burst. It is taken here for the same reason the PS/2 handler takes it: a report carries at most six keys, so the echo is bounded at a few bytes per tick rather than by what a sender chooses to send.

### Where it starts

The stack is brought up after PCI and before the network, and it emits the boot records every other subsystem emits: `usb: started` or `usb: skipped: rach2.nousb`, then one record per controller, one per port with a device, and one per device enumerated. `kernel.c` gains the bring-up call, the boot-loop pump, and `usb_poll` in the timer beside `netdev_poll`, `tcp_tick` and `ip_loop_drain`.

### Memory

Static, aligned, identity-mapped arrays in the kernel image, the way `ahci.c` holds its command list. No allocator: the tables are fixed, so the memory can be too. The device-context base address array, the input and device contexts, the event-ring segment table and the scratchpad pointer array each get the alignment the specification requires; each ring is one page.

The scratchpad count is read from `HCSPARAMS2` and a controller may ask for as many as 1023 pages, which is four megabytes on a machine limited to thirty-two. The driver compiles in a maximum, refuses a controller that asks for more with a record naming both numbers, and the host model has a row for each side of that limit.

A data buffer described by one TRB may not cross a 64 KiB boundary; the reports are eight bytes inside a page, so the rule is satisfied by construction and the Readme records it rather than leaving it to be rediscovered.

Rings are read and written by two parties, so every field the controller also touches is `volatile`, and a compiler barrier separates filling a TRB from ringing the doorbell, and reading a cycle bit from reading the rest of the event. x86 gives the ordering; the barrier stops the compiler from taking it away. QEMU will not fail if this is missing, so the review reads it rather than the tests.

### The fixed limits

Four devices, four endpoints each, two of which may be interrupt endpoints; sixteen root ports; one keyboard and one mouse; a compiled maximum for scratchpad pages. Each limit is reached by a probe or a model row and each refusal is required, the way milestone 34's limit probes require the process, descriptor, socket and control-block refusals.

### The boot-line switch

`rach2.nousb`, in the shape every other optional device has. The probe emits `started` or `skipped: rach2.nousb`, and `tests/run subset` gains that boot and requires the record, which is what makes the switch a checked contract rather than a claim.

### What stays out, and why it is a decision rather than a deferral

Mass storage, hubs, report-descriptor parsing, isochronous transfers, booting from a USB disk, and UHCI. The first two are the next milestone and the interface is shaped for them now. The rest have no reader in this tree. Each is named in `rach2/sys/usb/Readme.md` as absent, with the sentence that says why.

## Decisions on the hardware path

### Firmware ownership

The laptop runs InsydeH2O in CSM mode, so the firmware may hold the controller for its own PS/2 emulation of USB keyboards. The driver sets the OS-owned bit in `USBLEGSUP`, waits up to one second for the BIOS-owned bit to clear, then disables the SMI enables in `USBLEGCTLSTS` and clears their status bits. On a timeout it refuses the controller and says so, leaving PS/2 alone rather than racing system management mode.

QEMU's model does not implement that capability at all: it builds only the two supported-protocol capabilities. So this path cannot be exercised in QEMU. It is proved in the host model, where the handoff can be made to succeed, to be slow, and never to complete, and then it is proved once on the laptop.

### Port routing

On this chipset the USB2 pins are routed to whichever controller `XUSB2PR` at PCI configuration offset `0xD0` selects, masked by `XUSB2PRM` at `0xD4`. If the firmware left a port on the EHCI, the xHCI will not see it. Both registers are in configuration space, so they can be read before any driver exists.

Phase 1 reads and reports them on the laptop and writes neither. Whether the driver ever writes them is decided by the owner, with the values in front of them, in phase 6. A write is undone by a power cycle, so it is not the irreversible kind, but it changes the running machine and the owner decides it. Anything written is put back at shutdown.

### VT-d

The DMAR table exists on this laptop and firmware may enable translation, in which case registers answer and DMA does not. The same constraint the network card already has. Declared unsupported, required off, and the firmware's setting recorded in phase 6.

### Internal input is not touched

The laptop's keyboard and touchpad are 8042 devices and stay on the PS/2 driver throughout. Nothing in this milestone can leave the machine without input.

### If the laptop refuses

The firmware may lock the port routing, or hold the controller and never hand it over, or the ports may be on the EHCI with no way to move them. If that happens the milestone does not grow an EHCI driver to get around it. Phase 6 records what the machine did, with the register values, in `hardware.md`; the driver keeps refusing the controller and saying why; PS/2 remains the laptop's input. The owner is then shown the evidence and decides whether the milestone closes with USB working in QEMU alone, or stays open for another attempt. The milestone does not close itself on that question, and the record says which way it went.

## Decisions on tests and tooling

### The host model

`tests/host/xhci_hosttest.c` drives `xhci.c` against a model of a controller through a `struct xhci_io` of `r32`, `w32` and `udelay`, exactly as `ahci_hosttest.c` drives `ahci.c`. Each row drives one step and requires one answer, and the positive case is checked beside every refusal, which is the mistake milestone 34 made once and had to correct.

Rows, at least: a controller that resets and one whose `HCRST` never clears; one whose `CNR` stays set; a `PAGESIZE` without 4 KiB; a firmware handoff that completes at once, one that completes slowly, and one that never completes; a scratchpad request of zero, of several, and of more than the compiled maximum; both context sizes; a command ring that wraps through its Link TRB with the cycle bit flipping; an event ring dequeued to its end with `ERDP` written back; Enable Slot, Address Device, Configure Endpoint, and a Configure Endpoint refused for bandwidth; a control transfer in three stages; a short packet reported through the residual; a stalled endpoint reset and used again; a transfer that never completes, stopped and reissued; a command that never completes, aborting the ring; a port that reports a device and one that does not; a connect that bounces; and a disconnect with a transfer outstanding.

`tests/host/usb_hosttest.c` is pure: descriptor walking over truncated, overlong and self-referential descriptor sets under AddressSanitizer; configuration choice; interval and packet-size derivation for a low-speed, a full-speed and a high-speed device; the fixed tables filled and the refusal required; boot report decoding including a short report; and the report diff that turns state into make and break scancodes, including rollover, a modifier, a navigation key with its `0xE0` prefix, a key held across reports, and a disconnect releasing what was held.

### The QEMU suites

`tests/run usb` boots with `-device qemu-xhci,id=xhci` and both devices bound to that controller by name, on a machine with `i8042=off`, so the PS/2 keyboard and mouse are not present to satisfy a check the USB path was supposed to satisfy. It requires, in order: the enumeration records for both devices; `/proc/usb` naming them with the right speed; a key sent through the QEMU monitor arriving at the guest terminal; a mouse movement arriving as a packet; a device added after boot being enumerated; one removed being torn down; and a key held across an unplug not being stuck down afterwards.

The suite runs twice, once with `usb_version=2` and once with `usb_version=1`, which is how QEMU offers a full-speed device and therefore the only way in an emulator to cover the speed a real keyboard actually uses.

`tests/run subset` gains the `rach2.nousb` boot. `tests/run procfiles` gains the `/proc/usb` case. The registry gains the row and the tier decision is made in phase 7 with a measurement, not a guess.

### `/proc/usb`

One line per enumerated device: port, speed, vendor and product identifiers, class, and the product string where the device offers one. It is the guest's view of the stack and the thing `usbprobe` reads. A machine with no controller, or one booted with `rach2.nousb`, has the file and it is empty; that is the contract, and the `rach2.nousb` boot requires it, because a file that is missing and a file that is empty are different answers to a program that opens it. `synthfs.Readme.md` gains its row in the commit that adds it.

### Tooling that changes

* `tests/system/runner/harness.c` gains a QEMU monitor connection and its steps: `key=NAME` to type, `mouse=DX,DY,BUTTONS` to move, and `usb=add:SPEC` and `usb=del:ID` to plug and unplug. This is the one new capability in the test system. Its argument parsing is proved by `harness --selftest` and its effect by the suite.
* `tests/system/suites.mk` gains the `run-usb` target and its two boots.
* `tests/registry` gains `usb`.
* `rach2/qemu_devices.sh` and `rach2/run.sh` gain a way to attach a USB controller and devices to a manual run, so a person can try it the way they try audio today.
* `rach2/sources.mk` gains the new C files, which is what makes `make check-sources` and the guest build see them.

### The guest build

The kernel builds on Rach2 itself with TinyCC, so the new files stay inside the shared C89 dialect: no anonymous unions, no declarations after statements, no compound literals, no `//` comments. `rach2/guest.mk` derives its object list from `sources.mk`, so nothing is written twice, and `tools/verify/guestcheck.c` checks the derivation. `tests/run selfbuild` is the proof: the machine builds the stack that drives its own keyboard.

## Documentation

* `rach2/sys/usb/Readme.md` is new: the layers and what each owns, the controller and why it is the only one, the state machine and why nothing blocks, the fixed limits, the boot-protocol restriction, the firmware handoff, the port routing, hotplug, shutdown, what is absent and why, and the verification.
* The input layer gets its own section in `rach2/Readme.md`: what it is, which drivers feed it, and what a scancode means when it did not come from a device that produces scancodes.
* `rach2/Readme.md` also: the subsystem table gains `sys/usb/` and `sys/input.c`, the supported hardware list gains USB HID, the boot-option table gains `rach2.nousb`, the limits paragraph gains the USB limits, and the subsystem reference list gains the new Readme.
* `Readme.md`: the system paragraph and the hardware-status paragraph.
* `rach2/sys/fs/synthfs.Readme.md`: the `/proc/usb` row and its contract.
* `tests/Readme.md`: `usbprobe` in the right list and the new suite.
* `tests/system/Readme.md`: the monitor steps, the new suite, and what the suite can and cannot prove without the machine.
* `tools/Readme.md`: only if a checker changes.
* `dev-log/35-usb/Readme.md` and the `dev-log/Readme.md` entry, in phase 7.

Phase 7 also runs the documentation and comment pass in the shape of the one run after milestone 34: every document and every comment this milestone touched is read against the code it describes, and a Codex read of the reader-facing documents is used as an independent check. That pass found four documents naming a file that had not existed for four milestones, so it is part of the plan and not an afterthought.

## How the work is kept track of

Four files beside this plan, all tracked, all committed at each checkpoint.

* `progress.md`: every item in this plan as a checklist.
* `current.md`: the resume state only. The active phase, the item in progress, the evidence gathered, the exact next action, and the file being read when the state was written. Replaced, never appended.
* `notes.md`: deviations, measurements, defects found and what was done about them, and every decision this plan left to the run.
* `hardware.md`: what the two machines report, side by side, so a difference between them is visible rather than inferred. Started in phase 1 with what configuration space gives, filled in phase 3 for QEMU and phase 6 for the laptop.

After every phase, every compaction recovery and every batch of commits, one line is reported: `M35: phase N of 7; X of Y items done; HEAD abc1234; tree clean|dirty`.

## Standing rules for the run

* Work directly. The only delegation is an independent review, one reviewer per diff, at most six files, at most ten minutes. There are six reviews in this milestone and no more: one after each of phases 2, 3, 4, 5, 6 and 7. The count is fixed now; no result of one review starts another.
* Commit in scoped units in the house style, directly on the branch, no attribution, never push.
* Every commit passes `tests/run style` and `tests/run host`. A commit that touches a subsystem runs that subsystem's suites singly. The whole set runs at the end of each phase and nowhere else.
* A test is proved by making it fail. A mutation is driven from a script that restores the source in a trap and rebuilds before the verdict is believed.
* A change that alters a contract rewrites the owning Readme in the same commit.
* Anything on the laptop that changes the running machine is presented to the owner with the values read from that machine, at the time, every time. Nothing writes the SSD in this milestone.
* Nothing is deferred. A thing discovered mid-run that the plan did not foresee is written into `notes.md` with the phase that will do it. Phase 7 reads `notes.md` from the top; every open item is completed, or removed from scope by a decision of the owner recorded in `notes.md`. There is no third outcome.

## The phases

### Phase 1: arrival and inventory

Read `AGENTS.md`, `CLAUDE.md` and this plan in full. Require a clean tree and record `git log --oneline -12`. Create `progress.md`, `current.md`, `notes.md` and `hardware.md` in the first commit.

Then the kernel reports what USB hardware it has, from configuration space alone, because no register mapping exists yet: a record per controller found at class `0x0C`, subclass `0x03`, with its interface byte, vendor and device identifiers, BAR, and, on an Intel xHCI, `XUSB2PR` and `XUSB2PRM`. `tests/system/accept/metal_accept.sh` gains the same line so the laptop can be asked over SSH.

Two measurements go into `hardware.md`: what QEMU reports, and what the laptop reports, including whether an external USB keyboard already types at the Rach2 console through the firmware's legacy emulation. That last one costs one boot and decides nothing about the plan, but it tells the reader of the record what the driver is for. The capability registers are not read here; they need the mapping phase 3 makes.

Proof: the records appear in QEMU with a controller attached and are absent without one; the acceptance script reports the same on the laptop.

### Phase 2: the interface and the model

`sys/usb/hcd.h`, `sys/usb/xhci.h` with `struct xhci_io`, and the first half of `tests/host/xhci_hosttest.c`: the model of a controller, and the rows for discovery, D0, the aperture check, the firmware handoff, stop, reset, the pause, `CNR`, the capability reads, both context sizes, `PAGESIZE`, and the scratchpad request including the refusal above the compiled maximum. `xhci.c` grows only as far as those rows require.

Proof: every row passes and each has its mutation. Mutations: skip the ownership wait, ignore `CNR`, drop the post-reset pause, assume 32-byte contexts, skip the scratchpad array, and accept a scratchpad request above the maximum. Each must fail exactly its row.

### Phase 3: bring-up

The rest of the controller as far as a port that reports a device: the context base array, the command ring with its Link TRB, the event ring and its segment table, the doorbells, `CONFIG`, `RS`, port power, debounce, port reset, the event drain, and `usb_stop`. The model rows for the rings, the wrap, `ERDP`, the port cases, and the command abort. QEMU's capability values are written into `hardware.md` from a boot.

Proof: in QEMU the kernel records a device present on a root port with its speed, and records nothing when no device is attached. In the model, a command that never completes aborts the ring, an event ring wrapped through its segment is read in order, a port that never leaves reset is given up on, and a `PORTSC` write clears the change bit it meant to and leaves the others alone.

### Phase 4: enumeration

`sys/usb/core.c`: the state machine, Enable Slot, the input contexts, Address Device, the descriptors, Evaluate Context, Configure Endpoint with its derived fields, `SET_CONFIGURATION` after it, the device table, and endpoint recovery. `/proc/usb` and `usbprobe`. `tests/host/usb_hosttest.c` in full.

Proof: `/proc/usb` in QEMU lists the keyboard and the mouse with their identifiers and speeds at both `usb_version` settings; `usbprobe` exits zero; the malformed-descriptor rows pass under AddressSanitizer; the fixed tables fill and refuse; no step of enumeration blocks, shown by the boot pump completing inside its bounded loop and by the timer path containing no wait.

Mutations: send `SET_CONFIGURATION` before Configure Endpoint, accept a descriptor whose length field exceeds the buffer, skip the Evaluate Context step, derive the interval as if every device were high-speed, and let a fifth device into a four-device table. Each must fail its check.

### Phase 5: input and HID

`sys/input.c` first, as a refactor with no USB in it: the translation and routing move out of the PS/2 handler, `input_key` and `input_mouse` appear, and `/dev/mouse` stops depending on the PS/2 mouse. `ttyprobe`, `stdintest`, `jobprobe`, the `console` suite and a Doom boot pass unchanged, which is what says the refactor changed nothing.

Then `sys/usb/hid.c`: the boot protocol, the report diff, the scancode table with its modifiers and `0xE0` sequences, and the mouse packets. The harness monitor connection and its steps. `tests/run usb` with both speeds, typing, movement, hotplug, unplug and the held-key release. `tests/run subset` gains the `rach2.nousb` boot.

Proof: a key sent through the monitor reaches the guest terminal and is read by a program there, on a machine with no 8042 at all; a movement becomes a packet a program reads; a device added after boot appears in `/proc/usb` and one removed disappears; a key held across an unplug is not stuck; the `rach2.nousb` boot records `skipped` and leaves `/proc/usb` empty.

Mutations: emit make codes without break codes, translate the rollover report, drop the previous-report diff so a held key repeats, ignore the disconnect event, drop the held-key release on teardown, and disconnect the USB path from `input_key` entirely. Each must fail its check, and the last one is what proves the suite is measuring the USB path and not another one.

### Phase 6: the machine

The laptop. Present the readings from phase 1 to the owner and ask what is to be done about port routing. Record the firmware's VT-d setting. Then boot the stack on the machine with an external USB keyboard and mouse: the firmware handoff, the real capability values, the scratchpad count, the context size, the ports, and the devices. Every value goes into `hardware.md` beside the QEMU one.

`tests/run metal` and the acceptance script gain the USB lines. Whatever the machine does that QEMU did not is written into `notes.md` and fixed there, not worked around.

Proof: a person types on a USB keyboard at the laptop's Rach2 console and moves a USB mouse, with the internal keyboard untouched; the acceptance script reports the same devices over SSH; a boot with `rach2.nousb` still reaches a shell on PS/2; a poweroff and a reboot both complete with the controller halted and the routing as it was found.

### Phase 7: the record

`rach2/sys/usb/Readme.md` and every document listed under Documentation. The documentation and comment pass, with the Codex read. The tier decision for the new suite, measured. `notes.md` read from the top and every line closed. The full suite set twice, with no machine left running, and `make test` measured. `dev-log/35-usb/Readme.md` with a trust statement in milestone 34's form: what a passing run proves about the USB stack, what only the laptop proved, and what neither could. `dev-log/Readme.md` gains the entry in the same commit. The tree is clean and nothing is pushed.

## Recovery after a compaction

Read `AGENTS.md`, `CLAUDE.md`, this plan's decisions and the current phase, `progress.md`, `current.md`, `notes.md` and `hardware.md`. Run `git status --short --branch` and `git log --oneline -15`, inspect every uncommitted path, and resume the current phase's first unfinished item. Report the one line.
