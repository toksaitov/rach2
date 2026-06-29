# Rach2 File System (KRFFS v2)

A small hierarchical file system with real directories and the modern `*at` file API a libc builds `FILE*` and `DIR*` on. KRFFS has directories and path resolution, so musl's `fopen`, `stat`, `opendir`, and `mkdir` work and a navigable tree exists: a program can `mkdir` a directory, create and read a file inside it, `chdir` into it and resolve relative names, list it, and `cat /etc/motd` from a tree the host staged into the image.

## Pieces

| File      | Responsibility                                                                                                                                  |
|-----------|-------------------------------------------------------------------------------------------------------------------------------------------------|
| `krffs.*` | The on-disk format, the linear node list, and the v2 path resolver (`krffs_walk`/`krffs_resolve`, `krffs_node_path`, `mkdir`/`rmdir`/`readdir`) |
| `fd.*`    | The per-process descriptor table: type, the resolved node or socket, offset, and flags (READ/WRITE/APPEND/CLOEXEC)                              |

The descriptor, path, directory, and `*at` syscalls live in `../syscall.c`; this directory is the file system itself plus the descriptor table it attaches to.

## On-disk format

KRFFS is a flat linear list of nodes packed into one image: each `struct krffs_node` header (magic, type, id, `name[256]`, mode, uid, gid, three timestamps, `size`, `previous_node_size`, `data_size`, `parent_id`) is followed by its `data_size` bytes, and the next node sits `size` bytes on. The header is byte-identical in the kernel (`krffs.h`) and the host tools (`krffs/krffs_node.h`), so an image written by the host mounts in the guest.

v2 added one field, `parent_id`, at the end of the header (so `sizeof` propagates to every host tool automatically). That single field turns the flat list into a tree:

* A directory is a node whose `mode & S_IFMT == S_IFDIR` (`0040000`); a regular file carries bare permission bits (no type bit), which keeps old flat images readable.
* Every node has a unique id `mix(parent_id, name)`, an FNV-style mix of the parent's id and the name, never zero. Siblings have unique names, so the id is unambiguous and survives a move (it depends on name and parent, not offset).
* The root has the reserved id `KRFFS_ROOT_ID == 0`. Host-created nodes leave `parent_id` zero, so they resolve as root's children with no host change; the root node itself is matched by pointer (`node == fs->node`) since `krffs_find_node_by_id` deliberately never hands back id 0.

## On a real disk

KRFFS mounts three ways, chosen at boot: the RAM-disk multiboot module in place (the default), the same module through the page-cache window (`rach2.fswindow`), or a real ATA disk partition through that window (`rach2.fsdisk`). The file system code is identical in all three; it only ever pointer-walks its node list, and on the window paths that walk is demand-paged.

On the disk path the kernel slices the first KRFFS (MBR type `0x7F`) partition out of the disk and maps it into a fixed kernel-virtual window (`PCWIN_BASE == 0xC0000000`, spanning up to the lowest device MMIO BAR that lands in it and capped at `PC_WINDOW_CAP == 256 MiB`, with the partition clamped to fit) backed by a fixed-size page cache (`../mm/pagecache.c`). A fault in the window polls the missing page in from the disk through the polled ATA driver (`../block/ata.c`); a write dirties it; eviction and `fsync`/`sync` write dirty pages back with a FLUSH CACHE. So `fsync` is honestly durable on the disk path: a written-and-synced file survives a reboot, proved by the two-boot `tests/persistprobe`. On the RAM-module path the image is already as durable as memory, so `fsync` is a no-op. A disk KRFFS must fit the window and KRFFS's 32-bit size, so the host tools cap a partition at 256 MiB and any KRFFS at just under 4 GiB.

## Path resolution

`krffs_walk` is the resolver. It splits a path on `/` and, starting at the root (or a given directory id), matches each component by `name` and `parent_id`, descending only into directories. `.` keeps the current directory; `..` follows the directory's own `parent_id` (and resolves a root parent back to the root rather than to NULL). `krffs_resolve_e` reports whether a failure was a missing component (`-ENOENT`) or a non-directory in the middle of the path (`-ENOTDIR`), which the syscall layer returns verbatim. `krffs_node_path` walks `parent_id` upward to rebuild a node's absolute, canonical path (used by `getcwd`, `chdir`, and dirfd resolution; `..` and `.` collapse for free because it follows real links).

`open`, `stat`, `unlink`, `rename`, `mkdir`, `rmdir`, `getdents64`, and `chdir` all route through the walk, so `/etc`, `/bin`, and a `mkdir`'d subtree are real directories rather than a flat namespace.

## The current directory and the `*at` API

Each process carries an absolute, canonical `cwd` string in its PCB (`"/"` at start, inherited across `fork`). A relative user path is joined onto the cwd at copy-in (`copy_path_cwd`), so the existing root-relative resolver, which already honors `.` and `..`, walks the right tree; an absolute path passes through. `chdir` resolves the target, requires a directory, and stores its canonical path; `getcwd` (183) returns it.

The `*at` calls let a libc build `FILE*` and `DIR*` and resolve against a directory descriptor:

* `openat` (295) and `fstatat64` (300): resolve relative to `AT_FDCWD` (the cwd), an absolute path, or a real directory fd (`copy_path_at` rebuilds the dir's path with `krffs_node_path`).
* `fstat64` (197): `stat` by descriptor; the root, a device, and a socket report `S_IFDIR`, `S_IFCHR`, and `S_IFSOCK`.
* `_llseek` (140): 64-bit seek; KRFFS offsets are 32-bit, so a seek past 4 GB is rejected rather than truncated.
* `fcntl64` (221): `F_DUPFD(_CLOEXEC)`, `F_GETFD`/`F_SETFD` (the close-on-exec bit), `F_GETFL`/`F_SETFL` (access mode and `O_APPEND`).
* `O_CLOEXEC` and `O_APPEND` are honored at `open`; `execve` drops `FD_CLOEXEC` descriptors at its commit point.

## Deliberate simplifications

What KRFFS deliberately leaves out, without compromising the tree structure:

* No users, permissions, or ownership: modes are stored but not enforced; `chmod` and `chown` are effectively no-ops.
* Linear scans: directory lookup and `getdents` walk the node list (no hashing or B-trees), which is fine at teaching scale.
* No hard links, no symlink chains, no journaling. `AT_SYMLINK_NOFOLLOW` is accepted but inert.
* Open-file descriptions are refcounted (the shared seek offset). `fd_entry_t` holds a `struct open_file *`, a small refcounted cell from a fixed pool (`fs/fd.c`) carrying the seek offset, so `dup`/`dup2`/`fork`/`F_DUPFD` share one offset the Unix way (a read or seek through one descriptor is seen by the others), and closing one descriptor leaves the duplicates working until the last reference goes; `release_fd_table` drops every reference when a process dies, so the pool never leaks. The backing object (KRFFS node or socket) and `FD_CLOEXEC` stay per-descriptor (a socket is refcounted per fd; `dup`/`dup2` clear the new fd's CLOEXEC, `execve` closes the marked ones). Probe: `tests/ofdprobe.c` (`OFDPROBE OK`) and the musl `tests/musl/ofdmusl.c` (`OFDMUSL OK`). One caveat remains: a directory fd is still not pinned against `rmdir` (a stale dirfd is rejected defensively by a round-trip check); pinning the inode in the description is a later refinement.
* The interactive host FUSE mount stays flat. Images gain directories through `krffs/stageimage.krffs` (the manifest-driven builder, which writes v2 directory nodes directly) and the in-guest `mkdir`; teaching the 1100-line FUSE mount to browse directories is a later convenience, not needed by the kernel or the integration.

## Testing

* `tests/dirprobe`: `mkdir`, nested create/write/read, duplicate-`mkdir` and create-in-missing-dir rejection, non-empty `rmdir` refusal, `getdents` listing exactly a subdir's children, the `*at` calls (`openat`, `fstat64`, `fstatat64`, `_llseek`, `fcntl64`), `O_APPEND`/`F_DUPFD`/`O_CREAT`-mode, `chdir` plus `getcwd` plus relative resolution plus a dirfd-relative `openat`, `ENOTDIR` versus `ENOENT`, and `/x/..` resolving to the root. Prints `DIRPROBE OK`.
* `tests/musl/catfile`: a musl program that reads a host-staged `/etc/motd`, `opendir`s `/etc`, then `mkdir`s a directory, writes and reads a file inside it, `stat`s it, lists it, and cleans up, all through `<stdio.h>` and `<dirent.h>`. Prints `CATFILE OK`.
* `krffs/stageimage.krffs` stages an `/etc` tree (`/etc/motd`, `/etc/hostname`) into an image from its manifest at build time, proving the host tools speak the hierarchical format.
