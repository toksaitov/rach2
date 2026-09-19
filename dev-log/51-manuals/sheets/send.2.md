# send(2)

NAME: send - the socket operation family: send, sendto.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 send`; the writer adds those lines.

Notes from the milestone: Streams and datagrams; broadcast permission; EPIPE.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## send: socketcall 9, handler do_send

send(2): write to a connected stream socket (the peer is the
connection's). */


## sendto: socketcall 11, handler do_sendto

sendto(2): a stream socket ignores the destination and behaves like send; a
datagram socket sends to the sockaddr, binding an ephemeral source port
first. */
