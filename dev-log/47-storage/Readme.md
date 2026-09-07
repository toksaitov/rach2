# Milestone 47: storage

Every disk the kernel finds is a record of one registry, published as `/dev/diskN` with a node for each partition its table describes. The partition tables are read by one service that both the MBR and GPT paths go through. A root is named by identity rather than by position, so a disk that moves keeps its root. AHCI disks answer on their interrupts with a queue behind each port, and the IDE driver serves both channels and both drives. Files may be as large as their file system allows, ext2 volumes are no longer bounded by a table compiled into the kernel, and a root can be grown into the free space after it. Nothing here ran on the Lenovo.

## What is built

`disk.c` keeps one record per disk: its identity from the drive's own IDENTIFY answer, its capacity, its transport and controller position, a window over the whole medium, a window per partition, and a lifetime state. A driver registers a disk as it finds it, and the record outlives a stick that is pulled, so a handle held across the removal reaches a record that is still there and answers that the disk is gone.

`partition.c` is the one reader of a partition table. It takes the four MBR primaries by position, walks an extended chain numbering its logical partitions from five, and reads a GPT from its primary header or, when that fails its checksum, from the backup at the end of the medium. A partition is refused rather than published when it lies outside the medium, overlaps one already taken, carries a number the table cannot address, or, in an extended chain, would hold the record that names the next partition.

`devfs` publishes a node for every disk and every partition, all of them block devices, so `blkid`, `findfs` and `mount LABEL=` find their volumes the way they do elsewhere. A whole-disk node takes writes only on the recovery kernel, and one writer at a time holds a disk.

The root is named on the boot line by `rach2.root=` as a partition identifier, an MBR signature and entry, a file-system identifier, a label, or a node path, with `rach2.rootfstype=` beside it. Every partition of every disk is compared, exactly one match mounts, and an identity found on two disks is refused by name. `rach2.boot=` names the disk whose boot partition the kernel uses, and the searches read from that disk.

AHCI takes an interrupt vector after the root is mounted, enables each port's completion and fatal interrupts, and keeps one command active per port with a queue behind it. The polled path stays for the page-fault context, draining the port first. The IDE driver was rewritten for both channels and both drives, with the 48-bit commands above sector 2^28, and refuses a drive whose logical sector is not 512 bytes, as the AHCI driver does.

A block request carries up to the device's own limit, sixty-four sectors on AHCI, in heap pages that the command table describes one entry per page. The buffer cache finds a block through an index by device and block, and writes runs of adjacent dirty blocks in one request. An ext2 write marks the bitmap bytes, descriptors and pointer words its allocations touch and writes them at the end of the write, the bitmaps and descriptors before the pointers that name their blocks.

A file offset is 64 bits throughout, `_llseek` reaches any offset the format allows, and `ftruncate64` exists. Each file system refuses a write or a truncate past its own limit by name: ext2's is the reach of its pointers or its 32-bit block count, 2095103 MiB at 4096-byte blocks; FAT32's is 4 GiB; exFAT's is the seek's own reach. ext2 stores the high half of a regular file's size and sets the volume's large-file flag before the inode that needs it.

The ext2 descriptor table is taken from the heap at mount, so there is no compiled group limit. The volume check's workspace comes from the heap for the length of the check, sized from the volume, after the free frames are counted against it; a dirty volume the memory cannot hold mounts read-only and says so.

`resize2fs` is ported beside `e2fsck` and staged on the recovery system. `growroot` grows a root partition and its file system into the free space after it: the disk by its signature, the entry by its number, the space bounded by the next partition, the disk's end or the MBR's reach, the table rewritten and read again, then the checker and the resizer. `growset` computes, from a volume's own geometry, the sectors such a grow writes, which is the set to save before one.

## What proves it

`tests/run disks`, `parts`, `bootorder`, `bus`, `queue`, `usbdisk`, `large` and `grow` are new or rewritten for this milestone, and the older disk, mount, file-system and boot suites run unchanged against the new paths. The proofs of the large-file and grow work are recorded once at their full size and run small by default: `RACH2_LARGE_MIB=5120` writes a 5 GiB file on ext2 and on exFAT, and `RACH2_GROW_LENOVO=1` grows a 1 GiB root to a partition the size of the installed machine's disk.

Five mutations were proved: a partition service that takes the first match rather than the only one fails `parts`; a driver that never chooses the 48-bit commands fails `bus`; a driver left polled after the switch fails `queue`; a grow set without its block-bitmap ranges fails `grow`; and an ext2 file limit of 4 GiB fails `large`. A dirty ext2 volume on a machine with no free frames mounts read-only by name, which the host test proves by mutation as well.

## The machine

Nothing in this milestone touched the Lenovo. The grow, the identity roots and the interrupt-driven controller are proved in QEMU; milestone 50 is the first run on the machine.

## Trust

The rates the disk path reaches are QEMU's, not a disk's: a file written through ext2 on the emulated AHCI controller goes at about 8 MiB a second and through exFAT at about 10, against 4 and 1 before the request layer and the buffer cache were widened. What the change proves is that the path moves a run of blocks per request and finds a block without walking its list; it says nothing about the Lenovo's controller.

A disk with a logical sector other than 512 bytes is refused by both drivers, and that refusal is unproved in QEMU: its IDE and AHCI models take no such drive, so the row written for it was removed and the restriction stands on the code alone. Entry 11 of the shortcomings list records it.

The grow is proved on a 16 GiB disk by default and once on a partition the size of the installed machine's, both in QEMU, with `growset` computing the sectors the grow writes and `qemu-img map` confirming that nothing outside them was written. The memory a grow needs was measured by halving the machine down to 16 MiB, where it still finished; 8 MiB does not boot, which entry 12 records. Neither says what the machine will do with a disk that answers slowly.

The check's workspace and the descriptor table come from the heap, so a volume is bounded by memory rather than by a compiled limit. The refusal when memory is short is proved by a host test with the free frames forced to none; it is not proved on a machine that ran out of memory while working.

`resize2fs` and `growroot` are staged on the recovery system and have never run on hardware. The identity roots, the interrupt-driven controller and the queue behind each port are likewise proved only in QEMU, and milestone 50 is the first run on the machine.

## What was accepted

MILESTONE_ACCEPTED
