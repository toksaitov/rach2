# rename(2)

NAME: rename - the system call family: rename.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: Within one mount; over an open file; directories; EBUSY on the memory root for replacing a different file a loop device stands on.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## rename: number 38, handler sys_rename

rename(2): EBX = old path, ECX = new path. Returns 0 or -errno.
As in `unlink`, every refusal is decided here, because `krffs_rename`
answers all of them with a bare -1, which the wrapper reports as `EIO`.
The two directory cases are the ones POSIX separates and the memory root
does not (a mounted KRFFS volume answers `EIO` for both, and ext2 moves
directories): a directory SOURCE is `-EPERM`, since a directory's children
reference its id and a move would orphan them, so the rename cannot be
performed at all; a directory DESTINATION under a plain source is
`-EISDIR`, the case that errno is reserved for. The rest follow the
reporting resolver: `-ENOENT` for a missing source or destination parent,
`-ENOTDIR` for a plain file used as a path component, `-EINVAL` for a
destination naming no entry.
Either path naming the root is `-EINVAL`, from the empty-name check above:
`copy_path_cwd` canonicalizes it to "/", which loses its only component.
`-ENOENT` would be a lie, since the root exists, and POSIX names `EINVAL`
for a pathname that is the root. The identity tests below are therefore
unreachable today and are kept as defence: freeing the root as a replaced
destination would take the file system with it, and that guarantee should
not rest on a canonicalizer in another function.

### helper cred_access_node

The permission check over a KRFFS node. The root directory reads as 0755,
the fixed identity stat reports (its stored mode has the dirty-mount
flag). Executing a regular file demands an execute bit somewhere even for
root: root runs programs, not data.

### helper cred_access_ref

The permission check over a node of any type, from the attributes its
file system reports: the same nine-bit rule as cred_access_node, plus
the execute rule for a regular file.

### helper cred_remove_ref

The removal rules for a directory on a type other than the root: write
permission on the directory, and in a sticky directory the entry's owner,
the directory's owner, or root. The root type keeps its own copy of the
rule beside its node fields.
