# Milestone 50: the root takes the disk

## The goal

The installer offers the root partition nearly the whole disk, minus what
the boot, the fallback and the recovery partitions need, and the system
runs on it: a volume of hundreds of gigabytes mounted, written, checked
after a stop and repaired, with the file size limits of a 32-bit kernel
stated rather than discovered. The milestone is complete when the Lenovo's
disk has been installed that way and `e2fsck` from recovery and the
kernel's own check both accept the volume.

## What the tree settles before this plan

* The ext2 driver refuses a volume of more than 128 block groups, 16 GiB at
  4 KiB blocks (`ext2.h:9`); its dirty-volume check walks with bounds sized
  for that (`ext2.c`, the check's block and inode limits).
* File offsets are 32 bits on the ordinary path (`fd.h`); a file larger than
  4 GiB is not this kernel's to make, whatever the volume's size.
* The e2fsprogs port builds `e2fsck` only; there is no `resize2fs` or
  `mke2fs` on the machine, and `mkext2.sh` makes the image on the host at a
  fixed size.
* Milestone 46's installer writes the sealed layout; its manifest names the
  image's size and hash.
* The Lenovo's disk is 1 TB, and its fourth partition entry is already 2 GiB
  long with a 1 GiB file system in it.

## Decisions

### The volume is grown, not written large

The image stays the size it is, since a 900 GiB file on a stick is not a
thing to carry. The installer writes it, then grows the file system to the
partition, with `resize2fs` ported to musl from the pinned e2fsprogs and
staged on the recovery image, offline, before the first boot. The
partition entry is written by the installer from the disk's size.

### The driver takes a large volume

The group limit becomes a bound on what the driver keeps in memory rather
than on what it mounts: group descriptors are read through the cache as
needed, not held whole; the dirty-volume check walks in bounded passes with
its memory sized from what the machine has. What the driver still refuses,
it refuses by name.

### The 32-bit limits are stated

A file is at most what a 32-bit offset addresses; the volume may be as
large as ext2 allows. `disk(7)` and the ext2 Readme say both.

## Phases

### Phase 1: `resize2fs`

Ported, staged on recovery, grown volumes accepted by `e2fsck -fn` and by
the host. Proof: a QEMU disk grown from 1 GiB to 20 GiB and checked.

### Phase 2: the driver

Group descriptors read as needed; the check in passes. Proof: the 20 GiB
volume mounted, written, killed and repaired; the ext2 host test over a
large fixture.

### Phase 3: the installer

The partition entry from the disk's size; the grow after the write; the
question that offers it. Proof: the installer row on a large QEMU disk.

### Phase 4: the machine

The Lenovo installed with the root taking the disk, under the rule.

### Phase 5: the record

`disk(7)`, the ext2 Readme, notes, `tests/run all`, the milestone's Readme
and the index entry.

## Risks

* A check that walks a large volume on a machine with little memory: the
  passes are bounded and the memory sized, and the row runs with QEMU at
  64 MiB.
* `resize2fs` on a volume with `resize_inode` off, which is what this
  driver writes; the port is tried against exactly this image before it is
  trusted.
