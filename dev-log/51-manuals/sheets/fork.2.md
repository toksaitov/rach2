# fork(2)

NAME: fork - the system call family: fork, vfork.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: vfork is fork here (the same handler): the parent is not suspended and the child has its own address space, copied on write.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## fork: number 2, handler sys_fork

Create a child process (fork(2)).
Clones the current process: PCB, page directory (copy-on-write user
pages), and fd table. The child is set up to return from fork with value
0; the parent gets the child's PID. Returns -errno on failure.
Among what the child inherits is the program name, which `execve` sets and a
child that never execs would otherwise never have: a daemon that forks
itself into the background is exactly such a child, and without this it
shows in `ps` and in /proc/<pid>/stat with no name for as long as it runs.

### helper kstack_free

Give back the stack whose top is `top`. */


## vfork: number 190, handler sys_fork


