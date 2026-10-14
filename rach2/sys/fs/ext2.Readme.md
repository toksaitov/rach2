# ext2

`ext2.c` is the ext2 file system: the type `ext2` of `mount(2)` and the root of the installed system, read and written through the buffer cache (`bcache.h`). It exists so that the root on a disk has a standard format with a standard checker: the host's `mke2fs` makes the volumes (`tools/image/mkext2.sh`), and [e2fsck(8)](../../../man/man8/e2fsck.8) repairs them. The driver repairs nothing but free counts. It checks a volume not unmounted cleanly before writing to it, and on a `sync` or `dirsync` mount writes in an order that leaves every stop repairable without losing a file's data. The code is the same on i686 and x86-64.

## Interfaces

`ext2.h` declares one record, `vfs_ext2_type`: the name `ext2`, the constructor `ext2_mount`, the operations, `ns_serialised` set and `nodev` clear (`vfs.h`). `mount(2)` finds it by name, and the boot path mounts an ext2 root with it through `vfs_init_root`, on the partition the boot line selects as [rach2/Readme.md](../../Readme.md) says. Of the VFS operations it leaves out `lookup_rest`, `pin` and `unpin`. The constructor ignores the option string. The driver reads three mount flags: `MS_RDONLY`, and `MS_SYNCHRONOUS` and `MS_DIRSYNC`, either of which makes the mount strict.

What a program sees is in the pages of the calls: [mount(2)](../../../man/man2/mount.2) for `sync`, `dirsync` and remount, [statfs(2)](../../../man/man2/statfs.2), [getdents64(2)](../../../man/man2/getdents64.2), [mknod(2)](../../../man/man2/mknod.2), [symlink(2)](../../../man/man2/symlink.2), [ioctl(2)](../../../man/man2/ioctl.2) for the inode flags, and [sync(2)](../../../man/man2/sync.2) for `fsync` and its recorded error. The kernel log has each mount as `ext2: mounted: N blocks of B, I inodes, G groups`, with `, read-only` for a read-only mount, and the records named below.

## Design

### The format

The driver takes revision 0 and 1 superblocks, blocks of 1024, 2048 and 4096 bytes, and inodes of 128 and 256 bytes. It reads the primary superblock at byte 1024 and the primary descriptor table at block `s_first_data_block + 1`, and never reads or writes the backup copies.

* Incompatible features: `filetype` is the one known; any other fails the mount, which excludes ext4's extents, 64-bit numbers and flexible groups, and a journal that needs recovery.
* Read-only-compatible features: `sparse_super` and `large_file` are known; any other mounts the volume read-only.
* Compatible features: `dir_index` mounts the volume read-only, since a writer that keeps only the linear entries would leave the index stale. The rest are ignored, `has_journal` among them, so a journal is neither replayed nor written, except that the descriptor blocks `resize_inode` reserves must fit the volume.
* A block size below 4096 mounts the volume read-only: `bcache_write` writes a whole 4 KiB buffer, and two dependent metadata blocks in one buffer could not be written in order.

`tools/image/mkext2.sh` formats with 4 KiB blocks, 128-byte inodes, `sparse_super`, `large_file`, `filetype` and `resize_inode`.

The constructor fails with `EINVAL`, logged as `ext2: refused: WHAT`, when the magic is not `0xEF53` or the revision or sizes are not those above; the first data block is not 1 at 1 KiB blocks and 0 otherwise; a count is zero, out of range, or more than a bitmap block, the device or the groups hold; the descriptor table does not fit; a group's bitmaps or inode table lie outside it or overlap, or its free count passes its size; or the root inode is not a directory. A volume the driver mounts but does not write is logged as `ext2: mounted read-only: WHY`.

### Reading

An inode is the first 128 bytes of its slot; the rest of a 256-byte inode is neither read nor written. The owner and group have their high 16 bits at bytes 120 and 122, and a regular file's size its high word at byte 108, so a regular file's size is 64 bits and a directory's 32.

Every number from the disk is checked before use. An inode number must be within the count. A block pointer must lie inside the volume and outside the structural blocks: the boot block, superblock copies, descriptor tables, bitmaps and inode tables (`data_block_ok`). A directory entry must lie inside its block and the directory's size, with a `rec_len` of at least 8 and a multiple of 4, its name inside the record and its inode within the count. A fault is `EIO` and a log record such as `ext2: inode N: block B is out of range`; a bad pointer or entry also sets the superblock's error bit, written at once on a writable mount, so that the next writable mount checks the volume.

A file is read through 12 direct pointers and single, double and triple indirect blocks; a hole reads as zeros. A read does not change the access time.

A directory has no index, so a lookup reads entries until it finds the name. As in Linux's ext2, it starts where the last lookup in that directory found its name, remembered for 8 directories (`EXT2_LOOKUP_HINTS`), and wraps around: `ls` and `find` look up names in listing order, so a listing takes time proportional to the names, not their square. A lookup and the searches that add and remove entries read each block once into a copy from the heap, or through the cache without one. `readdir` returns every entry with an inode, `.` and `..` included, at a byte offset. `path` rebuilds a directory's path from `..` entries, for the `*at` calls.

A device node has its numbers in the first pointer word, a byte each of major and minor, or in Linux's 32-bit encoding in the second when the first is zero. The driver's `open` answers `EACCES` for one; [open(2)](../../../man/man2/open.2) says what a program gets.

### Symbolic links

A target shorter than 60 bytes is kept in the inode's pointer words, with `i_blocks` 0; a longer one is in the link's first block. `readlink` answers `EIO` for a size of a block or more, for a link of 60 bytes or more with no blocks, and for a long link without its block.

The resolver follows a link as [vfs.Readme.md](vfs.Readme.md) says. The calls that act on a final link itself are `lstat`, `lchown`, `readlink`, `unlink`, `rename`, `symlink` and `rmdir`; `link`, and `linkat` without `AT_SYMLINK_FOLLOW`; `fstatat`, `fchownat` and `utimensat` with `AT_SYMLINK_NOFOLLOW`; and `open` with `O_NOFOLLOW`, which answers `ELOOP`, with `O_PATH` and `O_NOFOLLOW`, which opens the link itself, or with `O_CREAT` and `O_EXCL`, which answers `EEXIST`. `stat`, `access`, `chdir`, `execve`, `truncate`, `chmod` and `chown` follow it.

### Writing

Every operation that changes the volume holds the mount's namespace lock throughout, and `assert_locked` panics on one that does not. Since the type record sets `ns_serialised`, the VFS takes the same lock around a description's reads and writes.

A mount is strict when it has `sync` or `dirsync` (`em_strict`). On a strict mount each metadata write reaches the medium before the next begins: `bcache_write` waits for the device and flushes the disk's cache. An inode bitmap alone goes by `bcache_write_unflushed`, and its descriptor's flush puts it on the medium. Any other mount writes back as Linux's ext2 does: a change is dirty in the buffer cache and reaches the disk, in no promised order, at `sync`, `fsync`, `syncfs`, unmount, the periodic sync of [update(8)](../../../man/man8/update.8) and eviction, and in runs once a free or a drain leaves a quarter of the cache's limit dirty (`em_balance`). On either kind of mount the superblock's free counts and state go straight to the medium (`sb_store`).

On a strict mount the writes reach the medium in this order:

* An inode is allocated by setting its bit and lowering its group's counts, then written initialised, then named; it is freed by freeing its blocks, then writing it zeroed with its deletion time, then clearing its bit and raising the counts.
* A block is allocated in its bitmap and its group's count before a pointer points to it, and a new pointer block is written zeroed before the pointer to it. `i_blocks` goes with the pointers, and `i_size` last or with them.
* A new entry goes into an empty record with room, or into the slack behind an entry's name, which the old record still covers, after which the old record is shortened to end where the new one begins; one write does both when the two headers and the new name lie in one 512-byte sector. A directory grows by a block zeroed with one empty entry, chained before or with the inode write that makes the size cover it; the entry comes last.
* An entry is removed before the link count drops: the first of a block by clearing its inode number, any other by joining its record to its predecessor's. The inode is freed with its last name; one that an open file or a working directory holds keeps its blocks until its last `put`, and a removed directory takes no new entry and lists as `ENOENT`.
* A truncation writes the smaller size and zeroes the tail of the last block kept, then clears pointers from the end, then frees the blocks. A growth writes the size over a hole.
* `mkdir` writes the new directory's block with `.` and `..`, then its inode with a count of two, then the parent's raised count, and the parent's entry last; `rmdir` removes the entry, lowers the parent's count and frees the inode. `link` raises the count, then writes the name; `symlink` writes a long target's block, then the inode, then the name; `mknod` writes the inode with its device number, then the name.
* `rename` of a file raises its count, writes the new entry or points the destination's entry at it, removes the old entry and lowers the count, so the file has a name at every stop; a replaced file loses a count as in `unlink`. `rename` of a directory removes an empty destination as `rmdir` does, writes the new entry, rewrites `..` and raises the new parent's count when the parents differ, then removes the old entry and lowers the old parent's count. A move of a directory below itself is `EINVAL`, found by a walk up `..` before any write.

Freeing a file's blocks is one batch (`punch`): the pointer slots and the inode change in the cache marked to reach the device first, the bitmaps and descriptors after (`bcache_dirty_ordered`), so no pointer on the disk points to a block whose bit is clear, whichever path writes.

A stop between two writes of a strict mount can leave an inode with no name (an orphan), a freed inode whose bit is set, a block nothing points to (a lost block), blocks past a file's size or an `i_blocks` behind its pointers, a link count above the names, or a directory with two names or a `..` naming the other parent. No data outside the range the operation was changing is lost or shared, and `e2fsck` repairs each. On a mount that writes back, a stop leaves whatever dirty buffers had reached the disk, for `e2fsck` to repair.

### The file-write batch

A write to a regular file goes through a batch the mount keeps across calls (`struct ext2_writeback`): private copies of the blocks it changes, a heap page each, 256 for data and 32 for metadata, taken when the mount becomes writable, and up to 16 pending inodes, which the batch holds. A batch takes at most 1 MiB of written bytes, a block written twice counting twice. A copy belongs to a stage: allocation bitmaps and descriptors; data; pointer blocks, a level at a time from those that name data up; the superblock's `large_file` flag; inode table blocks.

The batch admits an overwrite of blocks the file has inside its size, and an append that starts on a block boundary or in a last block the file has; any other write drains the batch first and again after each block. Each block is planned before anything changes: the free block chosen and its bitmap and descriptor copied, the pointer path and data block copied or zeroed. Then it is committed, without I/O, to the copies and to the inode's pointers, `i_blocks`, size and times, which reads see at once, since `read_bytes` looks in the copies first. `ENOSPC` is decided before any of a write is accepted, from every data and pointer block the range lacks, against the free count less the reserved blocks for a caller that is not privileged (`proc_privileged`).

A drain writes the stages in order; on a strict mount each is synced and flushed before the next enters the cache. The batch drains when full, at `fsync` and `sync`, at the release of an open description, before an attribute change or namespace operation (`assert_writable`), and before a put of an unlinked inode, so no block is freed or reused while an older write is pending. A failed drain keeps its copies and resumes at its stage next time; a removed device is not tried again. A failure makes the mount read-only and sets its error bit, and every later `fsync` reports the first one. A write that fails after some of its bytes were accepted returns their count.

### The superblock's state

A writable mount clears the valid bit of `s_state` on the medium before the mount is published. The unmount's prepare step syncs the volume and sets the bit again only when every write succeeded and the error bit is clear, and clears it again if the VFS then abandons the unmount. A remount read-only does the same as the prepare step, and is `EBUSY` while a file removed from the volume is open, since a read-only mount frees no inode. A volume cut off while writable, or whose writes failed, is therefore checked at its next writable mount. The superblock's free counts are the sums of the descriptors', which the driver keeps exact.

### The check

A writable mount or remount of a volume whose valid bit is clear or whose error bit is set checks it before writing. A read-only mount, asked for or forced by the format, is not checked and writes nothing.

The check marks blocks in the bitmap the FAT drivers' checks share (`fat_walk_acquire` in `fatcommon.h`), one check at a time, and keeps on the heap one bit per inode, the inodes of 32 directory levels, a table of 512 files with more than one link, and a bitmap block. It marks the structural blocks and those of the reserved inodes below `s_first_ino`; walks the tree from the root, marking each block every inode points to and checking each directory's `.`, `..`, entries, holes and link count; reads the used inodes the walk did not reach; and compares both bitmaps with the marks, recounting each group's free counts into its descriptor.

It repairs nothing else. A used inode the walk did not reach is an orphan, its blocks marked so that nothing else takes them; a used block nothing marked is lost. Both stay, held by their bits, and the mount is writable with the error bit set, so that no unmount marks the volume clean before `e2fsck` has run; the log has `ext2: check: N lost blocks, M orphan inodes, kept for e2fsck`, or `ext2: check: clean`.

A fault mounts the volume read-only, with the error bit written, the counts as the disk had them, and the first fault logged as `ext2: check: inode N: WHY`: a block out of range or claimed twice; pointers that disagree with the size or `i_blocks`; a directory with two parents, a hole, or `.` and `..` out of place; an entry of the wrong form or type, or naming a free inode; a link count that disagrees with the names; a bitmap that calls a used block or reached inode free; or a tree deeper than the levels or with more linked files than the table. A workspace the heap cannot give mounts the volume read-only too. Of the stops a strict mount leaves, a lost block, an orphan and a freed inode whose bit is set mount writable; the others mount read-only.

### Allocation

A block is allocated first fit from the block after the last one given out, through every group once, ending at the start of the group it began in. An inode goes in its parent's group when that has one free, else in the next group that has, wrapping. A caller that is not privileged cannot take the blocks the superblock reserves; no inodes are reserved. An operation that meets `ENOSPC` at its entry after allocating its inode frees the inode.

A new inode belongs to the caller's effective user and group, or to the directory's group when the directory is set-group-ID, and a new directory there is set-group-ID too, as Linux's `inode_init_owner` does. Its generation comes from a count that starts at each mount at the time in seconds shifted 8 bits left.

## Limits

* The largest file is what the block pointers and the 32-bit `i_blocks` address: 2196875759616 bytes at 4 KiB blocks. A write, truncate or `fallocate` past it is `EFBIG`, logged with the limit.
* A directory is at most 4 GiB less a block (`EFBIG`), a link count 65000 (`EMLINK`), a name 255 bytes, and a symbolic link's target a block less one byte, 4095 bytes at 4 KiB blocks (`ENAMETOOLONG`).
* `path` and the rename check climb at most 2048 directories (`VFS_PATH_MAX / 2`).
* Times are 32-bit seconds; a time the clock gives past 2038-01-19 03:14:07 UTC is stored as 0.
* Extended attributes are not read, and an inode's attribute block (`i_file_acl`) is not freed with it.
* Nodes in memory and mounts come from the heap without a fixed count (`ENOMEM`). There are no quotas.

## Tests

* `tests/run host` runs `tests/host/ext2_hosttest.c`, with the batch rows in `ext2_batch_cases.h` and `ext2_batch_lifecycle.h`, and `tests/host/ext2_reserve_hosttest.c`. The first mounts, reads and writes volumes the host's `mke2fs` makes, sound and damaged, and stops all but one of its write sequences after every write on a `dirsync` mount to judge the stop by `e2fsck` and by the check.
* `sh tests/host/ext2_batch_mutate.sh`, `sh tests/host/ext2_reserve_mutate.sh` and `sh tests/system/ext2_batch_mutate.sh` prove those rows and the batch's disk-command counts with compiled faults.
* `tests/run ext2` runs file-system probes on an ext2 root and on a mounted volume, and boots os-real on an ext2 root through a kill, the check, a fault and recovery's `e2fsck`.
* `tests/run large` writes a file past 4 GiB, and `tests/run selfbuild-ext2` builds the kernel on an ext2 root.

[tests/host/Readme.md](../../../tests/host/Readme.md) and [tests/system/Readme.md](../../../tests/system/Readme.md) describe each.
