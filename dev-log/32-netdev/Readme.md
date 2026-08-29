# Milestone 32: the third card

A third network driver, and the driver interface that carries three instead of accommodating two.

## What was wrong

`netdev_probe` tried the RTL8139 and then the RTL8168GU. The first is what `run.sh` asks QEMU for by name; the second is the chip in the Lenovo G50-70, which nothing on the development host emulates. Neither is the card QEMU attaches when nobody asks. `rach2/Readme.md` recorded the consequence and left it there: with `NET=off` the script passes no `-device`, QEMU supplies its own e1000, and the kernel could not drive it. A person running `qemu-system-i386` from a generic command line got a machine with no network and no explanation.

Three parts of `struct netdev_ops` had also been written for two drivers and no longer described what the drivers did. `tx` returned nothing, so a frame a driver refused was invisible above it, and `/proc/net/dev` printed its drop columns as literal zeros with a comment claiming that was what a healthy card reports. Nothing quiesced a bus-mastering card at shutdown; it kept writing into its receive ring while the machine reset around it. `SIOCGIFFLAGS` set `IFF_RUNNING` whenever a card was bound, so an unplugged cable reported the same as a working link.

## The driver

`sys/network/driver/e1000.c` drives the Intel 82540EM, PCI `8086:100e`, which is what QEMU's `e1000` device is. It is the descriptor-ring design with nothing else on top: two rings of 16-byte descriptors, a head the card owns and a tail the driver owns, and a done bit written back per descriptor. It has no indirect-access windows and no vendor firmware, which is what makes it the one to read before `rtl8168.c` rather than after it.

32 descriptors each way, 2048-byte receive buffers, `RCTL.SECRC` set so a descriptor's length is the frame's length with no arithmetic, and interrupt-driven receive with the shared-line service loop `rtl8139_irq` already uses. The station address comes from `RAL0` and `RAH0` when the valid bit is set and from EEPROM words 0 through 2 otherwise; both paths exist because a driver with only the first works on every emulator and fails on a cold real card. Register access goes through an indirection, as the RTL8168GU's does, so a host model can drive the whole bring-up.

Four things are checked before a received frame is handed up, and any one failing drops it and counts it: the descriptor's error byte must be clear, `EOP` must be set on the same descriptor, and the length must cover an Ethernet header without exceeding the buffer. A descriptor can report done and end-of-packet and still carry an error, and an unchecked length would hand the stack bytes past the end of the array. QEMU produces none of these conditions, so the host model produces all four.

The probe order became RTL8139, e1000, RTL8168GU, with a reason at each position recorded where the table is.

## The interface

Three additions, each with a consumer already in the tree, each implemented by all three drivers.

`tx` returns a verdict. The interface counts what each layer refused, and `/proc/net/dev` prints six real counters where it printed two. `stop` turns the receiver and transmitter off, masks the interrupts, and clears PCI bus mastering, in that order; `kernel_shutdown` calls it, and so does `netdev_probe` after a bring-up that failed, since a driver can fail after enabling DMA and an unbound driver is one nothing else will ever reach. `link` reports whether the link is up and at what speed and duplex, and `IFF_RUNNING` now means that rather than that a card is bound. A driver that cannot report a link keeps the older meaning, because for such a card that is the most that can honestly be said.

The driver was written first, against the interface as it stood, and the interface was revised afterwards with three drivers in hand. An interface designed from two examples and validated by a third is a guess.

## What the proofs found

`k_puts` writes to the VGA console only. The shutdown path used it for the goodbye line, so a machine read over netconsole, which taps serial, was never told it was stopping. Found while writing the wire contract for `netdev_stop`, whose own comment had asserted that line reached the wire.

The card really was transmitting through the reset. The mutation that removes `netdev_stop` puts `poweroff: powering off` on the captured wire, which is the evidence that a bus-mastering card kept sending while the machine stopped around it.

`rach2/sys/lib/kstring.c` cannot be compiled natively at `-O1` or above without a sanitizer: it defines `memset` as a wrapper around `k_memset`, whose loop the native compiler recognises as the memset idiom and compiles into a call to the wrapper. The suite always passes sanitizers, whose interceptor breaks the recursion, so no suite run had ever shown it. The e1000 driver keeps to plain byte loops and depends on nothing outside itself, as the RTL8168GU already did.

## Verification

`tests/host/e1000_hosttest.c` drives the driver against a software model of the chip, and `tests/host/netdev_hosttest.c` drives the table against three models whose verdicts it sets, which is the only way to ask what happens to a card that is present and will not start. `tests/run e1000` runs the probe boot and the ping, packet, netif, dgram, netconsole, raw, dhcp, and ssh work on the other card, plus three probe-order boots: two cards attached binds the RTL8139, one binds the e1000, and an 82574L that no driver claims reports no supported card and still reaches a shell. The `dhcp` target, `runtests`, and `ssh.sh` take the card as a parameter, defaulting to the RTL8139 so every existing invocation is unchanged.

`procfiles` asserts the sixteen `/proc/net/dev` fields and that `ifconfig` reports the interface running. `netconsole` asserts the shutdown wire contract through a new `pcapcheck --reject`, because a line that was never sent and a line that was sent and missed look the same from inside the machine.

Forty-three mutations across five scripts each made one named check fail and only that one. A cold build from `distclean` takes 35 seconds at `-j8`, unchanged from milestone 30's baseline. The kernel grew 5,687 bytes of text and 116,000 of bss, the second being the rings and buffers. All 44 suites pass.

## Not done

No 82574L, no jumbo frames, no offloads, MSI or MSI-X, multiple queues, VLAN handling, interrupt throttling, or runtime power management. The stack still has one interface. The storage drivers have the same DMA exposure at shutdown and no equivalent to `stop`. Nothing in this milestone touched the laptop.
