/*
 * suspend.h: suspend to idle, the sleep state Linux calls `freeze`.
 *
 * Writing `freeze` or `mem` to /sys/power/state holds every process but
 * the writer, which waits while the processors idle, and lets them go again
 * at a wake event: the clock's alarm or a key. Interrupts go on being
 * taken, and the clocks go on counting, since nothing of the machine is
 * powered down.
 */
#ifndef _SUSPEND_H_
#define _SUSPEND_H_

struct process;

/**
 * Suspend to idle until a wake event, as the current process. 0 at the
 * wake, -EBUSY while another suspend holds the processes, -EINTR when a
 * signal ends the wait.
 */
int suspend_to_idle(void);

/** Whether the scheduler may run `p` now: any process, but while a suspend
 * holds them only the one that suspended. */
int suspend_lets_run(const struct process *p);

/** A wake event, from an interrupt's handler: a suspend in force ends. */
void suspend_wake(void);

#endif /* _SUSPEND_H_ */
