# wait(2)

NAME: wait - the system call family: waitpid, wait4.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: wait4 fills rusage with zeros.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## waitpid: number 7, handler sys_waitpid

sys_waitpid - wait for a child to change state.
Args: EBX = pid (-1 = any child), ECX = status pointer, EDX = options.
Returns the reaped child's PID, 0 (WNOHANG, none ready), or -errno.

### helper do_wait

Shared core of waitpid(7) and wait4(114).
`status` (may be NULL) receives the Linux-encoded wait status that
sys_exit/terminate already stored in exit_code; `rusage_ptr` (0 for
waitpid, may be 0 for wait4) is zero-filled on reap. With WNOHANG, returns
0 instead of blocking when a child exists but has not exited.


## wait4: number 114, handler sys_wait4

sys_wait4 - like waitpid plus an optional rusage out-pointer
(zero-filled). Args: EBX = pid, ECX = status, EDX = options, ESI = rusage.
musl's wait/waitpid are implemented on top of this.

### helper do_wait

Shared core of waitpid(7) and wait4(114).
`status` (may be NULL) receives the Linux-encoded wait status that
sys_exit/terminate already stored in exit_code; `rusage_ptr` (0 for
waitpid, may be 0 for wait4) is zero-filled on reap. With WNOHANG, returns
0 instead of blocking when a child exists but has not exited.
