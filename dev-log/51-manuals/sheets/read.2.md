# read(2)

NAME: read - the system call family: read, readv.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: Regular files, directories (EISDIR), pipes, the console, ptys, sockets, devices; short reads; EINTR; nonblocking; readv scatter.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## read: number 3, handler sys_read

read(2): EBX = fd, ECX = buf, EDX = count. */

### helper read_fd

Read up to `count` bytes from descriptor `fd` (read(2)).
EBX = fd, ECX = buf, EDX = count. Returns the bytes read, 0 at end, or
-errno. Dispatches by backing kind. A console read (stdin) blocks in the
tty line discipline until a cooked line or VMIN raw bytes arrive; in
graphics mode it is a non-blocking raw scancode read instead.


## readv: number 145, handler sys_readv

Scatter read (readv(2)). The twin of `writev` above, and it exists for the
same reason that one does: musl's buffered stdio issues this call rather than
`read` whenever it is filling more than a single byte, so without it every
program that reads through `fgets`, `fread` or `scanf` fails on this system.
That is why several programs in this tree read a byte at a time through
`read` instead, and why busybox's `chpasswd`, which reads its input with
`xmalloc_fgetline`, exited without a message before this existed.
EBX = fd, ECX = struct iovec[], EDX = iovcnt. Returns the total bytes read
across the iovecs, or a negative errno.
The whole vector list is checked before a byte is taken, because a read
cannot be undone: a caller that named a bad destination in its last buffer
gets -EFAULT with its input still queued, not a partial total over
data that is now gone. The check is the one `write` makes of a buffer it is
about to hand a driver, `user_buffer_ok` with write access, so an address
that is merely inside the user window but unmapped or read-only is refused
here and not discovered half way through. A list that asks for nothing still
checks the descriptor, so a closed fd is -EBADF and not a quiet zero.
The buffers are then filled in order, and what "in order" costs depends on
what is behind the descriptor, because a second read is not always the same
thing as reading on.
A regular file is read again for each buffer. Its read returns at once and
its bytes carry no message boundary, so a walk is the single scatter read.
A datagram socket is NOT read again: one datagram is one message, and a
second read would take the next one while the first one's tail was dropped.
The waiting datagram is copied out piece by piece instead, through
`socket_dgram_peek`, and consumed once at the end, so the whole of it fills
as many buffers as it needs and the remainder past the last one is discarded
the way a plain `read` discards it.
A pipe, a stream socket, a device or a `/proc` file is read again only while
it says it has more, since a read of an empty one blocks for input nobody has
written. Asking and then reading is safe for two reasons together: no other
process runs in between, because a syscall is not preemptible here, and the
interrupts that do run can only ADD to what is readable, never take it away.
Nothing else empties a pipe or a receive ring.
A terminal is read exactly once, and that is not a limitation but its
contract: the line discipline hands back one line, and filling a second
buffer with the line after it would return two lines where a reader asked
for one.
Returning less than was asked for is what a reader must already handle, and
musl's stdio, which is what issues this call, loops.

### helper read_fd

Read up to `count` bytes from descriptor `fd` (read(2)).
EBX = fd, ECX = buf, EDX = count. Returns the bytes read, 0 at end, or
-errno. Dispatches by backing kind. A console read (stdin) blocks in the
tty line discipline until a cooked line or VMIN raw bytes arrive; in
graphics mode it is a non-blocking raw scancode read instead.

### helper readv_dgram

Fill the buffers of a `readv` from one waiting datagram.
A datagram is a message, so it is taken once and copied out in pieces: the
buffers are filled in order until the datagram runs out, and what is left
over when the buffers run out first is dropped, which is what a `read` too
small for a datagram already does. Exactly one datagram is consumed either
way, and a datagram of no bytes returns 0 without being end of input.
The message is taken out of the socket whole and under one hold, and only
then spread across the buffers. Both halves of that are deliberate.
Taking it under one hold is not optional: a syscall body runs with interrupts
ENABLED (only preemption is off, see net.h), so the receive interrupt would
otherwise deliver the next datagram over this one between two buffers, and
the caller would get a prefix of one message followed by bytes of another.
Spreading it afterwards, with interrupts on and through `copy_to_user`, is
what keeps a caller's own iovec array from being a weapon. The buffers are
read from user memory as the copy goes, so a caller can point one buffer AT
its own vector list and have the datagram rewrite the destinations behind it.
Those addresses were never checked by the pass that ran before the read.
`copy_to_user` refuses an address it cannot fault in, which makes such a
caller a process scribbling on itself, not a raw kernel write to
whatever it named.
The message buffer is one datagram wide on a 16 KB kernel stack. That is the
whole of what "a kernel buffer to read into" ever needed to be.
`src_ip` and `src_port` report the datagram's sender for a caller that has
somewhere to put it, and are NULL for one that has not; the sender is read
under the same hold as the copy, since consuming is what publishes it.
Returns the bytes copied, or a negative errno.

### helper read_ready

Read into `buf` only if that can be done without waiting. Returns the bytes
read, 0 when there are none, or a negative errno.
This is what fills the second and later buffers of a `readv` on anything
whose bytes arrive, not sit there. Readiness is asked of the backing
object itself: the ops vtable answers `poll`, and a stream socket has a
non-blocking read of its own. Nothing can consume the bytes between the
question and the read, since a system call here runs with interrupts off from
end to end.
A terminal answers 0 whatever it has. Its read returns one line, and a
second line into the next buffer would answer a question the reader did not
ask.
