# chdir(2)

NAME: chdir - the system call family: chdir, getcwd.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: The working directory held as a reference; getcwd buffer sizes (ERANGE).

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## chdir: number 12, handler sys_chdir

Change the current working directory (chdir(2)).
EBX = path. Returns 0 or -errno. Stores the canonical absolute path of the
resolved directory (a KRFFS directory, a directory of another mounted
file system, or a synthetic /dev or /proc directory); a regular file is
-ENOTDIR, a missing path -ENOENT.

### helper cred_access_ref

The permission check over a node of any type, from the attributes its
file system reports: the same nine-bit rule as cred_access_node, plus
the execute rule for a regular file.

### helper collapse_dots

Collapse `.` and `..` in an absolute path that a physical walk has just
resolved, for the working directory's string: after the walk succeeded,
every `..` names the parent the walk climbed to. `path` must start with
'/'; the output never grows, so the rewrite is in place.

### helper cred_access_node

The permission check over a KRFFS node. The root directory reads as 0755,
the fixed identity stat reports (its stored mode has the dirty-mount
flag). Executing a regular file demands an execute bit somewhere even for
root: root runs programs, not data.


## getcwd: number 183, handler sys_getcwd

getcwd(2): EBX = buf, ECX = size. Returns the path length including the
NUL, or -errno. */
