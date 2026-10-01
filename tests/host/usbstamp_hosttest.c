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
process_t *current_proc;
process_t *proc_idle;
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
uint32_t pit_now_us(void)
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
void ptrace_switch_debug(const process_t *old, const process_t *next)
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
