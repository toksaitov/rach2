# tmpfs

`tmpfs.c` is a file system in memory, the type `tmpfs` of `mount(2)`: it has no device, and its files go when it is unmounted. The kernel mounts one at `/dev/shm` at boot, where the C libraries' `shm_open` and `sem_open` make their files, and keeps a second, which no path reaches, for the files of `memfd_create(2)` and the segments of System V shared memory. It has every kind of node: regular files, directories, symbolic links, hard links, FIFOs, socket files and device nodes.

## Interfaces

[mount(2)](../../../man/man2/mount.2) gives the options `mode=` and `size=`, the remount and what `statfs(2)` reports; [memfd_create(2)](../../../man/man2/memfd_create.2) and [fcntl(2)](../../../man/man2/fcntl.2) give memfds and seals; and [fallocate(2)](../../../man/man2/fallocate.2) gives allocation and holes. To the rest of the kernel `tmpfs.h` gives `vfs_tmpfs_type`; `tmpfs_shmem`, a regular file's bytes, which a shared mapping maps (`mmap` in `../syscall_proc.c`, and `../ipc/sysv.c`); `tmpfs_memfd`, which makes a file in no directory; `tmpfs_seals` and `tmpfs_add_seals`; and `tmpfs_label`, the name `/proc/PID/fd` shows for a memfd. Everything else is reached through the operations table of [vfs.Readme.md](vfs.Readme.md).

## Design

### Nodes

A node (`struct tmpfs_node`) is a record from the heap. Inode numbers count up from 1, the root's, and are not reused while the mount lasts. A new node takes the caller's effective ids, but the group of a set-group-ID directory it is made in, which also gives a new directory the bit, as Linux's `inode_init_owner` does.

Two counts keep a node. `refs` counts the holds of lookups, descriptions, working directories and mounts, and for a directory one for each subdirectory, through the subdirectory's `parent`; `links` counts the names. `settle` frees a node when both are 0 and lets go of its parent, which may free the parent in turn, in a loop. So a file unlinked while open stays readable until its last close, and a removed directory that a process still has keeps its removed parent, which its `..` lists and looks up. A directory reports two links and one per subdirectory, and a size of 20 bytes for each name and for `.` and `..` (`DIRENT_BYTES`), as Linux's tmpfs does.

### Directories

A directory's names are a list of `struct tmpfs_dirent` in the order they were made or renamed into it, each with a place, `off`, from 2 up, that stays while the name is there. A listing's cursor is 0 for `.`, 1 for `..`, and then one past the place of the last name returned, so a name removed during a listing moves no other and `rm -r`, which removes names as it lists them, misses none; a name renamed during a listing takes a new place at the end and may be listed again. When the next place would be `0xFFFFFFFF` the names are numbered again from 2 (`renumber`). A lookup scans the list, and so does each step of a listing, from the head to its cursor.

`tmpfs_rename` replaces a file or an empty directory, and answers `EINVAL` for a directory moved below itself. It takes the new name's entry from the heap before it changes either directory, so a rename the heap cannot serve answers `ENOSPC` and changes nothing.

### File bytes

A regular file's bytes are a `struct shmem` (`../mm/shmem.c`, in [../mm/Readme.md](../mm/Readme.md)): a directory of leaves of `SHMEM_LEAF`, 1024, frames, a page made when it is first written or mapped, so a hole costs nothing, up to `SHMEM_MAX_SIZE`, 16 TiB less a page, on both architectures. A shared mapping maps the file's frames, and a truncation takes the pages it frees out of every mapping first. Each mount counts its files' pages against its limit (`struct shmem_quota`), half the machine's frames unless `size=` gives another, and a page past it fails a write with `ENOSPC`.

### Calls and locking

`vfs_tmpfs_type` sets `ns_serialised`, so the calls that use a description of a tmpfs file, a read, a write and a truncation among them, take the mount's namespace lock before the description's ([Readme.md](Readme.md)); the operations take no lock of their own. A read advances the access time. `tmpfs_open` answers `ENXIO` for a device node, since `open(2)` opens the `/dev` device with the node's number before it calls the type. `fsync`, `sync` and the first half of an unmount do nothing and succeed; the unmount then frees the tree in a loop over the parent links (`free_tree`), not by recursion, since a tree may be as deep as a path is long and the kernel stack is 16 KiB.

### The kernel's two mounts

`/dev/shm` is entry 3 of the permanent mounts, with the mode and flags Linux systems give it ([vfs.Readme.md](vfs.Readme.md) has the table). `/dev` is synthetic and has no directory to cover, so `/dev/shm` is a nameless mount in `/dev`'s root directory, entered by its name, and `umount2` of it fails with `EBUSY`.

Entry 4, mounted with `mode=0555,size=0`, has no parent and no path, and `/proc/mounts` does not list it (`vfs_memfd_mount`). Its root stays empty. Each memfd, and each System V segment, a file named `SYSV`, is a node in no directory (`tmpfs_memfd`), made with one hold that its description takes, so it goes with the description's last reference as an unlinked file does.

### Seals

A regular file's seals are the `TMPFS_SEAL_` bits, which have the values of `fcntl`'s `F_SEAL_` bits and are only added (`tmpfs_add_seals`). A file starts with `F_SEAL_SEAL`, a memfd with the seals its flags give. Each seal is tested where its change is made, and the change fails with `EPERM`: a write under the two write seals, or past the end under `F_SEAL_GROW`, in `tmpfs_write`, which writes nothing then; a size the shrink or grow seal forbids in `tmpfs_truncate` and `tmpfs_setattr`, and a change of the execute bits under `F_SEAL_EXEC` in `tmpfs_setattr`; and a growth or a hole the seals forbid in `tmpfs_fallocate`. Under `F_SEAL_GROW`, `writev_begin` in `../syscall_fs.c` fails a vector that would pass the end before its first element is written, and holds the mount's lock across the elements so that nothing comes between the test and them. Sealing a file that has execute bits with `F_SEAL_EXEC` adds the shrink, grow, write and future-write seals, as on Linux.

## Limits

* The files are in memory only: an unmount or a reboot loses them.
* The seals do not reach a shared mapping: `mmap(2)` maps a file sealed against writing writable, a mapping made before the seal goes on writing, and `F_SEAL_WRITE` is added while one exists. On Linux the three fail.
* The only limit is the mount's pages; there is no count of inodes and no per-user quota.
* A lookup and a rename take time in proportion to the names in the directory, and a whole listing in proportion to their square.

## Tests

* `tests/run host` runs `tmpfs_hosttest`, which includes `tmpfs.c` and `../mm/shmem.c` over a pool of host pages and covers file bytes and holes, links, renames, the kinds of node, listings, held and removed nodes, the mount's limit, `fallocate`, truncation of mapped pages, and memfds and seals.
* `tests/run default` runs `musl/tmpfsmusl` (a mount of its own, its unmount, remount and binds, and `/dev/shm`), `musl/memfdmusl` (each flag and seal), `musl/mknodmusl` and `musl/chrootmusl`.
