# setsockopt(2)

NAME: setsockopt - the socket operation family: setsockopt, getsockopt.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 setsockopt`; the writer adds those lines.

Notes from the milestone: The options answered (SO_REUSEADDR, SO_BROADCAST, SO_ERROR, SO_RCVTIMEO and the rest the code lists) and ENOPROTOOPT.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## setsockopt: socketcall 14, handler do_setsockopt

setsockopt: accept the boolean options a forking server sets (SO_REUSEADDR,
TCP_NODELAY, SO_KEEPALIVE) so its setup succeeds. The stack already reuses
listen ports and sends promptly, so these are honored-by-behavior or
documented no-ops; an unknown option is accepted instead of failing a
server's startup. */


## getsockopt: socketcall 15, handler do_getsockopt

getsockopt: report the integer options programs read at setup: SO_ERROR (no
async error is tracked, so 0), SO_TYPE (the real socket type), and the
boolean options as 0 (off). Always reports a 4-byte int result. */
