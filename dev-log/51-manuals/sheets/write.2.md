# write(2)

NAME: write - the system call family: write, writev.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 write`; the writer adds those lines.

Notes from the milestone: The same by kind; the console; a read-only mount; the raw disk nodes (EPERM on the ordinary kernel); O_APPEND; writev.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## write: number 4, handler sys_write

write(2): EBX = fd, ECX = buf, EDX = count. */

### helper write_fd

Write `count` bytes of user buffer `buf` to descriptor `fd`.
The shared body of write(2) and writev(2). Dispatches by backing kind: an
ops-backed object (pipe/tty/pty/device) through its vtable, a socket to the
network, the console through the tty output processor, a KRFFS file, or the
framebuffer. Returns the bytes written or a negative errno.


## writev: number 146, handler sys_writev

(no comment)

### helper write_fd

Write `count` bytes of user buffer `buf` to descriptor `fd`.
The shared body of write(2) and writev(2). Dispatches by backing kind: an
ops-backed object (pipe/tty/pty/device) through its vtable, a socket to the
network, the console through the tty output processor, a KRFFS file, or the
framebuffer. Returns the bytes written or a negative errno.
