# stat(2)

NAME: stat - the system call family: stat, lstat, stat64, lstat64, fstat64, fstatat64.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: The stat64 layout; what each file system reports (times, links, sizes); symbolic links on ext2 (lstat); device numbers for the /dev nodes; fstatat64 flags.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## stat: number 106, handler sys_stat

stat(106) follows symlinks; lstat(107) does not. musl maps SYS_stat and
SYS_lstat to the 64-bit forms, 195 and 196, so these two serve a caller
that issues the old numbers itself.


## lstat: number 107, handler sys_lstat

(no comment)


## stat64: number 195, handler sys_stat64

stat64(195) follows symlinks; lstat64(196) does not. On i386 musl maps its
stat()/lstat() onto these (via the statx -> *stat64 fallback), so lstat()
only works once 196 exists. The follow flag decides whether a symbolic
link at the end, on /proc or on a mounted type, is the result or its
target. */


## lstat64: number 196, handler sys_lstat64

(no comment)


## fstat64: number 197, handler sys_fstat64

fstat64 (syscall 197): EBX = fd, ECX = stat64 buffer. The descriptor's
node is stat'd; a root directory fd (node == NULL) reports the root
directory.

### helper fstat_fill

Fill a stat64 from an open descriptor by type.
Shared by sys_fstat64 and the followed /proc/<pid>/fd/<n> path (whose
target is just this fd). Where the object has an identity of its own (a
socket, a pipe, a device, a /proc snapshot, a KRFFS node), the inode
reported here is that identity and is the same number the descriptor's
`/proc/<pid>/fd` link carries, so a program reading one can match the other.


## fstatat64: number 300, handler sys_fstatat64

fstatat64 (syscall 300): EBX = dirfd, ECX = path, EDX = stat64 buffer, ESI
= flags. The path resolves the same way as openat (absolute, or relative
to AT_FDCWD/the dirfd; see copy_path_at). AT_SYMLINK_NOFOLLOW is honored
for the /proc magic symlinks; KRFFS has none.

### helper stat64_root_times

The root directory's own three times, into a stat64.
The root is a real node and creating an entry in it stamps its times, so it
reports them like any other directory; a machine with no file system mounted
has nothing to report but the boot time.
