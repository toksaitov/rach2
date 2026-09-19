# reboot(2)

NAME: reboot - the system call family: reboot.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 reboot`; the writer adds those lines.

Notes from the milestone: The magic numbers, the commands (halt, power off, restart), uid 0; what a halt does on the console.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## reboot: number 88, handler sys_reboot

reboot(2): stop the machine. EBX = magic1, ECX = magic2, EDX = command.
Does not return for the three commands that stop the machine. Returns
`-EPERM` for an unprivileged caller, `-EINVAL` for a wrong magic or an
unknown command, and 0 for `RB_DISABLE_CAD`.
The privilege check comes before the magics, which inverts the order the rest
of this file uses, and Linux inverts it the same way. A magic-first handler
would let any uid learn the ABI of the one call that stops the machine and
erases the evidence of what it was doing; nothing is learned from `-EPERM`.
`RB_DISABLE_CAD` returning 0 is what makes the rest testable. Every command
that works stops the machine, so without one that succeeds quietly a caller
cannot tell an accepted magic from a rejected one: both would answer
`-EINVAL`. It asks for a state this kernel is permanently in, since there is
no keyboard reboot to disable, so 0 is a true answer. `RB_ENABLE_CAD` asks
for a feature that does not exist and answers `-EINVAL`, because a 0 there
would be the one reply a caller could not tell from a working one.

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */
