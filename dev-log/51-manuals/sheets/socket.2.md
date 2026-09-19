# socket(2)

NAME: socket - the socket operation family: socket.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 socket`; the writer adds those lines.

Notes from the milestone: AF_INET only; SOCK_STREAM, SOCK_DGRAM, SOCK_RAW as the code allows and who may open a raw socket; the pool from the heap.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## socket: socketcall 1, handler do_socket

socket(2): only AF_INET (domain 2), SOCK_STREAM, and SOCK_DGRAM. Returns a
new socket fd or -errno. */
