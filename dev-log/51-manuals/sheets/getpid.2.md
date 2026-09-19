# getpid(2)

NAME: getpid - the system call family: getpid, getppid, getpgid, getpgrp, setpgid, getsid, setsid.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 getpid`; the writer adds those lines.

Notes from the milestone: The process and session identities; the console session and the controlling terminal are in the tty pages (phase 3, section 4 console).

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## getpid: number 20, handler sys_getpid

sys_getpid - return the calling process's PID (getpid(2)).


## getppid: number 64, handler sys_getppid

sys_getppid - return the parent's PID (getppid(2)).


## getpgid: number 132, handler sys_getpgid

sys_getpgid - return a process's process-group id (getpgid(2)).
Args: EBX = pid (0 = caller).


## getpgrp: number 65, handler sys_getpgrp

sys_getpgrp - the caller's process-group id (getpgrp(2), the no-arg form).


## setpgid: number 57, handler sys_setpgid

Set a process's process group (setpgid(2)).
EBX = pid (0 = caller), ECX = pgid (0 = the target's own pid, i.e. make it
a group leader). The target must be the caller or a child of the caller,
must not be a session leader, and the destination group must live in the
target's session. Returns 0 or -errno.


## getsid: number 147, handler sys_getsid

sys_getsid - return a process's session id (getsid(2)).
Args: EBX = pid (0 = caller).


## setsid: number 66, handler sys_setsid

Create a new session (setsid(2)).
The caller becomes the leader of a new session AND a new process group,
both numbered with its pid. Fails with -EPERM if the caller already leads
a process group (a group leader cannot carry its group into a new
session). Returns the new session id (the caller's pid).
