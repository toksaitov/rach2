# prlimit64(2)

NAME: prlimit64 - the system call family: prlimit64, getrlimit, setrlimit, ugetrlimit.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 prlimit64`; the writer adds those lines.

Notes from the milestone: See limits(7): the three limits kept, the refusals of each, the hard bound; the other resources answered as unlimited or refused as the code does.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## prlimit64: number 340, handler sys_prlimit64

prlimit64(340): EBX=pid (0/self), ECX=resource, EDX=new (rlimit64* or NULL),
ESI=old (rlimit64* or NULL). 64-bit struct rlimit64 { u64 cur, max }. The
old limits are reported before the new are set, as POSIX's `setrlimit`
and `getrlimit` see them. This is the form musl issues for both. */

### helper rlimit_set

Set a kept limit of `p`: the soft value at most the hard, the hard raised
by root only and lowered by anyone, and RLIMIT_NOFILE never above the
table's limit. A limit that is not kept is accepted and not changed.
0, or -EINVAL and -EPERM as POSIX names them.


## getrlimit: number 76, handler sys_getrlimit

getrlimit(76)/ugetrlimit(191): 32-bit struct rlimit { u32 rlim_cur,
rlim_max }. */


## setrlimit: number 75, handler sys_setrlimit

setrlimit(75): the 32-bit form of a set. */

### helper rlimit_set

Set a kept limit of `p`: the soft value at most the hard, the hard raised
by root only and lowered by anyone, and RLIMIT_NOFILE never above the
table's limit. A limit that is not kept is accepted and not changed.
0, or -EINVAL and -EPERM as POSIX names them.


## ugetrlimit: number 191, handler sys_getrlimit


