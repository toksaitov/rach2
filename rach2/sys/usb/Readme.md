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
| `msc.c` | The flash drive, over Bulk-Only Transport. |
| `usb.c` | The inventory, the bring-up, and the stack's turn from the timer. |

Addressing belongs to the controller rather than the core: xHCI's Address Device command replaces the `SET_ADDRESS` request, so the core asks the driver to address a device and never sends that request itself. A second controller whose bus really takes the request would send it inside its own `device_address`.

## The one pump, and the two contexts

The event ring is drained by one function, `usb_pump` in `usb.c`, and by nothing else. Two contexts turn it: the controller's own interrupt, which is a message on vector 48, and the 100 Hz timer, which turns it every tick whether or not a message came.

Whichever turns it, the interrupt flag stays as that context found it. The handler runs from an interrupt gate and the timer's handler does too, and the pump saves and restores the flag rather than enabling interrupts on its way out. That is what keeps the two from being inside the ring at once on this one processor. A pump that returns with the flag raised records `usb: pump: interrupts were enabled inside it`, once.

The handler acknowledges the interrupt before it drains, and the two are one call in the driver (`xhci_interrupt_take`). In the other order, an acknowledgement after the drain would clear the pending bit of an event written in between, and that event would wait for the timer with no message coming for it. The dequeue pointer is written back with the event-handler-busy bit as one, and while the interrupter is on it is written on an empty drain as well: the controller sets that bit when it raises an interrupt and raises no other until it is written back, so a handler that found the ring already drained by the timer would otherwise silence the controller for good.

The timer is the fallback for a lost edge, so a missed message costs at most ten milliseconds and never a hang. How often it finds work that no message was coming for is counted and reported as `FBK` in `/proc/interrupts`. Whether a message was coming is asked of the processor rather than of the controller: the controller's own pending bit is set whether or not a message went out, while a message in the processor's request or in-service register is one this tick's interrupt flag has held back, which is a race rather than a lost edge. It is asked once after the drain and once at the end of the pump, because the event reaches memory before the message reaches the processor. Under the emulator the count is zero; on hardware a small count beside the vector's own is that same race.

`rach2.nomsi` leaves the interrupter off and the timer is the only pump, which is this stack as it was before the local APIC existed. A controller with no MSI capability is the same case. `rach2.msidrop=N` silences the controller after N messages, which no hardware switch can do on demand, and is how the fallback is seen to work.

The kernel stamps four points of a keystroke's path and prints them when a reader takes the byte: the handler's entry, the drain that took the event, the key reaching the line discipline, and the read. The record is `usb: latency: msi 1, entry -5, wake 5, read 79, us from the drain`. The drain is the base because it is the first moment this stack knows the event exists; the controller stamps nothing, so the time it wrote the event is not a number any of this can report. The last of the four includes whatever the reading program was doing when the key arrived; the first three are this kernel's own. There is one set of stamps and not one per byte, so a burst of keys taken by one read prints the last of them.

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
| Device slots the controller is asked for | 8, of which slot zero is the controller's, so seven devices |
| Endpoint numbers reachable on a device | 0 to 15, which is thirty-one directions; a boot device uses two and a stick two |
| Root ports reached | 16 |
| Scratchpad pages given | 32 |
| Keyboards and mice driven at once | as many as the slots hold, with one modifier state between them |
| Flash drives driven at once | 2, the size of the class driver's table and of the disk layer's names |
| Requests waiting on one stick | 16, the request pool's own size |
| A stick's request deadline | ten seconds, in the block layer and in the driver alike |

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

## Mass storage

A flash drive is a SCSI target reached through two bulk endpoints. Every command is two or three transfers: a 31-byte command block wrapper out, the data in or out when the command has any, and a 13-byte status wrapper in. `msc.c` is a state machine over those three, stepped from the same pump the timer and the interrupt share, with one command in flight per stick.

Bring-up sends the stick a fixed sequence of commands and checks each answer before the next. `Get Max LUN`, which a stick may stall, and a stall is one unit and not a failure, as is an answer above 15; `INQUIRY`, which must report a direct-access disk, and whose two names are kept for `/proc/usb`; `TEST UNIT READY` until it succeeds, within ten seconds per unit, with `REQUEST SENSE` after each failure, since a drive does not take the next command until the sense of the last failure has been read, and a unit whose sense says it has no medium is passed over for the next; `READ CAPACITY(10)`, whose field is the address of the last block and not a count, whose `0xFFFFFFFF` means a medium larger than a ten-byte read can address, which is refused, and whose block size must be 512 bytes; and `MODE SENSE(6)`, whose write-protect bit makes every mount of the stick read-only and whose refusal is the ordinary answer of a drive with nothing to report.

Every way a command can end has one answer. A data stage that stalls has the halt cleared on that endpoint and the status wrapper read; a status wrapper that stalls has its endpoint cleared and is read again, once. A wrapper the stick will not take, a status wrapper that stalls a second time, one that arrives with the wrong tag or the wrong signature or a status this class does not define, a residue larger than the transfer, and a phase error each lead to the full recovery, whose order the specification fixes: the class reset, then the halt cleared on the inward endpoint, then on the outward one. A check condition is followed by `REQUEST SENSE`, and the driver branches on the sense key: a unit attention, which a stick reports after a reset or a change of medium, has the command sent once more; an illegal request in answer to `SYNCHRONIZE CACHE` is a stick with no cache and not a failure; any other key fails the request with `EIO`. A transfer that never answers is not completed by the driver at its deadline, because the controller still owns the transfer and the page it points at, and that page is the block layer's: the full recovery is begun instead, its clearing of the endpoints takes the transfer back, and the driver completes the request when recovery ends. The block layer's own deadline is the same ten seconds, so the process waiting is woken with `EIO` at that deadline and the request's page stays quarantined until the driver's late completion frees it, which is what the quarantine is for. Every command carries a tag that goes up across every stick and every attach, so a completion left over from a stick that was pulled cannot match a command of the one plugged in after it. The stick's clearing of a halt goes through the core, because the controller takes one command at a time for the whole stack.

The block device the stick is published as has `submit` and `flush` and not `read` and `write`: a transfer to a stick completes later, from the pump, so the device cannot be used at boot or with interrupts off. A process reads it through the block layer's sleeping path, and the disk layer reads its partition table from the pump through `block_request_begin`, which starts one request and is asked after on later passes. Those two are the only entries from a process, and they run with interrupts masked: everything else that touches a stick runs in the pump, and the emulator completes a transfer inside the doorbell write, so the interrupt for a wrapper can arrive before the stage that sent it has been recorded. Requests are admitted in the order they arrived and served one at a time, since two processes can each be asleep inside a request on the one stick; the next one waiting goes out at the end of every pump pass, after a command ended and after a timeout alike, so nothing submitted in between overtakes it. A flush waits its turn like a read, which is what puts it after every write before it. `flush` is `SYNCHRONIZE CACHE`; the mounts call it at an unmount, a sync and an fsync, and the stick's line in `/proc/usb` counts the ones it answered.

A stick that is pulled out is a port that says so, which the core already sees. The transfer in flight is abandoned by its generation and its request is failed, every request waiting is failed, and every later one is refused, so a process asleep in a read returns with `EIO` rather than waiting for a device that will not answer. The slot is not freed then: its block device is what a mount, an open description and a held buffer all point at, and a stick plugged in afterwards would be handed those pointers with the slot. The disk layer counts what still holds the stick and frees the slot when the count reaches zero; [`block/Readme.md`](../block/Readme.md) has that count and what it covers.

## What is absent, and why

* **Hubs.** A device plugged into a hub is not seen. Both machines have enough root ports for a keyboard and a mouse.
* **Report descriptors.** Above.
* **Isochronous transfers.** Audio and video over USB. Nothing here reads them.
* **Typematic repeat.** A held key on a USB keyboard does not repeat. The PS/2 keyboard's repeat comes from its hardware; a USB host is supposed to make its own, and this one does not.
* **More than one interrupter.** The controller has one, on vector 48, and every event goes to it. Several would need several vectors and a rule for which endpoint answers on which.
* **EHCI.** The companion controller is not driven and will not be. Taking its pins costs one register write. Driving it would cost a controller driver, a hub driver and split transactions, for an interface no machine made after about 2014 has.
* **Booting from a USB disk.** The firmware loads the kernel; this stack starts afterwards.

## The guest's view

`/proc/usb` has one line per configured device: the port, the speed, the identifiers, the class and the protocol. A machine that has had a stick has more: a line per stick, with its node name, the two names `INQUIRY` gave, its size in sectors, its partitions, whether its table is still being read, it is ready or it is closing, with the holds left on it when it is closing, the flushes it has completed, and `ro` when it is write protected; and a last line counting the free names and the sticks given up. A machine that never had one writes neither, so a machine with no controller, or one booted with `rach2.nousb`, has the file and it is empty, which is a different answer from the file not being there. A program that counts devices counts the lines that begin with `port`.

The mouse is read through the device Doom opens, by the bare name `mouse`. It opens when a mouse of either kind exists.

`rach2.nousb` switches the stack off, and the probe says `usb: skipped: rach2.nousb` rather than going quiet.

## Stopping

The controller is halted when the machine stops, because other operating systems have a quirk on this exact part for a controller still running as the machine restarts.

It is not given back. Restoring the firmware's interrupt sources and its claim wakes its system-management handler, which re-initialises USB legacy support before this kernel's next instruction runs: on the notebook that took twenty seconds, in the middle of every reboot. The firmware initialises USB again on the next boot whatever this kernel leaves behind. A bring-up that fails is the other case, and it does give the controller back, because a machine with no USB driver should keep whatever its firmware was doing for it.

## Verification

`tests/run host` runs the four host tests written for this stack. `xhci_hosttest` drives the driver against a model of a controller through `struct xhci_io`: the discovery, the D3 wake, the aperture and register-set bounds, the firmware handoff at once and slowly and never, the halt and reset with its ready wait and its Intel pause, the scratchpad request and its refusal, both context sizes, the rings and their wrap, the command ring and its abort, the ports, the debounce, the device and endpoint commands, and the interrupter: the moderation interval, the two enables, the acknowledgement that keeps the enable beside it, the dequeue pointer written with the busy bit so the next event raises an interrupt of its own, and an event written between the acknowledgement and the drain. `usb_hosttest` walks descriptors no device would send, in buffers allocated to the exact length under AddressSanitizer. `hid_hosttest` hands the class driver reports and reads back what it gave the input layer: a key held, two keys at once, a modifier, an arrow, a rollover, a short report, and an unplug with a key still down. `msc_hosttest` drives the storage driver against a model of a Bulk-Only device that can be told to vanish at any stage: the bring-up in each of its forms, every row of the error matrix, the reads and the writes, the flush and its three answers, the admission queue and its order, a completion delivered between the waiting side's check and its sleep, and a stick pulled out at each stage, which fails every request it had and keeps its slot until the disk layer gives it back.

`tests/run usb` is fourteen boots against QEMU, listed one by one in [the system tests' Readme](../../../tests/system/Readme.md). Eight of them take the 8042 away, so that a key typed or a mouse moved on those machines can only have come over USB. What they prove about this stack is that a keyboard and a mouse are enumerated at both device speeds and read through both the shell's buffer and the terminal, that a device plugged in or pulled out after the boot is taken up and taken down, that a device this kernel cannot drive is refused once and costs the others nothing, and that the controller's messages arrive and are counted while the timer's pump stays behind them.

`tests/run subset` boots with `rach2.nousb` and requires the probe to record that it was skipped. Every check added here was proved by a mutation that makes it fail.
