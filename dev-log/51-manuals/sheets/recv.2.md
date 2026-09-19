# recv(2)

NAME: recv - the socket operation family: recv, recvfrom, recvmsg.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 recv`; the writer adds those lines.

Notes from the milestone: Streams, datagrams, raw; recvmsg scatter; MSG_ flags honoured.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## recv: socketcall 10, handler do_recv

recv(2): read from a connected stream socket. Honors MSG_DONTWAIT and
O_NONBLOCK. */


## recvfrom: socketcall 12, handler do_recvfrom

recvfrom(2): a stream socket behaves like recv (no source writeback); a
datagram socket reports the source address, truncated to the caller's
capacity. Honors MSG_DONTWAIT and O_NONBLOCK. */


## recvmsg: socketcall 17, handler do_recvmsg

Receive one datagram into a scatter list (recvmsg, socketcall 17).
The DHCP client is what needs this: it reads its raw socket through
`recvmsg`, not `read` so that it can look for `PACKET_AUXDATA` in the
control data, and finding none it verifies the UDP checksum itself. So the
scatter half is the whole of the work here. The control buffer is answered
with a length of zero, which is the true answer: this stack attaches no
ancillary data to anything.
The sender is reported through `msg_name`: a `sockaddr_in` for an AF_INET
socket, and for a packet socket the `sockaddr_ll` the frame came from, with
the sender's hardware address and who the frame was addressed to.
