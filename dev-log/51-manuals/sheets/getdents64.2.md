# getdents64(2)

NAME: getdents64 - the system call family: getdents64.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: The record layout; what each file system lists; . and ..; the synthetic file systems.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## getdents64: number 220, handler sys_getdents64

Read directory entries (getdents64(2)).
EBX = fd, ECX = dirp buffer, EDX = count. Packs linux_dirent64 records
into the buffer, one per child, and advances the fd's offset cursor.
Returns the bytes written, 0 at end of directory, or -errno.

### helper fd_node_check

Re-validate a descriptor's cached KRFFS node before a file operation.
A held node is never freed now, so this stops being an identity test and
becomes what it always was in fact, a resolution. Two shapes are live: an
orphan, which no path names and which nothing will free while this descriptor
holds it; and a named node, which must still resolve back to itself. Anything
else is a descriptor whose file went away by a route that did not consult the
holders, and stays -EBADF.
The old round-trip could not tell a freed node from a different file the
allocator later placed at the same address. That hole is closed for every
regular-file descriptor by the lifetime, not by this check: no address
a descriptor has is handed out while it has it.
A NULL node is the root directory and needs no check. Directories are
deliberately out of scope: `rmdir` still frees the node under an open
directory descriptor, so a relative path through one still answers -EBADF.
