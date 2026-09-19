# connect(2)

NAME: connect - the socket operation family: connect.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 connect`; the writer adds those lines.

Notes from the milestone: The handshake, timeouts, ECONNREFUSED and ECONNRESET, the loopback path.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## connect: socketcall 3, handler do_connect

connect(2): start the TCP handshake to the sockaddr. A blocking socket waits
it out (-EINTR on a signal); a non-blocking one returns 0 or -EINPROGRESS. */
