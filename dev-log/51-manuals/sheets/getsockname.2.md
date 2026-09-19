# getsockname(2)

NAME: getsockname - the socket operation family: getsockname, getpeername.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: The one local address; the peer after connect or accept.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## getsockname: socketcall 6, handler do_getsockname

getsockname: the socket's local address, the single NIC IP (net_ip_addr) plus
its bound or ephemeral port. */


## getpeername: socketcall 7, handler do_getpeername

getpeername: the connected peer's address (a connected stream socket
only). */
