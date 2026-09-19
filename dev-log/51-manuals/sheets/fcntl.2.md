# fcntl(2)

NAME: fcntl - the system call family: fcntl64.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: The commands answered (F_DUPFD, F_GETFD, F_SETFD, F_GETFL, F_SETFL and the others the code lists) and those refused.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## fcntl64: number 221, handler sys_fcntl64

Descriptor control (fcntl64, syscall 221).
EBX=fd, ECX=cmd, EDX=arg. Supports the descriptor-flag and status-flag
round-trips libc needs: F_DUPFD(_CLOEXEC), F_GETFD/F_SETFD (the
close-on-exec bit), and F_GETFL/F_SETFL (access + O_NONBLOCK/O_APPEND,
on the shared open-file description, which the write and socket paths
read).
