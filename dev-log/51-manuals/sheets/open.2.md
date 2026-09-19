# open(2)

NAME: open - the system call family: open, openat, creat, close.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: Flags honoured (O_CREAT, O_EXCL, O_TRUNC, O_APPEND, O_NONBLOCK, O_CLOEXEC, O_DIRECTORY and the rest as the code says), modes on each file system, read-only mounts (EROFS), synthetic file systems, the descriptor limit (EMFILE), a directory opened for writing; openat with AT_FDCWD and a descriptor; close of a loop-backed file.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## open: number 5, handler sys_open

(no comment)


## openat: number 295, handler sys_openat

openat (syscall 295): EBX=dirfd, ECX=path, EDX=flags, ESI=mode. musl
issues openat (not the legacy open) for every <stdio.h> file open. An
absolute path is taken as-is, a relative path resolves against the cwd
(AT_FDCWD) or against the directory the dirfd refers to (see
copy_path_at).


## creat: number 8, handler sys_creat

creat(2): EBX = path, ECX = mode. Returns an fd open for writing, or
-errno. An existing regular file is truncated to zero by the call; a
directory, the root included, is EISDIR.


## close: number 6, handler sys_close

close(2): EBX = fd. Returns 0 or -EBADF. */

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
