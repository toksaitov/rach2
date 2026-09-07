# Milestone 48: notes

## Where the work stands

Phase 1 is done. Phase 2 is part done: the driver layer holds interfaces and
the e1000 has a record per card, but the stack above them is still written for
one interface. Phases 3 and 4 have not been started.

## Phase 1, done

Commits `f71aad0` through `a1c01ee`.

Every xHCI controller is driven from `struct xhci_ctrl`, one per controller,
holding what used to be forty file-scope variables in `xhci.c`: the aperture,
the capability record, the legacy-handoff state, the PCI position, the command
and event rings, the context base array, the per-port state, the completions,
the slot table, the input context and the command in flight. Every function in
the driver takes it as `hc`, and the register accessors in `struct xhci_io`
take it too, so a host test can model two controllers.

`xhci_discover` walks the whole bus and takes a record for every xHCI
controller on it. A controller of another interface is named and not driven,
through `usb_interface_name`. A controller whose bring-up fails is left out and
says why; the others still come up. `xhci_controllers`, `xhci_controller` and
`xhci_number` are how the rest of the kernel reaches them.

The tables are sized from each controller's own registers: the port state and
the slot table from what it reports, the scratchpads as many as it asks for,
and a slot's device context and each endpoint's ring one heap page each, taken
when the slot is addressed and given back when it closes. Because the heap is
not identity-mapped, every address the controller reads is the physical one
recorded beside the pointer this kernel reaches it by.

Above the driver: a device's record and a hub's record are each an
identity-mapped frame, taken when the device appears and given back when it
goes, so the controller can write a device's descriptors and reports straight
into it. A configuration is read whole, and one longer than the record's
buffer is refused by name. A device that finds no slot waits in `ST_SLOTWAIT`
and takes one when another device goes. A hub port above fifteen is reached
through the route encoding the standard defines, where the translator's own
port number says which port it really is.

Two faults came out of the work. The endpoint ring's link block pointed at the
ring's virtual address, which the controller reads as physical. And
completions were matched by slot number alone, which repeats across
controllers, so a hub on one controller took the other's answers; `struct
usb_completion` now carries the controller that answered.

Proofs: the whole host tier, with new rows for the slot wait, the port above
fifteen and the long configuration, each proved by mutation; `tests/run usb`
with a two-controller row where four devices run across two controllers and a
keyboard still types; `usbdisk`, `memory`, `pools`, `queue`, `default`.

## Phase 2, part done

Commits `58482dc` and `89eaee9`.

`struct netdev` is one interface: the driver, that driver's own record for the
card, and the name and index the rest of the kernel knows it by. A driver's
`init` takes the number of the card it is being asked for and hands back its
own record; every other operation takes that record. `netdev_probe` asks each
driver for its cards one after another and makes an interface of each, naming
them `eth0` upward and numbering them from one. A card that is in the machine
and will not start is silenced and left out, and the drivers after it are
still asked, so a machine whose second card is broken keeps its first.

The e1000 has `struct e1000` per card, holding its registers, rings, buffers
and counters, with its interrupt handler servicing every card it brought up,
since the line is shared. `pci_find_nth_by_id` is what finds the second card
of an identity. The RTL8139 and the RTL8168 keep one card each and say so.

What is left in phase 2, and it is the larger half: the stack above the
interfaces. `net_mac`, `net_ip_addr`, `net_ip_mask` and `net_ip_gateway` in
`rach2/sys/network/net.h` are one set for the machine, and every consumer
reads them directly: ARP, IP, TCP, UDP, DHCP, the sockets, `/proc/net/*` and
the `SIOCGIF*` ioctls. Each has to become the interface's own, with an ARP
entry naming its interface, a route selecting one, and the configuration
matched by MAC so probe order does not decide policy. Every caller that now
says `netdev_first()` is a place where the interface has to be chosen rather
than assumed; that is the list of what remains.

## What is left in the milestone

The rest of phase 2, as above: the stack per interface.

Phase 3: the firmware check before the Insyde SMI reboot, and the chipset
table's fallbacks, with a host test of the reboot decision.

Phase 4: the close. Build from clean, the full set, everything fixed, the
milestone's diff to Codex in units of six files, every finding fixed, the full
set again. The machine is not touched.

## What was found on the way

The `usb` suite's latency row asserts that a keystroke arrived by message and
not by the timer's pump. It passes five times out of five when its machine
runs alone and failed twice when a second machine ran beside it. That is the
pooled-run race of entry 2 of the shortcomings list, not a fault of this
milestone's code, but it will fail a full set now and then.
