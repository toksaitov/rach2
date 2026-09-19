# sync(2)

NAME: sync - the system call family: sync, fsync, fdatasync.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 sync`; the writer adds those lines.

Notes from the milestone: sync writes every mount's dirty buffers and the page cache; fsync is fdatasync (the same handler); what a failed write-back reports.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## sync: number 36, handler sys_sync

Flush all buffered file-system writes to stable storage (sync(2)).
Takes no argument. Every active mount's `sync` runs and the buffer cache
writes its dirty buffers, then every dirty page of the disk-backed window
goes back via `pc_sync` (a no-op when KRFFS is the RAM module). Like
Linux's sync(2), it returns 0 even if a writeback failed; what failed
stays dirty for a later call, and a mount whose device failed a write is
read-only from then on.


## fsync: number 118, handler sys_fsync

Flush a file's data to stable storage (fsync(2); also serves fdatasync).
EBX = fd. Validates the descriptor (any open fd; the type is not checked),
then writes back every dirty page of the disk-backed window via `pc_sync`.
The flush is whole-cache, not per-file, a safe superset of the
contract. When KRFFS is the RAM module the cache is empty, so the flush is
a no-op and the call only validates the fd. Returns 0, -EBADF for a bad
descriptor, or -EIO when a disk writeback fails. dropbear writes its host
key with the open/write/fsync/rename idiom and treats an fsync error as
fatal, so the -EIO is real backpressure.


## fdatasync: number 148, handler sys_fsync


