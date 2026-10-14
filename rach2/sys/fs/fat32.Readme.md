# FAT32

`fat32.c` is the `vfat` file system type. It reads and writes a FAT32 volume on a device that `mount(2)` places: a disk partition, a USB stick or one of its partitions, or a loop device. Rach2's own disks keep their boot and recovery partitions in FAT32, which `rach2-install` and `init` mount through this type, and a volume another system wrote mounts the same way. The driver writes metadata in an order after which a crash leaves only a few known faults, and a writable mount of a volume that was not unmounted cleanly checks it and repairs those faults before the mount is published.

## Interfaces

* `vfs_vfat_type`, declared in `fat32.h`, is the type record `mount(2)` finds by the name `vfat` ([vfs.Readme.md](vfs.Readme.md)). It has no operation for hard links, symbolic links, device nodes, preallocation or remounting.
* The options are `umask=`, `uid=`, `gid=` and `ro`, as the last subsection of Design says; any other fails the mount with `EINVAL`.
* What a program sees is in the manual: [mount(2)](../../../man/man2/mount.2) for the flags, remounts and the bounds of the dirty-volume check; [stat(2)](../../../man/man2/stat.2), [statfs(2)](../../../man/man2/statfs.2) and [getdents64(2)](../../../man/man2/getdents64.2) for what they report; [chmod(2)](../../../man/man2/chmod.2) and [chown(2)](../../../man/man2/chown.2) for the changes refused; [truncate(2)](../../../man/man2/truncate.2) for the size limit; and [rename(2)](../../../man/man2/rename.2) for directory renames.

## Design

### The volume

`fat_mount` parses the options and reads the first 512 bytes of sector 0. It refuses the volume with `EINVAL` and the record `vfat: refused: CHECK` at the first of these checks that fails: the `boot-sector signature`; `not a FAT32 volume`, by Linux's rule (a zero 16-bit FAT size, a nonzero 32-bit one, no root entries, FAT version zero) whatever the cluster count, since the 65525-cluster threshold is a rule for formatting; a `sector size` of 512; `sectors per cluster` a power of two from 1 to 64; nonzero `reserved sectors`; a `FAT count` of 1 or 2; `total sectors` nonzero and within the device; `FAT regions` within the volume; a `data region` of a cluster or more; a `cluster count` of at most `FAT_MAX_CLUSTERS`, 0x0FFFFFEE, so that no cluster number reaches the reserved values; a `FAT size` with an entry for every cluster; a `root cluster` in the data region; and an `active FAT` the volume has.

With mirroring on (bit 7 of the extended flags clear) the driver reads FAT 0 and writes each FAT entry to both copies, FAT 0 first; with it off, only the FAT whose number bits 0 to 3 give. An FSInfo sector in the reserved region with its lead and structure signatures gives the next-free hint; its free count is not read, and the driver counts the free clusters in the active FAT when it first needs them. The backup boot sector is not read.

The volume is reached through the buffer cache in 4 KiB blocks, one buffer held at a time. A FAT entry is masked to 28 bits, and a write keeps the top four bits. An entry of 0x0FFFFFF8 or more ends a chain; a free, reserved or bad entry inside one, or one past the volume, is `EIO`. `chain_at` bounds its walk to the Nth cluster by the cluster count, so a loop ends in `EIO`, and answers `ENXIO` for a chain that ends early, which a directory reads as its end and a file as damage. A node keeps the cluster its last walk reached, so a sequential read or an append does not walk from the start.

### Directories and names

`dir_scan` reads a directory as 32-byte entries, one file at a time. A long name is used when its entries form one sequence (the first flagged 0x40 with an ordinal from 1 to 20, the rest counting down to 1, each with the same checksum and a zero type byte and cluster field), the checksum matches the short entry after them, and the name converts to at most 255 bytes of UTF-8. Otherwise the short name is shown, each part lower-cased when its case bit in byte 12 is set, a leading 0x05 read as 0xE5, and a byte above 0x7F shown as `?`. Deleted entries, volume labels and long entries with no short entry are skipped. The position `getdents64` reports after an entry is the index of the next 32-byte entry. Lookup matches the long name or the short name, folding ASCII case.

A new name is checked and converted by `fat_name_units` and stored by the rules of `fat32name.h`. A name that fits 8.3, each part all upper or all lower case, is a short entry alone, its lower-case parts recorded in the case bits, as Windows NT and mtools store it. Any other name is long entries of 13 UTF-16 units each in front of a short entry with an alias of this driver's own making: up to six characters of the base, `~` and the smallest number from 1 to 999999 that no short entry of the directory has, and up to three of the last extension, with lower case raised and a byte a short name cannot have written as `_`. `long name.txt` becomes `LONGNA~1.TXT`, then `LONGNA~2.TXT`, as mtools numbers them; `Mixed.c` becomes `MIXED~1.C` and `.hidden` becomes `_~1.HID`. A name with no free number answers `ENOSPC`.

### Nodes, times and attributes

A node (`struct fat_node`) comes from the kernel heap when a lookup or a create first needs it, and for the root at mount; it is on its mount's list and is freed at its last put. A directory is identified by its first cluster, so two lookups of it share a node; a file by its directory's first cluster and the index of its short entry. A rename moves a held node to the new entry, so a description and the advisory locks, which are found by the node ([lock.Readme.md](lock.Readme.md)), stay with the file. `fat_path` rebuilds a directory's path through each `..`, for a directory descriptor's `*at` calls, which then follow a rename, and for the path `chdir` records.

A create sets the creation, modification and access stamps from the clock, the creation time with its hundredths. A write and a truncate stamp the modification time; nothing else changes a stamp but `setattr`, which stores the access date, the modification time and the write bits, the last as the read-only attribute. A new file has the archive attribute; the hidden and system attributes are kept and have no effect.

### Writing

Every change to the volume runs under the mount's namespace lock. Create, unlink, mkdir, rmdir and rename arrive with it from the VFS; write, truncate, setattr, fsync and sync take it, as do the last put of an unlinked file and the first free count of `statfs`. `assert_locked` panics on a change made without it. Reads, lookups and listings take no lock, since the type record leaves `ns_serialised` at 0.

Metadata is written through and data written back, by the buffer cache's rules ([vfs.Readme.md](vfs.Readme.md)): FAT entries, directory entries, the zeros of a new directory cluster, the dirty flag and FSInfo go through `bcache_write`, and a file's data and the zero fill of its new clusters are left dirty. The FAT entries of a run allocated or freed together are set dirty and written one FAT block at a time in each copy (`struct fat_batch`), the whole run before anything points to it.

`bcache_write` returns when the device has answered. An ATA, AHCI or NVMe disk answers once the write is on the medium; a USB stick answers from a cache of its own ([block Readme](../block/Readme.md)). So where a write refers to what an earlier one made, the driver calls `block_flush` between them, which has the device put its cache on the medium, or `sync_disk`, which writes back the device's dirty buffers and then flushes. Every `write` syncs the device before it rewrites the entry, so a `write` that returns has its data on the medium.

The order keeps one rule: a crash may leak clusters, but never leaves two chains sharing a cluster, an entry pointing to a free cluster, a chain shorter than its size, or a size covering bytes neither written nor zeroed.

### The order of writes

* Create: the directory grows first when it has no run of free entries for the name. The long entries are written highest ordinal first and the short entry last, with no cluster and size zero.
* Write: new clusters are allocated as a run of their own, chained, ended, zero-filled and joined to nothing. The bytes go into the old clusters and the run, a gap past the old size zeroed. The device is synced; the old tail's FAT entry is pointed at the run and flushed; the entry with the new size is written last. A crash before it leaves a chain longer than its size, a surplus tail. A file with no cluster gets its first cluster and size in the one entry write.
* Truncate to a smaller size: the entry with the new size first, its first cluster zero when the size is; a flush; the last cluster kept marked as the end; the cut tail freed last. To a larger size: a write of zeros, in the same order.
* Unlink: the short entry is freed, then its long entries from the nearest back, then the chain, unless the file is open; then the chain is freed at the node's last put, and a crash in between leaks it.
* Mkdir: one cluster is allocated, ended and zeroed, and its `.` and `..` written; the parent grows if it must; the device is synced; the parent's entries are written last. A crash before them leaks the cluster.
* Rmdir: the directory must have only `.` and `..` (`ENOTEMPTY`) and must not be open or a working directory (`EBUSY`). Its entries are freed and flushed before its chain is freed.
* Directory growth: one cluster is allocated, ended and zeroed, the device synced, and the old tail's FAT entry pointed at it.
* Rename: in one directory, with nothing under the new name and as many long entries as before, the entries are rewritten in place, long ones first. Otherwise the new name, its alias and its room are decided first, with the destination still there, so that `ENOSPC` destroys nothing; an existing destination is freed as an unlink frees it; the source's entries are freed and flushed; a moved directory's `..` is rewritten and flushed; the new entries are written last. A crash after the source is freed and before the new entries are written leaves a chain no name reaches.
* `free_chain` begins with a flush, so that what detached the chain (a smaller size, a freed entry or an ended tail) is on the medium before any of its clusters is free.

A write that fails after joining its run ends the old tail again and frees the run, or leaves the run as lost clusters when that fails too. A failed write makes every mount on the device read-only ([vfs.Readme.md](vfs.Readme.md)).

### The dirty flag, FSInfo and allocation

The first metadata write of a mount sets the dirty flag: bit 27 of FAT entry 1 cleared, in each FAT the driver writes, then bit 0 of byte 65 of the boot sector set. A volume is dirty when either says so. Only a clean unmount clears the flag, after the device is synced and FSInfo written; a failure there sets it again and leaves the mount in place. For a mount that has gone read-only, `fat_prepare_unmount` writes nothing and leaves the volume marked.

FSInfo gets the driver's free count and hint at `sync` while the flag is set, at every writable unmount, and after the dirty-volume check. Allocation is first-fit from the hint, wrapping once; the hint moves past each cluster taken and down to a lower one freed. `ENOSPC` is decided before any FAT entry is written, and a run that cannot be finished is freed.

### The check of a dirty volume

`fat_check_volume` runs before a writable mount of a dirty volume is published. It takes the walk's bitmap, or mounts read-only when memory is short, then walks the root's chain and every directory depth first, on a stack of its own, reading the active FAT, the copy written first, and marking each cluster a chain reaches. Four faults are what the order of writes leaves after a crash:

* Orphan long entries, with no short entry after them or one whose checksum differs, are freed as the walk finds them, the last first.
* A surplus tail is recorded and, once the whole tree has been walked, cut off: the size's last cluster is marked as the end and the tail freed. The walk finishes first because a tail that another chain enters is a cross-link, which no repair touches.
* A mirror FAT that differs from the active one is rewritten from it, last, after FSInfo has been given the free count the check made.
* Lost clusters, used and reached by nothing, are counted in the record and left, since one may be the only copy of a file a rename was moving.

Anything else is a state this driver does not leave: a cluster reached twice; a chain that leaves the volume, ends in a free or bad entry or is shorter than its size; a size with no chain; a directory with no cluster, or a wrong `.` or `..`; a short name twice in one directory; or a tree past the bounds `WALK_DEPTH`, `WALK_TAILS` and `WALK_DIR_ENTRIES`, which mount(2) gives. The volume then mounts read-only, with the fault in the record. A volume that passes stays marked dirty until its clean unmount.

### What the FAT drivers share

`fatcommon.c` has what the FAT32 and exFAT drivers both need and neither owns; what is on the disk stays with each driver, and each function's contract is in `fatcommon.h`.

* Names: `fat_name_units` checks a name and converts it from UTF-8 to UTF-16, and `fat_units_utf8` converts back. A name FAT cannot store is `EINVAL`: `.` or `..`, malformed UTF-8, a control character or one of `" * / : < > ? \ |`, or a trailing dot or space. A name past 255 UTF-16 units or 255 bytes is `ENAMETOOLONG`.
* Mount options: `fat_parse_options` takes `umask=` in octal and `uid=` and `gid=` in decimal, the options of Linux's vfat, which BusyBox's `mount` passes through; without them the values are 022, 0 and 0. It accepts `ro` and ignores it, since a read-only mount is the `MS_RDONLY` flag.
* Timestamps: `fat_epoch`, `fat_stamp` and `fat_now` convert between seconds and DOS dates and times, which FAT records in local time with no zone and the kernel reads as UTC. A stamp `fat_stamp` or `fat_now` makes is floored at 1980-01-01 and capped at 2106-02-07 06:28:15 UTC, the last second an unsigned 32-bit count reaches, since the kernel has no 64-bit division.
* Inode numbers: `fat_inode` gives a directory its first cluster, and a file its directory's first cluster plus one in the high 32 bits and its entry index plus one in the low, the root counting as cluster 0, so that no two entries share a number.
* The allocation hint: `fat_hint_start` and `fat_hint_next` keep a first-fit search on the clusters of the volume, wrapping past the last to cluster 2.
* The walk's bitmap: one bitmap with a bit for each cluster, taken by `fat_walk_acquire` and given back by `fat_walk_release`, serves the dirty-volume checks of FAT32, exFAT and ext2, one at a time; a second check waits, and a signal does not end the wait. The bitmap is sized to the volume in whole pages from the heap, and refused with `ENOMEM` when it would leave fewer free frames than the reserve, so that a check does not starve the cache and the processes beside it; the caller then mounts read-only.

## Limits

* A volume is at most 2 TiB, the reach of a 32-bit sector count, and a file at most 4 GiB less one byte; a write or truncate past that is `EFBIG`.
* A name is at most 255 bytes, the kernel's limit for a component, where FAT allows 255 UTF-16 units; a longer long name on the volume, or one that is not valid UTF-16, is listed and opened by its short name. Case is folded for ASCII letters alone, and a rename that changes only the case of a name succeeds and changes nothing. The alias rule is not claimed to match Windows.
* There are no hard links, symbolic links, device nodes, FIFOs or sockets. Every entry has the mount's owner, group and mode, less the write bits for the read-only attribute.
* A directory's times do not change when entries are added or removed.
* A read-only mount of a dirty volume is not checked, and a lost cluster or a fault the check does not repair is left for a host tool.
* `fat_path` gives no path for a directory more than 31 levels below the root, so a descriptor of one fails its `*at` calls with `ENAMETOOLONG`.

## Tests

* `tests/run host` runs the host tests ([tests/host/Readme.md](../../../tests/host/Readme.md)) and replays the corpus of `tests/fuzz/fat32_fuzz.c` ([tests/fuzz/Readme.md](../../../tests/fuzz/Readme.md)). `fatname_hosttest.c` tests the name rules against cases recorded from mtools. `fatcheck_hosttest.c` tests `mkfatfixture --check`, the host's walk by which the other tests and the suites judge a volume, against every fixture `tests/host/mkfatfixture.c` builds. `fat32_hosttest.c` runs the driver over images through a device model that keeps writes pending until a flush (`tests/host/compat/devmodel.c`), and judges with the walk every state a crash could leave after each write and at each failed barrier.
* `sh tests/host/fat_barriers_mutate.sh` removes each `sync_disk` and `block_flush` call of `fat32.c` and `exfat.c` in turn and requires the host test's `--barriers` rows to fail; the flush in `fat_rmdir` is unproved by construction, since `free_chain` flushes next. `sh tests/host/fat_synced_mutate.sh` proves the rows on an unlink and a truncate after an `fsync`.
* `tests/run fatwrite` and `tests/run mount`, in the core tier, and `tests/run loopdev` boot the driver on volumes mtools wrote, as [tests/system/suites.md](../../../tests/system/suites.md) lists: written from a program and from BusyBox, read back after a reboot, killed in the middle of a write and checked at the next mount, written by two processes under the lag device, and mounted through a loop device.
