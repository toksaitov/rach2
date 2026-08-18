# Milestone 24: onto the notebook

An old Rach2 booted a real Lenovo G50-70. The current one does not, and this milestone is the work of making it boot that machine again, from a USB stick, with networking, so that developing on it stops meaning photographing its screen and starts meaning an SSH session.

The plan is in `plan.md`, written before any code and reviewed to agreement. This file records what was built, what was proved and where, and what is still waiting for the hardware. It is written while the local half is complete and the machine has not yet been booted, so every claim below says which of the two it belongs to.

## What the machine is, and what that decides

A Lenovo G50-70: Haswell, Insyde firmware in CSM mode, one SATA SSD holding a Windows installation, and a Realtek RTL8168GU on the PCI bus. Three facts about it shaped everything.

It has no serial port. Every other machine this project has run on could be debugged by reading a serial line; this one cannot, so a boot that stops leaves nothing but the last line on its screen. That is why the module-placement check formats its refusal into the panic rather than the serial log, why there are switches to skip a device probe one at a time, and why the kernel log is repeated onto the wire as broadcast frames.

Its SSD is somebody's Windows. Nothing this milestone builds may write it, and a promise that size cannot rest on remembering not to call the write path, so it is a build switch and an allowlist in the drivers, checked on the artifact and on the bytes of a disk.

Its network card is not emulated by anything on this desk. The driver was ported from a pinned Linux reference and its first execution will be on the laptop, so the weight a boot usually carries is carried instead by a software model of the chip in a host test, and by review.

## Phase A: the image and the layout check

A `rach2-metal` image profile: busybox, the set-id trio, dropbear, `/etc`, the authorized keys, and nothing else. No compiler, no `/src`, no installer; a machine with no writable disk cannot use them, and every byte costs RAM under the kernel's 32 MiB direct map. It builds to 2.4 MB of 4.

The boot now refuses a module placed over the kernel, past the direct map, or over another module, and the refusal names the rule and the range. The check is a pure function (`sys/mm/modlayout.c`) with a host test, because QEMU can craft only one of its four refusals: an oversized module is a file, but no bootloader will place two modules over each other on request. `modlayout_describe` formats the whole refusal into a caller's buffer so it can reach `k_panic`, which is the only output a machine with no serial port has.

`rach2.diag` prints one report before any driver runs: where the kernel ends, where the allocator's break sits, where the direct map ends, how many frames survived, every module's range, and the Ethernet controller's identity read from `TxConfig`. It reads and configures nothing. Rehearsed against the RTL8139 it found the device, stepped over the I/O BAR to the memory BAR, mapped the window, read `TxConfig` and refused the wrong revision, so only the 8168's answer to it is unrehearsed.

## Phase B: a device interface, a ported driver, and the log on the wire

The stack no longer names a card. A driver publishes a `netdev_ops` and `netdev_probe` binds the first that answers; the RTL8139 moved behind it unchanged, which is what keeps every existing test's machine exactly what it was. A driver's `init` reports three things and not two, because "the card is not here" and "the card is here and would not start" are different diagnostics and on a machine with no network to report over, the second is the only one there is.

`driver/rtl8168.c` is a port of Linux v6.16 (commit `038d61fd642278bab63ee8ef722c50d10ab01e8f`) for the version that file calls `RTL_GIGA_MAC_VER_42`, and every sequence names the reference function it came from. Two departures are deliberate and are in the file's header: link state is read from `PHYstatus`, which the reference declares and never reads because phylib reaches the PHY over MDIO, and the PHY is powered up here, because the reference suspends it at probe and lets phylib wake it and nothing here would.

`tests/host/rtl8168_hosttest.c` drives the whole bring-up against a software model of the chip's registers: completion flags that move only when a command completes, an out-of-band bit that must be cleared, a link list handshaked twice, descriptors whose ownership moves, and a recorded write log the test asserts the reference's order against. It cannot prove PCIe ordering, that DMA reaches the rings, the PHY's analog behaviour, or whether this board needs the firmware patch the port omits.

Beside it, netconsole: the serial stream repeated into broadcast frames of EtherType `0x88b5`, below IP on purpose, so it works on the first boot on an unknown network and keeps working when the address configuration is the thing that is wrong. `make netconsole-test` captures every frame QEMU's guest sends and checks one boot id, sequence numbers in order, lengths, checksums, and a line printed before the network device existed, which is what proves the buffer held what came before transmitting was possible.

## Phase C: the tick, the address, the reset, and an unattended server

The 100 Hz timer drives a polled device (`netdev_poll`), and the RTL8168GU's transmit became asynchronous: hand the frame over, ring the doorbell, return. The tick reclaims and rings again for anything outstanding, so a lost poll request costs 10 ms rather than the connection. A full ring is the one case a transmit still waits on, briefly, because dropping the moment a burst outruns the chip loses frames the next tick would have made room for.

The address comes from the boot line: `rach2.ip=A.B.C.D/prefix` and `rach2.gateway=A.B.C.D`, parsed into the one configuration every layer reads. The rule about defaults is the part worth remembering: with neither option the machine keeps the SLIRP tuple it always had, and with an address but no gateway there is NO gateway at all rather than the SLIRP one, because `10.0.2.2` beside a real LAN address is a router that does not exist. The parser refuses more than it accepts, and `make test-addressed` runs the entire probe suite with the address named on the boot line instead of compiled in, which is what proves the configuration reaches ARP, IP, TCP, the sockets and the boot line.

Restarting the machine now tries three mechanisms in order, most of the machine first: the firmware's own SMI handler, then the chipset reset register, then the keyboard controller pulse. Trying them in the other order works on QEMU, where the chipset register is the one that acts, and would leave the target parked at a halt, which over SSH is indistinguishable from a kernel that died.

`init-metal`, the third build of `utils/init.c`, supervises dropbear beside the console login, because nobody is going to type a command at a machine across the room. `make metal-test` is the rehearsal: the image is given power and a command line, NOTHING is typed at its console, and then SSH by key, a pty, a file each way compared by checksum, and the server killed by pid with a different pid required to answer.

## Phase D: the disk it must not write, and the controller it must reset

A second kernel is built with `-DRACH2_DISK_READONLY` into its own object tree. Under it, both drivers consult an allowlist where a command leaves them for the device, and only the four reading commands are permitted; everything else is refused, including commands the file has never heard of. A build switch and not a boot flag, because a boot flag is editable at the GRUB prompt by whoever is sitting at the machine.

Which policy an artifact carries is read off the artifact, since checking for the absence of a write path proves nothing when the commands are numbers and the functions are static. `make diskpolicy-test` then shows it on the bytes: the same freshly built disk booted with each kernel, the ordinary one required to mount and change it, and the metal one required to refuse a command by name and leave the image byte for byte as it was.

The metal ISO carries that kernel and no other profile does, checked where the image is built rather than assumed: staging the ordinary kernel under the metal name would produce exactly the image this milestone promises not to make, and for a while it did.

The AHCI driver now resets the controller and re-establishes each SATA link. Milestone 10 had to skip the HBA reset, because without a COMRESET the link never comes back and the port reports a device that is never ready; with the COMRESET that is a state the driver can get out of, and the reset is worth having on a machine that takes the controller over from firmware that has been using it. Two details had to be right and were not at first: the received-FIS area must be programmed and `PxCMD.FRE` enabled before the reset, because the device answers with a FIS and a FIS arrives only if the port is listening; and the signature must not be read until the task file says the device has finished its own reset, since before that it reads as all ones.

## What was found on the way

Three defects were found by measurement or by a test rather than by reading, and each was in code that looked right.

Every hardware wait in the kernel was between two and seven times too short. `init_pit` writes control byte 0x36, which selects mode 3, where the 8254's counter steps by two per input clock, so a delay converting counts to microseconds at the input frequency measures half of what it should; and the window multiplication overflowed 32 bits besides. A boot self-test that measures 200 ms of `k_udelay` against the 100 Hz tick read 30 ms before the fix and 200 ms after. Nothing in QEMU cared. On the chip this milestone exists for, every settle time in the bring-up would have been short.

The bounded-wait helper in the RTL8168 driver accepted any bit of its mask where the reference requires all of them, so a chip with one FIFO still holding data would have been treated as drained and reset underneath itself. That came from an independent review against the reference.

The netconsole's drain followed a moving end, so a device that reported an error from inside every transmit would have kept the machine inside one `printf` forever, transmitting the report of its own failure to transmit. The host test written to provoke exactly that hung, which is also why every host test now runs under a watchdog.

## The supported surface, and what is excluded

An excluded device is listed here rather than skipped quietly, because a reader who cannot tell "not built" from "built and broken" cannot use this record.

| Part | On the metal image | Proved by |
|------|--------------------|-----------|
| Boot from a USB stick (CSM, El Torito hybrid ISO) | yes | the laptop, unproved |
| VGA text console and the 8042 keyboard | yes | the laptop, unproved |
| RTL8168GU Ethernet | yes | the chip model in `verify-host`; the wire waits for the laptop |
| Netconsole over that card | yes | `make netconsole-test` over the RTL8139; the 8168 waits |
| Boot-line IPv4, ARP, ICMP, UDP, TCP, sockets | yes | `make test-addressed`, the whole suite |
| Dropbear over SSH, supervised by init | yes | `make metal-test`, unattended |
| The console login as the recovery path | yes | `make metal-test`, `make login-test` |
| AHCI to identify and read | yes | `make test-disk-ahci`; the reset and COMRESET wait for the laptop |
| Reading the disk without mounting it | yes | `make diskdiag-test`, on the bytes |
| Writing any disk | NO, refused by the build | `make diskpolicy-test`, on the bytes |
| Restart | yes | `make reset-test`, each mechanism alone |
| Power off | yes, when a profile matches | `make shutdown-test`; the laptop's chipset row is unproved |
| Trusted entropy for the host key | required | `make metal-test`; needs RDRAND on the laptop |
| HD Audio | NO, excluded from the image | nothing stages an audio program; the driver is in the kernel |
| The compiler, `/src`, pdpmake, the installer | NO, excluded from the image | a machine with no writable disk cannot use them |
| Doom | NO, excluded from the image | RAM under the 32 MiB direct map |
| Writing the SSD, carving a partition, a persistent host key | NO, phase E | gated on the owner's approval after a full backup |
| DHCP, DNS, IPv6, a second interface | NO | out of scope; a numeric address is the contract |

## A kernel bug this milestone's own growth exposed

The self-build capstone failed near the end, and it was not a flake. `sys_getdents64`'s directory cursor was a count of the children seen so far. `rm -rf` reads a batch of names, unlinks them, and reads on from that cursor, so every unlink renumbered the survivors and the next read skipped one entry for each file already gone. The leftovers made `rmdir` refuse, and all a user saw was "Directory not empty".

It had presumably always been there, and it surfaced now because this milestone added enough files to `rach2/sys` that listing it stopped fitting in one call: below that threshold every name is read before any unlink, and the defect cannot show.

The cursor is now the byte offset of the last node returned. That survives deletion, because removing a node only widens free extents and allocation splits a free node rather than shifting a reserved one. It does not survive a file growing out of its extent, which relocates that node; a child that grows while another descriptor is mid-listing can be missed or repeated. That is a concurrent modification of a directory being read, which POSIX leaves unspecified, and it is not reachable from the loop the cursor exists for. The boundary is written in the function rather than left to be rediscovered. `tests/unlinkdirprobe.c` reproduces the original defect with a deliberately small buffer and fails against the old cursor.

## What is proved, and where

In QEMU, from a clean tree: the whole existing probe suite, twice, once with the address compiled in and once with it on the boot line; the netconsole capture; every subset of the optional device switches; the read-only kernel against a real disk image; all three AHCI paths with the controller reset in place; the SSH capstone; and the unattended metal rehearsal. On the host: 27 seam tests, including the chip model, the address parser, the command allowlist and the netconsole.

Nothing here has touched the laptop. Everything below is unproved until it does.

Who supplies each proof matters, because some of it cannot be scripted. The scripted half of the acceptance is `tools/metal_accept.sh`, run over SSH from the development host: it reads what the machine says it is, checks the running kernel is the read-only build, checks the disk is not mounted, and compares the drive's identity and the hash of its first sector against `baseline.md`. It is rehearsed inside `make metal-test`, so it is not being debugged on the machine it is meant to measure. The human half is `checklist.md` and stays human: a cable pulled and put back, cold power cycles, a screen photographed, a sound heard, and Windows booting with the stick out.

## What was refuted

* That nothing calls `netdev_poll` was raised as a defect during phase B's review and refuted: the plan assigns the timer hook to phase C, where it landed.
* That `install-test` had regressed: it failed once and passed on a serial re-run. Its matrix boot has a 75-second bounded wait and three QEMU tests were running at once; the script is byte-identical to the milestone-23 close.
* That the AHCI host test needed a full register model like the RTL8168's. The bounds that matter are arithmetic, and one of them was a real defect (an all-ones capacity accepted, truncating every later LBA), fixed in the driver where it lives.
* The plan's literal poweroff numbers. `0x1804` is derivable and is now derived from the chipset's own base register; only the sleep type is a constant, carried per profile with its provenance, because it lives in AML this kernel does not interpret.

## The first LAN session, measured

The machine was booted from the stick on a real network and reached over SSH from the development host. What that settled:

* The ported driver works. `xid 0x509` as predicted, `Network... OK (RTL8168GU)`, ARP resolved, ICMP both ways, and a TCP session carrying SSH. The bring-up sequence, the four indirect windows, the descriptor rings and the PHY all behave on the chip nothing here can emulate.
* Init brings the machine up unattended: dropbear was listening before the login prompt appeared, and root authenticated by key with nothing typed at the console.
* `lynxpoint-lp` matched once the mobile device-id range was added, and `poweroff` cuts power rather than halting.
* Name resolution needed `/etc/resolv.conf`, which the metal image did not stage. It names 1.1.1.1, which is what this LAN's router hands out; the router forwards DNS too, but a fixed public resolver keeps working if the stick moves.
* A 104 KB file crossed in each direction, byte for byte by checksum.

Two throughput numbers, which the plan asked for and which say different things:

| Measurement | Laptop, RTL8168GU, polled | QEMU, RTL8139, interrupt-driven |
|---|---|---|
| A whole SSH session: connect, authenticate, run, close | 0.3 s | comparable |
| Bulk transfer, 104 KB one direction | 0.9 KB/s | 1.1 KB/s |

The interactive figure is what remote development needs and it is fine. The bulk figure is poor, and the comparison is the point: the polled driver on real hardware is within twenty percent of the interrupt-driven one in QEMU, so this is not the new driver and not the 100 Hz poll. It is a defect in the TCP stack that predates this milestone, and it is recorded here as the next thing worth chasing rather than attributed to the hardware. (Milestone 25 phase A found and fixed it: the receiver never told a peer that its window had reopened, so the peer's persist timer set the rate. The same 64 KiB transfer now takes 0.291 s instead of 60.179 s. See `dev-log/25-sealed/Readme.md`.)

## What waits for the hardware

* That the RTL8168GU driver works at all. The model proves the sequences and their order; it cannot prove PCIe ordering, that DMA reaches the rings, the PHY's behaviour, or whether this board needs the firmware patch the port omits. The first proof is a packet capture on the development host showing our frames with the machine's own source address.
* That the AHCI reset and COMRESET behave on a Lynx Point controller, and that COMRESET is in fact the recovery for the link that comes up dead after a warm reboot.
* That the three-mechanism restart restarts this machine, and that poweroff works: the ACPI path still derives its address from a PIIX4's configuration space and will refuse on this chipset, falling back to a halt. The PCH power-management path and the ACPI-enable handshake are written down in the plan and not yet built.
* That the module layout fits under the direct map on this machine's memory map.
* Everything about the SSD beyond reading it, which is phase E.

## Phase E, designed and deliberately not built

Phase E's local half was designed in full and then not built, which is a decision rather than an omission, so the reasons are here.

It is gated twice by the plan's own terms: on the owner's explicit approval after a full backup of the SSD, and on the MBR inventory. Neither gate has opened, so the carved topology those pieces address does not exist. Building them now would mean shipping a `rach2.bootuuid=` selection that nothing can select, and adding a GRUB entry to the stick that addresses volumes that are not there. The second is worse than useless: the stick that works today is the one the first laptop session depends on, and an entry pointing at a missing volume is a way to break it before it has ever been used.

The design also had one part that should not be built as designed, and it is worth recording so it is not rediscovered later. To make the fixture bootable it published a WRITABLE `/dev/bootpart` window over a FAT volume, chosen by a boot-line token, under the ordinary read-write kernel. That is precisely the shape this milestone spent phase D removing: a boot flag is editable at the GRUB prompt, and the volume in question would sit on the disk holding somebody's Windows. Whatever phase E builds, the write side of it belongs behind the build switch, not behind a token.

What is inherited when phase E begins: the MBR arithmetic is already decided by the inventory (entries 3 and 4 are free, so the carve uses primary slots and no extended-partition support is needed), `baseline.md` holds the numbers to check the disk against, and `/proc/disk` and `/dev/disk0` are the instrument for checking them.

## The baseline, and its deadline

`baseline.md` holds what Windows measured on this machine before Rach2 had touched it: the drive's model and serial, its 1953525168 sectors, its disk signature, its two partition entries and the SHA-256 of its first sector. It exists in the tree because `Lenovo_G50-70_report_temp.txt`, where those numbers came from, is untracked and is deleted when the hardware work concludes, and because the SSD half of this milestone has no other baseline to check itself against.

## The record's own housekeeping

`Lenovo_G50-70_report_temp.txt` sits untracked in the repository root and is deleted at the very end of the milestone, once the hardware work has concluded. It has never been committed.
