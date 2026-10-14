# KRFFS in the kernel

KRFFS, the K&R first-fit file system, is the project's own file system and the root a Rach2 machine boots. An image is one region of bytes holding a ring of nodes, allocated first fit after the storage allocator of The C Programming Language, small enough for a reader to follow whole. `krffs.c` is the format and its operations, written once over a backend; `krffsmount.c` is the type `mount(2)` places on a block device; the adapter `krffs_ops` in `vfs.c` puts the boot root on the VFS ([vfs.Readme.md](vfs.Readme.md)). The host tools in [krffs/](../../../krffs/Readme.md) build, read and repair the same images, and their `fsck.krffs` judges what the kernel writes. This document is the one description of the format.

## Interfaces

* `mount(2)` with type `krffs` places a volume on a partition window, a whole disk or an attached loop device ([mount.2](../../../man/man2/mount.2), [vfs.Readme.md](vfs.Readme.md)).
* The boot root is the Multiboot module in place, the module through the page-cache window (`rach2.fswindow`), or a partition of disk 0 through the window (`rach2.fsdisk` or `rach2.root=disk`), as [../../Readme.md](../../Readme.md) describes.
* [statfs.2](../../../man/man2/statfs.2) gives what `statfs(2)` reports, and [rename.2](../../../man/man2/rename.2), [link.2](../../../man/man2/link.2) and [symlink.2](../../../man/man2/symlink.2) how a directory rename, a hard link and a symbolic link are refused.
* Kernel log records: `krffs: mount: refused: REASON`; `krffs: check: repaired: N backward link(s), N orphan(s), N repeated name(s)`; `krffs: mount: a check is owed and this mount is read-only`; `krffs: refused: a field at N would reach the medium in two pieces`; and, for a disk root, `krffs: not cleanly unmounted last time; run fsck.krffs`.
* `krffs.h` has the offset interface, the `krffs_*_at` functions on a `krffs_mount_t`, and the boot root's pointer interface.

## Design

### The format

An image is a region of bytes addressed by offset from its first byte, and records no size of its own. The region is a ring of nodes that tiles it: every byte belongs to one node, and free space is free nodes in the ring, with no list beside it. A node is a packed header of 357 bytes, `krffs_node_t`, followed by its data. The fields are little-endian:

| Offset | Bytes | Field | Contents |
| --- | --- | --- | --- |
| 0 | 2 | `magic` | `0x4B52`, "KR" |
| 2 | 1 | `type` | 0 root, 1 free, 2 reserved |
| 3 | 8 | `id` | the node's identifier |
| 11 | 256 | `name` | one path component, ending in NUL within the field |
| 267 | 2 | `mode` | type, set-id, sticky and permission bits |
| 269 | 4 | `uid` | owner |
| 273 | 4 | `gid` | group |
| 277 | 16 | `access_time` | seconds and nanoseconds, signed 8 bytes each |
| 293 | 16 | `modification_time` | the same |
| 309 | 16 | `attribute_change_time` | the same |
| 325 | 8 | `size` | the node's whole size in bytes, header included |
| 333 | 8 | `previous_node_size` | the size of the node before it |
| 341 | 8 | `data_size` | the bytes of data in use |
| 349 | 8 | `parent_id` | the identifier of the directory the node is in |

The node at offset 0 is the root, which is also the root directory. Walking forward adds a node's `size` to its offset, and the walk past the last node wraps to offset 0, which makes the nodes a ring; walking backward subtracts `previous_node_size`, and the root's is the size of the last node. A reserved node is a file, a directory, a FIFO or a socket file. A free node may keep the name and `data_size` of what was removed there; only its type says it is free.

The type bits of `mode` say what a reserved node is: `S_IFDIR` (0040000) a directory, `S_IFIFO` (0010000) a FIFO and `S_IFSOCK` (0140000) a socket file, the last two with no data, and no type bits a regular file. A reader compares the whole field, `mode & 0170000`, since a socket's bits include the directory's. There is no device node, because the header has no field for a device number. The root's stored mode is 0755 with no type bits, and its bit 04000 is the dirty flag (`KRFFS_MODE_DIRTY`), so the kernel answers for the root by its position and never by its mode.

A node is found by its directory and its one-component name: `parent_id` is its directory's identifier, 0 (`KRFFS_ROOT_ID`) for the root directory whatever the root's own `id` field has, and two nodes of one directory do not share a name. A directory's `id` is `krffs_mix_hash`, a mix in the style of FNV-1a that is never 0, of its parent's identifier and the DJB hash of its name (`h = h * 33 ^ c` from 5381, in 64 bits), and its children have it in their `parent_id`; that is why a directory is never renamed or moved. Two names can have one DJB hash (`bC` and `cb` do), so `mkdir` refuses a directory whose identifier another directory has (`KRFFS_ETAKEN`, reported as `EINVAL`), which would share its children. A regular file's `id` finds nothing: the host stager writes the mixed value, and a file the kernel creates or renames gets the DJB hash of its name alone.

On the boot root a node's inode number is `krffs_node_ino`: 1 for the root, a directory's `id`, and for any other node the mix of its `parent_id` and its `id`, which stays when the node's bytes move or its name is removed, and which two files of one directory whose names have one hash share. On a volume it is the node's offset plus one, which changes when the node moves. Advisory locks find a file by its node, not by this number ([lock.Readme.md](lock.Readme.md)).

A creation stamps all three times; a write or a truncation the modification and change times; a change of mode, owner or times the change time; and making, removing or renaming an entry its directory's modification and change times. Only `utimensat` changes the access time, since a stamp on every read would make reading a write.

### What a mount requires

`krffs_chain_walk` refuses the ring unless every node has the magic number and a known type, a size of at least one header that stays inside the region, a `data_size` that fits inside it (a free node excepted), and, if reserved, a name that ends within its field, and unless the walk ends exactly at the region's end. The full check (`krffs_chain_is_valid`) also requires each backward link to be the size of the node before it. Every walk is bounded by the most nodes the region can hold, so a damaged ring cannot loop.

* The boot root (`init_krffs`) requires the magic number and the root type at offset 0 and the full check.
* A volume (`krffs_volume_mount`) is refused when smaller than two headers (`EINVAL`) or of 4 GiB or more (`EFBIG`), and requires the magic number and the root type (`EINVAL`, or `EIO` when the first node cannot be read) and the walk without the backward links (`EINVAL`, a read that fails inside it included). A writable mount of a volume whose dirty flag is set repairs it first, then requires the full check and sets the flag. A read-only mount repairs nothing, since nothing it does follows a backward link, and logs that a check is owed.

A volume's region is the whole device, so an image on a partition larger than itself fails the walk rather than mounting short.

### The dirty flag

The flag says that a check is owed, not what is wrong. A writable volume's mount sets it on the medium as its last step. Its unmount (`kv_prepare_unmount`) writes the dirty buffers and flushes the device, then clears the flag with a write and a flush of its own; if the clear fails, the mount becomes read-only and the unmount answers `EIO`. A mount that went read-only before its unmount keeps the flag. An unmount abandoned after the clear sets it again (`kv_abort_unmount`), or, if that fails, makes the mount read-only.

A disk root (`init_krffs_disk` in `kernel.c`) logs a flag it finds set, sets the flag and writes it through, failing the mount if that does not reach the disk, and then frees the orphans. Only `kernel_shutdown` clears it, when the debug shell's `halt`, `poweroff` or `reboot`, or `reboot(2)`, stops the machine, and only after the page cache has written everything back. A root in memory keeps no flag.

### The order of writes

A machine can stop between any two writes, and on a volume they reach the medium a 4 KiB block at a time. The rule is that the forward chain is the truth, and each write that reaches the medium leaves a ring that can be walked. Every operation in `krffs.c` keeps three consequences of it, with sync points (`krffs_sync_point`) where the backend puts everything written so far on the medium:

* The bytes come before the field that publishes them: a node's data before the `data_size` that counts it, a new node's header before the `size` that reveals it.
* One field publishes one change. A split writes the tail's free header inside the free node and then stores the shortened `size`, at which the ring gains a node; a removal stores the type as free. An operation may publish twice, if each store leaves a ring that can be walked.
* The backward link comes last and may lag; a stop before it is repaired leaves the one fault a check rebuilds from the walk.

A field must also reach the medium whole: one that falls across a block boundary is written in two pieces, and a stop between them leaves a value nobody wrote. On a mount with such a unit (`grain`, 4096 on a volume, 0 on the boot root), `krffs_put` refuses a write whose changed bytes fall in two blocks, and the operation fails as any refused write does. An 8-byte field crosses a boundary at 7 of every 4096 offsets. The 1-byte type and the 256-byte name are written without the rule, and so is every field of a node no path reaches yet: a node being made, whose name is empty until its last write, and a copy being filled for a move, which is free until its type is written. A rename asks the rule of the identifier and the parent before it removes a destination.

The operations in those terms:

* Create (`krffs_allocate_err`, then `krffs_create_node_at` or `krffs_mkdir_at`). Split the free node if the rest can hold a header; write an empty name, sync, `data_size`, sync, and the type, which makes it a node, sync. Then the parent, the identifier, the mode, the owner (the caller's effective ids on a volume, root's on the boot root) and the three times, sync, and last the name, which is what a path reaches, sync. The empty name matters: a free node keeps the name it had, and publishing that would bring a removed file back.
* Write past the end (`krffs_write_at`, `krffs_resize_at`). Take the whole free node after the file with one `size`, repair the backward link, sync; write the surplus's free header inside the node, sync, and store the shortened `size`. Then zero a hole, write the bytes, sync, and store `data_size`. Writing the surplus's header first would land on the free node's own header whenever the growth is shorter than a header.
* Truncate (`krffs_truncate_node_at`). Store the shorter `data_size` and sync, since the surplus's header is written inside bytes the old length counts; then the header, merged with a free successor, sync, and the shortened `size`. Growth is zeroed and synced before the longer `data_size`.
* Remove (`krffs_remove_at`). Store the type as free, merge with a free successor by one `size`, then with a free predecessor by one `size`, repair the backward link, sync.
* Move. A file that must grow with no free node after it is copied into a node allocated elsewhere, whose fields, data and `data_size` reach the medium before its type, and the type before the old node is removed. A stop leaves the file whole in one node or in two under one name. A move whose old node will not free fails, since the running kernel would find the old node first.
* Rename (`krffs_rename_at`), of a file only. Remove a destination file; write the identifier and the parent, sync, and the name, sync. A stop between leaves the file in the new directory under its old name, or under a name partly written.

A surplus too small for a header of its own stays inside its node, where a later shrink or a removal reclaims it.

Only a backend with sync points keeps this order on the medium. The boot root's memory backend has none: a disk root's pages reach the disk as the page cache writes them back ([../mm/Readme.md](../mm/Readme.md)), and after a stop the dirty flag sends the image to `fsck.krffs`.

### The repair

`krffs_repair_at`, run by a writable mount of a dirty volume, repairs the three faults the order of writes can leave:

* A backward link the forward chain contradicts: rebuilt from the walk.
* One name on two nodes of a directory, an interrupted move: the larger node keeps the name, since a move only makes a node larger. A directory keeps the name over a file whatever the sizes, since its children refer to it by identifier; a rename stopped between its parent and its name puts a file beside a directory of the same name.
* A reserved node with no name, published before it was named or removed while open: freed and merged with its free neighbours (`krffs_reap_orphans_at`).

It counts the named nodes before it writes anything, and refuses a volume with more than 4096 (`KRFFS_REPAIR_NAMES`) with `EIO`, since the search for a repeated name compares every pair. The disk root does not repair: it requires the full check, frees orphans at every mount, and leaves the rest to `fsck.krffs`, which makes the same judgements without that bound and clears the flag ([krffs/Readme.md](../../../krffs/Readme.md)).

Anything else, a torn header, a size that leaves the region, data that does not fit, a name that never ends, is not a state this order of writes produces, so the image is refused and nothing is changed.

### The core and its backends

`krffs.c` addresses every node by its offset and reaches the medium through a `krffs_backend_t` of `read`, `write` and `sync`; a `krffs_mount_t` has the backend, its context, the region's size and the grain. A walk reads a node without its name (`krffs_brief`, 99 bytes in two reads) and reads a name only to compare it.

The memory backend, which the boot root uses, copies once each way over the Multiboot module in place or the page-cache window at `PCWIN_BASE`, and its `sync` does nothing. The descriptor table, the system call layer and the boot code keep `krffs_node_t` pointers into that image, so the root has a pointer interface at the end of `krffs.c` that turns a pointer into an offset and refuses one outside the image. Its adapter, `krffs_ops`, uses a node's address as the handle, has no `hold`, `open` or `pin`, takes the mount's lock for its data operations, and leaves attributes to the system call layer.

The block backend (`krffsmount.c`) reads and writes through the buffer cache a block at a time, one buffer held at a time, and its `sync` is `bcache_sync` and then `block_flush`, so a sync point is on the medium. A node handle is the offset plus one, so the root's offset 0 is not NULL. An open handle is a `struct krffs_open`, one per open node: the node's current offset, the counts of opens and of `O_PATH` descriptions that keep the node (`kv_pin`), and whether its name has been removed. The data operations read the offset under the mount's lock and a relocation updates it, so a handle stays valid across moves. An unlink or a rename over a file that is open or kept removes the name and leaves the node for the last close to free. Since the type counts no holds, `rmdir` answers `EBUSY` for a directory that is open, kept or some process's working directory, found in the process records, and `chdir` keeps the directory with a pin from its walk until the working directory records it. `kv_create` and `kv_mknod` answer `EEXIST` for a name that is there; a creation the core refuses answers `ENOSPC` when no free node fits, `EINVAL` for a taken directory identifier, and `EIO` when the medium refused a read, a write or a sync (`made_errno`).

### The allocator

Allocation is first fit with split and coalesce. The first free node with room for a header and the bytes asked for is taken, and split when the rest can hold a header. A file grows into the free node after it when that is large enough, and is moved otherwise. A removal merges the node with its free neighbours, so no two free nodes are adjacent. A write that cannot grow its file answers `KRFFS_ENOSPACE`, reported as `ENOSPC`. Each allocation, and each step of a lookup, walks the ring from offset 0.

## Limits

* There are no hard links, no symbolic links and no device nodes; a directory is never renamed or moved; a volume takes no options and no remount between read-only and writable.
* A region, and so a file, is smaller than 4 GiB; a write that would reach past offset `0xFFFFFFFF` is `EFBIG`.
* A name is at most 255 bytes. The namespace operations join the directory's whole path with the name, which must fit in 255 bytes (`ENAMETOOLONG`), and a directory's path is rebuilt only to 64 levels (`KRFFS_PATH_MAX_DEPTH`).
* A listing returns the stored children only, with no `.` or `..` entry.
* A lookup of one component, a creation and an allocation each walk the ring, and a listing of the boot root walks it from the start for each entry.
* A rename over an existing file removes the destination before the source takes its name, so a stop between loses the destination.
* A write that could not grow its file answers `ENOSPC` even when the medium refused a write during the resize, so `ENOSPC` from a failing volume does not prove it full.

## Tests

* `tests/run host` runs `krffs_hosttest.c`, the core over a recorder that replays every prefix of an operation's writes and requires each to leave a ring that can be walked; `krffsmount_hosttest.c`, the volume's relocation, `O_PATH` pins, writeback errors, the clean flag's order and the creation errnos; and `krffs_hash_hosttest.c`, the stager's hashes against the kernel's. `krffs_id_mutate.sh`, `krffs_inode_mutate.sh`, `krffs_open_mutate.sh`, `krffs_root_lock_mutate.sh` and `krffsmount_mutate.sh` in `tests/host/` prove those rows against faults compiled into private copies.
* `tests/run krffsmount` places a volume (`krffsprobe`), writes it, reads it back after a reboot, kills a machine while it writes, runs two writers, and has the kernel refuse broken volumes; `fsck.krffs` judges the volume after each boot that wrote it.
* `tests/run crash` shows the dirty flag catching an unclean stop of a disk root, `tests/run persist` checks `fsync` across a reboot, `tests/run stress` runs a disk root larger than memory, and `tests/run fdlock` covers a pending relocation and the truncation of an orphan.
* `tests/run default` runs `vfsprobe`, whose rows include what a description keeps across a relocation on the boot root, and `tests/run krffscheck` runs `fsck.krffs` on images broken on purpose and on every prefix of a create, a write past the end, a truncation, a removal and a move made in the order above ([../../../tests/system/Readme.md](../../../tests/system/Readme.md)).
