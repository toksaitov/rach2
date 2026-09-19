# symlink(2)

NAME: symlink - the system call family: symlink, symlinkat, readlink, readlinkat.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 symlink`; the writer adds those lines.

Notes from the milestone: Symbolic links on ext2; the resolver's limit; readlink on other file systems.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## symlink: number 83, handler sys_symlink

(no comment)


## symlinkat: number 304, handler sys_symlinkat

(no comment)


## readlink: number 85, handler sys_readlink

(no comment)

### helper do_readlink

readlink core (readlink(2)/readlinkat(2)).
The links are /proc/self, the /proc/<pid>/fd/<n> magic links (which
ttyname reads), and the symbolic links of a mounted type that has them,
read through the type. The result is the byte count and is NOT
NUL-terminated (the readlink contract; musl appends its own terminator and
treats a full buffer as truncation). `name` is the path, leading '/'
stripped.


## readlinkat: number 305, handler sys_readlinkat

(no comment)

### helper do_readlink

readlink core (readlink(2)/readlinkat(2)).
The links are /proc/self, the /proc/<pid>/fd/<n> magic links (which
ttyname reads), and the symbolic links of a mounted type that has them,
read through the type. The result is the byte count and is NOT
NUL-terminated (the readlink contract; musl appends its own terminator and
treats a full buffer as truncation). `name` is the path, leading '/'
stripped.
