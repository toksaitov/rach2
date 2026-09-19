# chown(2)

NAME: chown - the system call family: chown, lchown, chown32, lchown32, fchown, fchown32, fchownat.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: Owners on each file system; who may change them; the set-id clearing.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## chown: number 182, handler sys_chown




## lchown: number 16, handler sys_chown

chown/lchown/fchownat share do_chown; both the 16-bit and *32 numbers route
here since musl issues the 16-bit chown on i386. */


## chown32: number 212, handler sys_chown




## lchown32: number 198, handler sys_chown




## fchown: number 95, handler sys_fchown

fchown(fd, uid, gid): the descriptor form, under the same ownership rule
as the path forms, on the node or the mounted volume the descriptor is
open on. */

### helper krffs_chown_node

The chown rule over a resolved KRFFS node, the POSIX split: changing the
owner takes root; changing the group takes root, or being the owner and
naming one of your own groups. `(id_t)-1` keeps a field, and a no-change
value needs no right at all, so `chown(path, -1, -1)` always succeeds on an
existing file.

### helper vfs_chown_plan

What a chown changes on a mounted file system, and the set-id bits it
takes with it.
`vst` is read from the node and updated in place; the return is the
`VFS_SET_*` mask to apply, 0 when the call changes nothing, or `-EPERM`
when this caller may not make the change. A chown that changes neither
owner is permitted to every caller, as it is on the boot root: a value
that changes nothing needs no right, and a refusal there would stop
`chown(path, -1, -1)`. The set-id clear is `vfs_kill_setid`'s rule, which
is `cred_kill_setid`'s rule for the boot root: a directory keeps its bits,
and a file without group-execute keeps setgid, since there the bit marks
mandatory locking and grants nobody anything.

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */


## fchown32: number 207, handler sys_fchown




## fchownat: number 298, handler sys_fchownat

(no comment)
