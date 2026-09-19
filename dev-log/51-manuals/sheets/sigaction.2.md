# sigaction(2)

NAME: sigaction - the system call family: rt_sigaction, rt_sigprocmask, rt_sigsuspend, sigreturn, rt_sigreturn, pause.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 sigaction`; the writer adds those lines.

Notes from the milestone: The signals this kernel delivers, when (the return from a system call or an interrupt), the default actions, SIGKILL and SIGSTOP, job control signals; the sizes of the sets.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## rt_sigaction: number 174, handler sys_rt_sigaction

Install or query a signal disposition (rt_sigaction, syscall 174).
EBX=signo, ECX=const struct k_sigaction* act, EDX=struct k_sigaction* old,
ESI=sigsetsize. Stores the action per-signal; SIGKILL/SIGSTOP cannot be
changed.


## rt_sigprocmask: number 175, handler sys_rt_sigprocmask

Examine or change the blocked-signal mask (rt_sigprocmask, syscall 175).
EBX=how (0 BLOCK/1 UNBLOCK/2 SETMASK), ECX=const set*, EDX=old set*,
ESI=sigsetsize. SIGKILL/SIGSTOP can never be blocked.


## rt_sigsuspend: number 179, handler sys_rt_sigsuspend

Atomically install a signal mask and wait (rt_sigsuspend(2)).
EBX = const sigset_t *mask, ECX = sigsetsize (8). Replaces the blocked mask
with *mask, waits for a deliverable signal, restores the old mask, then
returns -EINTR. The handler runs under the RESTORED mask: a simplification,
since full save-the-mask-through-the-handler semantics are not modelled.


## sigreturn: number 119, handler sys_sigreturn

sigreturn (syscall 119): return through musl's legacy __restore (plain
handlers, no SA_SIGINFO). It pops the signal word before the `int $0x80`,
so the frame sits 8 bytes below the user esp. Reads the same kernel-built
frame as the rt path; the popped word is the handler's `sig` argument and
is discarded.

### helper signal_restore

The shared body of sigreturn.
Reads the frame the restorer left on the user stack, validates it, and
restores the saved trapframe (segment selectors and EFLAGS forced to safe
user values, since the frame is on the untrusted user stack) plus the
pre-handler signal mask. Returns the restored EAX so the dispatcher does not
clobber it.
`delta` is how far below the current user esp the frame begins, which
differs by trampoline: musl's __restore_rt enters with esp just past
`pretcode` (delta 4), while the legacy __restore pops the signal word
first and so enters 4 bytes higher (delta 8). Both read the same
kernel-built frame.


## rt_sigreturn: number 173, handler sys_rt_sigreturn

rt_sigreturn (syscall 173): return through musl's __restore_rt (SA_SIGINFO
handlers), a bare `int $0x80`, so the frame sits 4 bytes below the user esp.

### helper signal_restore

The shared body of sigreturn.
Reads the frame the restorer left on the user stack, validates it, and
restores the saved trapframe (segment selectors and EFLAGS forced to safe
user values, since the frame is on the untrusted user stack) plus the
pre-handler signal mask. Returns the restored EAX so the dispatcher does not
clobber it.
`delta` is how far below the current user esp the frame begins, which
differs by trampoline: musl's __restore_rt enters with esp just past
`pretcode` (delta 4), while the legacy __restore pops the signal word
first and so enters 4 bytes higher (delta 8). Both read the same
kernel-built frame.


## pause: number 29, handler sys_pause

sys_pause - sleep until a signal is delivered (pause(2)). Returns -EINTR
once a deliverable signal is pending (the signal then acts on the return
to user).
