# Lockup detectors

`watchdog.c` finds the three hangs Linux's kernel/watchdog.c, watchdog_perf.c, watchdog_buddy.c and hung_task.c find, with their messages and settings: a soft lockup, a processor that has not scheduled for 2 x `watchdog_thresh` seconds, 20 by default; a hard lockup, a processor whose timer interrupts have stopped for `watchdog_thresh` seconds; and a hung task, a thread in uninterruptible sleep, the state D, for `hung_task_timeout_secs`, 120 by default. Each report gives the processor, the thread, the registers and the return addresses of the stack, and with its panic option set ends in a panic; the all-CPU backtrace options add every other processor's registers and stack from an NMI. [lockup(7)](../../man/man7/lockup.7) is the user's page.

## Use

The settings are the files of `/proc/sys/kernel` that Linux has, and boot options of the same names ([the command line](../cmdline.md)): `watchdog`, `soft_watchdog`, `nmi_watchdog`, `watchdog_thresh`, `softlockup_panic`, `hardlockup_panic`, `softlockup_all_cpu_backtrace`, `hardlockup_all_cpu_backtrace`, `hung_task_all_cpu_backtrace`, `hung_task_panic`, `hung_task_check_count`, `hung_task_timeout_secs`, `hung_task_check_interval_secs` and `hung_task_warnings`. Each is also `sysctl.kernel.NAME=` on the command line, and the boot options `nmi_watchdog=panic|nopanic|0|1`, `softlockup_panic=`, `nowatchdog`, `nosoftlockup` and `watchdog_thresh=` read as Linux's do (kernel/watchdog.c:79-101 and :325-349). A write that changes `watchdog`, `soft_watchdog`, `nmi_watchdog` or `watchdog_thresh` stops each processor's detectors and starts them again at its next tick, with a new sample period and their timestamps touched, as Linux's `proc_watchdog_update` does, so that 0 disarms the counter and no detector reads a time from before the write; another write takes effect at the next sample period, 2 x `watchdog_thresh` / 5 seconds, and a write of the hung-task timeout or interval wakes `khungtaskd` at once. A read of `watchdog`, `soft_watchdog` or `nmi_watchdog` gives whether a detector it names is on by the switches, as Linux's does; where the hard detector cannot run, with no counter and one processor, `nmi_watchdog` reads 0 and is mode 0444.

The debug shell's `lockup soft|hard|hung SECONDS` holds a lockup of each kind for the detectors to report, as Linux's lib/test_lockup.c does: the shell's processor spins in the kernel with interrupts on or off, or the kernel thread `test_lockup` sleeps uninterruptibly while the shell turns the scheduler on and waits. `lockup: done` follows when it ends. Root's shell does the same with a write of `soft|hard|hung SECONDS` to `/sys/kernel/debug/test_lockup` ([debugfs(5)](../../man/man5/debugfs.5)), in which the writer spins, or sleeps in the state D itself, as test_lockup's `state=D` sleeps in the process that loads it; the write returns when the lockup ends.

## Results

```
watchdog: BUG: soft lockup - CPU#0 stuck for 5s! [:0]
watchdog: CPU: 0 PID: 0 Comm:  Not tainted 6.18.0-rach2
watchdog: RIP: 0010:0xffffffff8039e7ba
...
watchdog: Call Trace:
watchdog:  0xffffffff8039c700
```

The hard lockup's headline is `watchdog: watchdog detected hard LOCKUP on cpu N`, followed by the registers when the processor's own counter caught it, or by `nmi_backtrace: NMI backtrace for cpu N` and its registers when the buddy check did. The hung task's is `hung_task: INFO: task NAME:PID blocked for more than N seconds.` with Linux's two lines after it, the thread's state line and its stack. The kernel has no symbol table, so a trace gives addresses; `addr2line -f -e build/ARCH/rach2/kernel.debug ADDRESS` names them. A panic's reason is `softlockup: hung tasks`, `Hard LOCKUP` or `hung_task: blocked tasks`, as Linux's. The headline is a `KLOG_CRIT` record, which the panic screen shows again on its last rows, and the dump is at `KLOG_ERR` or below, so that it does not take the headline's place there.

## Interfaces

`watchdog.h` declares the settings, `watchdog_init`, which the boot calls once after the other processors start, and the calls the rest of the kernel makes: `watchdog_tick` from every processor's tick, `watchdog_sched` from `schedule` and `sched_service_point`, `watchdog_wait` from each turn of a wait with interrupts off (`smp_service`), `watchdog_nmi` from the NMI handler, `touch_softlockup_watchdog` and `touch_nmi_watchdog` for code that keeps a processor on purpose, and `watchdog_test` for the shell. The PC's part is in [`platform.h`](platform.h), `arch/x86/pmu.c`, the counter (`platform_pmu_probe`, `_enable`, `_disable`, `_overflow`) and the clock (`platform_cpu_hz`), and `arch/x86/nmiwd.c`, the register dump and stack walks (`platform_show_regs`, `platform_show_stack`, `platform_blocked_frame`) and whether a frame is a halted idle wait (`platform_frame_idle`).

## Design

### Soft lockup

Each processor counts its ticks and runs `watchdog_timer_fn` every sample period of ticks or of the TSC, whichever ends first, from the tick's router before it takes the big lock (kernel/watchdog.c:675-775), so that the first tick after a spin with interrupts off runs it at once, in the context of the spin, as Linux's expired hrtimer does. The timer queues the soft-lockup function, which Linux runs in a stopper thread and which here runs at the processor's next schedule point: `schedule`, `sched_service_point`, which is Rach2's `cond_resched`, a tick that finds user mode, or a tick that finds the processor halted in an idle wait. The function stores the time, and a timer that finds it more than 2 x `watchdog_thresh` seconds old reports. Times are seconds of the TSC at the rate `platform_pmu_probe` measured, Linux's `running_clock`, since the kernel's tick count moves only with the holder of the big lock.

### Hard lockup

Where the processor has a cycles counter, Intel's architectural performance monitor or AMD's core counters, each processor programs counter 0 to count unhalted core cycles and raise an NMI after `watchdog_thresh` seconds of them at the TSC's rate (arch/x86/kernel/apic/hw_nmi.c:27), through its local APIC's performance-counter entry, which the handler unmasks again after each NMI. A period wider than the counter's writable width, 31 bits without Intel's full-width alias, is counted in loads. The NMI checks that the processor's timer count has moved since the last NMI (kernel/watchdog.c:131-221), skipping one that comes sooner than 4/5 of the period after the last, ten in a row at most (kernel/watchdog_perf.c:58). Without a counter, as under QEMU's emulator, each processor checks the next one's count every third sample period, the buddy check (kernel/watchdog_buddy.c:88), and has it log its registers and stack from an NMI.

Linux has no big lock, and Rach2's waiters for it spin with interrupts off, the idle processors among them, which take it at each tick to schedule. So a sample period without a tick in a wait for the big lock counts as the waiter's timer, moving its count and its soft-lockup time and running its buddy check every third (`watchdog_wait`), and only the holder is reported. A wait for anything else, a spinlock or another processor's answer, leaves the count still, so that its buddy or its counter reports it, as Linux reports a processor that spins with interrupts off, and two processors that wait on each other are both reported; without a counter such a wait runs its own buddy check each third period, so that the check goes on while every processor waits.

### Hung task

The kernel thread `khungtaskd` wakes every `hung_task_check_interval_secs`, or every timeout when that is 0, and walks the processes for one blocked uninterruptibly whose switch count has not moved for the timeout (kernel/hung_task.c:91, :181 and :361). Each report counts down `hung_task_warnings`; at 0 the reports stop, and -1 never stops them. The thread's stack is read from the frame its switch saved.

### Reports and the panic

Reports are serialized among the detectors by a lock taken again by the processor that holds it, as an NMI on that processor would, and waited for at most a second, since the holder may be the processor locked up. klog builds one record at a time and its other callers hold the big lock, so the builder puts its state back after each record, for an NMI that logs in the middle of another. With a panic option the report's records go out directly, since the panic never returns to drain the queue. The panic stops the other processors with an interprocessor interrupt and, after a second, an NMI to each that did not stop, as Linux's x86 stop does (arch/x86/kernel/smp.c:202 and :227), so that a processor with interrupts off stops too.

## Limits

* The panic picture clears the screen, so a report reaches the screen before it and stays whole on the serial port, netconsole and the log.
* A klog record of a processor that holds the big lock and one of a detector on another processor can still interleave, since klog takes no lock.
* `watchdog_cpumask` and per-processor masks are not kept; every processor online is watched.
* A processor other than Intel's with the architectural monitor or AMD's with core counters uses the buddy check, which needs two processors.
* The first process's name is empty, so a soft lockup in the debug shell shows `[:0]`.

## Tests

* `tests/run smp` has the cases `machine.smp.softlockup`, `hardlockup` and `hungtask` of [`machine.smp.json`](../../tests/cases/machine.smp.json): on two processors with `watchdog_thresh=2` and `sysctl.kernel.hung_task_timeout_secs=3`, the debug shell's `lockup` of each kind must be reported with its call trace and, under `softlockup_panic=1`, `nmi_watchdog=panic` or `hung_task_panic=1`, end in its panic with both processors halted; `machine.smp.lockups` holds all three in one boot without a panic option, and each must be reported and give the shell back. Under TCG the hard lockup is the buddy check's; under KVM, whose `-cpu max` has a PMU, the counter's.
* `tests/host/pmu_hosttest.c` runs `arch/x86/pmu.c` against a model of the counter's registers on the Lenovo's Haswell and six other processors, and checks each register write, the NMIs due at each period and the counter stopped after the disable.
