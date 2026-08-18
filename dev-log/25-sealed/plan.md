# Milestone 25: the sealed disk

## The goal

Milestone 24 put Rach2 on the Lenovo G50-70 from a USB stick and proved the machine over SSH: the RTL8168GU on a real LAN, an AHCI disk read whose MBR hash matched Windows' own baseline byte for byte, poweroff, and a kernel that could not write the SSD if it tried. This milestone ends the stick. The 250 GB SSD becomes the machine's only disk, written once over a USB adapter, screwed in once, and not removed again unless the hardware fails. Everything that changes afterwards arrives over SSH.

The machine sits in the same home as its owner. A hung machine costs a walk to the power button, which is acceptable; opening the case to reach the disk is not. This plan builds the paths that avoid reopening the machine, and it leaves out the machinery a data centre would need to survive failures a person in the next room can walk over and fix. Those omissions are listed at the end with their consequences.

## The two levels of disk authority

This is the milestone's organising decision, and an early draft got it wrong by putting GRUB updates out of reach of the everyday machine. Updating a boot loader is ordinary work. The split is:

Level one, on every image including the everyday one: the kernel slots, the boot menu, grubenv, and GRUB's embedded core image. Two bounded devices carry it. `/dev/bootpart` already exists and covers partition 1. `/dev/grubcore` is new, covers the post-MBR area at LBA 1 through 2047, and is named for the authority it grants rather than for the geography it spans. Sector 0 is not in either device and is never written after build.

Level two, on the recovery image only: any region of the disk, at any offset, which is what "reflash the whole disk however we want" requires. `/dev/disk0` carries it.

Level two is selected by a separate kernel build, not by a token on the boot line. This repository already settled that question once: `RACH2_DISK_READONLY` is a compile-time switch with its own object tree and an artifact check (`tools/check_diskpolicy.py`) precisely because a GRUB command line can be edited and must not be able to undo a disk policy. A maintenance token on the boot line is editable in exactly that way, so it would not have been a boundary at all. The everyday kernel compiles the unconditional `-EPERM` body for `/dev/disk0`, and only a kernel built with the recovery switch compiles a whole-disk write at all.

The recovery switch is a new macro with its own component and object tree, and it is not `RACH2_DISK_READONLY`. That macro cannot be reused, because it is enforced far below devfs: the command filter permits only IDENTIFY and READ, and both drivers pass every command through it, so a kernel carrying it could not write the boot partition or the gap either. Both the everyday and the recovery kernel therefore use the ordinary read-write command policy, and the new macro controls one thing only, whether devfs exposes a writable whole-disk node. The recovery artifact gets its own build directory and its own banner, and the artifact check refuses an everyday kernel offered as recovery.

Two honest qualifications belong here rather than in a footnote. First, "the everyday boot cannot write outside the boot partition and the gap" is a claim about raw sector access from userland. The mounted KRFFS root is writable by design and is written during mount itself, through the page cache. That is the enforceable claim and the plan states it that way. Second, this is an operational separation, not a defence against a hostile root account: level one can already replace the kernel that boots next, so anyone who is root can arrange to run their own code later. The model limits mistakes and limits the running kernel; it does not survive a hostile root.

## What the investigation settled before this plan

Six things were measured or booted rather than assumed, and five of them changed the design. An adversarial review of the finished plan then found eleven more, two of them serious enough to change the disk map and the network defaults; those corrections are recorded in the phases they belong to.

Sending a file to the machine is two hundred times slower than reading one from it. Measured under QEMU over SSH: 64 KB out takes 0.305 s (215 KB/s), 64 KB in takes 60.0 s (1.09 KB/s), with the knee at `TCP_RING_SIZE` (4096, `rach2/sys/network/tcp/tcp.h:25`). `dev-log/24-metal/Readme.md:128` recorded the slow figure and called it a property of the stack. It is one defect: `socket_tcp_recv` and `socket_tcp_try_recv` (`socket.c:500-513, 527-543`) advance `recv_head` without recomputing `tcb->rcv_wnd` or sending anything, so the peer learns the window reopened only from its own persist timer. That is the missing receiver window update of RFC 1122 4.2.3.3. Every path in this milestone is an inbound transfer, so it goes first.

The GRUB core image can be replaced without ever writing sector 0, and the scheme was booted before it was written down. Sector 0 holds the partition table at byte 446, and a disk writes whole sectors, so switching the pointer at `boot.img[0x5c]` would be an MBR write no matter how carefully the other bytes were preserved. Instead LBA 1 stays the entry sector for the life of the disk, and the two core images live in tail slots at LBA 2 through 1024 and LBA 1025 through 2047. LBA 1 holds GRUB's `diskboot` code and, at offset `0x1f4`, the blocklist naming which tail to load. An update writes the inactive tail, reads it back, compares, and only then rewrites LBA 1. A test that copied the current tail to the second slot and changed only that blocklist reached a Rach2 login through GRUB 2.12.

The boot menu must use `source`, not `configfile`, and the stub's own entries must come first. Twenty GRUB-only disks were built and booted to settle both. `configfile` opens a new environment context, so the second file's entries land in a fresh menu displayed on the spot, and `default`, `timeout` and `fallback` are not exported into it: the observed result is a second menu with no countdown that waits forever. `source` merges entries into one list and displays nothing early. Separately, `fallback` accepts only a number, and that number indexes the merged list in definition order, so with the stub's entries last a menu file supplied later silently moves what `fallback=1` selects; both orders were built and booted and the logs differ exactly there. One more trap: `default` is parsed as a number before it is matched as an id, so ids must start with a letter.

The boot partition is FAT32, not FAT16, and a 64 MB partition breaks the tool that manages it unless the cluster size changes too. The builder already creates a `fat32` partition and runs `mformat -F`, and the kernel's MBR gate accepts only types 0x0b and 0x0c, so calling it FAT16 would suppress `/dev/bootpart` entirely. At 64 MB with the default geometry there are about 130500 clusters and a 1020-sector FAT, against `BOOTSLOT_FAT_MAX_SECTORS` 512 and `BOOTSLOT_MAX_CLUSTERS` 65536 (`utils/bootslot.h:24-25`), so `bootslot_open` refuses the volume. `mformat -c 8` gives 4 KiB clusters, 16348 clusters and a 128-sector FAT, inside every bound. Two places assume one sector per cluster: `tools/mkdisk_grub.sh:215` and `tests/host/mkfatfixture.py:108`, which means the library's cluster arithmetic has never been exercised at any other geometry.

Whole-disk reflash needs 64-bit offsets, which the kernel does not have. `struct open_file` stores a 32-bit offset, `_llseek` receives 64 bits from musl and truncates them into that field, and the diagnostic window is clamped to 8388607 sectors. None of that can address a 250 GB SSD, so level two does not meet its requirement without a `uint64_t` offset for the raw windows.

busybox already contains `udhcpc`, and the kernel surface it needs is the same surface any DHCP client needs. `udhcpc` is in the vendored 1.36.1 source but `CONFIG_UDHCPC` is not set, and `ifconfig` and every `ip` applet are disabled while `route` is enabled. What it requires is enumerated in the networking phase below.

## The name

The boot-partition tool becomes `bootctl`, replacing `install-kernel`. systemd's `bootctl` manages a boot partition's kernels, entries and defaults on most Linux machines today, and its verbs line up with what this tool already does. Rach2 ships `ping`, `nc`, `login` and `su` under their real names for the same reason, so a student who learns this one carries the knowledge off the machine. The BSD family's `installboot` was rejected because it names installing the boot loader, which the tool never does. macOS `bless` was rejected as opaque. The verbs keep Rach2's trial and commit vocabulary from milestone 23, because the kernel diagnostics, the Readmes and the harness all speak it already:

    bootctl status            what is installed, what is committed, what boots next
    bootctl kernel FILE       stage a kernel into the candidate slot
    bootctl config FILE       stage a boot menu into the configuration slot
    bootctl grub FILE         write a core image into the inactive tail slot and switch
    bootctl env KEY [VALUE]   read, or set, one allowlisted grubenv key
    bootctl trial ID          boot entry ID once, on the next boot only
    bootctl commit            make the booted trial the saved default
    bootctl cancel            clear an armed trial

## The disk, once and for all

    LBA 0                  MBR: GRUB boot.img in bytes 0-445, the table at 446
    LBA 1                  GRUB's entry sector: diskboot, and the blocklist
    LBA 2..1024            core image tail slot A, 1023 sectors
    LBA 1025..2047         core image tail slot B, 1023 sectors
    LBA 2048..133119       partition 1, 64 MiB FAT32 type 0x0c, 4 KiB clusters
    LBA 133120..1157119    partition 2, 500 MiB KRFFS, the root
    LBA 1157120..1288191   partition 3, 64 MiB FAT32, recovery
    LBA 1288192..1290239   1 MiB trailing margin
    the rest               unpartitioned, reserved

The image is 630 MiB, 1290240 sectors, 660602880 bytes. Partition 4 is empty. Everything past the image keeps whatever the drive held before.

Partition 1 holds, all preallocated at build so no later write extends a file or moves a cluster chain:

    /boot/candidate     2 MiB kernel slot, the one bootctl rewrites
    /boot/fallback      2 MiB kernel slot, written once at build, never again
    /boot/boot.cfg      the rewritable menu, padded to a multiple of the cluster
    /boot/grub/grub.cfg the immutable stub
    /boot/grub/grubenv  the 1024-byte state block

Partition 3 holds the recovery kernel and its RAM root, both written once at build. They live there rather than beside the everyday kernels for a reason found in review: recovery on partition 1 cannot repair partition 1. If that filesystem's metadata is damaged, GRUB cannot read the menu or the recovery kernel, and a recovery entry defined in a menu on the same partition is unreachable exactly when it is needed.

So the core image carries an embedded configuration of its own, built into it by `grub-mkimage -c`. Its exact shape matters and was settled by booting, because the obvious forms do not work. The early parser that runs an embedded file is not the full GRUB language: `if [ -f ... ]` fails with `Unknown command 'if'` even when `test` is built into the core, and `normal FILE` on a missing file drops to the prompt and never reaches the next line. What does work is an ordered pair with no conditional at all:

    configfile (hd0,msdos1)/boot/grub/grub.cfg
    configfile (hd0,msdos3)/boot/grub/recovery.cfg

`configfile` returns when its file cannot be read, so a good partition 1 runs the stub and never reaches the second line, and a destroyed partition 1 falls through to the recovery menu. Both cases were built and booted, the second after zeroing partition 1's boot sector and both FATs.

The recovery file has a second requirement found the same way. `configfile` does not retarget GRUB's variables, so a menu loaded from partition 3 still has `root=hd0,msdos1` and its `multiboot /boot/recovery` would read the destroyed partition. The recovery file therefore sets `root=(hd0,msdos3)` before its entry, or device-qualifies both paths. With that correction and partition 1 destroyed, a real Rach2 kernel and its RAM module booted from partition 3 to a shell.

The core image measures 157713 bytes with that exact two-line configuration embedded, 309 sectors, of which 308 live in a tail slot, leaving 715 of the slot's 1023 sectors free for later modules. The build asserts the 1024-sector bound rather than relying on that margin.

The 500 MiB root sits below the kernel's 512 MiB page-cache window, but `krffs/mkdisk.sh` still refuses anything over 256 MiB, and that stale bound is corrected in the phase that builds the image.

Publication of the level-one devices is checked against the geometry compiled into the build, not against whatever the MBR happens to say: the signature, the exact type and bounds of entries 0, 1 and 2, entry 3 empty, and a device at least as large as the layout. A damaged partition table then publishes nothing rather than publishing a `/dev/grubcore` that reaches further than it should or a `/dev/bootpart` sitting over the root.

## Phase A: the receiver window update

Recompute `tcb->rcv_wnd` after `recv_head` advances in `socket_tcp_recv` and `socket_tcp_try_recv`, and send a window update when the window has reopened by at least two maximum segment sizes or half the receive buffer, which is the standard rule. This is a change in the transmit direction only and touches no state machine.

It is first because every remote path in this milestone is a file arriving at the machine. At today's rate a 700 KB kernel takes about eleven minutes to upload and a 500 MB root is out of the question; the rehearsal in phase H is not viable until this is fixed.

* Proof: the measurement repeated host to guest at 4 KB, 64 KB and 1 MB, recorded beside the numbers above; the existing TCP suites and the SSH capstone green.
* Shown failing: the window-update call removed and the 64 KB transfer back over 50 s; a host test over the update predicate showing no segment is sent for a one-byte read.
* Readmes: `rach2/sys/network/Readme.md`; the claim at `dev-log/24-metal/Readme.md:128` corrected in this milestone's record rather than edited in place.

## Phase B: the tool's new name and shape

The rename on its own, before any new capability, so the commit that touches twenty-one files changes no behavior. `utils/install-kernel.c` becomes `utils/bootctl.c` with a subcommand parser in front of the existing operations; `--where`'s extent report becomes part of `status`. No alias is shipped.

Three boundaries hold it down. The `bootslot` library keeps its name, because it is named for what it manages rather than for the tool that drives it, and because two dozen of its error substrings are matched literally by the host test; tidying the library during a rename pass is the likeliest way to break something. `dev-log/` is left as written, since docs state the current contract and history lives in the record. `make install-test` keeps its name, because the loop it proves still installs a kernel.

`status` is the phase's one new behavior, and it exists because the tool is the only window onto a disk nobody will open: which kernel is in each slot and its size, what `saved_entry` and `next_entry` hold, whether a trial is armed or consumed, which entry this boot came from, and the slot extents. It reports nothing about the core image here, because the device that would answer that question does not exist until the next phase; the core tail is added to `status` in phase E, beside the command that changes it.

Two hazards belong here and the verify gate catches neither. `tools/install_test.sh:256` parses `--where` with `awk '/candidate/{print $5}'` to find the sector it later corrupts; rewording that line moves the field, and a wrong field corrupts the wrong sector silently instead of failing, so the `awk` moves in the same commit. And `prune-stale` sweeps only the top of `build/utils` (`utils/Makefile:72-78`), so a stale `build/utils/sbin/install-kernel` survives until `make distclean`, while `tools/check_setid.py` compares manifest strings without stat-ing the host file, so a manifest line left on the old name would keep staging a real, stale binary with nothing reporting it.

* Proof: `tests/host/installkernel_hosttest.c` renamed to `bootctl_hosttest.c` and extended over the parser (unknown verb, missing argument, surplus argument, each old spelling rejected); `tools/hosttest.sh` globs the directory, so no registration changes. `tools/install_test.sh` and `tools/selfbuild_test.sh` green end to end on the new tokens.
* Shown failing: the harness pointed at the old `install-kernel: committed` string fails; the `awk` field check against a reworded extent line reports no sector rather than a wrong one; a stale `sbin/` binary staged by an un-updated manifest caught by a clean-tree build.
* Readmes: `Readme.md`, `utils/Readme.md`, `tools/Readme.md`, `tests/host/Readme.md`, `krffs/Readme.md`; the three manifests, `check_setid.py`, `utils/guest.mk` and `mkdisk_grub.sh`, with `check_guest.py` holding the host and guest lists equal.

## Phase C: the two levels, as devices

The kernel work that makes the rest possible, together with the disk geometry it validates against. Those two cannot be separated in ordering: a kernel compiled to expect the sealed layout while the builder still produces a 16 MiB boot partition at 512-byte clusters would describe a disk that does not exist, and a 64 MiB partition built before the cluster size changes is refused outright by `bootslot_open`. They can be separated in commits, and they must be, because taken together they are four unrelated risks in one change. The phase lands in four steps, each with its own proof:

1. The geometry. The three partitions, the image size, `mformat -c 8`, the host FAT fixture, the builder's hard-coded 512-byte cluster assertion at line 215, and `krffs/mkdisk.sh`'s stale 256 MiB refusal. Proved by the current kernel booting the new image unchanged and ignoring partition 3.
2. Level-one publication. The one-time probe, the exact MBR validation, and `/dev/bootpart` and `/dev/grubcore` published from discovery rather than from a mount. Proved by the RAM-root case having both devices, and by every bounded-range refusal.
3. Wide offsets. The 64-bit offset in the open-file description and both seek syscalls. This is the step most likely to break something unrelated, because the single 32-bit offset in `struct open_file` is shared by every descriptor in the system, so regular files, procfs, terminals and every device are in its blast radius, not only the raw windows. Proved by the existing file, procfs, terminal and device tests before any write authority is added.
4. Recovery authority and throughput. The new macro, component and artifact check, the writable whole-disk node, the per-write privilege recheck, and the 32 KiB batching. Its own proof, so it can land as a reviewable change rather than waiting for phase G to exercise it indirectly: a recovery build writing a bounded region through `/dev/disk0` and reading it back equal, the artifact check accepting a recovery kernel and refusing an everyday one, and an instrumented count showing a large request reaches each driver in 64-sector commands rather than one sector at a time.

Today `/dev/bootpart` is published only at the end of a successful disk mount and `/dev/disk0` only under `rach2.diskdiag`, so a RAM-root recovery boot has neither. The boot order is refactored: `block_probe` runs once, sector 0 is read and validated against the compiled geometry once, the level-one devices are published, and only then is the root source chosen.

`/dev/grubcore` is added at minor 3, mode 0600, covering LBA 1 through 2047 and nothing else. `/dev/disk0` becomes the whole device in a recovery build and is absent, or read-only for diagnostics, in every other build. Each raw handler checks, before the first copy or driver call, that the offset and count are whole sectors and that the range lies inside the node, using a subtraction form that cannot wrap. Each also checks the effective uid at write time rather than trusting the mode: uid 0 passes the generic mode check anyway, and a process can open while privileged, drop privilege, and keep the descriptor.

Two supporting changes land here. The raw windows get a 64-bit offset in the open-file description so `/dev/disk0` can address a 250 GB drive, with the legacy `lseek` refusing results it cannot represent. And the raw handlers stop writing one sector per driver call: a single 32 KB static buffer in `.bss`, below the 32 MB identity-mapped limit so its virtual address is its physical address as the AHCI PRDT requires, and 64 sectors per call, which is one command for both drivers. That takes a 500 MB reflash from about a million driver calls and cache flushes to sixteen thousand. The buffer is safe from a second writer because syscalls are not preemptible, and that reason is written down rather than assumed.

* Proof: host tests over the bounds arithmetic (offset and count not sector multiples, a range ending exactly at the end, a range one sector past it, a count that would wrap); a QEMU boot showing both level-one devices present on a RAM-root boot that mounts no disk.
* Shown failing: a crafted MBR with entry 0 moved by one sector publishes neither device; `printf x > /dev/disk0` on an everyday kernel refused, and the disk image byte-compared afterwards to prove nothing was written, following the pattern in `tools/diskpolicy_test.sh:74-110`; a descriptor opened as root, then privilege dropped, then written, refused.
* Readmes: the device table in the filesystem Readme, and a new section stating the two levels and their exact ranges.

## Phase D: the rewritable boot menu

`/boot/grub/grub.cfg` becomes a short immutable stub. It loads grubenv, defines every literal entry it owns, and only then tests for `/boot/boot.cfg` and `source`s it; `source` is the last statement before `set default`, `set fallback` and `set timeout`. `/boot/boot.cfg` is the rewritable half, preallocated to a fixed size that is a multiple of the cluster.

That order is the safety property. `source` merges the second file's entries onto the tail, so the stub's entries hold the low indices whatever the file contains, `set fallback=1` keeps selecting the entry it was written for, and an id the second file reuses loses to the stub's own. A `boot.cfg` that is missing, truncated mid-write, or wrong leaves the stub's entries defined and the machine boots.

The entry list is fixed here once, because the indices are a contract:

    0  candidate        the staged kernel
    1  fallback         the build's kernel
    2  candidate-static the staged kernel, static address
    3  fallback-static  the build's kernel, static address
    4  debug            the rescue shell
    5  recovery         the recovery kernel and its RAM root

Every entry carries `rach2.disk=ahci`, every id begins with a letter, and `bootctl trial`'s allowlist grows `recovery`. One invariant governs the list: no entry other than index 0 carries `rach2.slot=candidate`, because the only evidence a candidate booted is the consumed `next_entry` line plus that token, so a second entry carrying it would let a kernel that never booted be committed.

Entry 5 names artifacts that phase G has not built yet. It is written into the stub here, because the indices are a contract and adding an entry later would move them, but it is marked as not yet bootable and excluded from this phase's verification. Phase G stages the artifacts and adds the assertion that the entry boots.

`bootslot_open` resolves three files by padded 8.3 name today and never resolves `grub.cfg` into a writable file, so there is no chain to write a menu through. This phase adds `boot.cfg` as a fourth resolved file, and the build-time preallocation is what lets that chain be found once and never grow. The move to 4 KiB clusters belongs to phase C and is already done by the time this phase runs; what happens here is that the library's cluster arithmetic is exercised over the new geometry for the first time.

* Proof, in QEMU: a menu staged and the machine booted into an entry only the staged file defines; the slot's length unchanged before and after; grubenv round-tripped through `bootctl env`; the FAT fixture rebuilt at 4 KiB clusters with the library tests green over the new geometry.
* Shown failing: a `boot.cfg` whose first entry is left incomplete, with GRUB's parse error in the log and the boot arriving at the stub's `fallback`; the stub reordered so its entries come last, showing `fallback=1` selecting a different entry; a menu one byte over the slot refused; a write to a grubenv key outside the allowlist refused.
* Readmes: the boot-partition contract in `krffs/Readme.md` and `tools/Readme.md`.

## Phase E: GRUB updates from any image

`bootctl grub FILE` accepts a complete core image of 2 to 1024 sectors and refuses 1025. The arithmetic: a tail slot holds 1023 sectors, the image's first sector is the entry sector and does not live in the tail, so a 1024-sector image contributes exactly 1023 tail sectors and fills a slot. The tool patches the first sector's blocklist at `0x1f4` to the inactive tail's first LBA and its count at `0x1fc` to the image's sector count minus one, writes the remaining sectors into that tail through `/dev/grubcore`, zero-fills the rest of the slot so a shorter image leaves nothing of a longer predecessor behind, reads the whole slot back and compares against what it intended to write, and only then writes the patched first sector to LBA 1. A failure anywhere before that last write leaves the old entry sector and the old tail selected and the machine boots as before.

One risk remains and is stated exactly. The old core image stays in place until the final sector write, so a failure before that point costs nothing. A torn or bad write to LBA 1 makes GRUB unreachable, and recovery cannot help, because GRUB is what loads recovery. This is not safe against a power cut and the plan does not claim it is. It is accepted for three reasons: the owner requires GRUB updates from the everyday machine, the exposure is one sector written after the replacement has been verified, and nothing short of firmware network boot or removable media would remove it.

* Proof, in QEMU: two core images built with different early configuration so the stub prints which tail is running; install B, reboot, see B; install A into the now-inactive slot, reboot, see A. The running core is proved from its own output, not from reading the pointer, because reading the pointer proves only bytes at rest.
* Shown failing: an image of 1025 sectors refused before any write; a host test that reports a write as successful while dropping it, requiring the readback to mismatch, LBA 1 to be unchanged, and the next boot to still run the old tail.
* Readmes: a GRUB section in `tools/Readme.md` giving the slot geometry as a build and runtime contract, including the embedded configuration and its recovery fallback.

## Phase F: networking, one interface ABI and two clients

The owner's requirement is that os-real leans on busybox while the custom image can configure networking without it. That decides the shape rather than leaving it open: if os-real runs `udhcpc`, the kernel must expose the Linux-compatible interface anyway, and once it exists a kernel-internal DHCP client would be a second implementation of something the interface already supports. So the kernel grows one interface, and two clients use it. This is the same division milestone 19 established when os-real took busybox's account tools and os-simple kept the in-house ones.

The kernel surface, enumerated from the client's own source rather than guessed: a privileged `AF_PACKET/SOCK_DGRAM` socket bound by interface index, whose transmit takes an L3 packet plus a destination MAC and whose receive copies IPv4 payloads out of `ethernet_rx` before `ip_rx`'s configured-address check, since every DHCP reply arrives before the machine owns the address it is being offered; `recvmsg` as socketcall 17, which does not exist; an `AF_INET/SOCK_RAW/IPPROTO_RAW` descriptor used only for interface queries; the ioctls `SIOCGIFINDEX`, `SIOCGIFHWADDR`, `SIOCGIFADDR`, `SIOCGIFFLAGS`, `SIOCSIFFLAGS`, `SIOCSIFADDR`, `SIOCSIFNETMASK`, `SIOCSIFBRDADDR`, `SIOCADDRT` and `SIOCDELRT`, dispatched from `sys_ioctl` when the descriptor is a socket, with the setters restricted to effective uid 0; a peer on a connected UDP socket so `connect` and `write` work, which renewal needs and which are both refused today; and real handling of `SO_BROADCAST`, `SO_BINDTODEVICE` and `SO_REUSEADDR`, which currently succeed as blanket no-ops. `PACKET_AUXDATA` should fail with `ENOPROTOOPT`, which the client tolerates and then verifies the checksum itself; today it would get a blanket success and no auxiliary data, which is the wrong answer. ARP conflict detection stays off, so `SOCK_PACKET` is not needed.

One kernel change is subtle and belongs in the plan rather than in review comments: changing the address must clear ARP state and abort TCP connections that recorded the old local address, because a TCB copies the address when it is opened and would otherwise be left inconsistent.

Configuration moves to userland and follows an existing file rather than an invented one. os-real stages `/etc/network/interfaces` with `auto eth0` and either `iface eth0 inet dhcp` or an `inet static` stanza, and init runs `ifup -a`.

Enabling `udhcpc` does not compile, and that is the phase's first piece of work rather than a surprise during it. `networking/udhcp/dhcpc.c` includes `<linux/filter.h>` and `<linux/if_packet.h>` unconditionally, and neither exists in the ported musl's include tree; a build with the applet enabled stops at the first of them. This is the documented reason the applet was switched off, recorded in `vendor/busybox/Readme.md`.

The route is a pinned busybox patch, and it is named here because the obvious alternative was tried and does not work. Copying the two Linux user ABI headers into the musl include tree fails immediately: `filter.h` includes `linux/compiler.h`, and between them the two pull in `linux/types.h`, `linux/bpf_common.h` and `asm/byteorder.h`. Substituting musl's own `<netpacket/packet.h>` is closer but still short, because it supplies `sockaddr_ll` and `PACKET_AUXDATA` while `dhcpc.c` also uses `struct tpacket_auxdata` and `TP_STATUS_CSUMNOTREADY` outside any disabled block.

The patch that builds does three things: it drops the `linux/filter.h` include, whose every use sits inside an `#if 0` block, switches to `<netpacket/packet.h>`, and supplies the auxiliary-data structure and that one status bit behind a compatibility guard. With ARP probing and RFC 3397 left off, the enabled object list is `common.o packet.o signalpipe.o socket.o dhcpc.o`, and a full build compiles and links with no further dependency appearing. The phase proves this with a complete busybox build, not with `oldconfig`.

The patch also needs a build prerequisite, and this is the first patch busybox has ever carried, so the gap is real rather than theoretical. `build_busybox.sh` applies everything under `vendor/busybox/patches/` after the extract, but `BUSYBOX_STAMP` depends only on the build script, the two configs, the wrapper, the tarball and the libc stamp. A later edit to the patch would leave the stamp current and keep shipping a binary built from the old one. musl already solved this: `LIBC_PATCHES` is a wildcard prerequisite with a comment saying why. busybox gets the same treatment, and `vendor/busybox/Readme.md`'s claim that the vendored tree is pristine changes with it.

The busybox configuration needs a specific correction that a first draft got wrong. `ifup` is already enabled, and so is `CONFIG_FEATURE_IFUPDOWN_IP`, which selects the `ip` backend over the `ifconfig` and `route` one. Every `ip` applet is disabled, so `ifup` today would try to run a command that does not exist, and enabling `ifconfig` alone would not change which backend it picks. So the change is to turn `CONFIG_FEATURE_IFUPDOWN_IP` off and turn `CONFIG_IFCONFIG` on, alongside `CONFIG_UDHCPC` and the lease script. `ifup` also runs the `if-pre-up.d` and `if-up.d` directories through `run-parts` and treats a failure as a failed method, and the manifests do not create them, so all four hook directories are staged. The phase's proof is `ifup -a` running in the produced filesystem, not busybox merely compiling.

The custom image reads the same file with in-house tools. The DHCP client there is a substantial program, and its scope is written down here rather than discovered during the work: it builds and validates IPv4 and UDP framing including checksums, runs the discover, request, acknowledge exchange with a bounded retry schedule, parses and applies the subnet mask, router, DNS servers, lease time and server identifier, handles a NAK, and implements the T1 and T2 renewal and rebinding timers and what to do when a lease expires. Both clients renew, so the two images behave the same way. It also writes `/etc/resolv.conf` from the lease, replacing it atomically. An earlier draft claimed DNS needed nothing new because `utils/resolv.c` already reads that file; reading it is not writing it, and the current images hard-code the QEMU resolver. The new programs go into both `utils/Makefile` and `utils/guest.mk`, which `tools/check_guest.py` holds equal.

The kernel's default address does not change, and this is a correction to an earlier draft that would have broken the gate. A boot with no address option still gets the SLIRP tuple `10.0.2.15/24`, because `make test`, `make ping-test` and the self-build test all boot with no append string and `tests/loopprobe.c` compares against that exact address. Those harnesses have no init and no userland configurator, so a machine that came up unconfigured would simply fail them. Instead a new token, `rach2.ip=none`, means come up with the NIC probed and IPv4 unconfigured because userland will do it. The sealed image carries that token; nothing else does, and every existing suite runs unchanged.

`rach2.ip=A.B.C.D/prefix` stays as an explicit early-boot override for tests and rescue. Netconsole is unaffected by any of this because it transmits broadcast Ethernet frames below IP and needs no address at all, which is why it is the way to find a machine whose address you do not know. Init starts the console login first, then `ifup`, then dropbear, so a network failure never removes local recovery.

* Proof, in QEMU: SLIRP's DHCP behavior was read from the libslirp source rather than assumed, including that it always broadcasts the offer and the acknowledgement and skips a zero UDP checksum. The machine boots with a `dhcp` stanza on an explicit `net=192.168.90.0/24,dhcpstart=192.168.90.20`, takes the lease, and the network suite runs over the address it was given. The same image with a `static` stanza is proved in the same run, and both are proved twice: once with `udhcpc` on os-real and once with the in-house client on the custom image.
* Shown failing: a QEMU `hubport` on an unused hub is a network with no server and needs no host sockets; the client gives up inside its bound, the machine still reaches a login, and the boot log names the failure. Malformed offers (wrong transaction id, truncated option field, an offer of a network or broadcast address) rejected in host tests over the parser. An address change with a TCP connection open shown to abort that connection rather than leave it inconsistent.
* Proof, at the laptop: one boot at home and one at the office, each taking an address with no boot line edited. That was the original request.
* Readmes: `rach2/sys/network/Readme.md`, the written-once contract in `netconf.h`, the sentence in `tools/mkiso.sh` that says there is no DHCP client, `vendor/busybox/Readme.md` whose list of disabled applets stops being true, and the root `Readme.md`, whose claim that busybox is unmodified and that only three vendored trees carry patches becomes false the moment this patch lands.

## Phase G: recovery and whole-disk reflash

The recovery image is a separate kernel build with the recovery switch, plus a RAM root loaded as a multiboot module. Both live on partition 3, and they are reachable two ways: stub entry 5 on partition 1 for the ordinary case, and the configuration embedded in the core image when partition 1 cannot be read at all. Recovery mounts no filesystem from the target disk, which is what lets it rewrite a broken root. Its own userland is the RAM image, so it has SSH and tools.

`reflash DEVICE OFFSET BYTES` takes the length as an argument, and that is a correction to an earlier draft rather than a decoration. The earlier form read a stream and promised to refuse an input that ran past the device before writing a byte, which a pipe cannot support: its length is unknown until it ends, so the check could only happen after part of it had already been written. With the count declared, the tool seeks to the device end, validates the offset and count by subtraction before it reads any input, and only then starts. All three numbers parse as 64-bit.

Two details follow from the device contract. A read from an ssh pipe returns any positive number of bytes, while the raw devices accept only whole sectors, so the tool coalesces reads into sector-sized writes rather than passing each read straight through. And it requires the input to end exactly at the declared count, refusing both a short stream and a trailing partial sector.

The tool reports the byte count written and refuses to run past the end. That shape makes `ssh` the transport with no code of its own, and busybox already supplies `dd`, `sha256sum`, `cmp`, `sync` and `gunzip` for the rest. Recovery also carries `bootctl`, so a broken root, a broken kernel slot, a broken menu, a damaged boot filesystem and the inactive core slot are all repairable from one session. What recovery cannot repair is stated in the risks: LBA 0, LBA 1, and an active core image too damaged to read partition 3.

While `devfs.c` is open, one comment is corrected: it claims the 0400 mode already refuses `open(O_WRONLY)`, which is false for root, since the credential check returns success the moment the uid is zero. The mode stops non-root and nothing else.

* Proof, in QEMU: the drill. Zero the KRFFS magic at the root partition's first bytes with the machine stopped, boot the ordinary entry and require the mount failure rather than an SSH answer, select recovery through a host-crafted `next_entry`, stream the compressed root through an ssh pipe into `/dev/disk0` at the root's offset, require the exact byte count, read the region back through the same device and compare hashes, then reboot and require a clean mount and a login. The form is `gzip -c IMAGE | ssh HOST 'gunzip | reflash /dev/disk0 OFFSET 524288000'`, the host supplying the uncompressed length it already knows.
* Proof of the embedded fallback, on the image the build produces rather than on a scratch disk: with partition 1 readable, the boot selects partition 1's stub; with partition 1's boot sector and both FATs zeroed, the same image reaches the real recovery kernel and its module from partition 3. Without this pair, a missing `recovery.cfg`, a wrong path, or a forgotten `set root=(hd0,msdos3)` passes every other test in the milestone and is discovered only on the day it is needed.
* Shown failing: `reflash` on an everyday kernel refused, and a shell redirect into the device refused the same way, with the image byte-compared afterwards to prove the refusal was real; a write running past the device refused before the first byte; an everyday artifact offered as recovery rejected by the artifact check.
* Readmes: a recovery section in `tools/Readme.md` naming the build, the devices and the exact `ssh` invocations; the corrected comment in `devfs.c`.

## Phase H: the sealed image, rehearsed end to end

A new `tools/sealed_test.sh` behind `make sealed-test`, not an extension of `install_test.sh`. The existing harness proves the installer itself and owns assertions that must not move: the refusal to commit while a trial is armed, the marker inside the trial boot's own byte interval, the cold boot selecting the committed candidate, the damaged and repaired intervals, the second pass over the disk the first pass modified, and the three grubenv states. The sealed rehearsal proves something different, that the finished production layout can be operated over SSH.

It takes one byte-for-byte copy of the flash artifact, verifies its hash, and mutates only that copy for the whole run, in raw format so that offsets computed on the host mean the bytes that reach the SSD. One QEMU at a time, one serial log per cold boot, guest reboots staying inside the same process and isolated by ordered byte offsets, host faults requiring a stop and an offline edit.

There are two runs, and separating them fixes a contradiction in an earlier draft. The fault run does the whole loop: cold boot to a login over SSH; stage a kernel, trial, reboot, commit; a bad kernel caught by the fallback and repaired; a new menu, then a truncated one caught by the stub; a core image switched to the other tail and back; a lease taken; the root damaged and reflashed from recovery; and a final cold boot proving the machine returned to a good state. It asserts the fallback and recovery tokens where they belong. The control run, on a fresh copy, injects no faults and also omits the deliberate entry into recovery, then asserts that no fallback and no recovery token appears anywhere. A control run that entered recovery on purpose could never assert the absence of a recovery token, which is what the earlier draft asked for.

The budget is twelve to fifteen minutes and stays unverified until the harness prints per-phase times. If it runs long, the answer is to remove duplicated work that other harnesses already own, not to shrink the root, switch away from raw, skip the readback, or start later phases from fresh images, because those would stop testing the production artifact or the accumulated state.

Only then is the physical write worth doing, and the last deliverable is a host procedure filed beside `dev-log/24-metal/checklist.md`. It names the drive by model and serial and confirms both before any write, since the milestone 24 baseline describes the laptop's original 1 TB drive and not the one being sealed. Then: boot the image in QEMU one last time, unmount the device, write 660602880 bytes (630 MiB, 1290240 sectors), read the same count back from the device and compare hashes, and record what was on the disk that day.

* Proof: the rehearsal green from a clean tree, and the image's hash recorded beside the manifest that built it.
* Shown failing: every recovery step has its matching injected fault named with an exact offset, and the control run proves those steps would otherwise have nothing to catch.
* Readmes: `tools/Readme.md`'s disk section; the physical-write procedure filed with the record.

## Phase I: the sweep, the record, and the gate

`make verify-style` and `make verify-host` from a clean tree, the QEMU suites, the SSH capstone, `install-test`, `selfbuild-test` and `sealed-test` as separate claims. An independent adversarial review of the whole diff, every finding adjudicated with evidence and every confirmed one fixed. The record in `dev-log/25-sealed/Readme.md`: what was built, what was refuted, what was deferred and why, and the measured numbers from the laptop's first sealed boot.

## Risks, in expected order

Three things cannot be repaired remotely, because every recovery path runs after GRUB has worked: LBA 0, LBA 1, and an active core image damaged badly enough that it cannot read partition 3. Phase E reduces the core exposure to one sector written last, after the replacement has been read back and compared, and the embedded configuration means partition 1 can be destroyed without losing recovery. Nothing removes the entry sector itself as a single point of failure.

The write path in a recovery build is the riskiest code in the milestone. A bug that lets an everyday kernel write `/dev/disk0` turns any root mistake into a destroyed disk. The distinction is compiled in rather than parsed from a boot line, two tests assert the refusal on an everyday kernel including through a shell redirect, and phase I's review is pointed at it.

The networking phase is the largest surface: a new address family, a new socketcall, ten ioctls and connected UDP. It is also the phase most likely to disturb something already working, since it touches the socket layer that SSH depends on. The existing suites and the SSH capstone run unchanged throughout, and the static path is proved in the same run as the leased one.

DHCP moves address configuration into userland, so the machine now comes up unconfigured until init runs. The mitigation is that the console login starts before `ifup`, netconsole works with no address at all, and `rach2.ip=` remains for boots that need reachability before init.

A write to the menu slot or grubenv interrupted halfway is expected eventually. The stub's own entries cover the first and grubenv's fixed block plus the allowlist covers the second, and both have a shown-failing case.

Once the disk is sealed, a machine that will not boot cannot be fixed remotely. That is why the order is what it is: every remote path is proved in QEMU against a byte-identical image before the SSD is written.

## What this milestone deliberately leaves out

A second root partition. A bad root is recovered from the recovery image, a path this plan builds and tests. The consequence: recovery takes a reboot into recovery rather than into another root.

A separate partition for configuration. It lives in the root. The consequence: reflashing the root loses it, so the procedure says to keep a copy.

A watchdog. The consequence: a hang costs a walk to the power button.

Network boot and removable recovery media. The consequence: damage to LBA 0, to LBA 1, or to the active core image is a case-open job. Those are the failures with no remote path, and they are named in the risks rather than hidden here.

Redundancy below GRUB. Nothing this milestone can build removes the entry sector as a single point of failure.

## What must not be broken

Sector 0. No code in this milestone writes it after build, and `/dev/grubcore` does not reach it.

The compiled distinction between an everyday kernel and a recovery kernel. It is not a boot-line token, and it must not become one.

The trial and commit state machine from milestone 23: fallback by entry number, the menu ids, the `rach2.slot=` tokens including the substring the harness rewrites with `sed`, the byte patterns `bootslot.c` scans for, the 1024-byte grubenv block, and exit status 1 on every refusal.

The rule that exactly one menu entry carries `rach2.slot=candidate`. It is what makes a commit mean the kernel booted, and the menu grows in this milestone, so it is the invariant most likely to be lost by accident.

Every error substring in `bootslot.c` that the host test matches literally, which is why the library is neither renamed nor tidied in the same pass.

The assertions `tools/install_test.sh` already owns, listed in phase H, which stay in that harness rather than migrating into the new one.

The static `rach2.ip=` form and QEMU's SLIRP tuple, byte for byte, so every existing network suite keeps passing while DHCP is added beside it.

The guest build's equality with the host lists, checked by `tools/check_guest.py`.
