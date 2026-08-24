# Milestone 24: booting the notebook

An earlier Rach2 booted a Lenovo G50-70. The current system does not. This milestone restores a networked boot from a USB stick so development can use SSH instead of photographs of the screen.

The reviewed `plan.md` was written before implementation. This record distinguishes what was built and proved locally from what still requires the hardware. At the time of writing, the local work was complete and the notebook had not booted the new system.

## Hardware constraints

The Lenovo G50-70 has a Haswell processor, Insyde firmware in CSM mode, one SATA SSD with Windows installed, and a Realtek RTL8168GU on the PCI bus. Three properties determined the design.

The notebook has no serial port. Earlier machines provided a serial log, but a stopped boot on this notebook leaves only the last screen line. The module-placement check therefore includes its diagnostic in the panic. Boot switches can skip individual device probes, and netconsole repeats the kernel log in broadcast Ethernet frames.

The SSD contains another person's Windows installation. This milestone must not write it. A build switch and driver command allowlist enforce that rule, and tests check both the kernel artifact and the disk bytes.

No available emulator implements this network card. The driver comes from a pinned Linux reference, and its first execution will be on the notebook. A host test uses a software model of the chip, and review compares the port with its reference.

## Phase A: the image and the layout check

The `rach2-metal` image contains BusyBox, the three set-id programs, Dropbear, `/etc`, and the authorized keys. It omits the compiler, `/src`, and the installer because a machine without a writable disk cannot use them. Image size consumes RAM below the kernel's 32 MiB direct-map limit. The image uses 2.4 MB of the available 4 MB.

The boot rejects a module that overlaps the kernel, extends beyond the direct map, or overlaps another module. Its diagnostic identifies the rule and address range. A host test exercises the pure check in `sys/mm/modlayout.c`. QEMU can produce only one of the four failures: an oversized module can be supplied as a file, but a bootloader cannot be directed to overlap two modules. `modlayout_describe` writes the complete diagnostic to the caller's buffer for `k_panic`, the only available output without a serial port.

Before any driver runs, `rach2.diag` prints one report with the kernel end, allocator break, direct-map end, surviving frame count, every module range, and the Ethernet controller identity from `TxConfig`. It reads and configures nothing. The RTL8139 rehearsal found the device, skipped its I/O BAR for the memory BAR, mapped the window, read `TxConfig`, and rejected the wrong revision. Only the 8168 response remains unrehearsed.

## Phase B: the network interface, driver, and remote log

The network stack no longer selects a specific card. Each driver provides `netdev_ops`, and `netdev_probe` binds the first device that responds. The unchanged RTL8139 driver moved behind this interface, preserving the virtual machine used by every existing test. A driver's `init` distinguishes three results rather than two. The result "the card is not here" differs from "the card is here and would not start"; without working networking, the latter diagnostic is the only report of an initialization failure.

`driver/rtl8168.c` ports the `RTL_GIGA_MAC_VER_42` support from Linux v6.16, commit `038d61fd642278bab63ee8ef722c50d10ab01e8f`. Each sequence identifies its reference function. The file header documents two deliberate differences. This driver reads link state from `PHYstatus`; the reference declares that register but uses phylib and MDIO instead. This driver also powers up the PHY because the reference suspends it during probe and relies on phylib to wake it.

`tests/host/rtl8168_hosttest.c` exercises complete initialization against a software model of the chip registers. Completion flags change only when commands finish. The model requires clearing an out-of-band bit, performs the link-list handshake twice, transfers descriptor ownership, and records writes for comparison with the reference order. It cannot prove PCIe ordering, DMA access to the rings, analog PHY behavior, or whether this board requires the omitted firmware patch.

Netconsole copies the serial stream into broadcast frames with EtherType `0x88b5`. It operates below IP so the first boot can report on an unknown network and address-configuration failures do not disable logging. `make netconsole-test` captures every frame from the QEMU guest. It checks one boot id, ordered sequence numbers, lengths, checksums, and a line printed before the network device existed. The last check proves that the buffer retained output produced before transmission became possible.

## Phase C: polling, addressing, reset, and unattended SSH

The 100 Hz timer calls `netdev_poll` for a polled device. RTL8168GU transmission is asynchronous: give the frame to the device, ring its doorbell, and return. Each tick reclaims completed descriptors and rings the doorbell again when work remains. A lost poll request therefore delays progress by 10 ms instead of stopping the connection. Transmission waits briefly only when the ring is full; immediately dropping a burst would lose frames for which the next tick could free space.

The boot line supplies `rach2.ip=A.B.C.D/prefix` and `rach2.gateway=A.B.C.D`, which the parser converts into the single configuration read by every network layer. Without either option, Rach2 keeps its existing SLIRP settings. With an address but no gateway, it configures no gateway; retaining `10.0.2.2` with a real LAN address would specify a nonexistent router. The parser accepts only its defined forms. `make test-addressed` runs the complete probe suite with a boot-line address instead of the compiled address, proving that the configuration reaches ARP, IP, TCP, sockets, and the boot command line.

A restart tries three mechanisms in order: the firmware SMI handler, the chipset reset register, and the keyboard-controller pulse. This order begins with the mechanism that resets the most hardware. Reversing it works in QEMU because the chipset register responds there, but would leave the target halted. Over SSH, that halt cannot be distinguished from a kernel failure.

`init-metal`, the third build of `utils/init.c`, supervises Dropbear alongside the console login so the remote notebook requires no console input. `make metal-test` rehearses unattended operation. The harness supplies power and a command line but types nothing at the console. It then checks key-authenticated SSH, a pty, a file copied in each direction and compared by checksum, and server supervision by killing its pid and requiring a different pid to answer.

## Phase D: read-only disks and AHCI reset

A second kernel uses `-DRACH2_DISK_READONLY` and a separate object tree. In that build, both disk drivers check a command allowlist immediately before submitting work to the device. Only the four read commands are accepted; every other command, including an unknown one, is rejected. This policy is a build option rather than a boot flag because a person at the GRUB prompt can edit boot flags.

The artifact identifies which policy it implements. Searching for an absent write path proves nothing because commands are numeric and the relevant functions are static. `make diskpolicy-test` then checks behavior against disk bytes. It boots the same newly built disk with each kernel. The ordinary kernel must mount and modify it; the metal kernel must reject a named command and leave the image byte-for-byte unchanged.

The metal ISO contains that kernel, and no other profile does. The image build checks this distinction. Staging the ordinary kernel under the metal name would create exactly the image prohibited by this milestone, and an earlier version did so.

The AHCI driver resets the controller and reestablishes each SATA link. Milestone 10 skipped the HBA reset because the link never recovered without COMRESET, leaving a device that never became ready. COMRESET provides a recovery path, so the driver can reset a controller inherited from active firmware. Two details were initially wrong. The received-FIS area must be programmed and `PxCMD.FRE` enabled before reset so the port can receive the device's response. The driver must also wait for the task file to report reset completion before reading the signature; earlier reads return all ones.

## Defects found during implementation

Measurements or tests found three defects that code inspection had missed.

Every hardware wait in the kernel was two to seven times too short. `init_pit` writes control byte 0x36, selecting mode 3. In that mode, the 8254 counter advances by two for each input-clock count, so conversion at the input frequency produces half the intended delay. The window multiplication also overflowed 32 bits. A boot self-test measured 200 ms of `k_udelay` against the 100 Hz tick. It observed 30 ms before the correction and 200 ms afterward. The error did not affect QEMU tests, but every initialization delay for the target chip would have been short.

The RTL8168 bounded-wait helper accepted any bit in its mask, while the reference requires every bit. A chip with one FIFO still containing data could therefore be treated as drained and reset prematurely. An independent comparison with the reference found this defect.

The netconsole drain loop followed a moving endpoint. If every transmission reported an error through `printf`, the error text extended the same buffer being drained and the loop never ended. The host test for this case hung, which led to running every host test under a watchdog.

## Supported and excluded features

The table lists excluded devices explicitly so a reader can distinguish "not built" from "built and broken".

| Part | On the metal image | Evidence |
|------|--------------------|----------|
| Boot from a USB stick (CSM, El Torito hybrid ISO) | Yes | The laptop; not yet proved |
| VGA text console and the 8042 keyboard | Yes | The laptop; not yet proved |
| RTL8168GU Ethernet | Yes | The chip model in `verify-host`; not yet tested on the notebook |
| Netconsole over that card | Yes | `make netconsole-test` with RTL8139; the 8168 path is not yet tested |
| Boot-line IPv4, ARP, ICMP, UDP, TCP, sockets | Yes | The complete suite under `make test-addressed` |
| Dropbear over SSH, supervised by init | Yes | Unattended `make metal-test` |
| The console login as the recovery path | Yes | `make metal-test` and `make login-test` |
| AHCI identification and reads | Yes | `make test-disk-ahci`; reset and COMRESET are not yet tested on the laptop |
| Reading the disk without mounting it | Yes | Byte-level checks in `make diskdiag-test` |
| Writing any disk | No; the build rejects it | Byte-level checks in `make diskpolicy-test` |
| Restart | Yes | `make reset-test`, with each mechanism isolated |
| Power off | Yes, when a profile matches | `make shutdown-test`; the laptop chipset row is unproved |
| Trusted entropy for the host key | Required | `make metal-test`; requires RDRAND on the laptop |
| HD Audio | No; excluded from the image | No audio program is staged, although the kernel contains the driver |
| The compiler, `/src`, pdpmake, the installer | No; excluded from the image | A machine without writable storage cannot use them |
| Doom | No; excluded from the image | RAM is limited by the 32 MiB direct map |
| Writing the SSD, carving a partition, a persistent host key | No; deferred to phase E | Requires the owner's approval after a complete backup |
| DHCP, DNS, IPv6, a second interface | No | Out of scope; a numeric address is the contract |

## Directory iteration defect

The final self-build test failed consistently. The `sys_getdents64` directory cursor counted children already returned. `rm -rf` reads a batch of names, unlinks them, and resumes from that count. Each unlink renumbered the remaining children, so the next read skipped one entry for every removed file. The remaining entries made `rmdir` fail with only "Directory not empty" reported to the user.

The defect presumably already existed. This milestone added enough files to `rach2/sys` that its listing no longer fit in one call. Below that threshold, the caller reads every name before unlinking any entry, so the error remains hidden.

The cursor now records the byte offset of the last returned node. Deletion preserves that position because removing a node only enlarges free extents, and allocation splits a free node instead of shifting a reserved node. Growth beyond a file's extent can still relocate the node. If a child grows while another descriptor lists the directory, that child can be missed or repeated. POSIX leaves concurrent modification during directory iteration unspecified, and the deletion loop does not reach this case. The function documents this limit. `tests/unlinkdirprobe.c` reproduces the original failure with a deliberately small buffer and fails with the old cursor.

## Verification status

QEMU verification from a clean tree ran the complete probe suite twice: once with the compiled address and once with the boot-line address. It also covered netconsole capture, every subset of the optional device switches, the read-only kernel with a disk image, all three AHCI paths after controller reset, the SSH capstone, and the unattended metal test. The host ran 27 host tests, including the chip model, address parser, command allowlist, and netconsole.

None of this verification used the notebook. The remaining claims require that hardware.

Some acceptance steps require a person. The scripted part, `tools/metal_accept.sh`, runs over SSH from the development host. It reads the reported machine identity, requires the read-only kernel, verifies that the disk is not mounted, and compares the drive identity and first-sector hash with `baseline.md`. `make metal-test` rehearses this script before it runs on the target. The manual `checklist.md` covers disconnecting and reconnecting a cable, cold power cycles, a screen photograph, audible output, and booting Windows after removing the stick.

## What was refuted

* A phase B review reported that nothing called `netdev_poll`. The plan assigned the timer hook to phase C, and that phase added the call.
* One failure suggested that `install-test` had regressed, but a serial rerun passed. Its matrix boot has a 75-second time limit, and three QEMU tests had run concurrently. The script remains byte-identical to the milestone-23 version.
* A review requested a complete AHCI register model like the RTL8168 model. The important AHCI bounds are arithmetic. Testing found one real error: accepting an all-ones capacity truncated every later LBA. The driver now rejects it.
* The plan specified literal poweroff values. The driver now derives `0x1804` from the chipset base register. Only the sleep type remains a per-profile constant with recorded provenance because it resides in AML that this kernel cannot interpret.

## First measured LAN session

The notebook booted from the stick on a real network and accepted an SSH connection from the development host. The session established these results:

* The ported driver works. It reported the predicted `xid 0x509` and `Network... OK (RTL8168GU)`. ARP resolved, ICMP passed in both directions, and SSH used a TCP connection. The initialization sequence, four indirect windows, descriptor rings, and PHY all worked on the physical chip absent from the emulators.
* Init starts the machine unattended. Dropbear listened before the login prompt appeared, and root authenticated by key without console input.
* After adding the mobile device-id range, `lynxpoint-lp` matched and `poweroff` cut power instead of halting.
* Name resolution required `/etc/resolv.conf`, which the metal image had omitted. It specifies 1.1.1.1, the address supplied by this LAN's router. The router also forwards DNS, but a fixed public resolver remains usable when the stick moves.
* A 104 KB file transferred in each direction and matched byte-for-byte by checksum.

The plan requested two throughput measurements with different scopes:

| Measurement | Notebook, polled RTL8168GU | QEMU, interrupt-driven RTL8139 |
|---|---|---|
| Complete SSH session: connect, authenticate, run, close | 0.3 s | Comparable |
| One-way bulk transfer, 104 KB | 0.9 KB/s | 1.1 KB/s |

The interactive result is sufficient for remote development, but bulk throughput is poor. The polled real-hardware driver is within twenty percent of the interrupt-driven QEMU driver, so the similar rates point to an older TCP defect rather than the new driver or its 100 Hz poll. Milestone 25 phase A found and corrected that defect: the receiver did not notify its peer when the receive window reopened, so the peer's persist timer determined the rate. The same 64 KiB transfer then took 0.291 s instead of 60.179 s. See `dev-log/25-sealed/Readme.md`.

## Claims awaiting hardware

* The RTL8168GU driver has not yet run on the device. The model verifies the sequences and their order, but cannot verify PCIe ordering, DMA access to the rings, PHY behavior, or a possible requirement for the omitted firmware patch. The first proof will be a packet capture on the development host showing frames with the notebook's source address.
* AHCI reset and COMRESET have not yet run on a Lynx Point controller. The hardware must also show whether COMRESET recovers a link that fails after a warm reboot.
* The three restart mechanisms and poweroff remain unproved on this notebook. The ACPI path derives its address from PIIX4 configuration space and will reject this chipset, then halt. The plan specifies a PCH power-management path and ACPI-enable handshake that have not been built.
* The notebook memory map must show that the module layout fits below the direct-map limit.
* Phase E covers all SSD behavior beyond reads.

## Phase E, designed and deliberately not built

The local part of phase E was fully designed but deliberately not implemented. This section records the reasons.

The plan requires both the owner's explicit approval after a complete SSD backup and an MBR inventory. Neither prerequisite has been met, so the proposed partition layout does not exist. Implementation would add a `rach2.bootuuid=` selector with no selectable volume and a GRUB entry that refers to absent volumes. The working stick is required for the first notebook session, and such an entry could break it before that session.

One part of the design must also change before implementation. To boot the fixture, it exposed a writable `/dev/bootpart` window over a FAT volume selected by a boot-line token under the ordinary read-write kernel. Phase D removed this authority because a person can edit a boot flag at the GRUB prompt, and this volume would reside on the Windows disk. Any phase E write path must use the build switch instead of a token.

Phase E inherits several decisions. The inventory shows that MBR entries 3 and 4 are free, so partitioning can use primary entries without extended-partition support. `baseline.md` records the expected disk values. `/proc/disk` and `/dev/disk0` provide the measurements used to compare them.

## Baseline and retention

`baseline.md` records Windows measurements taken before Rach2 accessed the notebook: the drive model and serial, 1953525168 sectors, the disk signature, two partition entries, and the SHA-256 of the first sector. The tracked file preserves these values because their source, `Lenovo_G50-70_report_temp.txt`, is untracked and will be deleted after the hardware work. No other baseline exists for the SSD work.

## Temporary source record

`Lenovo_G50-70_report_temp.txt` remains untracked in the repository root. Delete it only after all hardware work in the milestone is complete. It has never been committed.
