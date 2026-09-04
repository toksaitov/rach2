# Milestone 43 hardware

What the Lenovo G50-70 reports in this session, beside what QEMU reported
when each check was written. The seven earlier `hardware.md` files, from
milestones 36 to 42, are the procedures; this file is their results.

## Arrival

Read over SSH from the kernel the machine booted on arrival, before any
install.

| Reading | Value |
|---------|-------|
| Address | 192.168.0.106 |
| Boot line | `rach2.root=disk rach2.slot=candidate rach2.disk=ahci rach2.netconsole rach2.ip=none` |
| `bootctl status` | candidate at sector 312, 4096 sectors, 2097152 bytes; fallback at sector 4408, 1632 sectors, 834472 bytes; saved_entry candidate; next_entry empty; booted candidate |
| `/proc/mounts` | `/dev/disk0 / krffs rw,relatime`, devfs, proc |
| `/proc/usb` | port 2 low 46d:c34b class 3 protocol 1 (keyboard); port 3 low 46d:c077 class 3 protocol 2 (mouse) |
| `/dev` | bootlog bootpart console full grubcore null ptmx random tty urandom zero |

The arrival kernel predates milestone 36: it has no partition nodes, so
the hashes of the root and recovery partitions are read after the first
install.

## Phase 1: the first install

The final kernel of milestone 42, `474b990c` by SHA-256, 1382560 bytes,
installed through `bootctl kernel` from the arrival kernel. The first
trial was booted with `rach2.nomsi rach2.apicdiag` through a staged menu
that `bootctl config` wrote and one trial consumed: it intercepts
`next_entry=candidate`, consumes it as the stub would, and boots the
candidate slot with the extra tokens, so a kernel that hangs still
falls back on the next boot. Rehearsed on the sealed disk in QEMU
first. The second trial was the plain candidate line; `bootctl commit`
followed it.

| Reading | Value |
|---------|-------|
| Boot to SSH, both trials | about 40 s from `reboot` |
| APIC | see `dev-log/39-msi/hardware.md`, the Lenovo column |
| xHCI | 00:14.0, 8086:9c31, MSI on vector 48; an EHCI at 00:1d.0 beside it |
| HDA | the driver binds 00:1b.0 on vector 49; codec 0x14f1510f |
| Disk | AHCI port 0, 488397168 sectors, 238475 MiB |
| Network | RTL8168GU, MAC 28:D2:44:7E:E0:AF |
| USB at boot | a hub 35d6:2510 on port 1 with nothing behind it; the keyboard on port 2; the mouse on port 3; ports 4, 6 and 7 the soldered devices this kernel refuses |
| `/dev` | bootlog bootpart console disk0p1 disk0p2 disk0p3 full grubcore null ptmx random tty urandom zero |

The partitions' hashes after the install, read on the machine with `dd`
over each node and `sha256sum`:

| Partition | SHA-256 |
|-----------|---------|
| `/dev/disk0p1`, the boot partition, after the install and the commit | `9861ae5566f7c55ac9dddca7f23b9b39d0f267203659ac8d2c3efe9644029dbb` |
| `/dev/disk0p2`, the KRFFS root | `0c49f17c11cb69d1fd4a42824f32b75faac9b4744c8bfd2d2725cbee75ffc4b3` |
| `/dev/disk0p3`, the recovery partition | `5bbc9904f9f70806e4ce296e182049ee64904e120584673574408df700ff5299` |

The boot partition as pulled before the install hashed to
`5a3df08c724d410b1f83f89f09dad77a0fc99a784e1da48371db3ad6b0f06fe9`,
and its copy is on the host. The KRFFS root's hash is taken with the
root mounted and writable, so it is the value at that moment and not a
seal; the recovery partition's is a seal.

The milestone 36 checks, run by hand and then by the acceptance
script's new section: `/dev/disk0p1` and `/dev/disk0p3` mount read-only
from BusyBox's `mount`, list, and unmount; the boot partition's four
files are the sizes `bootctl status` reports for its slots. A 4 MiB
transfer over SSH from the host and a 500 MiB read of the root partition
both completed with the interrupt count moving only on the timer and the
xHCI, since the disk and the network are polled paths.

## Phase 1: the interrupts

`/proc/interrupts` on the final kernel, read over SSH before and after a
console session in which the owner logged in on the USB keyboard, ran
Doom with the shareware IWAD for more than ten seconds, moved the mouse,
and quit. Two boots: the committed candidate line, and a trial with
`rach2.nomsi rach2.blocklag=30` staged through the menu file.

| Vector | With messages, before | after | With `rach2.nomsi`, after |
|--------|-----------------------|-------|---------------------------|
| 32, the timer | 36982 | 46362 | 93393 |
| 33, the 8042 keyboard | 1 | 11 | 280 |
| 39, IRQ 7, no driver | absent | absent | 57 |
| 42, the HDA on its legacy line | absent | absent | 1319 |
| 44, the 8042 mouse | 1 | 1 | absent |
| 48, the xHCI by message | 753 | 2914 | absent |
| 49, the HDA by message | absent | 845 | absent |
| spurious | 0 | 0 | 0 |
| usb fallback | 0 | 0 | 0 |

With messages the xHCI and the HDA count on their vectors and the
timer's pump finds nothing. With `rach2.nomsi` the xHCI is pumped by the
timer and the HDA takes its legacy line, IRQ 10; vector 39 is IRQ 7,
which both controllers name as their legacy line in configuration space
and which the 8259 also raises as its spurious interrupt, and the 57 it
took have no driver. Both boots typed and played. Under
`rach2.blocklag=30` every request through the lag device leaves the
`block: completion dropped: stale generation` record the device exists
to provoke, which the owner saw on the console before Doom; it is the
test device's own record and not the machine's.

The 4 MiB transfer over SSH from the host and the 500 MiB read of the
root partition, on the first boot: `/proc/stat`'s `intr` moved from
14711 to 16072 across the transfer, the timer's share; the network and
the disk are polled paths and neither has a vector.

## Phase 2: the fourth partition

| Reading | Value |
|---------|-------|
| Sector zero before | `f00ee34d3209ae7bd079155c3d48e4fd1be9a8003a3f3897d327e4aaa7145e8d` |
| Sector zero after | `a02966c9f463e19a2441eb8c983ddc83b9a06649b95ad4c1335a86b609946698`, ten bytes changed, all in entry 4 |
| Entry 4 | type 0x0C, sector 1290240, 4194304 sectors |
| The range's head before | `2c1c569a332417e6bc29e1042ba7c6b386cadf984e1d5f170ef0bf6aa1aa15ce`, 16 MiB |
| The volume's metadata written | 4214784 bytes, `07c73b270404e9612536bcc5ae2cdf736ef3d4544475d7a08ac35dc4777a5a17` |
| Write time through `reflash` | 13 s |
| Recovery partition after | `5bbc9904f9f70806e4ce296e182049ee64904e120584673574408df700ff5299`, unchanged |
| Boot partition's files after | kernel bytes `70ec5e83...`, fallback `fff0988b...`, `grub.cfg` `17e032aa...`, all as before |
| A file copy of 4 MB through the mount | intact across unmount and mount |
| The stick copy through the FAT driver | 0.65 MB/s falling to 0.17 MB/s by 900 MB |
| The recovery stick | the recovery ISO with an 8 GiB FAT32 second partition, booted at the BIOS menu; recovery from partition 3 has an older kernel and its own host key |

## Phase 3: the stick on a root port

The data stick is the SanDisk 3.2Gen1 (781:55a9), 61524148224 bytes,
formatted on the mac mini with `diskutil eraseDisk ExFAT RACH2DATA GPT`,
which gave it a GUID table with a 200 MiB EFI vfat partition and the
exFAT volume. The rates are `dd` in 32 KiB pieces from the whole device
and 64 KiB pieces through the exFAT mount, 16 MiB each unless stated.

| Socket | Port | Speed | Raw read | exFAT write | exFAT read |
|--------|------|-------|----------|-------------|------------|
| Right side, black | 1 | high | 8.4 MB/s | 2.1 MB/s | 8.7 MB/s |
| Left side, black | 3 | high | 8.4 MB/s | | |
| Left side, blue | 10 | super | 23.2 MB/s | | |
| Left side, blue, `rach2.nomsi` | 10 | super | 103 KB/s, 4 MiB | about 60 KB/s | |

In every socket `blkid` reports both partitions, the volume mounts by
`LABEL=RACH2DATA`, lists the files the Mac and the earlier sockets
wrote, takes a file and `sync`, and unmounts with two flushes counted
in `/proc/usb`. The pull under a mount in port 1: a read and a write
fail with `EIO`, `umount` returns 0, `/proc/usb` shows the disk closing
with one hold and then reclaimed.

The clock self-test: `clock: failed: 2000 us asked, 2018 us seen` on
the MSI boot and `1361 us seen` on the nomsi boot; the phase 1 boots
passed it with 2019 us.

The three files the sockets wrote read on the Mac with their content;
their times show six hours ahead because the RTC holds local time.
`diskutil verifyVolume`, which runs `fsck_exfat -n -x`, finds the
volume the kernel wrote in three sockets clean.

The same volume with an MBR, in port 1: `blkid` reports the one
partition, the volume mounts by label, lists the Mac's file with the
time the Mac wrote it, takes a file and `sync`, and unmounts with two
flushes.

The stick image `rach2-stick.img` built from the tree at b392fc8,
`86178f18...`, written to the stick on the Mac and read back with the
same hash. The notebook booted it from the firmware's boot menu, first
GRUB entry, into the RAM root with `rach2.ip=192.168.0.105/24`: the
self-tests passed 13 of 13, `blkid` reports the ISO and the exFAT
fixture volume on `/dev/usb0p2`, the volume mounted, listed the
fixture's files, took a file and `sync`, and unmounted with two flushes.
The file that boot wrote reads on the Mac from the stick's second
partition, and `diskutil verifyVolume` finds that volume clean. The
notebook then booted its disk again as the candidate.

## Phase 4: the hub

The owner's hub is a USB 3.0 hub: its high-speed half is the 35d6:2510
with four ports, and in the blue socket its SuperSpeed half, 35d6:3510
protocol 0, appears on port 10 and is refused with `it did not give a
hub descriptor`. The keyboard (46d:c34b) and the mouse (46d:c077)
are low-speed devices; behind the high-speed hub they run through the
controller's transaction translator, which QEMU does not model.

| Socket | Port | Tree | Keys and mouse | Stick behind the hub |
|--------|------|------|----------------|----------------------|
| Right side, black | 1 | hub high; 1.1 stick high; 1.3 keyboard low; 1.4 mouse low | work, Doom played | mounts, takes a file, raw read 8.3 MB/s |
| Left side, black | 3 | the same tree on port 3 | work, Doom played | mounts, takes a file, raw read 8.2 MB/s |
| Left side, blue | 2 and 10 | hub high on port 2 with the keyboard and mouse; the SuperSpeed half on port 10 refused | work, Doom played | absent: the stick attaches to the SuperSpeed half, which this kernel does not drive |

In port 1: the stick pulled from the hub under a mount answered `EIO`,
unmounted with rc 0 and was reclaimed while the keyboard and mouse
stayed up; plugged back it was a new device with its volume found; the
hub pulled with all three on it gave four slots back, and plugged back
it enumerated all three within 0.35 s of its own record, after which
the keys, the mouse and the stick's mount worked. The owner's 1 TB
JMicron 152d:578 disk, plugged in by mistake for forty seconds, was
found, reported 1953525168 sectors, and given back on its pull.

The translator proof, 2026-09-03, with the owner's approval of the two
kernel installs on `/dev/disk0` through `bootctl`: a kernel whose slot
context omits the translator's slot and port, `15069d72...`, installed
and trial-booted with the hub in port 1. The controller answered the
Address Device command for the keyboard and the mouse with code 4, a
parameter error, and `/proc/usb` showed the hub alone; QEMU's full-speed
hub cannot show this. Then the kernel the tree at a604e04 builds,
`0e2d1d11...`, 1386668 bytes, installed, trial-booted with the keyboard
and mouse enumerated behind the hub, and committed as the saved
default. On both boots the stick on the hub at boot time refused its
configuration; see the phase 4 differences.
The stick pulled from the hub and plugged back on the committed kernel
enumerated at 285 s, mounted and took a file. The device count against
the twelve entries: the hub, the keyboard, the mouse and the stick are
four, and the owner's disk made a fifth for a minute; no more devices
are available here, so the table's limit is not reached on this
hardware.

## Phase 5: the ext2 root

| Step | Reading |
|------|---------|
| The recovery stick | `rach2-recovery-stick.img`, `f530085a...`, written on the Mac and read back; RACH2SAVE formatted with `newfs_msdos`; booted at the firmware menu with `reflash` and `e2fsck` in its root |
| The fourth partition's head before | `e173865850166dda...`, 16 MiB, saved on the host and the stick |
| The ext2 root written | `rach2-metal-disk.ext2`, `d7045c2a...`, 1073741824 bytes at 660602880, in 224 s through `gzip`, `ssh` and `reflash`; read back with the same hash |
| The boot partition before | `37897384b0495f05...`, 64 MiB, saved on the host and the stick; the stick copy took 45 minutes through the FAT32 driver |
| The boot partition after | `e802b7b7...`: `grub.cfg` with `rach2.root=ext2` on the two candidate entries, 7693 bytes of the image changed, written in 15 s and read back |
| The recovery partition | `5bbc9904...` before and after both writes |
| The trial | armed from the disk's recovery entry; `/proc/cmdline` has `rach2.root=ext2`, `/proc/mounts` has `/dev/disk0p4 / ext2 rw,relatime`, 23 MB used of 1 GiB, SSH answers |
| On the candidate | `tcc -static` builds and runs a program; a file survives `reboot -f`, which unmounts cleanly on this kernel, so no check ran; `bootctl status` reports the slot |
| The fallback | `bootctl trial fallback` booted the KRFFS root, `/dev/disk0 / krffs`, without the ext2 file; `bootctl trial candidate` booted ext2 again with it |
| The commit | `bootctl commit` from the candidate: the saved default is the candidate |

The first ext2 root written was the wrong image, `os-real.ext2` with the
plain init; its trial booted to the console with no network and no SSH
server, and the root was written again with `rach2-metal-disk.ext2`
from the disk's recovery entry, which the owner reached by typing
`bootctl trial recovery && reboot` at the console. The disk's recovery
partition is milestone 37's and has no `e2fsck`; the stick's recovery
has it.

The crash test, 2026-09-03: a loop appending a line and rewriting a
1 MiB file seven times a second on the ext2 root, and the owner held the
power button until the machine was off. The next boot recorded
`ext2: check: clean` at 0.46 s before the mount, mounted the root
read-write, and the log file had its 140 lines up to the cut. At the
console on the ext2 root the keyboard, the mouse and Doom work as on the
KRFFS root.

## Phase 6: the acceptance script

Run from the mac mini against the ext2 root with the hub in the
right-side black socket carrying the keyboard, the mouse and the
recovery stick. The first run failed four lines, each a fault of the
script and not of the machine: it required milestone 25's read-only
stick kernel of a system whose root is on the disk; it took the clean
check after the power cut for a failure; and it took the recovery
stick's ISO half as the volume to write, which failed the write and the
flush count. The script now tells the installed kernel from the stick's
by the boot line's root, accepts `ext2: check: clean` and fails any
other check record, uses whichever disk slot holds the stick, and picks
its first FAT or exFAT volume, leaving an ISO half alone. The second
run passes every line on the machine, and `tests/run metal` passes the
same script against QEMU. The first run is the proof of the three
changed checks: each failed before its change on the case it now
accepts.

## Phase 7: the install round

The tree's kernel installed through `bootctl` twice on 2026-09-04 with
the owner's approval, `65053b03...` and then `a4304a7c...` with two
fixes the round found, each trial-booted on the ext2 root and
committed; all eleven self-tests pass on both boots, the clock's among
them, and the stick on the hub at boot enumerates under the five-second
configuration window where it had timed out at half a second.

| Check | Reading |
|-------|---------|
| The hub in the blue socket | port 2 the high-speed half with the keyboard and mouse; port 10 the SuperSpeed hub, protocol 0, think time 0; the stick at 10.2, super |
| The stick behind the SuperSpeed hub | mounts, takes a file, raw read 22.2 MB/s, unmounts with two flushes |
| The pull under a mount | the hub reports the link fallen inactive, then the disconnection; the read answers `EIO`, `umount` returns 0, the disk is reclaimed |
| The replug | on the first kernel not seen, since the warm reset the link report asked for never ended on the empty port; on `a4304a7c...` enumerated in 0.3 s, mounted and read |

The FAT32 writer on the machine, `a4304a7c...` and later: a 64 MiB
write to the stick's RACH2SAVE volume behind the SuperSpeed hub ran at
2.2 MB/s and its removal took under a second, where phase 2's copy
through the same driver ran at 0.65 MB/s falling to 0.17 MB/s. One such
write of 64 MiB stopped at 33 MB with the stick silent, recorded under
the phase 7 differences; the next completed.

| Left side, blue, `rach2.nomsi`, on the line | 10.2 | super | 21.1 MB/s | 2.3 MB/s | |

The nomsi row above is `e9246422...` with the controller's PCI line:
`usb: interrupts: the line, irq 7 on vector 39`, 48361 interrupts on
that vector in the minute the reads took, no fallback.

The recovery partition, 2026-09-04, with the owner's approval of the
write on `/dev/disk0`: partition 3 saved as
`saved/recovery-partition-before.img`, `5bbc9904...`, on the host and
on the stick; the same volume with `/boot/recovery` and
`/boot/recovery.krffs` replaced by the tree's recovery kernel and image
written through `gzip`, `ssh` and `reflash` in 26 s and read back as
`d28d96f1...`; the recovery entry then booted from partition 3 with
`/sbin/e2fsck` and the USB stack, and `e2fsck -fn /dev/disk0p4` found
the ext2 root clean. The saved default stayed the candidate throughout.

## Phase 8: the machine's own build

From the ext2 root on 2026-09-04, the staged `/src` copied to
`/home/test/kbuild` and built with the staged pdpmake and TinyCC:
the kernel in 7.3 s, 684700 bytes, `c9280827...`; the utilities and
the shell with exit 0. The staged sources are the tree the ext2 image
was built from, `bb3f628` or thereabouts, so that kernel is behind the
installed one by the fixes of the install round; the root is rewritten
from the current tree before the kernel the machine builds is
installed.

The root rewritten from the current tree, `7f724aed...`, with the
owner's approval, read back with that hash and `e2fsck` clean; the
machine then built its kernel again from the staged `/src`: 688796
bytes, `8ed3c0f8...`, installed through `bootctl kernel`, trial-booted
with all eleven self-tests passing, the five USB devices up and SSH
answering, and committed as the saved default. The machine runs the
kernel it built.
