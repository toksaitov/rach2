# Processes

The process subsystem, `rach2/sys/proc/` with the handlers in `rach2/sys/syscall_proc.c`, runs programs as processes. It keeps a record for each thread and a thread group that makes a process of several; makes processes and threads with `fork` and `clone`, loads programs with `execve`, and ends them with `exit` and reports their end to `wait`; schedules them on every processor under one big lock over the kernel; and keeps their credentials, resource limits and seccomp filters, writes their core files, and keeps them from running during a suspend to idle. A program sees Linux's process calls for its architecture, whose contracts are in section 2 of the manual. Four parts of this directory have documents of their own: [signals](signal.Readme.md), [ptrace](ptrace.Readme.md), [futexes](futex.Readme.md) and [namespaces](ns.Readme.md). The context switch and the entry to the kernel are [the architectures'](../arch/Readme.md), the thread pointer is [thread-local storage](../arch/tls.Readme.md), and the address space a process runs in is [memory's](../mm/Readme.md).

## Interfaces

### The files

A file named without a directory is in `rach2/sys/proc/`; other paths are from the top of the tree.

| File | What it has |
| --- | --- |
| `process.h`, `process.c` | The thread's record, `struct process`, the list of records, process ids, the reparenting of children |
| `tgroup.h` | The thread group: what the threads of one process share |
| `percpu.h` | Each processor's record, `struct percpu`, and `current_proc` |
| `smp.h`, `smp.c` | The big lock, the requests one processor leaves another, the start of the other processors |
| `sched.h`, `sched.c` | The run queues, `schedule`, preemption, the idle wait, CPU time and the load averages |
| `wait_queue.h`, `wait_queue.c` | Wait queues |
| `exit.c` | The end of a thread and of a process, `proc_exit_cleanup` |
| `elf.h`, `elf.c` | ELF validation and the segment loader |
| `uimage.h`, `uimage.c` | The initial stack of a new image |
| `auxv.h` | The auxiliary vector's types |
| `seccomp.h`, `seccomp.c` | seccomp filters |
| `coredump.h`, `coredump.c` | Core files |
| `suspend.h`, `suspend.c` | Suspend to idle |
| `futex.h`, `futex.c` | [Futexes](futex.Readme.md) |
| `ns.h`, `ns.c` | [Namespaces](ns.Readme.md) |
| `ptrace.h`, `ptrace.c` | [ptrace](ptrace.Readme.md) |
| `sigframe.h` | The signal frames each architecture builds ([signals](signal.Readme.md)) |
| `rach2/sys/syscall_proc.c` | The handlers of the calls below, and of the signal and address space calls |
| `rach2/kernel.c` | Process 0, the first program, the timer's tick, and the end of a process by a signal |

### The system calls

The calls this document covers, each with the page that has its contract. [syscalls(2)](../../../man/man2/syscalls.2) gives the numbers on both architectures.

| Calls | Manual page |
| --- | --- |
| `fork`, `vfork`, `clone`, `clone3` | [fork(2)](../../../man/man2/fork.2) |
| `execve` | [execve(2)](../../../man/man2/execve.2) |
| `exit`, `exit_group` | [exit(2)](../../../man/man2/exit.2) |
| `wait4`, `waitid`, and `waitpid` on i686 | [wait(2)](../../../man/man2/wait.2) |
| `getpid`, `gettid`, `getppid`, `getpgid`, `setpgid`, `getpgrp`, `getsid`, `setsid` | [getpid(2)](../../../man/man2/getpid.2) |
| `getuid`, `geteuid`, `getgid`, `getegid`, `getresuid`, `getresgid`, `getgroups`, as their `32` forms on i686 | [getuid(2)](../../../man/man2/getuid.2) |
| `setuid`, `setgid`, `setreuid`, `setregid`, `setresuid`, `setresgid`, `setgroups`, as their `32` forms on i686 | [setuid(2)](../../../man/man2/setuid.2) |
| `capget`, `capset` | [capget(2)](../../../man/man2/capget.2) |
| `prlimit64`, `getrlimit`, `setrlimit`, and `ugetrlimit` on i686 | [prlimit64(2)](../../../man/man2/prlimit64.2) |
| `getrusage` | [getrusage(2)](../../../man/man2/getrusage.2) |
| `sched_yield`, `sched_setscheduler`, `sched_getscheduler`, `sched_setparam`, `sched_getparam`, `sched_get_priority_max`, `sched_get_priority_min`, `sched_rr_get_interval`, `sched_setaffinity`, `sched_getaffinity`, `getcpu` | [sched_setscheduler(2)](../../../man/man2/sched_setscheduler.2) |
| `getpriority`, `setpriority` | [getpriority(2)](../../../man/man2/getpriority.2) |
| `ioprio_set`, `ioprio_get` | [ioprio_set(2)](../../../man/man2/ioprio_set.2) |
| `prctl`, `personality` | [prctl(2)](../../../man/man2/prctl.2) |
| `seccomp` | [seccomp(2)](../../../man/man2/seccomp.2) |
| `umask` | [umask(2)](../../../man/man2/umask.2) |

A program's resource limits and where they come from are in [limits(7)](../../../man/man7/limits.7), a core file's layout in [core(5)](../../../man/man5/core.5), the files of `/proc/PID` in [proc(5)](../../../man/man5/proc.5), and `/sys/power/state` in [sysfs(5)](../../../man/man5/sysfs.5).

### The kernel's interfaces

The rest of the kernel reaches processes through these headers:

* `process.h`: `current_proc`, the running thread's record (`percpu.h`); `get_process`, a record by its id; `proc_privileged`, whether a thread has privilege over the whole system; `proc_in_syscall`; and the signal functions, `signal_post` and the rest, which [signals](signal.Readme.md) describes.
* `sched.h`: `schedule`, `sched_make_ready`, which every wake goes through, `sched_preempt`, which an interrupt calls on its way out, `sched_service_point` for a long system call, and `idle_wait_irq_off` and `idle_wait_irq_on`, the only idle waits.
* `wait_queue.h`: `wq_wait_event`, `wq_wait_event_nointr`, `wq_wake`, `wq_wake_all`, and `wq_poll_add` and `wq_poll_remove` for `poll` and `epoll`.
* `smp.h`: `bkl_enter` and `bkl_exit`, which the interrupt routers call (`rach2/sys/arch/x86/isr.c`); `smp_tlb_shootdown`, `smp_space_release` and `smp_kernel_mapping_changed`, which the page tables call; `smp_stop_others`, for a panic and a power off; and `smp_start`.
* `seccomp.h`: `seccomp_permit`, which the system call dispatcher calls (`rach2/sys/syscall.c`).
* `coredump.h`: `core_notes` and `core_write`, which `fs_core_file` in `rach2/sys/syscall_fs.c` writes through.
* `suspend.h`: `suspend_to_idle`, which a write of `freeze` or `mem` to `/sys/power/state` calls, and `suspend_wake`, which the clock's alarm and the keyboard call.

## Design

### The records

Each thread has a record, `struct process` (`process.h`), from the kernel heap (`alloc_process`). The records are on one list, `proc_list`, in the order they were made; `get_process` finds one by its id, and the run queues are the same list (below). A record is in one of six states: `PROC_READY`, `PROC_RUNNING`, `PROC_BLOCKED`, `PROC_STOPPED`, `PROC_ZOMBIE`, and `PROC_UNUSED`, which marks a record that `fork` is still building or that a reaper has taken and is freeing, so that no wait, signal or selection takes it. A new record takes the id after the last one given, passing over any id a live record has; after 1,999,999,999 the ids start again from 1. The boot's self-tests take ids before the first program, and `proc_reclaim_boot_pids` then starts the numbering again at 1, so that init is process 1.

A process is a thread group, `struct tgroup` (`tgroup.h`). Each thread is a record of its own, with its id, registers, kernel stack, thread pointer, blocked mask, pending signals and CPU time. The group has what the threads share: the signal dispositions and the signals sent to the process, a stop in effect, the POSIX timers and the interval timers of processor time, the CPU time of the threads that have ended and of the children reaped, the most memory an address space of the process had in use, the OOM score adjustment, and the process's end: `exiting` and the status the process ends with after `exit_group` or a fatal signal, `exec_task` while an `execve` ends the other threads, and `core_dumping` while a core is written. The thread whose id is the process id is the group's `leader`. Every record's `tgid` is that id, which `getpid` reports, and its `pid` is the thread's own, which `gettid` reports. `live` counts the threads that have not ended, and `refs` the records that point at the group.

Three more parts of a process are counted records of their own, which `clone` shares or copies: the address space, `struct mm`, shared under `CLONE_VM` and copied on write otherwise ([memory](../mm/Readme.md)); the descriptor table, shared under `CLONE_FILES` ([the file system layer](../fs/Readme.md)); and `struct fs_struct` (`process.h`), the working directory, the root directory `chroot` set and the file mode creation mask, shared under `CLONE_FS`. A thread that a C library makes shares all three with the rest of its process.

The thread group's `execed` flag records a successful `execve` since the group was made. `exec_commit` sets it after any change of leader, and a new group starts with it clear. `setpgid` refuses a parent's request for a child whose flag is set with `EACCES`; a process may still change its own group ([getpid(2)](../../../man/man2/getpid.2)).

Process 0, `proc_idle`, is the kernel's own. `kernel_main` (`rach2/kernel.c`) makes it on the boot stack with no address space, and the boot and the debug shell run as it. The first program, and each program the debug shell runs, start on the boot stack too, without a fork (`enter_program` in `rach2/kernel.c`). Each processor also has an idle task (`smp_idle_task` in `smp.c`): a record off the list, with id 0 and a guarded stack of its own, which no system call can reach and no reaper frees.

### Fork and clone

`fork`, `vfork`, `clone` and `clone3` all reach `do_fork` (`rach2/sys/syscall_proc.c`) with a `struct fork_req`: the clone flags, the child's stack pointer, and the addresses of the ids the call stores. `clone_with` refuses a flag it does not know with `EINVAL`, and requires `SIGCHLD` as the exit signal of a new process; `CLONE_THREAD` needs `CLONE_SIGHAND`, which needs `CLONE_VM`. `do_fork` works in this order, and a failure at any step returns to the parent with what was built for the child given back:

1. It lets the caches give back memory (`mem_balance`), and refuses with `EAGAIN` a caller whose real user already has `RLIMIT_NPROC` records, each thread and each zombie counted (`fork_nproc`).
2. It takes a record and marks it `PROC_UNUSED` while the child is built, since the building may sleep.
3. It gives the child its address space, the parent's own with one more user under `CLONE_VM` and a copy-on-write copy otherwise (`fork_mm`), and a kernel stack (`kstack_alloc`), whose guard page [the architectures' Readme](../arch/Readme.md) describes.
4. The child's parent is the caller's process, or the caller's parent under `CLONE_THREAD` and `CLONE_PARENT`. `fork_ids` copies the credentials, the limits, the session and process group, the nice value and I/O priority, the persona, `no_new_privs`, the capability flag, the seccomp mode and filters, the OOM score adjustment and the program name, and `ns_fork` gives the child its namespaces and its numbers in its PID namespaces ([namespaces](ns.Readme.md)).
5. The child's thread pointer is the one given with `CLONE_SETTLS`, or the parent's ([thread-local storage](../arch/tls.Readme.md)), and the floating-point registers are saved into the child's area as they are now.
6. `fork_shares` shares or copies the directories and the descriptor table. A copied table refers to the same open file descriptions, each with one more reference. `CLONE_FS` fails with `EAGAIN` while a thread that shares the record is in an `execve` that applies set-id bits (Credentials, below).
7. The child's id is stored where `CLONE_CHILD_SETTID` and `CLONE_PARENT_SETTID` ask, and the address given with `CLONE_CHILD_CLEARTID` is kept for the thread's end ([futexes](futex.Readme.md)).
8. A SIGKILL pending for the caller, an `exit_group` or an `execve` by another of its threads refuses the child with `EINTR`, since a child made then would outlive its process's end. Nothing after this step sleeps.
9. `fork_signals` adds the child to the caller's group under `CLONE_THREAD`, and otherwise gives its new group a copy of the dispositions; either way it has the caller's blocked mask and no pending signal. `arch_fork_frame` lays out its kernel stack so that its first switch returns to user mode with 0 as the call's result.
10. The child takes the parent's processor and affinity and a depth of 1 in the big lock, and `sched_make_ready` puts it on a run queue. A tracer's event is reported ([ptrace](ptrace.Readme.md)). Under `CLONE_VFORK` the parent then waits, in a sleep only SIGKILL ends, until the child calls `execve` or ends (`proc_vfork_wait`).

The parent gets the child's id in the parent's PID namespace.

### execve

`sys_execve` builds the new image beside the old one and replaces the old one only when nothing can fail any more, so every refusal and every failure of the load returns to the caller with its image, its credentials and its descriptors as they were. Each call has a workspace of its own, `struct exec_state`, from the heap, which the caller's record points at (`exec_ws`), since a read of a program on a mounted volume sleeps and another `execve` may run meanwhile. `exec_run` works in this order:

1. It copies the path, joined to the working directory when it is relative, and makes the new address space (`mm_new`).
2. `exec_open` resolves the path from the caller's root, checks the execute right, and opens the program, noting whether the caller may read the file and what its set-id bits would do; [execve(2)](../../../man/man2/execve.2) has what it refuses. The new address space keeps the program's file from then on ([memory](../mm/Readme.md)), and the load reads that file whatever is put at its path meanwhile. A path through `/proc/PID/exe` is replaced by the program's own path (`exec_link_name`).
3. `exec_copy_args` copies the path and the strings of the environment and of argv onto pages taken for the new stack (`uimage.c`, below).
4. A file that is not an ELF image of the architecture and begins with `#!` gives the path of its interpreter (`exec_script`, `exec_shebang`): argv[0] is taken away, the interpreter, its one argument and the script's path go in its place, and the interpreter is opened as the program. An interpreter that is itself a script is refused.
5. `uimage_place` makes room for the pointers and the auxiliary vector below the strings. `exec_ids` chooses the ids the image runs with (Credentials, below), and `exec_build` loads the program, and the interpreter whose path its `PT_INTERP` gives, into the new address space (`elf.c`, below), makes the regions, refuses with `ENOMEM` an image whose regions pass `RLIMIT_AS`, and writes the initial stack, with 16 bytes from the kernel's random generator for `AT_RANDOM`.
6. `exec_files_stage` takes the storage of a descriptor table of the caller's own when the caller shares one, so that the allocation fails while the call can still return.
7. `exec_one_thread` sends every other thread of the process SIGKILL and waits, in a sleep no signal ends, until they have ended. A process already ending, or one in which another thread's `execve` has begun that wait, makes the call return `EAGAIN`, and the caller ends on its way back to user mode.
8. `exec_commit` makes the image the process's and cannot fail. The thread releases its robust futexes ([futexes](futex.Readme.md)), and a thread other than the leader takes the leader's place and id (`exec_take_leader`). The POSIX timers are deleted; a caller that shares its descriptor table gets one of its own, and the close-on-exec descriptors are closed; the new effective ids become the saved ids too; caught signals go back to their default action and the alternate signal stack is cleared; the program name is set from the path. The new address space is loaded and the old one put, a parent waiting in `vfork` is let go, the frame is set at the new entry (`arch_exec_frame`), the thread pointer becomes 0, and the floating-point state is reset.

`elf.c` accepts an image of the architecture's class and machine, `ELF_CLASS` and `ELF_MACHINE` in its `cpu.h` (ELFCLASS32 `EM_386` on i686, ELFCLASS64 `EM_X86_64` on x86-64), little-endian, of type `ET_EXEC` or `ET_DYN` (`elf_valid_exec`). The loader reads the file a piece at a time through a function, `elf_read_fn`, and never has it whole in memory, so a program's size has no bound; `exec_file_read` takes a whole page of a file on a mounted volume from the file page cache when it is there and leaves it there when it is not, so a program started again reads none of its text from the disk. `elf_read_layout` reads at most `ELF_MAX_PHNUM`, 32, program headers, the span of the `PT_LOAD` segments, and the interpreter's path, absolute and at most `ELF_INTERP_MAX`, 256, bytes with its NUL. `elf_load_image_at` checks each `PT_LOAD` before it takes a page: within the user range below the stack, not wrapping, and starting at or after the page where the segment before it ends. It then fills zeroed pages of the new directory with the segment's file bytes, and makes a page read-only where its segment has no `PF_W`, and not executable where it has no `PF_X`; i686 maps every page executable ([the architectures' Readme](../arch/Readme.md)). At most `ELF_MAX_SEGS`, 8, `PT_LOAD` segments are taken, from which `elf_setup_vmas` and `elf_add_vmas` make the regions. A position-independent program is moved to `ELF_ET_DYN_BASE` and a position-independent interpreter to just below `USER_MMAP_BASE` (`exec_build`, `exec_interp`); [memory](../mm/Readme.md) has the layout these constants give on each architecture.

`uimage.c` builds the initial stack in frames taken for the new image while the old one runs, as Linux's `linux_binprm` has its argument pages. The strings are copied from the top down: the program's path, the environment from its last string, and argv from its last, so argv[0] is the lowest. One string may be `UIMAGE_STRING_MAX`, 131072, bytes with its NUL, and the strings with their pointers may take a quarter of the soft `RLIMIT_STACK`, at least 128 KiB and at most 6 MiB (`uimage_budget`), as on Linux. Below the strings `uimage_write` puts the 16 bytes of `AT_RANDOM`, aligned to 16 bytes, then argc, the argv and envp pointers each ended by NULL, and the auxiliary vector of `UIMAGE_AUXV`, 16, entries (`auxv.h`), each a word of a pointer's width; the entry stack pointer is aligned to 16 bytes. `uimage_map` maps the frames into the new directory where the stack is. [execve(2)](../../../man/man2/execve.2) has the vector's entries and the contract.

### Exit and wait

A thread ends by `exit` (`exit_current` in `rach2/sys/syscall_proc.c`), and a process by `exit_group`, which first sends every other thread SIGKILL and records the status the process ends with (`proc_group_exit`). A signal whose default action ends the process reaches `terminate_current_process` (`rach2/kernel.c`), which writes a core file when the action asks for one (Core files, below), sends the other threads SIGKILL the same way, and ends the thread. Both paths run `proc_exit_cleanup` (`exit.c`), in this order:

1. It sets `in_syscall`, so that an interrupt only asks for a switch, and the thread's own part goes: the word given with `CLONE_CHILD_CLEARTID` is cleared and a waiter on it woken, and the thread's robust futexes are released (`proc_thread_gone`, [futexes](futex.Readme.md)).
2. The thread drops its address space and runs on the kernel's page directory; the last user's drop frees the memory ([memory](../mm/Readme.md)).
3. It drops its descriptor table and its directories; the last reference closes the descriptors and lets go of the working directory and the root. Both may sleep, so they come before the zombie is published. A parent waiting in `vfork` is let go.
4. With the process's last thread, the POSIX timers and the System V semaphore adjustments go, the threads the process traces are let go, the init of a PID namespace sends the rest of its namespace SIGKILL ([namespaces](ns.Readme.md)), the children are given to init (`reparent_children`), and the leader takes the group's status when `exit_group` or a fatal signal ended the process.
5. The record becomes `PROC_ZOMBIE`. A thread that is not the last wakes an `execve` or a core dump that waits for it. The last sends the parent SIGCHLD and wakes its wait, unless a process other than the parent traces the leader: that tracer's wait reports the end first and then lets the parent have it ([ptrace](ptrace.Readme.md)).

The thread then leaves the processor for good. A thread that is not the leader and has no tracer is freed at the next return to user mode of any thread (`reap_threads`; `proc_dead_threads` counts them). The leader's record stays until the parent reaps the process. A process that ends gives up the framebuffer if it has it, and one that leads its session gives up the console and the pseudo-terminals the session controls.

`reparent_children` (`process.c`) gives each child of an ending process to init, or to the init of the child's PID namespace, and sends init SIGCHLD for a child that is already a zombie, so that init reaps it; it also sends each living child the parent-death signal `prctl` set. When init itself ends, a real `/sbin/init` panics the kernel (`panic_init_died`); under the debug shell, which runs each program it starts as init, the kernel ends and reaps every other user process (`proc_end_others`) and returns to the shell ([the kernel's Readme](../../Readme.md)).

`wait4`, `waitid` and i686's `waitpid` share `wait_find`. It walks the records for a child the call selects that has an event the call asks for, an end, a stop or a continue, and for a thread the caller traces; with none it sleeps on the group's `chld` queue. The event is reserved for the caller while the report is copied out (`wait_hold`): a zombie to reap is marked `PROC_UNUSED`, so that no other thread's wait takes it, and a copy that fails because another thread unmapped the buffer meanwhile returns `EFAULT` and puts the event back (`wait_done`). The reap adds the child's CPU time, with that of the children it reaped, to the caller's group, and frees the child's address space, kernel stack and record (`proc_release`).

### Scheduling

`sched.c` runs every ready thread in turn on each processor, with one policy. A record's `cpu` is the number of the processor whose run queue it is on, so the run queues are the process list read through that field: a record is on one queue by construction, and the list's order is the order of selection. A spinlock, `sched_lock`, covers the fields of the queues and is taken after the big lock.

`schedule` chooses the next thread for the processor it runs on, alternating two kinds of selection. An ordinary selection takes the next ready record of the processor's queue after its cursor, and moves the cursor there. A wake preference takes a record a wait queue woke (`woken`), once, without moving the cursor, so that a reader runs soon after its data arrives. At least every second selection is an ordinary one, so a thread that sleeps and wakes often still waits its turn. When its own queue has nothing to run, a processor goes on with its current thread if that one can still run; otherwise it takes the ready record of the busiest other queue that may run there (`sched_steal`), and with nothing there either it switches to its idle task. A record may run on a processor its affinity has, a 64-bit mask that `sched_setaffinity` sets, and only once no other processor is running it. `sched_make_ready`, through which every wake goes, puts a record on the queue of the processor it last ran on, unless that one is busy and another it may use is idle, and sends an idle processor an interprocessor interrupt so that it looks at once.

Before the switch `schedule` saves the floating-point registers into the outgoing record's area and loads the incoming record's, gives the outgoing record the processor's depth in the big lock and the processor the incoming record's, and makes the incoming record the processor's `current`. `switch_context` then changes the stack, the page directory and the thread pointer ([the architectures' Readme](../arch/Readme.md)). Interrupts are off from the selection to the switch, and the big lock stays held across it, so that no other processor sees the outgoing thread before its registers are saved.

Each processor's timer interrupts it 100 times a second (`TICK_HZ` in `rach2/sys/arch/x86/platform.c`; [the PC platform](../arch/x86/Readme.md)), and the tick's handler (`timer_handler` in `rach2/kernel.c`) and every device's interrupt end with `sched_preempt`. A thread interrupted in user mode, process 0 and a kernel thread are switched at once. A thread inside a system call is not, since the call's state may be half changed: the request is kept in the record (`need_resched`), and the dispatcher takes it at the call's exit (`sched_take_preempt`), so a process that is always inside a call still gives way once per call. A call that loops over a large buffer takes the switch between two pieces of its loop through `sched_service_point`. A processor's idle task is not switched from the interrupt either; its loop selects as soon as the handler returns.

An idle wait, a halt with interrupts on that expects to resume, goes through `idle_wait_irq_off` or `idle_wait_irq_on`. Each marks the processor idle for the tick to see, lets the big lock go for the halt and takes it again after. The first enters with interrupts off and returns with them off. An `sti` takes effect only after the instruction that follows it, so an interrupt that became pending while the caller tested its condition arrives once the `hlt` has begun, and ends it. `tests/run style` refuses a `sti; hlt`, or a call of the architecture's halt, anywhere else (`tools/verify/check.sh`).

The tick charges the thread it interrupted with user time when the interrupted code was ring 3's and with system time otherwise, and a tick that found the processor in an idle wait to no one (`sched_account_tick`); a processor counts the global ticks it missed with its next. The same tick runs the process's `ITIMER_VIRTUAL` and `ITIMER_PROF` timers and the POSIX timers of processor time. Every 501 ticks the boot processor folds the threads that are running or ready into the three load averages, as Linux's `calc_load` does (`sched_load_tick`). `times`, `getrusage`, `wait4`, `/proc/stat` and `/proc/PID/stat` report these counts.

Each record keeps a nice value, from -20 to 19, and an I/O priority, which a fork passes on and an exec keeps; the scheduler reads neither. The scheduling calls report `SCHED_OTHER` with priority 0 and refuse every other policy.

### Wait queues

A wait queue, `wait_queue_t` (`wait_queue.h`), is the list of the threads that sleep until one condition is true: data in a pipe, a datagram on a socket, a line from a terminal, a disk's answer. A sleeper's entry is the `struct wq_waiter` in its own record, and `poll` brings one for each object it watches, from an array it took before it began, so entering a queue allocates nothing and no queue has a size.

`wq_wait_event` tests the condition, and a pending signal, with interrupts off; puts the thread on the queue, marks it `PROC_BLOCKED` and calls `schedule`; and tests again when it runs. A wake therefore either came before the test, which then saw the condition true, or finds the thread on the queue. A thread still blocked when `schedule` returns, because the processor had nothing else to run, waits in `idle_wait_irq_off`. A pending signal ends the wait with `EINTR`; a signal does not end `wq_wait_event_nointr`, the wait for a lock another thread has or for a transfer a device has in flight for the caller. A deadline is `wakeup_tick` in the record, which the tick's `proc_wake_sleepers` (`process.c`) acts on. `wq_wake` empties the queue and makes each sleeper ready with one wake preference; each sleeper tests its own condition again, so a wake that was not for it costs one test. A wake also counts what kind of change it was, data, room or a hangup, which an edge-triggered `epoll` reads ([readiness](../io/Readme.md)). Each list is also under the wait queues' spinlock.

### Processors and the big lock

The kernel runs on each enabled processor the firmware lists, up to `NR_CPUS`, 64 (`percpu.h`), or as many as `maxcpus=N` on the command line allows; [the PC platform](../arch/x86/Readme.md) finds and starts them. `smp_start` (`smp.c`) makes the boot processor's idle task, then for each other processor makes its idle task, has the platform start it, and waits up to 100 ms for it to come online. The boot and the processor each decide in one compare-and-exchange of the processor's `admit` word whether it is counted or refused, so a processor that comes as the wait ends is either counted or stopped, never running uncounted. The log says `smp: processors: N online of M`.

Each processor has a `struct percpu` in `cpu_table` (`percpu.h`): the thread it runs, its idle task, the page directory it translates through, its depth in the big lock, its tick counts, and the requests other processors leave it. `this_cpu()` is the running processor's entry, which `arch_cpu_id()` indexes, and `current_proc` is its `current`. A processor changes its own fields; another reads them only while it has the big lock, and writes only the requests.

One lock covers the kernel. Every entry to it, a system call, a fault or an interrupt, takes the big lock (`bkl_enter`), again when the processor has it already, and its return gives one level back (`bkl_exit`). A return to user mode and an idle wait let it go whatever the depth (`bkl_drop`) and take it again at the same depth (`bkl_take`). So user code runs on every processor at once, and kernel code on one at a time. The lock is a word set by an atomic exchange, and its holder is the processor whose depth is above 0. A processor that waits for it answers the requests left for it meanwhile, since the holder may be waiting for that answer. The interprocessor interrupt, the NMI, the machine check and the double fault are taken without it ([the architectures' Readme](../arch/Readme.md)). The parts of the kernel that every processor enters on its ticks, faults and allocations, the run queues, the wait queues, the frame allocator, the heap, the timers and the console, also have spinlocks of their own, taken after the big lock and never before it (`rach2/sys/lib/spinlock.h`).

A request is a word in the target's record and an interprocessor interrupt that has it look (`smp_service`):

* A shootdown (`smp_tlb_shootdown`): after a user page's entry changes, each other processor that translates through that page directory drops the page's translation, or every user translation, and the sender waits until each has cleared its word. A processor whose thread has no address space, an idle task or a kernel thread that kept the directory loaded, is not interrupted; the directory is marked stale there and loaded again at the switch to one of its threads.
* A release (`smp_space_release`): before a directory that no thread uses is freed, each processor that still translates through it loads the kernel's.
* A change of a kernel mapping (`smp_kernel_mapping_changed`) increments a count, and a processor whose count is behind drops all its translations when it next takes the big lock.
* A stop (`smp_stop_others`), for a panic and a power off, is final. The sender waits up to a second for each processor to take it.

### Credentials

Each record has the real, effective and saved user and group ids and up to `NGROUPS_MAX`, 8, supplementary groups (`process.h`). The ids are the kernel's, those of the first user namespace; a process in another one sees and names them through its maps ([namespaces](ns.Readme.md)). The setters change the calling thread's ids, as Linux's do, under the saved-id rules of POSIX: a privileged caller sets any id its user namespace maps, and another moves its ids only among its real, effective and saved ones. A change of an effective id clears the address space's dumpable flag and the parent-death signal, as Linux's `commit_creds` does, and a change of the effective user id takes away the capabilities held in a user namespace (`cred_moved`).

The kernel keeps every capability or none. `proc_privileged` (`process.h`) is true for a thread with effective user id 0 in the first user namespace that has not given its capabilities up. `capset` with every set empty gives them up (`nocaps`) until an exec gives them back, and `capget` reports all of Linux's 41, numbered 0 to 40, or none. In a user namespace other than the first a thread may keep part of the sets; [namespaces](ns.Readme.md) gives when it is privileged there.

`exec_ids` chooses the ids of a new image from the file's set-id bits and the caller's effective ids, and `exec_privileges` makes an image that runs with ids its caller lacks not dumpable and lowers its soft `RLIMIT_STACK`, when above 8 MiB, to 8 MiB; [execve(2)](../../../man/man2/execve.2) has the rules. For a caller that is not privileged, the bits do not apply when its thread has a tracer that is not privileged or shares its directories with a process outside its group, since either could change the image once it had the ids. While an exec that applies them runs, `CLONE_FS` fails in the threads that share its directories (`in_exec` in `struct fs_struct`), so that the choice stays true until the image is the process's.

### Resource limits

A record has six limits, `RLIM_COUNT` in `process.h`, each a soft value that is enforced and a hard value the soft one may be raised to, `RLIM_INFINITY` for none. A limit is a `long`: 32 bits on i686, where a 64-bit value from `prlimit64` past them is kept as no limit, and 64 bits on x86-64. A fork copies the limits and an exec keeps them, but for the lowered stack limit above. `rlimit_set` (`rach2/sys/syscall_proc.c`) writes the limit into every thread of the process; it refuses a soft value above the hard one with `EINVAL`, a raised hard value from a caller that is not privileged with `EPERM`, and a hard `RLIMIT_NOFILE` above `FD_TABLE_LIMIT` with `EPERM`. A resource Linux has and this kernel does not keep reads as unlimited, and a set of one is accepted and changes nothing.

| Limit | Default soft, hard | Enforced by |
| --- | --- | --- |
| `RLIMIT_NPROC` | 4096, 4096 (`PROC_COUNT_POLICY`) | `fork_nproc`: the records of the caller's real user, each thread and zombie counted; real user id 0 is exempt |
| `RLIMIT_NOFILE` | 1024, 1024 (`FD_TABLE_LIMIT`) | the descriptor allocator, `rach2/sys/fs/fd.c` |
| `RLIMIT_AS` | none, none | `brk`, `mmap`, `mremap`, the growth of the stack at a page fault (`rach2/sys/mm/paging.c`), and the image `execve` builds |
| `RLIMIT_STACK` | 8 MiB (`RLIM_STACK_DEFAULT`), none | `execve`: the room for the strings and their pointers, and how far the new stack may grow |
| `RLIMIT_CORE` | 0, none | a core file: none below one page, and the file stops at the soft value |
| `RLIMIT_FSIZE` | none, none | a write, a truncation or an `fallocate` past it fails with `EFBIG` and sends `SIGXFSZ` (`fsize_refused` in `rach2/sys/syscall_fs.c`) |

Init sets the machine's defaults from `/etc/rach2/limits` before its first child, as [limits(7)](../../../man/man7/limits.7) describes.

### seccomp

A thread has a chain of filters, newest first, each a classic BPF program, the instruction set of the BSD packet filter (`seccomp.c`). A filter runs over Linux's `struct seccomp_data`: the call's number, the architecture's audit number, `AUDIT_ARCH` in its `cpu.h` (`0x40000003` on i686, `0xC000003E` on x86-64), the instruction pointer, and the six arguments, each widened to 64 bits. The check at the install, as Linux's `seccomp_check_filter`, takes at most 4096 instructions, loads inside the structure, sixteen scratch words, and jumps that go forward and stay inside the program, so every program ends at a return. A chain may have 32768 instructions, each filter counted with four more. A caller without `no_new_privs` must be privileged to install a filter, and `SECCOMP_FILTER_FLAG_TSYNC` gives the chain to every thread of the process.

The dispatcher (`_syscall_handler` in `rach2/sys/syscall.c`) runs the filters after a tracer's stop at the call's entry, on the call the tracer left, and before the handler. Every filter of the chain runs (`chain_action`), and the action of highest precedence is taken, the actions compared as signed numbers as Linux compares them (`precedence`); [seccomp(2)](../../../man/man2/seccomp.2) has what each action does. The strict mode's four calls are `SECCOMP_NR_` in each `cpu.h`, with `rt_sigreturn` in place of `sigreturn` on x86-64. A fork passes the mode and the chain on, a chain is shared and counted by the threads and children that have it, and an exec keeps both.

### Core files

A signal whose default action dumps core, a seccomp kill of the process, and a signal frame that cannot be built or read back end the process through `terminate_current_process` with a core asked for. `terminate_current_process` (`rach2/kernel.c`) writes the core when the process is dumpable ([prctl(2)](../../../man/man2/prctl.2)), its soft `RLIMIT_CORE` is at least a page, and no other thread has begun to end it ([core(5)](../../../man/man5/core.5)). It sets the group's `core_dumping`, sends the other threads `SIGKILL` (`proc_group_exit`), and calls `fs_core_file` (`rach2/sys/syscall_fs.c`) before this thread releases anything. Each other thread, ending by that `SIGKILL`, saves its floating-point state and parks before it releases anything (`core_park` in `coredump.c`), so that none runs in user mode on another processor while its registers are copied or the memory written.

`core_notes` waits until each other thread has parked or ended, as Linux's `coredump_wait` waits, or until an exec by one of them waits for the rest, and builds the `PT_NOTE` segment in the heap: the dumping thread's `elf_prstatus`, the process's `elf_prpsinfo` and auxiliary vector, the thread's floating-point notes (`arch_core_fp_notes`), then each other thread's `elf_prstatus` and floating-point notes. The registers are those `arch_regs_get` reads from the frame at the top of each kernel stack, as the ptrace requests read them ([ptrace](ptrace.Readme.md)).

`core_write` takes the regions once into a list, which the program headers and the pages follow, since another process that shares the address space, as a `vfork` child does, is not ended and may change the regions while the writer sleeps; each page is looked up again as it is read, and one unmapped meanwhile is a hole. Each page is read through `paging_access`, which brings in from the file a page of a mapped file the process never touched; a page of anonymous memory it never touched is a hole, which the writer may skip. A process may have up to `VMA_LIMIT`, 65536, regions (`rach2/sys/mm/vm.h`), more than the ELF header's 16-bit count of program headers can record, so a file of 65535 headers or more uses ELF's extended numbering. `fs_core_file` writes the file `core` in the working directory: it unlinks whatever has the name, creates the file with `O_EXCL` and `O_NOFOLLOW`, and writes through a descriptor marked closing from the moment it is opened, so that a process sharing the descriptor table cannot use it. The parked threads go on to their end once the file is written or given up (`core_release`), and the wait status has `WCOREFLAG` only when the whole file was written.

### Suspend to idle

A write of `freeze` or `mem` to `/sys/power/state` (`rach2/sys/fs/procfs.c`) calls `suspend_to_idle` (`suspend.c`), the sleep state Linux calls freeze. The writer sleeps on a wait queue, and until it wakes the scheduler runs no other process (`suspend_lets_run`), so each processor idles. Interrupts are taken and the clocks count, since nothing is powered down. The clock's alarm interrupt (`rach2/sys/fs/devfs.c`) and a key (`input_key` in `rach2/sys/input.c`) are the wake events (`suspend_wake`): the writer runs, its write returns, and the other processes run again. A signal ends the wait with `EINTR`, and a write while another suspend is in force fails with `EBUSY`.

## Limits

* One lock covers the kernel: kernel code runs on one processor at a time, and a long system call delays every other processor's entry to the kernel.
* The scheduler has one policy, round robin with a slice of one 10 ms tick, and no priorities; the nice values and I/O priorities are kept and not read. The load averages count no sleeper, uninterruptible or not.
* The kernel runs on at most 64 processors, and an affinity mask has 64 bits.
* In the first user namespace a thread has all 41 capabilities or none, and the inheritable set is always empty.
* A thread has at most 8 supplementary groups, where Linux allows 65536.
* `clone` refuses `CLONE_SIGHAND` without `CLONE_THREAD`, an exit signal other than `SIGCHLD` for a process, `CLONE_PIDFD`, and the cgroup and time namespaces. No descriptor refers to a process, so `waitid`'s `P_PIDFD` fails with `EBADF`, and `__WCLONE` without `__WALL` selects no child.
* `execve` follows one level of `#!`, and reads a first line of at most 128 bytes.
* Six resource limits are kept; `RLIMIT_MEMLOCK`, `RLIMIT_NICE`, `RLIMIT_RTPRIO` and Linux's others read as unlimited.
* `getrusage` and `wait4` report the CPU time and `ru_maxrss` alone, and `ru_maxrss` is sampled where an address space ends, not at every fault.
* `prctl` takes ten options, the parent-death signal, the dumpable flag, the name, `no_new_privs` and the seccomp mode, each set and read; any other is `EINVAL`.
* seccomp has no user notification: a filter cannot be installed with a listener, and `SECCOMP_RET_USER_NOTIF` fails the call with `ENOSYS`. `SECCOMP_RET_LOG` allows the call and logs nothing.
* `personality` takes only `PER_LINUX` and `PER_LINUX32`, and its flags change nothing; the kernel places no address at random.
* Suspend to idle powers nothing down. A write of `mem` to `/sys/power/state` suspends to idle as `freeze` does, and the kernel has no suspend to memory or to disk.

## Tests

`tests/run default` boots the test image and runs every guest probe in one machine, and `ARCH=x86_64 tests/run default` runs them on x86-64 ([the tests' Readme](../../../tests/Readme.md)). These test the process subsystem:

* `tests/forktest.c`, `tests/exitprobe.c`, `tests/orphanprobe.c`, `tests/zombieprobe.c`, `tests/survivorprobe.c` and `tests/forkleak.c`: fork, an end by exit, by a signal and after a failed exec, reparenting onto init, signal teardown beside a writer, an init that exits with a runnable child, and no frame lost across fork and reap.
* `tests/musl/clonemusl.c`, `tests/musl/vforkmusl.c`, `tests/musl/spawnmusl.c` and `tests/musl/threadmusl.c`: `clone` as glibc's fork calls it, `vfork` and `CLONE_VM|CLONE_VFORK`, `posix_spawn`, and threads: their ids, shared descriptors and directories, an `execve` from a thread and two at once.
* `tests/auxvprobe.c`, `tests/musl/execargmusl.c` and `tests/musl/stackmusl.c`: the initial stack, the limits on the strings `execve` copies, and `RLIMIT_STACK`.
* `tests/musl/waitidmusl.c`, `tests/musl/cputimemusl.c` and `tests/jobprobe.c`: `waitid`'s siginfo, CPU time, and sessions and process groups.
* `tests/credprobe.c`, `tests/suidprobe.c`, `tests/musl/suidmusl.c` and `tests/musl/capmusl.c`: the credential rules, a set-id exec, and capabilities.
* `tests/limitprobe.c`: `RLIMIT_NPROC`, `RLIMIT_NOFILE` and `RLIMIT_AS`.
* `tests/musl/schedmusl.c`, `tests/musl/prctlmusl.c`, `tests/musl/seccompmusl.c`, `tests/musl/coremusl.c` and `tests/musl/callsmusl.c`: thread ids, nice values and the scheduling calls, `prctl`, seccomp filters, core files, and among other calls `getcpu`, `personality` and a suspend to idle.

`tests/system/probe_faults.sh` puts the faults of `tests/system/probe_faults.txt` into the tree one at a time, each of which a row of the default suite must catch; `sh tests/system/probe_faults.sh NAME` runs one.

Other suites ([the suites](../../../tests/system/suites.md)):

* `tests/run smp` boots one processor, four, and two of four, and on each runs `tests/musl/smpmusl.c`, which finds the processors and runs a stress of fork, exec, pipes, futexes and `munmap` across them. On two processors it also runs the debug shell's preemption test and ring-3 payload and raises an NMI, which must stop the machine, and it runs a parallel build on four (`tests/system/suites/smp.sh`).
* `tests/run fpu` checks that a process's floating-point state is its own across switches.
* `tests/run exitline` checks that the plain process-exit line is printed under the debug shell and nowhere else.
* `tests/run memory` includes the overrun of a kernel stack ([the architectures' Readme](../arch/Readme.md)).

`tests/run host` runs the host tests ([the host tests](../../../tests/host/Readme.md)) of this directory: `sched_hosttest.c`, `wait_queue_hosttest.c`, `smp_hosttest.c`, `process_timer_hosttest.c`, `exit_hosttest.c`, `elf_hosttest.c`, `uimage_hosttest.c`, `coredump_hosttest.c` and `corefile_hosttest.c`, in `tests/host/`. `sched_mutate.sh`, `smp_mutate.sh`, `exit_mutate.sh`, `uimage_mutate.sh`, `corefile_mutate.sh` and `kstack_mutate.sh` there prove them with faults in private copies of the sources.
