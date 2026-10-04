/**
 * Drive the production USB recorder and scheduler with explicit event order.
 *
 * host-deps:
 *
 * The clock, interrupt mask and CPU switch are substituted. An optional
 * argument emits one record for the USB suite's own AWK checker.
 */
#include <stdarg.h>

#include "usb/stamp.c"
#include "proc/sched.c"

extern int printf(const char *format, ...);
extern int dprintf(int fd, const char *format, ...);
extern int vsnprintf(char *out, unsigned long size, const char *format, va_list args);
extern int strcmp(const char *a, const char *b);
extern char *strstr(const char *text, const char *part);

process_t *proc_list;
struct percpu cpu_table[NR_CPUS];
process_t *proc_idle;
volatile uint32_t kernel_ticks;
static process_t processes[2];
static uint32_t clock_us, irq_flags;
static unsigned records;
static char record[512];
static int failures;

#define CHECK(condition, detail)                               \
    do {                                                       \
        if (!(condition)) {                                    \
            dprintf(2, "usbstamp-test: failed: %s\n", detail); \
            failures++;                                        \
        }                                                      \
    } while (0)

/** Substitute an explicit clock; the test does not wait. */
uint32_t timer_now_us(void)
{
    return clock_us;
}

/** Model interrupt masking around the diagnostic snapshot. */
uint32_t kirq_save(void)
{
    uint32_t flags = irq_flags;

    irq_flags = 0;
    return flags;
}

/** Restore the saved mask. */
void kirq_restore(uint32_t flags)
{
    irq_flags = flags;
}

/** No row waits for an interrupt. */
void arch_idle(void)
{}

void arch_idle_masked(void)
{}

/** Capture the production record exactly as the suite receives it. */
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

/** One processor: the run queues' lock is the interrupt mask alone. */
uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    (void)lock;
    return kirq_save();
}

void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    (void)lock;
    kirq_restore(flags);
}

void spin_lock(struct spinlock *lock)
{
    (void)lock;
}

void spin_unlock(struct spinlock *lock)
{
    (void)lock;
}

/** Nor a big lock to let go around an idle wait, or another processor to
 * wake. */
int bkl_drop(void)
{
    return 0;
}

void bkl_take(int depth)
{
    (void)depth;
}

void smp_kick(int cpu)
{
    (void)cpu;
}

/** The idle task's loop is never run here. */
void arch_irq_disable(void)
{}

/** No process here arms a timer of processor time, which posts a signal. */
void signal_post(process_t *target, int signo)
{
    (void)target;
    (void)signo;
}

/** No suspend to idle holds the processes here. */
int suspend_lets_run(const struct process *p)
{
    (void)p;
    return 1;
}

/** Nor a POSIX timer on a clock of processor time. */
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

/** No debug register is set in the host switch model. */
void arch_debug_switch(const struct process *old, const struct process *next)
{
    (void)old;
    (void)next;
}

/** Check publication while leaving the host on its own stack. */
void switch_context(process_t *old, process_t *next)
{
    (void)old;
    CHECK(!irq_flags && current_proc == next, "dispatch is atomic");
}

/** These rows have no loopback packets. */
int ip_loop_pending(void)
{
    return 0;
}

/** These rows have no loopback packets. */
void ip_loop_drain_from(int from)
{
    (void)from;
}

/** Arm one blocked reader beside one running process. */
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
    processes[1].state = PROC_READY;
    processes[1].woken = 1;
    records = 0;
    record[0] = '\0';
}

/** Dispatch through the actual scheduler, then delay the terminal's read. */
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

/** Emit slow and faulty records for the same checker used by the guest rows. */
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
    if (!failures) {
        printf("usbstamp-test: result: pass\n");
    }
    return failures != 0;
}
