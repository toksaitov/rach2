# Milestone 47: notes

The record of the work as it went, phase by phase. The plan is `plan.md`; the accepted result is `Readme.md`.

## Phase 1: the registry

The registry is `rach2/sys/block/disk.c` with `disk.h`: up to eight disks, numbered in the order the drivers register them, each with its whole-disk device, transport and location, the IDENTIFY strings, the partition table state and entries, a partition view per non-empty entry, a whole-disk view for `/dev/diskN`, and the raw writer's reservation for that disk. `block_probe` scans every registered disk's first sector after the driver probe; the boot disk's views are then re-set from the table the root was chosen by, over the lag device when `rach2.blocklag` asks for one, so the nodes and the root come from one read of sector 0.

AHCI keeps up to four disks per controller, each with its own command list, received-FIS area and command table; the reset points every port at a scratch pair nothing reads, and each kept port is started with its own areas. A port that fails to start or identify is stopped and skipped with a record. The ATA driver registers its one disk. `block0` is the first disk's device, so the boot paths are unchanged.

devfs makes the nodes when a disk registers: `/dev/diskN` and `/dev/diskNp1` to `p4`, one vtable, listed while the window is published. Disk 0 keeps its minors, 1 and 4 to 7; disk N takes minors from 64 + 8N. Inodes are the table's entries first, then the disk nodes in registration order. The writer reservation is per disk; the claims are compared on the physical disk as before.

`/proc/disk` keeps its record of disk 0 and then prints one `disk N TRANSPORT LOCATION SECTORS TABLE MODEL` line per registered disk.

Proof: `tests/run disks` (new, core) boots two AHCI disks of 64 MiB and 20 MiB on one controller with `rach2.diskdiag`, and `disksprobe` requires both in `/proc/disk` with their ports and sizes, the nodes of both, no node for an absent partition or disk, and a partition of each mounted (KRFFS, FAT32), written and read back; the host then reads the FAT32 file with `mtype`. The mutation that keeps only the first port in the AHCI scan makes the suite fail; restored and rebuilt, it passes. The disk suites (`disk`, `disk-ahci`, `disk-q35`, `bootpart`, `diskdiag`, `mount`, `persist`, `persist-ahci`, `usbdisk`, `diskpolicy`, `procfiles`, `krffsmount`, `loopdev`, `ext2`, `fatwrite`, `exfat`, `sealed`, `install`, `grub`, `bootline`, `default`, `metal`, `diskfull`, `stress-ahci`) and the host tier pass unchanged.
