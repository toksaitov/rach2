# ptrace

`proc/ptrace.c` implements `ptrace(2)`, system call 26 on i686 and 101 on x86-64, as GDB and strace use it: a tracer process stops the threads of another process, its tracees, reads and changes their memory and registers, and resumes them. The same register code gives a core file its notes, which GDB reads; [the process subsystem's Readme](Readme.md) describes the core file. [ptrace(2)](../../../man/man2/ptrace.2) gives the requests' contract and [core(5)](../../../man/man5/core.5) the file's format; this document describes how the kernel keeps a tracee's state, where a tracee stops, how the requests reach its memory and registers, how a wait reports it, and how the hardware breakpoints work, on both architectures. File names are given from `rach2/sys/`, and `kernel.c` is `rach2/kernel.c`.

## Interfaces

A tracer makes one call, `ptrace`, and learns of its tracees' stops and ends from `wait4` and `waitid` ([wait(2)](../../../man/man2/wait.2)) and from `SIGCHLD`. `/proc/PID/mem` ([proc(5)](../../../man/man5/proc.5)) reads and writes another process's memory through the code the memory requests use.

`proc/ptrace.h` declares the stops, which the code where each happens calls:

| Function | Called from | The stop |
| --- | --- | --- |
| `ptrace_return_stops` | `deliver_signals` | a seized thread's first, and the one `PTRACE_INTERRUPT` requests |
| `ptrace_signal_stop` | `deliver_signals` | a signal about to be acted on |
| `ptrace_group_stop` | `deliver_signals` | the stop of the tracee's process |
| `ptrace_group_change` | `sig_group_stop`, `sig_job_control` | a new stop when a seized tracee's process stops or is sent `SIGCONT` |
| `ptrace_event` | `do_fork`, `ptrace_exec`, `exit_current`, `terminate_current_process`, `seccomp_trace` | an event the options include |
| `ptrace_syscall_stop` | `_syscall_handler` | a system call's entry and exit |

It also declares `ptrace_fork_attach`, which `do_fork` calls; `ptrace_exit_notify`, `ptrace_exit_release` and `ptrace_tracer_gone`, for the end of a tracee or a tracer; `ptrace_tracer_privileged`, which `execve` calls before it applies set-ID bits; and `ptrace_tracee`, which finds a stopped tracee again after a copy that slept.

`arch/arch.h` declares the architecture's part, which each architecture's `ptrace.c` and `arch/x86/debugreg.c` define: `arch_ptrace` answers the architecture's own requests, `arch_regs_get` reads the registers in the order of its `user_regs_struct`, `arch_regset_size`, `arch_regset_get` and `arch_regset_set` move a register set, `arch_ptrace_step` sets or clears the trap flag, `arch_core_fp_size` and `arch_core_fp_notes` write a core file's floating-point notes, and `arch_debug_switch` loads the hardware breakpoints.

Named without their `PTRACE_` prefix, the requests `proc/ptrace.c` answers are `TRACEME`, `ATTACH`, `SEIZE`, the four memory requests, `CONT`, `SYSCALL`, `SINGLESTEP`, `KILL`, `DETACH`, `INTERRUPT`, `LISTEN`, `SETOPTIONS`, `GETEVENTMSG`, `GETSIGINFO`, `SETSIGINFO`, `GET_SYSCALL_INFO`, `GETREGSET` and `SETREGSET`. The architecture answers the rest:

| | i686 (`arch/i686/ptrace.c`) | x86-64 (`arch/x86_64/ptrace.c`) |
| --- | --- | --- |
| Requests of `arch_ptrace` | `PEEKUSER`, `POKEUSER`, `GETREGS`, `SETREGS`, `GETFPREGS`, `SETFPREGS`, `GETFPXREGS`, `SETFPXREGS`, `GET_THREAD_AREA`, `SET_THREAD_AREA` | `PEEKUSER`, `POKEUSER`, `GETREGS`, `SETREGS`, `GETFPREGS`, `SETFPREGS`, `ARCH_PRCTL`, `GET_THREAD_AREA`, `SET_THREAD_AREA`; for an i386 tracer, i386's list without `ARCH_PRCTL` |
| Register sets | `NT_PRSTATUS`, `NT_PRFPREG`, `NT_PRXFPREG`, `NT_X86_XSTATE` | `NT_PRSTATUS`, `NT_PRFPREG`, `NT_X86_XSTATE`; for a tracee running 32-bit code, i386's, with `NT_PRXFPREG` and `NT_386_TLS` |
| A core file's floating-point notes | `NT_PRFPREG`, `NT_PRXFPREG`, and `NT_X86_XSTATE` with XSAVE | `NT_PRFPREG`, and `NT_X86_XSTATE` with XSAVE |

An i386 tracer is a thread that makes `ptrace` by `int $0x80` on x86-64 (`in_compat_syscall`). It gets i386's interface, as from a Linux with i386 emulation: `ptrace_stopped` moves 4-byte words for the memory requests and `GETEVENTMSG`, `siginfo_access` gives the i386 siginfo (`siginfo_to_caller`, `siginfo_from_caller`), `regset_access` reads the 8-byte i386 `struct iovec`, and `arch_ptrace` answers through `ia32_arch_ptrace`, with `user_regs_struct32` and `struct user32`. The register sets follow the tracee instead, as Linux's `task_user_regset_view` chooses them: a tracee whose code segment is not the 64-bit one has the i386 sets for any tracer.

Any other request answers `EIO`. [The architectures' Readme](../arch/Readme.md) gives each architecture's `user_regs_struct` and `struct user`.

## Design

### The tracing state

A thread's record (`proc/process.h`) keeps its tracing state:

* `ptrace`, the `PT_` flags of `ptrace.h`: traced, seized, stopping at system calls, stepping, a stop requested (`PT_INTERRUPT`), a seized thread's first stop still to come (`PT_START`), and kept in its stop by `PTRACE_LISTEN`;
* `tracer`, the tracer's process ID, and `ptrace_opts`, the options;
* `ptrace_status`, the wait status of the ptrace stop the thread is in, or 0; `ptrace_reported`, set once the tracer's wait has reported that stop; `ptrace_signo`, the signal the tracer resumes it with; `ptrace_msg`, the event's message, which a system call stop sets to 1 at the entry and 2 at the exit, as Linux does, so that `PTRACE_GET_SYSCALL_INFO` tells the two apart; and `ptrace_info`, the siginfo of the stop;
* `orig_nr`, the system call a stop is in, or -1, which the tracer reads and writes as `orig_eax` or `orig_rax`;
* `ptrace_exit_seen`, set once the tracer's wait has reported the end of the process;
* `debugreg[8]`, the debug registers as the tracer set them.

The tracer is a process, not a thread: `tracer` is its thread group's ID, so any of its threads may make the requests and wait. Nothing in the tracer's record lists its tracees; its wait finds them by their `tracer`.

`ptrace_attach`, for `PTRACE_ATTACH` and `PTRACE_SEIZE`, and `ptrace_traceme` set the state; [ptrace(2)](../../../man/man2/ptrace.2) gives who may. Since each thread of the tracer may make the requests, a tracer is privileged only when every live thread of it is (`group_privileged`). An unprivileged caller is refused a thread inside `execve` (its `exec_ws` set), because the exec sets the new image's IDs according to the tracer the thread has: it ignores set-ID bits under a tracer that is not privileged (`exec_traced` in `syscall_proc.c`). `PTRACE_ATTACH` sends `SIGSTOP`, and both requests make a thread parked in its process's stop runnable (`ptrace_unpark`), so that it makes that stop a ptrace stop. `PTRACE_SEIZE` and `PTRACE_SETOPTIONS` test every bit of the caller's word, all 64 on x86-64, against `PTRACE_O_MASK`, and refuse an unknown one without changing anything.

### Stops

A tracee stops in `ptrace_stop`, on its own kernel stack. It stores the status and the siginfo, sends the tracer `SIGCHLD` with `CLD_TRAPPED` and the thread's ID and real user ID translated into the tracer's namespaces, wakes the tracer's wait, and sleeps as `PROC_STOPPED` until a resume clears the status or a `SIGKILL` is pending; `sig_job_control` makes a thread in a ptrace stop runnable for a `SIGKILL` but not for a `SIGCONT`, so a `SIGCONT` does not end a ptrace stop. A thread whose tracer has gone does not stop, and is no longer traced. The stops are taken where Linux takes them:

* A signal-delivery stop (`ptrace_signal_stop`), in `deliver_signals`, after a signal is taken off the pending sets and before its disposition is read; `SIGKILL` makes none. The signal the tracer resumes with is the one acted on: the same signal keeps its siginfo, which `PTRACE_SETSIGINFO` may have changed, another gets `SI_USER` with the tracer's process ID and real user ID translated into the thread's namespaces (`tracer_info`), 0 drops it, and one the thread blocks is posted back to the thread.
* A group stop (`ptrace_group_stop`), when the process stops: once for each thread and stop (`stop_seen`), with the stop signal, and with `PTRACE_EVENT_STOP` in the status for a seized thread. The tracer may then run the thread while its process stays stopped. A thread parked in its process's stop when a tracer attaches leaves the park (`sig_park`) and makes its group stop then.
* An event stop (`ptrace_event`) with `SIGTRAP`, when the options include the event: in `do_fork` after a fork, a vfork or a clone and at the vfork's end, after an `execve` (`ptrace_exec`), in `exit_current` and `terminate_current_process` as a thread begins to end, and in `seccomp_trace` for a filter's `SECCOMP_RET_TRACE`. A child a traced thread makes is traced too when the options include its event (`ptrace_fork_attach`), and starts with `SIGSTOP` pending, or under a seizing tracer with its first stop still to come (`PT_START`). Without the exec option, an exec posts `SIGTRAP` to a thread that was not seized.
* A system-call stop (`ptrace_syscall_stop`), at the entry and the exit of each call after `PTRACE_SYSCALL`, which the next section describes.
* A return stop (`ptrace_return_stops`), as a seized thread returns to user mode: its first, and the one `PTRACE_INTERRUPT` requests, each a `PTRACE_EVENT_STOP` with `SIGTRAP`, or with the stop signal while the process is stopped. The loop looks again after each stop, so a request made while the thread was stopped is answered before a user instruction runs.

`PTRACE_INTERRUPT`, and a change of a seized thread's group stop, set `PT_INTERRUPT` and the thread's signal-work flag, `sig_woken`, and wake the thread if it sleeps, as Linux's `ptrace_signal_wake_up` sets TIF_SIGPENDING before its wake: a wait without the big lock, such as a pipe's, ends only on that flag, and would otherwise find its condition false and sleep again. `signal_pending` returns nonzero for it as for a stop signal, so a thread asleep in an interruptible call returns `EINTR` and makes the stop on its way back to user mode, and a call that a stop and a continue would restart is restarted once the tracer resumes it. A traced thread's wait also ends on a signal it ignores, as Linux's `sig_ignored` has it, so the tracer sees that signal at a delivery stop; with the signal suppressed or ignored, the wait is restarted as after a stop.

`PTRACE_LISTEN` (`ptrace_listen`) keeps a seized thread in a `PTRACE_EVENT_STOP` as a stopped process keeps its threads. When the process's stop begins, and on every `SIGCONT`, whether or not it ends a stop, as Linux's `prepare_signal` calls `ptrace_trap_notify`, `ptrace_group_change`, Linux's `JOBCTL_TRAP_NOTIFY`, requests a new stop of each seized thread (`ptrace_restop`): a listening thread leaves its stop and makes the new one at once, and another makes it when its tracer next resumes it. A thread that reports the group stop itself drops the request, which the report answers. A request that comes before the `PTRACE_LISTEN` is kept, and the listen then resumes the thread into the new stop at once: strace answers a group stop with `PTRACE_LISTEN`, and the `SIGCONT` it sends can end the stop on another processor before the listen.

### Registers and system calls

A stopped tracee is not running, so its user registers are the frame its entry to the kernel pushed at the top of its kernel stack (`proc_user_frame`), and the requests read and write that frame. `regs_set` in each `ptrace.c` changes only what a return to user mode can load, as Linux's `putreg` does: of the flags only those a program may change, the code and stack selectors only to the user's, on i686 a data selector only to the null, user data or TLS selector, and on x86-64 the DS, ES, FS and GS selectors only to the null or a user selector and the FS and GS bases only to user addresses.

Every entry from user mode that is not a system call, a processor exception or an interrupt, sets `orig_nr` to -1 (`user_entry` in `arch/x86/isr.c`), as Linux sets `orig_ax`, so that a tracer and a core file read -1 there after a fault. At a system call's entry stop, `orig_nr` is the call's number and the result register is `-ENOSYS`. Neither is put back after the stop: the dispatcher (`_syscall_handler` in `syscall.c`) reads the number again from `orig_nr`, runs that call with the registers the tracer left, answers `ENOSYS` for a number of no call, and runs none for -1, whose result is what the tracer left in the result register. A thread sent `SIGKILL` during the entry stop runs no call. The seccomp filters then judge the call the tracer left.

The dispatcher translates an interrupted call's result into Linux's restart code before the exit stop: -512 (`ERESTARTSYS`), -513 (`ERESTARTNOINTR`), -514 (`ERESTARTNOHAND`) or -516 (`ERESTART_RESTARTBLOCK`), or keeps -4 (`EINTR`) for a call that does not restart ([signals](signal.Readme.md)). The code stays in the result register through the signal-delivery stop. After that stop, `deliver_signals` reads the live result and `orig_nr`: a restart code follows its own rule, any other result cancels the restart, and a changed call number is the number restarted. A compat result is sign-extended from 32 bits before the comparison, including a code an i386 tracer wrote back. Saving the registers, setting `orig_nr` to -1 and then restoring the registers restores the restart too.

A call the dispatcher translated counts as signal work, as Linux keeps `TIF_SIGPENDING` set until `get_signal` runs, so it restarts by its rule even when another thread took its signal before it returned. With `orig_nr` -1, or with no pending signal or stop to take and a call that was not interrupted, a code the tracer wrote reaches the program unchanged, as on Linux. Otherwise a code whose rule does not restart after the selected handler becomes `EINTR`. A restart through the block enters `restart_syscall`, 0 on i386 and 219 on x86-64, which the next entry stop shows in `orig_nr`. A signal return clears `orig_nr` to -1, so another pending signal cannot treat the restored user registers as an interrupted call.

`PTRACE_SINGLESTEP` over a system call stops at its exit with `SIGTRAP` and `TRAP_TRACE`, with the result in place and before the next user instruction; `PTRACE_O_TRACESYSGOOD` does not mark that stop. A signal the tracer gives when it resumes from a system-call stop is posted to the thread afresh, with the siginfo a signal-delivery stop gives another signal.

### Memory

The memory requests go through `tracee_memory`, which holds a reference to the tracee's address space while `paging_access` (`mm/paging.c`) reads or writes it. That function works on an address space that need not be the one loaded. It brings a missing page in as a fault would (`vm_resolve_in`). Before a write to a private page it gives the page a frame of its own (`page_private`): the copy a copy-on-write fault makes, or, for a read-only page another address space shares, such as the text a fork left in both, a copy with the same rights. A breakpoint written into the text so changes the tracee alone, and the text stays read-only. It refuses a page of no region, of a `PROT_NONE`, guard or device region, of memfd_secret or of the vDSO's data page, `[vvar]`, and a read-only shared page for a write, as Linux's `get_user_pages` refuses them. `/proc/PID/mem` and the core file read memory the same way. A caller that is not privileged gets `EIO` for an address space that is not dumpable, as an exec after the attach can make it.

### Requests that sleep

Bringing a page in, and a copy to or from the tracer's own memory, may sleep, and meanwhile a `SIGKILL` may end the tracee, or another thread of the tracer may resume it and reap it. So no request uses the tracee's record after such a sleep. A memory request holds the address space and not the record; a request that copies out takes what it copies before the copy; and a request that copies in, as `PTRACE_SETREGS` does, finds the tracee again after the copy (`ptrace_tracee`) and answers `ESRCH` when it is no longer in a ptrace stop.

`PTRACE_GETREGSET` and `PTRACE_SETREGSET` (`regset_access`) read the `struct iovec`, refuse a length that is not a multiple of the set's unit, cut it to the set's size, move the set through a buffer from the heap, since an XSAVE image is larger than a kernel stack frame should be, and write back the length moved, as Linux's `ptrace_regset` does. The architecture gives each set's size and unit and converts it.

### Waits and ends

`wait_find` (`syscall_proc.c`) looks at the caller's tracees as well as its children (`wait_tracee`), and reports a tracee to its tracer before its parent's wait reports anything:

* a ptrace stop the wait has not reported, once, with `CLD_TRAPPED`, and `si_status` the status shifted right by 8 bits, the signal with the event above it, so that a caller of `waitid` can tell an event stop from a plain `SIGTRAP`;
* a traced thread that ended, which the report frees, since such a thread is not freed at another thread's return to user mode as an untraced one is;
* the end of a process whose parent is another process. The last thread of such a process notifies the tracer and not the parent (`proc_exit_cleanup` in `proc/exit.c`), the parent's wait passes the zombie over while it is traced, and the tracer's report releases the process (`ptrace_exit_release`), which posts `SIGCHLD` when the child requested notification and always wakes its wait.

A tracer's wait selects every tracee as if `__WALL` were set, including nonleader threads and zero-signal children. A traced thread's end also sends its tracer `SIGCHLD` (`ptrace_exit_notify`), with the siginfo `proc_child_info` makes for the tracer, translated into the tracer's namespaces as a parent's is into the parent's.

`ptrace_detach`, for `PTRACE_DETACH`, a tracer's end and the report of a process's end, clears the tracing state, the trap flag the kernel set and the debug registers, and resumes a stopped tracee with the signal given. A tracee whose process is stopped then parks in that stop: a live one owes the stop again, and is marked for signal work and woken (`ptrace_signal_wake`), as Linux's `__ptrace_unlink` sets `JOBCTL_STOP_PENDING` and calls `ptrace_signal_wake_up`, since one its tracer resumed during the stop may sleep in a wait, such as a pipe's, that ends only on that flag. When the tracer's process ends (`ptrace_tracer_gone`), each tracee is detached, sent `SIGKILL` first under `PTRACE_O_EXITKILL`, and the parent of a tracee whose process has ended is notified, as the tracer's report would have notified it.

### The hardware breakpoints

The x86 debug registers are DR0 to DR3, four breakpoint addresses; DR6, the status of the last debug exception; and DR7, which enables each breakpoint for an execution, a write or an access of 1, 2 or 4 bytes, or 8 on x86-64. A tracer sets them through `PTRACE_POKEUSER` at the `u_debugreg` words of `struct user`. `debugreg_poke` (`arch/x86/debugreg.c`, which both architectures use) keeps them in the thread's `debugreg` after `dr7_valid` checks them: a breakpoint must cover user addresses alone, or it would fire in the kernel's own code, and DR7 may set no I/O breakpoint, general-detect bit or reserved bit. [ptrace(2)](../../../man/man2/ptrace.2) gives the rule.

`arch_debug_switch`, which `schedule` calls (`proc/sched.c`), loads DR0 to DR3 and DR7 when the next thread has a breakpoint enabled, and clears DR7 when it has none and the processor had some loaded. A flag for each processor, `dr7_live`, records what the processor has loaded apart from the thread's copy, since a tracer that ends on another processor clears a running tracee's copy (`ptrace_detach`) while the processor still has the breakpoints. When such a breakpoint fires, the thread's copy has no enables: `debugreg_code` clears DR7 and no signal is sent, though a single step at the same instruction still sends its own. `debugreg_resume` loads the thread's breakpoints again on each return to user mode, after a dropped kernel exception turned them off. The instructions that move the registers are written as bytes, since TinyCC's assembler has no mnemonic for them.

A debug exception in user mode belongs to the thread: `debugreg_status` moves DR6 into the thread's copy and clears it, and the exception becomes `SIGTRAP` with `TRAP_HWBKPT` for a breakpoint or `TRAP_TRACE` for a single step, forced as any user exception is ([signals](signal.Readme.md)).

A debug exception in kernel mode is dropped, as Linux drops it: Linux's ptrace breakpoints exclude the kernel (`ptrace_breakpoint_init` sets `exclude_kernel`), so `perf_bp_event` never calls `ptrace_triggered` for a hit in kernel mode and `exc_debug_kernel` sends no signal. Such an exception comes when the kernel touches a watched user address in a copy to or from the tracee's memory, and the copy goes on; or when a watched `MOV SS` delays its exception past an `int $0x80` or `syscall` to the first instruction of the kernel's entry. On i686 the handler (`_exception_handler` in `arch/x86/isr.c`) and on x86-64 the debug entry, on its own interrupt stack ([the architectures' Readme](../arch/Readme.md)), call `debugreg_drop`, which clears DR6 and turns the processor's breakpoints off until the next switch or return to user mode, so that the rest of a copy through a watched range takes no further exception. No signal is posted, no wait ends and the thread's saved DR6 is unchanged; a tracee's own access to the address after the call still traps.

### Core files

A core file has the registers of each thread as the requests read them. [The process subsystem's Readme](Readme.md) describes how the kernel writes one.

## Limits

* The tracer is a whole process; Linux makes the attaching thread the tracer.
* An unprivileged caller's attach to a thread inside `execve` fails with `EPERM`, where Linux waits for the exec to end.
* `PTRACE_INTERRUPT` of a call that is never restarted, such as `nanosleep`, makes it fail with `EINTR`, where Linux restarts it.
* Linux's `PTRACE_PEEKSIGINFO`, `PTRACE_GETSIGMASK`, `PTRACE_SETSIGMASK` and `PTRACE_SYSEMU` are among the requests that answer `EIO`.
* On x86-64 `orig_rax` is kept in 32 bits.
* i686 has one TLS entry for `PTRACE_GET_THREAD_AREA` and `PTRACE_SET_THREAD_AREA` ([thread-local storage](../arch/tls.Readme.md)).
* The core file is always named `core` and has no `NT_SIGINFO` or `NT_FILE` note ([core(5)](../../../man/man5/core.5)).

## Tests

`tests/run default` runs every guest probe, each in a boot of its own on the test image ([the tests' Readme](../../../tests/Readme.md)). Among them:

* `tests/musl/ptracemusl.c` traces a child as GDB does: its memory through the requests and `/proc/PID/mem`, its registers and floating-point state, a single step, a breakpoint in read-only text, the system-call stops and a call changed or skipped at its entry, a restart cancelled at the exit, the exec event, a debug register, an attach, a detach and a seize, `PTRACE_INTERRUPT` of a read and of a listening tracee, the fork event under a seizing tracer, the reports of a tracee's end to its tracer and its parent, and the thread pointer through `PTRACE_ARCH_PRCTL` on x86-64 and `PTRACE_SET_THREAD_AREA` on i686. Its debug-register rows step over a system call to its exact return and watch the kernel's access to user memory in a `read`, a `poll` and an `rt_sigtimedwait`, which stops nothing and posts nothing, while the tracee's own write of the word after the `read` still traps.
* `tests/xsaveprobe.c` reads and writes a stopped child's YMM registers through `NT_X86_XSTATE` and finds them in a core file's note.
* `tests/musl/coremusl.c` reads the notes and the memory of the core file an aborted child leaves, and checks the limit, the dumpable flag, a link at the name, and a process of two threads.
* `tests/musl/suidmusl.c` requires `EIO` for `PTRACE_PEEKTEXT` of an image its tracer may not read, `tests/musl/capmusl.c` the refusals of `PTRACE_SEIZE` and `PTRACE_TRACEME` by privilege, and `tests/musl/threadmusl.c` uses a tracer to hold a thread at the entry of an `execve`.

`tests/run fpu` requires `NT_X86_XSTATE` to be `ENODEV` where the kernel keeps the state without XSAVE ([the suite inventory](../../../tests/system/suites.md)).

`tests/run host` ([the host tests](../../../tests/host/Readme.md)) runs `ptrace_hosttest.c`, which includes `proc/ptrace.c`: a tracee resumed during a register set's iovec copy, a syscall step's status and siginfo, and a listening stop. `debugreg_hosttest.c` models two processors: stale enables after a detach, and a kernel hit dropped with the processor's enables, which the return to user mode loads again, also after a switch or a migration. `coredump_hosttest.c` checks `core_write` against regions that change while it writes, the wait for the parked threads, and the extended numbering; `corefile_hosttest.c` checks `fs_core_file`'s descriptor and limit; and `fpuframe_hosttest.c` and `fpuframe64_hosttest.c` check the XSAVE image of the register set and the note.

Every suite in `tests/registry` runs on both architectures unless its row says otherwise; `ARCH=x86_64 tests/run NAME` runs it on x86-64.
