# umask(2)

NAME: umask - the system call family: umask.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 umask`; the writer adds those lines.

Notes from the milestone: The mask and the mounts that honour it (the root KRFFS takes the mode as given, as open(2) says).

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## umask: number 60, handler sys_umask

sys_umask - set the file-creation mask, returning the previous value
(umask(2)). Args: EBX = new mask (only the low 9 permission bits are
kept).
