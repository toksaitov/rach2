# `usb/`: the USB stack

Rach2 drives USB keyboards and mice. The stack is a host-controller driver, a device core, and one class driver, and only the first of the three touches a register.

## Why xHCI, and only xHCI

There are four host-controller interfaces, and this kernel implements the newest. UHCI, OHCI and EHCI are on no machine this project runs on except QEMU, and xHCI is on both: QEMU offers `qemu-xhci`, and the notebook has an Intel Lynx Point-LP xHCI at `00:14.0`.

It is also the only one that reaches a low-speed keyboard without a second mechanism. On xHCI the controller handles low, full, high and SuperSpeed alike. On the notebook's EHCI a low-speed device is reachable only through the chipset's integrated rate-matching hub, which would cost a hub class driver and split transactions to arrive where xHCI arrives directly.

## The layers

| File | What it owns |
|------|--------------|
| `hcd.h` | What a host controller provides, and all the layers above it may assume. |
| `xhci.c` | The controller: registers, rings, contexts, commands, transfers. |
| `core.c` | Devices, descriptors, the enumeration state machine, class matching. |
| `hid.c` | The boot-protocol keyboard and mouse. |
| `usb.c` | The inventory, the bring-up, and the stack's turn from the timer. |

Addressing belongs to the controller rather than the core: xHCI's Address Device command replaces the `SET_ADDRESS` request, so the core asks the driver to address a device and never sends that request itself. A second controller whose bus really takes the request would send it inside its own `device_address`.

## The two pumps, and the one ring

The event ring is drained by one function, `usb_pump` in `usb.c`, and by nothing else. Two contexts turn it: the controller's own interrupt, which is a message on vector 48, and the 100 Hz timer, which turns it every tick whether or not a message came.

Whichever turns it, the interrupt flag stays as that context found it. The handler runs from an interrupt gate and the timer's handler does too, and the pump saves and restores the flag rather than enabling interrupts on its way out. That is what keeps the two from being inside the ring at once on this one processor, and the pump says so at runtime: a pump that returns with the flag raised records `usb: pump: interrupts were enabled inside it`, once.

The handler acknowledges the interrupt before it drains, and the two are one call in the driver (`xhci_interrupt_take`). An event the controller writes between a drain and an acknowledgement that came after it would have its pending bit cleared without having been read, and would wait for the timer with no message coming for it. The dequeue pointer is written back with the event-handler-busy bit as one, on an empty drain as well: the controller sets that bit when it raises an interrupt and raises no other until it is written back, so a handler that found the ring already drained by the timer would otherwise silence the controller for good.

The timer is the fallback for a lost edge, so a missed message costs at most ten milliseconds and never a hang. How often it finds work that no message was coming for is counted and reported as `FBK` in `/proc/interrupts`. The question of whether a message was coming is put to the processor and not to the controller: a message already in the processor's own request register is one that this tick's interrupt flag is holding back, which is a race and not a lost edge.

`rach2.nomsi` leaves the interrupter off and the timer is the only pump, which is this stack as it was before the local APIC existed. A controller with no MSI capability is the same case. `rach2.msidrop=N` silences the controller after N messages, which no hardware switch can do on demand, and is how the fallback is seen to work.

The kernel stamps four points of a keystroke's path and prints them when a reader takes the byte: the handler's entry, the event the controller wrote, the key reaching the line discipline, and the read. The record is `usb: latency: msi 1, entry -5, wake 5, read 79, us from the event`. The last of the four holds whatever the reading program was doing when the key arrived; the first three are this kernel's own.

## Nothing waits

Nothing on the pump's path may wait: not a port reset, not a control transfer, not a command completion, not a millisecond delay. So `submit` queues work and returns, `poll` reports what finished, and enumeration is a state machine that advances at most one step per call. Each device has its own state and a deadline counted in milliseconds; a step that does not complete by its deadline fails the device and gives the slot back.

At boot the kernel waits while the timer drives that machine, so the devices present at reset are enumerated before the first process starts. The boot path does not drive it itself: two parties draining one event ring, one of them inside an interrupt of the other, is the defect this arrangement exists to prevent. The controller's own interrupt is turned on last, after the devices present at reset are enumerated, so that no message arrives at a stack that is still building its rings.

One command is in flight at a time, and one device is enumerated at a time. The controller has one command ring; a second device waits at its port, which costs a hundred milliseconds.

The only waits in the driver are in bring-up, which runs once: the reset, the firmware handoff, and the fixed millisecond an Intel controller needs after its reset is asserted before its registers may be read again.

## What the controller is asked for

`CAPLENGTH`, the interface version, the slot, interrupter and port counts, the scratchpad count, the context size, the addressing width, the register offsets and the supported page sizes are all read rather than assumed: the two machines this kernel runs on differ in several of them, and a driver that guessed any one would work on one machine and corrupt memory on the other.

A controller is refused, with a record saying which, when its aperture is a 64-bit window above 4 GiB, when its registers do not fit the 64 KiB this driver maps, when it does not offer 4 KiB pages, when its capability list leaves the aperture, when it never halts or never resets or never says it is ready, when the firmware never gives it up, or when it asks for more scratchpad pages than this kernel compiles in.

## The ports the chipset will give

An Intel chipset routes each USB2 pin to either this controller or the companion EHCI, and the firmware decides at power-on. It leaves some on the companion so that an operating system with no xHCI driver still has working sockets. This kernel has no EHCI driver, so a pin left there is a socket nothing can reach: on the notebook this milestone was written for, the firmware gave this controller two of nine, and the seven it kept were the two sockets a person plugs into.

So bring-up moves them. `XUSB2PR` at configuration offset `0xD0` takes the pins `XUSB2PRM` at `0xD4` says may move, and the SuperSpeed pair at `0xD8` and `0xDC` works the same way. The mask is read rather than assumed, and only an Intel controller is written to at all. The routing is the firmware's again after the next power cycle, so this runs on every boot, and a machine on which it goes wrong is one power cycle from the way it was. It is what every other operating system on this hardware does at the same point.

## A device this kernel cannot drive

A machine has USB devices soldered into it: a Bluetooth radio, a card reader, a camera. None of them offers a boot-protocol keyboard or mouse, so each is refused, and the refusal has to be remembered or the device is enumerated again the moment its port is looked at.

It is remembered by port, in a word of bits, and not by holding an entry in the device table. A notebook with three such devices would otherwise spend three of its four entries on them, leaving one for everything a person plugs in: unplug once and the freed entry goes to another soldered device, after which nothing plugged in is enumerated again until the machine reboots. A bit is cleared when its port says the device has gone, so a device refused once is tried again when it is plugged in again.

## The fixed limits

| Limit | Value |
|-------|-------|
| Device slots the controller is asked for | 4, of which slot zero is the controller's, so three devices |
| Endpoint numbers reachable on a device | 0 to 3, which is seven directions; a boot device uses two |
| Root ports reached | 16 |
| Scratchpad pages given | 32 |
| Keyboards and mice driven at once | as many as the slots hold, with one modifier state between them |

Each is a table in the kernel image. A controller with more root ports than this is used as far as sixteen and says so; one that requires more scratchpad pages is refused with both numbers in the record.

The modifier state is shared rather than one per keyboard. Two keyboards plugged in at once would share a shift: releasing it on either releases it for both. That is the cost of one input path, and it is the one place where the two keyboards are not independent.

## Boot protocol, and no report descriptors

A human-interface device describes its reports in a language of its own, and reading that language is the largest part of a USB HID stack. Every firmware skips it the same way this does: a keyboard and a mouse must offer a boot protocol, whose two report layouts are fixed. A device that offers no boot protocol is refused.

The cost is that a device whose interface is not class 3, subclass 1, protocol 1 or 2 is not driven, whatever else it can do.

## From a report to a keystroke

A HID report is a state and a scancode is an event. Each report is compared with the one before it: a usage that appeared is a make code, one that went away is a break code, and one in both sends nothing. The codes are set-1, which is what the PS/2 keyboard produces, so no code downstream has to tell the two kinds of keyboard apart. The modifier byte is turned the same way, with the right-hand modifiers in their two-byte extended form.

A report whose key slots hold an error usage is the keyboard saying it has lost count of its own keys. Its modifier byte is still good and is read; its key array is dropped, so what is really held stays held.

A device that goes releases everything it was holding. The release that would have lifted a held key is on a device that is no longer there, so this driver sends it.

A mouse report becomes a packet in the queue the PS/2 mouse fills, with the vertical movement turned over: a USB mouse's Y grows downward and that queue's grows upward.

## What is absent, and why

* **Mass storage.** The next milestone. The interface is shaped for it: `endpoint_open` takes an interrupt or a bulk endpoint, and the control endpoint is made while the device is addressed, so a class driver adds no controller code.
* **Hubs.** A device plugged into a hub is not seen. Both machines have enough root ports for a keyboard and a mouse.
* **Report descriptors.** Above.
* **Isochronous transfers.** Audio and video over USB. Nothing here reads them.
* **Typematic repeat.** A held key on a USB keyboard does not repeat. The PS/2 keyboard's repeat comes from its hardware; a USB host is supposed to make its own, and this one does not.
* **More than one interrupter.** The controller has one, on vector 48, and every event goes to it. Several would need several vectors and a rule for which endpoint answers on which.
* **EHCI.** The companion controller is not driven and will not be. Taking its pins costs one register write. Driving it would cost a controller driver, a hub driver and split transactions, for an interface no machine made after about 2014 has.
* **Booting from a USB disk.** The firmware loads the kernel; this stack starts afterwards.

## The guest's view

`/proc/usb` has one line per configured device: the port, the speed, the identifiers, the class and the protocol. A machine with no controller, or one booted with `rach2.nousb`, has the file and it is empty, which is a different answer from the file not being there.

The mouse is read through the device Doom opens, by the bare name `mouse`. It opens when a mouse of either kind exists.

`rach2.nousb` switches the stack off, and the probe says `usb: skipped: rach2.nousb` rather than going quiet.

## Stopping

The controller is halted when the machine stops, because other operating systems have a quirk on this exact part for a controller still running as the machine restarts.

It is not given back. Restoring the firmware's interrupt sources and its claim wakes its system-management handler, which re-initialises USB legacy support before this kernel's next instruction runs: on the notebook that took twenty seconds, in the middle of every reboot. The firmware initialises USB again on the next boot whatever this kernel leaves behind. A bring-up that fails is the other case, and it does give the controller back, because a machine with no USB driver should keep whatever its firmware was doing for it.

## Verification

`tests/run host` runs the three host tests written for this stack. `xhci_hosttest` drives the driver against a model of a controller through `struct xhci_io`: the discovery, the D3 wake, the aperture and register-set bounds, the firmware handoff at once and slowly and never, the halt and reset with its ready wait and its Intel pause, the scratchpad request and its refusal, both context sizes, the rings and their wrap, the command ring and its abort, the ports, the debounce, the device and endpoint commands, and the interrupter: the moderation interval, the two enables, the acknowledgement that keeps the enable beside it, the dequeue pointer written with the busy bit so the next event raises an interrupt of its own, and an event written between the acknowledgement and the drain. `usb_hosttest` walks descriptors no device would send, in buffers allocated to the exact length under AddressSanitizer. `hid_hosttest` hands the class driver reports and reads back what it gave the input layer: a key held, two keys at once, a modifier, an arrow, a rollover, a short report, and an unplug with a key still down.

`tests/run usb` is fifteen boots against QEMU. Most of them take the 8042 away, so that a key typed or a mouse moved on those machines can only have come over USB.

* a high-speed keyboard and mouse are enumerated and listed in `/proc/usb`
* the same at full speed, which is the speed a real keyboard uses
* a key typed through the emulator's monitor reaches the debug shell
* and is read by a program through the terminal
* a mouse moved through the monitor arrives at the mouse device, on both axes and in the right direction
* a keyboard plugged in after the boot is enumerated, and one pulled out is taken down
* with `rach2.nousb` the file is there and empty
* the controller's messages arrive, are counted on vector 48 in `/proc/interrupts`, and the timer's pump finds none of their work
* with `rach2.nomsi` there is no vector and no fallback, and the keyboard still types
* a mouse moved over messages arrives at the mouse device
* the four stages of a keystroke are recorded, the handler enters within a millisecond of its own event, and the reader runs on the wake rather than at the next tick
* with `rach2.msidrop=3` the controller sends three messages and no more, and the machine goes on typing with the fallback count rising
* with the APIC enabled the 8259's keyboard and timer still arrive, which the model in the host test cannot show

`tests/run subset` boots with `rach2.nousb` and requires the probe to record that it was skipped. Every check added here was proved by a mutation that makes it fail.
