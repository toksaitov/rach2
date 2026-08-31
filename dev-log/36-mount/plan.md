# Milestone 36: mount

## The goal

The kernel gets a file-system interface: one mount table, one path resolver, and one set of operations that every file system implements, so that a second file system can be mounted beside the root and unmounted again. Under it goes the ordinary block path that every later milestone builds on: a disk request that is allowed to take time, and a fixed buffer cache. On top of it goes the first file system that uses that path, FAT32, read-only, mounted from a partition of an AHCI disk.

The milestone is complete when `mount -t vfat /dev/disk0p2 /mnt` works from the BusyBox shell in QEMU and on the Lenovo G50-70, the mounted volume can be listed and read, `umount /mnt` refuses while a file on it is open and succeeds afterwards, and every suite that passed before the milestone still passes. Writing to FAT32, exFAT, USB storage, hubs and ext2 are the next four milestones and are not started here. The interfaces fixed here are the ones those four build on, so the plan spends its care on them rather than on the FAT driver.

This plan is the authority for the run. A conflict between this plan and the current tree is resolved by the decisions here; a conflict between this plan and `CLAUDE.md` is resolved by `CLAUDE.md`. The plan was revised twice after independent review. The first revision corrected the request lifetime, the descriptor model, the mount identity, the FAT volume check and the proof of several tests. The second made `..` physical, gave directory descriptors and working directories a held node, added the reservation of a mount target and the quarantine of a timed-out request, split the unmount into a retryable and an irreversible half, and moved the buffer cache's write rules to the milestone that first writes.

## What the tree settles before this plan

These facts were read from the code before the plan was written, and the plan depends on them.

* The KRFFS root is not read through a driver. It is mapped into a window at `0xD0000000`, at most 512 MiB and shortened at boot to stop below the lowest device BAR above that base, and faulted in a page at a time. The fault handler runs with interrupts off and polls the disk. That arrangement stays as it is. Nothing in this milestone changes how the root is reached.
* `struct block_device` promises that `read` and `write` are synchronous and polled and may be called with interrupts off. ATA and AHCI keep that promise. The USB stack cannot make it, and milestone 38 needs a device that sleeps the caller.
* The kernel does not preempt a process while it is inside a system call: the timer's reschedule check refuses when `in_syscall` is set. Kernel code runs to completion between the points where it chooses to sleep. Mutual exclusion inside the kernel is therefore the absence of a sleep, which is how the pipe and terminal code already works. Today no file-system operation sleeps. With the lag device below, a read on a mounted volume will, in this milestone; after milestone 38 a lookup, a read or a sync on a USB volume will, and every rule below is written for both.
* `syscall.c` is 11,000 lines with about 160 direct calls into KRFFS and 63 into devfs and procfs. Every path is canonicalised to an absolute form first; `synth_classify` then decides between KRFFS and the two synthetic namespaces by prefix, and each syscall dispatches on that kind by hand. `krffs_resolve_x` walks a whole absolute path and charges a search-permission predicate on every directory it enters. A descriptor on a regular file holds a `krffs_node_t *` in its `fd_entry`; the shared offset and status flags live in the open-file description, `struct open_file`, which `dup` and `fork` share by reference count. Because KRFFS relocates a file that grows past its extent, the write path repoints every descriptor entry in every process from the old pointer to the new one. An orphaned KRFFS node is freed by its last holder, found by scanning descriptor entries. KRFFS directory nodes hold no data and are never relocated, and KRFFS refuses to rename a directory.
* A directory descriptor holds its canonical absolute path, and `openat` and the other `*at` calls resolve against that path. A process's working directory is a canonical path string. `..` is removed lexically before any lookup, so `/x/../proc` reaches `/proc` whether or not `/x` exists. `mmap` accepts anonymous mappings only, so no file is ever mapped into a process.
* `/proc/mounts` is three fixed lines, and `procprobe` checks that there are exactly three. `statfs` answers for the three mounts by classified kind. `ofdprobe` checks the shared-description rules on KRFFS files.
* BusyBox `mount` without `-t` reads `/proc/filesystems` and tries every file system that is not marked `nodev`. BusyBox `umount` reads `/proc/mounts` through `getmntent`. `CONFIG_FEATURE_MTAB_SUPPORT` is off, so neither touches `/etc/mtab`. When `mount(2)` fails with `EROFS` or `EACCES`, BusyBox retries read-only and says so. musl's `umount` is `umount2` with flags zero. The i386 numbers are 21 for `mount` and 52 for `umount2`.
* The disk nodes are `/dev/disk0` (whole disk, published with `rach2.diskdiag` or in the recovery build), `/dev/bootpart` (MBR entry 0) and `/dev/grubcore` (the post-MBR gap). They are raw windows over `block_partition` views, with one write-capable descriptor allowed across the three. The ordinary kernel has the read-write driver policy and refuses whole-disk writes at `/dev/disk0`; the metal kernel's driver policy is read-only.
* `tests/host/mkfatfixture.c` writes FAT32 volumes from the specification, with long-name entries, `.` and `..`, subdirectories, and fourteen malformed variants. `utils/bootslot.c` reads FAT32 in user space with field-by-field checks and decides that a volume is FAT32 by its FAT-size fields, not by its cluster count, because `mformat` and the sealed boot partition are both below the specification's threshold. mtools is a required host tool; dosfstools is not installed.
* The sealed disk on the Lenovo has a 64 MiB FAT32 boot partition first, the 500 MiB KRFFS root second, and a 64 MiB FAT32 recovery partition third. Two FAT32 volumes to mount already exist on the machine. Installing a kernel there writes the boot partition, through `bootctl`, and is the one write this milestone makes on the machine.
* Kernel code is C89 on both compilers and links without libgcc, so a 64-bit division or remainder would fail the link. None is added: sector arithmetic on 64-bit values uses shifts and adds.

## Decisions on the file-system interface

### The mount table

`sys/fs/vfs.c` owns a fixed table of eight mounts. Entry zero is the root. Each entry records its type, the block device it was mounted from when it has one, the mount flags, its state, the parent mount and the held node of the directory it covers, the canonical path of that directory for `/proc/mounts` and `getcwd`, a reference count, a count of operations in progress, and a private pointer for the file system.

The covered directory is held through the parent file system's `hold`, so a mount point is a node and not a name: it cannot be reached under another spelling, and it is the node the resolver climbs back to for `..` at a mount root. A directory that is a mount point may not be unlinked, removed or renamed while the mount exists; each answers `EBUSY`. A mount point's canonical path is at most 63 bytes, and a longer target answers `ENAMETOOLONG` before anything is claimed.

### The resolver

Every path-taking syscall goes through one function. It takes a starting point, which is the root mount's root for an absolute path, the caller's working directory for a relative one, or the held node of a directory descriptor for an `*at` call; splits the path into components, dropping empty ones and `.`; and walks the components one at a time, each step being a lookup of one name in one directory on one mount.

Three things happen at each step. A name is looked up through the mount's `lookup`, after the search permission on the directory has been checked against the caller's credentials from the attributes the file system reports; the walk lives in the common layer so that permission checking exists once. A directory that is the covered node of a mount is replaced by that mount's root before the walk continues. And `..` is physical: at a mount root it goes to the parent mount's covered node and continues there, at the root of the root mount it stays, and anywhere else it is a `lookup` of `..`, which KRFFS answers from `parent_id` and FAT32 from the `..` entry. A path that names a missing or unsearchable directory before its `..` therefore fails, as it does on Linux, where today it succeeds; that is a change in what a program can see, it is the correct one, and any test that relied on the old answer is corrected rather than kept.

`synth_classify`'s protection of the synthetic namespaces does not depend on the lexical step: a mount is decided by the node the walk arrives at, not by the spelling of the path, so `/x/../proc/mounts` reaches `/proc` through `/x` if `/x` is a searchable directory and fails otherwise, and neither answer is a bypass.

The resolver caches nothing. There is no name cache in this milestone; every syscall resolves from its path, as today. The buffer cache under the block path is the only cache added.

### The type and the operations

`struct vfs_type` is one file-system kind: its name as `mount(2)` and `/proc/filesystems` spell it, a `mount` constructor, and its operations. The constructor takes the block device, the flags and the option string, validates the volume, parses the options, sets up the mount's private state and its root, and returns; a constructor that fails leaves nothing behind. The table of types holds four entries and one is filled here, `vfat`.

`struct vfs_ops`, one per type:

* `lookup(mnt, dir, name, &node)`: one name in one directory, `..` included. `dir` of `NULL` means the mount root.
* `hold(mnt, node)` and `put(mnt, node)`: a node handle is held by whoever got it from `lookup`, by every open-file description that refers to it, by every working directory that is it, by a mount entry that covers it, and by nothing else; the last `put` lets the file system forget it. For KRFFS both are empty, because a KRFFS node is a place in the mapped image. For FAT32 a node is an entry in a fixed per-mount pool of thirty-two, freed at the last `put`; a lookup that finds the pool full answers `ENOMEM`, and the Readme says what fills it, which is thirty-two files open at once on one volume. A directory's identity is its first cluster, which is what a `..` entry gives and which is unique on a volume; a file's is its directory's cluster, its entry index, and a generation number the pool advances when it hands that entry out again, so that in milestone 37 a name unlinked while open and then reused does not alias the open file. An unlinked node stays held until its last `put` and is found by no lookup. That pool is the whole of the in-memory inode table this milestone has, and it is per file system rather than in the VFS.
* `getattr(mnt, node, &st)` and `setattr(mnt, node, what, &st)`: what `stat` reports and what `chmod`, `chown` and `utimensat` change.
* `open(mnt, node, flags, &handle)` and `release(mnt, handle)`: once per successful `open(2)` and once at the last close of the description that came from it. The handle is the file system's per-open object; a file system that needs none returns its node. Every type counts its opens and releases, and the record written at unmount says both numbers, which is how a test sees that they match.
* `read`, `write`, `truncate`, `fsync`, `readdir`: on a handle, at an offset the caller supplies.
* `create`, `unlink`, `mkdir`, `rmdir`, `rename`, `link`, `readlink`: the namespace. `rename` is within one mount; the common layer answers `EXDEV` across mounts. `link` answers `EPERM` where the file system has no links.
* `statfs` and `sync`: the whole mount.
* `prepare_unmount(mnt)` and `unmount(mnt)`: the two halves of an unmount. The first writes the volume's last metadata into cache buffers and dismantles nothing, so that it can be repeated or abandoned; the second frees the private state and is not undone. The buffer cache is written between them, and a write that fails returns the mount to `active` with nothing lost, because nothing irreversible has happened yet.

An operation a file system does not implement is a `NULL` pointer, and the common layer answers the standard error for it: `EROFS` for a write-side operation on a read-only mount, `EPERM` for `link`, `EINVAL` for `readlink` on something that is not a link. That contract is written in `vfs.h`, and milestones 37 and 40 fill it in rather than change it.

Credentials, permission policy, the pathname buffers, descriptor sharing, `O_APPEND`, `O_NONBLOCK`, `FD_CLOEXEC`, the mount flags and the cross-mount checks stay in the common layer. A file system sees a node, a name, a handle, an offset and a buffer.

### Descriptions, directories, and the working directory

The open-file description, `struct open_file`, gains the mount, the held node and the file system's open handle. A descriptor entry keeps its own flags and its pointer to the description, as it does now, and pipes, terminals and sockets keep their `file_ops` vtable unchanged. `FD_TYPE_FILE` means a file on a mounted file system, and its read, write, seek, stat, `getdents64`, truncate and `fsync` go to the mount's operations with the description's handle.

The file system's `open` runs once per successful `open(2)` and its `release` once, when the description's reference count reaches zero, which is the last close in whichever process holds it. `dup`, `dup2`, `dup3`, `F_DUPFD` and `fork` share the description and do not open the file again. A description never re-resolves its path: a file unlinked after opening still reads, and an unmount cannot move a description onto another file system.

A directory descriptor is a description too, holding its mount and node, and an `*at` call starts its walk at that node. The canonical path is kept beside it for `/proc/<pid>/fd`, and for nothing else. A process's working directory is likewise a held mount and node with its canonical path beside it for `getcwd`; `chdir` resolves, holds the new node, releases the old, and rewrites the string from the resolution. Both are the reason the second review overturned the first draft: with a held node, a directory renamed under an open descriptor stays the directory that was opened, and a directory opened before a mount is placed on it stays the covered directory, which is what Linux does and what a path string cannot do.

A mount's reference count is the number of descriptions on it, plus the number of working directories in it, plus the number of mounts whose covered directory is on it; `chdir` moves a reference from one mount to another, and a child mount takes its parent's reference when its slot is reserved and gives it back when it fails or is unmounted, so a parent cannot be unmounted from under a child.

A description has a busy flag and a wait queue. `read`, `write` and `lseek` on a description hold it for the length of the operation, so two processes sharing one description through `dup` or `fork` cannot both read the same offset while one sleeps in a request; the second sleeps on the description and continues where the first left the offset. Today that costs a flag; with the lag device it is what keeps `cat` from repeating a block.

KRFFS's relocation repointing becomes a VFS helper that scans the descriptions, working directories and mount entries of one mount, and the orphan rule that frees a nameless node at its last holder counts descriptions the same way. Both behave as they do today, and `ofdprobe` is the check that says so.

### Mount states and locks

A mount is `mounting`, `active` or `unmounting`. Every VFS entry point takes a transient count on the mount for the length of the operation and releases it on return, so a process asleep in a lookup or a read on that mount, with no description yet, is still counted. The unmount path itself, with the `sync` and `prepare_unmount` it calls, is the one path that does not take the count, since it is the path that waits for the count to reach zero.

`mount(2)` takes a table slot in the `mounting` state, with the parent mount and the covered node recorded and held, before the constructor runs, because with the lag device the constructor sleeps and another process could remove or rename the target under it. While a slot is `mounting`, the covered directory answers `EBUSY` to `unlink`, `rmdir` and `rename`, and a second `mount(2)` on the same target answers `EBUSY`. A constructor that fails releases the covered node, the claim and the slot on every path.

`umount2` moves the mount to `unmounting` before anything that can sleep, which refuses new descriptions, new working directories and new operations with `EBUSY`, waits for the in-progress count to reach zero, then checks the reference count. On `EBUSY` or on a failed writeback the mount returns to `active`.

Each mount has one namespace lock, taken by `create`, `unlink`, `mkdir`, `rmdir`, `rename` and by the constructor and the unmount, so that two namespace changes on one volume do not interleave across a sleep. The order in which a process may take things is fixed and checked: the mount table, then a mount's namespace lock, then a description, then buffers in the order the cache section gives. A process holding one may take only what comes after it. Today nothing takes two at once across a sleep; milestone 37's writes are the first that do, and the order is written now so that they inherit it rather than invent it.

### The two synthetic mounts

`/dev` and `/proc` become entries two and three of the table, with a `vfs_type` each. They are the one exception to component lookup: their type sets `lookup_rest` instead of `lookup`, and the resolver hands them the remainder of the path in one call, which is what `synth_classify` already does, with `..` inside that remainder resolved lexically by the type because its namespace is flat and a `..` that would climb above the mount root is returned to the resolver, which continues at the parent's covered node. Their nodes are value handles, the packed classification, with empty `hold` and `put`. A `lookup_rest` type may not host a mount point: `mount(2)` with a target on one answers `EINVAL`, because the resolver cannot stop inside a remainder to cross a mount. The synthetic namespaces are flat tables classified by string; rewriting them as trees is not in this milestone's risk budget, and the shortcut is a documented private property of those two types that a disk file system may not use. The two are permanent: `umount2` on either answers `EBUSY`, and so does `umount2` on the root.

`synth_classify` and the `VNODE_` kinds become the private vocabulary of those two types and leave the syscall layer.

### mount and umount2

`mount(source, target, type, flags, data)` requires effective uid 0. `type` names a type in the table. `source` must be a device node under `/dev` that is a partition window. `target` must be an existing directory on a mount that permits mount points, not already a mount point or reserved by one, and not `/`. The order of work is: resolve and hold the target; check its path length; claim the source's sector range; take the slot as `mounting`; run the constructor; publish the entry as `active`. Every failure after the first step releases what the steps before it took.

`MS_RDONLY` is honoured and recorded. `MS_NOEXEC` and `MS_NOSUID` are honoured and recorded: `execve` refuses a program on a `noexec` mount with `EACCES`, and the set-id bits of a program on a `nosuid` mount are ignored, both in the common layer where the checks already are. `MS_NODEV` is recorded and enforced where a device node could exist, which is on no mountable file system in this milestone; milestone 41 enforces it for ext2. `MS_NOATIME`, `MS_NODIRATIME`, `MS_RELATIME` and `MS_SILENT` are accepted and recorded, because no mountable file system here updates access times. `MS_REMOUNT`, `MS_BIND`, `MS_MOVE` and any other flag answer `EINVAL`. `data` is the option string the type parses; an option it does not know answers `EINVAL`.

`umount2(target, flags)` requires effective uid 0. `flags` must be zero; `MNT_FORCE`, `MNT_DETACH` and `UMOUNT_NOFOLLOW` answer `EINVAL`, because a lazy or forced unmount is a promise this kernel cannot keep. `target` must be a mount point, or the call answers `EINVAL`. On success the order is: the mount goes to `unmounting`; the in-progress count drains; the reference count is checked; `sync`; `prepare_unmount`; the buffer cache writes the device's dirty buffers, and a failure here returns the mount to `active` with `-EIO`; the cache drops the device's buffers; `unmount`; the covered node is released; the claim is released; the table entry clears.

The 22 `umount` syscall is not implemented; musl does not use it.

### /proc/mounts and /proc/filesystems

`/proc/mounts` is generated from the table, one line per `active` entry in mount order, in the six-field form `getmntent` reads: source, mount point, type, options, and two zeros. The three lines that exist today keep their exact text, so `procprobe` and `procfiles` pass unchanged until a fourth mount appears.

`/proc/filesystems` is new: `nodev` lines for `devfs` and `proc`, and a plain line for every type `mount(2)` accepts, which is `vfat` in this milestone. KRFFS is not listed, because the root is not mounted through `mount(2)` and cannot be mounted through it while it lives in the window.

### Partition nodes and claims

`/dev/disk0p1` through `/dev/disk0p4` are published for every non-empty MBR entry whenever a disk answers, the way `/dev/bootpart` is published for entry zero. They are read-only raw windows, mode 0400: a write answers `EPERM` in every build, because the writable windows already exist under their own names and a whole-disk write belongs to recovery. `mount(2)` takes its source from one of them, or from `/dev/bootpart`, which is the same sectors under another name.

A claim is a sector range on the backing disk: the ultimate device under every partition view, a first sector and a last sector. Three names can describe one range, so claims are compared as ranges and never as device pointers. `mount(2)` claims its range before the first sector is read and releases it on every failing path. While a range is claimed: a second mount overlapping it answers `EBUSY`, whatever its flags, so milestone 37 cannot make two writable mounts over one volume; a write-capable open of a raw window overlapping it answers `EBUSY`; and while such a descriptor is open, `mount(2)` on the range answers `EBUSY`. The claim table sits beside the single-writer rule in devfs and the two are checked together.

## Decisions on the block path

### One interface, one flag, one request

`struct block_device` gains a `flags` word, a `deadline_ticks`, and an optional `submit`. `BLOCK_FAULT_SAFE` says the device completes a transfer by polling and may be used with interrupts off; ATA, AHCI and every partition view over them set it, and such a device keeps its `read` and `write`. A device without the flag provides `submit` instead: it starts a request and returns, and later calls `block_complete(request, generation, status)` from whatever context it completes in. The disk root mount in `kernel.c` refuses a backing device without the flag, with a record that says why, so the rule that the window may not sit on a sleeping device is enforced where the backing is installed.

`struct block_request` is the one object a sleeping device sees: the device, the direction, the first sector, the count, its own page, a status, a state, a wait queue, and a generation. Requests come from a fixed pool of sixteen, one per process. The page is the request's, not the caller's: a write is copied into the page before `submit`, and a read is copied out to the caller after a successful completion. A request is at most one page, eight sectors; `block_request` splits a larger transfer into requests of that size, one in flight at a time, so a raw window's 64-sector batch is eight requests on a sleeping device and one call on a polled one. The generation is captured by the device at `submit` and handed back at `block_complete`; a completion whose generation is not the request's current one is dropped with a record.

A request that reaches its deadline is not freed. It goes to a `quarantined` state, its page and slot stay out of the pool, and the caller gets `-EIO`; the slot returns to the pool only when the device completes it, late, or when the device is reset and says so. That is what keeps a late transfer from writing into a page that a newer request now owns, and it is why the generation is not enough on its own. A caller that finds no free slot, because too many are quarantined, gets `-EIO` at once and a record that says how many are held; it does not wait.

`block_request(dev, write, lba, count, buffer)` is the one entry every file system and every raw window uses from now on. On a fault-safe device it calls the vtable and returns, from any context, at boot included. On a device with `submit` it takes a request, submits it, sleeps the calling process until the completion or the device's deadline, copies, and returns the result. The check that panics on interrupts off is made only on that sleeping path. The deadline is the device's, with a default of five seconds, because milestone 38's drive may need longer for a spin-up than a lag device needs for anything.

No device with `submit` exists after this milestone except the test device below. The sleep, the wake, the deadline, the quarantine, the generation and the copy in both directions are written and proved here, reads through the partition nodes and a mounted volume and writes through `/dev/bootpart` on the suite's disk, so that milestone 38's USB disk is the second user of that path and not the first.

### The buffer cache

`sys/fs/bcache.c`: 64 buffers of 4096 bytes, static, keyed by device and block number, where a block is eight sectors; the last block of a device whose sector count is not a multiple of eight is read and written as the sectors that exist. The operations are the ones in Lions' commentary: `bcache_get` finds or reads a block and returns it held, `bcache_release` lets it go, `bcache_dirty` marks it for writing, `bcache_sync` writes every dirty buffer of one device, and `bcache_invalidate` drops a device's buffers at unmount and refuses while any is held. Replacement is least recently released.

This milestone uses the cache to read. `bcache_dirty` and `bcache_sync` exist and are called by nothing, and the rules for a dirty victim, for a write that fails, for a cache full of unwritable buffers and for the order of a sync are written and proved in milestone 37 against the first file system that writes, where an end-to-end check can see them. Writing them here would prove them against a fake and prove them again there.

A held buffer is busy. Because the kernel does not preempt inside a syscall, a second process reaches a busy buffer only while the first is asleep in a request, so a process asking for a busy buffer sleeps on the buffer's wait queue and is woken at release. To make that free of deadlock when operations sleep, a process may hold at most four buffers at once, and while it holds any it may acquire only a higher block number on the same device or any block on a device it holds nothing on; `bcache_get` checks both and panics on a violation, so a file system that breaks the rule learns in the host test rather than on a machine. Two processes wanting the same cold block of a mounted volume, with the lag device underneath, is the guest test of the busy path, observed through counters in `/proc/disk`.

### The lag device

`rach2.blocklag=N` wraps the boot disk in a test device with `submit` and no fault-safe flag: every request is queued and completed from the timer N ticks later, and the caller sleeps in between. The partition nodes, `/dev/bootpart` and the mounts use it; the root window keeps the real device underneath, so a machine whose root is on the disk boots as usual and mounts through the lag. `/dev/bootpart` is on the lag so that a write can be proved through the sleeping path in this milestone, where every mount is read-only: the QEMU suite's disk is disposable, and a sector written through the lag and read back is the proof of the write copy. With N above the deadline it is a device that never answers in time, which is how the quarantine is exercised without a mutation. It exists to prove the sleeping path with a device that behaves and one that does not, before USB exists, and it is a boot option and about a hundred lines.

## Decisions on FAT32

### Read-only, and what that means

`sys/fs/fat32.c` implements the constructor, `lookup`, `hold`, `put`, `getattr`, `open`, `release`, `read`, `readdir`, `statfs`, `sync` and `prepare_unmount` as nothing, and `unmount`. Every write-side operation is `NULL`. A `mount(2)` without `MS_RDONLY` answers `EROFS`, which is the answer a write-protected medium gives on Linux, and BusyBox then retries read-only and prints that it did. Milestone 37 removes that refusal when writing exists.

### The volume check

The BPB is checked field by field before the mount is published, with the rules `bootslot.c` uses and the ones it did not need. A volume is FAT32 when its 16-bit FAT size is zero, its 32-bit FAT size is not, its root entry count is zero, and its FAT version is zero, which is how Linux and `bootslot.c` decide it; the specification's threshold of 65525 clusters is a formatting rule and not a recognition rule, since `mformat` writes smaller FAT32 volumes and the Lenovo's boot partition has 16,384 clusters. Then: sector size 512; sectors per cluster a power of two no larger than 64; reserved sectors at least one; one or two FATs; total sectors within the partition; the reserved and FAT regions subtracted from the total without wrapping; a FAT large enough for the cluster count; a cluster count no larger than the FAT32 maximum; the root cluster within the data region; and the active FAT index from `BPB_ExtFlags` within the FAT count. A volume that fails any check is refused with a record naming the field.

`BPB_ExtFlags` decides which FAT is read: with mirroring on, the first; with mirroring off, the one the flags name. FSInfo and the backup boot sector are not read. A FAT entry is masked to 28 bits before it is judged, and free, bad, reserved and the whole end-of-chain range are told apart; a cluster number outside the data region ends the chain with `EIO`, and a chain is walked with a bound of the cluster count so that a loop ends in `EIO` for that file rather than a hang.

### Directories, names, times, and modes

A directory is read entry by entry: a first byte of zero ends it, `0xE5` marks a deleted entry and is skipped, a volume label is skipped, and a first byte of `0x05` stands for `0xE5` in a short name. A long name is assembled from its entries in order, with the ordinal, the type byte, the zero first-cluster field, the terminator and the padding all checked, and it is kept only when its checksum matches the short entry that follows; otherwise the short name is used. UTF-16 becomes UTF-8, with surrogate pairs joined. A short name is shown in lower case where the reserved byte's case bits say so, as Windows writes them; a short-name byte above `0x7F` is shown as `?`, as Linux's default `iocharset=ascii` does, and such a file is reachable by its long name. Lookup folds ASCII case, as FAT does; a non-ASCII name matches exactly, and the Readme says so. A name that does not fit the kernel's 255-byte component is unreachable and answers `ENAMETOOLONG`. An empty file has first cluster zero and reads as empty. The `..` entry of a directory gives its parent's first cluster, and zero there means the root.

FAT stores local time with no zone; the driver reads it as UTC. Every file reports the mount's `uid`, `gid` and a mode from the mount's `umask` option (0644 for files and 0755 for directories under the default 022), which are the options Linux's vfat takes and BusyBox passes through. There are no owners, links or symlinks on FAT, and `getattr` says so.

`statfs` reports the cluster size as the block size, the cluster count as the block count, and free clusters counted from the active FAT on the first `statfs` call and kept, since a read-only volume cannot change.

### What the driver refuses

FAT12, FAT16, exFAT, sector sizes other than 512, a volume larger than 2 TiB, and any BPB that fails a check. Directories are read at 32-byte entry offsets, which is what `getdents64` reports and what a program hands back.

## Memory and limits

| Limit | Value |
|-------|-------|
| Mounts, including the three permanent ones | 8 |
| Mount point path, as recorded for `/proc/mounts` | 63 bytes |
| File-system types | 4 |
| Block requests, quarantined ones included | 16, one per process, each with its own page |
| Buffer cache | 64 buffers of 4 KiB, 256 KiB static |
| Buffers one process may hold | 4 |
| FAT32 nodes held per mount | 32 |
| Partition nodes | 4, one per MBR entry |
| Claims | 8, one per mount, plus the raw writer |

Everything is a static table, like the rest of the kernel.

## What stays out

Writing to FAT32, and therefore FSInfo, the second FAT, allocation, the dirty-volume rules, and the buffer cache's write rules: milestone 37. exFAT: milestone 37. A device that really sleeps, and device naming that survives enumeration order: milestone 38. Symbolic-link following: milestone 41; the physical `..` it needs is here. `MS_NOSUID` proved: milestone 41, because no file system mountable before it can carry a set-id bit. Mounting KRFFS through `mount(2)`, bind and move mounts, remount, lazy and forced unmount, `/etc/mtab`, `chroot`, `pivot_root`: not planned. A name cache: not until something measures the need.

## Decisions on tests and tooling

* `tests/host/bcache_hosttest.c`: the buffer cache against a fake block device that records every transfer. Hits and misses, the replacement order, a failed read answered with no buffer, invalidation refused while held, the hold limit and the ordering rule refused, a short last block, and a request larger than a buffer refused. The write rows come with milestone 37.
* `tests/host/fat32_hosttest.c`: the driver over the buffer cache over an image file. The volumes are `mkfatfixture`'s, which gains: a fragmented file and a fragmented directory, an empty file with cluster zero, a long name crossing a 4 KiB boundary and a cluster boundary, a long name with a bad checksum that must fall back to its short name, a `0x05` short name, a volume label, a deleted entry, a surrogate pair, a volume with mirroring off whose active FAT is the second and whose first FAT is deliberately wrong, a subdirectory whose `..` is read back to its parent, and a small volume that is valid FAT32 below 65525 clusters. Every malformed variant must be refused at mount or end in `EIO` at the file it damages, under AddressSanitizer.
* `tests/vfsprobe.c`: one row per migrated syscall family, run on the root before any mount exists and again through a mount in phase 5: absolute, relative, working-directory and directory-descriptor forms, repeated slashes, `.`, `..` through an existing directory, `..` through a missing one and through an unsearchable one, `..` at the root, `/procx` beside `/proc`, and the order in which errors are reported. The row for a directory descriptor used after its directory is renamed cannot run before a mountable file system renames directories, and it is milestone 37's, written there. It is written in phase 3 before the first family moves, its rows that the lexical `..` answers differently are marked and flipped in the commit that makes `..` physical, and its rows are run after each family commit.
* `tests/mountprobe.c`: `mount` read-only and its refusal read-write; `stat`, `open`, `read`, `getdents64`, `statfs` and `execve` on the mount; the mount lifetime: a file and a directory opened, duplicated with `dup`, `dup2` and `F_DUPFD`, inherited across `fork`, closed in different processes, one marked close-on-exec and dropped by `execve`, a failed `open` and a failed `dup`, with `umount2` answering `EBUSY` until the last description is gone and the unmount record showing opens equal to releases; `EBUSY` for a working directory inside; `EINVAL` for every refused flag and for a target on `/proc`; `ENAMETOOLONG` for a long target; `EXDEV` for a rename across; `EBUSY` for `rmdir`, `unlink` and `rename` of the mount point; `EACCES` for `execve` on a `noexec` mount of a volume mounted with `umask=000`, so that the mode alone would have allowed it; `EPERM` after `setuid`; and `/proc/mounts` and `/proc/filesystems` read back.
* `tests/lagprobe.c`: with `rach2.blocklag`, a parent reads a raw partition while a child counts timer ticks; the parent's read must take at least the lag and the child must have counted during it, which is what proves the parent slept. A sector written through `/dev/bootpart` on the lag, past the boot slots, reads back through `/dev/disk0p1` and the write copy is proved. With the lag above the deadline, a read answers `EIO` in deadline time, a second read on the same slot afterwards returns the right bytes, and the record shows the late completion dropped. Through a mounted volume: two children read the same cold block at once and the `/proc/disk` counters show one waiting for the busy buffer; two processes sharing one description through `fork` read a file to its end and between them see every byte once; and a child runs `rmdir` on the target while the parent's `mount` sleeps in the constructor and gets `EBUSY`.
* `tests/system/suites/mount.sh`, `tests/run mount`, core tier: the probe boots; a BusyBox boot on os-real that mounts, lists, reads and unmounts through the shell, with the listing compared against `mdir` and every file's bytes compared against `mcopy` on the host, on a volume that mtools wrote and fragmented by writing, deleting and writing again; the `bootctl` conflict answered with `EBUSY`; and the lag probe.
* The suite's disk is `krffs/mkdisk.sh`'s test disk with a second partition added by `tools/image/mkmbr` and formatted and filled with mtools at its offset. The fixture script lives with the suite.
* Every existing suite is the proof that the resolver changed nothing it should not have. `tests/run all` runs after phase 3 and after phase 5, not only at the end.
* `tools/setup/doctor.sh` lists dosfstools as optional, for `fsck.fat` on a host that has it.
* `tests/system/accept/metal_accept.sh` gains the partition nodes, a read-only mount of the boot partition, and the lag check done with the shell: a background loop counting while `dd` reads a partition through the lag.

## Documentation

* `rach2/sys/fs/vfs.Readme.md` is new: the mount table and held covered nodes, the resolver and physical `..`, the type and the operations and their contract, node handles and identities, descriptions and directory descriptors and the working directory, the reference count and the description lock, mount states and the lock order, `mount` and `umount2` and the flags, the two synthetic types and their shortcut, the block request and its quarantine, the buffer cache and its rules, the lag device, the claim table, the limits, what is absent and why, and the verification.
* `rach2/sys/fs/fat32.Readme.md` is new: what is read, the volume check, directories and names, identities, times and modes, what is refused, and the verification.
* `rach2/sys/fs/Readme.md`: KRFFS's operations, `..` from `parent_id`, the walk that moved to the common layer, and repointing through the VFS.
* `rach2/sys/fs/synthfs.Readme.md`: `/proc/mounts` from the table, `/proc/filesystems`, the partition nodes, the `/proc/disk` counters, and the `lookup_rest` shortcut.
* `rach2/sys/block/Readme.md`: the flag, `submit` and `block_complete`, the request object and the quarantine, `block_request`, the lag device, and the claim rule beside the single-writer rule.
* `rach2/Readme.md`: the subsystem table, the boot-option table, the syscall list, the limits, the file systems the kernel mounts, and the change in how `..` resolves.
* `Readme.md`: the system paragraph.
* `vendor/busybox/Readme.md`: `mount` and `umount` and what they read.
* `tests/Readme.md`, `tests/host/Readme.md`, `tests/system/Readme.md`, `tools/Readme.md` where a tool changes, and `krffs/Readme.md` if `mkdisk.sh` changes.
* `dev-log/36-mount/Readme.md` and the `dev-log/Readme.md` entry, in phase 7.

Phase 7 is the sweep: the documents, the code the migration left redundant, the build, the runner and the suites, each read for freshness and for weight, in that order.

## How the work is kept track of

Four files beside this plan, all tracked, all committed at each checkpoint: `progress.md`, every item here as a checklist; `current.md`, the resume state only, replaced and never appended; `notes.md`, deviations, measurements, defects and decisions the plan left to the run; `hardware.md`, what QEMU and the Lenovo report about their disks and partitions.

After every phase, every compaction recovery and every batch of commits, one line is reported: `M36: phase N of 7; X of Y items done; HEAD abc1234; tree clean|dirty`.

## Standing rules for the run

* Work directly. The only delegation is an independent review, one reviewer per unit, at most six files, at most ten minutes. There are six review checkpoints, after phases 2, 3, 4, 5, 6 and 7. Phase 1 lists the files each checkpoint will cover and fixes the number of units for the whole milestone from that list; a checkpoint whose diff exceeds six files is reviewed as several units, and a fix that changes a contract gets one focused re-review of the files it changed. No result of one review starts a review the list does not name.
* Commit in scoped units in the house style, on the branch, no attribution, never push.
* Every commit passes `tests/run style` and `tests/run host`. A commit that touches a subsystem runs that subsystem's suites singly. The whole set runs after phases 3 and 5 and at the end of phase 7.
* A test is proved by making it fail. A mutation is driven from a script that restores the source in a trap and rebuilds before the verdict is believed.
* A change that alters a contract rewrites the owning Readme in the same commit.
* A change that makes something redundant removes it in the same commit: a helper with one caller left, a suite that another now covers, a Make rule a listed rule could serve, a document paragraph the code no longer matches. The tree after this milestone is not larger than the milestone's own additions.
* The one write on the Lenovo is the kernel installation through `bootctl`, presented and approved as `CLAUDE.md` requires. Every mount there is read-only, the two FAT partitions are hashed after the installation and before the checks, and no `bootctl` command that writes runs between the two hashes.
* Nothing is deferred. A thing discovered mid-run goes into `notes.md` with the phase that will do it, and phase 7 closes every line or records the owner's decision to drop it.

## The phases

### Phase 1: arrival and inventory

Read `AGENTS.md`, `CLAUDE.md` and this plan in full. Require a clean tree and record `git log --oneline -12`. Create the four tracking files in the first commit.

Then measure what the refactor will touch: the call sites into KRFFS, devfs and procfs grouped by syscall family, written into `notes.md` as the migration order for phase 3, and the files each review checkpoint will cover, from which the number of review units is fixed. Find every test that depends on the lexical `..` answer and list it for the commit that changes it. Confirm that `tools/image/mkmbr` accepts a second entry and that mtools can format a partition at an offset, by building the suite's disk once by hand. Record what the Lenovo reports about its partitions over SSH into `hardware.md`.

Proof: the tables in `notes.md` and `hardware.md`, and the suite's disk built and listed with `mdir`.

### Phase 2: the block path

The flag, `deadline_ticks`, `submit` and `block_complete`, the request pool with its pages, states and generations, `block_request`, the refusal in the disk root mount, the buffer cache's read half and its hold rules, the lag device, and the partition nodes. The raw windows move onto `block_request`. `bcache_hosttest.c` and the raw-window half of `lagprobe`.

Proof: the host rows pass; `lagprobe` shows the parent asleep while the child counts, a timed-out request answered in deadline time and its slot reused correctly afterwards; a boot with `rach2.blocklag=2 rach2.fsdisk` mounts its root through the real device and reads its partitions through the lag; the partition nodes appear with the sizes the MBR gives; every disk suite passes unchanged.

Mutations: complete a lag request without waking the sleeper, which the deadline must catch; free a timed-out slot at once, proved by submitting a second request through the same slot and delivering the first completion late, with a sentinel in the second request's page; accept a completion with a stale generation; clear the flag on AHCI, which the root mount must refuse; publish a partition node for an empty MBR entry; let a process hold a fifth buffer; let a process take a lower block while holding a higher one. Each must fail exactly its check.

### Phase 3: the interface and the migration

`vfs.h` and `vfs.c` with the three permanent entries, the resolver with physical `..` and mount crossing by held node, the type and operations for KRFFS and the two synthetic types, the mount states and locks, the description lock, and the description, directory and working-directory fields. `vfsprobe` first, run on the tree as it is, so that its rows are known to pass before anything moves, with the rows that change under physical `..` marked. Then the migration of `syscall.c`, one family per commit, in the order `notes.md` recorded: lookup, `stat` and `access`; `open`, `creat`, `openat` and the description fields; `read`, `write`, `lseek`, `getdents64`, `truncate` and `fsync`; `unlink`, `mkdir`, `rmdir`, `chmod`, `chown` and `utimensat`; `rename` and `link`; `statfs`, `sync`, `chdir`, `getcwd` and `execve`. After each family the `vfsprobe` rows for it run. After the last family, `synth_classify` is called from the two synthetic types only, KRFFS's whole-path walker is gone, and the lexical `..` step is gone with the marked rows flipped in that commit.

Proof: `vfsprobe` and every suite pass after each family, the core tier singly and `tests/run all` after the last. `procfiles`, `authority`, `login`, `persist`, `diskfull`, `ofdprobe` and `selfbuild` are the ones that reach the most paths.

Mutations: match `/procx` as `/proc`; skip the search-permission check on one directory level, which `authority` must catch; resolve `..` lexically, which the missing-directory row must catch; re-resolve a description's path on read, which the unlink-then-read check must catch; drop the relocation repointing, which the growth check in `persist` must catch, and if nothing catches it a `vfsprobe` row is added until something does; call the file system's `open` once per descriptor instead of once per description, which the unmount record's counts must catch.

### Phase 4: FAT32

`fat32.c`, `fat32.h`, the `vfat` type, the node pool and its identities, the new `mkfatfixture` cases, and `fat32_hosttest.c` over every volume.

Proof: the well-formed volumes read back exactly what the builder wrote, long names and all, through the fragmented files, across the boundaries and back up through `..`; each malformed volume is refused at mount or fails at the file it damages, and nothing reads past a buffer under AddressSanitizer.

Mutations: refuse a valid FAT32 below 65525 clusters, which the small-volume row must catch; skip the chain bound, which the loop fixture must catch; ignore the long-name checksum, which the bad-checksum fixture must catch; read the first FAT on the volume whose active FAT is the second, which that fixture must catch; accept a root cluster past the data region; take an unmasked FAT entry at face value; answer `..` with the directory itself. Each must fail exactly its row.

### Phase 5: mount

`mount` and `umount2` with their order of work, the flags and their enforcement, `/proc/mounts` from the table, `/proc/filesystems`, the claim table, `mountprobe`, the mounted half of `lagprobe`, `vfsprobe` through a mount, the suite, and the BusyBox boot.

Proof: `tests/run mount` passes; `mount` in the os-real shell lists a volume that `mdir` on the host lists identically and whose files `mcopy` reads identically; `umount` answers `EBUSY` with a file open and succeeds after it is closed; `tests/run all` passes.

Mutations: allow `umount2` with a description open in another process; allow a working directory inside an unmounted mount; accept `MNT_DETACH`; allow `rename` across mounts; allow `bootctl` to open `/dev/bootpart` for writing while the boot partition is mounted; allow a second mount over a claimed range; run a program from a `noexec` mount; let `rmdir` remove a target while a constructor sleeps; let two processes read one description without the lock, which the every-byte-once row must catch; drop the busy-buffer wait, which the counter row must catch. Each must fail exactly its check.

### Phase 6: the machine

The Lenovo, over SSH. Install the kernel through `bootctl` as the approved write, and commit it once it boots. Then hash `/dev/disk0p1` and `/dev/disk0p3`. Mount the boot partition read-only, list it, compare the candidate slot's bytes with the kernel that was installed, mount the recovery partition and list it, and unmount both. Boot once more with `rach2.blocklag=3` on the ordinary root and run the shell lag check against a real AHCI through `/dev/disk0p3`. Hash both partitions again; they must be identical to the first pair. `metal_accept.sh` gains the same checks. Whatever the machine does that QEMU did not goes into `notes.md` and is fixed, not worked around.

Proof: the hashes match, the listing matches `bootctl status`, the lag check shows the counter advancing while `dd` waits, and the machine reboots normally with `bootctl` unaffected.

### Phase 7: the record and the sweep

This phase is as large as a build phase, and it is where the tree is left no heavier than the milestone needed. Milestones 30, 31, 34 and 35 each spent days cleaning what earlier milestones had left behind in the build, the runner, the suites and the documents; this phase exists so that the next milestone does not have to.

The documents: the two new Readmes and every document listed above. Every document and every comment this milestone touched is read against the code it describes, and a Codex read of the reader-facing documents is the independent check. A comment that narrates the body, a header that repeats the Readme, and a Readme paragraph that describes a phase rather than a contract are removed.

The code: everything the migration made redundant is gone, not left for later. `krffs_resolve_x` and its predicate, the lexical `..` step, the per-syscall kind dispatch, and any helper in `syscall.c` that only one file system still needs move to that file system or are deleted. `synth_classify` is reachable from the two synthetic types and nowhere else. `syscall.c` is measured before and after, and the line count after the milestone is smaller than before it, which is the point of a resolver.

The build: every Make rule, manifest line, fixture script and tool added this milestone is listed in `notes.md` with the reason it exists, and one that a listed rule could have served is folded into it. No `build_*.sh` is added. New kernel sources are in the shared manifests and `guestcheck` and `tests/run selfbuild` pass, so the guest build stays whole. `tools/setup/doctor.sh` lists exactly what the build needs and nothing it does not.

The tests: the new suite's tier decision is measured, not guessed. A probe or a boot that proves what another already proves is removed. The harness is unchanged unless a step form was needed, and the runner is unchanged. Every check added here was proved by a mutation, and the mutation scripts are in `notes.md` with the check each one failed. The full suite set runs twice with no machine left running, and `make test` is timed against the milestone 35 figure.

The record: `notes.md` read from the top and every line closed. `dev-log/36-mount/Readme.md` with a trust statement: what a passing run proves about the interface and the block path, what only the Lenovo proved, and what neither could. The `dev-log/Readme.md` entry in the same commit. The tree is clean and nothing is pushed.

## Risks, in expected order

* A syscall that resolves differently after the migration and that no suite reaches. The migration goes one family at a time so that a break is found within one commit, `vfsprobe` has a row per family and runs after each, and `tests/run all` runs twice.
* The change to `..`. A program or a test that relied on `/missing/../x` succeeding breaks. Phase 1 lists such tests; `vfsprobe` marks the rows; the commit that flips them is one commit, reviewed on its own.
* Description lifetime under `dup`, `fork`, close-on-exec, exit and failed opens. `ofdprobe` covers the sharing rules today and `mountprobe` combines them with `umount2`; the unmount record's counts are what say `open` and `release` paired.
* KRFFS relocation. The repointing moves onto descriptions, working directories and mount entries and must still find every one, in every process. The `persist` growth check and the phase 3 mutation are what say it does.
* The claim table and the single-writer rule disagreeing about who may write the boot partition. Both are checked in `mountprobe` and in the `install` suite, which runs `bootctl` and must still pass.
* The busy rules, the quarantine and the description lock need a scheduler to prove. `lagprobe` is the proof, and the shell form of it runs on the Lenovo as well as in QEMU.
* FAT volumes the fixtures do not cover. The os-real boot compares the kernel's listing and file bytes with mtools' on a volume mtools wrote and fragmented, and the Lenovo's boot partition was written by `mformat` and `bootctl`, so three writers' volumes are read before the milestone closes.

## Recovery after a compaction

Read `AGENTS.md`, `CLAUDE.md`, this plan's decisions and the current phase, `progress.md`, `current.md`, `notes.md` and `hardware.md`. Run `git status --short --branch` and `git log --oneline -15`, inspect every uncommitted path, and resume the current phase's first unfinished item. Report the one line.

## The goal prompt

`Run milestone 36, mount, from dev-log/36-mount/plan.md.`
