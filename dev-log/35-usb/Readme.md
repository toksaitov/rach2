# Milestone 35: USB

The kernel drives USB keyboards and mice. It has a host-controller driver, a device core, one class driver, and an input layer that the PS/2 keyboard and the USB one share. It works in QEMU at the two device speeds QEMU can produce. The notebook half is not done, and why is at the end.

## What was built

`sys/usb/xhci.c` is the controller. xHCI is the only register interface both machines have, and the only one that reaches a low-speed keyboard without a hub driver and split transactions: on the notebook's companion EHCI a keyboard is behind a rate-matching hub, and on xHCI it is not behind anything. UHCI is simpler and exists only in QEMU, so it was refused rather than written.

`sys/usb/core.c` is the standard half: descriptors, the enumeration state machine, class matching, and no register. `sys/usb/hid.c` is the boot protocol, which is what every firmware uses and what removes the report-descriptor parser, the largest body of code in a HID stack. `sys/input.c` is where a keystroke goes after either keyboard produces it.

Nothing on the timer's path waits. The event ring is drained from the 100 Hz timer handler, which runs with interrupts off, so `submit` queues and `poll` reports, a port reset is two calls, and enumeration is a state machine that advances one step per call with a deadline counted in milliseconds. Bring-up does wait, once, in thread context: the reset, the firmware handoff, the millisecond an Intel controller needs afterwards, and the hundred milliseconds the specification asks for between a connect and a reset. Every other decision in the stack follows from that.

## What running it found

Four defects in what this milestone built, and one older one in the test system.

The boot loop and the timer both drained the event ring and both stepped the state machine. The timer interrupts the boot loop, so two drains of one ring ran inside each other and two devices were enumerated over the top of one another, one addressed with the other's input context. Which device failed changed from boot to boot. The boot path now waits and the timer does the work.

A device that was configured drained the controller for its own completions and threw away everyone else's, so a second device enumerating beside a running one was never answered. There is one drain now, and each completion goes to the device it names.

A report of all zeros, which is a keyboard saying nothing is down, was dropped as if it were the rollover report. Every key held would have stayed held for ever. The host test found it the same day it was written.

`usb_poll` ran from the first tick of the boot, long before there was a controller, and the event ring it would have read is a zeroed array whose cycle bits look exactly like a controller's. There is a flag for that now.

The older one is in the test system rather than the kernel. Two suites built the harness kernel in their own recipes, so the pool ran two `make`s in `rach2/` at once and they wrote over each other's link response file. It failed `selfbuild` and, through a half-built image, two `dhcp` suites. That kernel is built by `prepare-all` now, before the pool starts.

## What was refused

Mass storage, hubs, report descriptors, isochronous transfers, typematic repeat, interrupts, and booting from a USB disk. Each is named in `rach2/sys/usb/Readme.md` with the reason. The first is the next milestone and the interface is shaped for it: `endpoint_open` is generic over control, interrupt and bulk, so a class driver adds no controller code.

## Verification

Three host tests. `xhci_hosttest` drives the driver against a model of a controller through `struct xhci_io`, the way `ahci_hosttest` drives the disk: a firmware handoff that answers slowly and one that never answers, a reset that never completes, a controller that never says it is ready, a scratchpad request larger than this kernel has, both context sizes, a ring wrapping through its link block with the cycle bit changing, a command that is never answered and the abort that takes the ring back, a port whose reset never finishes, a control transfer's three blocks and the one event that reports them, a short answer, a stall, a transfer nothing answers, and a completion for a slot the controller no longer holds. None of those can be produced on demand on a machine; several of them can happen there, which is the reason for the model.

`usb_hosttest` walks descriptors no device would send, in buffers allocated to the exact length under AddressSanitizer. `hid_hosttest` hands the class driver reports and reads back what it gave the input layer: a key held across reports, two keys at once, a modifier, an arrow, a rollover with its modifiers still good, a short report, and an unplug with a key still down.

`tests/run usb` is seven boots. Three of them take the 8042 away, because a key typed on a machine that still has a PS/2 keyboard could have come from either keyboard, and a check that passes either way says nothing about this stack. The harness gained the channel it needs for that: QEMU's monitor on a unix socket beside the serial line, with steps for a key, a movement, and a device plugged in or pulled out.

Every check added here was proved by a mutation that makes it fail, driven from a script that restores the source under a trap and rebuilds before the verdict is believed.

## Measurements

All from the development machine: ten cores, 24 GB, macOS, QEMU 11, twenty pool workers.

`make test` is 172 seconds, unchanged: the `usb` suite joined the core tier, which is 22 suites now, and a suite that runs beside the others in the pool costs nothing on the clock. `tests/run usb` is 39 seconds on its own. `tests/run all` is 471 seconds for 60 suites, also unchanged, with no machine left running.

Three full runs were made. The third was clean: 470 seconds, 60 suites, nothing left running. The first two had one failure each and neither reproduced. `selfbuild` failed on `musl/pwauthmusl` in one and `raw-e1000` exceeded a sixty-second wait in the other; each suite passes alone, in 243 and 156 seconds. Passing alone does not prove what caused either failure, and neither was investigated further: what is known is that both are checks with a time limit, that both ran under twenty workers, and that neither is part of what this milestone built. A reader should have that rather than a clean number.

## What a passing run means

It means the stack enumerates a keyboard and a mouse on an emulated controller at full and high speed; that a key typed on the emulated keyboard reaches both the debug shell and a program reading the terminal, on a machine with no other keyboard; that a mouse moved on both axes arrives the right way up in the queue Doom reads; that a device plugged in after the boot is enumerated and one pulled out is taken down; and that the driver refuses a controller whose registers answer wrongly, in each of the ways the model answers wrongly.

Low speed is not enumerated anywhere. QEMU offers full and high speed and no lower, so what the stack does with a low-speed device is exercised only where the interval and the default packet size are derived from it. A real keyboard is a low-speed device, which is one more reason the notebook matters.

It does not mean the stack works on the notebook. Nothing in QEMU exercises the firmware handoff, because QEMU's controller implements no legacy-support capability at all; nothing exercises the scratchpad pages or 64-byte contexts, because QEMU's controller wants none and uses neither. Those three are written from the specification and exercised in the model, which is weaker than exercising them on hardware. The Intel port routing has neither: it is four configuration registers this kernel reads and reports and has never written, and no test touches it at all.

Six other things are known and not closed. The core's table of four devices is never filled by a test. The Evaluate Context step, which corrects a full-speed device's control endpoint, is exercised in the model and by no machine, because QEMU's devices report the size that is assumed. A completion is matched to a device and an endpoint rather than to the exact block that was submitted. The modifier state is shared between keyboards. A key held while its keyboard is unplugged is released in the host test and nowhere else, because QEMU's `sendkey` cannot hold a key. And nothing reads the raw scancode ring, so the path a USB keyboard takes into Doom is right by construction and unproved.

## The notebook

Phase 6 is not done, and neither is the last item of phase 1 nor reviews 5 and 6 of the plan's six. They need the machine, a USB keyboard and mouse plugged into it, and a decision from the owner about the chipset's port routing with the real register values in front of them.

What is ready for that session: the kernel reports its USB inventory from configuration space at boot, including `XUSB2PR` and `XUSB2PRM`; `tests/system/accept/metal_accept.sh` prints those lines back over SSH, though it requires only that they exist and not that a device was enumerated; `rach2.nousb` switches the stack off if a boot with it goes wrong; and the notebook's own keyboard is on the 8042 and is untouched by any of this, so USB cannot lock anyone out of it.

`hardware.md` holds what QEMU reports. The notebook's column is empty: its controllers, its routing registers, its capability values, its scratchpad count, its context size and its firmware's VT-d setting are all unread.

The plan says this is the owner's to close rather than the milestone's. With the evidence above, they decide whether it closes with QEMU alone or stays open for another attempt. No such decision has been recorded, so it is open.
