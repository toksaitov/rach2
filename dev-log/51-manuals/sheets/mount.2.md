# mount(2)

NAME: mount - the system call family: mount, umount2.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: See mount(8) for the types and flags; here the call: the argument order, the copies, the refusals and their errors in order (ENODEV, EINVAL, ENOTDIR, EBUSY, ENAMETOOLONG, EPERM), the claim on the source range, the mount table; umount2 flags and EBUSY.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## mount: number 21, handler sys_mount

mount(2): EBX = source, ECX = target, EDX = type, ESI = flags, EDI = data.
Effective uid 0 is required. The strings are copied here; the order of
work and every other refusal are the VFS's (vfs_do_mount). `data` may be
NULL, which is an empty option string.

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */

### helper join_abs

Join base + '/' + rel into pathbuf as an absolute path.
`base` is "/" or "/a/b" with no trailing slash; `rellen` is k_strlen(rel).
Returns the length, or -ENAMETOOLONG if it would not fit.


## umount2: number 52, handler sys_umount2

umount2(2): EBX = target, ECX = flags. Effective uid 0 is required, and
the flags must be zero: MNT_FORCE, MNT_DETACH and UMOUNT_NOFOLLOW are
promises this kernel cannot keep and answer EINVAL.

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */

### helper join_abs

Join base + '/' + rel into pathbuf as an absolute path.
`base` is "/" or "/a/b" with no trailing slash; `rellen` is k_strlen(rel).
Returns the length, or -ENAMETOOLONG if it would not fit.
