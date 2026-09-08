# Milestone 48: notes

## Where the work stands

Phases 1, 2, 3 and 4 are complete. Final verification included a clean build and the full test set, a Codex review of the milestone's diff in six units, fixes for the findings, and another clean build and full test set.

## Phase 1: xHCI instances

Commits `f71aad0` through `a1c01ee`.

Each xHCI controller has a `struct xhci_ctrl` containing what were forty file-scope variables in `xhci.c`. Every driver function receives it as `hc`, as do the register accessors in `struct xhci_io`, allowing a host test to model two controllers.

`xhci_discover` scans the whole bus and allocates a record for every controller on it. It reports controllers with another interface without driving them. If a controller fails to start, it reports the reason, omits that controller and continues starting the others.

The port-state table is sized from the controller's reported port count; the slot table and context array are sized from its slot count. The driver enables every supported slot. A slot's device context and each endpoint's ring occupy one heap page each, allocated when the slot is addressed and freed when it closes. Because the heap is not identity-mapped, every address supplied to the controller is the physical address recorded beside the kernel pointer to the page. The driver allocates the number of scratchpads specified by the controller.

Each device record and hub record occupies a frame from the identity-mapped window. This lets the controller write descriptors and reports into the records and provides space to read a complete configuration. A configuration larger than the record's buffer is rejected with a diagnostic that identifies it. When no slot is available, the device remains in `ST_SLOTWAIT` until another device is removed and releases a slot. A hub port above fifteen is reached through the route encoding, which is valid only when a translator identifies the port.

The work exposed two faults. The endpoint ring's link block contained the ring's virtual address, which the controller interpreted as physical. Completions were matched by slot number alone, although slot numbers repeat across controllers, so a hub on one controller received completions from another. `struct usb_completion` now records the controller that produced the completion.

## Phase 2: interface instances

Commits `58482dc` through `8930f04`.

`struct netdev` represents one interface. It contains the driver, the driver's record for the card, the interface name and index used by the rest of the kernel, the card's station address, the interface's address, mask and gateway, its counters, and its administrative up status. `netdev_probe` calls each driver to enumerate its cards and creates an interface for each, with names starting at `eth0`. A card that is present but fails to start is silenced and omitted; probing continues with that driver's next card and then with the remaining drivers. The driver receives and retains the interface at startup, so each delivered frame identifies its incoming interface.

The e1000 driver has one record per card. Its rings and buffers are allocated from identity-mapped frames at startup instead of being statically allocated. Four sets in `.bss` require 452 KiB, which the 16 MiB machine booted by `tests/run grow` cannot spare. `pci_find_nth_by_id` finds the second card with a matching identity. The RTL8139 and RTL8168 drivers still support one card each.

A datagram is sent through the interface that provides a route to its destination. ARP entries record the interface on which they were learned, and a lookup matches only entries for that interface. `/proc/net/dev`, `/proc/net/route` and `/proc/net/arp` report per-interface data. Every `SIOCGIF*` returns information about the interface specified by the caller, and a packet socket can bind to any interface. `rach2.ifaddr=<mac>,<addr>/<prefix>[,<gateway>]` selects an interface for configuration by its station address, so configuration does not depend on probe order.

The boot report prints one line per interface. `tests/run netif` boots two e1000 cards on two subnets. Both are registered as interfaces. The second is configured by its station address with its own address and gateway; the first retains its address and gateway. Ping still works, and taking the second interface down leaves the first operational. `rach2.nicfail=N` forces the Nth card that would otherwise start to fail, allowing the test to exercise startup failure on a machine whose cards all start.

## Phase 3: the platform

Commits `ce78481` and `b0c38e7`.

`smbios.c` reads the BIOS vendor from the firmware table. The reset code sends `0xFB` to port `0xB2` only when the reported vendor matches the firmware whose system-management handler implements that command. On every other machine, the code uses the chipset register and reports why it skipped the firmware command. The reset writes use indirection so `power_hosttest` can check the decision without performing a reset.

Since milestone 43, power-off has used a chipset-specific sequence only when the board table matches, and the standard path otherwise. That part of the phase was already complete. The audio driver now reports every controller it does not use.

## What the plan's proof set asked for

Phase 1 required two controllers with a keyboard on one and sticks behind a hub on the other; a stick removed from each while the other controller was busy; one controller failing at probe while the other remained operational; all slots occupied and a device waiting for one; a test row with more devices than the old twelve; a hub port above fifteen; and a complete configuration descriptor read and an invalid one rejected.

`tests/run usb` and the host tests cover all these cases except the row with more than twelve devices, recorded as entry 19 of the shortcomings list. Raising `USB_MAX_DEVICES` leaves the modelled hub with too few ports. The fault is in the model or a port-fifteen path; it is unrelated to a limit set by this milestone. The device table still has twelve entries.

Phase 2 required tests of two e1000 cards on two subnets and an RTL8139 alongside an e1000, with both cards reachable at once, SSH through each, DHCP on each interface by name, and one ARP destination reachable through each. It also required a card to fail at probe, an interface to be taken down after startup, and the sealed image's network files to contain no QEMU address. `tests/run netif` and `tests/run dhcp` cover all these cases. The mixed pair has a separate test row. The two-card machine pings a neighbour on each subnet. The machine that obtains a lease on each interface by name runs the SSH server and accepts a connection through either card, as its connection record confirms. The client writes the sealed disk's `/etc/resolv.conf` when it receives a lease; `/etc/hosts` lists the loopback and this machine.

These test rows exposed two faults. TCP verified a segment's checksum against the machine's address, so every segment addressed to the second card was dropped. An accepted connection also recorded the machine's address instead of the address on which the connection arrived, so replies used the wrong source address. The DHCP suite's last row now verifies that a connection to the second card works.

## What the mutations proved

Each of ten new checks was proved by making it fail. They check waiting for a slot, routing above fifteen, reading a complete configuration, registering two cards as two interfaces, keeping one interface operational when the other is down, applying address setters to the specified interface, sending through a packet socket's bound interface, limiting the firmware-specific reset to that firmware, keeping one controller operational when another fails at startup, and accepting and answering a TCP connection on the second card.

## Phase 4: the close

Codex reviewed the milestone's diff in six units grouped by contract: the controller and its driver, the device core and the class drivers, the interface layer and its drivers, the address and socket layers, the platform, and the suites. The review produced twenty-nine findings, described below with the changes made.

A controller omitted after a failed startup retained its allocated tables; `xhci_forget` now frees them and closes the record. Completions were matched by slot number alone, although slot numbers repeat across controllers. Driver state was indexed by the device's slot rather than the core's index, so devices on two controllers shared state. A hub's frame was freed while the core still retained the device, and the route encoding clamped a port above fifteen whether or not a translator identified it. A controller beyond the fourth stopped the bus scan. The scan now continues and reports the address of each controller beyond the limit.

A single-card driver stored the interface before checking which card the probe requested. The probe's second call therefore stored an interface slot that was never registered, and every frame the driver delivered was counted there. The address, mask and gateway ioctls wrote the machine's configuration regardless of the specified interface, so `ifconfig eth1` changed eth0's configuration and aborted its connections. Each setter now uses the specified interface. The pending address and provisional mask retained between two ioctls are also per interface. A datagram used the machine's address as its source regardless of the outgoing interface. A packet socket bound to an interface still received every frame the machine received and sent through the first card. The binding now records the interface and filters received frames by it. A `sockaddr_ll` that specifies an interface sends through it, and each received datagram is reported with its incoming interface. The destination check compared frames against the first card's station address, so it treated every frame addressed to the second card as addressed to another machine. The new probe row found this fault.

Probing continues with a driver's next card after one of its cards fails. Previously, probing continued after a failure injected by `rach2.nicfail` but stopped after a real failure. The suite therefore tested behavior that did not occur after a real card failure.

The firmware table is not always in the identity-mapped window. On a machine with more than 32 MiB, it is usually near the top of memory, where reading it without a mapping would fault. A table above the window is therefore mapped before it is read. The structure's vendor field identifies the vendor string, which is not always the first string. An entry point shorter than the specification requires is rejected because the address would be read from bytes outside its checksum coverage. The 32-bit anchor's inner `_DMI_` anchor is also checked. A string is read only if it terminates within the table. The host test checked a copy of the reset decision rather than the kernel's decision. `tests/run reset` now boots a machine whose firmware is SeaBIOS and one given `-smbios type=0,vendor=Insyde Corp.`, and requires the command to be skipped on the first and sent on the second.

The two-controller row did not verify that the controllers had separate interrupts or identify which controller completed a keystroke. The machine's only keyboard is now behind the hub on the second controller, and the row requires each controller to have its own interrupt. The two-card row did not read `/proc/net/dev`; the probe now does.

## What was found on the way

The `usb` suite's latency row checks that a keystroke was delivered by message rather than by timer-driven processing. It passes five times out of five when its machine runs alone and failed twice when a second machine ran concurrently. Entry 2 of the shortcomings list records this race in pooled runs. The race is unrelated to this milestone's code but can intermittently fail a full test set.