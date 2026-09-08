# Milestone 48: USB and network devices

The kernel drives every xHCI controller the machine has, up to four, and every network card its three drivers claim, in whatever order the bus presents them. Each controller and interface has its own record. The address, driver and rings that were shared machine-wide are now maintained per device. Platform decisions previously written for one machine now use information from the running machine. Nothing here ran on the Lenovo.

## What is built

`struct xhci_ctrl` represents one controller: its aperture, capability record, command and event rings, context array, port state, slot table, completions and the command in flight. Discovery scans the whole bus and allocates a record for each controller, up to four. It reports controllers of other interfaces without driving them and reports the address of each controller beyond the fourth. If initialization fails, the driver returns what it acquired from the firmware and closes that controller while continuing to initialize the others. Every driver function, including the register accessors, takes the controller record, so a host test can model two controllers at once.

The driver sizes the port state from the controller's reported port count and the slot table and context array from its slot count. It enables every slot the controller supports. A slot's device context and each endpoint's ring occupy one heap page each, allocated when the slot is addressed and freed when it closes. The heap is not identity-mapped, so the driver records each page's physical address beside the pointer the kernel uses to access it. Every address written for the controller to read is that physical address. The driver allocates the number of scratchpads specified by the controller.

Each device record and hub record occupies a frame from the identity-mapped window. This lets the controller write descriptors and reports into the records and provides enough space to read a complete configuration. If no slot is available for a device, slot allocation waits until another device is removed. A hub port above fifteen is addressed through the route encoding, which is legal only where a translator identifies the port. Each completion identifies its controller, since slot numbers start at one on every controller.

`struct netdev` represents one interface: the driver, the driver's record for the card, the interface name and index used by the rest of the kernel, the card's station address, the interface's address, mask and gateway, its counters, and its administrative up or down state. `netdev_probe` enumerates each driver's cards and creates an interface for each, naming them `eth0` upward. If a card is present but fails to initialize, it reports the card and the reason, omits that card, and continues to the driver's next card. The e1000 has a record per card and allocates its rings and buffers from identity-mapped frames during initialization instead of using compiled-in storage. The RTL8139 and the RTL8168 drive one card each.

A datagram is sent through the interface selected for its destination, with that interface's address as its source. ARP entries record the interface on which they were learned. A TCP segment's checksum uses its destination address, and a connection records the local address on which it arrived, so a service replies through the card on which the connection arrived. A packet socket bound to an interface receives that interface's frames and sends through it. The kernel checks whether a frame is addressed to this machine against the receiving card's station address. `SIOCGIF*` and `SIOCSIF*` read and write the configuration of the interface specified by the caller. The first interface's configuration is also the machine's, since TCP still uses one source address when opening a connection. `rach2.ifaddr=<mac>,<addr>/<prefix>[,<gateway>]` configures an interface by its station address, so configuration does not depend on probe order.

`smbios.c` reads the BIOS vendor from the firmware table, bounds every field by the length specified in the anchor, and maps the table if it lies above the identity-mapped window. The reset code sends `0xFB` to port `0xB2` only when the reported vendor matches the firmware whose system-management handler implements that command. On every other machine, it uses the chipset register and reports the firmware vendor it read. The audio driver reports any controller it does not drive.

## What proves it

`tests/run usb` boots two controllers with a hub and four devices distributed across them and requires each controller to have its own interrupt. It sends keyboard input through the machine's only keyboard, behind the hub on the second controller. It mounts three sticks across both controllers and removes one from each controller while reading a stick on the other. It uses `rach2.hcfail=N` to fail a controller's initialization and verifies that the others initialize without it. `hub_hosttest`, `xhci_hosttest`, `usb_hosttest` and `msc_hosttest` exercise two modelled controllers, waiting for a slot, route encoding and reading a complete configuration.

`tests/run netif` boots two e1000 cards on two subnets and an RTL8139 alongside an e1000, with each interface configured by its station address. It pings a neighbour on each subnet, reads `/proc/net/dev` for both interfaces, and binds a packet socket to each. It requires every datagram to identify its receiving interface. It takes the second interface down and requires the first to continue serving, and it uses `rach2.nicfail=N` to fail a card's initialization. `tests/run dhcp` boots two cards on two networks with a server on each and obtains a lease on each interface by name. It runs the SSH server and requires it to answer a connection arriving on either card. Each network's capture must contain packets from the address leased on that network.

`tests/run reset` boots a machine with SeaBIOS and one given `-smbios type=0,vendor=Insyde Corp.`. It requires the firmware-specific reset command to be skipped on the first and sent on the second. `power_hosttest` tests vendor reads from tables with arbitrary vendor strings, no vendor string, lengths extending beyond the table, or missing terminators.

## The machine

Nothing in this milestone ran on the Lenovo. It has one xHCI controller and one network card, so this milestone's system tests ran only in QEMU. Milestone 50 will test this kernel on the Lenovo.

## Trust

The machines with two controllers and two cards are emulated by QEMU. These tests show that the kernel keeps one record per device and responds through the correct device. They provide no evidence about operation on a real machine with two controllers; the project has no such machine.

The device table still has twelve entries, and the plan required a test row with more devices. Increasing the table size exceeds the modelled hub's port capacity, as recorded in entry 19 of the shortcomings list. The fixed table size imposes the limit; memory allocation does not, since each device record occupies a frame allocated when the device appears.

The firmware check is tested against a table supplied by QEMU on request and against tables constructed byte by byte in a host test. The kernel's command has not been tested with Insyde firmware; that test is scheduled for milestone 50.

The RTL8139 and the RTL8168 drive one card each, so a machine with two of either has one interface and the second card is not driven. The e1000 is the driver with a record per card.

## What was accepted

* Commit: `45d2a24`, the last change of the code; the documents came after it.
* Build options: the repository's default kernel builds, `-ansi` with `-Werror` and no libgcc, and since milestone 47 with `-Wdeclaration-after-statement`. The host tests build with `-m32 -mno-sse -mno-sse2 -ansi` and `-Werror`. The device suites boot QEMU with `-cpu max`; the two-controller rows add `-machine pc,i8042=off` so a keystroke can have come only from USB, the two-card rows give `rach2.ifaddr=` once per card, and the firmware row gives `-smbios type=0,vendor=Insyde Corp.`.
* SHA-256 of the kernels and images the closing run used:
  * `53763ad52a9486832afe8fcdb29d7a92f35eaae47cd5769bef75f38424bb044d` `build/rach2/kernel`
  * `9129b122c206c870f40253fcf28fa20e084f6fc67e5d9b601014994b47b25bb6` `build/rach2-recovery/kernel`
  * `df9f449db688e703593266815596368ed5b72e23aef92b0ac137999752de2d90` `build/rach2-metal/kernel`
  * `4720d4068f4bfc22bba3045f1e4ad2289a61dfc9f5fc3728c421e4563910843f` `build/images/test.krffs`
  * `7591b2c6865aa59e6a3696d6ea8e2e6f83bf6d9db57dd51162943f69a4609569` `build/images/os-simple.krffs`
  * `5cb02c6c2121b272d42c80a801c1d1c5152b56e92904cdb121ac551605055fb0` `build/images/os-real.krffs`
  * `dc495cdc5ea9ad49378228ae1b508250aaa645ee487a7440b9f06e62fc1c3143` `build/images/rach2-recovery.krffs`
  * `0bef7963030326b76ac4f187d656a9ef27ba658ccd53952573ca547ecbef5720` `build/images/rach2-metal.krffs`
* Test commands and results, both at 20 workers on 2026-09-08:
  * `rm -rf build && tests/run all` at the end of phase 3, before the review: all 84 suites passed.
  * `rm -rf build && tests/run all` at `45d2a24`, after every review finding was fixed and the rows the plan's proof set asked for were written: all 84 suites passed in 2186 s, the clean build included.
  * A run between the two was voided: its sources were edited while it ran, so the suites that started later booted a kernel the earlier ones had not been judged on. Its result is not recorded here.
* Review: the milestone's diff, `204a9cb..828b12c`, went to Codex in six units grouped by contract, at most six files each: the controller and its driver, the device core and the class drivers, the interface layer and its drivers, the address and socket layers, the platform, and the suites. They returned 29 findings. Every one was fixed, in `75987a1` through `139f80b`; none was rejected. The prose of this Readme and of `notes.md` is Codex's, from the facts gathered here.
* Hardware arrangement: Apple M4 host, macOS 26.6.2, QEMU 11.1.1. No run or write on the Lenovo.
