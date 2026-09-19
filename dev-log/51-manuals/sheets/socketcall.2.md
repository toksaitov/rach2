# socketcall(2)

NAME: socketcall - the socket operation family: socketcall.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 socketcall`; the writer adds those lines.

Notes from the milestone: The one number for the whole family: EBX the operation, ECX the argument block; the operation numbers (1 socket, 2 bind, 3 connect, 4 listen, 5 accept, 6 getsockname, 7 getpeername, 9 send, 10 recv, 11 sendto, 12 recvfrom, 13 shutdown, 14 setsockopt, 15 getsockopt, 17 recvmsg, 0x1000 ping); the ones missing (socketpair, sendmsg, accept4) answer EINVAL or ENOSYS as the code says.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## socketcall: number 102, handler sys_socketcall

Multiplexed socket syscall (socketcall, syscall 102).
EBX is the SC_* sub-call number, ECX a user pointer to its argument array.
Copies the right number of 32-bit args in one fault-checked read, then
dispatches to the matching do_* helper. Returns its result, or -EINVAL for an
unknown sub-call.

### helper do_recvmsg

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
