# bind(2)

NAME: bind - the socket operation family: bind.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: Ports, the one local address, 127/8 as an alias of it, EADDRINUSE, privileged ports.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## bind: socketcall 2, handler do_bind

bind(2): record the local port from the sockaddr; a datagram socket also
registers it with UDP (-EADDRINUSE if taken). */
