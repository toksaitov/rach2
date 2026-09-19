# setuid(2)

NAME: setuid - the system call family: setuid32, setgid32, setreuid32, setregid32, setresuid32, setresgid32, setgroups32.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: The identity family, write side: the privilege rules, the saved ids, the NPROC accounting on a real uid change.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## setuid32: number 213, handler sys_setuid

(no comment)

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */


## setgid32: number 214, handler sys_setgid

(no comment)

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */


## setreuid32: number 203, handler sys_setreuid

(no comment)

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */


## setregid32: number 204, handler sys_setregid

(no comment)

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */


## setresuid32: number 208, handler sys_setresuid

(no comment)

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */


## setresgid32: number 210, handler sys_setresgid

(no comment)

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */


## setgroups32: number 206, handler sys_setgroups

setgroups/getgroups (the *32 forms): the supplementary list, privileged to
set (effective uid 0), NGROUPS_MAX entries. getgroups with size 0 reports the
count, the POSIX sizing idiom. */

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */
