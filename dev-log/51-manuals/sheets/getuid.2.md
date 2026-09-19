# getuid(2)

NAME: getuid - the system call family: getuid32, geteuid32, getgid32, getegid32, getresuid32, getresgid32, getgroups32.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 getuid`; the writer adds those lines.

Notes from the milestone: The identity family, read side.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## getuid32: number 199, handler sys_getuid

Identity (the i386 *32 variants, 199-202): the four getters read the PCB
credentials, all zero (root) until a login server drops them. The setters
and the group list live with the rest of the id family near do_chown.


## geteuid32: number 201, handler sys_geteuid

(no comment)


## getgid32: number 200, handler sys_getgid

(no comment)


## getegid32: number 202, handler sys_getegid

(no comment)


## getresuid32: number 209, handler sys_getresuid

getresuid/getresgid (the *32 forms): report all three ids through the three
user pointers, the introspection half of the saved-id model. */


## getresgid32: number 211, handler sys_getresgid

(no comment)


## getgroups32: number 205, handler sys_getgroups

(no comment)
