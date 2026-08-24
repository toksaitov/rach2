# Milestone 25: the sealed disk

## The goal

Milestone 24 booted Rach2 on the Lenovo G50-70 from a USB stick and verified the machine over SSH. The evidence covered the RTL8168GU on a real LAN, an AHCI read whose MBR hash matched the Windows baseline byte for byte, poweroff, and a kernel unable to write the SSD. This milestone removes the stick. The 250 GB SSD becomes the only disk, written once through a USB adapter and installed until the hardware fails. All subsequent changes arrive over SSH.

The machine remains in the owner's home. A hang may require using the power button, but ordinary maintenance must not require opening the case. The plan provides remote update and recovery paths while omitting data-center mechanisms unnecessary for a machine in the next room. The final section lists each omission and its consequence.

## The two levels of disk authority

The milestone divides disk access into two levels. An early draft incorrectly restricted GRUB updates to recovery, although boot-loader updates are ordinary maintenance.

Level one exists in every image, including the everyday image. It covers the kernel slots, boot menu, grubenv, and GRUB's embedded core image through two bounded devices. `/dev/bootpart` already covers partition 1. The new `/dev/grubcore` covers the post-MBR area from LBA 1 through 2047. Its name describes the granted operation rather than the region. Neither device includes sector 0, which remains unchanged after the build.

Level two exists only in the recovery image. It permits access to any disk region at any offset, as required to "reflash the whole disk however we want". `/dev/disk0` provides this access.

A separate kernel build selects level two; no boot-line token can enable it. The repository already uses the compile-time `RACH2_DISK_READONLY` switch, separate object tree, and `tools/check_diskpolicy.py` artifact check because an editable GRUB command line must not override disk policy. A maintenance token would provide no separation. The everyday kernel compiles an unconditional `-EPERM` implementation for `/dev/disk0`. Only the recovery build includes whole-disk writes.

The recovery switch is a new macro with a separate component and object tree, not `RACH2_DISK_READONLY`. The existing macro is enforced below devfs: both drivers use a command filter that permits only IDENTIFY and READ. Reusing it would also prevent writes to the boot partition and post-MBR region. Both kernels therefore use the ordinary read-write command policy. The new macro controls only whether devfs exposes a writable whole-disk node. The recovery artifact has its own build directory and banner, and its checker refuses an everyday kernel presented as recovery.

Two qualifications define this boundary. First, "the everyday boot cannot write outside the boot partition and the gap" applies only to raw userland sector access. The mounted KRFFS root remains writable by design and changes during mount through the page cache. Second, the separation limits operational mistakes rather than a hostile root account. Level one already permits replacement of the next kernel, so root can arrange to execute arbitrary code later. The model constrains the running kernel but does not defend against hostile root.

## What the investigation settled before this plan

Six questions were settled by measurement or boot tests, and five results changed the design. A later adversarial review found eleven additional issues. Two required changes to the disk map and network defaults. Each phase records its corrections.

Sending a file to the machine is two hundred times slower than reading one from it. QEMU measurements over SSH gave 0.305 s (215 KB/s) for 64 KB out and 60.0 s (1.09 KB/s) for 64 KB in, with the change at `TCP_RING_SIZE` (4096, `rach2/sys/network/tcp/tcp.h:25`). `dev-log/24-metal/Readme.md:128` recorded the slow result as a general stack property. One defect explains it: `socket_tcp_recv` and `socket_tcp_try_recv` (`socket.c:500-513, 527-543`) advance `recv_head` without recomputing `tcb->rcv_wnd` or transmitting an update. The peer learns that the window reopened only through its persist timer. This is the missing receiver-window update from RFC 1122 4.2.3.3. Every remote transfer in this milestone enters the machine, so this correction comes first.

The tested GRUB layout replaces the core image without writing sector 0. Sector 0 contains the partition table at byte 446, and disk writes operate on complete sectors. Changing the pointer at `boot.img[0x5c]` would therefore rewrite the MBR even if all adjacent bytes were preserved. LBA 1 remains the permanent entry sector. Two core-image tails occupy LBA 2 through 1024 and LBA 1025 through 2047. LBA 1 contains GRUB's `diskboot` code and the blocklist at offset `0x1f4` that selects a tail. An update writes and verifies the inactive tail before rewriting LBA 1. A test copied the current tail to the second slot, changed only the blocklist, and reached a Rach2 login through GRUB 2.12.

The boot menu must use `source` instead of `configfile`, and the stub entries must precede the supplied entries. Twenty GRUB-only disk boots established both requirements. `configfile` creates another environment context, places the second file's entries in a new menu, and does not export `default`, `timeout`, or `fallback`; the resulting menu has no countdown and waits indefinitely. `source` merges the entries without displaying an intermediate menu. `fallback` accepts a numeric index into the merged definition order. If the stub entries come last, a later menu file changes the entry selected by `fallback=1`; boots with both orders produced logs that differed at this point. In addition, `default` is parsed as a number before matching an id, so every id must begin with a letter.

The boot partition is FAT32, not FAT16, and a 64 MB partition also requires a larger cluster size. The builder creates a `fat32` partition and invokes `mformat -F`. Kernel MBR validation accepts only types 0x0b and 0x0c, so a FAT16 type would suppress `/dev/bootpart`. At 64 MB with the default geometry, the volume has about 130500 clusters and a 1020-sector FAT. These exceed `BOOTSLOT_FAT_MAX_SECTORS` 512 and `BOOTSLOT_MAX_CLUSTERS` 65536 (`utils/bootslot.h:24-25`), causing `bootslot_open` to refuse the volume. `mformat -c 8` produces 4 KiB clusters, 16348 clusters, and a 128-sector FAT, all within the limits. `tools/mkdisk_grub.sh:215` and `tests/host/mkfatfixture.py:108` assume one sector per cluster, so the library's cluster arithmetic has not been exercised with another geometry.

Whole-disk reflash requires 64-bit offsets, which the kernel lacks. `struct open_file` stores a 32-bit offset, `_llseek` receives 64 bits from musl and truncates them into that field, and the diagnostic window is limited to 8388607 sectors. These limits cannot address a 250 GB SSD. Level two therefore requires a `uint64_t` offset for raw windows.

BusyBox already contains `udhcpc`, and its required kernel interface is also suitable for another DHCP client. The vendored 1.36.1 source includes `udhcpc`, but `CONFIG_UDHCPC` is unset. `ifconfig` and all `ip` applets are disabled, while `route` is enabled. The networking phase lists the missing interfaces.

## The name

The boot-partition tool becomes `bootctl`, replacing `install-kernel`. systemd's `bootctl` manages boot-partition kernels, entries, and defaults on most Linux systems, and its purpose matches this tool. Rach2 similarly uses the established names `ping`, `nc`, `login`, and `su`, giving students terminology that applies elsewhere. The BSD name `installboot` was rejected because it describes boot-loader installation, which this tool does not perform. The macOS name `bless` was rejected because it does not describe the operation. The verbs retain the trial and commit terms from milestone 23, as used by the kernel diagnostics, Readmes, and harness:

    bootctl status            what is installed, what is committed, what boots next
    bootctl kernel FILE       stage a kernel into the candidate slot
    bootctl config FILE       stage a boot menu into the configuration slot
    bootctl grub FILE         write a core image into the inactive tail slot and switch
    bootctl env KEY [VALUE]   read, or set, one allowlisted grubenv key
    bootctl trial ID          boot entry ID once, on the next boot only
    bootctl commit            make the booted trial the saved default
    bootctl cancel            clear an armed trial

## The final disk layout

    LBA 0                  MBR: GRUB boot.img in bytes 0-445, the table at 446
    LBA 1                  GRUB's entry sector: diskboot, and the blocklist
    LBA 2..1024            core image tail slot A, 1023 sectors
    LBA 1025..2047         core image tail slot B, 1023 sectors
    LBA 2048..133119       partition 1, 64 MiB FAT32 type 0x0c, 4 KiB clusters
    LBA 133120..1157119    partition 2, 500 MiB KRFFS, the root
    LBA 1157120..1288191   partition 3, 64 MiB FAT32, recovery
    LBA 1288192..1290239   1 MiB trailing margin
    the rest               unpartitioned, reserved

The image occupies 630 MiB, 1290240 sectors, or 660602880 bytes. Partition 4 is empty. Bytes beyond the image retain their previous contents.

Partition 1 contains the following files, preallocated during the build so later writes neither extend them nor relocate their cluster chains:

    /boot/candidate     2 MiB kernel slot, the one bootctl rewrites
    /boot/fallback      2 MiB kernel slot, written once at build, never again
    /boot/boot.cfg      the rewritable menu, padded to a multiple of the cluster
    /boot/grub/grub.cfg the immutable stub
    /boot/grub/grubenv  the 1024-byte state block

Partition 3 contains the recovery kernel and RAM root, both written once during the build. Review established that they cannot reside beside the everyday kernels on partition 1 because recovery on that partition cannot repair it. Damaged partition 1 metadata could make the menu and recovery kernel unreadable, leaving a recovery entry on the same partition inaccessible.

The core image therefore includes a configuration built with `grub-mkimage -c`. Boot tests determined its required form. The early parser does not implement the complete GRUB language: `if [ -f ... ]` produces `Unknown command 'if'` even when the core includes `test`, while `normal FILE` on a missing file opens the prompt without executing the next line. An unconditional ordered pair works:

    configfile (hd0,msdos1)/boot/grub/grub.cfg
    configfile (hd0,msdos3)/boot/grub/recovery.cfg

`configfile` returns when it cannot read the file. A readable partition 1 runs the stub without reaching the second command; a destroyed partition 1 continues to the recovery menu. Both cases booted successfully, including a disk with partition 1's boot sector and both FATs zeroed.

Boot tests found another recovery-file requirement. `configfile` does not change GRUB's variables, so a menu loaded from partition 3 retains `root=hd0,msdos1`; its `multiboot /boot/recovery` command would still read the damaged partition. The recovery file must set `root=(hd0,msdos3)` before the entry or qualify both paths with the device. After that correction, a Rach2 kernel and RAM module booted from partition 3 to a shell while partition 1 remained destroyed.

With the exact two-line configuration embedded, the core image measures 157713 bytes or 309 sectors. Its tail uses 308 sectors, leaving 715 of the 1023-sector slot available for later modules. The build enforces the 1024-sector limit directly instead of relying on the current margin.

The 500 MiB root fits within the kernel's 512 MiB page-cache window. `krffs/mkdisk.sh` still refuses sizes above 256 MiB, so the image-building phase corrects that stale limit.

Level-one device publication validates the MBR against the build-time geometry: the signature; exact types and bounds for entries 0, 1, and 2; an empty entry 3; and a device large enough for the layout. A damaged table publishes neither device, preventing `/dev/grubcore` from extending beyond its intended region or `/dev/bootpart` from overlapping the root.

## Phase A: the receiver window update

After `recv_head` advances in `socket_tcp_recv` or `socket_tcp_try_recv`, recompute `tcb->rcv_wnd` and send an update when the window has reopened by at least two maximum segment sizes or half the receive buffer, as required by the standard rule. The change affects only transmission and does not alter the state machine.

This work comes first because every remote path transfers a file into the machine. At the measured rate, a 700 KB kernel takes about eleven minutes to upload, while a 500 MB root is impractical. Phase H cannot rehearse the system until this defect is corrected.

* Proof: repeat the host-to-guest measurements at 4 KB, 64 KB, and 1 MB and record them beside the earlier results; require the existing TCP suites and SSH capstone to pass.
* Shown failing: remove the window-update call and require the 64 KB transfer to exceed 50 s; use a host test of the update predicate to show that a one-byte read sends no segment.
* Readmes: update `rach2/sys/network/Readme.md`; correct the claim at `dev-log/24-metal/Readme.md:128` in this milestone's record rather than modifying the historical file.

## Phase B: the tool's new name and shape

Rename the tool before adding capabilities so the twenty-one-file commit changes no behavior. `utils/install-kernel.c` becomes `utils/bootctl.c`, with a subcommand parser dispatching the existing operations. The extent report from `--where` moves into `status`. No compatibility alias is installed.

Three boundaries limit the rename. The `bootslot` library retains its name because it describes the managed object and because the host test matches two dozen error substrings literally. Refactoring the library during the rename would add unrelated risk. `dev-log/` remains unchanged because it records history rather than the current contract. `make install-test` retains its name because its loop still installs a kernel.

`status` is the only new behavior in this phase. It reports each slot's kernel and size, the values of `saved_entry` and `next_entry`, whether a trial is armed or consumed, the entry used for the current boot, and the slot extents. It omits the core image because the required device does not exist until the next phase. Phase E adds the core tail to `status` with the command that changes it.

Two hazards require explicit corrections because ordinary verification does not detect them. `tools/install_test.sh:256` parses `--where` with `awk '/candidate/{print $5}'` to locate the sector it later corrupts. Rewording the extent line moves the field and can silently corrupt another sector, so the `awk` change belongs in the rename commit. `prune-stale` scans only the top level of `build/utils` (`utils/Makefile:72-78`), allowing `build/utils/sbin/install-kernel` to survive until `make distclean`. Since `tools/check_setid.py` compares manifest strings without stat-ing the host path, an unchanged manifest could continue staging that stale binary without an error.

* Proof: rename `tests/host/installkernel_hosttest.c` to `bootctl_hosttest.c` and extend it for an unknown verb, missing argument, surplus argument, and every rejected old spelling. `tools/hosttest.sh` discovers the renamed test by glob, so registration does not change. Require `tools/install_test.sh` and `tools/selfbuild_test.sh` to pass end to end with the new tokens.
* Shown failing: point the harness at the old `install-kernel: committed` string and require failure; verify that the `awk` check returns no sector for a reworded extent line rather than an incorrect sector; use a clean-tree build to detect a stale `sbin/` binary staged by an un-updated manifest.
* Readmes: update `Readme.md`, `utils/Readme.md`, `tools/Readme.md`, `tests/host/Readme.md`, and `krffs/Readme.md`; update the three manifests, `check_setid.py`, `utils/guest.mk`, and `mkdisk_grub.sh`; require `check_guest.py` to keep the host and guest lists equal.

## Phase C: the two levels, as devices

This phase adds the kernel interfaces and the disk geometry they validate. Their order is coupled: a kernel expecting the sealed layout cannot operate with a builder still producing a 16 MiB boot partition with 512-byte clusters, while `bootslot_open` refuses a 64 MiB partition until the cluster size changes. The work remains in four separate commits because each step has a distinct risk and proof:

1. Geometry: add the three partitions, image size, `mformat -c 8`, host FAT fixture, correction to the builder's hard-coded 512-byte cluster assertion at line 215, and correction to `krffs/mkdisk.sh`'s stale 256 MiB refusal. Prove the step by booting the new image with the unchanged current kernel and confirming that it ignores partition 3.
2. Level-one publication: add the one-time probe, exact MBR validation, and publication of `/dev/bootpart` and `/dev/grubcore` during discovery rather than mount. Prove the step by requiring both devices in a RAM-root boot and testing every bounded-range refusal.
3. Wide offsets: add a 64-bit offset to the open-file description and update both seek syscalls. This step can affect unrelated code because the existing 32-bit offset in `struct open_file` is shared by regular files, procfs, terminals, all devices, and the raw windows. Run the existing file, procfs, terminal, and device tests before adding write authority.
4. Recovery authority and throughput: add the macro, component, artifact check, writable whole-disk node, per-write privilege check, and 32 KiB batching. Prove the step directly rather than waiting for phase G: write a bounded region through `/dev/disk0` with the recovery build and read it back; require the artifact checker to accept recovery and refuse everyday kernels; instrument a large request and require 64-sector commands at each driver instead of one-sector commands.

Currently `/dev/bootpart` appears only after a successful disk mount, and `/dev/disk0` appears only with `rach2.diskdiag`; a RAM-root recovery boot therefore has neither. Refactor boot order so `block_probe` runs once, sector 0 is read and checked once against the compiled geometry, the level-one devices are published, and root selection follows.

Add `/dev/grubcore` as minor 3 with mode 0600, covering only LBA 1 through 2047. In recovery, `/dev/disk0` covers the complete device; other builds omit it or expose it read-only for diagnostics. Before copying or invoking a driver, each raw handler verifies sector-aligned offsets and counts and checks the range with subtraction that cannot wrap. The write path also checks the effective uid at the time of each write. Since uid 0 bypasses the generic mode check, a process could otherwise open the device while privileged, drop privilege, and continue writing.

Two supporting changes complete the phase. Store a 64-bit raw-window offset in the open-file description so `/dev/disk0` can address a 250 GB drive, while legacy `lseek` refuses results it cannot represent. Batch raw transfers through one 32 KB static buffer in `.bss`, below the 32 MB identity-mapped limit so its virtual address is the physical address required by the AHCI PRDT. Each 64-sector call becomes one command for either driver. A 500 MB reflash then falls from about a million driver calls and cache flushes to sixteen thousand. Syscalls are not preemptible, so a second writer cannot use the buffer concurrently; document that constraint.

* Proof: host-test the bounds arithmetic with offsets and counts that are not sector multiples, a range ending exactly at the limit, a range extending one sector past it, and a count that would wrap. Boot QEMU with a RAM root that mounts no disk and require both level-one devices.
* Shown failing: move MBR entry 0 by one sector and require publication of neither device; run `printf x > /dev/disk0` on an everyday kernel and byte-compare the disk image afterward, following `tools/diskpolicy_test.sh:74-110`; open a descriptor as root, drop privilege, attempt a write, and require refusal.
* Readmes: update the filesystem Readme's device table and add a section defining the two authority levels and their exact ranges.

## Phase D: the rewritable boot menu

Convert `/boot/grub/grub.cfg` into a short immutable stub. It loads grubenv, defines all required entries, tests for `/boot/boot.cfg`, and then `source`s that file. `source` is the final statement before `set default`, `set fallback`, and `set timeout`. Preallocate the rewritable `/boot/boot.cfg` to a fixed size divisible by the cluster size.

The order preserves the stub entries at the lowest indices because `source` appends the second file's entries. `set fallback=1` therefore keeps its intended entry, and a repeated id from the second file does not replace the stub's id. If `boot.cfg` is absent, truncated during a write, or invalid, the stub entries remain defined and can boot the machine.

Fix the entry list here because later code depends on these indices:

    0  candidate        the staged kernel
    1  fallback         the build's kernel
    2  candidate-static the staged kernel, static address
    3  fallback-static  the build's kernel, static address
    4  debug            the rescue shell
    5  recovery         the recovery kernel and its RAM root

Every entry includes `rach2.disk=ahci`, every id begins with a letter, and the `bootctl trial` allowlist adds `recovery`. Only index 0 may include `rach2.slot=candidate`. A consumed `next_entry` line together with that token is the only evidence that the candidate booted. Reusing the token would allow commit of a kernel that had not run.

Entry 5 refers to artifacts that phase G has not yet built. Define it now to keep later indices stable, but mark it unbootable and exclude it from this phase's verification. Phase G stages the artifacts and verifies the entry.

Currently `bootslot_open` resolves three padded 8.3 names and does not resolve `grub.cfg` as writable, leaving no writable menu chain. Add `boot.cfg` as the fourth resolved file. Build-time preallocation makes its cluster chain fixed. Phase C has already changed the geometry to 4 KiB clusters, so this phase exercises the library's cluster arithmetic with that geometry for the first time.

* Proof, in QEMU: stage a menu and boot an entry defined only there; require the slot length to remain unchanged; round-trip grubenv through `bootctl env`; rebuild the FAT fixture with 4 KiB clusters and run the library tests on the new geometry.
* Shown failing: leave the first `boot.cfg` entry incomplete, require GRUB's parse error in the log, and boot the stub's `fallback`; place the stub entries last and show `fallback=1` selecting another entry; refuse a menu one byte larger than the slot; refuse a grubenv key outside the allowlist.
* Readmes: update the boot-partition contracts in `krffs/Readme.md` and `tools/Readme.md`.

## Phase E: GRUB updates from any image

`bootctl grub FILE` accepts a complete core image from 2 through 1024 sectors and refuses 1025. A tail contains 1023 sectors. Since the image's first sector is the entry sector rather than part of the tail, a 1024-sector image fills the tail with its remaining 1023 sectors. Patch the first sector's blocklist at `0x1f4` with the inactive tail's first LBA and the count at `0x1fc` with the image's sector count minus one. Write the remaining sectors through `/dev/grubcore`, zero-fill unused tail sectors, read and compare the complete tail, and only then write the patched first sector to LBA 1. Any earlier failure retains the old entry sector and selected tail.

One risk remains. The old core image stays selected until the final sector write, so earlier failures retain a bootable loader. A torn or failed write to LBA 1 makes GRUB unreachable, and recovery cannot run because GRUB loads it. The procedure is not safe against power loss and does not claim to be. The owner accepts this because everyday GRUB updates are required, the exposure is one sector written after verification, and only firmware network boot or removable media could remove it.

* Proof, in QEMU: build two core images with different early configurations that print the running tail; install B, reboot, and observe B; install A in the newly inactive slot, reboot, and observe A. Use output from the running core because reading the pointer proves only stored bytes.
* Shown failing: refuse a 1025-sector image before writing; model a device that reports but drops a write, then require a readback mismatch, unchanged LBA 1, and a subsequent boot from the old tail.
* Readmes: add a GRUB section to `tools/Readme.md` defining the slot geometry for the build and runtime, including the embedded configuration and recovery fallback.

## Phase F: networking, one interface ABI and two clients

The owner requires os-real to use BusyBox while the custom image configures networking without it. If os-real runs `udhcpc`, the kernel must provide its Linux-compatible interface. A kernel DHCP client would then duplicate behavior already available through that interface. The kernel therefore provides one interface used by two userland clients. Milestone 19 used the same division for BusyBox account tools in os-real and in-house tools in os-simple.

The client's source defines the required kernel interface. A privileged `AF_PACKET/SOCK_DGRAM` socket binds by interface index. Transmission accepts an L3 packet and destination MAC. Reception copies IPv4 payloads from `ethernet_rx` before `ip_rx` checks the configured address because DHCP replies arrive before the machine owns the offered address. Add `recvmsg` as socketcall 17. Provide an `AF_INET/SOCK_RAW/IPPROTO_RAW` descriptor for interface queries only. When `sys_ioctl` receives a socket descriptor, dispatch `SIOCGIFINDEX`, `SIOCGIFHWADDR`, `SIOCGIFADDR`, `SIOCGIFFLAGS`, `SIOCSIFFLAGS`, `SIOCSIFADDR`, `SIOCSIFNETMASK`, `SIOCSIFBRDADDR`, `SIOCADDRT`, and `SIOCDELRT`; restrict setters to effective uid 0. Store a peer for connected UDP so renewal can use `connect` and `write`, both currently refused. Implement `SO_BROADCAST`, `SO_BINDTODEVICE`, and `SO_REUSEADDR` instead of accepting them as no-ops. Return `ENOPROTOOPT` for `PACKET_AUXDATA`; the client tolerates that result and verifies the checksum itself, while the current blanket success incorrectly provides no auxiliary data. ARP conflict detection remains disabled, so `SOCK_PACKET` is unnecessary.

An address change must clear ARP state and abort TCP connections that recorded the old local address. A TCB copies the address when opened and would otherwise retain inconsistent state. This requirement belongs in the plan rather than only in review notes.

Move configuration into userland using the existing file format. os-real stages `/etc/network/interfaces` with `auto eth0` and either `iface eth0 inet dhcp` or an `inet static` stanza. Init invokes `ifup -a`.

The existing configuration cannot compile `udhcpc`. `networking/udhcp/dhcpc.c` unconditionally includes `<linux/filter.h>` and `<linux/if_packet.h>`, neither of which exists in the ported musl include tree. Enabling the applet stops at the first missing header. `vendor/busybox/Readme.md` records this as the reason the applet was disabled, so resolving it is the phase's first task.

Use a pinned BusyBox patch because the attempted header-copy alternative fails. Copying the two Linux user ABI headers into musl immediately fails: `filter.h` includes `linux/compiler.h`, and together they require `linux/types.h`, `linux/bpf_common.h`, and `asm/byteorder.h`. Musl's `<netpacket/packet.h>` provides `sockaddr_ll` and `PACKET_AUXDATA`, but `dhcpc.c` also uses `struct tpacket_auxdata` and `TP_STATUS_CSUMNOTREADY` outside disabled code.

The working patch removes the `linux/filter.h` include because every use is inside an `#if 0` block, substitutes `<netpacket/packet.h>`, and defines the auxiliary-data structure and status bit behind a compatibility guard. With ARP probing and RFC 3397 disabled, the object list is `common.o packet.o signalpipe.o socket.o dhcpc.o`. A complete build compiles and links without another dependency. Prove this with a full BusyBox build rather than `oldconfig`.

Add the patch set to the build prerequisites. This is BusyBox's first patch, exposing an existing omission: `build_busybox.sh` applies files under `vendor/busybox/patches/` after extraction, but `BUSYBOX_STAMP` depends only on the build script, two configs, wrapper, tarball, and libc stamp. Editing a patch would otherwise leave the stamp current and continue shipping an older binary. Musl already uses the wildcard prerequisite `LIBC_PATCHES` for this reason. Apply the same rule to BusyBox and update `vendor/busybox/Readme.md` because the vendored tree is no longer pristine.

Correct the BusyBox configuration from the first draft. `ifup` and `CONFIG_FEATURE_IFUPDOWN_IP` are enabled, selecting the `ip` backend instead of `ifconfig` and `route`. Since all `ip` applets are disabled, `ifup` would invoke a missing command; enabling only `ifconfig` would not change its backend. Disable `CONFIG_FEATURE_IFUPDOWN_IP` and enable `CONFIG_IFCONFIG`, `CONFIG_UDHCPC`, and the lease script. `ifup` invokes the `if-pre-up.d` and `if-up.d` directories through `run-parts` and treats failure as a failed method. The manifests create none of them, so stage all four hook directories. Prove the configuration by running `ifup -a` in the resulting filesystem, not merely by compiling BusyBox.

The custom image reads the same file through in-house tools. Its DHCP client must build and validate IPv4 and UDP frames and checksums; perform discover, request, and acknowledgement with bounded retries; parse and apply the subnet mask, router, DNS servers, lease time, and server identifier; handle NAK; implement T1 and T2 renewal and rebinding; and remove configuration at lease expiry. Both clients renew so the images have the same lifecycle. The client atomically replaces `/etc/resolv.conf` from the lease. An earlier draft incorrectly treated existing reads by `utils/resolv.c` as sufficient, although the current images hard-code the QEMU resolver and no code writes this file. Add the new programs to `utils/Makefile` and `utils/guest.mk`, whose equality is checked by `tools/check_guest.py`.

Retain the kernel's default address, correcting an earlier draft that would have broken existing verification. A boot without an address option still receives the SLIRP tuple `10.0.2.15/24`. `make test`, `make ping-test`, and the self-build test use no append string, while `tests/loopprobe.c` requires that exact address. Those harnesses have neither init nor a userland configurator and would fail if the machine began unconfigured. Add `rach2.ip=none` to mean that the NIC is probed but IPv4 remains unconfigured for userland. Only the sealed image uses this token, leaving existing suites unchanged.

Keep `rach2.ip=A.B.C.D/prefix` as the explicit early-boot override for tests and rescue. Netconsole transmits broadcast Ethernet frames below IP and needs no address, so it can report a machine whose address is unknown. Init starts console login before `ifup` and Dropbear, preserving local access after network failure.

* Proof, in QEMU: inspect libslirp source to establish that SLIRP broadcasts offers and acknowledgements and skips a zero UDP checksum. Boot with a `dhcp` stanza and explicit `net=192.168.90.0/24,dhcpstart=192.168.90.20`, obtain a lease, and run the network suite over the assigned address. Test a `static` stanza in the same run. Repeat both cases with `udhcpc` on os-real and the in-house client on the custom image.
* Shown failing: attach a QEMU `hubport` to an unused hub to provide a network without a server or host sockets; require the client to stop within its bound, preserve login, and report the failure in the boot log. Host-test malformed offers with a wrong transaction id, truncated option, and network or broadcast address. Change the address while a TCP connection is open and require that connection to abort.
* Proof, at the laptop: boot once at home and once at the office, obtaining an address without editing the boot line, as originally requested.
* Readmes: update `rach2/sys/network/Readme.md`, the written-once contract in `netconf.h`, the claim in `tools/mkiso.sh` that no DHCP client exists, the disabled-applet list in `vendor/busybox/Readme.md`, and the root `Readme.md` claims that BusyBox is unmodified and only three vendored trees carry patches.

## Phase G: recovery and whole-disk reflash

Build recovery as a separate kernel with the recovery switch and a RAM root loaded as a multiboot module. Both reside on partition 3. Stub entry 5 on partition 1 starts them during ordinary operation; the core image's embedded configuration starts them when partition 1 is unreadable. Recovery mounts no file system from the target disk, allowing it to rewrite a damaged root. The RAM image supplies its userland, SSH server, and tools.

`reflash DEVICE OFFSET BYTES` requires the length as an argument, correcting an impossible promise in the earlier draft. A pipe's total length is unknown until it ends, so a streaming form could detect overflow only after writing part of the input. With a declared count, the tool seeks to the device end and validates the offset and count by subtraction before reading input. Parse all three values as 64-bit numbers.

The device contract adds two requirements. Reads from an ssh pipe may return any positive byte count, while raw devices accept only complete sectors, so the tool combines reads into sector-sized writes. It also requires the stream to end at the declared count and refuses a short stream or trailing partial sector.

The tool reports its written byte count and refuses ranges beyond the device. `ssh` provides transport without a separate protocol, while BusyBox supplies `dd`, `sha256sum`, `cmp`, `sync`, and `gunzip`. Recovery also includes `bootctl`, allowing one session to repair the root, kernel slot, menu, boot file system, or inactive core slot. The risk section lists the unavailable repairs: LBA 0, LBA 1, and an active core image unable to read partition 3.

Correct a related comment in `devfs.c`. It claims mode 0400 refuses `open(O_WRONLY)`, but the credential check immediately accepts uid zero. The mode restricts non-root callers only.

* Proof, in QEMU: stop the machine and zero the KRFFS magic at the beginning of the root partition. Boot the ordinary entry and require mount failure without SSH. Select recovery through a host-written `next_entry`, stream the compressed root over ssh into `/dev/disk0` at the root offset, require the exact byte count, read the region through the same device, compare hashes, reboot, and require a clean mount and login. Use `gzip -c IMAGE | ssh HOST 'gunzip | reflash /dev/disk0 OFFSET 524288000'`, with the host providing the known uncompressed length.
* Proof of the embedded fallback, using the produced image rather than a scratch disk: with partition 1 readable, require the partition 1 stub; after zeroing partition 1's boot sector and both FATs, require the recovery kernel and module from partition 3. This pair detects a missing `recovery.cfg`, wrong path, or omitted `set root=(hd0,msdos3)` that other milestone tests would miss.
* Shown failing: refuse `reflash` and shell redirection on an everyday kernel, then byte-compare the image to prove it is unchanged; refuse a range beyond the device before reading input; reject an everyday artifact presented as recovery.
* Readmes: add a recovery section to `tools/Readme.md` that defines the build, devices, and exact `ssh` commands; correct the comment in `devfs.c`.

## Phase H: the sealed image, rehearsed end to end

Add `tools/sealed_test.sh` behind `make sealed-test` instead of extending `install_test.sh`. The existing harness verifies installer behavior that must remain there: refusal to commit an armed trial, the marker inside the trial boot's byte interval, cold boot of the committed candidate, damaged and repaired intervals, a second pass over the modified disk, and the three grubenv states. The new rehearsal verifies operation of the completed production layout over SSH.

The harness makes one byte-for-byte copy of the flash artifact, verifies its hash, and modifies only that copy for the complete run. It retains raw format so host offsets correspond to SSD bytes. Run one QEMU instance at a time and keep one serial log per cold boot. Guest reboots remain within one process and are separated by ordered byte offsets; host fault injection stops the machine before offline edits.

Separate fault and control runs to correct an earlier contradiction. The fault run performs a cold boot and SSH login; stages, trials, reboots, and commits a kernel; catches and repairs a bad kernel through fallback; stages a menu and then verifies the stub after truncation; switches the core image to the other tail and back; obtains a lease; damages and reflashes the root from recovery; and performs a final cold boot into a good state. It requires fallback and recovery tokens at their expected points. The control run uses a fresh copy, injects no faults, avoids deliberate recovery, and requires no fallback or recovery token. The earlier draft entered recovery in the control while also requiring the recovery token to be absent.

The time budget is twelve to fifteen minutes and remains unverified until the harness prints each phase's duration. If it exceeds the budget, remove work already covered by another harness. Do not shrink the root, change from raw format, skip readback, or begin later phases from fresh images because those changes would stop testing the production artifact and accumulated state.

After rehearsal, write a host procedure beside `dev-log/24-metal/checklist.md`. It identifies the drive by model and serial before any write because the milestone 24 baseline describes the laptop's original 1 TB drive rather than the new SSD. The procedure boots the image once more in QEMU, unmounts the device, writes 660602880 bytes (630 MiB, 1290240 sectors), reads the same amount from the device, compares hashes, and records the disk contents from that day.

* Proof: require the rehearsal to pass from a clean tree and record the image hash beside its manifest.
* Shown failing: pair each recovery step with an injected fault at an exact offset; use the control run to show that the recovery observations are absent without those faults.
* Readmes: update the disk section in `tools/Readme.md`; file the physical-write procedure with the milestone record.

## Phase I: the sweep, the record, and verification

From a clean tree, run `make verify-style`, `make verify-host`, the QEMU suites, SSH capstone, `install-test`, `selfbuild-test`, and `sealed-test` as separate claims. Obtain an independent adversarial review of the complete diff, evaluate every finding with evidence, and fix each confirmed issue. In `dev-log/25-sealed/Readme.md`, record the implementation, refuted claims, deferred work and reasons, and measurements from the first sealed laptop boot.

## Risks, in expected order

Remote recovery depends on GRUB, so it cannot repair LBA 0, LBA 1, or an active core image too damaged to read partition 3. Phase E limits the core update to one final sector after reading and comparing the replacement. The embedded configuration permits destruction of partition 1 without losing recovery, but the entry sector remains a single point of failure.

The recovery write path has the highest consequence. If an everyday kernel can write `/dev/disk0`, a root mistake can destroy the complete disk. Compile the distinction instead of parsing a boot-line token. Two tests require refusal by the everyday kernel, including through shell redirection, and phase I reviews this boundary explicitly.

Networking contains the largest change: a new address family, a new socketcall, ten ioctls, and connected UDP. It modifies the socket layer used by SSH and therefore has the greatest regression risk. Run the existing suites and SSH capstone unchanged throughout, and verify the static and leased configurations in the same run.

DHCP moves address configuration to userland, leaving the machine unconfigured until init runs. Start console login before `ifup`, retain netconsole operation without an address, and keep `rach2.ip=` for boots that require network access before init.

An interrupted menu-slot or grubenv write must be expected. The stub entries protect the menu case; grubenv's fixed block and allowlist constrain the state case. Each requires a failing test.

After sealing, remote repair is unavailable when the machine cannot boot. Therefore every remote path must pass in QEMU against a byte-identical image before writing the SSD.

## What this milestone deliberately leaves out

The milestone omits a second root partition. It repairs a bad root from the tested recovery image, which requires rebooting into recovery instead of another root.

The milestone omits a separate configuration partition. Configuration remains in the root and is lost during a root reflash, so the procedure requires a separate copy.

The milestone omits a watchdog. A hang therefore requires using the physical power button.

The milestone omits network boot and removable recovery media. Damage to LBA 0, LBA 1, or the active core image therefore requires opening the case. The risk section identifies these failures as having no remote repair path.

The milestone omits redundancy below GRUB. The entry sector remains a single point of failure.

## What must not be broken

Sector 0 must remain unchanged after the build, and `/dev/grubcore` must not reach it.

The everyday and recovery kernels must retain their compiled distinction. It is not a boot-line token and must never become one.

Preserve milestone 23's trial and commit state machine: fallback by entry number; menu ids; `rach2.slot=` tokens, including the substring rewritten by the harness with `sed`; byte patterns scanned by `bootslot.c`; the 1024-byte grubenv block; and exit status 1 for every refusal.

Exactly one menu entry must include `rach2.slot=candidate`. This makes commit evidence that the kernel booted, and the expanded menu must retain the invariant.

Preserve every `bootslot.c` error substring matched literally by the host test. Do not rename or refactor the library in the same change.

Keep the phase H assertions already owned by `tools/install_test.sh` in that harness rather than moving them to the new one.

Preserve the static `rach2.ip=` form and QEMU's SLIRP tuple byte for byte so existing network suites continue to pass after adding DHCP.

Preserve equality between the guest build and host lists, as checked by `tools/check_guest.py`.
