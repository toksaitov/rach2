# tmpfs

`tmpfs.c` is a file system in memory, as Linux's tmpfs is: its files go when it is unmounted. It is the type `tmpfs` of `mount(2)`, which stands on no device, and the permanent mount `/dev/shm`.

## Nodes

tmpfs has every kind of node: regular files, directories, symbolic links, hard links, FIFOs, socket files and device nodes. A node is a record from the heap with its mode, its owner, its three times with nanoseconds, a device node's number, and a symbolic link's target; its inode number counts up from 1, the root's, and is not reused while the mount lasts. A node counts the holds of lookups, descriptions, working directories and mounts, and the names that name it, and is freed when both are gone, so a file unlinked while open stays readable until its last close. A directory's link count is two and one for each subdirectory, and its size is twenty bytes for each name and for `.` and `..`, as Linux reports.

A regular file's bytes are a `struct shmem` (`../mm/shmem.c`): a table of frames, one per page, a page made when it is first written or mapped, so a hole costs nothing. A shared mapping of the file maps those frames, and a frame a mapping still has outlives a truncation that frees it from the table. A file is at most 1 GiB (`SHMEM_MAX_SIZE`), `EFBIG` past that, and a page the frame allocator cannot give fails a write with `ENOSPC`. There is no quota of its own: tmpfs files take the machine's memory, and `statfs` reports the frames in all and free as its blocks.

A directory lists `.`, `..`, then its names in the order they were made or renamed; a name removed during a listing moves the ones after it down. `rename` moves a name within the mount, replaces a file or an empty directory, and refuses a directory under itself with `EINVAL`. A device node does not open through tmpfs itself: `open(2)` opens the `/dev` device with its number, as it does on ext2.

## Mounts

`mount -t tmpfs SOURCE DIR`, with any source string, `none` or `tmpfs` by custom, makes an empty tmpfs whose root has mode 1777, or the octal mode an option `mode=` gives; the other options Linux's tmpfs takes, `size=` among them, are accepted and change nothing. Nothing is claimed and nothing is written at unmount, which frees every node.

`/dev/shm` is a tmpfs the kernel mounts at boot, with `nosuid` and `nodev` as Linux systems mount it, where GNU libc's `shm_open` and `sem_open` make their files. `/dev` is devfs, a synthetic type that is handed the rest of a path in one call and has no directory node a mount could cover, so `/dev/shm` is a nameless mount standing in `/dev`'s root directory, found by its name as `/dev` and `/proc` are in the root's: the resolver enters it before devfs sees the rest of the path, and the `/dev` listing shows it as a directory. It is permanent: `umount2` refuses it with `EBUSY` ([vfs.Readme.md](vfs.Readme.md)).

## Verification

`tests/host/tmpfs_hosttest.c` runs the type over a pool of host pages: a file's bytes across a hole, a cut and a growth; directory link counts; hard links; rename's replacements and refusals; symbolic links, FIFOs and device nodes with their numbers; the listing's order and types; a directory's path; a node held after its last name; a shared mapping's reference to a page; and every frame back after the unmount. `tests/musl/tmpfsmusl.c`, in the default suite, mounts one on Rach2 and checks its files, directories, links and statfs, its lines in `/proc/mounts` and `/proc/filesystems`, the unmount refused while a file is open and done after, and `/dev/shm`: listed in `/dev`, a directory of mode 1777 where a file is made, written, read and removed, whose device nodes do not open, and which cannot be unmounted.
