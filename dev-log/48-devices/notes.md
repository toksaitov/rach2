# Milestone 48: notes

## Where the work stands

Phases 1, 2, 3 and 4 are done. The close ran the clean build and the full set,
sent the milestone's diff to Codex in six units, fixed what came back, and ran
the clean build and the full set again.

## Phase 1: xHCI instances

Commits `f71aad0` through `a1c01ee`.

Every xHCI controller is driven from `struct xhci_ctrl`, one per controller,
holding what used to be forty file-scope variables in `xhci.c`. Every function
in the driver takes it as `hc`, and the register accessors in `struct xhci_io`
take it too, so a host test can model two controllers.

`xhci_discover` walks the whole bus and takes a record for every controller on
it. A controller of another interface is named and not driven; one whose
bring-up fails is left out with its reason and the others still come up.

The tables are the controller's own size: the port state from the ports it
reports, the slot table and the context array from its slot count, and it is
told to enable every slot it offers. A slot's device context and each
endpoint's ring are one heap page each, taken when the slot is addressed and
given back when it closes. Because the heap is not identity-mapped, every
address written for the controller to read is the physical one recorded beside
the pointer this kernel reaches the page by. Scratchpads are as many as the
controller asks for.

A device's record and a hub's record are each a frame from the identity-mapped
window. That is what lets the controller write descriptors and reports into
them, and what makes room to read a configuration whole; one longer than the
record's buffer is refused by name. A device that finds no slot waits in
`ST_SLOTWAIT` and takes one when another device goes. A hub port above fifteen
is reached through the route encoding, which is legal only where a translator
names the port.

Two faults came out of the work. The endpoint ring's link block pointed at the
ring's virtual address, which the controller reads as physical. And
completions were matched by slot number alone, which repeats across
controllers, so a hub on one controller took the other's answers; `struct
usb_completion` now carries the controller that answered.

## Phase 2: interface instances

Commits `58482dc` through `8930f04`.

`struct netdev` is one interface: the driver, that driver's own record for the
card, the name and index the rest of the kernel knows it by, the card's
station address, that interface's own address, mask and way out, its counters,
and whether an administrator has it up. `netdev_probe` asks each driver for its
cards one after another and makes an interface of each, naming them `eth0`
upward. A card that is present and will not start is silenced and left out and
the drivers after it are still asked. A driver is given the interface at
bring-up and keeps it, so every frame it delivers says where it arrived.

The e1000 has a record per card. Its rings and buffers are taken from
identity-mapped frames when the card comes up rather than compiled in, since
four sets of them in `.bss` cost 452 KiB and the 16 MiB machine
`tests/run grow` boots has no such margin. `pci_find_nth_by_id` finds the
second card of an identity. The RTL8139 and the RTL8168 keep one card each.

A datagram leaves by the interface its destination reaches. ARP entries name
the interface they were learned on and a lookup matches only on the same one.
`/proc/net/dev`, `/proc/net/route` and `/proc/net/arp` are per interface, every
`SIOCGIF*` answers about the interface the caller names, and a packet socket
binds to any of them. `rach2.ifaddr=<mac>,<addr>/<prefix>[,<gateway>]`
configures an interface by the station address it carries, so probe order does
not decide policy.

The boot report is a line per interface. `tests/run netif` boots two e1000
cards on two subnets: both become interfaces, the second is configured by its
station and takes its own address and gateway, the first keeps its own, the
ping still works, and taking the second down leaves the first serving.
`rach2.nicfail=N` refuses the Nth card that would have come up, which is how a
card failing its bring-up is seen on a machine whose cards all start.

## Phase 3: the platform

Commits `ce78481` and `b0c38e7`.

`smbios.c` reads the BIOS vendor out of the firmware's own table. The reset
sends `0xFB` to port `0xB2`, a command to one firmware's system-management
handler, only when the firmware says it is that firmware; every other machine
goes to the chipset register and says why the first was skipped. The reset's
writes go through an indirection so the decision can be watched without
running one, which `power_hosttest` does.

The power-off already used a chipset-specific sequence only where the board
table matched, and the standard path otherwise, so that half of the phase was
settled by milestone 43. The audio driver now names any controller it did not
take.

## What the plan's proof set asked for

Phase 1 asked for two controllers with a keyboard on one and sticks behind a
hub on the other, a stick pulled from each while the other controller is busy,
one controller failed at probe with the other up, every slot taken and a device
waiting for one, a row with more devices than the old twelve, a hub port above
fifteen, and a configuration descriptor read whole and a wrong one refused.

All of them are in `tests/run usb` and the host tests except the row with more
than twelve devices, which is entry 19 of the shortcomings list: raising
`USB_MAX_DEVICES` leaves the modelled hub short of its ports, and the fault is
in the model or in a port-fifteen path, not in a limit this milestone set. The
device table is still twelve.

Phase 2 asked for two e1000 cards on two subnets and an RTL8139 beside an
e1000, both reachable at once, SSH through each, DHCP on each interface by
name, one ARP destination reachable through each, a card failed at probe and
one taken down after startup, and the sealed image's network files carrying no
QEMU address. All of them are in `tests/run netif` and `tests/run dhcp`: the
mixed pair is a row of its own, the two-card machine pings a neighbour on each
subnet, and the machine that takes a lease on each interface by name runs the
SSH server and answers a connection arriving on either card, which its own
record of who reached it says. The sealed disk's `/etc/resolv.conf` is written
by the client when a lease arrives and its `/etc/hosts` names the loopback and
this machine.

Two faults came out of writing those rows. TCP verified a segment's checksum
against the machine's own address, so every segment addressed to the second
card was dropped, and an accepted connection recorded the machine's address
rather than the one the connection arrived on, so an answer left from the wrong
address. A connection to the second card now works, which is what the DHCP
suite's last row shows.

## What the mutations proved

Ten new checks, each proved by making it fail: the slot wait, the route above
fifteen, the configuration read whole, two cards being two interfaces, a
downed interface leaving the other serving, the address setters writing the
interface they are given, a packet socket sending through the interface it is
bound to, the firmware's own reset going only to that firmware, a controller
refused at its bring-up leaving the other serving, and a TCP connection
arriving on the second card being taken and answered.

## Phase 4: the close

The milestone's diff went to Codex in six units grouped by contract: the
controller and its driver, the device core and the class drivers, the interface
layer and its drivers, the address and socket layers, the platform, and the
suites. Twenty-nine findings came back. What they were and what was done:

The controller and the core. A controller left out after a failed bring-up kept
the tables it had taken, so `xhci_forget` gives them back and closes the record.
A completion was matched by slot number alone, which repeats across
controllers. A device's driver state was keyed by slot rather than by the core's
index, so two controllers' devices shared it. A hub's frame was freed while the
core still had the device, and the route encoding clamped a port above fifteen
whether or not a translator named it. A controller past the fourth stopped the
whole bus walk; the walk goes on and each one past the limit is named at its
address.

The interfaces. A single-card driver stored the interface before it checked
which card it was being asked for, so the probe's second call latched onto a
slot that never became one and every frame it delivered was counted there. The
address, mask and gateway ioctls wrote the machine's configuration whatever
interface they were given, so `ifconfig eth1` moved eth0 and aborted its
connections; each setter now takes the interface, and the pending address and
provisional mask that live between two ioctls belong to that interface too. A
datagram carried the machine's address as its source whichever interface it left
by. A packet socket bound to an interface was still offered every frame the
machine received and still sent through the first card; the binding carries the
interface, the receive is filtered by it, a `sockaddr_ll` that names one sends
through it, and each datagram is reported with the interface it arrived on.
Whether a frame was addressed to this machine was judged against the first
card's station address, so everything the second card was sent read as
somebody else's; the new probe row found that.

A driver's next card is asked for after one of its cards fails. Before this,
`rach2.nicfail` continued and a real failure did not, so the suite proved a path
the machine would never take.

The platform. The firmware's table is not always in the identity-mapped window:
a machine with more than 32 MiB usually publishes it near the top of memory, and
reading it there would fault, so a table above the window is mapped first. The
vendor is the string the structure's vendor field names and not always the first
one. An entry point shorter than the specification's is refused, since the
address is read from bytes its checksum did not cover, and the 32-bit anchor's
inner `_DMI_` anchor is checked too. A string that does not end inside the table
is not read as one. The host test proved a copy of the decision rather than the
kernel's, so `tests/run reset` now boots a machine whose firmware is SeaBIOS and
one given `-smbios type=0,vendor=Insyde Corp.`, and requires the command to be
skipped on the first and sent on the second.

The suites. The two-controller row proved neither separate interrupts nor which
controller answered a keystroke: the machine's only keyboard is now behind the
hub on the second controller, and the row requires each controller to hold an
interrupt of its own. The two-card row never read `/proc/net/dev`, which the
probe now does.

## What was found on the way

The `usb` suite's latency row asserts that a keystroke arrived by message and
not by the timer's pump. It passes five times out of five when its machine
runs alone and failed twice when a second machine ran beside it. That is the
pooled-run race of entry 2 of the shortcomings list, not a fault of this
milestone's code, but it will fail a full set now and then.
