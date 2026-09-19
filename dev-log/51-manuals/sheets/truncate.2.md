# truncate(2)

NAME: truncate - the system call family: ftruncate, ftruncate64.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 truncate`; the writer adds those lines.

Notes from the milestone: Which file systems grow and shrink; a loop-backed file (EBUSY); the 64-bit form.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## ftruncate: number 93, handler sys_ftruncate

(no comment)

### helper truncate_fd

Truncate an open file to `length` (ftruncate(2)).
The descriptor must be a writable file on a mounted file system. A
successful shrink or extend clears the set-id bits for an unprivileged
caller, like a write. `SYS_FTRUNCATE` takes the length in ECX, and
`SYS_FTRUNCATE64`, the form musl calls, in ECX and EDX as the low and
high halves; the file system answers a length past what it can hold.


## ftruncate64: number 194, handler sys_ftruncate64

(no comment)

### helper truncate_fd

Truncate an open file to `length` (ftruncate(2)).
The descriptor must be a writable file on a mounted file system. A
successful shrink or extend clears the set-id bits for an unprivileged
caller, like a write. `SYS_FTRUNCATE` takes the length in ECX, and
`SYS_FTRUNCATE64`, the form musl calls, in ECX and EDX as the low and
high halves; the file system answers a length past what it can hold.
