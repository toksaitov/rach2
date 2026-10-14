# `usb/`: the USB stack

The USB stack drives keyboards, mice, flash drives and hubs attached to xHCI host controllers. `xhci.c` is the controller driver and the only code that programs a controller. `core.c` enumerates each device that appears on a root port or a hub port and passes it to a class driver: `hid.c` for a boot-protocol keyboard or mouse, `hub.c` for a hub, `msc.c` for Bulk-Only mass storage. `usb.c` brings the controllers up and runs the stack from the timer and from the controllers' interrupts. The keyboard, mouse and removable disks of a current PC are USB devices, and the firmware's PS/2 emulation of a USB keyboard stops once the kernel takes the controller from the firmware, so the kernel drives them itself.

## Interfaces

`kernel.c` calls the stack through `usb.h`. After the root file system is mounted, unless the command line has `rach2.nousb`, it calls `usb_report_controllers`, which logs each USB controller from PCI configuration space alone, and `usb_init`, which brings up every xHCI controller and waits for the devices present at boot. `usb_poll` runs the stack on each tick of processor 0's 100 Hz timer, and `usb_stop` halts the controllers at shutdown. The terminal's read, the scheduler and `getrandom` call the `usb_stamp_` functions for the latency record.

The stack delivers:

* Keystrokes as set-1 scancodes through `input_key`, the call the PS/2 keyboard driver makes, and mouse reports through `input_mouse`, into the queue the PS/2 mouse fills ([`input.h`](../input.h)). `input_mouse_found`, called when a boot mouse is configured, lets the mouse device of [`synthfs.Readme.md`](../fs/synthfs.Readme.md) be opened.
* A block device for each logical unit of a flash drive, with `submit` and `flush` and no `read` or `write`, since a transfer completes later, in the pump. `block/usbdisk.c` registers it as a removable disk and publishes its nodes; [`block/Readme.md`](../block/Readme.md) describes that side.
* `/proc/usb` and the `FBK` line of `/proc/interrupts` ([`proc(5)`](../../../man/man5/proc.5)), and a link and a `uevent` per device under `/sys/bus/usb/devices`, which BusyBox's `lsusb` reads ([`sysfs(5)`](../../../man/man5/sysfs.5), [`busybox-util-linux(1)`](../../../man/man1/busybox-util-linux.1)).

The boot options:

* `rach2.nousb`: the stack is not started, and the boot logs `usb: skipped: rach2.nousb`.
* `rach2.nomsi`: the local APIC is not used ([`acpi(4)`](../../../man/man4/acpi.4)), so each controller interrupts on its PCI line.
* `rach2.dma32`: every controller is driven as one whose AC64 bit is clear, to test the 32-bit address path.
* `rach2.hcfail=N`: the Nth controller tried, counting from 0, fails its bring-up, to test what happens to the others.
* `rach2.msidrop=N`: the Nth interrupt turns every controller's interrupts off, leaving the timer to run the stack.
* `rach2.usblatency`: the latency record is printed.
* `rach2.hubslow=MS`: a hub's first scan waits MS milliseconds between two ports.

The stack logs under `usb`, and the mass-storage driver under `usbdisk`. A device's place in the tree is written as its root port followed by each hub's port: `5.2` is port 2 of a hub on root port 5.

## Design

| File | What it does |
| --- | --- |
| `usb.c` | The PCI inventory, the bring-up, the pump, the interrupt handler and the stop |
| `xhci.c`, `xhci.h` | Registers, rings, contexts, commands, transfers and events |
| `hcd.h` | The types the controller driver shares with the layers above |
| `core.c`, `core.h` | Device records, descriptors, enumeration, closing, and the rows of `/proc/usb` and `/sys` |
| `hid.c` | The boot-protocol keyboard and mouse |
| `hub.c` | The hub class driver |
| `msc.c` | Mass storage over Bulk-Only Transport |
| `stamp.c` | The keystroke latency record |

The core and the class drivers call the functions `xhci.h` declares. `hcd.h` also defines `struct usb_hcd`, an operations table for a second controller driver, which no code uses.

Nothing waits except bring-up. An operation that takes time is started in one pass of the pump and checked in a later one, against a deadline counted in milliseconds of a clock that each pass advances.

### Controllers

`xhci_discover` takes each xHCI controller on the PCI bus, up to `XHCI_MAX_CONTROLLERS`, 4, into a `struct xhci_ctrl` of its own. A UHCI, OHCI or EHCI controller, or an xHCI past the fourth, is logged and not driven. Discovery puts the function in power state D0, sets the port routing of an Intel controller, and maps the first 64 KiB of BAR 0 (`XHCI_MAP_SIZE`). A BAR above 4 GiB is mapped at its full address by the x86-64 kernel; the i686 kernel, whose addresses are 32 bits, does not drive such a controller.

`usb_init` then brings each controller up through five functions of `xhci.c`. `xhci_read_caps` reads the capability registers, rejects a controller whose registers lie outside the mapped window or that lacks 4 KiB pages, and sizes the slot table, the context base address array and the port state from the controller's own counts. `xhci_own` takes the controller from the firmware through the USB legacy support capability, if the controller has one, waiting up to a second, and turns off the firmware's SMIs. `xhci_halt_reset` halts and resets the controller, with the 1 ms pause an Intel controller needs before its registers may be read again. `xhci_scratchpads` gives it the scratchpad pages it requires; a controller that requires more than 512 (`XHCI_MAX_SCRATCHPADS`), the 8-byte entries that fit in one page, is not driven. `xhci_start` enables every device slot the controller has, programs the command ring and a one-segment event ring on interrupter 0, starts it, and powers each root port that has no power.

A controller that fails a step is given back to the firmware (`xhci_release`) and removed from the table, and the others come up. `usb_init` then waits 100 ms for the ports to settle and up to 400 ms (`USB_ENUMERATE_MS`) while the timer's pump enumerates the devices present, and enables the controllers' interrupts last. A slower device is finished by the pump after the boot goes on.

The command and event rings and the input context are arrays in the controller's static record. The context base address array, the scratchpads, each device context and each endpoint ring are pages taken as needed: from the heap (`kalloc_pages`), anywhere in memory, for a controller with AC64 set, and from the DMA pool (`alloc_kernel_frame`), below `HARD_MEMORY_LIMIT`, 32 MiB on both architectures, for one with AC64 clear. Every address is given in both halves. On x86-64 a piece of a transfer's buffer can lie above 4 GiB; for a controller with AC64 clear it moves through a DMA pool page kept with the transfer's record, copied in before an OUT transfer and back after an IN transfer, before the completion is reported.

### The companion EHCI

On an Intel chipset with a companion EHCI, four registers in the xHCI function's configuration space route the ports: `XUSB2PR` (0xD0) sends each USB 2 port to the xHCI or the EHCI controller, `USB3PSSEN` (0xD8) enables each port's SuperSpeed side, and `XUSB2PRM` (0xD4) and `USB3PRM` (0xDC) are masks of the bits that may be changed. The firmware may leave ports on the EHCI, and this kernel has no EHCI driver, so `take_ports` switches every port the masks allow to the xHCI controller on each boot. The routing reverts at the next power cycle.

A device whose port is switched in the middle of a transfer may not respond to the xHCI controller's Address Device, so before the switch `quiesce_ehci` takes each EHCI from the firmware through its legacy support capability, turns off its SMIs, halts it, masks its interrupts, and clears its configure flag if the firmware owned it.

### Rings, commands and transfers

A controller has one command ring of 256 TRBs (`XHCI_RING_TRBS`) and one command in flight, which lets one input context serve every command. A command not completed within a second (`COMMAND_MS`) is aborted. It stays outstanding until its completion or the Command Ring Stopped event arrives, and the next command is issued only after that event and with the Command Ring Running bit clear; an Enable Slot that succeeds during the abort still yields its slot, so the slot can be disabled ([xHCI 1.2](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf), 4.6.1 and 4.9.3).

`xhci_events` drains up to 256 events a call into an eight-entry completion queue (`XHCI_COMPLETIONS`); a transfer event that would overflow it stays on the ring for the next pass. The dequeue pointer is written back with the Event Handler Busy bit after a drain that took events and after every drain while interrupts are on, since the controller raises no further interrupt until that bit is written. After each drain the root ports' connect bits are read, which times the 100 ms debounce.

`xhci_submit` writes a control transfer as Setup, Data and Status TRBs, and a bulk or interrupt transfer as a chain of Normal TRBs, one per page-bounded piece. The first TRB's cycle bit is flipped last, so the controller never fetches half a transfer. An endpoint ring is one page of 256 TRBs, and an endpoint has two transfer records of up to 130 TRBs (`XHCI_TD_TRBS`): a 512 KiB buffer that starts within a page is 129 pieces. A transfer's generation, a number its caller chooses, comes back in its completion, so a caller that gave up on a transfer can tell its late completion from the current one.

A transfer is retired, and its buffer free, when an event reports its last TRB, when its endpoint has been quiesced, or when its slot has been disabled. QEMU posts no event for the last TRB of a transfer that ended short, so such a transfer stays the controller's until its endpoint is quiesced. A stall, a transaction error, babble or a split error halts an endpoint, and any other error puts it in the Error state (xHCI 1.2, 4.8.3); nothing is submitted on it until it is quiesced. `usb_core_endpoint_quiesce` issues Stop Endpoint on a running endpoint, Reset Endpoint on a halted one, and neither on one stopped or in the Error state, then Set TR Dequeue to the next TRB the driver will write, each on the core's command turn and each required to succeed, and then retires the endpoint's transfers.

### The pump and its two contexts

`usb_pump` is the only code that drains the event rings. It advances each controller's clock, which aborts a command past its deadline, drains each ring, and calls `usb_core_step`. The timer runs it through `usb_poll`, with 10 ms elapsed. The controller's interrupt runs it through `usb_msi_handler`, which acknowledges the interrupt and drains in one call, `xhci_interrupt_take`, acknowledging first: an event written between a drain and a later acknowledgement would have its pending bit cleared unread and wait for the timer.

Both contexts run with interrupts off, and the pump restores the interrupt flag it found and never enables interrupts, so the two are never inside a ring at once; the block layer relies on this when a process sleeps on a stick's request. A stick's `submit` and `flush`, the entries from process context, run with interrupts off too, since QEMU completes a transfer inside the doorbell write.

Each controller gets its own MSI vector from `isr_alloc_vector`, 48 to 63, when the local APIC is in use and the controller has an MSI capability. Otherwise it raises its PCI line, IRQ 1 to 15, through the I/O APIC, or through the 8259 when there is no I/O APIC in use, as under `rach2.nomsi`; the handler is registered first, since a level-triggered line the firmware left asserted would otherwise reach no handler. A controller with neither is left to the timer. Every handler is named `xhci` and drains every controller.

Once any controller's interrupts are on, a timer pass that took events while no USB interrupt was pending adds one to the fallback count, `FBK` in `/proc/interrupts`. The pending state is read after the drain and again after the core's step, since a message can arrive after its event; a message still in flight at both reads is counted too.

### Enumeration

`usb_core_step` delivers each completion to the storage driver, the hub driver, or the device the completion's controller and slot identify, since slot numbers start at 1 on every controller. It takes down each device whose port shows no connection, gives a record to each new connection on the first 32 root ports of each controller and on the ports of each running hub, steps every device, and then steps the storage driver, the hub driver and the USB disk layer. The 16 records (`USB_MAX_DEVICES`) are shared by all controllers and hubs. A device record is one 4 KiB frame of the DMA pool, since the controller writes descriptors and reports straight into it; it has room for a configuration of 3072 bytes (`USB_DESC_MAX`).

A device moves one step per pass at most. Its port must be connected for 100 ms without a change, and a connection that does not settle within 5 s frees the record. The device then takes its controller's command turn, held by one device of each controller at a time, since the controller has one command in flight; devices on two controllers enumerate at the same time. Its port is reset; Enable Slot and Address Device follow, with a default control packet size of 8 bytes at low and full speed, 64 at high speed and 512 at SuperSpeed. The first 8 bytes of the device descriptor give the real packet size, which Evaluate Context sets for a full-speed device; then come the 18-byte device descriptor, the 9-byte configuration header and the whole configuration. Configure Endpoint opens each endpoint the class driver needs, a separate command for each of a stick's two, and `SET_CONFIGURATION` follows. A stick goes to `msc_attach`, a hub to `hub_attach`, and a keyboard or mouse is put in boot protocol with `SET_PROTOCOL`.

`SET_CONFIGURATION` has 5 s (`CONFIG_MS`), since a stick may still be starting its flash memory, and every other step 500 ms (`STEP_MS`); a step that fails or passes its deadline fails the device. When Enable Slot completes with code 9, no slot free, the device keeps its record and its reset and issues Enable Slot again every 500 ms.

`usb_parse_config` walks the configuration by each descriptor's length, stopping at a length below 2 or past the bytes that arrived, and chooses the first interface with alternate setting 0 that is a boot keyboard or mouse (class 3, subclass 1, protocol 1 or 2) or a hub (class 9, protocol 0 to 3) with an interrupt IN endpoint, or mass storage (class 8, subclass 6, protocol 0x50) with a bulk IN and a bulk OUT endpoint. A SuperSpeed hub is recognized by its speed, since USB 3 puts its hub protocol in the device descriptor.

When a device fails or no class driver takes it, its port's bit is set in a bitmap, per controller or in the hub's record, and the port is left alone without spending a record. The bit is cleared when the port shows no connection, so the next device plugged in there is tried. When no record is free, the port is logged once and tried on every pass.

### Locations

A device is opened at a `struct usb_location` (`hcd.h`): its root port, its depth, its route string and its transaction translator. The route string has 4 bits for each of 5 tiers (`USB_TREE_TIERS`), the port on the hub nearest the root in the lowest; a device with more than 5 hubs above it is not driven. A port above 15 is written as 15, which the specification allows only for a low- or full-speed device directly below its high-speed translator, whose 8-bit port number gives the real port; anywhere else a device on such a port is not driven.

A low- or full-speed device's translator is the nearest high-speed hub above it, with that hub's port on the path and whether the hub has one translator per port. `xhci.c` writes them into the slot context, and the controller performs the split transactions. A hub's own slot context gets the hub bit, its port count, its think time and, when it has one translator per port, the multi-TT bit, through Evaluate Context.

### Keyboards and mice

A keyboard or mouse has one interrupt IN transfer outstanding, of the endpoint's packet size up to 8 bytes; each completion goes to `hid_report` and the next is queued. After a failed transfer the core quiesces the endpoint and resumes.

A boot keyboard's report is 8 bytes: modifier bits, a reserved byte and six key usages. `hid.c` keeps each device's previous report and sends the difference as set-1 make and break codes, with the 0xE0 prefix for the right Ctrl and Alt, the GUI and Application keys, the arrows, the editing keys, and the keypad's Enter and slash. The table covers usages 0x04 to 0x65, and a usage without a code sends nothing. A report whose key slots hold an error usage, 0x01 to 0x03, which a keyboard sends when it cannot tell which keys are down, has its modifiers read and its keys ignored, so the held keys stay down. When a keyboard goes, `hid_gone` releases every key it had down.

A boot mouse's report is at least 3 bytes: three buttons and signed X and Y. Y is negated, since a USB mouse's Y grows downward and the PS/2 queue's grows upward.

### Hubs

`hub_attach` reads the hub descriptor, of type 0x2A for a SuperSpeed hub and 0x29 otherwise, and uses at most 31 ports (`USB_HUB_PORT_MAX`). A SuperSpeed hub is given its tier with `SET_HUB_DEPTH`, which it needs to route packets (USB 3.0, 10.14.2.8); a hub whose interface protocol is 2 is set to one translator per port with `SET_INTERFACE`, and driven with one translator if the request fails. The hub's slot context is written through the core's command turn. Each port is then powered with its own `SET_PORT_FEATURE(PORT_POWER)`, even on a hub whose ports switch together, since USB 2.0, 11.11.1, leaves a ganged hub's other ports off until each gets a request of its own. After the power-good time, twice the descriptor's byte in milliseconds and held between 100 and 510 ms, each port's status is read once, and the connected ports go to the core when the scan ends.

The running hub keeps a transfer outstanding on its status change endpoint. Bit 0 of the answer is the hub's own status, whose local-power and over-current changes are logged and cleared. For each port whose bit is set, the driver reads the status, acts on the change, clears each change bit, and reads the status once more: the hub clears a change latch when the clearing request arrives, so a change latched after the first read would be lost, and the hub would leave the port out of its bitmap from then on. A hub whose second read fails is closed. A connection change closes the device below, even with the port still connected, and so does a lost enable on a USB 2 hub; the core then enumerates whatever is connected. An over-current powers the port off, closes the device below, and powers the port on once after a second; the port stays off if the fault recurs. On a SuperSpeed hub a link in SS.Inactive closes the device below and gets a warm reset, after the earlier change bits are cleared so that clearing them cannot clear the warm reset's completion.

The core's reset of a hub port is `SET_PORT_FEATURE(PORT_RESET)`, a status read every 20 ms until the reset ends, and the clearing of `C_PORT_RESET`, within 500 ms. A hub port settles only once the hub's first scan has ended. One control request is in flight per hub, and a request not completed in 500 ms fails the hub, which the core closes with every device below it. A detached hub's record, a DMA pool frame like a device's, is kept until the hub's slot is disabled.

### Mass storage

Each SCSI command to a stick is a 31-byte command block wrapper, a data stage when there is data, and a 13-byte status wrapper, with one command in flight per stick. The wrapper's tag increases across every stick and every attach, so a completion left from a pulled stick cannot match the next stick's command.

`Get Max LUN` comes first, and a stall, a failure or an answer above 15 means one unit. Each unit, at most 4 (`MSC_MAX_LUNS`), gets `INQUIRY`, and a unit whose device type is other than direct access, such as a drive's CD-ROM unit, is passed over. The unit serial number page follows; then `TEST UNIT READY`, with `REQUEST SENSE` after each failure and 100 ms between tries, until the unit is ready or reports no medium, which passes it over; then `READ CAPACITY(10)`, whose answer is the last block's address, and `READ CAPACITY(16)` when that is 0xFFFFFFFF; then `MODE SENSE(6)` for the write-protect bit. A unit whose blocks are not 512 bytes is passed over. Each unit has 10 s to finish (`MSC_READY_MS`), and past that the stick is failed. The units kept are published, and a stick with none is failed.

A unit's block device has 512-byte sectors, `BLOCK_READ_ONLY` when the unit is write-protected, at most 1024 sectors, 512 KiB, per request (`MSC_REQ_SECTORS`), and a deadline period of 1000 ticks, 10 s (`MSC_DEADLINE_TICKS`). A read or write is `READ(10)` or `WRITE(10)`, or the 16-byte form on a unit of more than 2^32 - 1 sectors, and a flush is `SYNCHRONIZE CACHE(10)`. A write to a write-protected unit fails with `EROFS`. Requests wait in a queue of 16 (`MSC_QUEUE`), the block layer's limit on live requests, and are served in arrival order, so a flush follows every write before it.

A request succeeds only when the status wrapper reports success with no residue and the data stage moved every byte. After a check condition `REQUEST SENSE` is sent: UNIT ATTENTION sends the command once more, ILLEGAL REQUEST in answer to `SYNCHRONIZE CACHE` is a stick without a cache and counts as success, and any other sense fails the request with `EIO`. A stalled data stage has its endpoint cleared and the status wrapper read; a stalled status wrapper has the IN endpoint cleared and is read once more. A bad or missing wrapper, a phase error, or a failed data stage starts reset recovery in the order the class fixes: the Bulk-Only Mass Storage Reset, then the halt cleared on the IN endpoint, then on the OUT endpoint. Each clear is the core's quiesce of the endpoint followed by `CLEAR_FEATURE(ENDPOINT_HALT)` to the device.

A request's pages belong to the controller from the doorbell until the transfer is retired, and the request completes only after; a short transfer under QEMU is retired by a quiesce before the request completes or the next command starts. A transfer not completed in 10 s (`MSC_STEP_MS`) fails the stick during bring-up; later, the bulk endpoints are quiesced, IN then OUT, and reset recovery runs. When a quiesce, a clear or the reset fails or passes its deadline, the stick is failed and handed to the core (`usb_core_device_drop`), which closes the device and enumerates the port again through a port reset, as a replug would.

When a stick is pulled, its waiting requests fail with `EIO`, and so does the request in flight unless the controller may still reach its pages, in which case it fails when Disable Slot completes. The stick's record is kept until the disk layer has released it (`msc_release`) and its slot is disabled, since a mount or an open file may still point at its block device.

### Closing a device

A device that has gone is taken down on the next pass, the devices below a hub first: its class driver is told, then Disable Slot is issued on the command turn, and the record is freed when it succeeds. A Disable Slot that fails or does not complete within 5 s (`CLOSE_LIMIT_MS`) leaves the record a tombstone, never reused, since the controller may still write into the slot's rings. A device taken down during its port reset releases the command turn at once, since a port reset issues no command; one with a command in flight keeps the turn until the command completes or is aborted, and a slot that a late Enable Slot yields is disabled too.

### The latency record

Under `rach2.usblatency`, `stamp.c` records the path of the last USB keystroke and logs one line, `usb: latency: ...`, when a reader takes its byte: the times of the handler's entry, of the code reaching the input layer and of the read, in microseconds relative to the drain that took the event; whether a blocked reader was recorded and then dispatched; how many pumps ran before the dispatch; and how many bytes `getrandom` completed meanwhile. The controller gives no timestamp of its own.

### Stopping

`usb_stop` turns each controller's interrupts off, clears Run/Stop and waits up to 16 ms for the halt, since a controller left running can wake or reset the machine. The kernel keeps the controller; it does not give it back to the firmware.

## Limits

| Limit | Value | Defined as |
| --- | --- | --- |
| Controllers | 4 | `XHCI_MAX_CONTROLLERS`, `xhci.h` |
| Device records, all controllers and hubs | 16 | `USB_MAX_DEVICES`, `core.h` |
| Hubs, detached ones not yet disabled included | 8 | `HUB_MAX`, `hub.h` |
| Ports used on a hub | 31 | `USB_HUB_PORT_MAX`, `hcd.h` |
| Root ports scanned per controller | 32 | `usb_core_step`, `core.c` |
| Tiers of hubs | 5 | `USB_TREE_TIERS`, `hcd.h` |
| Device slots per controller | the controller's count, at most 255 | `XHCI_SLOTS`, `xhci.h` |
| Configuration descriptor | 3072 bytes | `USB_DESC_MAX`, `core.h` |
| Sticks | 8 | `MSC_MAX_DEVICES`, `msc.h` |
| Logical units per stick | 4 | `MSC_MAX_LUNS`, `msc.h` |
| Requests waiting on a stick | 16 | `MSC_QUEUE`, `msc.c` |

The disk side's limits are in [`block/Readme.md`](../block/Readme.md).

* Only xHCI controllers are driven.
* Keyboards and mice are driven through the boot protocol alone. Report descriptors are not read, so a device without a boot interface is not driven and a mouse wheel is ignored.
* A held key does not repeat.
* All keyboards share the input layer's one modifier state, so a shift released on one is released for all.
* Mass storage is SCSI over Bulk-Only Transport, with 512-byte blocks. A drive that offers only UAS is not driven.
* There are no isochronous transfers, so no USB audio or video.
* A controller has one interrupter and one command in flight, so its devices are enumerated one at a time.
* No device or port is suspended.
* The root file system is never on a USB disk, since the stack starts after it is mounted.
* A tombstone record and its slot are lost until the machine restarts.

## Tests

`tests/run host` runs these under AddressSanitizer and UndefinedBehaviorSanitizer; [`tests/host/Readme.md`](../../../tests/host/Readme.md) lists their rows:

* `usb_hosttest.c`: the core without a controller, given descriptors no device would send in buffers of exactly their length, and the endpoint quiesce over a controller stub.
* `xhci_hosttest.c`: the controller driver against a model controller reached through `struct xhci_io`, from discovery and the firmware handoff to commands, transfers, interrupts and the 32-bit address path.
* `hid_hosttest.c`: reports in, scancodes out.
* `hub_hosttest.c`: the core and the hub driver over a model of a tree of hubs.
* `msc_hosttest.c`: the storage driver against a model stick that fails at any stage or is pulled.
* `usb_interrupt_hosttest.c`: interrupt setup and the fallback count across controllers.
* `usbstamp_hosttest.c`: the latency record with the scheduler.
* `usbdisk_hosttest.c`: `block/usbdisk.c`, the disk side.

`sh tests/host/usb_contract_mutate.sh`, `sh tests/host/usb_lifecycle_mutate.sh`, `sh tests/host/dma32_mutate.sh` and `sh tests/system/usb_latency_mutate.sh` compile faults into private copies of the sources, require each to fail its rows, and require the restored sources to pass.

`tests/run usb` boots the machines `tests/system/suites/usb.sh` lists on a `qemu-xhci` controller: keyboards and mice at high and full speed, typing on machines without an 8042, plugging and pulling, a device with no driver, messages, lines and the fallback, the latency record, hubs behind hubs, a full device table, sticks pulled during a read, two controllers, and `rach2.hcfail`. `tests/run usbdisk` tests sticks as disks, and `tests/run subset` requires `usb: skipped: rach2.nousb`; all three are in the core tier. `metal`, `loopdev`, `diskrate`, `krffsmount`, `installer` and `uefi` attach USB devices for their own checks. A suite runs on x86-64 with `ARCH=x86_64` before `tests/run`. The suite's script, [`tests/system/suites/usb.sh`](../../../tests/system/suites/usb.sh), describes each boot.
