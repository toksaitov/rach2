/*
 * watchdog.h: the lockup detectors, as Linux 6.12's kernel/watchdog.c,
 * watchdog_perf.c, watchdog_buddy.c and hung_task.c have them: the soft
 * lockup of a processor that has not scheduled for 2 x `watchdog_thresh`
 * seconds, the hard lockup of one whose timer interrupts stopped, and the
 * hung task, a thread in uninterruptible sleep for `hung_task_timeout_secs`.
 */
#ifndef _WATCHDOG_H_
#define _WATCHDOG_H_

#include "arch/arch.h"
#include "lib/kint.h"

/*
 * The settings, each a file of /proc/sys/kernel under its Linux name, which
 * the boot options set first (`watchdog_init`) and procfs writes; each is
 * read where it is used, so a write takes effect at the next sample period.
 * `watchdog_user_enabled` is `watchdog`, `watchdog_softlockup_user_enabled`
 * `soft_watchdog` and `watchdog_hardlockup_user_enabled` `nmi_watchdog`.
 */
extern int watchdog_user_enabled;
extern int watchdog_softlockup_user_enabled;
extern int watchdog_hardlockup_user_enabled;
extern int watchdog_thresh;
extern int softlockup_panic;
extern int hardlockup_panic;
extern int sysctl_softlockup_all_cpu_backtrace;
extern int sysctl_hardlockup_all_cpu_backtrace;
extern int sysctl_hung_task_all_cpu_backtrace;
extern int sysctl_hung_task_panic;
extern int sysctl_hung_task_check_count;
extern int sysctl_hung_task_timeout_secs;
extern int sysctl_hung_task_check_interval_secs;
extern int sysctl_hung_task_warnings;

/**
 * Start the detectors, once, on the boot processor after the others are
 * started (`smp_start`): read the boot options, find the hard-lockup
 * detector's counter or fall back to the buddy check, and start the
 * hung-task thread, `khungtaskd`. Every processor online is enabled at
 * once, so that the buddy check watches it from the start, and arms its own
 * counter at its next sample period.
 */
void watchdog_init(void);

/**
 * Run the detectors' part of this processor's tick, from the tick's router
 * before it takes the big lock, which a processor locked up with
 * interrupts off may hold for good: count the tick, run the watchdog's
 * timer each sample period, 2 x `watchdog_thresh` / 5 seconds, and report a
 * soft lockup on this processor or a hard lockup on the next. `params` is
 * the frame the tick interrupted. Interrupts are off.
 */
void watchdog_tick(isr_params_t *params);

/**
 * The most ticks an idle processor may let pass between two of its ticks
 * (apictimer.c): half a sample period, so that its watchdog timer still
 * runs every period or two and its buddy sees its count move, as Linux's
 * watchdog hrtimer ends a NO_HZ processor's idle each sample period
 * (kernel/watchdog.c:675-697); a second's worth while `watchdog_thresh` is
 * 0. Any context.
 */
uint32_t watchdog_idle_ticks(void);

/**
 * Keep the hard-lockup detector going through a wait with interrupts off,
 * which takes no ticks: a sample period of a wait for the big lock counts
 * as this processor's timer, and the buddy check runs every third period of
 * any wait, so that a processor that locks up holding what the others wait
 * for is reported. Called on each turn of such a wait (`smp_service`);
 * cheap on most.
 */
void watchdog_wait(void);

/** Note that this processor has scheduled, or reached a point where it
 * could have (`sched_service_point`), which is where Linux's stopper thread
 * would run and touch the soft-lockup timestamp. Interrupts are off. */
void watchdog_sched(void);
/** Touch the lockup detectors after an idle wait with the tick stopped.
 * Called on the waking processor with interrupts off. */
void watchdog_idle_exit(void);

/**
 * Take an NMI meant for the watchdog: a backtrace request, the counter's
 * overflow, which runs the hard-lockup check, or the stop of a panic that
 * the interprocessor interrupt did not reach (`smp_stop_others`), which
 * `smp_stop_nmi` takes and does not return from. 1 when the NMI was the
 * watchdog's or came late for it, 0 for one of another source, which the
 * caller reports as a fault of the machine.
 *
 * The processor latches one NMI while it handles another. When an NMI
 * handles two sources, a backtrace request and the counter's overflow, the
 * second may have latched one of its own, which then finds nothing to do;
 * so an NMI that finds nothing, back to back with one that handled more
 * than one source at the same program counter, is claimed, as Linux's
 * `default_do_nmi` swallows it (arch/x86/kernel/nmi.c:404-405, 472-475).
 * One the counter raised just before it was armed again or stopped, or
 * that AMD's counter raised late after an overflow it handled, finds
 * nothing either, and is claimed within the counter's window
 * (`platform_pmu_late`), which on AMD opens once the hard-lockup check of
 * the overflow has run.
 */
int watchdog_nmi(isr_params_t *params);

/** Delay this processor's next soft-lockup report by a sample period, for
 * code known to keep the processor for long, as Linux's
 * `touch_softlockup_watchdog`. */
void touch_softlockup_watchdog(void);

/** Skip this processor's next hard-lockup check, as Linux's
 * `touch_nmi_watchdog`, which also touches the soft-lockup detector. */
void touch_nmi_watchdog(void);

/** Whether the hard-lockup detector can run here: a counter that raises an
 * NMI, or the buddy check, which needs two processors. */
int watchdog_hardlockup_available(void);

/** A write changed `watchdog`, `soft_watchdog`, `nmi_watchdog` or
 * `watchdog_thresh`: each processor stops its detectors and starts them
 * again with the new settings at its next tick, or its next sample period
 * in a wait, as Linux's `proc_watchdog_update` does. */
void watchdog_reconfigure(void);

/** The value a read of /proc/sys/kernel/watchdog (`which` 0),
 * soft_watchdog (1) or nmi_watchdog (2) gives: 1 when a detector it names
 * is on by the switches, whatever the threshold, as Linux reads its
 * `watchdog_enabled` (kernel/watchdog.c:980-985), so that nmi_watchdog
 * reads 0 where the hard detector cannot run. */
int watchdog_sysctl_value(int which);

/** A /proc/sys/kernel file of the hung-task detector was written: wake
 * `khungtaskd` to read its timeout and interval again, as Linux's
 * `proc_dohung_task_timeout_secs` does. */
void watchdog_sysctl_changed(void);

/**
 * Log "Call Trace:" and the return address of each frame from the frame
 * pointer `fp` up, at `level` under `subsystem`. The walk reads only the
 * KERNEL_STACK_SIZE bytes below `top`, the stack's top, and only upward; a
 * `top` of 0 logs no frames.
 */
void watchdog_show_trace(const char *subsystem, int level, uintptr_t fp, uintptr_t top);

/**
 * The debug shell's `lockup` command, Linux's lib/test_lockup.c: hold this
 * processor for `seconds` with interrupts on (`kind` "soft") or off
 * ("hard"), or have a kernel thread, `test_lockup`, sleep uninterruptibly
 * for that long ("hung"), with the scheduler on meanwhile. 0, or -1 for a
 * kind it does not know or a thread it could not start.
 */
int watchdog_test(const char *kind, unsigned int seconds);

#endif /* _WATCHDOG_H_ */
