# exit(2)

NAME: exit - the system call family: exit, exit_group.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: Both end the process; exit_group is the same handler, since there are no threads. The init case under the debug shell and the plain status line are in the handler comment.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## exit: number 1, handler sys_exit

Terminate the current process (exit(2)/exit_group(2)). Does not return.
EBX has the exit status. Releases files and cwd, reparents children to init,
publishes the zombie and wakes its parent, then reschedules. A normal child
leaves its mappings, descriptor-table storage, record and stack to do_wait.
A real init's exit is fatal (panic_init_died); an init exiting under the
debug shell returns to it.
The plain status line goes out only where the debug shell is the supervisor.
There it is how a person, and the harness in its no-token mode, read a
transient init's outcome. On an image running `/sbin/init` and a login shell
every process exit would print it, a forked child's included, and the shell
already answers the question through `$?`. The line that hands the shell back
below is not gated: the probe runner scores probes from that one.
An init exiting under the debug shell first ends every other user process
through `proc_end_others`, then returns without scheduling: a child left
runnable would take the processor, and the exited init, a zombie, would
never be picked again to make the return. A process that is not init
leaves through the scheduler as a zombie its parent reaps; when nothing is
ready at that moment, since its parent may be gone, it idles for the next
interrupt with interrupts off between checks and schedules on its own,
because the timer's preemption is deferred inside a call. A halt there
kept the machine forever.

### helper proc_end_others

End every other user process before init returns the machine to the shell.
What the program left running is killed and reaped here while init is
still a live process the scheduler runs: a leftover asleep in the disk
driver with a mount's lock held finishes its call and releases the lock
before the signal ends it, and a leftover spinning in user mode takes the
signal at its next interrupt, since signals are delivered on the return
from an interrupt as well. The shell's resume disables the scheduler, so
anything left runnable or holding a lock then would run no further, and
the next program would wait on it forever. A zombie is reaped as `wait`
reaps one. Kernel threads and the idle record are not user processes
and stay.


## exit_group: number 252, handler sys_exit


