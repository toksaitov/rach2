# KRFFS and file descriptors

KRFFS is the kernel's own writable file system: the RAM images' and the fallback root's, since the everyday root on a disk is an ext2 partition ([ext2.Readme.md](ext2.Readme.md)). It stores a hierarchical namespace in one contiguous region and uses a first-fit extent allocator. The same code can address a RAM module directly, a RAM module through the page-cache window, or a partition on an ATA or AHCI disk.

`krffs.c` and `krffs.h` implement the format, the allocator, the namespace and the file operations, and `krffsmount.c` is the type `mount(2)` places over a block device; the format, its two backends and the order in which a change reaches the medium are in [krffs.Readme.md](krffs.Readme.md). `fd.c` and `fd.h` implement descriptor tables and shared open-file descriptions. The syscall-facing permission, descriptor, and metadata rules are in `../syscall_fs.c`. The mount table and the resolver every path goes through are described in [vfs.Readme.md](vfs.Readme.md), the kernel-generated `/dev` and `/proc` namespaces in [synthfs.Readme.md](synthfs.Readme.md), the FAT32 type, which reads and writes, in [fat32.Readme.md](fat32.Readme.md), and the page cache and disk drivers in `../mm/Readme.md` and `../block/Readme.md`.

## Paths and directories

Every path-taking syscall resolves through the mount table's resolver, one component at a time, with `..` resolved physically ([vfs.Readme.md](vfs.Readme.md)). KRFFS answers each step with one lookup of `(parent_id, name)`; its `..` is the node whose ID is the directory's `parent_id`, and the root's `..` is the root.

Each process starts with `cwd` set to `/` and inherits it across `fork`. Relative paths resolve against that string; a working directory inside a mount placed by `mount(2)` also holds that mount. `openat` and the other implemented `*at` calls accept `AT_FDCWD`, an absolute path, or a real directory descriptor. A directory descriptor is resolved back to its current absolute name before use; it is not a capability that bypasses ancestor permissions. `fchdir` makes that name the working directory.

A path may be 4096 bytes with its terminator, Linux's `PATH_MAX` (`VFS_PATH_MAX`). A call copies each path it is given into a page from `pathbuf.c` and joins a relative one to the working directory, or to the path of the directory a descriptor is open on, in place; the joined path is held to the same bound, where Linux bounds only the path the caller passed, and past it the call answers `ENAMETOOLONG`. Each process has up to eight such pages, taken the first time one of its calls needs that many at once and kept for the record's life, so a path costs a frame the first time and never after, and the count of frame allocations `rach2.framefail` keeps does not move with the paths a program names. A call takes four at most: a rename's two paths, and the target of a link the resolver follows with the path it joins the target to. The working directory's path is kept in the process record when it is shorter than 64 bytes, and in the heap up to the same 4096 when it is longer.

KRFFS, the boot root and a mounted volume alike, names a node it creates, removes or renames by its path within the volume, built in 256 bytes and climbing at most 64 directories, so on KRFFS a name whose path is longer or deeper answers `ENAMETOOLONG`; ext2 and the FAT types have no such bound of their own.

Path resolution checks execute permission on every directory entered. Ordinary calls use effective IDs. `access` and `faccessat` use real IDs for both the prefixes and the target. The target's own read, write, or execute permission is checked separately by the operation that needs it.

KRFFS directory reads return only stored children. They do not synthesize `.` or `..` entries. Lookup and enumeration are linear scans. `getdents64` keeps its place in the open-file description: the boot root stores the last returned node's offset plus one, and a mounted volume the next node's offset, with an end of its own. Deleting entries between batches does not renumber the remaining nodes. Creating or relocating a child during enumeration may make that child appear twice or not at all; concurrent namespace modification during a directory read is otherwise unspecified.

KRFFS has no hard links or symlink nodes. `link` and `linkat` validate their arguments and source; a read-only mount answers `EROFS` first, and a writable one `EPERM`, since no KRFFS type has the operation. Symbolic links exposed under `/proc` belong to the synthetic file system. Renaming or moving a directory is refused because its children refer to its current ID: the boot root answers `EPERM` and a mounted volume `EIO`. Files may be renamed within or across directories, and an existing regular-file destination may be replaced.

## Descriptor state and lifetime

Process allocation initializes three console descriptors, each with a separate open-file description. If any description cannot be allocated, initialization releases the partial set and process creation fails before publishing the process. Each process has a descriptor table allocated from the kernel heap. It starts with 16 entries and doubles as it fills, up to 1024, the width of a `select` set; an open past that limit returns `EMFILE`. Open-file descriptions are allocated separately from the heap, one per open, without a fixed pool. An open-file description contains the shared 64-bit file offset, a separate 64-bit offset for raw disk windows, the `O_APPEND` and `O_NONBLOCK` status flags, and a reference count.

A description on a mounted file system also holds its mount, its node, and the file system's per-open handle; the file system's `open` runs once per successful `open(2)` and its `release` once at the last close, and the record written at unmount says both counts. `read`, `write`, `lseek` and `ftruncate` hold the description for the length of the operation, so two processes sharing one description cannot both read the same offset while one sleeps in a disk request; `pread64` and `pwrite64` hold it on a regular file too and leave its offset alone.

Mounted descriptor operations take the mount namespace lock before the description lock. Append selects EOF under both locks, including across independently opened descriptions.

`dup`, `dup2`, `dup3`, `F_DUPFD`, `F_DUPFD_CLOEXEC`, and `fork` share the open-file description. A seek, append setting, nonblocking setting, or read through one duplicate is visible through the others. `FD_CLOEXEC` is per descriptor. Ordinary duplication clears it on the new descriptor; `dup3(O_CLOEXEC)` and `F_DUPFD_CLOEXEC` set it. A successful `execve` closes marked descriptors.

A descriptor open on a regular file continues to name that file after unlink or replacement by rename. If a name is removed while any descriptor holds its node, KRFFS clears the node's name and keeps the extent as an orphan. Orphans are absent from lookup, directory reads, debug listings, file counts, and directory emptiness checks. The final close frees the extent. Process exit releases every open descriptor and therefore performs the same final-close work.

Regular-file relocation preserves the same lifetime across all process tables. Directories are deliberately different: `rmdir` may free a directory that still has an open descriptor. On the boot root, later `getdents64` or relative `*at` use through that descriptor returns `EBADF`. A mounted volume has no such check and does not need one: it refuses to remove a directory anybody has open.

## Modes, ownership, and times

New files and directories receive the creator's effective uid and gid. Their requested permission bits are masked by the process umask. The kernel enforces the owner, group, and other permission bits; effective uid 0 overrides them except that executing a regular file still requires at least one execute bit.

Creating, removing, or renaming an entry requires write permission on its parent directory. A sticky directory additionally restricts removal and both ends of replacement to root, the directory owner, or the affected entry's owner. The parent write check runs first, so a caller without it receives `EACCES` rather than the sticky check's `EPERM`.

Setuid and executable setgid files change the effective and saved IDs at exec. A non-root write or truncate clears setuid and clears setgid when group execute is set. A change of owner or group applies the same clear even for root. A no-change `chown` changes neither mode nor timestamps. An unprivileged owner cannot set setgid on a regular file whose group is not one of that process's groups.

Creation initializes atime, mtime, and ctime. Data changes advance mtime and ctime; metadata changes advance ctime; directory entry changes advance the containing directory's mtime and ctime. Reads do not advance atime. `utimensat` can set atime and mtime explicitly and advances ctime when it changes them. The root's timestamps are reported but `utimensat` returns success without changing them.

There are no ACLs, quotas, file capabilities, hard-link support, link-count tracking, or journal. The boot root reports one link for every node; a mounted volume reports two for a directory and one for a file. The advisory-lock `fcntl` operations report no conflict and grant every lock request.

## Mounts and durability

The root can be mounted in three ways:

* The default mounts the multiboot RAM module directly. No page-cache backing is installed.
* `rach2.fswindow` maps the RAM module through the demand-paged window.
* `rach2.fsdisk` or `rach2.root=disk` maps a disk partition through that window.

The window starts at `0xD0000000`. Its span ends at the lowest device-memory BAR that falls within it and is capped at 512 MiB. A disk partition must be nonempty, page-aligned, contained within the disk, and no larger than the available span. Disk discovery examines the four nonempty MBR entries in order and selects the first whose first sector has the KRFFS magic, root type, and a plausible root extent. It does not select by the MBR type byte. The complete mount validator then checks the extent sequence.

`fsync` validates the descriptor; on a mounted file it runs that type's `fsync`, and on a boot-root file it flushes the whole page cache rather than one file. `sync` runs every active mount's `sync` and then the page cache's writeback, and always returns success. On a direct RAM mount the cache is empty. On a windowed RAM mount, writeback copies dirty pages into the in-memory module; the result still does not survive a reboot. On a disk mount, eviction and explicit writeback reach the selected ATA or AHCI partition, so synchronized data persists.

A disk-backed boot root sets the root node's dirty bit and writes it through before it repairs any orphan, and then reclaims all nameless reserved nodes, whether or not the previous dirty bit was set. A volume mounted by `mount(2)` repairs only what a dirty flag says was left behind, and sets its own flag after. Clean shutdown first flushes all dirty data, then clears the bit and flushes again. `sync` alone does not clear it. The root dirty bit shares the stored position of setuid, so permission and mode paths exclude the root by identity.

`krffs/mkdisk.sh` limits a generated disk partition to the 512 MiB window cap. The actual kernel limit may be smaller when a device BAR shortens the window. The host KRFFS tools also accept standalone regions up to 4095 MiB; that larger host limit does not make such a region mountable through the kernel window.

## mount and umount2

The root is placed by the boot path and cannot be unmounted. `mount(2)` and `umount2(2)`, the mount table, the claims on a mounted partition's sectors, and the buffer cache the mounted types read through are described in [vfs.Readme.md](vfs.Readme.md).

## Tests

The default `make test` run includes the basic file, directory, metadata, rename, and listing probes. `dirprobe`, `dirgrow`, `unlinkdirprobe`, and `pathcanon` cover hierarchy, `*at` resolution, relocation, deletion during enumeration, and canonicalization. `fdstale` covers regular-file lifetime, orphan visibility and release, cross-process holders, relocation repointing, and the open-directory exception. `ofdprobe` covers shared offsets, fork and duplication, close-on-exec state, and pool reuse.

`ownprobe`, `permprobe`, `modeprobe`, `searchprobe`, and `stickyprobe` cover ownership, umask, permission checks, set-ID changes, ancestor search, and sticky directories. The default run also uses the ported musl for its stat, timestamp, identity, and permission cases.

`tests/musl/catfile.c` and `tests/musl/ofdmusl.c` are built as `/musl/catfile` and `/musl/ofdmusl` in the test image but are not in the default runner. They provide manual end-to-end checks of musl stdio and directory calls, and of a duplicated stdio descriptor's shared offset.

`tests/run diskfull` writes into a disk-backed root until the write is refused, requires `ENOSPC` and requires the file to hold what was written before the refusal, then unlinks it and writes again to require the space back.

`tests/run persist` and `tests/run persist-ahci` boot the same disk image twice and require a file written through `fsync` and `sync` on the first boot to be read back on the second. `tests/run host` includes `readfile_hosttest`, which checks the host extractor's bounded path walk and malformed-image refusals, and `krffs_hosttest`, which runs the kernel's own allocator, namespace and file operations over a backend of its own.
