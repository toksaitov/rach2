# Milestone 48: USB and network

## The goal

The kernel drives every controller and every card it has a driver for,
however many the machine has and in whatever order they are found: two
xHCI controllers, a dozen or more USB devices behind several hubs, two
network cards of the same model or different ones, each interface with
its own address, and a card that fails leaving the others working. What
the kernel cannot drive, an EHCI-only machine or a wireless card, it says
by name at boot. The milestone is complete when the proof set below
passes in QEMU with two controllers and two cards and the Lenovo's own
devices work as before.

## What the tree settles before this plan

* xHCI is the one host-controller driver. Discovery takes one controller
  and keeps its state globally (`xhci.h:7`, `xhci.c:297`); the limits are
  12 devices, 2 hubs, 12 slots, 16 root ports and 32 scratchpad pages
  (`core.h:20`, `hub.h:16`, `xhci.c:543`); extra ports are clipped and a
  controller needing more scratchpads is rejected; a hub with more than
  fifteen ports is clipped to `USB_HUB_PORT_MAX` because the route string
  encodes a port in four bits (`hub.c:340`, `core.c:460`); a configuration
  descriptor is truncated at 64 bytes (`core.c:999`); the command, event,
  endpoint and completion rings are fixed. An Intel controller's port
  routing is written on vendor identity alone (`xhci.c:267`). EHCI is
  declined (`usb.c:9`).
* Three network drivers; the common layer keeps one selected driver
  (`netdev.c:21`), the first that comes up is the interface and a
  supported card that fails stops the probe (`netdev.c:35`); each driver
  keeps one instance in globals (`rtl8168.c:106`); the stack has one MAC,
  one address configuration and one interface state (`netif.c:49`,
  `net.h:45`); a route is the connected subnet or the one gateway
  (`ip.c:263`); ARP entries have no interface. Manifests name `eth0` and
  the resolver files carry QEMU's addresses (`rach2-metal-disk.manifest:133`).
* The platform: power management uses a small chipset table
  (`board.c:42`); reboot sends an Insyde-specific SMI command first without
  checking the firmware (`power.c:241`); audio takes one HDA controller
  (`hda.c:1605`). PCI already scans all 256 buses (`pci.c:217`). The
  metal acceptance reads the USB and audio vectors as fixed numbers
  (`metal_accept.sh:249`), which milestone 46's allocated vectors break.
* Milestone 46 allocates objects and interrupt vectors; milestone 47 put
  USB disks in the block registry.

## Decisions

### Controllers are instances

xHCI state is per controller, every supported controller is driven, and
each has its own vector, rings and completion state. Ports and scratchpads
are sized from the controller's capability registers. Device and hub
records are allocated; a slot count is the controller's own limit and a
device that finds no slot waits for one. Configuration descriptors are
read whole and validated. The Intel port-routing write is done only on a
controller whose identity matches a list, with the reason recorded.

### EHCI is named, not driven

An EHCI-only machine is older than this project's target and a driver
would be a project of its own, with transaction translators and companion
controllers behind it. The boot says "USB: EHCI at 00:1d.0, not driven".
UHCI, OHCI and UAS are the same.

### Interfaces are instances

Each card gets its own driver instance, MAC, addresses, link state and
queues; a failed card leaves the others; two cards of one model work.
Interfaces are `eth0`, `eth1` in probe order and may be configured by MAC
so probe order does not decide policy. ARP entries carry their interface;
routes select an interface; connected routes for each interface and one
chosen default; packet sockets, DHCP and interface ioctls name the
interface. Bonding, bridging, forwarding, wireless and IPv6 are not this
milestone's. The manifests stop naming QEMU's addresses.

### The platform checks before it acts

The Insyde reboot command is sent only when the firmware identifies
itself as Insyde; the chipset table is consulted for what it knows and the
standard paths run for the rest; HDA drives the first controller and
names any other.

## Phases

### Phase 1: xHCI instances

Per-controller state, capability-sized arrays, allocated records,
descriptors whole. Proof: QEMU with two xHCI controllers, a keyboard on
one and three sticks behind a hub on the other, all working, a stick
pulled from each while the other controller is busy, one controller
failed at probe with the other up, every slot taken and a device waiting
for one and getting it; a row with more devices than the old twelve; a
hub with more than fifteen ports clipped to fifteen and the reason logged;
a configuration descriptor longer than 64 bytes read whole and one with a
wrong length refused.

### Phase 2: interface instances

Driver instances, the stack per interface, ARP and routes by interface,
configuration by MAC. Proof: QEMU with two e1000 cards on two subnets
and again with an RTL8139 beside an e1000, both reachable at once under
traffic, SSH through each, DHCP on each interface by name, one ARP
destination reachable through each; one card failed at probe with the
other up, and one taken down after startup with the other still serving;
the sealed image's network files carrying no QEMU address.

### Phase 3: the platform and the machine

The firmware check before the SMI reboot; the chipset table's fallbacks;
the metal acceptance reading vectors from `/proc/interrupts` by device
rather than by number; the Lenovo booted with its keyboard, mouse, sticks,
hub and card as before. Proof: a host test of the reboot decision that
feeds it the Lenovo's firmware strings and another's and checks which
port writes it makes; the metal acceptance; the reboot from SSH.

### Phase 4: the close

Proof, once and in this order: build from clean; the full set; a boot on
the Lenovo and the SSH capstone; everything that comes up fixed; the whole
milestone's diff to Codex in units of six files, most serious first; every
finding fixed; the full set again. The USB and network Readmes carry
the facts in the same commits as the code and Codex writes their prose in
the same round. Problems found on the way that this milestone did not set
out to fix are already in `dev-log/50-shortcomings/plan.md` with file and
line, and none is fixed here unless the milestone cannot close without it.
Then notes, the milestone's Readme and the index entry.

## Risks

* Two controllers sharing nothing is the design, but the input layer
  behind them is one; the keyboard row on each controller proves it.
* A second interface changes the meaning of every address in the stack;
  the review is asked for the case where a packet arrives on the wrong
  one.
