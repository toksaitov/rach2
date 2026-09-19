# mkdir(2)

NAME: mkdir - the system call family: mkdir, rmdir.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 mkdir`; the writer adds those lines.

Notes from the milestone: Modes; non-empty directories (ENOTEMPTY); the root and mount points.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## mkdir: number 39, handler sys_mkdir

sys_mkdir - create a directory.
Args: EBX = path, ECX = mode (masked by umask, like any creation). Returns 0
or -errno (-EEXIST if it exists, -ENOENT if a parent is missing).

### helper cred_access_ref

The permission check over a node of any type, from the attributes its
file system reports: the same nine-bit rule as cred_access_node, plus
the execute rule for a regular file.

### helper cred_access_node

The permission check over a KRFFS node. The root directory reads as 0755,
the fixed identity stat reports (its stored mode has the dirty-mount
flag). Executing a regular file demands an execute bit somewhere even for
root: root runs programs, not data.

### helper krffs_stamp_new_node

Stamps a newly created KRFFS node with the creating process's identity:
owner and group from the effective ids, permission bits from `mode` with
the file-creation mask applied, the type bits kept. The V7 creation rule:
who you effectively are is whose the file is.


## rmdir: number 40, handler sys_rmdir

sys_rmdir - remove an empty directory.
Args: EBX = path. Returns 0 or -errno (-ENOENT, -ENOTDIR, -ENOTEMPTY).

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
