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
