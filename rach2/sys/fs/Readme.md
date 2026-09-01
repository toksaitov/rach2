# KRFFS and file descriptors

KRFFS is the kernel's writable file system. It stores a hierarchical namespace in one contiguous region and uses a first-fit extent allocator. The same code can address a RAM module directly, a RAM module through the page-cache window, or a partition on an ATA or AHCI disk.

`krffs.c` and `krffs.h` implement the on-disk format, allocator, namespace, and file data operations. `fd.c` and `fd.h` implement descriptor tables and shared open-file descriptions. The syscall-facing permission, descriptor, and metadata rules are in `../syscall.c`. The mount table and the resolver every path goes through are described in [vfs.Readme.md](vfs.Readme.md), the kernel-generated `/dev` and `/proc` namespaces in [synthfs.Readme.md](synthfs.Readme.md), the read-only FAT32 type in [fat32.Readme.md](fat32.Readme.md), and the page cache and disk drivers in `../mm/Readme.md` and `../block/Readme.md`.

## On-disk layout

The region is a circular, doubly linked sequence of extents. Every extent starts with the same packed 357-byte header. Its fields are the magic and node type; ID and 256-byte name buffer; mode, uid, gid, and three timestamps; total extent size, preceding extent size, data size, and parent ID. File data follows the header.

There are three node types: root, free, and reserved. A reserved node represents either a regular file or a directory. Directory modes include `S_IFDIR`; regular-file modes store only the permission and special bits. The root is a separate node with logical ID 0 and a stored mode of 0755 without `S_IFDIR`. Stat and permission code therefore handle the root by identity.

Mount validation requires a root node with the KRFFS magic and root type. It then walks the complete extent sequence and checks each node's magic, type, size, preceding-size link, bounds, exact tiling of the region, and closure back to the root. A malformed sequence fails validation before the mount is published.

A child records its containing directory's ID in `parent_id`. Lookup scans the node sequence for a matching `(parent_id, name)` pair. A directory's ID is a 64-bit mix of its parent ID and name. The implementation does not detect ID collisions. Regular-file IDs are not consistent between creation routes: the host stager uses the parent-aware mix for every staged node, while guest creation and rename use the older DJB hash of the basename. The kernel path resolver does not use a regular file's ID, but `stat` and `getdents64` report it as the inode number.

Names may contain at most 255 bytes. The syscall pathname buffers hold at most 255 bytes plus the terminating NUL. A process's absolute current directory is smaller: 63 bytes plus NUL. Reconstructing a path from parent IDs also stops after 64 components.

## Allocation

Allocation scans for the first free extent large enough for the header and requested data. It splits a larger extent when the remainder can hold another header. Freeing an extent coalesces adjacent free extents and repairs the backward link of the following node.

Growth first consumes an adjacent free extent. Otherwise it allocates a new extent, copies the node and its data, and frees the old extent. The write path then repoints every regular-file descriptor in every process from the old node to the new one. Sparse growth through write or truncate zero-fills the gap. A short remainder that cannot hold a header stays in the allocated extent.

A write that cannot grow the file separates its two reasons. No free extent large enough returns `KRFFS_ENOSPACE`, which the write syscall reports as `ENOSPC`; a rejected argument returns `KRFFS_EBAD` and `EIO`. A caller can therefore tell a full file system from a failed write.

## Paths and directories

Every path-taking syscall resolves through the mount table's resolver, one component at a time, with `..` resolved physically ([vfs.Readme.md](vfs.Readme.md)). KRFFS answers each step with one lookup of `(parent_id, name)`; its `..` is the node whose ID is the directory's `parent_id`, and the root's `..` is the root.

Each process starts with `cwd` set to `/` and inherits it across `fork`. Relative paths resolve against that string; a working directory inside a mount placed by `mount(2)` also holds that mount. `openat` and the other implemented `*at` calls accept `AT_FDCWD`, an absolute path, or a real directory descriptor. A directory descriptor is resolved back to its current absolute name before use; it is not a capability that bypasses ancestor permissions.

Path resolution checks execute permission on every directory entered. Ordinary calls use effective IDs. `access` and `faccessat` use real IDs for both the prefixes and the target. The target's own read, write, or execute permission is checked separately by the operation that needs it.

KRFFS directory reads return only stored children. They do not synthesize `.` or `..` entries. Lookup and enumeration are linear scans. `getdents64` stores the byte offset of the last returned node plus one in the open-file description. Deleting entries between batches does not renumber the remaining nodes. Creating or relocating a child during enumeration may make that child appear twice or not at all; concurrent namespace modification during a directory read is otherwise unspecified.

KRFFS has no hard links or symlink nodes. `link` and `linkat` validate their arguments and source, then return `EPERM`. Symbolic links exposed under `/proc` belong to the synthetic file system. Renaming or moving a directory returns `EPERM` because its children refer to its current ID. Files may be renamed within or across directories, and an existing regular-file destination may be replaced.

## Descriptor state and lifetime

Each process has 16 descriptor slots. The global open-file-description pool has 256 entries, enough for 16 processes with 16 distinct descriptions each. An open-file description contains the shared 32-bit file offset, a separate 64-bit offset for raw disk windows, the `O_APPEND` and `O_NONBLOCK` status flags, and a reference count.

A description on a mounted file system also holds its mount, its node, and the file system's per-open handle; the file system's `open` runs once per successful `open(2)` and its `release` once at the last close, and the record written at unmount says both counts. `read`, `write`, `lseek` and `ftruncate` hold the description for the length of the operation, so two processes sharing one description cannot both read the same offset while one sleeps in a disk request.

`dup`, `dup2`, `dup3`, `F_DUPFD`, `F_DUPFD_CLOEXEC`, and `fork` share the open-file description. A seek, append setting, nonblocking setting, or read through one duplicate is visible through the others. `FD_CLOEXEC` is per descriptor. Ordinary duplication clears it on the new descriptor; `dup3(O_CLOEXEC)` and `F_DUPFD_CLOEXEC` set it. A successful `execve` closes marked descriptors.

A descriptor open on a regular file continues to name that file after unlink or replacement by rename. If a name is removed while any descriptor holds its node, KRFFS clears the node's name and keeps the extent as an orphan. Orphans are absent from lookup, directory reads, debug listings, file counts, and directory emptiness checks. The final close frees the extent. Process exit releases every open descriptor and therefore performs the same final-close work.

Regular-file relocation preserves the same lifetime across all process tables. Directories are deliberately different: `rmdir` may free a directory that still has an open descriptor. Later `getdents64` or relative `*at` use through that descriptor returns `EBADF`.

## Modes, ownership, and times

New files and directories receive the creator's effective uid and gid. Their requested permission bits are masked by the process umask. The kernel enforces the owner, group, and other permission bits; effective uid 0 overrides them except that executing a regular file still requires at least one execute bit.

Creating, removing, or renaming an entry requires write permission on its parent directory. A sticky directory additionally restricts removal and both ends of replacement to root, the directory owner, or the affected entry's owner. The parent write check runs first, so a caller without it receives `EACCES` rather than the sticky check's `EPERM`.

Setuid and executable setgid files change the effective and saved IDs at exec. A non-root write or truncate clears setuid and clears setgid when group execute is set. A change of owner or group applies the same clear even for root. A no-change `chown` changes neither mode nor timestamps. An unprivileged owner cannot set setgid on a regular file whose group is not one of that process's groups.

Creation initializes atime, mtime, and ctime. Data changes advance mtime and ctime; metadata changes advance ctime; directory entry changes advance the containing directory's mtime and ctime. Reads do not advance atime. `utimensat` can set atime and mtime explicitly and advances ctime when it changes them. The root's timestamps are reported but `utimensat` returns success without changing them.

There are no ACLs, quotas, file capabilities, hard-link support, link-count tracking, or journal. Stat reports one link for every KRFFS node. The advisory-lock `fcntl` operations report no conflict and grant every lock request.

## Mounts and durability

The root can be mounted in three ways:

* The default mounts the multiboot RAM module directly. No page-cache backing is installed.
* `rach2.fswindow` maps the RAM module through the demand-paged window.
* `rach2.fsdisk` or `rach2.root=disk` maps a disk partition through that window.

The window starts at `0xD0000000`. Its span ends at the lowest device-memory BAR that falls within it and is capped at 512 MiB. A disk partition must be nonempty, page-aligned, contained within the disk, and no larger than the available span. Disk discovery examines the four nonempty MBR entries in order and selects the first whose first sector has the KRFFS magic, root type, and a plausible root extent. It does not select by the MBR type byte. The complete mount validator then checks the extent sequence.

`fsync` validates the descriptor and flushes the entire page cache, not one file. `sync` performs the same whole-cache writeback but always returns success. On a direct RAM mount the cache is empty. On a windowed RAM mount, writeback copies dirty pages into the in-memory module; the result still does not survive a reboot. On a disk mount, eviction and explicit writeback reach the selected ATA or AHCI partition, so synchronized data persists.

A disk mount sets the root node's dirty bit and writes it through before it repairs any orphan. It then reclaims all nameless reserved nodes, whether or not the previous dirty bit was set. Clean shutdown first flushes all dirty data, then clears the bit and flushes again. `sync` alone does not clear it. The root dirty bit shares the stored position of setuid, so permission and mode paths exclude the root by identity.

`krffs/mkdisk.sh` limits a generated disk partition to the 512 MiB window cap. The actual kernel limit may be smaller when a device BAR shortens the window. The host KRFFS tools also accept standalone regions up to 4095 MiB; that larger host limit does not make such a region mountable through the kernel window.

## mount and umount2

The root is placed by the boot path and cannot be unmounted. `mount(2)` and `umount2(2)`, the mount table, the claims on a mounted partition's sectors, and the buffer cache the mounted types read through are described in [vfs.Readme.md](vfs.Readme.md).

## Tests

The default `make test` run includes the basic file, directory, metadata, rename, and listing probes. `dirprobe`, `dirgrow`, `unlinkdirprobe`, and `pathcanon` cover hierarchy, `*at` resolution, relocation, deletion during enumeration, and canonicalization. `fdstale` covers regular-file lifetime, orphan visibility and release, cross-process holders, relocation repointing, and the open-directory exception. `ofdprobe` covers shared offsets, fork and duplication, close-on-exec state, and pool reuse.

`ownprobe`, `permprobe`, `modeprobe`, `searchprobe`, and `stickyprobe` cover ownership, umask, permission checks, set-ID changes, ancestor search, and sticky directories. The default run also uses the ported musl for its stat, timestamp, identity, and permission cases.

`tests/musl/catfile.c` and `tests/musl/ofdmusl.c` are built as `/musl/catfile` and `/musl/ofdmusl` in the test image but are not in the default runner. They provide manual end-to-end checks of musl stdio and directory calls, and of a duplicated stdio descriptor's shared offset.

`tests/run diskfull` writes into a disk-backed root until the write is refused, requires `ENOSPC` and requires the file to hold what was written before the refusal, then unlinks it and writes again to require the space back.

`tests/run persist` and `tests/run persist-ahci` boot the same disk image twice and require a file written through `fsync` and `sync` on the first boot to be read back on the second. `tests/run host` includes `readfile_hosttest`, which checks the host extractor's bounded path walk and malformed-image refusals; it does not exercise the kernel allocator or namespace implementation.
