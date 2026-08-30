# Milestone 35: USB

The kernel drives USB keyboards and mice. One host-controller driver, a device core, one class driver, and an input layer the two kinds of keyboard share. It works in QEMU at both device speeds a keyboard can use. The notebook half is not done, and why is at the end.

## What was built

`sys/usb/xhci.c` is the controller. xHCI is the only register interface both machines have, and the only one that reaches a low-speed keyboard without a hub driver and split transactions: on the notebook's companion EHCI a keyboard is behind a rate-matching hub, and on xHCI it is not behind anything. UHCI is simpler and exists only in QEMU, so it was refused rather than written.

`sys/usb/core.c` is the standard half: descriptors, the enumeration state machine, class matching, and no register. `sys/usb/hid.c` is the boot protocol, which is what every firmware uses and what removes the report-descriptor parser, the largest body of code in a HID stack. `sys/input.c` is where a keystroke goes after either keyboard produces it.

Nothing in the stack waits. The event ring is drained from the 100 Hz timer handler, which runs with interrupts off, so `submit` queues and `poll` reports, a port reset is two calls, and enumeration is a state machine that advances one step per call with a deadline counted in milliseconds. That decision is the shape of everything else here.

## What running it found

Four defects, three of them mine and one older.

The boot loop and the timer both drained the event ring and both stepped the state machine. The timer interrupts the boot loop, so two drains of one ring ran inside each other and two devices were enumerated over the top of one another, one addressed with the other's input context. Which device failed changed from boot to boot. The boot path now waits and the timer does the work.

A device that was configured drained the controller for its own completions and threw away everyone else's, so a second device enumerating beside a running one was never answered. There is one drain now, and each completion goes to the device it names.

A report of all zeros, which is a keyboard saying nothing is down, was dropped as if it were the rollover report. Every key held would have stayed held for ever. The host test found it the same day it was written.

`usb_poll` ran from the first tick of the boot, long before there was a controller, and the event ring it would have read is a zeroed array whose cycle bits look exactly like a controller's.

## What was refused

Mass storage, hubs, report descriptors, isochronous transfers, typematic repeat, interrupts, and booting from a USB disk. Each is named in `rach2/sys/usb/Readme.md` with the reason. The first is the next milestone and the interface is shaped for it: `endpoint_open` is generic over control, interrupt and bulk, so a class driver adds no controller code.

## Verification

Three host tests. `xhci_hosttest` drives the driver against a model of a controller through `struct xhci_io`, the way `ahci_hosttest` drives the disk: a firmware handoff that answers slowly and one that never answers, a reset that never completes, a controller that never says it is ready, a scratchpad request larger than this kernel has, both context sizes, a ring wrapping through its link block with the cycle bit changing, a command that is never answered and the abort that takes the ring back, and a port whose reset never finishes. None of those can be made to happen on a machine.

`usb_hosttest` walks descriptors no device would send, in buffers allocated to the exact length under AddressSanitizer. `hid_hosttest` hands the class driver reports and reads back what it gave the input layer: a key held across reports, two keys at once, a modifier, an arrow, a rollover with its modifiers still good, a short report, and an unplug with a key still down.

`tests/run usb` is seven boots. Three of them take the 8042 away, because a key typed on a machine that still has a PS/2 keyboard could have arrived by either road, and a check that passes either way says nothing about this stack. The harness gained the channel it needs for that: QEMU's monitor on a unix socket beside the serial line, with steps for a key, a movement, and a device plugged in or pulled out.

Every check added here was proved by a mutation that makes it fail, driven from a script that restores the source under a trap and rebuilds before the verdict is believed.

## What a passing run means

It means the stack enumerates a keyboard and a mouse on an emulated controller at low, full and high speed; that a key typed on the emulated keyboard reaches both the debug shell and a program reading the terminal, on a machine with no other keyboard; that a mouse moved on both axes arrives the right way up in the queue Doom reads; that a device plugged in after the boot is enumerated and one pulled out is taken down; and that the driver refuses a controller which lies to it, in each of the ways the model can lie.

It does not mean the stack works on the notebook. Nothing in QEMU exercises the firmware handoff, because QEMU's controller implements no legacy-support capability at all; nothing exercises the Intel port routing, the scratchpad pages, or a controller with 64-byte contexts, because QEMU's has none, uses none and does not. Those paths are written from the specification and proved in the model, which is a weaker thing than proved on hardware, and the milestone says so rather than implying otherwise.

## The notebook

Phase 6 is not done. It needs the machine, a USB keyboard and mouse plugged into it, and a decision from the owner about the chipset's port routing with the real register values in front of them. Everything it needs is ready: the kernel reports its USB inventory from configuration space at boot, including `XUSB2PR` and `XUSB2PRM`; `tests/system/accept/metal_accept.sh` reads those lines back over SSH; `rach2.nousb` switches the stack off if a boot with it goes wrong; and the notebook's own keyboard is on the 8042 and is not touched by any of this, so USB cannot lock anyone out of it.

`hardware.md` holds what QEMU reports, with the notebook's column empty and marked.
