# statfs(2)

NAME: statfs - the system call family: statfs64, fstatfs64.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: The fields each file system fills; the magic numbers.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## statfs64: number 268, handler sys_statfs64

statfs64(2): the file system a path is on.
EBX is the path, ECX the caller's sizeof(struct statfs64), EDX the buffer.
The size is checked rather than ignored: a caller built against a different
layout would otherwise be handed bytes it would read as different fields.

### helper statfs_magic_for_kind

Which of the three mounts a classified path belongs to. */

### helper statfs_fill_mount

A statfs64 from a mount's own statfs: the numbers vfat reports. */

### helper statfs_fill

Fill a statfs64 for the boot root or one of the two synthetic mounts;
`statfs_fill_mount` answers for a mounted volume from its own statfs.
Only the root has blocks: it is KRFFS, and the region's size and its free
nodes' usable bytes are both real, reported in 1024-byte units because that
is what `df` prints in by default. The two synthetic mounts report their own
magic and no blocks at all, which is what they have and what Linux reports
for them; answering with the root's numbers would tell `df` that `/proc` is
a 16 MB file system 77 percent full. `f_files` is zero throughout, which says
"not counted" rather than claiming an inode budget this format does not keep.


## fstatfs64: number 269, handler sys_fstatfs64

fstatfs64(2): the same numbers, named by an open descriptor; a /proc
descriptor answers proc's and a device's answers devfs's.
EBX is the descriptor, ECX the caller's size, EDX the buffer.

### helper statfs_fill

Fill a statfs64 for the boot root or one of the two synthetic mounts;
`statfs_fill_mount` answers for a mounted volume from its own statfs.
Only the root has blocks: it is KRFFS, and the region's size and its free
nodes' usable bytes are both real, reported in 1024-byte units because that
is what `df` prints in by default. The two synthetic mounts report their own
magic and no blocks at all, which is what they have and what Linux reports
for them; answering with the root's numbers would tell `df` that `/proc` is
a 16 MB file system 77 percent full. `f_files` is zero throughout, which says
"not counted" rather than claiming an inode budget this format does not keep.

### helper statfs_fill_mount

A statfs64 from a mount's own statfs: the numbers vfat reports. */
