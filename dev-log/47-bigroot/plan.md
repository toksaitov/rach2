# Milestone 47: the root takes the disk

## The goal

The root partition is nearly the whole disk, minus what the boot, the
fallback and the recovery partitions need, and the system runs on it: a volume of hundreds of gigabytes mounted, written, checked
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
* The e2fsprogs port builds `e2fsck` only (`build_e2fsprogs.sh:91` leaves
  `resize2fs` out); there is no `resize2fs` or `mke2fs` on the machine.
  `mkext2.sh` takes a size argument and makes a sparse image; the 1024 is
  the Makefile's choice, not the script's. The pinned `resize2fs` grows
  its bitmaps in memory (`resize/resize2fs.c:808`).
* `reflash` reads a stream and writes every byte of it
  (`sectorcopy.c:126`); a sparse image is not smaller to it, and a file on
  the stick's exFAT volume is read through 32-bit offsets.
* The ext2 writer caps a file at `0xFFFFFFFF` bytes (`ext2.c:2053`); the
  dirty-volume check (`ext2.c:2758`) tracks every block and inode of the
  volume at once. The group descriptors are one array read at mount
  (`ext2.c:93`).
* The recovery RAM image is built to a 4 MiB budget (`Makefile:493`).
* The sealed image is written today by hand from recovery with `reflash`,
  partition by partition, under the Readme's "Updating the installed
  machine"; the installer is milestone 48 and will offer the whole disk
  from its first version because this milestone comes first.
* The Lenovo's SATA disk has 488397168 sectors, 238475 MiB
  (`dev-log/43-machine/hardware.md:43`); the 1 TB disk in the same record
  was a USB disk plugged in by mistake. The fourth partition entry is
  already 2 GiB long with a 1 GiB file system in it.

## Decisions

### The volume is grown, not written large

The image stays the size it is. A large sparse image made on the host is
not a shortcut: `reflash` would write every zero of it, the stick's volume
cannot hold it, and only an extent package, offsets and lengths and hashes
with a writer that seeks, would carry it, which is a format and a tool of
its own. So the installer writes the image, then grows the file system to
the partition with `resize2fs`, ported to musl from the pinned e2fsprogs
and staged on the recovery image with its budget raised by name, offline,
before the first boot. Its memory on the target size is measured before it
is trusted, and if it does not fit the machine, the extent package is the
recorded fallback. The partition entry is written from the disk's size by
a `growroot` step in the recovery image that the installer of milestone 48
will call and that a person can run by hand: it rewrites the fourth
entry, rereads the table and runs `resize2fs`.

### The driver takes a large volume

The group descriptors become an array sized at mount from the superblock,
about 60 KB for this disk and 240 KB for a 1 TB one, allocated from the
memory milestone 46 made available; not a cache with lifetimes on every
access. The dirty-volume check is not partitioned: a dirty volume larger
than the check's bound mounts read-only, says so, and is repaired by
recovery's `e2fsck`. That narrows the automatic check to the sizes it can
hold, which is the bound it can keep. What the driver refuses, it refuses by
name.

### The limits are stated exactly

A file is at most 4 GiB less one byte; the volume is bounded by the MBR's
32-bit sector count, 2 TiB, and by the largest size this milestone has
tested. `disk(7)` and the ext2 Readme say all three.

## Phases

### Phase 1: `resize2fs`

Ported, staged on recovery, grown volumes accepted by `e2fsck -fn` and by
the host. Proof: a sparse QEMU disk the size of the Lenovo's, 238475 MiB,
grown from 1 GiB to the whole partition with QEMU at 64 MiB, the memory
of `resize2fs` and of the recovery `e2fsck` measured and recorded, and a
growth interrupted by a killed machine repaired afterwards.

### Phase 2: the driver

The descriptor array sized at mount; the read-only answer for an oversized
dirty volume. Proof: the target-sized volume mounted, files written near
its far end, read, unlinked and counted by `statfs`, the machine killed
and the volume repaired from recovery; the ext2 host test over a fixture
with the metadata of that size; the phase ends with a Codex review of the
driver change as one unit.

### Phase 3: the grow step

`growroot` in the recovery image: the fourth entry rewritten from the
disk's size, the table reread, `resize2fs` run, `e2fsck -fn` to confirm.
Proof: the row on a target-sized QEMU disk, and a run interrupted after
the entry is written and before the grow, repaired by running it again.

### Phase 4: the machine

The Lenovo's root grown to the disk from recovery, under the rule: the
stick booted and shown to work, the fourth entry and the table saved on
the host and the stick, recovery the saved default until the grown root
has booted and been checked.

### Phase 5: the record

The ext2 and block Readmes and the Readme's "Updating the installed
machine" corrected in the same commits, notes, `tests/run all`, the
milestone's Readme and the index entry. `disk(7)` is milestone 49's.

## Risks

* A volume that comes up dirty and too large for the check will run
  read-only until someone boots recovery; the message says exactly that
  and the manual page repeats it.
* `resize2fs` on a volume with `resize_inode` off, which is what this
  driver writes; the port is tried against exactly this image before it is
  trusted.
