# Rach2 Signals

A small but real signal subsystem: per-process dispositions and masks, delivery on the way back to user mode through a frame pushed on the user stack, and the `sigreturn` trampoline that unwinds it. It is enough for the things a libc, a shell, and `abort()` actually need (`SIGSEGV`/`SIGFPE` from a fault, `SIGINT` from Ctrl-C, `SIGCHLD` on child exit, `SIGTERM`/`SIGABRT` teardown), and it is the path musl's `sigaction`, `raise`, and `abort` ride on. Standard signals only (1..64), including the job-control stops; real-time signal queueing is not implemented.

## Pieces

| File                | Responsibility                                                                                                                                                                      |
|---------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `lib/ksignal.h`     | Linux i386 signal numbers, the kernel-side `struct k_sigaction`, `SA_*` flags, and the default-action classes (term, ignore, stop)                                                  |
| `sys/syscall.c`     | The whole mechanism: `rt_sigaction`/`rt_sigprocmask`/`rt_sigreturn`/`sigreturn`, `kill`/`tkill`/`tgkill`, `signal_post`, `signal_pending`, the frame builder, and `deliver_signals` |
| `sys/isr_handler.s` | Calls `deliver_signals` from the ISR and IRQ return epilogues, just before `iret`, so a pending signal is acted on whenever the CPU returns to ring 3                               |
| `proc/process.h`    | The per-PCB state: `sigacts[1..64]`, and the 64-bit `sig_pending` and `sig_blocked` masks                                                                                           |

## The model

Each process keeps a disposition per signal (`sigacts[signo]`: a handler address, or the sentinels `KSIG_DFL`/`KSIG_IGN`) plus two 64-bit masks: `sig_pending` (bit `signo-1` set means the signal is raised but not yet acted on) and `sig_blocked` (temporarily held off). `kill(2)` and its thread-directed cousins `tkill`/`tgkill` all reduce to "set the pending bit on the target and wake it", since Rach2 is one thread per process. `SIGKILL` and `SIGSTOP` can never be caught, blocked, or ignored.

Who may signal whom is judged per target and per signal. A privileged sender passes; `SIGCONT` passes within one session, whoever owns the target; otherwise either of the sender's ids, real or effective, must match either of the target's, real or saved. The saved-uid arm is what lets the identity that launched a set-id program still signal it. The session exception is what makes job control work at all, and its absence was a live defect: `/bin/su` is `04755` and calls `setuid`, so the shell it execs runs with real uid 0, while ash implements `fg` as `killpg(pgid, SIGCONT)` and ignores the result, so a uid-1000 shell could not resume a job it had started through `su` and would wait forever on a process that never ran.

The exception cannot extend a sender's reach, because `setsid` only ever creates a session and never joins one, so nothing can move into a session to get at what is there. What it does open is worth naming rather than leaving implied: a member of a session may make a root process in that same session run its `SIGCONT` handler when the member chooses. The signal-0 existence probe keeps the strict rule, since the exception is for resuming a stopped job and not for discovering what is running. `tty_signal_group` is unchecked because the terminal raises those signals, not a process.

## Delivery on return-to-user

Signals are not delivered when posted; they are delivered when the target next returns to user mode. `deliver_signals(trapframe)` runs from the ISR and IRQ return path (gated on the saved `CS` being ring 3) and picks the lowest-numbered deliverable (pending, unblocked) signal, handling two cases.

* The default action terminates (the common case, such as an unhandled `SIGSEGV` or `SIGINT`), ignores (`SIGCHLD`, `SIGWINCH`, and the rest), or stops: `SIGSTOP`, `SIGTSTP`, `SIGTTIN`, and `SIGTTOU` park the process as `PROC_STOPPED`, which `wait4(WUNTRACED)` reports, and `SIGCONT` resumes it. The one exception is the real init (PID 1): a stop aimed at it is dropped, since freezing the system's only reaper would wedge it with no way back.
* A caught handler runs through `signal_setup_frame`, which pushes a `struct sig_frame` on the user stack (return address is the libc restorer, then the signal number, the `siginfo` and `ucontext` argument pointers, the saved trapframe, the pre-handler mask, and a magic tag), blocks the handler's mask for the run, and redirects the trapframe to the handler. The handler runs in user mode like a normal call.

When the handler returns it lands on the libc restorer, which invokes `sigreturn`. musl picks the restorer by `SA_SIGINFO`: a `siginfo` handler returns through `__restore_rt` to `rt_sigreturn(173)`, a plain handler through `__restore` to the legacy `sigreturn(119)` (which pops the signal word first). Both read the same kernel-built frame (`signal_restore(tf, delta)` handles the 4-byte difference), validate the magic, and restore the saved registers, instruction and stack pointers, and mask. The restore forces `CS`, `SS`, and `EFLAGS` to safe ring-3 values and coerces the data and TLS selectors to ring 3 (so `%gs` keeps its per-process TLS selector but a forged frame cannot smuggle a kernel selector into `iret`): the frame lives on the untrusted user stack, so it is sanitized, not trusted.

This is the same shape Linux uses: it builds a `rt_sigframe` on the user stack and returns through a libc `__restore_rt`. Only real-time signal queueing and a faithful `ucontext` layout are left out.

## Ctrl-C

The PS/2 keyboard IRQ and the serial console both turn Ctrl-C into `signal_post(foreground, SIGINT)`; with the default disposition this terminates the foreground program, and a program that installs a `SIGINT` handler catches it instead. Over the polled serial console the ETX byte (`0x03`) is detected in the `sys_read` stdin path and unwinds a blocking read with `-EINTR`.

## Probe

`tests/sigprobe.c` (`SIGPROBE OK`) is hermetic: it installs a handler with a tiny `rt_sigreturn` restorer and confirms it runs and resumes, blocks a signal and confirms it stays pending until unblocked in order, and forks a child that the default action terminates (the parent reaps `WTERMSIG`). The musl integration programs `tests/musl/sigmusl.c` (`sigaction` plus masking plus `abort()` giving `WTERMSIG == SIGABRT`) and `tests/musl/ctrlcmusl.c` (Ctrl-C interrupts a reading program) prove the same path end to end through libc.
