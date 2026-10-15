# exFAT

`exfat.c` is the `exfat` file system type. It reads and writes an exFAT volume through the buffer cache, refuses a volume whose boot region or root objects break the format, and checks a volume that was not unmounted cleanly before it mounts it writable. exFAT is the format flash media above 32 GB is sold with, and the install stick keeps its images on an exFAT partition, which [rach2-install(8)](../../../man/man8/rach2-install.8) and [rach2-recover(8)](../../../man/man8/rach2-recover.8) find by mounting every partition of every removable disk with this type. The driver follows Microsoft's published specification. What it shares with the FAT32 driver in `fatcommon.c`, the name check and its UTF-8 and UTF-16 conversion, the DOS timestamps, the mount options, the allocation hint and the dirty-volume walk's bitmap, is described in [fat32.Readme.md](fat32.Readme.md).

## Interfaces

* `vfs_exfat_type` (`exfat.h`) is the type `exfat` of `mount(2)`, on a partition, a stick or a loop device ([vfs.Readme.md](vfs.Readme.md)). `MS_RDONLY` gives a read-only mount, and the options are those of `vfat`: `umask=`, `uid=`, `gid=` and `ro`. A remount between read-only and writable is `EINVAL` ([mount(2)](../../../man/man2/mount.2)).
* What each call does on this type is in its manual page: [stat(2)](../../../man/man2/stat.2), [statfs(2)](../../../man/man2/statfs.2), [open(2)](../../../man/man2/open.2), [chmod(2)](../../../man/man2/chmod.2), [chown(2)](../../../man/man2/chown.2), [mkdir(2)](../../../man/man2/mkdir.2), [rename(2)](../../../man/man2/rename.2), [link(2)](../../../man/man2/link.2), [truncate(2)](../../../man/man2/truncate.2), [getdents64(2)](../../../man/man2/getdents64.2), [fcntl(2)](../../../man/man2/fcntl.2) and [sync(2)](../../../man/man2/sync.2).
* Every change runs under the mount's namespace lock. The caller holds it for `create`, `unlink`, `mkdir`, `rmdir` and `rename`, and the driver takes it for `write`, `truncate`, `setattr`, `fsync`, `sync` and the last `put` of an unlinked file; a change made without it panics. Reads run without it.

## Design

### The mount

`ex_mount` checks the volume before it publishes the mount. It computes the boot checksum of each boot region, sectors 0 to 10 and 12 to 22 with `VolumeFlags` and `PercentInUse` skipped, requires every 32-bit word of sectors 11 and 23 to equal it, and compares the two boot sectors byte for byte, those fields aside. When both regions pass and agree, the mount goes on. When only the backup passes, its fields are used with the main's flags, and the mount is read-only, as it is when the backup fails or the two disagree. When both fail, the mount is refused as `BootChecksum` if either boot sector has exFAT's name, jump and boot signature, and as a volume of another type if neither has.

A wrong `FileSystemName`, jump or boot signature says only that the volume is not exFAT, and is refused with no record, since the installer tries this type on every partition. These three are tested in both boot sectors before a checksum failure is recorded, so a volume of another type, whose sectors 11 and 23 hold no checksum, leaves no record either. The other fields are checked against the specification's ranges and the device, and refused with `exfat: refused: FIELD`: `MustBeZero`, `BytesPerSectorShift`, `SectorsPerClusterShift`, `NumberOfFats`, `FileSystemRevision`, `VolumeLength`, `FatOffset`, `ClusterCount`, `FatLength`, `ClusterHeapOffset`, `FirstClusterOfRootDirectory` and `VolumeFlags`. A volume with two FATs or with `MediaFailure` set mounts read-only, and the first reason for a read-only mount is recorded as `exfat: mounted read-only: WHY`.

The root directory is read through its FAT chain. It must have one allocation bitmap for the active FAT, with a bit for every cluster, and one up-case table, each inside the heap; a second of either, the lack of one, or a root chain that leaves the heap or loops is refused. A bitmap for the other FAT makes the mount read-only. Both objects are read through their FAT chains, since their entries have no `NoFatChain` flag.

### The up-case table

exFAT compares names without regard to case through a table on the volume that maps each UTF-16 unit to its upper case, and every lookup, comparison and name hash here goes through the volume's own table. At mount its checksum must match, its length must be even and at most 128 KiB, and its runs must expand to exactly 65,536 mappings: a `0xFFFF` and a count are that many identity mappings, any other unit maps the next code point, a run of zero is a fault, and a `0xFFFF` as the last unit maps U+FFFF to itself. The first 128 mappings must map each code point to itself, except `a` to `z`, which map to `A` to `Z`.

The record `exfat: up-case table: FORM` gives the mount's form of the table. One of at most 16 KiB, as the compressed recommended table is, is `held` in the mount's record and walked from its start for a unit. One of exactly 128 KiB with no run marker is `uncompressed, indexed through the cache` at offset `2 * unit`. A larger compressed one is `streamed through the cache` from its start for each unit. The mappings of the first 2048 code points are read once at mount, since a lookup hashes the name of every set it passes.

### Entry sets

A file or directory is an entry set: a file entry (type `0x85`), a stream extension (`0xC0`), `ceil(NameLength / 15)` file name entries (`0xC1`), and any benign secondaries. `set_read` reads a set one entry at a time and accepts it when:

* `SecondaryCount` is at least 2, and every entry after the file entry is an in-use secondary, so a damaged count cannot take in the next file's entries;
* the stream is second, the name entries follow it with the units past `NameLength` zero, and only benign secondaries follow them;
* the stream's reserved flag bits are clear, `AllocationPossible` is set, `ValidDataLength` is at most `DataLength`, and a directory's two lengths are equal and at most 256 MiB;
* a stream has a first cluster, inside the heap, exactly when it has a length, and a `NoFatChain` stream has a length and a run inside the heap;
* the name hash and the set checksum match.

A set that fails is passed over whole with the record `exfat: set skipped: WHY`. A set may cross a sector or a cluster. A vendor allocation entry (`0xE1`) has a first cluster and a length, which the dirty-volume check marks and a removal frees; its bytes are never read.

### Reading

A stream with `NoFatChain` set is contiguous, and its FAT entries are not read. Another chain is followed through the active FAT to `0xFFFFFFFF`; a bad cluster, a free entry, a value outside the heap, a cluster whose entry is itself, or more steps than the volume has clusters is `EIO`, after the bytes before it. A node remembers the last position it reached in its chain, so a sequential read or an append does not walk from the first cluster. Bytes past `ValidDataLength` read as zeros, and the file's size is `DataLength`. Every access goes through the buffer cache in 4 KiB blocks, one buffer held at a time, and a directory scan takes its 32-byte entries from a copy of the block in the mount's record.

A lookup converts the name to UTF-16, hashes it through the up-case table, and compares sets by length and hash and then by up-cased units; a name FAT cannot store is `ENOENT`. exFAT directories have no `.` and `..` entries, so a directory's node remembers the directory it was found in, and `..` is that one; when that node is gone from memory, a walk of the tree from the root finds it. A name whose UTF-8 form does not fit the kernel's 255-byte component is listed by its first 60 units, without splitting a surrogate pair, and cannot be reached by a path.

A node is the record of a file or directory in memory, allocated from the heap when a lookup or creation first reaches it and freed at its last `put`. A directory is identified by its first cluster, and a file by its directory's first cluster and the index of its file entry; the inode number is made from the same fields. A rename moves a held node to the new place, so an open file keeps its node and its advisory locks ([lock.Readme.md](lock.Readme.md)).

### The order of writes

Metadata is written with `bcache_write`, which returns once the device has the write, and data is written back ([bcache.h](bcache.h)). The order of writes is this:

* a cluster's bitmap bit is set and its chain written before any entry refers to it;
* data is written and synced before a length is published;
* a set is unpublished before its clusters are freed, and the clusters of a node a description holds are freed at its last `put` and not before;
* a truncation shortens the stream before it frees the tail;
* a directory grows by a zeroed, chained cluster before an entry is written into it.

A barrier is `sync_disk`, which writes the device's dirty buffers and then flushes the device's own cache with `block_flush`, or `block_flush` alone where only the order of two writes matters. On ATA, AHCI and NVMe disks a write through the cache is on the medium when it is answered and a sync of the cache ends with the disk's own flush, so the driver's flush finds nothing to do; a USB stick is sent `SYNCHRONIZE CACHE`, and a loop device syncs its backing file ([block/Readme.md](../block/Readme.md)).

The bitmap bits and FAT entries of a run of clusters allocated together, of a chain freed and of a contiguous chain written into the FAT are set in the cache and reach the disk in runs at the `sync_disk` that precedes the join, the publication or the return, so a write of many clusters costs a few metadata writes and not several per cluster. A `write` puts its bytes in the cache, runs `sync_disk`, joins the new run to the file's chain with one FAT entry and a flush, and then publishes the new lengths. When the join's flush fails, the old tail is ended again and the run freed; a tail that cannot be ended keeps the run as a surplus tail, which the dirty-volume check frees. A growing `truncate` joins its run the same way; a shrinking one publishes the shorter lengths, flushes, ends the kept chain and frees the rest.

An unlink of a file that no description holds frees its clusters before it returns, with a flush before the first is freed and `sync_disk` after the last, so they are free on the medium when the unlink returns. A held file keeps its clusters until its last `put`, and a crash before then leaves them lost. `fsync`, `sync` and the clean unmount each run `sync_disk`.

### Sets on disk

The in-use bit of the file entry publishes or retires a set. A creation builds the set in the mount's buffer and finds free entries for it, growing the directory when there are none. It writes any entries past the file entry's 512-byte sector and flushes after them, and then writes the file entry's sector, with the in-use bit and the checksum of the whole set, in one write; a stop before that write leaves orphan secondaries, which readers pass over and the dirty-volume check retires. A set whose file entry would fall in the last slot of a sector starts in the next sector, with an unused file entry (`0x05`) written into the slot when it was the end marker. So every set this driver makes has its file and stream entries in one sector, and a change to its lengths, first cluster, flags, attributes or times reaches the medium in one sector's write. On a set another writer made with those two entries in different sectors, the stream's sector is written first, and a stop before the file entry's leaves a set whose checksum alone fails, which the dirty-volume check rewrites.

A removal writes the file entry inactive, flushes when a secondary lies in another sector, and then writes the secondaries inactive. A vendor allocation's clusters are freed at once, and the stream's chain at once or at the held node's last `put`.

A rename within one directory, to a name no other set has, with as many entries as before, and with the stream and name entries in one sector, rewrites the set in place by the steps of a creation; a stop then leaves the old name or the new one, at worst with a checksum the check repairs. A set whose stream and name entries span two sectors is not rewritten in place, since a stop between the two would leave parts of both names under the old hash. Any other rename finds room at the destination first, so `ENOSPC` changes nothing. It writes the new set with its file entry inactive, retires a replaced destination, whose clusters wait for its holders as on an unlink, and then the source, flushes, and publishes the new file entry; a stop between the source's retirement and the publication leaves the chain with no name, as lost clusters. The new set has the source's file and stream entries and its benign secondaries.

### Allocation and lengths

Every file and directory this driver creates has a FAT chain and `NoFatChain` clear. Before a contiguous file or directory another writer made can grow, its run is written into the FAT and synced, and its stream is published with the flag clear. A cluster is allocated first-fit in the bitmap from a hint, the cluster after the last one allocated or a lower one freed since. The free count is counted from the bitmap once per mount, and `ENOSPC` is decided from it before anything is written. `mkdir` writes the new directory's cluster, zeroed and ended, before the set that refers to it, and a directory grows one zeroed cluster at a time.

A write that begins past `ValidDataLength` first writes zeros up to its offset within the file's clusters, and a new cluster is zeroed, so the valid length can rise to the write's end. A growing `truncate` raises `DataLength` alone, and a shrink lowers `ValidDataLength` to the new length when it was past it. A new set has its three times set to the present, with the 10 ms count and each UTC offset byte `0x80`, valid and zero. A write and a `truncate` set the modification time, and a read does not change the access time.

### The dirty bit

A writable mount sets `VolumeDirty` in the main boot sector, and it stays set for the mount's life, as Linux's driver keeps it. The mount, each `sync` and the clean unmount also write `PercentInUse`, or `0xFF` while the free count is not known. The clean unmount runs `sync_disk`, writes the percent, and clears the bit last, so a failure before leaves the volume marked dirty, as does a mount that a failed write made read-only. The backup boot region is never written.

Before the first change in a directory, the driver reads the directory to its end, and one with a critical entry this driver does not know is frozen: a create, `mkdir`, unlink, `rmdir` or rename in it is `EROFS`, it cannot itself be removed or renamed, and the record `exfat: directory cluster C frozen` gives the entry.

### The check of a dirty volume

When `VolumeDirty` is set at a writable mount, `ex_check_volume` walks the volume before the mount is published. It takes the walk bitmap the FAT32, exFAT and ext2 checks share, one walk at a time, and with it a static table of 4096 name hashes. It marks the chains of the root, the bitmap and the up-case table, then walks the tree, checking every set as the reader does and marking every chain it reaches, a contiguous stream by its length, a chained one to its end, and each vendor allocation. These are faults: a cluster reached twice; a chain that leaves the heap, loops or ends in a free or bad entry; a stream shorter than its length; a reached cluster whose bit is clear; a set that fails a rule other than its checksum; a directory with no cluster; a primary entry other than a file, bitmap, up-case table, label, volume GUID or TexFAT padding; a bitmap, up-case table or label outside the root; and two names in one directory that up-case alike. A fault makes the mount read-only, recorded as `exfat: check: WHY; read-only`, and the volume is left as it was.

Otherwise three states the order of writes leaves after a crash are repaired, in this order: a set whose checksum alone fails has its checksum rewritten; orphan secondaries are written inactive; and a surplus tail, the clusters a chain has past its length, is cut at the last cluster the length needs and then freed. The bitmap is then compared with the walk. A cluster in use that nothing reached is lost: it is counted, recorded and left allocated, since after a stop inside a rename it may have the only copy of a file's bytes. The dirty bit stays set until the clean unmount.

## Limits

* The logical sector must be 512 bytes (`BytesPerSectorShift` 9), and a cluster at most 32 MiB.
* A volume with two FATs (TexFAT) mounts read-only.
* A file is at most 2^63 - 1 bytes; a write or `truncate` past that is `EFBIG`. A directory is at most 256 MiB, and a creation that would grow one past it is `ENOSPC`.
* There are no hard links, symbolic links, device nodes, FIFOs or sockets (`EPERM`), and no `fallocate` (`EOPNOTSUPP`). Owner, group and mode come from the mount; only the write bits change, through the read-only attribute.
* A rename to another case of the same name in its own directory, such as `a` to `A`, succeeds and changes nothing.
* The volume label is neither read nor set.
* A dirty volume mounted read-only is read without a check.
* The dirty-volume check handles 32 levels of directories counting the root, 4096 names in the directories along the path it is walking, 16 surplus tails, 16 sets whose checksum alone fails and 64 orphan secondaries. Past any of these, or when the heap cannot supply the walk bitmap, the mount is read-only with the reason recorded.
* A directory whose node is not in memory is found by a walk from the root that descends at most 32 levels; a call that needs a deeper one, `..` or a change to a file in it, is `ENOENT`. A directory more than 64 levels below the root has no path for a directory descriptor's `*at` calls (`ENAMETOOLONG`).
* With a compressed up-case table larger than 16 KiB, each unit at or above U+0800 is found by reading the table from its start.

## Tests

* `tests/run exfat` (core) is six boots that write an exFAT volume from a program and from BusyBox, judge it on the host with `mkexfatfixture --check`, read it back after a reboot, and stop a write for the next mount to repair ([`tests/system/suites/exfat.sh`](../../../tests/system/suites/exfat.sh)). `tests/run usbdisk` (core), `large`, `diskrate` and `installer` also mount exFAT volumes.
* `tests/run host` runs `exfat_hosttest.c`, the driver over `mkexfatfixture`'s volumes and a device model that keeps every write pending until a flush, and `exfatcheck_hosttest.c`, which proves the host reader. `sh tests/host/exfat_scan_mutate.sh` and `sh tests/host/fat_barriers_mutate.sh` are mutation proofs of the cost rows and of each barrier. [tests/host/Readme.md](../../../tests/host/Readme.md) describes the rows.
