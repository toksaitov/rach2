# The file system layer

The file system layer is the part of the kernel between the system calls on files and descriptors and the file systems that keep them. It keeps each process's descriptor table and the open-file descriptions its descriptors share (`fd.c`, `fd.h`, `file.h`), makes the paths the calls are given absolute for the resolver (`pathbuf.c` and `../syscall_fs.c`), applies the rules of permission, ownership and time, and defines what reaches the medium at `fsync`, `sync`, unmount and `reboot`. The mount table, the resolver and each file system have documents of their own:

* [vfs.Readme.md](vfs.Readme.md): the mount table, the operations every type provides, `mount(2)` and `umount2(2)`, binds, the resolver, a process's root and working directory, claims on a disk's sectors, and the buffer cache.
* [krffs.Readme.md](krffs.Readme.md): KRFFS, the root of the RAM images, which the boot line or `mount(2)` can also place on a disk partition.
* [ext2.Readme.md](ext2.Readme.md), [fat32.Readme.md](fat32.Readme.md) (the type `vfat`) and [exfat.Readme.md](exfat.Readme.md): the disk formats of other systems.
* [tmpfs.Readme.md](tmpfs.Readme.md): tmpfs, `/dev/shm`, and the files of `memfd_create(2)`.
* [synthfs.Readme.md](synthfs.Readme.md): `/dev`, the raw disk windows, `/proc` and `/sys`.
* [lock.Readme.md](lock.Readme.md): the advisory locks of `fcntl(2)` and `flock(2)`.
* [../ipc/Readme.md](../ipc/Readme.md): the POSIX message queues at `/dev/mqueue`.
* [../mm/Readme.md](../mm/Readme.md): the page cache under the KRFFS root and the mappings of files; [../block/Readme.md](../block/Readme.md): the disks.

## Interfaces

The system calls are in `../syscall_fs.c`, and the manual pages give their contracts: [open(2)](../../../man/man2/open.2) (with `close` and `O_PATH`), [dup(2)](../../../man/man2/dup.2), [fcntl(2)](../../../man/man2/fcntl.2), [read(2)](../../../man/man2/read.2), [write(2)](../../../man/man2/write.2), [lseek(2)](../../../man/man2/lseek.2), [getdents64(2)](../../../man/man2/getdents64.2), [stat(2)](../../../man/man2/stat.2), [statfs(2)](../../../man/man2/statfs.2), [access(2)](../../../man/man2/access.2), [chmod(2)](../../../man/man2/chmod.2), [chown(2)](../../../man/man2/chown.2), [umask(2)](../../../man/man2/umask.2), [chdir(2)](../../../man/man2/chdir.2) (with `fchdir` and `getcwd`), [chroot(2)](../../../man/man2/chroot.2), [mkdir(2)](../../../man/man2/mkdir.2) (with `rmdir`), [mknod(2)](../../../man/man2/mknod.2), [link(2)](../../../man/man2/link.2) (with `unlink`), [symlink(2)](../../../man/man2/symlink.2), [rename(2)](../../../man/man2/rename.2), [truncate(2)](../../../man/man2/truncate.2), [fallocate(2)](../../../man/man2/fallocate.2), [sync(2)](../../../man/man2/sync.2) (with `fsync` and `syncfs`), [getxattr(2)](../../../man/man2/getxattr.2) and [gettimeofday(2)](../../../man/man2/gettimeofday.2) (with `utimensat`). The descriptor limit is in [limits(7)](../../../man/man7/limits.7) and `/proc/PID/fd` in [proc(5)](../../../man/man5/proc.5).

The rest of the kernel uses `fd.h` (the tables, entries and descriptions; `fd_alloc`, `fd_get`, `fd_ofd_ref`, `fd_ofd_unref` and the description lock `fd_ofd_lock`); `file.h` (`struct file_ops`, the read, write, poll, ioctl and close that a pipe, a pseudo-terminal, a device, a `/proc` file, an AF_UNIX socket or an anonymous descriptor such as an epoll set installs on its entry); `pathbuf.h` (`path_buf_get`, `path_buf_put` and `proc_cwd`); and from `syscall_internal.h` `resolve_named`, `copy_path_cwd` and `canonicalize_named` for `execve`, the permission checks, `fd_release_backing`, and `fd_holds_release`, which the dispatcher calls as each call returns.

## Design

### Descriptor tables

A process's record points to its table (`fd_table_t`), whose entries come from the heap. A table starts with `FD_TABLE_FIRST`, 16, entries and doubles as it fills, to at most `FD_TABLE_LIMIT`, 1024, the width of a `select` set; a caller is given no descriptor at or past its soft `RLIMIT_NOFILE` (`fd_limit`). `clone` with `CLONE_FILES` shares a table and counts its sharers in `refs`. A table first shared grows to all 1024 entries (`fd_table_share`), so that no entry moves while one sharer's call sleeps with a pointer to it and another opens files. `fork` gives the child a copy whose descriptors take another reference to the same descriptions (`files_copy` in `../syscall_proc.c`); `execve`, `close_range` with `CLOSE_RANGE_UNSHARE` and `unshare` with `CLONE_FILES` give a sharer a copy of its own (`proc_files_unshare`); and the last sharer to go closes every descriptor (`release_fd_table`).

An entry has its type (`fd_type_t`), what it is open on, its description, and its own flags: read, write, close-on-exec, `O_PATH`, and the two closing marks below. An entry with a `struct file_ops` is dispatched through it, and the others by their type: the files and directories of every file system, the console, network sockets, the framebuffer and the mouse. A new process has descriptors 0, 1 and 2 on the console, each open for reading and writing with a description of its own (`fd_init_stdio`), as Linux gives init `/dev/console`.

`fd_alloc` reserves a slot by giving it a new description and no type. A reserved slot is no descriptor to `fd_get`, to `fd_alloc` or to a copy of the table. An open reserves its descriptor before it resolves the path and gives the entry its type as its last step (`do_open`, `open_install`), so an open that can get no descriptor has created and truncated nothing.

A call on a shared table keeps every descriptor it looks up: `fd_get` sets the descriptor's bit in the calling thread's `fd_held` map, and the dispatcher clears the map as the call returns (`fd_holds_release`). `close_fd` drops the table's record locks on the file at once (`fd_close_locks`); if the descriptor's bit is set in another thread's map, it only sets `FD_FLAG_CLOSING`, which no lookup passes, and the release runs as the last such call ends. A release sets `FD_FLAG_CLOSING` and `FD_FLAG_RELEASING` before it begins, since it may sleep in a file system's `release`, and until it ends no lookup, `dup2`, copy of the table or second release takes the entry. So `poll`, `select` and `epoll_wait` keep each descriptor they wait on until the wait ends.

### Open-file descriptions

A description (`struct open_file`) is shared by the descriptors that `dup` and `fork` make from one open. It has the reference count; the 64-bit offset; `raw_offset`, a raw disk window's offset; the `O_APPEND` and `O_NONBLOCK` flags; and for a file of a mounted type the mount, the node and the type's per-open handle (`vmnt`, `vnode`, `fhandle`), the inode number at the open, and the pin of an `O_PATH` description on a type that needs one. Descriptions come from the heap, each in a record with the wait queue of its lock (`struct ofd_rec` in `fd.c`), on one list. A description's serial number is the count made before it, so `fd_ofd_live` can tell whether a description an epoll registration remembers is still open without reading an address the heap may have given to a later one. The last reference drops the description's own locks (`lock_ofd_gone`), reports the close to inotify, drops the pin, and calls the type's `release` through `vfs_desc_close`.

`truncate(2)` truncates through a description of its own, so it runs the type's `open` and `release`, and is counted in the unmount's log record, as an `open(2)` without `O_PATH` is ([vfs.Readme.md](vfs.Readme.md)).

The description lock (`fd_ofd_lock`) keeps a sharer from using the offset while another sleeps in a disk request with it. On a type that sets `ns_serialised` ([vfs.Readme.md](vfs.Readme.md)), it takes the mount's namespace lock first. `read`, `write`, `lseek`, `getdents64`, `ftruncate`, `truncate` and `fallocate` hold it, and so does the read or write of a device or `/proc` file whose offset moves; `pread64` and `pwrite64` hold it on a regular file and give a device or `/proc` file a copy of the description instead (`pio_view`). An `O_APPEND` write reads the file's size under it. A directory's offset is its listing's cursor, a value of the type's own, so `lseek` on a directory of a file system takes only 0, the current cursor, or one the listing reports, which it finds by reading the listing from the start (`dir_cursor_valid`).

### Paths

A call copies each path into a page from the calling process's set (`path_buf_get`), since a path may be `VFS_PATH_MAX`, 4096 bytes with its terminator, a quarter of the 16 KiB kernel stack. A process has up to `PATH_BUFS`, eight, pages, each taken the first time one of its calls needs that many and kept until the record is freed; the deepest call takes four, a rename's two paths and a symbolic link's target with the path it is joined to. So a path costs a frame only once, and a test that fails a chosen frame allocation with `rach2.framefail` is not moved by the paths a program uses.

`copy_path_at` makes every path absolute and canonical. A relative path is joined, in its page, to the working directory's path (`proc_cwd`) or to the path of the directory a descriptor is open on (`fd_dir_path`), and the whole is held to 4096 bytes. `canonicalize_abs` removes repeated slashes and `.` components and keeps `..`, which the resolver walks physically ([vfs.Readme.md](vfs.Readme.md)). How the path as written ended is kept beside it (`path_names_dir`), since a path that ends in a slash, `.` or `..` must reach a directory and the canonical copy no longer shows the ending. A last `.` is put back (`path_put_dot`), so the walk looks it up in the directory before it, with the search permission and the `ENOTDIR` of Linux's walk, and `resolve_named` makes such a path follow a final symbolic link and reach a directory. `execve` runs the node its one walk returned, and so does an interpreter's walk, so a symbolic link replaced between two walks cannot change what runs.

The join passes through the directories above the working directory, which Linux's relative walk never enters; [vfs.Readme.md](vfs.Readme.md) says how the resolver judges search permission on them. Since a relative name becomes an absolute path, a directory that no path reaches takes no new entries: a removed directory that a descriptor or a working directory still has stays empty. A `..` at the front of a path relative to a removed working directory is taken off the string instead (`removed_cwd_parents`), so `cd ..` leaves it.

A directory descriptor's path is made again at each use: a synthetic directory's from its kind and process; a directory's of a type with a `path` operation by the type, so the descriptor follows a rename; and a KRFFS root directory's from its node, which must still resolve to itself, or the call answers `EBADF`. The message queue file system, the one type without `path`, uses the path recorded at the open in the description's 64-byte `dirpath`. The path is then made relative to the process's root (`path_in_root`); one outside that root, from a descriptor opened before a `chroot`, keeps its path from the root of the mount table, with leading slashes that make the resolver start there, as Linux walks from such a descriptor.

The working directory's path is in the `fs_struct` that `CLONE_FS` shares, in its 64 bytes (`CWD_MAX`) when it fits and in the heap, up to 4096 bytes, when it does not (`proc_cwd_set`).

### Permissions, ownership and times

A process is privileged when `proc_privileged` is true of it; [the process subsystem's Readme](../proc/Readme.md) gives the rule. `cred_access_bits` judges one class, owner, group or other, by the effective ids, or the real ones for `access`, and passes a caller whose user id of that kind is 0 and who has kept its capabilities. `cred_access_node` applies it to a node of the KRFFS root and `cred_access_ref` to a node of another type, from the attributes the type reports. Both answer `EACCES` to the execution of a non-directory with no execute bit, even for root, and `cred_access_ref` answers `EPERM` to a write of an immutable file. The KRFFS root directory is judged as mode 0755 and not sticky by its identity, since its stored mode has the dirty flag where the setuid bit would be ([krffs.Readme.md](krffs.Readme.md)).

`cred_kill_setid` on the KRFFS root and `vfs_kill_setid` elsewhere take the set-id bits from a file that is not a directory, setuid always and setgid when group execute is set, after an unprivileged write that moved bytes or truncation, and after any `chown` that changes an owner. The sticky rule is `cred_sticky_ok` on the KRFFS root and `cred_remove_ref` elsewhere, each after the write check on the parent. Each call's page gives the rules it applies.

A new node takes the caller's effective ids and the mode under the umask. On the KRFFS root, ext2 and tmpfs a set-group-ID directory gives a new node its group, and a new directory the bit; a KRFFS volume gives the caller's group even there (`maker_gid` in `krffsmount.c`); FAT32 and exFAT report the mount's ids and mask. Each type keeps its own times and is given new ones through `setattr`; `utimensat` leaves the KRFFS root directory's times alone and succeeds. Only tmpfs advances a file's access time on a read.

### The lifetime of a file of the KRFFS root

The KRFFS root keeps no count of a node's holders; `fd_node_holders` counts them by a scan: every descriptor in every table that has the node, a descriptor in flight on an AF_UNIX socket, a mapping of the file, an address space that runs it, and for a FIFO each description, since its pipe is found by the node's address. An `unlink`, or a `rename` over a name, whose node has holders takes the name and keeps the node (`krffs_unlink_held`); the orphan is in no lookup, listing or count, and its last holder's release frees it (`fd_release_orphan`).

The count and the free run under the root's namespace lock (`fd_release_lock`), which `unlink` takes with its walk, `rename` after its two walks, an open from its walk until it has published its descriptor, and every allocation on the root. So the count includes every descriptor an open has made, and nothing allocates the extent or counts again while a free sleeps reading a neighbouring header. A holder kept in a list of its own, a mapping, a descriptor in flight or an address space, takes the lock before it leaves the list.

A directory is never an orphan: `rmdir` fails with `EBUSY` on one that a description or a working directory has, before it looks for names in it (`root_dir_in_use`), since a descriptor of the root finds its directory by the node's place, which the allocator would give to the next node made. Every other type keeps a node for its holders itself ([vfs.Readme.md](vfs.Readme.md)), and [mkdir(2)](../../../man/man2/mkdir.2) gives each type's answer to `rmdir` of a directory in use.

### Durability

The boot line chooses the root and how KRFFS reaches it ([../../Readme.md](../../Readme.md), [krffs.Readme.md](krffs.Readme.md)). `fsync` writes back the file's shared mappings (`filemap_sync_file`) and then calls the type's `fsync` through `vfs_fsync`, which flushes the disk's write cache after a type on a block device has written. A descriptor with no mount, a pipe or a device, writes the page cache.

| Where the file is | What `fsync` writes | Disk cache flushed | Survives a reboot |
| --- | --- | --- | --- |
| KRFFS root on a RAM module | nothing | no disk | no |
| KRFFS root through the window, on a RAM module | every dirty page, into the module | no disk | no |
| KRFFS root on a disk partition | every dirty page of the window | no | yes |
| KRFFS volume, FAT32, exFAT | every dirty buffer of the device | yes | yes |
| ext2 | every pending file of the mount, then every dirty buffer of the device | yes | yes |
| tmpfs, memfd | nothing | no disk | no |

`sync` writes every file's shared mappings; then, for each active mount on a device, runs the type's `sync`, writes the device's dirty buffers and flushes the device; then writes the page cache, waiting up to 3000 ticks, 30 seconds, for pages under a write. It returns 0 whatever failed; a failed write leaves its buffer dirty and the mounts on its device read-only ([vfs.Readme.md](vfs.Readme.md)). `syncfs` is `sync` once it has checked its descriptor. A successful `umount2` has run the type's `sync` and `prepare_unmount`, written the buffer cache and flushed the device; a failure leaves the volume mounted.

The kernel writes back nothing on a timer: [update(8)](../../../man/man8/update.8), which init starts, calls `sync` every thirty seconds. `reboot(2)` writes the root alone (`kernel_shutdown` in `../../kernel.c`). A KRFFS root on a disk has its page cache written, its dirty flag cleared and the cache written again; an ext2 root is synced and marked valid, as its unmount leaves it; any other root has its page cache written. A volume placed by `mount(2)` keeps what the last `sync`, `fsync` or unmount wrote.

## Limits

* No path sends a cache flush to the disk under a KRFFS root: `fsync`, `sync` and `reboot(2)` write its pages and stop there.
* `fsync` and `fdatasync` are one call, and on every type that writes anything they write the whole device or the whole page cache, not the one file. `syncfs` writes what `sync` writes and, like it, reports no failure.
* `reboot(2)` writes no volume placed by `mount(2)`.
* No file system keeps extended attributes: getting, setting and removing one answer `EOPNOTSUPP`, and a list is empty. There are no ACLs, file capabilities, per-user quotas or journal. Of the types that store files, only ext2 and tmpfs have hard links and symbolic links.
* A path joined to its directory must fit 4096 bytes with its terminator, where Linux bounds only the path the caller gave.
* Each type bounds the climb that rebuilds a directory descriptor's path, and `fchdir` or a relative `*at` call through a deeper directory answers `ENAMETOOLONG` however short its path. The Limits of [krffs.Readme.md](krffs.Readme.md), [ext2.Readme.md](ext2.Readme.md), [fat32.Readme.md](fat32.Readme.md) and [exfat.Readme.md](exfat.Readme.md) give each type's bound.
* The holders of a KRFFS root node are counted by a scan of every descriptor table, so an `unlink`, a `rename` over a name and the release of an orphan take time in proportion to the descriptors open on the machine.

## Tests

* `tests/run default` runs, among the other probes on the test image, `ofdprobe` (shared descriptions), `fdfull` and `limitprobe` (the descriptor limit), `fdstale` (a descriptor across an unlink, a rename and a relocation, and `rmdir` of a directory in use), `dirprobe`, `dirgrow`, `unlinkdirprobe`, `pathcanon` and `vfsprobe` (directories and paths), `ownprobe`, `permprobe`, `modeprobe`, `searchprobe` and `stickyprobe` (ownership and permission), and the musl programs `pathmaxmusl`, `fdcallmusl`, `atcallmusl`, `preadmusl`, `truncmusl`, `opathmusl`, `lchownmusl`, `direntmusl`, `statfsmusl`, `statmusl`, `stampmusl` and `threadmusl`.
* `tests/run fdlock` runs `fd_hosttest`, then boots a KRFFS disk root for `fdstale` and for concurrent offsets of a raw disk window.
* `tests/run persist` and `tests/run persist-ahci` boot one KRFFS disk image twice, on an IDE and on an AHCI controller, and require a file written, `fsync`ed and `sync`ed on the first boot to be read back on the second.
* `tests/run diskfull` fills the KRFFS root of the test image until a write fails with `ENOSPC`, requires the file to hold what was written, and requires the space back after an unlink.
* `tests/run host` runs `fd_hosttest` (descriptions, each sharer's descriptor limit, the errors of an allocation) and `open_hosttest` (opens and truncations, the close of a descriptor whose release sleeps, the key of a file's locks, and the release of a KRFFS root orphan).
* `/musl/catfile` and `/musl/ofdmusl` are staged on the test image and run by no suite.
