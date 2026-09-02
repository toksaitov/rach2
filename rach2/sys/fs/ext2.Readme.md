# ext2

`ext2.c` mounts an ext2 volume through the buffer cache, as `vfat` and `exfat` do, and is what `rach2.root=ext2` boots from in milestone 42's later phases. This document is the reader's contract; the writer's half is added with it.

## What is read

Revision 0 and revision 1 superblocks. Block sizes 1024, 2048 and 4096; inode sizes 128 and 256. Of the feature words the driver knows `filetype` as the one incompatible feature, `sparse_super` and `large_file` as the read-only-compatible ones, and ignores every compatible feature except `dir_index`. An unknown incompatible feature refuses the mount and the record names the bit; an unknown read-only-compatible feature, `dir_index`, and a block size below 4096 each mount the volume read-only with the reason in the record. `dir_index` is read-only because a writer that kept only the linear entries would leave the index stale, and the images this project makes are formatted without it; a small block is read-only because `bcache_write` writes a whole 4 KiB buffer, and two dependent metadata blocks sharing one buffer could not be written in order. A volume whose valid bit is clear or whose error bit is set mounts read-only until the mount-time check of a later phase decides otherwise.

Every field a lookup depends on is checked against the superblock and the group descriptors before it is used. A block a pointer names must lie inside the volume and outside the structural blocks: the boot block, every copy of the superblock and the descriptor table, each group's two bitmaps and its inode table; a pointer into one of those fails the read with `EIO` and a record naming the inode and the block. An inode number must lie within the count. A directory entry must lie within its block with a `rec_len` of at least its eight-byte header and a multiple of four, and its name within the entry; a malformed entry fails the lookup with `EIO` and a record naming the directory and the offset. The volume may have at most 128 groups, 16 GiB at 4 KiB blocks; a larger one is refused by name.

A regular file is read through its direct, indirect, double-indirect and triple-indirect blocks, with a hole read as zeros. The kernel's file offset is 32 bits: an inode whose size does not fit answers `EOVERFLOW` to `open` and `stat`, is not written to, and is left exactly as it is on disk, high word included. A symbolic link shorter than sixty bytes is in the inode's pointer words, a longer one in its first block; `readlink` answers `ENAMETOOLONG` when the buffer cannot hold the target. A device node reports its numbers in `stat` from the first pointer word in the old encoding, or from the second when the first is zero, and cannot be opened, because devices live in `/dev`; that is what `MS_NODEV` means here, and it is enforced by never opening one. Owners, modes, link counts and the three timestamps are the inode's, in seconds, with the year 2038 limit the format has. The owner's high sixteen bits are read from the inode's second half.

`readdir` lists every entry with an inode, `.` and `..` included, with `is_dir` from the entry's type where `filetype` is on and from the inode otherwise. The directory cursor is a byte offset into the directory. `path` rebuilds a directory's path from its `..` entry and the parent's entry that names it, for a directory descriptor's `*at` calls. `statfs` reports the format's magic, the block size, the block count, the free count summed from the group descriptors at mount, and a name limit of 255.

## Symbolic links

The walker follows a symbolic link when it is not the last component. For the last component the operation decides, as on Linux: `open` follows unless `O_NOFOLLOW` is given, in which case a link answers `ELOOP`, or `O_CREAT|O_EXCL` is given, in which case a link, dangling or not, answers `EEXIST`; `stat`, `access`, `chdir`, `execve`, `truncate`, `chmod`, `chown` and `utimensat` without `AT_SYMLINK_NOFOLLOW` follow; `lstat`, `readlink`, `unlink`, `rename`, `link`, `symlink` and `rmdir` do not. A followed link's target replaces the component, an absolute target restarts at the root, and the walk continues from the directory the link was found in, with a bound of forty links along one path, after which `ELOOP`. `..` is physical, so `link/..` goes to the parent of the target, as it does on Linux, and `..` after a link to a file answers `ENOTDIR`. `symlink`, `symlinkat` and `readlinkat` are real syscalls, and `link` and `linkat` are real on ext2, with `EPERM` for a directory, `EXDEV` across mounts and `EROFS` on a read-only mount; on KRFFS and the synthetic types both answer `EPERM`. [vfs.Readme.md](vfs.Readme.md) has the resolver.

## Limits

| Limit | Value |
|-------|-------|
| Block sizes | 1024, 2048, 4096 read; 4096 written |
| Inode sizes | 128, 256 |
| Groups | 128, which is 16 GiB at 4 KiB blocks |
| Largest file | 4 GiB less one, the kernel's 32-bit offset; a larger one answers `EOVERFLOW` and is left as it is |
| Symbolic links along one path | 40 |
| A followed target and the rest of its path | 511 bytes; `ENAMETOOLONG` past that |
| Name | 255 bytes, the format's and the kernel's |
| Timestamps | 32-bit seconds, until 2038 |
| Held nodes per mount | 64, a pool like FAT32's; `ENOMEM` beyond |
| Mounts at once | 4 |

## What stays out

ext3's journal and ext4's extents, 64-bit block numbers, checksums, flexible groups and large inodes: refused or read-only by the feature rules, as the format intends. Extended attributes and access lists: ignored on read. Quotas: no. `mknod`: no, devices live in `/dev`.

## Verification

`tests/run host` runs `ext2_hosttest`: the driver over the buffer cache over volumes the host's `mke2fs` made from a staged tree at 4 KiB blocks with 128- and 256-byte inodes and at 2 KiB, with `debugfs` adding device nodes in both encodings and a size past the kernel's offset; every file reads back against the tree and, on the profile volume, every node against `setidcheck`'s reader; a 1 KiB volume carries a sparse file with a byte behind each pointer level and mounts read-only by name; a `dir_index` volume and an unknown read-only feature mount read-only; an unknown incompatible feature, no magic, a block past the volume, a block inside the descriptors and a malformed entry each answer by name. `tests/run ext2` boots the harness kernel beside a fixture disk `tests/system/mkext2fixture.sh` wrote and runs `ext2probe`'s link rows on the mounted volume ([tests/system/Readme.md](../../../tests/system/Readme.md)).
