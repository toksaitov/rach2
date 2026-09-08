# Milestone 48: USB and network devices

The kernel drives every xHCI controller the machine has and every network card, in whatever order the bus presents them. A controller is a record, an interface is a record, and what used to be one machine-wide address, one driver and one set of rings is now one of each per device. The platform decisions that were written for one machine ask the machine that is running. Nothing here ran on the Lenovo.

## What is built

`struct xhci_ctrl` is one controller: its aperture, capability record, command and event rings, context array, port state, slot table, completions and the command in flight. Discovery walks the whole bus and takes a record for each controller, up to four; a controller of another interface is named and not driven, one past the fourth is named at its address, and one whose bring-up fails gives back what it took from the firmware and is closed while the others come up. Every function in the driver takes the record, and so do the register accessors, so a host test can model two controllers at once.

The tables are the controller's own size: the port state from the ports it reports, the slot table and the context array from its slot count, and it is told to enable every slot it offers. A slot's device context and each endpoint's ring are one heap page each, taken when the slot is addressed and given back when it closes; the heap is not identity-mapped, so every address written for the controller to read is the physical one recorded beside the pointer this kernel reaches the page by. Scratchpads are as many as the controller asks for.

A device's record and a hub's record are each a frame from the identity-mapped window, which is what lets the controller write descriptors and reports into them and what makes room to read a configuration whole. A device that finds no slot waits and takes one when another device goes. A hub port above fifteen is reached through the route encoding, which is legal only where a translator names the port. A completion carries the controller that answered it, since slot numbers start at one on every controller.

`struct netdev` is one interface: the driver, that driver's own record for the card, the name and index the rest of the kernel knows it by, the card's station address, that interface's own address, mask and way out, its counters, and whether an administrator has it up. `netdev_probe` asks each driver for its cards one after another and makes an interface of each, naming them `eth0` upward; a card that is present and will not start is named with its reason and left out, and the driver's next card is still asked for. The e1000 has a record per card, with its rings and buffers taken from identity-mapped frames when the card comes up rather than compiled in; the RTL8139 and the RTL8168 drive one card each.

A datagram leaves by the interface its destination reaches and carries that interface's address as its source. ARP entries name the interface they were learned on. A TCP segment's checksum is taken against the address it was sent to, and a connection records the address it arrived on, so a service answers on the card the connection reached it by. A packet socket bound to an interface is offered that interface's frames and sends through it, and whether a frame was addressed to this machine is judged against the station address of the card it arrived on. Every `SIOCGIF*` and `SIOCSIF*` answers about and writes the interface the caller names; the first interface's configuration is also the machine's, since one address is still what TCP opens a connection from. `rach2.ifaddr=<mac>,<addr>/<prefix>[,<gateway>]` configures an interface by the station address it carries, so probe order does not decide policy.

`smbios.c` reads the BIOS vendor out of the firmware's own table, bounding every field by the length the anchor gave and mapping a table that lies above the identity-mapped window. The reset sends `0xFB` to port `0xB2`, a command to one firmware's system-management handler, only when the firmware says it is that firmware; every other machine goes to the chipset register and says which firmware it read instead. The audio driver names any controller it did not take.

## What proves it

`tests/run usb` boots two controllers with a hub and four devices across them, requires each to hold an interrupt of its own, types on the machine's only keyboard, which is behind the hub on the second controller, mounts three sticks across both and pulls one from each controller while the other's is being read, and refuses a controller at its bring-up with `rach2.hcfail=N` to show the others coming up without it. `hub_hosttest`, `xhci_hosttest`, `usb_hosttest` and `msc_hosttest` drive two modelled controllers, the slot wait, the route encoding and the configuration read whole.

`tests/run netif` boots two e1000 cards on two subnets and an RTL8139 beside an e1000, each interface configured by its station address, pings a neighbour on each subnet, reads `/proc/net/dev` for both, binds a packet socket to each and requires every datagram to name the interface it arrived on, takes the second interface down and requires the first to keep serving, and refuses a card at its bring-up with `rach2.nicfail=N`. `tests/run dhcp` boots two cards on two networks with a server on each, takes a lease on each interface by name, runs the SSH server and requires it to answer a connection arriving on either card, with each network's capture holding packets from the address that network leased.

`tests/run reset` boots a machine whose firmware is SeaBIOS and one given `-smbios type=0,vendor=Insyde Corp.`, and requires the firmware's own reset command to be skipped on the first and sent on the second. `power_hosttest` reads the vendor out of tables that name it as any string, name none, run past their own end or never terminate.

## The machine

Nothing in this milestone touched the Lenovo. It has one xHCI controller and one network card, so the work here is proved in QEMU alone; milestone 50 runs this kernel on the machine.

## Trust

The two-controller and two-card machines are QEMU's. What they show is that the kernel keeps one record per device and answers on the right one; they say nothing about a real machine with two controllers, which this project has not got.

The device table is still twelve, and the plan asked for a row with more devices than that. Raising it leaves the modelled hub short of its ports, which is entry 19 of the shortcomings list; the limit is the table's, not the memory's, since a device's record is a frame taken when it appears.

The firmware check is proved against a table QEMU publishes on request and against tables built byte by byte in a host test. No Insyde firmware has read the command this kernel would send it; that is milestone 50's.

The RTL8139 and the RTL8168 drive one card each, so a machine with two of either has one interface and the second card is not driven. The e1000 is the driver with a record per card.

## What was accepted

Filled in at the close.
