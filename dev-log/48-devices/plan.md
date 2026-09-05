# Milestone 48: USB and network

## The goal

Drive every supported xHCI controller and network card, however many the machine has and in whatever order they are found: two xHCI controllers, a dozen or more USB devices behind several hubs, two network cards of the same model or different ones, each interface with its own address, and a card that fails leaving the others working. What the kernel cannot drive, an EHCI-only machine or a wireless card, it says by name at boot. The milestone is complete when the proof set below passes in QEMU with two controllers and two cards and the Lenovo's own devices work as before.

## What the tree settles before this plan

* xHCI is the one host-controller driver. Discovery takes one controller and keeps its state globally (`xhci.h:7`, `xhci.c:297`); the limits are 12 devices, 2 hubs, 12 slots, 16 root ports and 32 scratchpad pages (`core.h:20`, `hub.h:16`, `xhci.c:543`); extra ports are clipped and a controller needing more scratchpads is rejected; a hub with more than fifteen ports is clipped to `USB_HUB_PORT_MAX` (`hub.c:340`, `core.c:483`), though the xHCI specification defines route encoding for full- and high-speed hubs with more ports; a configuration descriptor is truncated at 64 bytes (`core.c:999`); the command, event, endpoint and completion rings are fixed. An Intel controller's port routing is written on vendor identity alone (`xhci.c:267`). EHCI is declined (`usb.c:9`).
* Three network drivers; the common layer keeps one selected driver (`netdev.c:21`), the first that comes up is the interface and a supported card that fails stops the probe (`netdev.c:35`); each driver keeps one instance in globals (`rtl8168.c:106`); the stack has one MAC, one address configuration and one interface state (`netif.c:49`, `net.h:45`); a route is the connected subnet or the one gateway (`ip.c:263`); ARP entries have no interface. Manifests name `eth0` and the resolver files carry QEMU's addresses (`rach2-metal-disk.manifest:133`).
* The platform: power management uses a small chipset table (`board.c:42`); reboot sends an Insyde-specific SMI command first without checking the firmware (`power.c:241`); audio takes one HDA controller (`hda.c:1605`). PCI already scans all 256 buses (`pci.c:217`). Milestone 46 allocated vectors and made the metal acceptance read them by device name.
* Milestone 46 allocates objects and interrupt vectors; milestone 47 put USB disks in the block registry.

## Decisions

### Controllers are instances

xHCI state is per controller, every supported controller is driven, and each has its own vector, rings and completion state. Ports and scratchpads are sized from the controller's capability registers. Device and hub records are allocated; a slot count is the controller's own limit and a device that finds no slot waits for one. Configuration descriptors are read whole and validated. The Intel port-routing write is done only on a controller whose identity matches a list, with the reason recorded.

### EHCI is named, not driven

An EHCI-only machine is older than this project's target and a driver would be a project of its own, with transaction translators and companion controllers behind it. Log unsupported controllers, for example `usb: unsupported: ehci at 00:1d.0`. UHCI, OHCI and UAS are the same.

### Interfaces are instances

Each card has a driver instance, MAC address, address configuration, link state and queues. Failure of one card does not stop the others; two cards of one model work. Interfaces are `eth0`, `eth1` in probe order and may be configured by MAC so probe order does not decide policy. Each ARP entry includes its interface. Routes select an interface. Configure connected routes for each interface and one default route; packet sockets, DHCP and interface ioctls name the interface. Bonding, bridging, forwarding, wireless and IPv6 are not this milestone's. The manifests stop naming QEMU's addresses.

### The platform checks before it acts

Send the Insyde reboot command only when the firmware identifies itself as Insyde. Use a chipset-specific sequence when the table has a matching entry. Otherwise use the standard paths; HDA drives the first controller and names any other.

## Phases

### Phase 1: xHCI instances

Per-controller state, capability-sized arrays, allocated records, descriptors whole. Proof: QEMU with two xHCI controllers, a keyboard on one and three sticks behind a hub on the other, all working, a stick pulled from each while the other controller is busy, one controller failed at probe with the other up, every slot taken and a device waiting for one and getting it; a row with more devices than the old twelve; a full-speed or high-speed hub with a device on a port above fifteen, driven through the route encoding the specification defines for it; a configuration descriptor longer than 64 bytes read whole and one with a wrong length refused.

### Phase 2: interface instances

Driver instances, the stack per interface, ARP and routes by interface, configuration by MAC. Proof: QEMU with two e1000 cards on two subnets and again with an RTL8139 beside an e1000, both reachable at once under traffic, SSH through each, DHCP on each interface by name, one ARP destination reachable through each; one card failed at probe with the other up, and one taken down after startup with the other still serving; the sealed image's network files carrying no QEMU address.

### Phase 3: the platform and the machine

The firmware check before the SMI reboot; the chipset table's fallbacks;  the Lenovo booted with its keyboard, mouse, sticks, hub and card as before. Proof: a host test of the reboot decision that feeds it the Lenovo's firmware strings and another's and checks which port writes it makes; the metal acceptance; the reboot from SSH.

### Phase 4: the close

Proof, once and in this order: build from clean; the full set; everything that comes up fixed; the whole milestone's diff to Codex in units of six files, most serious first; every finding fixed; the full set again; then the Lenovo and the SSH capstone. Update the USB and network Readmes in the commits that change their contracts. Codex writes the prose from verified facts. Problems found on the way that this milestone did not set out to fix are already in `dev-log/50-shortcomings/plan.md` with file and line, and none is fixed here unless the milestone cannot close without it. Record the results in `notes.md` and the milestone's Readme. Add the index entry.

## Risks

* Both controllers feed the shared input layer. Test a keyboard on each controller.
* A second interface changes the meaning of every address in the stack; the review is asked for the case where a packet arrives on the wrong one. 
