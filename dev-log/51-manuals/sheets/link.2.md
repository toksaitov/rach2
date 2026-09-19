# link(2)

NAME: link - the system call family: link, linkat, unlink.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: Hard links on ext2 only (EPERM or EXDEV elsewhere as the code says); unlink of an open file on the memory root (the descriptor rule); a file a loop device stands on (EBUSY, on the memory root; the other file systems keep an unlinked inode for the device).

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## link: number 9, handler sys_link

(no comment)


## linkat: number 303, handler sys_linkat

(no comment)


## unlink: number 10, handler sys_unlink

unlink(2): EBX = path. Returns 0 or -errno.
Every refusal is decided here, not below, because `krffs_unlink`
answers all of them with a bare -1, which the wrapper reports as `EIO`
whatever went wrong. A missing component gives `-ENOENT` and a plain file
in the middle of the path `-ENOTDIR`, both from the reporting resolver;
the root and any directory give `-EISDIR`, which is what tells a caller
to use `rmdir`. The root is tested by node identity because it has
0755 with no type bits and would otherwise read as a plain file.

### helper cred_remove_ref

The removal rules for a directory on a type other than the root: write
permission on the directory, and in a sticky directory the entry's owner,
the directory's owner, or root. The root type keeps its own copy of the
rule beside its node fields.

### helper cred_access_node

The permission check over a KRFFS node. The root directory reads as 0755,
the fixed identity stat reports (its stored mode has the dirty-mount
flag). Executing a regular file demands an execute bit somewhere even for
root: root runs programs, not data.

### helper cred_sticky_ok

The sticky rule on removing or renaming `victim` out of directory `dir`.
With the directory's sticky bit set, write permission on it is no longer
enough: only the entry's owner, the directory's owner, or a privileged caller
may take an entry out. That is what makes a shared 1777 directory usable, and
`-EPERM` is the errno POSIX names for it, not `EACCES`.
The SVR4 relaxation that also permits it when the caller can write the file
is not POSIX and is not adopted here: it would let a mode a victim chose for
their own file decide who may delete it.
The root is non-sticky by node identity, not by reading its mode, the
same fixed-mode rule `cred_access_node` uses, so `KRFFS_MODE_DIRTY` never
enters a permission answer.
