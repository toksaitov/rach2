# Milestone 41: the ext2 root

## The goal

The kernel gets the classic Unix file system, ext2 as Linux defines it, on the buffer cache and under the write rules of milestones 37 and 38, and uses it as its root. A root on ext2 is not limited by the mapped window that bounds a KRFFS root to half a gigabyte, has owners, modes, hard links, symbolic links and device nodes on disk, and is read and checked by every Linux and by the e2fsprogs tools. KRFFS stays what it is: the file system of the RAM images, of the test image, of recovery and of the sealed disk's second partition, which remains a bootable root and the fallback for the new one.

The milestone is complete when the os-real image exists as an ext2 volume built on the host and checked clean by `e2fsck`; when QEMU boots that volume as its root, builds the kernel on it through `tests/run selfbuild`, and survives a crash with the mount-time check and a repair by `e2fsck` from recovery; when a volume the kernel wrote is read back identically by `debugfs` and passes `e2fsck` clean; when symbolic links resolve through the walker that milestone 36 prepared for them; and when the Lenovo boots from an ext2 root on its own disk with the KRFFS root as its fallback, builds a program on it, and comes back after a reboot.

The milestone has three halves with a gate between each, which the first review asked to be three milestones: the staging tools, the checker's reader, the kernel's reader and the links, through phase 3; the writer, its crash matrix, the check and `e2fsck` in recovery, through phase 5; and the images, the suites and the machine, from phase 6. Each later part starts only when the one before it is closed and reviewed. They are one milestone because each depends on the one before and on nothing else, and the count of milestones has grown once already; if the run finds the gates are not enough, the owner can cut it at either gate with nothing lost.

This plan is the authority for the run. A conflict between this plan and the current tree is resolved by the decisions here; a conflict between this plan and `CLAUDE.md` is resolved by `CLAUDE.md`. The plan was revised twice after independent review. The first revision withdrew a crash guarantee the format cannot give, changed the inode allocation order, made the check seed its structural blocks and validate orphans, restricted writable mounts to 4 KiB blocks, fixed the symbolic-link rule for `O_CREAT|O_EXCL`, cut the port to `e2fsck` in recovery, and rewrote the machine procedure so that nothing is committed before the fallback has been proved. The second stated the promise's scope, the reserved inodes, the inode-bitmap comparison, the check on the error bit, the rename rules for one inode and for descendants, the atomic image target, and four proofs.

## What the tree settles before this plan

* The interface: `vfs_type` and `vfs_ops` with `lookup`, `hold`, `put`, `getattr`, `setattr`, `open`, `release`, `read`, `write`, `truncate`, `fsync`, `readdir`, `create`, `unlink`, `mkdir`, `rmdir`, `rename`, `link`, `readlink`, `statfs`, `sync`, `prepare_unmount` and `unmount`; physical `..` in a component loop; directory descriptors and working directories that hold a node; the lock order; the description lock; every mutating entry asserting the mount's lock. `link` answers `EPERM` on every file system so far, `readlink` serves procfs, and no file system has had a symbolic link, so the walker has never followed one. The kernel's file offset is 32 bits unsigned.
* The write rules: `bcache_write` for metadata in dependency order, which writes a whole 4 KiB buffer; data written back and synced before a size that covers it is published; a failed write making the mount read-only; `fsync` as a device sync; the `update` process; and the mount-time check with its shared bitmap of 2^20 bits, its safe repairs, and lost allocations counted and kept.
* The root today: `rach2.root=disk` scans the MBR for a KRFFS partition and maps it through the window; the RAM module otherwise. The block layer comes up before the root; the USB stack after it. `bootctl config` rewrites the boot menu's file with its entries and their kernel command lines and verifies it; the menu stub in the GRUB core resolves the fallback by entry number, not by anything in that file, so a bad file cannot take the fallback away. The recovery image is a RAM root with `reflash`, and the partition nodes are read-only in every build; this milestone makes the one exception below.
* The images: a manifest of `dir`, `file`, `text` and `tree` lines with modes and owners, staged into KRFFS by `stageimage.krffs`, checked by `setidcheck`, which reads the staged image's nodes for the set-id policy. The sealed disk image from `tools/image/mkdisk_grub.sh` has three partitions; the Lenovo's disk has a fourth, a 1 GiB FAT32 scratch partition from milestone 37, if the owner approved it there.
* ext2: revision 1, superblock at byte 1024 with magic `0xEF53` and backup copies in the groups `sparse_super` names, block sizes 1024 to 4096, inode sizes 128 and 256, block groups with a descriptor each, block and inode bitmaps, inode tables, twelve direct blocks and three levels of indirection, `i_size` and `i_blocks` kept in step, linear directories with `rec_len`, `name_len` and a file type, fast symbolic links in the inode and slow ones in a block, link counts, device numbers in the first block pointer or, when it is zero, in the second, and the feature sets: compatible features a driver may ignore, read-only-compatible features that force a read-only mount when unknown, incompatible features that refuse a mount when unknown. `s_state` has a valid bit that a mounting driver clears and a clean unmount sets, and an error bit a driver sets when it has found the volume inconsistent, which is how `e2fsck` knows what to do. A freed inode is zeroed by every serious ext2 driver before its bitmap bit is cleared, so that a stale pointer never survives in the inode table. Linux mounts a volume with any ext2, ext3 or ext4 superblock through one driver; `mke2fs` takes its default features from a configuration file, so a command that names every feature it wants and none it does not is the only way to a known profile.
* e2fsprogs: `mke2fs -d DIR` builds a volume from a directory, `debugfs` reads and edits one without mounting it, `e2fsck` checks and repairs, and all three build against musl, as Alpine ships them. On this host e2fsprogs is a keg-only Homebrew package, which puts `mke2fs` under `/opt/homebrew/opt/e2fsprogs/sbin`; on Debian it is in the base system. mtools has been a required host tool since milestone 23; e2fsprogs becomes one here, for the same reason, that the build makes an image with it, and the preflight finds it on the path, under the keg, or under `/sbin`, and probes it for the options the build uses.

## Decisions on the driver

### What is read

`sys/fs/ext2.c` reads revision 0 and 1 superblocks, with `filetype` as the one incompatible feature it knows, `sparse_super` and `large_file` as the read-only-compatible ones, and any compatible feature ignored except `dir_index`, which is not ignored: a volume with an indexed directory would have its index left stale by a writer that keeps only the linear entries, so `dir_index` mounts read-only with a record, and the images this project makes are formatted without it. An unknown incompatible feature refuses the mount by name; an unknown read-only-compatible one mounts read-only by name. Block sizes 1024, 2048 and 4096 are read; only 4096 is written, because `bcache_write` writes a whole 4 KiB buffer and two dependent metadata blocks sharing one buffer could not be written in order, so a 1 KiB or 2 KiB volume mounts read-only with a record, and the images this project makes are 4 KiB. Inode sizes 128 and 256. Every field a lookup depends on is bounds-checked against the superblock and the group descriptors before it is used: a block number within the volume and outside the structural blocks, an inode number within the count, a directory entry within its block with a `rec_len` of at least its header and a multiple of four, a name within the entry.

A regular file is read through its direct, indirect, double-indirect and triple-indirect blocks, with a hole read as zeros. The kernel's offset is 32 bits: an inode whose size does not fit answers `EOVERFLOW` to `open` and `stat`, is not written to, and is left exactly as it is on disk, high word included; a write or truncate that would take a file past the kernel's limit answers `EFBIG`. A symbolic link shorter than sixty bytes is in the inode, a longer one in its first block. A device node reports its numbers in `stat` from whichever pointer holds them and cannot be opened, because devices live in `/dev`; that is also what `MS_NODEV` means here, and it is enforced by never opening one. Owners, modes, link counts and the three timestamps are the inode's, in seconds, with the year 2038 limit the format has.

### Symbolic links

The walker follows a symbolic link when it is not the last component. For the last component the operation decides, as on Linux: `open` follows unless `O_NOFOLLOW` is given or `O_CREAT|O_EXCL` is given, in which case a link, dangling or not, answers `EEXIST`; `stat`, `access`, `chdir`, `execve`, `truncate`, `chmod`, `chown` and `utimensat` without `AT_SYMLINK_NOFOLLOW` follow; `lstat`, `readlink`, `unlink`, `rename`, `link`, `symlink` and `rmdir` do not. A followed link's target replaces the component, an absolute target restarts at the root, and the walk continues, with a bound of forty links along one path, after which `ELOOP`. `..` is physical since milestone 36, so `link/..` goes to the parent of the target, as it does on Linux. `symlink`, `symlinkat` and `readlinkat` become real syscalls, and `link` and `linkat` become real on ext2, with `EPERM` for a directory and `EXDEV` across mounts as before.

### The order of writes, and what it promises

Every metadata write is `bcache_write`, in the order below, and data is synced before a size covers it. The promise is narrower than FAT32's and it is the one the format allows without a journal: at every stop between two completed sector writes, no data is lost or shared outside the range the interrupted operation was itself changing or removing, and `e2fsck` repairs the volume without discarding a reachable file's contents. A truncation that has written its smaller size has removed its tail on purpose, and an overwrite of a block stopped between two of its sectors leaves the block half old and half new, which is the granularity of every disk and is allowed by the promise; what is not allowed is a stop that costs a file the operation was not touching. It is not a promise that every stop is structurally clean: a directory moved between parents, a replaced destination, a shrunk file, and a grown directory each have a stop that `e2fsck` reports as a topology or a block-map repair and not as a count, and those stops are listed here, held to the promise above, and proved by the matrix through a repair on a copy and a comparison afterwards. A stop that needed a journal to be clean is written down as such in the Readme.

* An inode is freed by writing it zeroed, with its deletion time, before its bitmap bit and its group count clear; an inode is allocated by setting its bitmap bit and its group count, then writing it initialised, then naming it. A crash between the last two leaves an allocated inode with no name and, because of the first rule, no stale pointers.
* A block is allocated in its bitmap before a pointer names it; data is written and synced before the size that covers it; a pointer block is written before the pointer to it; `i_blocks` is written with the pointers and `i_size` last. A stop between the pointers and the size leaves blocks past the end, a block-map repair.
* An entry is removed before the inode's link count is decremented, and the inode is freed, with its blocks, only when the count is zero and no description holds it; a held node keeps its inode and blocks until its last `put`, which is ext2's own rule and what makes an open unlinked file work; a crash while it is held leaves an allocated inode with no name, which is an orphan.
* A truncation shrinks `i_size` first, then clears the pointers from the end with `i_blocks`, then frees the blocks; a stop between leaves blocks past the end, a block-map repair.
* `mkdir` writes the new directory's block with `.` and `..`, its inode with a link count of two, the parent's count incremented, and the parent's entry last. `rmdir` is the reverse, on a directory with nothing but `.` and `..`, and refuses a held one with `EBUSY`.
* A directory grows by a block zeroed with one empty entry spanning it, allocated, chained in the inode with `i_blocks`, and covered by `i_size` last.
* `rename` of a file within a mount is a `link` followed by an `unlink`: the count is raised, the new entry written, the old entry removed, the count lowered; the file is reachable by at least one name at every stop, and the two stops where the count is one too high are count repairs. A rename whose source and destination are one inode, two names for the same file, changes nothing and answers success, as Linux does. A rename onto an existing file raises the source's count, rewrites the destination entry to the source inode, lowers the victim's count and frees it as an unlink does, then removes the old source entry and lowers the source's count; the stop after the rewrite leaves the victim an orphan. A rename onto an existing empty directory is refused unless the source is a directory, and then the victim is removed as `rmdir` does before the move. A directory may not be moved into itself or into one of its descendants, which the walk from the destination's parent to the root decides before anything is written, with `EINVAL`. A directory cannot be hard-linked, so its rename across parents writes the new entry, rewrites its `..`, raises the new parent's count, removes the old entry, and lowers the old parent's count, in that order; the stops inside leave the directory with two names or with a `..` that names one parent while an entry names another, which `e2fsck` repairs as topology, and the record says so.

The superblock's free counts are written at `sync` and at unmount, since they are summaries the group descriptors already hold exactly, and `e2fsck` treats them as such; the group descriptors are written through with their bitmaps.

### The valid bit, the error bit and the check

Mounting for writing clears `s_state`'s valid bit with `bcache_write` before the mount is published, and `prepare_unmount` sets it again after everything has reached the disk and only when the error bit is clear, which is the classic ext2 behaviour and what `e2fsck` reads. A volume whose valid bit is clear at mount was not unmounted, and one whose error bit is set was found wanting by a previous mount; either is checked before it is used, under the walk lock, with the shared bitmap raised from 2^20 to 2^22 bits, 512 KiB, which covers 16 GiB at 4 KiB blocks.

The check seeds the bitmap with every structural block, the superblock and its copies, the group descriptors and their copies, both bitmaps and the inode table of every group, and the blocks of the reserved inodes below `s_first_ino`, which on the profile this project writes is the bad-block inode and no other, so that none can be handed out as data and none is mistaken for an orphan. It walks the tree from the root inode with a visited set of inodes, so that a hard link and `..` are revisits and not duplicates, and marks every block every reached inode names, through every level of pointer; a block named by two different inodes, or twice by one, is a cross-link. It checks directory topology: `.` is the directory, `..` is its one parent, no directory has two parents, no cycle. It checks each inode's pointers against its size and its `i_blocks`, a count against the entries that name the inode both ways, and every entry for form. Then it scans every inode the inode bitmap says is allocated and the walk did not reach: each is an orphan, its pointers are validated and its blocks marked, so that the allocator cannot give them away while `e2fsck` has not yet reclaimed them. Then it compares the block bitmaps with the marks: allocated and unmarked is lost; marked and unallocated is a fault. It compares the inode bitmap with the reached set the same way: a reached inode whose bit is clear is a fault, since it could be handed out again. The group descriptors' free counts are recomputed from the bitmaps during the check and not trusted, since a crash leaves them stale.

The check repairs nothing. Lost blocks and orphan inodes are counted and kept, the volume mounts writable with their blocks held out of the allocator, and the error bit is set in `s_state` so that the volume is never marked clean again until `e2fsck` has run; an unmount leaves that bit as it is. Any fault mounts the root read-only with the first one named and the error bit set. A volume larger than the bitmap mounts read-only when dirty and says why. That is a smaller check than FAT32's, on purpose: ext2 has a real checker, it is ported in this milestone and lives on the recovery partition, and a kernel that repaired what `e2fsck` repairs would be a second, worse `e2fsck`.

### Free space and allocation

Free counts come from the group descriptors, summed once at mount and kept. An inode is allocated in the parent's group when it has one free, else in the first group that has one, which is the rule that keeps a directory's files near it. A block is allocated first-fit from the group of the file's inode, then from the group after the file's last block, which keeps a file written in one go contiguous. `ENOSPC` is decided under the mount's lock before any bitmap is written; the root's reserved blocks are honoured for a caller who is not root.

## Decisions on the root

`rach2.root=ext2` scans the partitions in MBR order for the first whose superblock has the magic and a feature set this driver can mount at all, and mounts it as the root through the VFS, as entry zero; the check then decides whether that root is writable or read-only, and a later partition is never tried in its place; `rach2.root=disk` keeps mounting KRFFS through the window, and the RAM module stays the default. The block layer is already up before the root; the buffer cache is initialised before it; the USB stack comes up after it, so a root on a stick is refused by name and is not this milestone's. `/` on ext2 has `/dev` and `/proc` mounted over it as before. `init`, `login` and everything else run from it through the read path milestone 36 gave `execve`.

A dirty root that fails its check mounts read-only with the error bit set, and what happens next is not promised by that root: its `init` and its shell may be among the damaged paths. The promise is made by recovery instead, which is a RAM root that needs nothing from the disk: it carries `e2fsck`, its `init` runs `e2fsck -p` on every ext2 partition it finds before it starts anything else, and the record of that run is in its boot log; over SSH, `e2fsck` can be run by hand as well. For that, the recovery build's partition nodes are writable, a new exception this milestone makes under the recovery kernel's authority from milestone 25, and no other build gets it. A root that mounts read-only is also what `bootctl` and `update` must survive: `bootctl` writes the boot partition and not the root, and `update` calls `sync` on a read-only mount and gets nothing to do.

The KRFFS root on the sealed disk's second partition stays bootable and is the fallback entry's root: the candidate entries carry `rach2.root=ext2` and the fallback carries `rach2.root=disk`, so a candidate kernel or root that does not come up falls back to a system that does, through the trial mechanism milestone 23 built and nothing new. A candidate that boots but cannot reach a shell or SSH is not committed, because nothing commits it but a person, and the next boot is the fallback.

## Decisions on the tools

### Building an ext2 image from a manifest

`stageimage.krffs` gains `--tree DIR`: it materialises a manifest into a host directory with the files and text it names, which it already knows how to read. `tools/image/mkext2.sh` then runs `mke2fs` with the complete feature set named, `-t ext2 -b 4096 -I 128 -O sparse_super,large_file,filetype,^dir_index,^resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize`, on that directory, reads the superblock back and refuses any feature or geometry outside that profile; then a `debugfs -w -f` script that the same run writes from the manifest sets each staged file's owner, group and mode with `set_inode_field`, keeping the inode's type bits and applying the manifest's permission and set-id bits, because a build that does not run as root cannot make a root-owned set-id file on the host and `debugfs` can. Then `e2fsck -fn` on the result, required clean, and then `setidcheck` on the finished image as a separate, mandatory gate, since `e2fsck` proves structure and not policy. The script builds and checks a temporary file and renames it to its target only after both gates pass, deleting it on any failure, so that no unchecked image ever exists under the target's name; `mkdisk_grub.sh` consumes that target and nothing else. The image for the sealed disk's fourth partition is 1 GiB, the size the scratch partition has, and it is sparse on the host.

### The set-id policy on ext2

`setidcheck` reads a staged image's nodes for the set-id sweep, and it must read ext2 images too. It gains a small read-only ext2 reader of its own, sharing nothing with the kernel's driver, which fails closed: it accepts exactly the profile `mkext2.sh` writes and refuses any other feature, any block out of range, any malformed entry and any inode it cannot place, so that an image the checker cannot read is an image the build rejects. It lists every inode with its type, owner, group, mode and set-id bits, follows every directory to its end, and reports hard-linked and duplicate names, which the policy compares with the manifest. That reader is also the independent implementation the host tests compare the kernel's driver against, with `debugfs` and `e2fsck` as the third and fourth.

### e2fsck on the machine

e2fsprogs is ported the way Dropbear and TinyCC were: a pinned tarball verified by hash, built against the ported musl behind a stamp file by `vendor/e2fsprogs/build_e2fsprogs.sh`, with `e2fsck` alone and the libraries it needs staged into the recovery image and nothing else, because `e2fsck` in recovery is the one use this milestone has and tests; `mke2fs` and `debugfs` on the machine would be formatters with no proof and are left unbuilt. BusyBox has no `e2fsck`, which is the standing reason a port is allowed. Phase 1 builds it once, records the binary's size, the recovery image's growth and its RAM use at boot, and sets the limit the port must stay under. The port's `Readme.md` says what is built, what is left out, and how it is checked.

### The disk image and the suites

`tools/image/mkdisk_grub.sh` gains an optional fourth partition, `RACH2_DISK_EXT2_MB`, filled from the ext2 image, and the sealed disk image has it; the menu entries do not change, and the candidate entries' command line carries `rach2.root=ext2` while the fallback's carries `rach2.root=disk`, through the same file `bootctl config` writes. `tests/run sealed`, `install`, `drill` and `selfbuild` gain their ext2 variants, and `selfbuild` on ext2 is the one that reads most of the volume.

## Memory and limits

| Limit | Value |
|-------|-------|
| Block sizes | 1024, 2048, 4096 read; 4096 written |
| Inode sizes | 128, 256 |
| Largest file | 4 GiB less one, the kernel's 32-bit offset; a larger one answers `EOVERFLOW` and is left as it is |
| Largest volume checked when dirty | 2^22 blocks: 16 GiB at 4 KiB |
| Symbolic links along one path | 40 |
| Name | 255 bytes, the format's and the kernel's |
| Timestamps | 32-bit seconds, until 2038 |
| Held nodes per mount | 64, a pool like FAT32's; `ENOMEM` beyond |
| The port | `e2fsck` in recovery, under the size and RAM limit phase 1 sets |

## What stays out

ext3's journal and ext4's extents, 64-bit block numbers, checksums, flexible groups and large inodes: refused or read-only by the feature rules, as the format intends. `dir_index`, and 1 KiB and 2 KiB blocks: read-only. Extended attributes and access lists: ignored on read, never written. Quotas: no. A root on USB: refused by name. Repair in the kernel: none; `e2fsck` is in recovery. `mke2fs` and `debugfs` on the machine: no use, not staged. Online resize: no. `mknod`: no, devices live in `/dev`.

## Decisions on tests and tooling

* `tests/host/ext2_hosttest.c`: the driver over the buffer cache over images `mke2fs` made from a staged tree, at each block size and inode size, with fast and slow symbolic links, hard links, a device node in each encoding, a sparse file, a file through every level of indirection, a file past the kernel's limit, and a directory across several blocks; every file read back against the tree and against `setidcheck`'s reader; `dir_index` and a 1 KiB volume mounting read-only by name. Then the write sequences FAT32's test has, plus `link`, `symlink`, `rename` as link-then-unlink, replacement rename of a file and of an empty directory, directory rename across parents, an open unlinked file written through and closed, a name of 255 bytes created and one of 256 refused with `ENAMETOOLONG`, a file grown to the limit and one byte past refused with `EFBIG`, the sixty-fourth node held and the sixty-fifth refused with `ENOMEM`, each refusal leaving the volume byte-for-byte as it was, and the reserved blocks refused to a non-root caller with `ENOSPC`; each read back through the driver, through `setidcheck`'s reader, and through `debugfs`, and every image passed to `e2fsck -fn` and required clean. Then the interruption matrix as in milestone 37, stopped after every metadata write and every data sync with a fresh mount and the content oracle, and judged three ways: the walk and `setidcheck`'s reader for structure; `e2fsck -fn` on the stopped image, whose expected answer at each stop is the one the order of writes lists for it; and a repair by `e2fsck -fy` on a copy, remounted, with every file's name, contents, links, owner, group and mode compared with the oracle's, which is what proves nothing was lost, and a mutation that corrupts a group in the copy proves the comparison looks. Then the dirty-volume rows: a clean tree mounting writable with its counts, a lost block kept and held out of the allocator, an orphan inode with pointers kept and held out, each structural block seeded, each fault mounting read-only by name, and the error bit set and surviving an unmount.
* `tests/host/setidcheck` gains rows for its ext2 reader: against `debugfs` on the profile's images, and refusing an image with a feature outside the profile, a block out of range, a malformed entry, and an inode it cannot place. Its policy rows gain a set-id file with the wrong owner, the wrong group, an added and a removed set-id bit, an extra set-id inode the manifest does not name, and a hard link to a set-id file.
* `tests/vfsprobe.c` gains the symbolic-link rows: a relative and an absolute target, a link to a directory with `..` through it, a chain of forty and of forty-one, `O_NOFOLLOW`, `O_CREAT|O_EXCL` on a dangling link answering `EEXIST`, `lstat` against `stat`, `readlink`, `unlink` of a link leaving its target, and a dangling link. And the hard-link rows: `link`, the count in `stat`, `unlink` of one name, and `EPERM` for a directory.
* `tests/system/suites/ext2.sh`, `tests/run ext2`, core tier: a boot with `rach2.root=ext2` on a disk with both roots, a write, a reboot, the read back; the probe rows; a crash boot with the dirty check's record, then a recovery boot whose `init` runs `e2fsck` and whose log says so, then the next ordinary boot clean; drills with a missing candidate root, a corrupt one and a dirty one, each falling back to the KRFFS root without a person; a `bootctl` install with the two command lines, asserting the exact command line and the mounted root in both slots, a trial that boots the fallback, the old kernel preserved, and the menu restored from its saved copy; and a BusyBox boot that makes a symbolic link, a hard link, and lists both.
* `tests/run selfbuild` on the ext2 root, which is the read test of a real working set; its two builds run in two clean directories and the suite shows that both read their sources from the ext2 root.
* `tests/run ext2` also asserts, from the boot log's order of records, that the partitions are scanned and the root mounted before the USB stack starts, and, in an ordinary build, that a write to a partition node fails and leaves the sector as it was.
* `tools/setup/doctor.sh` and `preflight.sh` list e2fsprogs as required, find it on the path, under the Homebrew keg and under `/sbin`, and probe the options `mkext2.sh` uses.
* Every mutation script in this milestone invalidates the stamp and the object it mutates and proves that the mutant reached the artifact that ran, because a stamp can make a rebuild reuse old output.
* `tests/system/accept/metal_accept.sh` gains the ext2 root checks.

## Documentation

* `rach2/sys/fs/ext2.Readme.md` is new: what is read, the features and why `dir_index` and small blocks are read-only, the offset limit, symbolic links and the walker's rules by operation, the order of writes and what it promises and what it does not, the valid bit and the error bit and the check and why it repairs nothing, allocation, the root and its fallback and recovery's `e2fsck`, the limits, what is refused, and the verification.
* `rach2/sys/fs/vfs.Readme.md`: symbolic links in the walker, `link` and `symlink`, the larger bitmap, the recovery build's writable partitions.
* `rach2/sys/fs/Readme.md`: what KRFFS is for now.
* `rach2/sys/block/Readme.md`: the recovery build's writable partition nodes.
* `rach2/Readme.md`: `rach2.root=ext2`, the syscall list, the file systems, the limits, and the boot paths.
* `Readme.md`: the system paragraph, the images, "Updating the installed machine" with the ext2 root and its fallback, and the hardware status.
* `krffs/Readme.md`: `--tree`.
* `tools/Readme.md`: `mkext2.sh`, the fourth partition, the checker's ext2 reader, e2fsprogs as a required tool.
* `vendor/e2fsprogs/Readme.md` is new, and `vendor/busybox/Readme.md` says why `e2fsck` is not BusyBox's.
* `utils/Readme.md`: `bootctl config` with two roots, and recovery's `init` running `e2fsck`.
* `tests/Readme.md`, `tests/host/Readme.md`, `tests/system/Readme.md`.
* `dev-log/41-ext2/Readme.md` and the `dev-log/Readme.md` entry, in phase 8.

## How the work is kept track of

`progress.md`, `current.md`, `notes.md` and `hardware.md` beside this plan, as before, and the one-line report `M41: phase N of 8; X of Y items done; HEAD abc1234; tree clean|dirty`.

## Standing rules for the run

* Work directly. The only delegation is an independent review, one reviewer per unit, at most six files, at most ten minutes. There are seven review checkpoints, after phases 2 to 8, with the number of units fixed in phase 1, and one focused re-review for a fix that changes a contract.
* Commit in scoped units in the house style, on the branch, no attribution, never push.
* Every commit passes `tests/run style` and `tests/run host`. A commit that touches a subsystem runs that subsystem's suites singly. The whole set runs after phases 4 and 6 and at the end of phase 8.
* A test is proved by making it fail. A mutation is driven from a script that restores the source in a trap, invalidates the stamps and objects it touched, rebuilds, and proves the mutant is in the artifact before the verdict is believed.
* A change that alters a contract rewrites the owning Readme in the same commit.
* A change that makes something redundant removes it in the same commit. The tree after this milestone is not larger than the milestone's own additions, the port excepted, and the port stays under the limit phase 1 sets.
* The vendored port follows the standing rules for ports: a pinned tarball, a hash, a build script behind a stamp, nothing of its output committed.
* Phase 4 does not start until phase 3 is closed and reviewed; phase 6 does not start until phase 5 is.
* On the Lenovo, the writes are the kernel installation through `bootctl`, the boot menu through `bootctl config`, and the ext2 image into the fourth partition through recovery, each presented and approved as `CLAUDE.md` requires. The KRFFS root and recovery partitions are hashed before and after and must match; the boot partition changes by design, and its change is recorded as the list of files `bootctl` wrote and their hashes, predicted before and compared after. If the fourth partition does not exist because milestone 37's owner refused it, the same proposal is made again here with milestone 37's procedure, the partition table saved and the new entry checked against the three others, and a second refusal closes the milestone on QEMU with the record saying so.
* Nothing is deferred. A thing discovered mid-run goes into `notes.md` with the phase that will do it, and phase 8 closes every line or records the owner's decision to drop it.

## The phases

### Phase 1: arrival, the tools and the port

Read `AGENTS.md`, `CLAUDE.md` and this plan in full. Require a clean tree and record `git log --oneline -12`. Create the four tracking files in the first commit. List the files each review checkpoint will cover and fix the units.

Install e2fsprogs on this host and make it a required tool: `preflight.sh`, `doctor.sh` with the three places to look and the option probe, and the Readme. Build one ext2 image from the os-real manifest by hand with the steps of `mkext2.sh`, check it with `e2fsck -fn`, list it with `debugfs`, and record the superblock, its feature words and one inode in `notes.md`. Check every number in the ext2 bullet above against the format's documentation and e2fsprogs' headers, and record each with its source. Build e2fsprogs against the ported musl once, with the stamp invalidated for the proof, run `e2fsck` in a recovery boot in QEMU against an image, record the binary's size, the recovery image's growth and its RAM at boot, and set the limit.

Proof: the image is clean and lists every staged file with the manifest's owner and mode; `e2fsck` runs in recovery; the tables in `notes.md`.

### Phase 2: the staging tools and the checker's reader

`stageimage.krffs --tree`, `mkext2.sh` with its profile and its superblock check, the `debugfs` script, and `setidcheck`'s ext2 reader with its rows. The os-real ext2 image as a Make target, checked by `e2fsck -fn` and by `setidcheck` at every build.

Proof: the image builds from the manifest and passes both gates; the reader lists it as `debugfs` does and refuses what it should.

Mutations: stage a set-id file with the wrong owner, with the wrong group, with an extra set-id bit, and an extra set-id inode; drop the `debugfs` step; format with `dir_index` on; write a block number past the volume into a fixture. Each must fail exactly its check.

### Phase 3: the reader in the kernel and the links

`ext2.c` for reading, the feature rules, the offset limit, the walker's symbolic-link rules by operation, `symlink`, `symlinkat`, `readlinkat`, `link` and `linkat`, the read rows of `ext2_hosttest.c`, and the `vfsprobe` rows on a mounted image. This closes the first part, and its review is the gate to the writer.

Proof: every image reads back against the tree and the reader; every link row passes; a mounted ext2 volume lists in the os-real shell.

Mutations: ignore an unknown incompatible feature; mount `dir_index` writable; follow a forty-first link; follow the last component under `O_NOFOLLOW`; follow a dangling link under `O_CREAT|O_EXCL`; read a slow link from the inode; read a device number from the first pointer alone; trust a `rec_len` past its block; take a block number inside the inode table; open a file past the offset limit. Each must fail exactly its row.

### Phase 4: the writer

Allocation and the free-then-allocate inode order, the order of writes for every operation, the rename as link then unlink and the two replacement cases, the held inode, the superblock counts at sync, `i_size` and `i_blocks`, and the valid bit. The write rows and the interruption matrix with its three judges at every stop.

Proof: every sequence reads back through the driver, the reader and `debugfs`, and `e2fsck` is clean; every stop gives the answer the order of writes lists for it, and the repaired copy has every file intact; the dirty rows give their answers.

Mutations: name an inode before it is written; allocate an inode before it is zeroed at free; grow the size before the pointers; grow the size before the data sync, which the oracle must catch; free a held inode at unlink; remove the old entry before writing the new one in a rename, which the repaired copy must catch as a lost file; write the superblock counts through on every allocation, which the write-count row must catch; never clear the valid bit. Each must fail exactly its check.

### Phase 5: the check, the root and recovery

The mount-time check with its seeds, its visited set, its topology rules and its orphan scan, and the error bit; `rach2.root=ext2`, the scan, the mount as entry zero and the boot order; the recovery build's writable partition nodes; `e2fsck` staged into recovery and run by its `init`; the crash boot, the drills and the repair. This closes the second part.

Proof: QEMU boots the ext2 root, `bootctl status` reports the slots, `/proc/mounts` shows the root; a crash leaves a dirty root that mounts read-only with the error bit when its check finds a fault and writable with the bit when it finds only orphans and lost blocks; recovery's `init` repairs it and the next boot is clean; a missing, a corrupt and a dirty candidate root each fall back to KRFFS without a person.

Mutations: mount a dirty root writable without the check; skip the structural seeds, which the fixture with a file pointing into the inode table must catch; skip the orphan scan, which the fixture with an orphan whose block is then allocated to a new file must catch; clear the error bit at unmount; let a non-recovery build write a partition node, which the ordinary-build write row must catch; scan the partitions after the USB stack, which the boot-order row must catch. Each must fail exactly its check.

### Phase 6: the images and the suites

The fourth partition in `mkdisk_grub.sh`, the sealed image with both roots and the two command lines, `tests/run ext2` in full, the ext2 variants of `sealed`, `install`, `drill` and `selfbuild`, and `tests/run all`.

Proof: every suite passes; `selfbuild` on ext2 builds the kernel twice in clean directories with identical hashes; the install-shaped rows assert both command lines and both mounted roots, the fallback trial, the preserved kernel and the restored menu.

### Phase 7: the machine

The Lenovo, over SSH, on mains power. Nothing is committed until the end. Save the boot menu file and the partition table on the host and on the recovery stick from milestone 39. Present the fourth partition's reflash with the ext2 image, from recovery, with the procedure and the safeguards of milestone 37's phase 6, and if the partition does not exist, present its creation first with that procedure. Then install the new kernel into the candidate slot through `bootctl`, leaving the old kernel and the KRFFS command line as the untouched fallback, and present the `bootctl config` file as a diff that puts `rach2.root=ext2` on the candidate entries only; write it. Trial boot. On the ext2 root: `bootctl status`, `tcc` builds a program, a file survives a reboot of the same trial, `df` shows the volume, SSH works, the keyboard, the mouse and a stick work as before. Then boot the fallback deliberately, which is the old kernel on the KRFFS root, show it comes up, and boot the candidate again. Only then `commit`. `metal_accept.sh` gains the checks.

Proof: the machine runs on ext2 with the KRFFS root one menu entry away and proved; the KRFFS root and recovery partitions' hashes match before and after; the boot partition's change is the predicted list of files.

### Phase 8: the record and the sweep

As in the milestones before, and as large. The documents, every one listed above, read against the code, with a Codex read of the reader-facing ones. The code: what the three file systems share read for what `fatcommon.c` and the VFS should hold; no helper with one caller; the KRFFS window path read for what is now only the RAM images' and said so. The build: every rule, manifest line, fixture and tool listed in `notes.md` with its reason; the port's script and stamp like the others', and its size against the limit; `guestcheck` and `tests/run selfbuild` pass on both roots. The tests: the tier decisions measured; a boot that proves what another proves removed; the matrix and mutation scripts in `notes.md`; the full set twice with no machine left running, and `make test` timed against milestone 40. The record: `notes.md` closed line by line, `dev-log/41-ext2/Readme.md` with its trust statement, and the `dev-log/Readme.md` entry in the same commit. The tree is clean and nothing is pushed.

## Risks, in expected order

* The order of writes having a stop the list does not name, where `e2fsck` discards something. Every stop is repaired on a copy and compared file by file; a lost file at any stop fails the matrix, and the list grows by that stop.
* The walker's symbolic links reaching code that assumed a component is a name: every path-taking syscall goes through the one walker since milestone 36, and `vfsprobe` runs its link rows against every family with the per-operation rules.
* The port of e2fsprogs. It is built and run in phase 1, before anything depends on it, and it is `e2fsck` alone.
* The dirty root on the machine. It mounts read-only rather than risking a repair, recovery repairs it without needing the root, and the fallback root is one menu entry away and is booted on purpose before anything is committed.
* The fourth partition's absence or refusal. Both answers are written, and the milestone closes on QEMU with the record if the machine half cannot run.
* The size. Three parts with two gates, each reviewed before the next begins, and the owner able to stop at either gate with a finished piece.

## Recovery after a compaction

Read `AGENTS.md`, `CLAUDE.md`, this plan's decisions and the current phase, `progress.md`, `current.md`, `notes.md` and `hardware.md`. Run `git status --short --branch` and `git log --oneline -15`, inspect every uncommitted path, and resume the current phase's first unfinished item. Report the one line.

## The goal prompt

`Run milestone 41, the ext2 root, from dev-log/41-ext2/plan.md.`
