# Signals

A signal notifies a process or one of its threads of an event: a key the terminal maps to an interrupt, a child that ended, a fault of its own instruction, a timer, or a `kill` from another process. The kernel keeps the signals sent and acts on one each time a thread returns to user mode: it ignores the signal, stops or ends the process, or runs a handler the program installed, on a stack frame laid out as Linux lays it out, so that musl, BusyBox and GNU libc programs built for Linux i386 or x86-64 run unchanged. The code is in `syscall_proc.c`, and the frames are in each architecture's `sigframe.c`. The calls' contracts are in their manual pages, chiefly [sigaction(2)](../../../man/man2/sigaction.2) and [kill(2)](../../../man/man2/kill.2); this document describes how the kernel keeps and delivers signals on both architectures, and [ptrace](ptrace.Readme.md) how a tracer intercepts them. File names are given from `rach2/sys/`, and `kernel.c` is `rach2/kernel.c`.

## Interfaces

### System calls

| Call | i686 | x86-64 | Manual page |
| --- | --- | --- | --- |
| `kill` | 37 | 62 | [kill(2)](../../../man/man2/kill.2) |
| `tkill` | 238 | 200 | [kill(2)](../../../man/man2/kill.2) |
| `tgkill` | 270 | 234 | [kill(2)](../../../man/man2/kill.2) |
| `rt_sigqueueinfo` | 178 | 129 | [rt_sigqueueinfo(2)](../../../man/man2/rt_sigqueueinfo.2) |
| `rt_tgsigqueueinfo` | 335 | 297 | [rt_sigqueueinfo(2)](../../../man/man2/rt_sigqueueinfo.2) |
| `rt_sigaction` | 174 | 13 | [sigaction(2)](../../../man/man2/sigaction.2) |
| `rt_sigprocmask` | 175 | 14 | [sigaction(2)](../../../man/man2/sigaction.2) |
| `sigprocmask` | 126 | none | [sigaction(2)](../../../man/man2/sigaction.2) |
| `rt_sigpending` | 176 | 127 | [sigaction(2)](../../../man/man2/sigaction.2) |
| `rt_sigsuspend` | 179 | 130 | [sigaction(2)](../../../man/man2/sigaction.2) |
| `pause` | 29 | 34 | [sigaction(2)](../../../man/man2/sigaction.2) |
| `rt_sigtimedwait` | 177 | 128 | [sigaction(2)](../../../man/man2/sigaction.2) |
| `sigaltstack` | 186 | 131 | [sigaction(2)](../../../man/man2/sigaction.2) |
| `sigreturn` | 119 | none | [sigaction(2)](../../../man/man2/sigaction.2) |
| `rt_sigreturn` | 173 | 15 | [sigaction(2)](../../../man/man2/sigaction.2) |
| `signalfd` | 321 | 282 | [signalfd(2)](../../../man/man2/signalfd.2) |
| `signalfd4` | 327 | 289 | [signalfd(2)](../../../man/man2/signalfd.2) |
| `alarm` | 27 | 37 | [setitimer(2)](../../../man/man2/setitimer.2) |
| `setitimer` | 104 | 38 | [setitimer(2)](../../../man/man2/setitimer.2) |
| `getitimer` | 105 | 36 | [setitimer(2)](../../../man/man2/setitimer.2) |

Signals also reach programs through [wait(2)](../../../man/man2/wait.2), which reports a child's stop, continue and end, `PR_SET_PDEATHSIG` of [prctl(2)](../../../man/man2/prctl.2), [timer_create(2)](../../../man/man2/timer_create.2), and the [core(5)](../../../man/man5/core.5) file a core signal's default action leaves.

Signals 1 to 64 are accepted (`KSIG_MAX` in `lib/ksignal.h`); 1 to 31 have Linux i386's names and numbers, which x86-64 shares. A set is a 64-bit word with bit `signo - 1` for each signal (`KSIG_BIT`), which the calls pass as eight bytes.

### Inside the kernel

`proc/process.h` declares what other subsystems call:

* `signal_post` sends a signal to the process a record belongs to, with a siginfo of `SI_KERNEL`; `signal_post_info` sends one with a siginfo the caller fills; `signal_post_thread` sends one to a single thread, which alone may take it.
* `signal_force` sends the signal of a processor exception to the faulting thread. A blocked or ignored signal goes back to its default disposition and is unblocked first, so the process dies unless a handler is installed.
* `signal_set_blocked` is the one way a thread's blocked mask changes.
* `signal_pending` is what an interruptible wait calls before it sleeps and after it wakes; the wait ends with `EINTR` when it returns nonzero.
* `deliver_signals` is called by `isr_leave` (`arch/x86/isr.c`) on each return from an exception, a device interrupt or a system call.
* `proc_child_info` fills the siginfo of a child's `SIGCHLD`, and `tty_signal_group` (`tty/tty.c`) posts a signal to every live user process of a process group.

The posting functions may be called with interrupts off and from an interrupt handler: the PS/2 keyboard posts Ctrl-C's `SIGINT` from its interrupt, and the timer `SIGALRM` from its. They change the 64-bit sets with interrupts off (`sig_irq_save`), since on i686 a read-modify-write of 64 bits is two stores.

## Design

### The state

A process is a thread group (`proc/tgroup.h`), and its signal state is divided as POSIX divides it. The group has the dispositions, `action[1..64]`, each a `struct k_sigaction` (`lib/ksignal.h`) in the layout musl's `rt_sigaction` passes: the handler, or `KSIG_DFL` (0) or `KSIG_IGN` (1), the flags, the restorer and the mask. Each thread's `sigacts` points at that array. The group also has `pending` and `info[1..64]`, the signals sent to the process as a whole and their siginfo; `stop`, the signal of the stop in effect, or 0; and `cpu_left` and `cpu_interval`, the virtual and profiling interval timers.

Each thread's record (`proc/process.h`) has:

* `sig_pending` and `sig_info[1..64]`, the signals sent to that thread alone, and `sig_blocked`, its mask;
* `altstack_sp`, `altstack_size` and `altstack_on`, the alternate stack;
* `saved_sigmask` and `sigmask_saved`, the mask a wait replaced for its duration;
* `restart_nr`, one more than the number of a system call to restart, or 0;
* `trap_nr`, `trap_err` and `trap_addr`, the vector, error code and fault address of its last processor exception, which a frame's `sigcontext` reports;
* `stop_seen`, set once the thread has taken the stop in effect;
* in the leader's record, `jc_status`, the stop or continue report for the parent's wait, and `alarm_tick` and `alarm_interval`, the real-time interval timer.

A pending signal is one bit, with one `struct k_siginfo` beside it. A signal posted again while it is pending changes nothing, so instances merge, and the siginfo kept is the first one's.

`fork` (`fork_signals` in `syscall_proc.c`) gives the child a group of its own with a copy of the dispositions, the parent's mask, nothing pending, no timers and the parent's alternate stack. A thread made with `CLONE_THREAD` shares its creator's group and takes its mask. A child that shares the address space without `CLONE_VFORK` gets no alternate stack. `execve` (`exec_commit`) puts each caught signal back to its default action, keeps the ignored ones, the mask, the pending signals and the interval timers, deletes the POSIX timers and removes the alternate stack.

### Sending

`signal_post_info` sets the process's pending bit, keeping the siginfo when the signal was not pending already, applies the job-control rules below, and wakes the one thread `sig_taker` selects, as Linux's `complete_signal` selects it: the thread the signal was sent to when it can take the signal, and otherwise the first thread that can. A thread can when it has not ended, is not stopped, and does not block the signal; when none can, the thread the signal was sent to is woken, unless it has ended. A process whose threads have all ended takes nothing. `signal_post_thread` sets the thread's own bit and wakes that thread. Waking (`sig_wake`) makes a thread asleep in the kernel runnable and clears its timed wake; an interruptible wait then calls `signal_pending`, and a wait that goes on arms its deadline again.

A signal's job-control effect is applied when it is posted, whatever its disposition and the mask (`sig_job_control`):

* `SIGCONT` discards the pending stop signals, the process's and each thread's. When the process is stopped, it clears the stop, makes runnable every stopped thread that is not in a ptrace stop, records the continue in `jc_status` for `wait4(WCONTINUED)`, and sends the parent `SIGCHLD` with `CLD_CONTINUED`.
* A stop signal, `SIGSTOP`, `SIGTSTP`, `SIGTTIN` or `SIGTTOU`, discards a pending `SIGCONT`.
* `SIGKILL` makes every stopped thread of the process runnable, so that it can take its default action.

`signal_set_blocked` removes `SIGKILL` and `SIGSTOP` from every mask. When the new mask blocks a signal pending for the process that the old mask did not, it wakes another thread that can take the signal, as Linux's `retarget_shared_pending` does, since `sig_taker` may have woken this thread alone for it. Every mask change goes through it: `rt_sigprocmask` and `sigprocmask`, a handler's mask and its return, and the masks of `rt_sigsuspend`, `rt_sigtimedwait`, a signalfd read, `ppoll`, `pselect6`, `epoll_pwait` and `epoll_pwait2`. Installing `SIG_IGN` discards a pending instance, the process's and each thread's (`sig_discard`).

### Permission and senders

`cred_may_signal` decides whether one process may signal another. A privileged sender, one for which `proc_privileged` is true ([the process subsystem's Readme](Readme.md)), may signal any process. Otherwise the sender's real or effective user ID must equal the target's real or saved user ID; the saved ID lets the user who started a set-user-ID program signal it. `SIGCONT` is also allowed within one session: `/bin/su` starts its shell with real user ID 0, and the user's shell that ran `su` as a job resumes it with `SIGCONT`, which the rule of user IDs alone would not allow. Signal 0, which tests a target and sends nothing, has no such exception. A signal from the terminal (`tty_signal_group`) is not checked, since no process sent it.

`kill` (`do_kill`) selects a process by the ID of any of its threads, translated through the caller's PID namespace (`pid_named`), or a process group; `kill(-1)` answers `ESRCH`. A process whose threads have all ended takes the signal and discards it until it is reaped, as on Linux. The siginfo is `SI_USER` with the sender's process ID and real user ID translated into the target's namespaces (`sender_info`); `tkill` and `tgkill` post to the thread with `SI_TKILL`. `rt_sigqueueinfo` and `rt_tgsigqueueinfo` post the siginfo the caller gives (`sigframe_take_siginfo`), and refuse a code of 0 or above, or `SI_TKILL`, to another process, since those codes are the kernel's.

### Exceptions

A processor exception in user mode belongs to the faulting thread. `_exception_handler` (`arch/x86/isr.c`) and the page fault handler (`mm/paging.c`) record the vector, the error code and, for a page fault, the address in the thread's record, and force the signal with `signal_force`:

| Exception | Signal | `si_code` |
| --- | --- | --- |
| Divide error (0) | `SIGFPE` | `FPE_INTDIV` |
| Debug (1) | `SIGTRAP` | `TRAP_HWBKPT` or `TRAP_TRACE`, from DR6 |
| Breakpoint (3), `int3` | `SIGTRAP` | `SI_KERNEL` |
| Invalid opcode (6) | `SIGILL` | `ILL_ILLOPN` |
| Segment not present (11), stack fault (12) | `SIGBUS` | `SI_KERNEL` |
| Page fault (14) | `SIGSEGV` | `SEGV_MAPERR` outside every region, `SEGV_ACCERR` inside one |
| Page fault for which no frame could be had | `SIGKILL` | `SI_KERNEL` |
| x87 error (16), SIMD error (19) | `SIGFPE` | the first of `FPE_FLTINV`, `FPE_FLTDIV`, `FPE_FLTOVF`, `FPE_FLTUND` and `FPE_FLTRES` the unit raised unmasked |
| Alignment check (17) | `SIGBUS` | `BUS_ADRALN` |
| Any other, the protection fault (13) and the overflow of `into` (4) among them | `SIGSEGV` | `SI_KERNEL` |

The divide error, the invalid opcode, the floating-point errors and the debug exception give the saved instruction pointer as `si_addr`, and the page fault the address it touched. A handler that returns without changing the saved registers runs a faulting instruction again; after a trap, `int3`, `into` or a single step, the program goes on with the next instruction, since the processor saves that instruction's address for a trap. The out-of-memory `SIGKILL` is forced because a `SIGSEGV` handler that returned would fault again. A seccomp filter's `SECCOMP_RET_TRAP` forces `SIGSYS` the same way (`proc/seccomp.c`). A debug exception in kernel mode is not the program's fault; [ptrace](ptrace.Readme.md) describes it.

### Waits

`signal_pending` returns nonzero when the thread has a signal pending, its own or its process's, that it does not block and does not ignore: one with a handler, or one whose default action ends or stops the process. A signal ignored by `SIG_IGN` or by a default of ignoring ends no wait. It also returns nonzero when a tracer requested a stop with `PTRACE_INTERRUPT`, and for a hardware breakpoint hit deferred in the kernel ([ptrace](ptrace.Readme.md)).

The interruptible waits call it: `wq_wait_event` (`proc/wait_queue.h`), which pipes, sockets and terminals sleep in, `poll_wait` (`io/poll.c`), the sleeps, the wait for a child (`wait_find`), and the waits for a signal. `wq_wait_event` tests its condition first, so a wait whose event has happened returns its result with a signal pending, and a call that moved bytes before the signal returns their count. `wq_wait_event_nointr`, for a lock another process has or a transfer a device has in flight, does not call it. Each call's page gives its own rule.

`pause` and `rt_sigsuspend` wait in `sig_wait` until `signal_pending` returns nonzero, and return `EINTR`; the signal acts on the way out. `rt_sigtimedwait` and a signalfd read take a signal of their set instead (`sigwait_take`). While they wait the set is unblocked, so that `sig_taker` can select the waiting thread, and the old mask comes back when they return. A signalfd has no wait queue: `poll` finds it readable while a signal of its set is pending for the poller, and a signal posted while the poller sleeps wakes it when it is the thread `sig_taker` selects.

### Delivery

`isr_leave` calls `deliver_signals` with interrupts off and the big lock held; the entries that take no lock, the interprocessor interrupt's among them, return without it ([the architectures' Readme](../arch/Readme.md)). On a return to kernel mode it does nothing. On a return to user mode it frees the records of threads that have ended (`reap_threads`), makes the stops a tracer requested of a traced thread (`ptrace_return_stops`), and then loops:

1. When the process is stopped and the thread has not taken the stop, the thread takes it (`group_stop_taken`): it parks in `sig_park` until a `SIGCONT` or a `SIGKILL`, or, when traced, makes a ptrace stop instead.
2. It takes one signal it does not block off the pending sets (`sig_dequeue`): the lowest-numbered of its own, or when it has none, the lowest-numbered of its process's, as Linux's `dequeue_signal` takes them. With none, the loop ends.
3. A traced thread stops for its tracer, which may replace the signal or cancel it (`ptrace_signal_stop`).
4. An ignored signal is dropped. A default action is the one `ksig_default_action` gives, which [sigaction(2)](../../../man/man2/sigaction.2) lists: to ignore; to stop the process (`sig_group_stop`), after which the loop starts again at step 1; or to end it, with a core file for the core signals (`terminate_current_process`).
5. A caught signal gets a frame (`sigframe_setup`), and `deliver_signals` returns. One handler is entered on each return to user mode; another pending signal is taken at the next return, which may come during the handler.

For a handler, `deliver_signals` adds the action's mask to the thread's, and the signal itself unless `SA_NODEFER` is set, and with `SA_RESETHAND` puts the disposition back to the default for the next signal. The frame records the mask to restore: the thread's own, or the one a wait replaced. `SA_NOCLDSTOP` and `SA_NOCLDWAIT` are stored and do nothing; the other flags act as the sections below describe.

A stop (`sig_group_stop`) sets the group's `stop`, records `(signo << 8) | 0x7f` in the leader's `jc_status` for `wait4(WUNTRACED)`, and sends the parent `SIGCHLD` with `CLD_STOPPED`. The other threads park as each next returns to user mode. `jc_status` is one slot, so a stop and a continue the parent has not yet collected replace each other's report. A stop aimed at init is dropped, since a stopped init would leave orphans with no reaper, unless the debug shell is the supervisor, as in a rescue boot and on the test image.

A default that ends the process calls `terminate_current_process` (`kernel.c`), which runs the teardown as a system call runs, with interrupts on and `in_syscall` set, since the core file and the release of the files may sleep on a disk that answers only by interrupt. It sends the other threads `SIGKILL` (`proc_group_exit`), writes the core file ([the process subsystem's Readme](Readme.md) describes how), and the last thread to end publishes the process's end with the signal in its wait status. An init that a signal ends panics the kernel (`panic_init_died`), unless the debug shell is the supervisor.

### Restarting a system call

When a system call returns `EINTR`, the dispatcher (`_syscall_handler` in `syscall.c`) records it in `restart_nr`, unless it is one that Linux never restarts after a handler (`syscall_restartable`): `pause`, `rt_sigsuspend`, `rt_sigtimedwait`, `nanosleep`, `clock_nanosleep`, `select`, `_newselect`, `pselect6`, `poll`, `ppoll`, `epoll_wait`, `epoll_pwait`, `epoll_pwait2`, a `futex` wait with a timeout (`futex_restartable`), and the two returns from a handler, whose result register is the interrupted call's. The test compares handlers, not numbers, so one list serves both architectures. `deliver_signals` restarts the call (`restart_syscall`) when no handler runs, as after a stop and a continue, and when the handler that runs has `SA_RESTART`: it puts the call's number back in EAX or RAX and moves the instruction pointer back over `int $0x80` or `syscall`, two bytes either way (`SYSCALL_INSN_LEN`). Otherwise the call returns `EINTR`. A tracer can cancel the restart ([ptrace](ptrace.Readme.md)).

`rt_sigsuspend`, and `ppoll`, `pselect6`, `epoll_pwait` and `epoll_pwait2` through `poll_mask_set` (`io/poll.c`), install a mask for their wait and keep the old one in `saved_sigmask`. When a signal ends the wait, the handler's frame records the old mask, so the handler runs under the wait's mask and its return restores the old one; when no handler runs, `deliver_signals` puts the old mask back at once. A wait that ends for another reason puts it back itself.

### The frames

`sigframe_setup` in each architecture's `sigframe.c` builds the frame Linux builds, below the user stack pointer, or at the top of the alternate stack for an `SA_ONSTACK` action when one is set and the thread is not on it already. The floating-point state goes above the frame, placed and written by `fpu_frame_place` and `fpu_frame_put` (`arch/x86/fpu.c`) from the thread's own area, which `fpu_save` fills first; [sigaction(2)](../../../man/man2/sigaction.2) gives its layout with and without XSAVE. A frame that cannot be written, or that would run past the bottom of the alternate stack, ends the process with `SIGSEGV`. A new program's auxiliary vector gives `AT_MINSIGSTKSZ`, the most bytes a frame takes (`sigframe_stack_bytes`).

| | i686 | x86-64 |
| --- | --- | --- |
| Frame of a handler without `SA_SIGINFO` | `struct sigframe` | `struct rt_sigframe` |
| Frame of a handler with `SA_SIGINFO` | `struct rt_sigframe` | `struct rt_sigframe` |
| The handler returns to | `sa_restorer` with `SA_RESTORER`, else the frame's `retcode` | `sa_restorer`; without `SA_RESTORER` the process ends with `SIGSEGV`, as on Linux |
| Left free below the stack pointer | nothing | the 128-byte red zone |
| Alignment at the handler's entry | `ESP + 4` a multiple of 16 | `RSP + 8` a multiple of 16 |
| Signal number, siginfo, ucontext | EAX, and for an rt frame EDX and ECX | RDI, RSI, RDX, with RAX 0 |

On i686 (`arch/i686/sigframe.c`), `struct sigframe` has the return address, the signal number, the `struct sigcontext`, an unused floating-point area of 624 bytes, kept so that the next field is where old programs read it, `extramask`, signals 33 to 64 of the mask to restore, and an 8-byte `retcode`, `popl %eax; movl $119, %eax; int $0x80`. `struct rt_sigframe` has the return address, the signal number, the addresses of the siginfo and the ucontext, the 128-byte `siginfo_t`, the `ucontext_t` (flags, link, alternate stack, `sigcontext` and the 8-byte mask), and an 8-byte `retcode`, `movl $173, %eax; int $0x80`. Debuggers recognize a signal frame by these two return codes.

On x86-64 (`arch/x86_64/sigframe.c`), `struct rt_sigframe` has the return address, then the `ucontext_t` (flags, link, alternate stack, the 256-byte `sigcontext` and the mask), then the 128-byte `siginfo_t`. The ucontext's flags have `UC_SIGCONTEXT_SS` and `UC_STRICT_RESTORE_SS`, as Linux sets them.

On both, the `sigcontext` has the interrupted registers and selectors, the trap number, error code and fault address from the thread's record, the floating-point state's address and the mask's first word; the ucontext's flags have `UC_FP_XSTATE` when the state is an XSAVE image; the frame's alternate stack has `SS_ONSTACK` only when the signal came while the thread was on that stack; and the handler starts with the direction, trap and resume flags clear and the initial floating-point state (`fpu_reset`).

The siginfo (`sigframe_fill_siginfo`) has the signal, `si_errno` and the code, then the fields of its kind: the address for `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE` and `SIGTRAP` with a code above 0; the call's address, number and architecture for a seccomp `SIGSYS`; and otherwise the sender's or child's process ID and user ID, and a child's status or the value of a timer or a queued signal. On i686 those fields begin at byte 12; on x86-64 they begin at byte 16, after the padding that aligns the union, and a value is eight bytes.

### The return from a handler

The restorer makes `sigreturn` for a plain frame on i686, and `rt_sigreturn` for an rt frame. `signal_return` (`syscall_proc.c`) calls `sigframe_restore`, sets the mask it read through `signal_set_blocked`, and returns the restored result register, so that the dispatcher's store of the result leaves it as it was. The frame is found from the stack pointer, which the handler's `ret` moved past the return address: on i686 the frame begins 4 bytes below it for `rt_sigreturn`, and 8 bytes below for `sigreturn`, whose restorer also pops the signal number; on x86-64 it begins 8 bytes below.

`sigframe_restore` takes back the general registers, the instruction and stack pointers, and of the flags only those a program may change, the arithmetic flags and the direction, trap, resume and alignment-check flags (`FIX_EFLAGS`). It loads the floating-point state through `fpu_frame_load`, which reads it into an area from the heap and loads it from there, since the copy from the user stack may sleep for a page and the switch away would save the registers over the thread's own area; a frame whose state address is 0 gets the initial state. From an rt frame it also takes back the alternate stack, as `sigaltstack` would take it, unless the thread is on that stack or the size is under 2048 bytes.

The code and stack selectors are always the user's. On i686 a data selector stays only when it is the user data selector, 0x23, or the TLS selector, 0x33, and becomes 0x23 otherwise, since the return loads it in ring 0, where any other would fault the kernel ([thread-local storage](../arch/tls.Readme.md)). On x86-64 an instruction pointer outside the lower half is refused, since `iretq` would fault on it. A frame that cannot be read, a state that `xrstor` or `fxrstor` would fault on, or a return for which the heap has no area ends the process with `SIGSEGV`; `fpu_frame_take` (`arch/x86/fpu.c`) makes the checks, and [sigaction(2)](../../../man/man2/sigaction.2) lists them.

### What the kernel sends

* `SIGCHLD` to the parent at a child's end (`proc_exit_cleanup` in `proc/exit.c`), with `proc_child_info`'s siginfo: `CLD_EXITED` and the exit code, or `CLD_KILLED` or `CLD_DUMPED` and the signal, with the child's process ID and real user ID translated into the parent's namespaces; at its stop, with `CLD_STOPPED`; and at its continue, with `CLD_CONTINUED`. Init gets one when it adopts a child that has already ended (`reparent_children` in `proc/process.c`), and a tracer one with `CLD_TRAPPED` for each ptrace stop.
* The signal a process set with `PR_SET_PDEATHSIG`, when its parent ends, once init has adopted it (`reparent_children`).
* `SIGALRM` from the timer tick when the leader's real-time timer is due (`proc_wake_sleepers`), and `SIGVTALRM` and `SIGPROF` when the process's processor-time timers run out (`sched_account_one` in `proc/sched.c`), each to the process; [setitimer(2)](../../../man/man2/setitimer.2) gives the rules.
* A POSIX timer's signal with `SI_TIMER` ([the I/O Readme](../io/Readme.md)), and a message queue's notification with `SI_MESGQ` (`ipc/mqueue.c`).
* `SIGPIPE` to the writing thread, with `SI_USER` from its own process, for a write to a pipe or FIFO with no reader (`ipc/pipe.c`) and a send on an `AF_UNIX` socket whose peer is gone, unless the flags have `MSG_NOSIGNAL` (`ipc/unix.c`). A TCP send posts none ([send(2)](../../../man/man2/send.2)).
* `SIGXFSZ` to the writing thread for a write or a truncation past its `RLIMIT_FSIZE` (`fsize_refused` in `syscall_fs.c`).
* `SIGINT`, `SIGQUIT` and `SIGTSTP` to the foreground process group for the terminal's interrupt, quit and suspend characters, `SIGHUP` when a pseudo-terminal's master closes, and `SIGWINCH` when its window size is set ([the terminal Readme](../tty/Readme.md)).
* `SIGTTIN` to the process group of a background process that reads its controlling terminal, and `SIGTTOU` to that of one that writes it while `TOSTOP` is set. The read or write returns `EINTR` and is restarted, as above, once the group is continued; when the caller blocks or ignores the signal, the call fails with `EIO` instead (`syscall_fs.c` for the console, `tty/pty.c` for a pseudo-terminal's slave).
* `SIGKILL` to the other threads of a process that `exit_group` or a fatal signal ends (`proc_group_exit`) or one of whose threads calls `execve` (`exec_one_thread`), and to every process of a PID namespace whose init ends (`proc/ns.c`).
* `SIGSYS` for a call a seccomp filter traps; `SIGTRAP` at the `execve` of a thread a tracer attached without `PTRACE_SEIZE` and without the exec option; and `SIGSTOP` to a thread `PTRACE_ATTACH` attaches.

## Limits

* A signal has one pending instance in each set. One sent again while it is pending merges with it and its siginfo is lost; real-time signals are not queued, so values `sigqueue` sends faster than they are taken are lost.
* `kill(-1)`, a signal to every process, answers `ESRCH`.
* `SA_NOCLDSTOP` and `SA_NOCLDWAIT` have no effect, and ignoring `SIGCHLD` reaps nothing: a parent always collects its children's ends with a wait.
* `SS_AUTODISARM` is accepted and has no effect.
* A stop does not wake the process's other threads: a thread asleep in the kernel goes on sleeping, and parks when its call returns.
* Init is protected from a stop alone: a signal whose default action ends a process ends init too, and the kernel then panics.
* i686 registers none of i386's older `signal`, `sigaction`, `sigsuspend` and `sigpending` calls, and no `rt_sigtimedwait_time64`; each answers `ENOSYS`. x86-64 has no `sigreturn` or `sigprocmask`, as on Linux.

## Tests

`tests/run default` runs every guest probe in one boot of the test image ([the tests' Readme](../../../tests/Readme.md)). Among them:

* `tests/sigprobe.c`: a handler and its return through `rt_sigreturn`, a blocked signal delivered once unblocked, a default termination, a handler that writes invalid selectors into its own frame, `SA_RESETHAND`, a sleep a signal ends where it lands (judged by `sleeps_interrupted` in `/proc/stat`, not by time), and the real-time interval timer.
* `tests/trapprobe.c`: an invalid opcode and a divide error end only the faulting child, with `SIGILL` and `SIGFPE`.
* `tests/ownprobe.c`: the permission rule, the saved user ID, the session's `SIGCONT`, and the stricter signal 0.
* `tests/waitprobe.c` and `tests/pipeprobe.c`: a socket receive a signal interrupts, and `SIGPIPE` with `EPIPE`.
* `tests/jobprobe.c`: group signals, a stop, a continue and a kill, and the background read's `SIGTTIN`.
* `tests/xsaveprobe.c`: the frame's XSAVE image, a handler's change to it, and a damaged XSAVE header, which must end the process with `SIGSEGV`.
* `tests/musl/sigmusl.c`: both restorers through musl, the mask, `abort`, and the context an `SA_SIGINFO` handler receives.
* `tests/musl/sigframemusl.c`: a fault handler that returns to its instruction, `SEGV_MAPERR`, `SA_RESTART` and its absence, `SA_ONSTACK`, `int3`, `rt_sigpending`, the old `sigprocmask`, `sigsuspend`'s mask, `SIGCHLD`'s siginfo, and a nested handler that runs off a small alternate stack.
* `tests/musl/threadmusl.c`: `pthread_kill`, a signal to the process that waits while every thread blocks it, and a second signal another thread takes while the first one's handler runs.
* `tests/musl/callsmusl.c`: `sigtimedwait`, `sigqueue`, `rt_sigqueueinfo`, a POSIX timer's overrun, and a signalfd.
* `tests/musl/waitidmusl.c`, the siginfo of a child's exit, death, stop and continue; `tests/musl/prctlmusl.c`, the parent-death signal; `tests/musl/seccompmusl.c`, `SIGSYS`; and `tests/musl/pwauthmusl.c`, signals above 34 and musl's refusal of 32 to 34.

Other suites, each with a line in [the suite inventory](../../../tests/system/suites.md):

* `tests/run fault` requires `killed by signal 11` from three probes that touch memory they may not.
* `tests/run conload` requires the interrupt character typed at the console to reach a foreground child under load, also behind 200 lines no one has read.
* `tests/run host` ([the host tests](../../../tests/host/Readme.md)) runs `fpuframe_hosttest.c` and `fpuframe64_hosttest.c`, the frame's floating-point state and a return's checks on each architecture; `process_timer_hosttest.c`, `SIGALRM` from the tick; and `syscall_hosttest.c`, the sleeps and the interval timer, a signal posted as a sleeper blocks among them.

`sh tests/system/sigcontext_mutate.sh` proves `sigmusl` against a kernel whose `sigframe.c` does not fill `uc_mcontext`, `sh tests/system/xsave_mutate.sh` proves `xsaveprobe` and the frame host tests against eight faults in `arch/x86/fpu.c`, and `sh tests/system/itimer_mutate.sh` proves `sigprobe`'s timer rows. `tests/musl/ctrlcmusl.c`, a program that Ctrl-C must end, is staged for a person to run, and no suite runs it.

Every suite in `tests/registry` runs on both architectures unless its row says otherwise; `ARCH=x86_64 tests/run NAME` runs it on x86-64.
