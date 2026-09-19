# dup(2)

NAME: dup - the system call family: dup, dup2, dup3.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 dup`; the writer adds those lines.

Notes from the milestone: The descriptor table and RLIMIT_NOFILE (EBADF for a destination at or above it); O_CLOEXEC with dup3.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## dup: number 41, handler sys_dup

Duplicate a descriptor onto the lowest free fd (dup(2)).
EBX = oldfd. Returns the new fd, or -errno. The duplicate SHARES oldfd's
open-file description (shared seek offset); a socket gains a reference;
the new fd's FD_CLOEXEC is clear.


## dup2: number 63, handler sys_dup2

dup2(2): EBX = oldfd, ECX = newfd. Returns newfd or -errno. */

### helper fd_release_backing

Release every backing reference a descriptor holds before its slot is freed.
Drops framebuffer ownership, the per-fd socket reference, and an ops-backed
object (pipe/tty/pty). Every fd-teardown site routes through this (close, the
dup2 replacement of an open target, the execve CLOEXEC sweep, and the
process-exit fd-table release) so none can forget a backing kind as new ones
are added. It does NOT free the slot; the caller does that with fd_free.
An ops-backed object rides the shared open-file description: dup/dup2/fork
copy the ops/priv pointers and bump the OFD refcount, so the object must
be released exactly once, when its LAST descriptor closes (the final OFD
reference, refcount about to reach 0). The socket reference, by contrast,
is counted per descriptor (socket_ref on dup/fork), so it is always
dropped here. No descriptor is both an ops object and a socket.


## dup3: number 330, handler sys_dup3

dup3(330): like dup2 but oldfd == newfd is -EINVAL and a flag can set
O_CLOEXEC on the new descriptor. */
