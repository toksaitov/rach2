# FAT32

`fat32.c` is the `vfat` type: a read-only reader of FAT32 volumes on a partition window, placed by `mount(2)` as described in [vfs.Readme.md](vfs.Readme.md). Everything on the volume is reached through the buffer cache in 4 KiB blocks, one buffer held at a time.

## What is read

The BIOS parameter block in sector 0, the active file allocation table, and the directory and file clusters the FAT chains name. Only the first 512 bytes of the boot sector are interpreted. The FSInfo sector and the backup boot sector are read by nothing, and a second FAT only when the volume's flags make it the active one.

## The volume check

The constructor reads sector 0 and checks it field by field before the mount is published. A volume that fails is refused with `EINVAL`, and the kernel log reports the field: `vfat: refused: FIELD`. In order: the boot-sector signature; the FAT32 recognition rule, which is Linux's (a zero 16-bit FAT size, a nonzero 32-bit one, no root-directory entries, FAT version zero), whatever the cluster count, because the 65525-cluster threshold is a formatting rule and volumes below it exist; a sector size of 512; sectors per cluster a power of two from 1 to 64; a nonzero reserved count; one or two FATs; a total sector count that fits the partition; FAT regions that fit before the data region; a nonempty data region whose cluster count fits a 28-bit FAT; a FAT large enough to cover the clusters; a root cluster inside the data region; and an active FAT the volume has. With mirroring on, the first FAT is read; with it off, the FAT selected by the flags.

## Directories and names

A directory is read at 32-byte entry offsets, which is what `getdents64` reports as the position. A long name is kept when its entries are in sequence and their checksum matches the short entry that follows; otherwise the short name is shown, in lower case where the reserved byte says so, with the `0x05` lead byte restored and a byte above `0x7F` shown as `?`. Deleted entries, volume labels and orphaned long-name fragments are skipped. Lookup folds ASCII case and matches the long name or the short alias, as Linux's vfat does; a non-ASCII name matches exactly. `.` is the directory itself, and `..` is resolved from the directory's own entry, with cluster zero mapped to the root.

## Chains and bounds

A FAT entry is masked to 28 bits. The walk to cluster `n` of a chain is bounded by the volume's cluster count, so a looped chain ends in `EIO` rather than a hang; a per-call step count would not do, because a looped directory whose clusters are full never reaches a zero entry. A chain that ends before the index it is asked for answers `ENXIO` inside the driver: a directory reads it as its end, a file as damage and answers `EIO`. A node keeps a hint from its last walk, used when the target is beyond it.

## Nodes and identities

A node is an entry in a fixed per-mount pool of 32, freed at its last put; a lookup that finds the pool full answers `ENOMEM` and logs it. A directory's identity is its first cluster, so two lookups of one directory share a node. A file's identity is its directory's cluster and its entry index. The inode number is built from the identity: a directory's is its cluster, and a file's has its directory's cluster plus one in the high word and its index plus one in the low, so no two entries share a number.

## Times and modes

FAT keeps local time with no zone; it is read as UTC. Days count from 1980-01-01 through the Gregorian rule; an unset or malformed stamp reads as 1980-01-01. Every entry reports the mount's `uid` and `gid` and the mode `0777` less its `umask`, the options Linux's vfat takes and BusyBox passes through, so under the default `umask=022` a file is `0755`. FAT has no execute bit, so a program on a volume runs unless the mount is `noexec`. The options are `umask=`, `uid=`, `gid=` and `ro`; any other option answers `EINVAL`.

## What is refused

Every write-side operation is NULL, so the common layer answers `EROFS`. A mount without `MS_RDONLY` answers `EROFS` from the constructor, which is what a write-protected medium answers on Linux and what BusyBox `mount` retries read-only. Four volumes can be mounted at once. `statfs` reports the free cluster count as of the first call, since a read-only volume cannot change, with `bsize` the cluster size and magic `0x4d44`.

## Verification

`tests/run host` runs `fat32_hosttest` over `mkfatfixture`'s volumes: the `--vfs` volume must read back exactly what the builder wrote, through fragmented chains, long names across a block boundary and with a surrogate pair, a name with a bad checksum, one over a `0x05` short name, a deleted entry, and `..` back to the root; a valid FAT32 below 65525 clusters mounts; the volume whose active FAT is the second reads through it; entries with reserved high bits are masked; and each malformed volume is refused at mount with the field named or answers `EIO` at the file it damages. `tests/run mount` mounts a volume that `mformat` wrote, reads it from a program and from BusyBox, and compares the listing with `mdir`'s.
