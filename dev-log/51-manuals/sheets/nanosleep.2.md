# nanosleep(2)

NAME: nanosleep - the system call family: nanosleep.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 nanosleep`; the writer adds those lines.

Notes from the milestone: Ticks are 10 ms; the sleep is rounded up to ticks; the remainder on a signal.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## nanosleep: number 162, handler sys_nanosleep

Block-and-schedule sleep (nanosleep, syscall 162, 32-bit timespec).
EBX = const struct timespec* req, ECX = struct timespec* rem (optional).
Rounds up to whole 10 ms ticks, then parks the process BLOCKED on its
wakeup_tick and yields; the timer wakes it at the deadline while OTHER
processes keep running. rem is written as zero on normal completion, or
the time left on -EINTR.
