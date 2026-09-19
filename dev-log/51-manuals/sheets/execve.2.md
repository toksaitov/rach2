# execve(2)

NAME: execve - the system call family: execve.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: The loader: static ELF only; the environment, the argument limits, set-id handling (the mount flag nosuid), the descriptors closed on exec; what a failed exec leaves.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## execve: number 11, handler sys_execve

Replace the current process image with a new program (execve(2)).
EBX = path, ECX = argv[], EDX = envp[]. Does not return on success. Copies
the path, argv, and envp into static buffers, validates the ELF header
BEFORE tearing the old image down (so a bad executable leaves the caller
intact), then frees the old user pages, streams the new program in, builds
the SysV/i386 initial stack, and redirects the iret frame to the entry
point. A failure after the old image is gone routes to
exec_fail_destroyed.

### helper cred_access_ref

The permission check over a node of any type, from the attributes its
file system reports: the same nine-bit rule as cred_access_node, plus
the execute rule for a regular file.

### helper cred_access_node

The permission check over a KRFFS node. The root directory reads as 0755,
the fixed identity stat reports (its stored mode has the dirty-mount
flag). Executing a regular file demands an execute bit somewhere even for
root: root runs programs, not data.

### helper exec_shebang

Rewrite an exec of a script into an exec of its interpreter.
A file whose first two bytes are `#!` names the program that should run it,
and Linux's rule is what everything expects: the rest of that line is the
interpreter and at most one argument, and the program then runs as
`interpreter [argument] script original-arguments...`. Without this, an exec
of a script fails with `ENOEXEC` and only a shell, which reads the file
itself, can run one. A DHCP client execs its lease script directly, so the
machine needs it.
One level only. A script whose interpreter is itself a script is refused,
not followed, because a chain of them is a loop nobody can see the
end of, and nothing here needs one.
The caller's `path` and `argv_bufs` are rewritten in place on success.
The script is read through `read` over `ctx`: the root's reader by path,
or the mount's over an open handle.
@param argc the argument count, updated when the vector grows.
@return 1 when rewritten, 0 when the file is not a script, or a negative
        errno for one whose first line names nothing.
