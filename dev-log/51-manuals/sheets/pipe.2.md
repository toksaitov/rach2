# pipe(2)

NAME: pipe - the system call family: pipe, pipe2.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 pipe`; the writer adds those lines.

Notes from the milestone: The pipe buffer size; O_NONBLOCK and O_CLOEXEC; EPIPE and SIGPIPE.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## pipe: number 42, handler sys_pipe

(no comment)


## pipe2: number 331, handler sys_pipe2

(no comment)
