# socket(2)

NAME: socket - the socket operation family: socket.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: AF_INET with SOCK_STREAM, SOCK_DGRAM and SOCK_RAW, and AF_PACKET with SOCK_DGRAM, as the code allows and who may open a raw socket; the pool from the heap.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## socket: socketcall 1, handler do_socket

socket(2): AF_INET with SOCK_STREAM, SOCK_DGRAM or SOCK_RAW (ICMP, or the
ioctl-only descriptor of IPPROTO_RAW), and AF_PACKET with SOCK_DGRAM; the
raw and packet forms take effective uid 0. Returns a new socket fd or
-errno. */
