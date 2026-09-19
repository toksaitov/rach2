# set_thread_area(2)

NAME: set_thread_area - the system call family: set_thread_area, set_tid_address.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 set_thread_area`; the writer adds those lines.

Notes from the milestone: The one TLS descriptor (GDT entry 6); set_tid_address records and returns the pid, since there are no threads.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## set_thread_area: number 243, handler sys_set_thread_area

Set up thread-local storage (set_thread_area(2), Linux i386).
EBX points at a struct user_desc. Validates it, programs the shared TLS
GDT descriptor (entry GDT_TLS_ENTRY) and this process's PCB base, then
writes the chosen entry number back so the C library can load
(entry_number << 3) | 3 into %gs. The base is reapplied to the GDT on
every context switch (switch.s). Returns 0 or a negative errno on a bad
pointer/request.


## set_tid_address: number 258, handler sys_set_tid_address

Record the clear-child-tid pointer and return the tid (set_tid_address(2)).
Rach2 has no threads, so this only returns the caller's PID as its thread id;
the pointer (EBX) is accepted and ignored.
