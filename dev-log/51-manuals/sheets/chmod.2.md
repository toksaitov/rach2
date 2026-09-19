# chmod(2)

NAME: chmod - the system call family: chmod, fchmod, fchmodat.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: Which file systems keep modes; set-id bits and the credential model (cred_kill_setid); EROFS.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## chmod: number 15, handler sys_chmod

chmod(path, mode) and fchmodat(dirfd, path, mode[, flags]) share do_chmod. */

### helper do_chmod

chmod core for the chmod/chown family.
A KRFFS path stores through `krffs_chmod_node` under the owner-or-root rule.
A pty slave (`/dev/pts/N`) stores on the pty, the state dropbear moves
before handing the terminal to the user. Every other synthetic /dev or
/proc object has fixed root-owned metadata: a no-change request validates
and succeeds, a change is refused. `name` is absolute with the leading '/'
stripped, matching do_access.


## fchmod: number 94, handler sys_fchmod

fchmod(fd, mode): the descriptor form. A KRFFS-backed regular file's stored
perms are updated; any other descriptor (device/pipe/socket/tty) is a no-op.

### helper krffs_chmod_node

The chmod rule over a resolved KRFFS node: the owner or root replaces the
permission bits; anyone else is refused. The root node's mode is never
touched (its spare bits carry the dirty-mount flag), but the rule still
answers, so a non-owner probing `/` gets the correct -EPERM.

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */

### helper cred_in_group

Non-zero when `gid` is the caller's effective gid or in its supplementary
list: the set a file owner may chgrp to, and the group the access check
matches. */


## fchmodat: number 306, handler sys_fchmodat

(no comment)

### helper do_chmod

chmod core for the chmod/chown family.
A KRFFS path stores through `krffs_chmod_node` under the owner-or-root rule.
A pty slave (`/dev/pts/N`) stores on the pty, the state dropbear moves
before handing the terminal to the user. Every other synthetic /dev or
/proc object has fixed root-owned metadata: a no-change request validates
and succeeds, a change is refused. `name` is absolute with the leading '/'
stripped, matching do_access.
