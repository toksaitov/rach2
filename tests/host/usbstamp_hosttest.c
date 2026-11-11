/**
 * Drive the production USB recorder and scheduler with explicit event order.
 *
 * host-deps:
 *
 * The clock, interrupt mask and CPU switch are substituted. An optional
 * argument emits one record for the USB suite's own AWK checker.
 */
#include <stdarg.h>
#include "compat/waketime_none.h"

#include "usb/stamp.c"
#include "proc/sched.c"

extern int printf(const char *format, ...);
#ifndef _WIN32
extern int dprintf(int fd, const char *format, ...);
#else
extern int _write(int fd, const void *bytes, unsigned size);
#endif
extern int vsnprintf(char *out, size_t size, const char *format, va_list args);
extern int strcmp(const char *a, const char *b);
extern char *strstr(const char *text, const char *part);

process_t *proc_list;
struct percpu cpu_table[NR_CPUS];
process_t *proc_idle;
volatile uint64_t kernel_ticks;

/** A spin's pause, which the host does not need. */
void arch_relax(void)
{}
/* pid 10, running, and pid 11, the console reader. */
static process_t processes[2];
/* The time `timer_now_us` returns, in microseconds; the modelled EFLAGS, with
 * 0x200 (IF) set while interrupts are enabled. */
static uint32_t clock_us, irq_flags;
/* The records `klog` has taken since `begin`, and the text of the last. */
static unsigned records;
static char record[512];
static int failures;

/** Preserve the diagnostic stream with the native CRT on each host. */
static void failed_check(const char *detail)
{
#ifdef _WIN32
    static const char prefix[] = "usbstamp-test: failed: ";
    unsigned size = 0;
    while (detail[size])
        size++;
    _write(2, prefix, sizeof(prefix) - 1);
    _write(2, detail, size);
    _write(2, "\n", 1);
#else
    dprintf(2, "usbstamp-test: failed: %s\n", detail);
#endif
}

#define CHECK(condition, detail)  \
    do {                          \
        if (!(condition)) {       \
            failed_check(detail); \
            failures++;           \
        }                         \
    } while (0)

/** Substitute an explicit clock; the test does not wait. */
uint32_t timer_now_us(void)
{
    return clock_us;
}

/** Read the explicit clock as the monotonic clock, for the run queue's
 * waits, which no row judges. */
void rtc_monotonic(int64_t *sec, int64_t *ns)
{
    *sec = clock_us / 1000000u;
    *ns = (int64_t)(clock_us % 1000000u) * 1000;
}

/** Timer list membership is checked by process_timer_hosttest. */
void proc_timer_update(process_t *p)
{
    (void)p;
}

/** Model interrupt masking around the diagnostic snapshot. */
uint32_t kirq_save(void)
{
    uint32_t flags = irq_flags;

    irq_flags = 0;
    return flags;
}

/** Restore the mask that `kirq_save` returned. */
void kirq_restore(uint32_t flags)
{
    irq_flags = flags;
}

/** Return at once: no row waits for an interrupt. */
void arch_idle_masked(void)
{}

/** Format the record into `record` as the suite receives it, and count it. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    va_list args;

    (void)level;
    CHECK(strcmp(subsystem, "usb") == 0, "record subsystem");
    va_start(args, format);
    vsnprintf(record, sizeof(record), format, args);
    va_end(args);
    records++;
}

/** Mask interrupts: with one processor the run queues' lock is the mask. */
uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    (void)lock;
    return kirq_save();
}

/** Restore the mask that `spin_lock_irqsave` returned. */
void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    (void)lock;
    kirq_restore(flags);
}

/** Do nothing: one processor takes no lock. */
void spin_lock(struct spinlock *lock)
{
    (void)lock;
}

/** Do nothing, as `spin_lock` does. */
void spin_unlock(struct spinlock *lock)
{
    (void)lock;
}

/** Return depth 0: the model has no big kernel lock to let go. */
int bkl_drop(void)
{
    return 0;
}

/** Do nothing: no big kernel lock was let go. */
void bkl_take(int depth)
{
    (void)depth;
}

/** Nothing: there is no big lock. */
void bkl_yield(void)
{}

/** Do nothing: there is no other processor to wake. */
void smp_kick(int cpu)
{
    (void)cpu;
}

/** Do nothing: the idle task's loop does not run here. */
void arch_irq_disable(void)
{}

/** Do nothing: no process here arms a timer of processor time. */
void signal_post(process_t *target, int signo)
{
    (void)target;
    (void)signo;
}

/** Return 1: no suspend to idle holds the processes here. */
int suspend_lets_run(const struct process *p)
{
    (void)p;
    return 1;
}

/** Do nothing: no process here has a POSIX timer on processor time. */
void ptimer_cpu_tick(const struct process *p, int user)
{
    (void)p;
    (void)user;
}

/** No floating-point instruction is needed by the host switch model. */
void fpu_save(void *area)
{
    (void)area;
}

/** No floating-point instruction is needed by the host switch model. */
void fpu_restore(const void *area)
{
    (void)area;
}

/** No rseq area is registered in the host switch model. */
void rseq_preempt(process_t *p)
{
    (void)p;
}

/** No rseq area is registered in the host switch model. */
void rseq_migrate(process_t *p)
{
    (void)p;
}

/** The recorder's model never waits for a remote affinity change. */
void wait_queue_init(wait_queue_t *wq)
{
    (void)wq;
}

/** No remote affinity waiter is present in the recorder's model. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** Refuse an unexpected remote affinity wait in this single-CPU model. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    CHECK(cond(arg), "unexpected remote affinity wait");
    return 0;
}

/** No process registers for the sync-core barrier in the host model. */
void membarrier_switch(const struct process *p)
{
    (void)p;
}

/** No debug register is set in the host switch model. */
void arch_debug_switch(struct process *old, const struct process *next)
{
    (void)old;
    (void)next;
}

/**
 * Check that the dispatch is atomic: interrupts are masked and `current_proc`
 * is already `next`. The host stays on its own stack and returns.
 */
void switch_context(process_t *old, process_t *next)
{
    (void)old;
    CHECK(!irq_flags && current_proc == next, "dispatch is atomic");
}

/** Return 0: these rows have no loopback packets. */
int ip_loop_pending(void)
{
    return 0;
}

/** Do nothing: these rows have no loopback packets. */
void ip_loop_drain_from(int from)
{
    (void)from;
}

/**
 * Arm one blocked reader, pid 11, beside one running process, pid 10, and
 * deliver one keystroke to it at 32000 us: a pump that entered at 1000 us and
 * drained at 31000 us, run by an interrupt when `msi` is nonzero. The reader
 * is woken but not yet dispatched, and no record has been taken.
 */
static void begin(int msi)
{
    unsigned i;
    unsigned char *bytes = (unsigned char *)processes;

    for (i = 0; i < sizeof(processes); i++) {
        bytes[i] = 0;
    }
    processes[0].pid = 10;
    processes[0].state = PROC_RUNNING;
    processes[0].next = &processes[1];
    processes[1].pid = 11;
    processes[1].state = PROC_BLOCKED;
    processes[1].prev = &processes[0];
    processes[0].affinity = processes[1].affinity = ~(uint64_t)0;
    cpu_table[0].online = 1;
    current_proc = proc_list = processes;
    init_sched();
    sched_enable();
    usb_stamp_init(1);
    usb_stamp_wait(11);
    usb_stamp_pump(1000, 31000, msi, 1);
    clock_us = 32000;
    usb_stamp_wake();
    sched_make_ready(&processes[1]);
    processes[1].woken = 1;
    records = 0;
    record[0] = '\0';
}

/**
 * Dispatch the reader through the scheduler unless it already runs, add a
 * pump and 65536 bytes of work after the dispatch, and read at 112000 us
 * with interrupts enabled; the read must leave them enabled.
 */
static void finish(void)
{
    if (current_proc->pid != 11) {
        schedule();
    }
    usb_stamp_pump(40000, 41000, 0, 0);
    usb_stamp_work(65536);
    clock_us = 112000;
    irq_flags = 0x200;
    usb_stamp_read();
    CHECK(irq_flags == 0x200, "read restores interrupts");
}

/**
 * With no argument, run every row and print the result. With a mode (spin,
 * timer, random, pump, work or undispatched), run that one case and print
 * its record for `tests/system/checkers/usblatency.awk`, the checker of the
 * guest rows; the exit status is 1 when a row of the case failed.
 */
int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "spin";
    int random = strcmp(mode, "random") == 0;

    begin(strcmp(mode, "timer") != 0);
    if (random) {
        usb_stamp_pump(40000, 41000, 0, 0);
        usb_stamp_work(4096);
        current_proc->in_syscall = 1;
        sched_preempt();
        sched_service_point();
    } else if (strcmp(mode, "pump") == 0) {
        usb_stamp_pump(40000, 41000, 0, 0);
    } else if (strcmp(mode, "work") == 0) {
        usb_stamp_work(4352);
    }
    if (strcmp(mode, "undispatched") == 0) {
        clock_us = 112000;
        usb_stamp_read();
    } else {
        finish();
    }
    CHECK(records == 1, "slow read remains recorded");
    if (argc > 1) {
        printf("usb: %s\n", record);
        return failures != 0;
    }
    CHECK(strstr(record, "entry -30000, wake 1000, read 81000") != 0, "diagnostic times");
    CHECK(strstr(record, "tracked 1, dispatch 1, pumps 0, work 0") != 0, "dispatch freezes counts");
    usb_stamp_read();
    CHECK(records == 1, "read consumes the record");
    begin(1);
    usb_stamp_dispatch(12);
    usb_stamp_work(256);
    CHECK(!stamp.dispatched && stamp.work == 256, "another process cannot freeze counts");
    usb_stamp_work(0xffffffffu);
    CHECK(stamp.work == 0xffffffffu, "work count saturates");
    usb_stamp_init(0);
    usb_stamp_pump(1, 2, 1, 1);
    usb_stamp_wake();
    usb_stamp_read();
    CHECK(records == 0, "disabled recorder is silent");
    begin(1);
    usb_stamp_pump(1, 2, 0, 0);
    usb_stamp_wake();
    usb_stamp_read();
    CHECK(records == 0, "a pump without an event invalidates the record");
    begin(1);
    schedule();
    usb_stamp_pump(50000, 51000, 0, 1);
    clock_us = 52000;
    usb_stamp_wake();
    clock_us = 112000;
    irq_flags = 0x200;
    usb_stamp_read();
    CHECK(records == 1 && strstr(record, "msi 1, entry -30000, wake 1000, read 81000") != 0 &&
              strstr(record, "tracked 1, dispatch 1") != 0,
          "a code after the dispatch leaves the keystroke's record");
    if (!failures) {
        printf("usbstamp-test: result: pass\n");
    }
    return failures != 0;
}
