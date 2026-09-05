# Signals

Rach2 keeps signal dispositions, a blocked mask, and a pending mask in each process. It posts signals immediately but acts on them only when the target returns to user mode. The Linux i386 signal numbers 1 through 31 have their usual names; the 64-bit masks also accept numbers 32 through 64. Each pending signal occupies one bit, so repeated instances coalesce. Queued real-time signals and queued `siginfo` values are not implemented.

CPU exceptions take a separate path. A user page fault terminates the process with `SIGSEGV`; the other user exceptions map to `SIGFPE`, `SIGTRAP`, `SIGILL`, `SIGBUS`, or `SIGSEGV`. These paths call `terminate_current_process` without consulting the blocked mask or disposition. A handler for one of those signals therefore runs only when the signal was posted, not when the CPU raised the corresponding exception.

## Process state

`process_t` contains `sigacts[1..64]`, `sig_pending`, and `sig_blocked`. A new process starts with default dispositions and empty masks. `fork` copies the dispositions and blocked mask but gives the child an empty pending set. `execve` preserves ignored dispositions and the blocked mask, resets caught dispositions to default, and clears the pending set.

`SIGKILL` and `SIGSTOP` cannot be caught or ignored, and every mask update removes their bits. Installing `SIG_IGN` for another signal also removes an instance already pending.

The main implementation is in `sys/syscall.c`. Signal numbers, action flags, and the kernel action layout are in `sys/lib/ksignal.h`; the PCB fields are in `sys/proc/process.h`; and both return epilogues in `sys/isr_handler.s` call `deliver_signals` before `iret`.

## Posting and permission

`signal_post` sets the target's pending bit and makes a blocked target ready. Posting has additional job-control effects:

* `SIGCONT` removes pending stop signals and resumes a stopped process. The parent receives `SIGCHLD` and a `wait4(WCONTINUED)` event.
* A stop signal removes a pending `SIGCONT`.
* `SIGKILL` makes a stopped process ready so it can take the terminating action.
* Other signals remain pending while a process is stopped.

`kill` supports a positive PID, PID zero for the caller's process group, and a PID below -1 for the named process group. PID -1 broadcast is not implemented and returns `-ESRCH`. Signal zero checks existence and permission without posting. `tkill` treats its positive thread ID as a PID. `tgkill` requires two positive IDs that both name that same process because Rach2 has one thread per process.

Root, identified by effective uid zero, may signal any live process. Otherwise the sender's real or effective uid must match the target's real or saved uid. `SIGCONT` is also allowed between members of one session, but signal zero does not receive that exception. Terminal-generated signals are posted to the foreground process group without applying a process sender's credentials.

## Dispositions and masks

The signal syscalls are `kill`, `tkill`, `tgkill`, `rt_sigaction`, `rt_sigprocmask`, `pause`, `rt_sigsuspend`, `sigreturn`, and `rt_sigreturn`. The real-time action and mask calls require an 8-byte signal set.

The default actions are:

* Ignore `SIGCHLD`, `SIGURG`, `SIGWINCH`, and `SIGCONT`.
* Stop on `SIGSTOP`, `SIGTSTP`, `SIGTTIN`, and `SIGTTOU`.
* Terminate on every other accepted signal number.

A stop records one status for `wait4(WUNTRACED)` and posts `SIGCHLD` to the parent. A later continue records one `wait4(WCONTINUED)` status. The single status slot means that an unreaped stop or continue event can be replaced by the next transition. The booted `/sbin/init` cannot be stopped; this exception does not apply to the debug shell's transient test process.

An ignored signal, including one whose default action is ignore, does not interrupt a blocking wait. A caught, terminating, or stopping signal makes an interruptible wait leave its loop. The syscall result is `-EINTR` unless an operation that already transferred data returns its partial count; a default stop or termination can act before user code observes that result.

`rt_sigaction` stores all action flags. Delivery implements the installed mask, `SA_NODEFER`, and `SA_RESETHAND`. It always uses the installed restorer address as the handler's return address; there is no kernel-provided fallback. musl sets `SA_RESTORER` and chooses `__restore` for a plain handler or `__restore_rt` for `SA_SIGINFO`. `SA_RESTART` is stored and returned by `sigaction`, but the kernel never restarts an interrupted syscall. `SA_ONSTACK`, `SA_NOCLDSTOP`, and `SA_NOCLDWAIT` have no special behavior.

`rt_sigsuspend` installs a temporary mask, waits for an acting signal, restores the old mask, and returns `-EINTR`. The old mask is restored before signal delivery, so the handler does not run under the temporary mask as it would on a complete POSIX implementation.

## Delivery and return

The interrupt and system-call epilogue and the hardware-IRQ epilogue call `deliver_signals` with interrupts disabled. It does nothing for a kernel-mode return. For a user-mode return it chooses the lowest-numbered pending, unblocked signal and clears that pending bit.

A default disposition ignores, stops, or terminates as described above. A caught disposition builds `struct sig_frame` below the user stack pointer, redirects `EIP` to the handler, and blocks the action's mask plus the delivered signal unless `SA_NODEFER` is set. `SA_RESETHAND` resets the disposition before the handler starts. Only one handler is entered at a time; remaining signals are reconsidered at a later return-to-user boundary.

The frame contains the installed restorer address, the signal number, pointers for the `siginfo_t` and `ucontext_t` arguments, the saved trapframe, the old mask, a magic value, and the 512-byte floating-point image `fxsave` writes, or `fnsave`'s 108 bytes in it under `rach2.nofxsr`, saved by `fpu_save` at delivery; the handler then starts with the initial floating-point state, as a fresh image does. The `siginfo` storage is zero except for `si_signo`, and the `ucontext` storage is zero. These arguments do not contain the complete Linux context or queued sender information.

When the handler returns, musl's plain restorer invokes `sigreturn(119)` after popping the signal argument; its `SA_SIGINFO` restorer invokes `rt_sigreturn(173)` directly. Both restore the same kernel frame. The kernel restores the general registers, instruction pointer, stack pointer, old mask and the floating-point image, but forces ring-3 code and stack selectors, accepts only ring-3 data selectors, and removes privileged `EFLAGS` bits. An unreadable or invalid frame terminates the process with `SIGSEGV`, and so does a floating-point image with a reserved MXCSR bit set, which `fxrstor` would fault on in the kernel.

## Kernel signal sources

Process exit, stop, and continue post `SIGCHLD` to the parent. A write to a pipe with no reader posts `SIGPIPE`. A pseudo-terminal resize posts `SIGWINCH`, and closing its master posts `SIGHUP` to the slave's foreground group.

With terminal `ISIG` enabled, the line discipline maps `VINTR`, `VQUIT`, and `VSUSP` to `SIGINT`, `SIGQUIT`, and `SIGTSTP`. The defaults are Ctrl-C, Ctrl-Backslash, and Ctrl-Z. It discards the current edit line, committed unread input, and a pending EOF before signaling the foreground group. PS/2 keyboard and COM1 input both use this discipline while a user session owns the console.

A foreground read interrupted by a caught signal returns `-EINTR`; a default `SIGINT` terminates before user code observes the return. A background terminal read posts `SIGTTIN`, and a background write with `TOSTOP` posts `SIGTTOU`. If the applicable signal is blocked or ignored, the operation returns `-EIO` instead. The console and pseudo-terminal slave use the same rule.

## Verification

The default `make test` run includes these signal checks:

* `sigprobe` covers a caught signal, blocking and unblocking, `rt_sigreturn`, and default termination.
* `ownprobe` covers uid permission, saved uid permission, the same-session `SIGCONT` exception, and the stricter signal-zero probe.
* `waitprobe` interrupts a blocked socket receive, `pipeprobe` catches `SIGPIPE`, and `jobprobe` covers group signals, stop, continue, kill, and background `SIGTTIN`.
* `musl/pwauthmusl` installs handlers for signals above 34 and for fault-numbered signals, and confirms that musl reserves 32 through 34.

`tests/run host` checks the line discipline's `ISIG` recognition and input flush under AddressSanitizer and UndefinedBehaviorSanitizer. The test image also stages `musl/sigmusl` and `musl/ctrlcmusl` for manual runs. The first covers musl's `sigaction`, mask, restorer, `raise`, and `abort` paths. The second blocks in `read`; sending Ctrl-C must terminate it with `SIGINT`, and any return from `read` is failure. See the [guest test inventory](../../../tests/Readme.md) for the manual procedure.
