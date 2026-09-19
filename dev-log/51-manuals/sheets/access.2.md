# access(2)

NAME: access - the system call family: access, faccessat.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: Modes; EROFS for W_OK on a read-only mount; the real ids used, which is access(2)'s own rule.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## access: number 33, handler sys_access

(no comment)

### helper do_access

access core: does the path exist and grant the requested mode?
R/W/X are checked against the object's mode bits by the caller's REAL ids
(the credential model access(2) uniquely uses), which is what makes
/proc/<pid>/stat reject W_OK while granting F_OK/R_OK. `name` is absolute
with the leading '/' stripped.


## faccessat: number 307, handler sys_faccessat

(no comment)

### helper do_access

access core: does the path exist and grant the requested mode?
R/W/X are checked against the object's mode bits by the caller's REAL ids
(the credential model access(2) uniquely uses), which is what makes
/proc/<pid>/stat reject W_OK while granting F_OK/R_OK. `name` is absolute
with the leading '/' stripped.
