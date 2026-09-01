# FAT32

`fat32.c` is the `vfat` type: FAT32 volumes on a partition window, read and written, placed by `mount(2)` as described in [vfs.Readme.md](vfs.Readme.md). Everything on the volume is reached through the buffer cache in 4 KiB blocks, one buffer held at a time.

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

## Writing

Metadata is written through and data is written back, as [vfs.Readme.md](vfs.Readme.md) describes for the cache. Every FAT entry, directory entry, the boot sector's flag and FSInfo go through `bcache_write` and reach the disk before the call returns, in call order, so the order in which the driver writes metadata is the order in which it reaches the disk. A file's bytes go through `bcache_dirty` and reach the disk at eviction, at `fsync`, at `sync`, and through the periodic sync; where a data write must reach the disk before a metadata write that describes it, the driver syncs the device between the two.

Every operation that changes the volume runs under the mount's namespace lock for its whole length: the namespace operations arrive with it held by the common layer, and `write`, `truncate`, `fsync`, `sync` and the reclamation a last `put` performs take it themselves. Two writers, or a writer and a sync, never interleave across the sleeps the writes make.

The rule for the order of writes: a crash may leak clusters and may never cross-link them, and an entry never names a cluster that is free.

* Create: the entry is written free of clusters, with size zero and first cluster zero. A create that needs a new directory cluster is a directory growth first.
* Extend: the new clusters are allocated as a chain of their own, ended and zero-filled, disconnected from the file; the data is written and the device synced; the FAT entry of the old tail is pointed at the new run; the entry's size grows. A crash before the last step leaves a chain longer than the size, which the walk treats as surplus. A file with no clusters yet publishes its first cluster and its size together in the one entry write.
* A write past the end zero-fills the gap before the size grows over it, so the file never reads stale bytes.
* Truncate: the entry's size shrinks first, and its first cluster goes to zero when the size does; the retained tail gets an end-of-chain mark; the detached tail is freed last. A grow is a write of zeros.
* Unlink: the entry is freed first; the chain is freed last, and only when no description holds the node. A held node keeps its chain until its last `put`, and a crash in between leaks the chain. A write through such a description changes the clusters and no entry.
* Mkdir: the cluster is allocated, ended, zeroed and given `.` and `..`, and the parent's entry is written last. A crash before the last step leaks the cluster.
* Rmdir: the directory must hold nothing but `.` and `..` (`ENOTEMPTY`) and must not be held by a description or a working directory (`EBUSY`); then as unlink.
* Directory growth: the new cluster is zeroed and ended before the old tail is pointed at it, so the directory never shows stale entries.
* Rename within one directory to a new name: the entry is rewritten in place. Otherwise, and across directories: if a destination exists it is freed first, its chain freed or left to its holders as in unlink; the source entry is freed; a moved directory's `..` is rewritten; the new entry is written last. A crash between the freeing of the source and the writing of the destination leaves a nameless chain, which the walk reports as lost.

The dirty flag is set before the first metadata write after a mount, in both places (bit 27 of FAT entry 1 cleared, bit 0 of byte 65 of the boot sector set), and stays set for the life of a writable mount. It is cleared only by the clean unmount, after every dirty buffer is written and FSInfo is current; a failure there leaves the volume marked dirty and the mount where it was. FSInfo, when the volume has one with its signatures, is written at `sync` and at unmount with the free count and the next-free hint this driver keeps; the hint is read at mount and the count is computed from the active FAT the first time it is needed. With mirroring on, every FAT write goes to the active copy first and the mirror second.

A cluster is allocated first-fit from the hint, which after a file's growth is the cluster after its last, and the search wraps once. `ENOSPC` is decided before any FAT sector is written, and a run that cannot be completed is given back. The largest file is 4 GiB less one byte, answered with `EFBIG` past that.

## What is refused

A name that does not fit 8.3 with each of its two parts all upper or all lower case answers `EINVAL` in this phase; the case is recorded in the entry's reserved byte, as Windows NT and mtools record it. Four volumes can be mounted at once. `statfs` reports the free cluster count kept by the driver, with `bsize` the cluster size and magic `0x4d44`. A mount whose device fails a write goes read-only for the rest of its life, as the cache's failure record says.

## Verification

`tests/run host` runs `fat32_hosttest` over `mkfatfixture`'s volumes: the `--vfs` volume must read back exactly what the builder wrote, through fragmented chains, long names across a block boundary and with a surrogate pair, a name with a bad checksum, one over a `0x05` short name, a deleted entry, and `..` back to the root; a valid FAT32 below 65525 clusters mounts; the volume whose active FAT is the second reads through it; entries with reserved high bits are masked; and each malformed volume is refused at mount with the field named or answers `EIO` at the file it damages. The write half of the same test runs each operation as a sequence on a fresh volume, reads the result back through the driver, dumps the image and walks it with `mkfatfixture --check`, and copies a file this driver wrote out with mtools where mtools is installed; then it stops each sequence after every one of its writes and walks the image, which must show nothing but what the order of writes allows: lost clusters, a surplus tail, orphan long-name entries, a lagging mirror or FSInfo, and the dirty flag. `tests/run mount` mounts a volume that `mformat` wrote, reads it from a program and from BusyBox, and compares the listing with `mdir`'s.
