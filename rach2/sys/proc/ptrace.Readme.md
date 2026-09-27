# ptrace

`ptrace.c` implements `ptrace(2)`, system call 26, as GDB and strace use it: a tracer process stops the threads of another, its tracees, reads and changes their memory and registers, and resumes them. `man/man2/ptrace.2` gives the user contract.

## State

A thread's record keeps its tracing state (`process.h`): the `PT_` flags, the tracer's process id, the options, the wait status of the ptrace stop it is in or 0, whether the tracer's wait has reported that stop, the signal the tracer resumes it with, the event message, the siginfo of the stop, `orig_eax`, and the debug registers. The tracer is a process, named by its thread group id, so any of its threads may make requests and wait.

## Stops

A tracee stops in `ptrace_stop`, in its own context: it sets the status, posts `SIGCHLD` with `CLD_TRAPPED` to the tracer, wakes the tracer's child queue, and parks as `PROC_STOPPED` until the tracer clears the status, or until `SIGKILL`, which `sig_job_control` makes runnable. `SIGCONT` does not end a ptrace stop. The stops are taken where Linux takes them:

* signal-delivery stops in `deliver_signals`, after a signal is taken off the pending sets and before its disposition is read (`ptrace_signal_stop`); the tracer's resume signal replaces it, and a blocked one is posted back;
* group stops, after the stop signal's own delivery stop, once per stop for each thread (`stop_seen`), after which the tracer may run the thread while its process stays stopped;
* event stops (`ptrace_event`) in `do_fork`, `exec_run`, `exit_current` and `terminate_current_process`; a child a traced thread makes is attached when the options ask for the event, and starts with `SIGSTOP` pending, or with a `PTRACE_EVENT_STOP` still to come for a seized tracer (`PT_START`);
* syscall stops in the dispatcher, at the entry and the exit (`ptrace_syscall_stop`).

A stopped tracee is not running, and its user registers are the trap frame at the top of its kernel stack (`user_frame`), which its entry pushed; the requests read and write that frame. `orig_eax` is the system call a stop is in, the one an interrupted call would restart for a signal-delivery stop, or -1; a tracer's -1 there cancels the restart.

## Memory

`paging_access` (`mm/paging.c`) reads and writes another address space: it brings missing pages in through `vm_resolve_in`, the fault path taking the address space as a parameter, and a write to a private page gives the page a frame of its own first (`page_private`), a copy-on-write fault's copy or a copy with the page's rights for read-only text a fork shares, so a breakpoint in the text changes the tracee alone.

## Wait

`wait_find` reports a tracee to its tracer before the tracee's own parent sees anything: an unreported ptrace stop; a traced thread that ended, which the report frees, since such a thread is not freed at the next return to user mode as an untraced one is; and the end of a process whose parent the tracer is not. The last thread of such a process tells the leader's tracer of the end and not the parent (`proc_exit_cleanup`), and the parent's wait passes the zombie over while another process traces it. The tracer's report lets the process go (`ptrace_exit_release`), which sends the parent `SIGCHLD` and wakes its wait, and the parent reaps it. A thread that is not its process's leader is selected only with `__WALL` or `__WCLONE`. A tracer's end detaches its tracees (`ptrace_tracer_gone`), with `SIGKILL` under `PTRACE_O_EXITKILL`, and tells the parent of a tracee whose process has ended, as the report would have.

## Debug registers

The tracer sets DR0 to DR3 and DR7 through `PTRACE_POKEUSER`, checked by `dr7_valid`; `ptrace_switch_debug`, called by `schedule`, loads them when the thread runs and clears DR7 when the next thread has none. The debug exception reads DR6 into the thread's copy and clears it (`ptrace_debug_status`), and a data breakpoint the kernel's own copy to user memory fires posts `SIGTRAP` and lets the copy go on. The registers are written with byte-coded instructions, since TinyCC's assembler has no mnemonic for them.

## Core files

`coredump.c` writes the core file `core(5)` describes, the form GDB reads: an ELF32 `ET_CORE` file whose `PT_NOTE` segment has each thread's `elf_prstatus`, read from the trap frame at the top of its kernel stack as the requests read it, and its floating-point state, with the process's `elf_prpsinfo` and auxiliary vector, and whose `PT_LOAD` segments have the regions' pages, read through `paging_access`. `core_write` produces the bytes through a writer it is given, and leaves an untouched page to the writer as a hole; `fs_core_file` in `syscall_fs.c` is the writer for the file, which it creates as `core` in the working directory. `terminate_current_process` calls it for a signal whose default action is to dump core, after the other threads are sent `SIGKILL` and before this thread releases anything, and sets `WCOREFLAG` in the status when the whole file was written. The notes are built first, while every thread's record is there; `core_write` then waits until the other threads have ended, each after the call it was in, as Linux's `coredump_wait` waits for them, or until an `execve` of one of them waits for the rest, and takes the regions once, into a list the program headers and the pages follow. Another process that shares the address space, as a `vfork` child does, is not ended and may change the regions while the writer sleeps; each page is looked up again as it is read, and one it unmapped is a hole. A process of 65534 regions or more, whose program headers the ELF header's 16-bit count cannot hold, gets ELF's extended numbering: `PN_XNUM` as that count, and the count in the `sh_info` of one null section header at the end of the file.

## Tests

`tests/musl/ptracemusl.c` traces a child through memory and register access, a single step, a breakpoint in read-only text, syscall stops, the exec event and a debug register, and attaches to and detaches from a running process. `tests/musl/coremusl.c` reads the notes and the memory of the core file an aborted child leaves, and checks the limit, the dumpable flag and a link at the name.
