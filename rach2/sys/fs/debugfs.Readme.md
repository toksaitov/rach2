# debugfs

debugfs is the debug file system the kernel mounts at `/sys/kernel/debug`: a tree of directories and files that the kernel's subsystems register, each file with the operations an open binds to its descriptor, as Linux's `debugfs_create_file` gives them. It exists for the test programs that drive the kernel through it: IGT's sync-file tests through `sync/sw_sync` and `sync/info`, its display tests through the DRM files under `dri/` ([../drm/Readme.md](../drm/Readme.md)), and the KVM lifetime checks through `files/creators`. What each file does is in [debugfs(5)](../../../man/man5/debugfs.5).

## Interfaces

[debugfs.h](debugfs.h) has the calls a subsystem makes, under the big kernel lock:

* `debugfs_create_file(path, mode, ops, open, data)` registers a file at a path from the mount's root, such as `sync/sw_sync`, with the directories on the path that are not there yet, mode 0755. Every node is root's. An open of the file binds `ops` to the descriptor with `priv` set to `data`, then calls `open`, which may put other operations and another `priv` in their place, as `sw_sync_open` does, or refuse the open. The open may sleep, as `crc/data`'s does for the commit lock, and leaves the descriptor alone after a sleep, since the table may have moved.
* `debugfs_create_text(path, mode, ops, data)` registers a text file, Linux's seq file with a write: `ops->show(data, buf, cap)` writes the text and returns its whole length, and is called to size it and again to fill it; a read from offset 0 makes the text anew, and later reads give the rest from the description's offset. `ops->store(data, buf, len)` takes a write from user space and may sleep; a file without it answers a write with `EINVAL`.
* `debugfs_create_dir(path, ops, owner)` registers a directory held by `owner`: `ops->get(owner)` runs as each node below it is made, and `ops->put(owner)` as each is freed, once no walk and no open description holds it. The DRM core holds its device this way, so a device unplugged with a file of it open stays until that file's last close.
* `debugfs_create_symlink(path, target)` registers a symbolic link, mode 0777, as `dri/0` links to the device's directory.
* `debugfs_remove(path)` takes a file or a directory and everything below it out of the tree at once. A descriptor open on a file taken out keeps its operations and `priv`, so the registrant keeps what they use until that descriptor's last close.

The VFS type `vfs_debugfs_type` ([vfs.c](vfs.c)) has debugfs.c's operations, `debugfs_vops`, and its constructor, `debugfs_mount`. The system call layer's open of a regular file of debugfs (`open_install` in [../syscall_fs.c](../syscall_fs.c)) calls `debugfs_bind`, which binds the file's operations to the new descriptor before the descriptor is published (`fd_install`); the reserved slot stays where it is while the open sleeps, and on a failure the caller frees it. The descriptor stays a file of the mount, `FD_TYPE_FILE` with its description's node and mount, so `fstat`, `/proc/PID/fd` and the mount's count of opens treat it as any file; `read`, `write`, `ioctl`, `poll` and `lseek` call its operations, as they do for any descriptor that has them, and the operations' `close` runs at the description's last close.

The kernel's own files are registered by the first mount: `sync/sw_sync`, mode 0644, whose open calls `sw_sync_open` ([../dmabuf/Readme.md](../dmabuf/Readme.md)), and `sync/info`, mode 0444, a text file whose `show` is `sw_sync_info`. KVM registers `kvm/exit_stats` when a backend probes ([../kvm/Readme.md](../kvm/Readme.md)). The DRM core registers each device's files under `dri/` as it registers the device and takes them out as it unplugs it (`drm_debugfs.c`), and the lockup detectors register `test_lockup` as they start ([../watchdog.Readme.md](../watchdog.Readme.md)).

## Design

A node is a heap record with its name, its parent, its children in the order they were made, its mode, its inode number, a count of holds, its owner, which it takes from its parent, and, for a file, its operations, its open or its text hooks, its data and its size, or, for a link, its target. The root is a static record, mode 0700 and root's, as Linux's default is, so no other user searches it; `mount(2)`'s `mode=`, `uid=` and `gid=` change it for every mount, since every mount shows the one tree, as Linux's single superblock does.

A walk's lookup and an open description each hold the node they have. A node taken out leaves its parent's list at once, so no lookup finds it, and a directory taken out answers `ENOENT` to a lookup and a listing; the record is freed by its last put. A node's inode number is the count of nodes made before it, plus one, and is never given again.

The walk enters debugfs from `/sys`: `/sys` is synthetic and takes the rest of a path in one call, so no node of it can be covered, and debugfs is a nameless mount entered by the names `kernel/debug` from `/sys`'s root, whose `..` is `/sys/kernel` ([vfs.Readme.md](vfs.Readme.md)). It is permanent mount 7, made at boot with `nosuid`, `nodev` and `noexec`, as Linux systems mount it.

A file's size is 0 and its times are those of its making, as on Linux; a truncation, such as IGT's open of `edid_override` with `O_TRUNC`, sets the size `stat` reports and nothing else. A text file's open keeps a record on the description with the text of its last read from offset 0; `lseek` moves the offset by `SEEK_SET` and `SEEK_CUR`, and refuses `SEEK_END` with `EINVAL`, as `seq_lseek` does. Nothing else changes the tree from user space: the VFS answers `EROFS` and `EPERM` for the operations debugfs leaves NULL.

## Limits

* The mount at `/sys/kernel/debug` is permanent: `umount2` answers `EBUSY`, where Linux's init system makes the mount and may take it away. A second `mount(2)` of debugfs, there or elsewhere, works as on Linux.
* `sendfile`, `splice` and `copy_file_range` take no debugfs file as their source, `EINVAL`, as they take no `/proc` file, where Linux copies from both; `pread64` reads one through its operations, at a copy of the description.

## Tests

* `tests/host/debugfs_hosttest.c` drives the tree, its VFS operations and the sync files, with a fence made, signalled and read through `sync/sw_sync` and `sync/info` ([../../../tests/host/Readme.md](../../../tests/host/Readme.md)).
* `tests/sysdrmprobe.c`'s `dri` row checks the first card's files in a boot: `dri/0` as a link to a directory, `name` and `clients` against the device, the CRC files and a connector forced off through `force` and its `/sys` status and given an EDID through `edid_override`, each seen by the next `GETCONNECTOR` where Linux's would see it ([../drm/Readme.md](../drm/Readme.md)).
* `tests/sysdrmprobe.c`'s `debugfs` row checks the mount in a boot and on the Lenovo: root's directory of mode 0700 on a device of its own, `..` as `/sys/kernel`, the listing of `sync`, root's open of `sw_sync` and `EACCES` for uid 65534.
