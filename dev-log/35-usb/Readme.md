# Milestone 35: USB

The kernel drives USB keyboards and mice. It has a host-controller driver, a device core, one class driver, and an input layer that the PS/2 keyboard and the USB one share. It works in QEMU at the two device speeds QEMU can produce, and on the notebook, where a person played Doom with a USB mouse and edited and compiled a program with a USB keyboard.

## What was built

`sys/usb/xhci.c` is the controller. xHCI is the only register interface both machines have, and the only one that reaches a low-speed keyboard without a hub driver and split transactions: on the notebook's companion EHCI a keyboard is behind a rate-matching hub, and on xHCI it is not behind anything. UHCI is simpler and exists only in QEMU, so it was refused rather than written.

`sys/usb/core.c` is the standard half: descriptors, the enumeration state machine, class matching, and no register. `sys/usb/hid.c` is the boot protocol, which is what every firmware uses and what removes the report-descriptor parser, the largest body of code in a HID stack. `sys/input.c` is where a keystroke goes after either keyboard produces it.

Nothing on the timer's path waits. The event ring is drained from the 100 Hz timer handler, which runs with interrupts off, so `submit` queues and `poll` reports, a port reset is two calls, and enumeration is a state machine that advances one step per call with a deadline counted in milliseconds. Bring-up does wait, once, in thread context: the reset, the firmware handoff, the millisecond an Intel controller needs afterwards, and the hundred milliseconds the specification asks for between a connect and a reset. Every other decision in the stack follows from that.

## What running it found

Six defects in what this milestone built, and one older one in the test system. The last two came from the notebook and from nothing else.

The boot loop and the timer both drained the event ring and both stepped the state machine. The timer interrupts the boot loop, so two drains of one ring ran inside each other and two devices were enumerated over the top of one another, one addressed with the other's input context. Which device failed changed from boot to boot. The boot path now waits and the timer does the work.

A device that was configured drained the controller for its own completions and threw away everyone else's, so a second device enumerating beside a running one was never answered. There is one drain now, and each completion goes to the device it names.

A report of all zeros, which is a keyboard saying nothing is down, was dropped as if it were the rollover report. Every key held would have stayed held for ever. The host test found it the same day it was written.

`usb_poll` ran from the first tick of the boot, long before there was a controller, and the event ring it would have read is a zeroed array whose cycle bits look exactly like a controller's. There is a flag for that now.

The notebook has a device on a port that this kernel cannot drive, permanently: a full-speed one which on that model is the internal Bluetooth radio. The stack refused it and then forgot the refusal, saw the port still connected, and enumerated it again every 150 milliseconds until the slots ran out, taking the keyboard that had already worked with them. A refused device now keeps its table entry until its port says the device has gone.

Closing a device ran outside the turn that enumerates one. There is one command in flight and one answer to it, so a device waiting for its slot could be handed the answer to another device's Disable Slot and take a slot being given back as the slot it had just been given. That is why the keyboard went silent rather than simply being refused. Closing takes the same turn now.

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

What a passing run does not say is what the notebook says. Three things QEMU cannot exercise at all ran there for the first time: the firmware handoff, which QEMU's controller has no capability for; the sixteen scratchpad pages that controller asked for, which QEMU's wants none of; and a low-speed device, which is what a real keyboard is and what QEMU does not offer. A person then used the mouse in Doom and the keyboard to edit and compile a program, which is the raw scancode ring and the terminal path driven by hand rather than by a test.

Four things are known and not closed. The core's table of four devices is never filled by a test. The Evaluate Context step, which corrects a full-speed device's control endpoint, is exercised in the model and by no machine, because the devices met so far report the size that is assumed. A completion is matched to a device and an endpoint rather than to the exact block that was submitted. And the modifier state is shared between keyboards, which is the cost of one input path and is correct for the one keyboard this kernel drives at a time.

Two measurements were not made at the notebook. Its firmware's VT-d setting was not read from the setup screen: the stack works, so translation is not on for that controller, but the setting itself is unrecorded. And no boot with `rach2.nousb` was made to see whether the firmware emulates a PS/2 keyboard out of a USB one on its own, because the machine had an older kernel when the session began.

## The notebook

The kernel went on over SSH with `bootctl kernel`, was tried once, and was committed after it worked. `hardware.md` holds what the machine reported, beside what QEMU reports, and `lenovo-bootlog.txt` is its own log.

```
usb: controller: 0:14.0 xhci interface 0x30 8086:9c31 bar0 0xC0600004
usb: routing: usb2 0xA of 0x1FF, usb3 0x1 of 0xF
usb: controller: 0:1d.0 ehci interface 0x20 8086:9c26 bar0 0xC061C000
usb: caps: scratchpads 16, contexts 32 bytes, 64-bit 1
usb: handoff: legsup 0x10801 legctl 0x40002000 before
usb: handoff: legsup 0x1000801 legctl 0x0 after
usb: ready: version 100, 13 ports, 32 slots, 32-byte contexts
usb: device: port 2: 2a7a:939f, class 3 protocol 1, endpoint 1
```

The routing line is the shape of that machine. `XUSB2PR` reads `0x00A` of a possible `0x1FF`: the firmware gave the xHCI two of the nine routable USB2 pins, and they are the ports this kernel numbers 2 and 4. Port 2 is the blue socket, the USB 3.0 one. The two black sockets are USB 2.0, their pins are on the companion EHCI, and a device in either raises no event this kernel can see, which is what a person at the machine experiences as nothing happening.

That was the routing decision the plan reserved for the owner, and one socket was not enough to answer it with: a keyboard and a mouse could not both be plugged in. So the pins are taken. `XUSB2PR` moves what `XUSB2PRM` allows, on an Intel controller and no other, on every boot, and the firmware has them back after any power cycle. All three sockets work now, and three more soldered devices came into view and are refused once each.

Using the machine found three more defects, all in what this milestone built and all fixed here. A refusal was remembered by keeping the device's table entry, so the three soldered devices held three of four entries and the first unplug left nothing for anything plugged in afterwards. Every reboot stopped for twenty seconds, because handing the controller back to the firmware wakes its system-management handler; the controller is halted on the way out now and not given back. And the harness's monitor slept rather than waiting for a prompt, which failed the replug row one run in four.

A person then plugged a keyboard and a mouse into different sockets, unplugged and replugged them, and used both.
