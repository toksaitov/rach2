/*
 * watchdog.c: the soft-lockup, hard-lockup and hung-task detectors, as
 * Linux 6.12's kernel/watchdog.c, watchdog_perf.c, watchdog_buddy.c,
 * hung_task.c and lib/nmi_backtrace.c have them, and the debug shell's test
 * of each, as lib/test_lockup.c.
 */
#include "watchdog.h"

#include "cmdline.h"
#include "fs/debugfs.h"
#include "klog.h"
#include "lib/kirq.h"
#include "lib/kstd.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"
#include "mm/usercopy.h"
#include "panic.h"
#include "platform.h"
#include "proc/percpu.h"
#include "proc/process.h"
#include "proc/sched.h"
#include "proc/smp.h"
#include "proc/wait_queue.h"
#include "syscall.h"
#include "timer.h"
#include "udelay.h"

/* Linux's PID_MAX_LIMIT (include/linux/threads.h:34-35), the default of
 * hung_task_check_count (kernel/hung_task.c:30). */
#if defined(__x86_64__)
#define HUNG_TASK_CHECK_COUNT 4194304
#else
#define HUNG_TASK_CHECK_COUNT 32768
#endif

/* The settings and their defaults, kernel/watchdog.c:45-59 and :303-310,
 * and kernel/hung_task.c:28-45; the hard-lockup detector is on by default,
 * as WATCHDOG_HARDLOCKUP_DEFAULT is where Linux has one. */
int watchdog_user_enabled = 1;
int watchdog_softlockup_user_enabled = 1;
int watchdog_hardlockup_user_enabled = 1;
int watchdog_thresh = 10;
int softlockup_panic;
int hardlockup_panic;
int sysctl_softlockup_all_cpu_backtrace;
int sysctl_hardlockup_all_cpu_backtrace;
int sysctl_hung_task_all_cpu_backtrace;
int sysctl_hung_task_panic;
int sysctl_hung_task_check_count = HUNG_TASK_CHECK_COUNT;
int sysctl_hung_task_timeout_secs = 120;
int sysctl_hung_task_check_interval_secs;
int sysctl_hung_task_warnings = 10;

/* Linux's NUM_SAMPLE_PERIODS (kernel/watchdog.c:43): the watchdog's timer
 * fires five times in a soft-lockup threshold. */
#define NUM_SAMPLE_PERIODS 5

/* A report timestamp that delays the next soft-lockup report by a period,
 * SOFTLOCKUP_DELAY_REPORT (kernel/watchdog.c:300). */
#define DELAY_REPORT 0xFFFFFFFFu

/* The detectors watchdog_enabled says are on (kernel/watchdog.c:44). */
#define WD_SOFT 1u
#define WD_HARD 2u

/* Each processor's state, Linux's per-cpu variables of the same names:
 * the watchdog timer's count, `hrtimer_interrupts`, and the count the last
 * hard-lockup check saw; a touch that skips the next check and whether this
 * lockup was reported; the ticks since the timer last fired and its TSC
 * then; whether the
 * soft-lockup function is queued (its completion not done); the timestamps
 * of the last schedule and the last report, in seconds of uptime; the
 * period the counter was armed with, 0 for none; whether the processor's
 * watchdog is enabled, which the buddy check reads; whether its first tick
 * since `watchdog_init` armed it; the last NMI's TSC
 * and the NMIs it filtered (kernel/watchdog_perf.c:58); and, for the
 * buddy check from a wait with interrupts off (`watchdog_wait`), the TSC of
 * the last tick, of the wait's last sample period and of the last buddy
 * check, the wait's turns and sample periods, and whether the wait's check
 * is under way; and the program counter the last NMI interrupted and
 * whether one NMI at it handled more than one source, Linux's
 * `last_nmi_rip` and `swallow_nmi` (arch/x86/kernel/nmi.c); and the
 * value of `wd_gen` its detectors last started at. */
struct wd_cpu {
    volatile unsigned int hrtimer_interrupts;
    unsigned int hrtimer_interrupts_saved;
    volatile int hardlockup_touched;
    int hardlockup_warned;
    uint32_t ticks;
    uint64_t timer_tsc;
    volatile int kicked;
    volatile uint32_t touch_ts;
    volatile uint32_t report_ts;
    uint64_t armed;
    volatile int enabled;
    uint64_t last_tsc;
    int nmi_rearmed;
    int primed;
    uint64_t tick_tsc;
    uint64_t wait_tsc;
    uint64_t check_tsc;
    unsigned int wait_turns;
    unsigned int wait_periods;
    int in_wait;
    uintptr_t last_nmi_pc;
    int swallow_nmi;
    unsigned int gen;
};

static struct wd_cpu wd[NR_CPUS];

/* Whether `watchdog_init` has run, and whether the counter raises the NMI;
 * without it the buddy check is the hard-lockup detector. */
static volatile int wd_ready;
static int wd_perf;

/* Moved by each write that changes `watchdog`, `soft_watchdog`,
 * `nmi_watchdog` or `watchdog_thresh` (`watchdog_reconfigure`); each
 * processor restarts its detectors when its `gen` differs. */
static volatile unsigned int wd_gen;

/* The guards of kernel/watchdog.c:111 and :321, which keep one processor at
 * a time dumping every processor's trace; and lib/nmi_backtrace.c's flag
 * and mask of the processors yet to answer a backtrace request. */
static volatile unsigned int hard_lockup_nmi_warn;
static volatile unsigned int soft_lockup_nmi_warn;
static volatile unsigned int backtrace_flag;
static volatile unsigned int backtrace_mask[NR_CPUS];

/* The processor that holds the report's serialization, as its number plus
 * one, and its depth: printk_cpu_sync_get_irqsave's lock, which a processor
 * may take again inside an NMI. */
static volatile unsigned int sync_owner;
static int sync_depth;

/** The detectors the switches ask for, Linux's `watchdog_enabled` as
 * `lockup_detector_update_enable` sets it (kernel/watchdog.c:283-292):
 * none while `watchdog` is 0, the soft one unless `soft_watchdog` is 0, the
 * hard one unless `nmi_watchdog` is 0 or it cannot run here. */
static unsigned int watchdog_switches(void)
{
    unsigned int e = 0;

    if (!watchdog_user_enabled) {
        return 0;
    }
    if (watchdog_softlockup_user_enabled) {
        e |= WD_SOFT;
    }
    if (watchdog_hardlockup_user_enabled && watchdog_hardlockup_available()) {
        e |= WD_HARD;
    }
    return e;
}

/** The detectors the settings ask for: the switches' while the threshold
 * is above 0, none at 0. */
static unsigned int watchdog_wanted(void)
{
    return watchdog_thresh > 0 ? watchdog_switches() : 0;
}

int watchdog_sysctl_value(int which)
{
    unsigned int bits = which == 0 ? WD_SOFT | WD_HARD : which == 1 ? WD_SOFT : WD_HARD;

    return (watchdog_switches() & bits) != 0;
}

/** The detectors on now: those `watchdog_wanted` gives, none before
 * `watchdog_init` has set every processor's record up. */
static unsigned int watchdog_enabled(void)
{
    return wd_ready ? watchdog_wanted() : 0;
}

int watchdog_hardlockup_available(void)
{
    return wd_perf || smp_cpu_count() > 1;
}

/** `n` / `d`, by shift and subtract, since the i686 has no 64-bit divide
 * without libgcc; `d` is not 0. */
static uint64_t div_u64(uint64_t n, uint64_t d)
{
    uint64_t q = 0, rem = 0;
    int bit;

    for (bit = 63; bit >= 0; bit--) {
        rem = (rem << 1) | ((n >> bit) & 1u);
        if (rem >= d) {
            rem -= d;
            q |= (uint64_t)1 << bit;
        }
    }
    return q;
}

/**
 * Seconds of this processor's clock, Linux's `get_timestamp`
 * (kernel/watchdog.c:549), which reads the local `running_clock`: the TSC
 * at the rate `platform_pmu_probe` measured, which runs while the kernel's tick
 * count, which only the holder of the big lock advances, stands still; or
 * that count where the rate is unknown.
 */
static uint32_t get_timestamp(void)
{
    uint64_t hz = platform_cpu_hz();

    if (hz == 0) {
        return (uint32_t)tick_div(tick_count(), TICK_HZ, (uint32_t *)0);
    }
    return (uint32_t)div_u64(arch_cycles(), hz);
}

/** The soft-lockup threshold, twice the hard one (kernel/watchdog.c:540). */
static uint32_t get_softlockup_thresh(void)
{
    return (uint32_t)watchdog_thresh * 2u;
}

/** The ticks of a sample period, `sample_period` (kernel/watchdog.c:564). */
static uint32_t sample_ticks(void)
{
    return get_softlockup_thresh() * TICK_HZ / NUM_SAMPLE_PERIODS;
}

/** Note now as the time of this processor's last soft-lockup report. */
static void update_report_ts(struct wd_cpu *w)
{
    w->report_ts = get_timestamp();
}

/** Note now as the time this processor last scheduled, and its last
 * report's (kernel/watchdog.c:574). */
static void update_touch_ts(struct wd_cpu *w)
{
    w->touch_ts = get_timestamp();
    update_report_ts(w);
}

void touch_softlockup_watchdog(void)
{
    wd[arch_cpu_id()].report_ts = DELAY_REPORT;
}

void touch_nmi_watchdog(void)
{
    wd[arch_cpu_id()].hardlockup_touched = 1;
    touch_softlockup_watchdog();
}

/** Take the reports' serialization with interrupts off, and return the
 * flags `sync_put` restores, waiting at most a second for another
 * processor that holds it, since that one may be the processor locked up.
 * klog builds one record at a time and the detectors run outside the big
 * lock that serializes its other callers, so every report of theirs is
 * logged under it; with interrupts on, a tick could switch the holder to
 * another processor, whose `sync_put` would then not release it, as
 * printk_cpu_sync_get_irqsave turns them off. */
static uint32_t sync_get(void)
{
    uint32_t flags = kirq_save();
    unsigned int me = arch_cpu_id() + 1u;
    uint32_t waited;

    if (sync_owner == me) {
        sync_depth++;
        return flags;
    }
    for (waited = 0; arch_atomic_cmpxchg(&sync_owner, 0u, me) != 0u && waited < 100000u; waited++) {
        k_udelay(10);
    }
    sync_owner = me;
    sync_depth = 1;
    return flags;
}

/** Release the reports' serialization taken by `sync_get`, at its last
 * depth, and put the interrupt flag back as `flags` has it. */
static void sync_put(uint32_t flags)
{
    if (sync_owner == arch_cpu_id() + 1u && --sync_depth == 0) {
        sync_owner = 0;
    }
    kirq_restore(flags);
}

/** Copy `t`'s name into `out`, sixteen bytes, each byte not printable as a
 * question mark: a user names its own threads, and a name with an escape
 * sequence would act on the screen the report is written to. */
static void task_name(const process_t *t, char out[16])
{
    int i;

    for (i = 0; i < 15 && t && t->comm[i]; i++) {
        out[i] = t->comm[i] >= 0x20 && t->comm[i] < 0x7F ? t->comm[i] : '?';
    }
    out[i] = '\0';
}

/* The most frames a trace gives. */
#define TRACE_MAX 32

/**
 * Copy into `out`, TRACE_MAX long, the return address of each frame from
 * the frame pointer `fp` up, reading only the KERNEL_STACK_SIZE bytes below
 * `top`, and only upward. The number copied, 0 for a `top` of 0.
 */
static int trace_walk(uintptr_t fp, uintptr_t top, uintptr_t *out)
{
    uintptr_t low = fp, ret;
    int n;

    for (n = 0; n < TRACE_MAX; n++) {
        if (top < KERNEL_STACK_SIZE || fp < top - KERNEL_STACK_SIZE ||
            fp > top - 2 * sizeof(uintptr_t) || fp % sizeof(uintptr_t) != 0 || fp < low) {
            break;
        }
        ret = ((const uintptr_t *)fp)[1];
        if (ret == 0) {
            break;
        }
        out[n] = ret;
        low = fp + 2 * sizeof(uintptr_t);
        fp = ((const uintptr_t *)fp)[0];
    }
    return n;
}

/** Log "Call Trace:" and the `n` return addresses in `frames`. */
static void trace_log(const char *subsystem, int level, const uintptr_t *frames, int n)
{
    int i;

    klog(level, subsystem, "Call Trace:");
    for (i = 0; i < n; i++) {
        klog(level, subsystem, " 0x%lx", (unsigned long)frames[i]);
    }
}

void watchdog_show_trace(const char *subsystem, int level, uintptr_t fp, uintptr_t top)
{
    uintptr_t frames[TRACE_MAX];
    int n = trace_walk(fp, top, frames);

    trace_log(subsystem, level, frames, n);
}

/** Linux's `show_regs`: the processor, the running thread and the frame
 * `regs` interrupted, or, without one, the caller's own stack, as
 * `dump_stack`. */
static void show_regs(const char *subsystem, int level, const isr_params_t *regs)
{
    process_t *t = current_proc;
    char name[16];

    task_name(t, name);
    klog(level, subsystem, "CPU: %d PID: %d Comm: %s Not tainted %s", (int)arch_cpu_id(),
         t ? (int)t->pid : 0, name, UTS_RELEASE);
    if (regs) {
        platform_show_regs(subsystem, level, regs);
    } else {
        platform_show_stack(subsystem, level);
    }
}

/**
 * Answer a backtrace request in an NMI or for this processor itself,
 * lib/nmi_backtrace.c:91: a processor the NMI found halted in its idle wait
 * is named and skipped, as Linux skips one in its idle loop. 1 when this
 * processor was asked.
 */
static int nmi_cpu_backtrace(const isr_params_t *regs)
{
    int cpu = (int)arch_cpu_id();
    uint32_t sflags;

    if (!backtrace_mask[cpu]) {
        return 0;
    }
    sflags = sync_get();
    if (regs && platform_frame_idle(regs)) {
        klog(KLOG_WARN, "nmi_backtrace", "NMI backtrace for cpu %d skipped: idling at 0x%lx", cpu,
             (unsigned long)FRAME_PC(regs));
    } else {
        klog(KLOG_WARN, "nmi_backtrace", "NMI backtrace for cpu %d", cpu);
        show_regs("nmi_backtrace", KLOG_WARN, regs);
    }
    sync_put(sflags);
    backtrace_mask[cpu] = 0;
    return 1;
}

/** Append the decimal `n` to `out` at `*at`, within `size` bytes. */
static void put_number(char *out, unsigned int size, unsigned int *at, int n)
{
    char digits[4];
    int k = 0;

    do {
        digits[k++] = (char)('0' + n % 10);
        n /= 10;
    } while (n && k < 4);
    while (k > 0 && *at + 1 < size) {
        out[(*at)++] = digits[--k];
    }
    out[*at] = '\0';
}

/** The processors of `backtrace_mask` as a list of ranges, "0-2,5", as
 * Linux's `%*pbl` prints a cpumask. */
static void mask_list(char *out, unsigned int size)
{
    unsigned int at = 0;
    int c, end;

    out[0] = '\0';
    for (c = 0; c < NR_CPUS; c++) {
        if (!backtrace_mask[c]) {
            continue;
        }
        end = c;
        while (end + 1 < NR_CPUS && backtrace_mask[end + 1]) {
            end++;
        }
        if (at != 0 && at + 1 < size) {
            out[at++] = ',';
        }
        put_number(out, size, &at, c);
        if (end != c) {
            if (at + 1 < size) {
                out[at++] = '-';
            }
            put_number(out, size, &at, end);
        }
        c = end;
    }
}

/**
 * Have the processors `want` says, a bit per processor (`want[c]`), but
 * `exclude` (-1 for none), log their traces, this one from `regs`, which
 * may be NULL, and the others from an NMI each; and wait up to ten seconds
 * for them, as lib/nmi_backtrace.c:35 does. A request while another is
 * under way does nothing.
 */
static void trigger_backtrace(const int want[NR_CPUS], int exclude, const isr_params_t *regs)
{
    int me = (int)arch_cpu_id(), c, left, i;
    char list[96];
    uint32_t sflags;

    if (arch_atomic_xchg(&backtrace_flag, 1u) != 0u) {
        return;
    }
    for (c = 0; c < NR_CPUS; c++) {
        backtrace_mask[c] = want[c] && c != exclude && cpu_table[c].online ? 1u : 0u;
    }
    if (backtrace_mask[me]) {
        nmi_cpu_backtrace(regs);
    }
    left = 0;
    for (c = 0; c < NR_CPUS; c++) {
        if (backtrace_mask[c]) {
            left++;
        }
    }
    if (left) {
        mask_list(list, sizeof(list));
        sflags = sync_get();
        klog(KLOG_INFO, "nmi_backtrace", "sending NMI from CPU %d to CPUs %s:", me, list);
        sync_put(sflags);
        for (c = 0; c < NR_CPUS; c++) {
            if (backtrace_mask[c]) {
                platform_cpu_nmi(c);
            }
        }
    }
    for (i = 0; i < 10 * 1000 && left; i++) {
        left = 0;
        for (c = 0; c < NR_CPUS; c++) {
            if (backtrace_mask[c]) {
                left++;
            }
        }
        if (left) {
            k_udelay(1000);
            touch_softlockup_watchdog();
        }
    }
    for (c = 0; c < NR_CPUS; c++) {
        backtrace_mask[c] = 0;
    }
    backtrace_flag = 0;
}

/** The trace of processor `cpu`, Linux's trigger_single_cpu_backtrace. */
static void trigger_one(int cpu, const isr_params_t *regs)
{
    int want[NR_CPUS];

    k_memset(want, 0, sizeof(want));
    want[cpu] = 1;
    trigger_backtrace(want, -1, regs);
}

/** The traces of every processor but `exclude`, Linux's
 * trigger_allbutcpu_cpu_backtrace. */
static void trigger_allbut(int exclude, const isr_params_t *regs)
{
    int want[NR_CPUS], c;

    for (c = 0; c < NR_CPUS; c++) {
        want[c] = 1;
    }
    trigger_backtrace(want, exclude, regs);
}

/**
 * kernel/watchdog.c:131-221: a processor whose watchdog timer has not fired
 * since the last check has had its interrupts off for a threshold. `cpu` is
 * the processor checked, this one from its counter's NMI with `regs`, or
 * the buddy from a tick, without.
 */
static void watchdog_hardlockup_check(int cpu, const isr_params_t *regs)
{
    struct wd_cpu *w = &wd[cpu];
    unsigned int hrint = w->hrtimer_interrupts;
    int me = (int)arch_cpu_id(), panicking = hardlockup_panic != 0;
    uint32_t sflags;

    if (w->hardlockup_touched) {
        w->hardlockup_touched = 0;
        return;
    }
    if (w->hrtimer_interrupts_saved != hrint) {
        w->hrtimer_interrupts_saved = hrint;
        w->hardlockup_warned = 0;
        return;
    }
    if (w->hardlockup_warned) {
        return;
    }
    if (sysctl_hardlockup_all_cpu_backtrace && arch_atomic_xchg(&hard_lockup_nmi_warn, 1u) != 0u) {
        return;
    }
    sflags = sync_get();
    klog(KLOG_CRIT, "watchdog", "watchdog detected hard LOCKUP on cpu %d", cpu);
    if (cpu == me) {
        show_regs("watchdog", KLOG_ERR, regs);
    }
    sync_put(sflags);
    if (cpu != me) {
        trigger_one(cpu, NULL);
    }
    if (sysctl_hardlockup_all_cpu_backtrace) {
        trigger_allbut(cpu, NULL);
        if (!panicking) {
            hard_lockup_nmi_warn = 0;
        }
    }
    if (panicking) {
        k_panic("Hard LOCKUP");
    }
    w->hardlockup_warned = 1;
}

/** The next processor after `cpu` whose watchdog is enabled, wrapping, or
 * -1 when there is none but `cpu` (kernel/watchdog_buddy.c:11). */
static int watchdog_next_cpu(int cpu)
{
    int i, c;

    for (i = 1; i < NR_CPUS; i++) {
        c = (cpu + i) % NR_CPUS;
        if (wd[c].enabled && cpu_table[c].online) {
            return c;
        }
    }
    return -1;
}

/** The TSC's counts in a sample period, 0 while the clock is unknown. */
static uint64_t sample_cycles(void)
{
    return tick_div(platform_cpu_hz() * sample_ticks(), TICK_HZ, (uint32_t *)0);
}

/** The buddy check of the next processor (kernel/watchdog_buddy.c:88), from
 * this processor's timer or its wait. A check sooner than two sample periods
 * after the last, as one from a wait just before the timer came back could
 * be, is skipped, since the next processor's count need not have moved. */
static void buddy_check(struct wd_cpu *w)
{
    uint64_t now = arch_cycles();
    int next;

    if (now - w->check_tsc < 2u * sample_cycles()) {
        return;
    }
    w->check_tsc = now;
    next = watchdog_next_cpu((int)arch_cpu_id());
    if (next >= 0) {
        watchdog_hardlockup_check(next, NULL);
    }
}

/** watchdog_hardlockup_kick and, without a counter, the buddy check every
 * third timer, kernel/watchdog.c:148 and watchdog_buddy.c:88. */
static void watchdog_hardlockup_kick(struct wd_cpu *w, unsigned int e)
{
    unsigned int n = ++w->hrtimer_interrupts;

    if (wd_perf || !(e & WD_HARD) || n % 3u != 0) {
        return;
    }
    buddy_check(w);
}

/**
 * Arm this processor's counter for a period of `watchdog_thresh` seconds of
 * cycles, `hw_nmi_get_sample_period`, or disarm it, when that changed. The
 * processor is touched, so that its counter's next check reads no count
 * from before.
 */
static void watchdog_hardlockup_arm(struct wd_cpu *w, unsigned int e)
{
    uint64_t want = 0;

    if (wd_perf && (e & WD_HARD)) {
        want = platform_cpu_hz() * (uint64_t)watchdog_thresh;
    }
    if (want == w->armed) {
        return;
    }
    w->hardlockup_touched = 1;
    if (want) {
        platform_pmu_enable(want);
    } else {
        platform_pmu_disable();
    }
    w->armed = want;
}

/** Enable this processor's detectors, if they were off, and arm or disarm
 * its counter for the detectors `e` (kernel/watchdog.c, `watchdog_enable`):
 * the soft-lockup timestamp is touched and the next hard-lockup check of
 * this processor and of the one its buddy check reads skipped, so that
 * neither reads a time or a count from before (kernel/watchdog_buddy.c:43
 * and :52). */
static void watchdog_enable(struct wd_cpu *w, unsigned int e)
{
    int next;

    if (e && !w->enabled) {
        update_touch_ts(w);
        w->hardlockup_touched = 1;
        next = watchdog_next_cpu((int)arch_cpu_id());
        if (next >= 0) {
            wd[next].hardlockup_touched = 1;
        }
        w->enabled = 1;
    }
    watchdog_hardlockup_arm(w, e);
}

/**
 * kernel/watchdog.c:675-775, `watchdog_timer_fn`, each sample period: kick
 * the hard-lockup detector, queue the soft-lockup function, which the next
 * schedule runs (`watchdog_sched`), and report a soft lockup when the last
 * one ran more than 2 x `watchdog_thresh` seconds ago.
 *
 * The switches and the threshold are read first, and the timer returns
 * when a write has moved `wd_gen` since its tick checked it: a write moves
 * it before it stores a new value (procfs.c's `sysctl_int_write`), so a
 * timer that read the new value sees the move, and no timer compares a
 * new threshold with timestamps from before it, as Linux stores the
 * threshold only once every timer has stopped (kernel/watchdog.c:867-880).
 */
static void watchdog_timer_fn(struct wd_cpu *w, isr_params_t *regs)
{
    unsigned int e = watchdog_enabled();
    uint32_t thresh = get_softlockup_thresh();
    uint32_t now, period_ts, duration, sflags;
    int me = (int)arch_cpu_id(), panicking;
    process_t *t;
    char name[16];

    k_barrier();
    if (w->gen != wd_gen) {
        return;
    }
    if (!e) {
        if (w->enabled) {
            w->enabled = 0;
            watchdog_hardlockup_arm(w, 0);
        }
        return;
    }
    watchdog_enable(w, e);
    watchdog_hardlockup_kick(w, e);
    w->kicked = 1;

    now = get_timestamp();
    period_ts = w->report_ts;
    if (period_ts == DELAY_REPORT) {
        update_report_ts(w);
        return;
    }
    if (!(e & WD_SOFT) || now - period_ts <= thresh || (int32_t)(now - period_ts) < 0) {
        return;
    }
    duration = now - w->touch_ts;
    if (sysctl_softlockup_all_cpu_backtrace && arch_atomic_xchg(&soft_lockup_nmi_warn, 1u) != 0u) {
        return;
    }
    update_report_ts(w);
    panicking = softlockup_panic != 0;
    t = current_proc;
    task_name(t, name);
    sflags = sync_get();
    klog(KLOG_CRIT, "watchdog", "BUG: soft lockup - CPU#%d stuck for %us! [%s:%d]", me,
         (unsigned int)duration, name, t ? (int)t->pid : 0);
    show_regs("watchdog", KLOG_ERR, regs);
    sync_put(sflags);
    if (sysctl_softlockup_all_cpu_backtrace) {
        trigger_allbut(me, NULL);
        if (!panicking) {
            soft_lockup_nmi_warn = 0;
        }
    }
    if (panicking) {
        k_panic("softlockup: hung tasks");
    }
}

/** softlockup_fn (kernel/watchdog.c:665): touch, once a timer queued it. */
static void softlockup_fn(struct wd_cpu *w)
{
    if (w->kicked) {
        w->kicked = 0;
        update_touch_ts(w);
    }
}

void watchdog_sched(void)
{
    softlockup_fn(&wd[arch_cpu_id()]);
}

/**
 * Write out klog's kept records when the processor that holds the big lock,
 * other than this one, has been reported hard-locked and has taken no tick
 * for a sample period since: the reports of the detectors, made in an NMI
 * or without the big lock, wait for a processor that holds it
 * (`klog_flush`), and that one is locked up. One that took a tick has come
 * back, and writes them out itself. A tick runs it, and so does each
 * sample period of a wait for the big lock (`watchdog_wait`), since under
 * load every other processor soon waits there with interrupts off and
 * takes no tick. Linux has no lock that every waiter spins on: its buddy
 * report runs in a processor that still takes interrupts, and printk's
 * irq_work writes it out there (kernel/printk/printk.c:4524-4583).
 */
static void flush_for_holder(int me)
{
    uint64_t now = arch_cycles(), cycles = sample_cycles();
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (c != me && cpu_table[c].bkl_depth > 0 && wd[c].hardlockup_warned &&
            (cycles == 0 || (int64_t)(now - wd[c].tick_tsc) > (int64_t)cycles)) {
            klog_flush_stuck();
            return;
        }
    }
}

/** The barrier keeps the store of a new setting after the move, which a
 * timer that reads the setting then sees (`watchdog_timer_fn`). */
void watchdog_reconfigure(void)
{
    wd_gen++;
    k_barrier();
}

/**
 * Stop this processor's detectors and start them again with the settings
 * now, once a write has changed them, as Linux's
 * `__lockup_detector_reconfigure` stops every timer and counter and starts
 * them through `watchdog_enable` (kernel/watchdog.c:875-878): the timer
 * starts a new sample period, and the timestamps are touched, so that no
 * check reads a time from before the write, while the detector was off or
 * its threshold longer.
 */
static void watchdog_restart(struct wd_cpu *w)
{
    w->gen = wd_gen;
    w->enabled = 0;
    watchdog_hardlockup_arm(w, 0);
    w->kicked = 0;
    w->ticks = 0;
    w->timer_tsc = w->tick_tsc;
    watchdog_enable(w, watchdog_enabled());
}

/**
 * A tick that finds user mode, or the processor halted in an idle wait,
 * runs the soft-lockup function: Linux's stopper thread would have
 * preempted the one and been chosen by the other, and here the tick that
 * finds another processor holding the big lock does not schedule at all.
 * The first tick after `watchdog_init` arms the processor's counter, so
 * that a lockup within the first sample period is caught, and the first
 * after a write of the settings restarts the detectors (`watchdog_restart`),
 * whether or not they are on now. The timer fires
 * after a sample period of ticks or of the TSC, whichever comes first, so
 * that the first tick after a wait with interrupts off fires it at once, as
 * Linux's hrtimer fires as soon as interrupts let it, in the context that
 * held them off.
 */
void watchdog_tick(isr_params_t *params)
{
    struct wd_cpu *w = &wd[arch_cpu_id()];
    uint32_t period = sample_ticks();
    uint64_t cycles = sample_cycles();

    if (!wd_ready) {
        return;
    }
    w->tick_tsc = arch_cycles();
    if (!w->primed) {
        w->primed = 1;
        watchdog_enable(w, watchdog_enabled());
    }
    if (w->gen != wd_gen) {
        watchdog_restart(w);
    }
    if (klog_kept()) {
        flush_for_holder((int)arch_cpu_id());
    }
    if (FRAME_USER(params) || this_cpu()->idle_wait) {
        softlockup_fn(w);
    }
    if (period == 0 ||
        (++w->ticks < period && (cycles == 0 || w->tick_tsc - w->timer_tsc < cycles))) {
        return;
    }
    w->ticks = 0;
    w->timer_tsc = w->tick_tsc;
    watchdog_timer_fn(w, params);
}

/**
 * A processor that waits with interrupts off, for the big lock, a spinlock
 * or another processor's answer, takes no ticks. Every processor that wants
 * the big lock waits so while another holds it, the idle ones too, which
 * take it at each tick to schedule; Linux has no such lock. A waiter for
 * the big lock is not locked up by its own fault, so each sample period of
 * its wait counts as its timer, which moves its count and runs the buddy
 * check every third, and only the holder is reported: by its own counter's
 * NMI, or by the buddy check that goes on from the waiter before it; the
 * wait touches its soft-lockup timestamp too, for the same reason. Any
 * other wait, for a spinlock or an answer, leaves its count still, so that
 * its buddy reports it, as Linux's detector reports a processor that spins
 * with interrupts off, and two that wait on each other are both reported;
 * without a counter, each third sample period of the wait runs its own
 * buddy check. Every 64th turn reads the TSC, and a period counts only
 * once the processor has gone a period without a tick.
 */
void watchdog_wait(void)
{
    struct wd_cpu *w;
    uint64_t now, period;
    unsigned int e;

    if (!wd_ready) {
        return;
    }
    w = &wd[arch_cpu_id()];
    if ((++w->wait_turns & 63u) != 0 || w->in_wait || k_panicking()) {
        return;
    }
    period = sample_cycles();
    now = arch_cycles();
    if (period == 0 || now - w->tick_tsc < period || now - w->wait_tsc < period) {
        return;
    }
    if (w->gen != wd_gen) {
        w->in_wait = 1;
        watchdog_restart(w);
        w->wait_tsc = now;
        w->in_wait = 0;
        return;
    }
    e = watchdog_enabled();
    if (!e) {
        return;
    }
    w->in_wait = 1;
    w->wait_tsc = now;
    watchdog_enable(w, e);
    if (this_cpu()->wake_bkl_waiting) {
        update_touch_ts(w);
        watchdog_hardlockup_kick(w, e);
        if (klog_kept()) {
            flush_for_holder((int)arch_cpu_id());
        }
    } else if ((e & WD_HARD) && !wd_perf && ++w->wait_periods % 3u == 0) {
        buddy_check(w);
    }
    w->in_wait = 0;
}

/**
 * kernel/watchdog_perf.c:58: an NMI sooner than 4/5 of the threshold after
 * the last, as turbo cycles bring one, is skipped, ten in a row at most.
 * The time is the TSC's, which an NMI may read.
 */
static int watchdog_check_timestamp(struct wd_cpu *w)
{
    uint64_t now = arch_cycles();
    uint64_t least =
        tick_div(platform_cpu_hz() * (uint64_t)watchdog_thresh * 4u, 5u, (uint32_t *)0);

    if (now - w->last_tsc < least && ++w->nmi_rearmed < 10) {
        return 0;
    }
    w->nmi_rearmed = 0;
    w->last_tsc = now;
    return 1;
}

int watchdog_nmi(isr_params_t *params)
{
    int cpu = (int)arch_cpu_id(), handled = 0, due, b2b;
    struct wd_cpu *w = &wd[cpu];
    uintptr_t pc = (uintptr_t)FRAME_PC(params);

    b2b = pc == w->last_nmi_pc;
    if (!b2b) {
        w->swallow_nmi = 0;
    }
    w->last_nmi_pc = pc;
    if (nmi_cpu_backtrace(params)) {
        handled++;
    }
    if (wd_perf && platform_pmu_overflow(&due)) {
        handled++;
        if (due && (watchdog_enabled() & WD_HARD) && watchdog_check_timestamp(w)) {
            watchdog_hardlockup_check(cpu, params);
        }
        platform_pmu_handled();
    }
    if (smp_stop_nmi()) {
        handled++;
    }
    if (handled > 1) {
        w->swallow_nmi = 1;
    }
    if (handled) {
        return 1;
    }
    return (b2b && w->swallow_nmi) || platform_pmu_late();
}

/* ---- the hung-task detector, kernel/hung_task.c ---- */

static wait_queue_t hung_wait;
static volatile uint32_t hung_gen;
static process_t *hung_thread;

void watchdog_sysctl_changed(void)
{
    hung_gen++;
    wq_wake_all(&hung_wait);
}

/* One hung task's report, copied out of its record with interrupts off and
 * logged with them on (`hung_task_log`). */
struct hung_report {
    char name[16];
    int pid, tgid, ppid;
    const char *wchan; /* a static string, or NULL */
    unsigned int secs; /* how long it has been blocked */
    uintptr_t pc;      /* where it blocked, 0 when not known */
    uintptr_t frames[TRACE_MAX];
    int nframes;
    int last; /* the warnings ran out with this report */
};

/**
 * check_hung_task, kernel/hung_task.c:90-149: whether `t`, blocked in a
 * sleep that no signal ends, has not been switched for `timeout` seconds
 * at the tick `now` and is to be reported; when it is, its report is
 * copied into `rep`. Interrupts are off, and `t` is only safe to read
 * while they stay off.
 */
static int check_hung_task(process_t *t, uint32_t timeout, uint64_t now, struct hung_report *rep)
{
    unsigned long switch_count = t->nvcsw + t->nivcsw;
    uintptr_t fp;

    if (switch_count == 0) {
        return 0;
    }
    if (switch_count != t->last_switch_count) {
        t->last_switch_count = switch_count;
        t->last_switch_time = now;
        return 0;
    }
    if (now - t->last_switch_time < (uint64_t)timeout * TICK_HZ) {
        return 0;
    }
    if (!sysctl_hung_task_warnings && !sysctl_hung_task_panic) {
        return 0;
    }
    if (sysctl_hung_task_warnings > 0) {
        sysctl_hung_task_warnings--;
    }
    task_name(t, rep->name);
    rep->pid = (int)t->pid;
    rep->tgid = (int)t->tgid;
    rep->ppid = (int)t->parent_pid;
    rep->wchan = t->wchan;
    rep->secs = (unsigned int)tick_div(now - t->last_switch_time, TICK_HZ, (uint32_t *)0);
    platform_blocked_frame(t, &rep->pc, &fp);
    rep->nframes = trace_walk(fp, t->kernel_stack, rep->frames);
    rep->last = !sysctl_hung_task_warnings;
    return 1;
}

/**
 * Log a hung task's report, as check_hung_task and sched_show_task do
 * (kernel/hung_task.c:131-145), with interrupts on, so that the ticks of
 * this processor go on while its forty records go out; and touch the
 * detectors, as Linux's does.
 */
static void hung_task_log(const struct hung_report *rep)
{
    klog(sysctl_hung_task_panic ? KLOG_CRIT : KLOG_ERR, "hung_task",
         "INFO: task %s:%d blocked for more than %u seconds.", rep->name, rep->pid, rep->secs);
    klog(KLOG_ERR, "hung_task", "      Not tainted %s #1", UTS_RELEASE);
    klog(KLOG_ERR, "hung_task",
         "\"echo 0 > /proc/sys/kernel/hung_task_timeout_secs\" disables this message.");
    klog(KLOG_INFO, "hung_task", "task:%s state:D pid:%d tgid:%d ppid:%d wchan:%s", rep->name,
         rep->pid, rep->tgid, rep->ppid, rep->wchan ? rep->wchan : "0");
    if (rep->pc) {
        klog(KLOG_INFO, "hung_task", " 0x%lx", (unsigned long)rep->pc);
    }
    trace_log("hung_task", KLOG_INFO, rep->frames, rep->nframes);
    if (rep->last) {
        klog(KLOG_INFO, "hung_task",
             "Future hung task reports are suppressed, see sysctl kernel.hung_task_warnings");
    }
    touch_nmi_watchdog();
}

/**
 * check_hung_uninterruptible_tasks, kernel/hung_task.c:181-229: every
 * thread blocked in a sleep no signal ends, but one SIGKILL ends
 * (:206-210).
 *
 * The walk runs with interrupts off. khungtaskd holds the big lock, and
 * the tick does not switch a kernel thread (`no_tick_switch`), so no
 * thread reaps a process and frees the record the walk stands on unless
 * khungtaskd sleeps or reaches a service point. Linux walks under
 * `rcu_read_lock` and breaks it every HUNG_TASK_LOCK_BREAK, a tenth of a
 * second, with references held, ending the walk when the task it stands on
 * has gone (`rcu_lock_break`, :40, :160-174, :201-205). A record here has
 * no count to hold, so the walk keeps the thread's id instead, lets
 * interrupts in after each report, which it logs then, and goes on from
 * the record with that id, or ends when there is none. Without a report it
 * does not break: the tick count stands still while khungtaskd holds the
 * big lock with interrupts off, and a walk of every thread's record is
 * short.
 */
static void check_hung_uninterruptible_tasks(uint32_t timeout)
{
    int max_count = sysctl_hung_task_check_count, reported = 0, want[NR_CPUS], c;
    uint64_t now = tick_count();
    struct hung_report rep;
    process_t *t;
    uint32_t pid, flags = kirq_save();

    for (t = proc_list; t; t = t->next) {
        if (max_count-- <= 0) {
            break;
        }
        if (t->state != PROC_BLOCKED || !t->uninterruptible || t->killable ||
            !check_hung_task(t, timeout, now, &rep)) {
            continue;
        }
        pid = t->pid;
        kirq_restore(flags);
        hung_task_log(&rep);
        reported = 1;
        flags = kirq_save();
        now = tick_count();
        t = get_process(pid);
        if (!t) {
            break;
        }
    }
    kirq_restore(flags);
    if (reported && sysctl_hung_task_all_cpu_backtrace) {
        for (c = 0; c < NR_CPUS; c++) {
            want[c] = 1;
        }
        trigger_backtrace(want, -1, NULL);
    }
    if (reported && sysctl_hung_task_panic) {
        k_panic("hung_task: blocked tasks");
    }
}

/* What khungtaskd's sleep waits for: a setting's write, which moves
 * `hung_gen` past `gen`, or the tick `due`, 0 for none. */
struct hung_wait_arg {
    uint32_t gen;
    uint64_t due;
};

/** Whether khungtaskd's sleep, its `struct hung_wait_arg`, is over. */
static int hung_due(void *arg)
{
    const struct hung_wait_arg *a = (const struct hung_wait_arg *)arg;

    return hung_gen != a->gen || (a->due != 0 && tick_count() >= a->due);
}

/**
 * khungtaskd, kernel/hung_task.c:361: check every
 * `hung_task_check_interval_secs`, or every timeout when that is 0, and
 * sleep while the timeout is 0. A write of either setting wakes it.
 */
static void khungtaskd_main(void)
{
    uint64_t last = tick_count(), due;
    uint32_t timeout, interval, flags;
    struct hung_wait_arg arg;

    arch_irq_enable();
    for (;;) {
        timeout = (uint32_t)sysctl_hung_task_timeout_secs;
        interval = (uint32_t)sysctl_hung_task_check_interval_secs;
        if (interval == 0 || interval > timeout) {
            interval = timeout;
        }
        due = last + (uint64_t)interval * TICK_HZ;
        if (timeout != 0 && tick_count() >= due) {
            check_hung_uninterruptible_tasks(timeout);
            last = tick_count();
            continue;
        }
        flags = kirq_save();
        arg.gen = hung_gen;
        arg.due = timeout != 0 ? due : 0;
        current_proc->wakeup_tick = arg.due;
        kirq_restore(flags);
        (void)wq_wait_event_idle(&hung_wait, hung_due, &arg);
        current_proc->wakeup_tick = 0;
    }
}

/** A kernel thread named `name` that begins at `entry`, ready; NULL when
 * no record or stack can be had. */
static process_t *kthread_start(const char *name, void (*entry)(void))
{
    process_t *p;
    uintptr_t base;
    uint32_t flags = kirq_save();

    base = kstack_alloc();
    p = base ? alloc_process() : NULL;
    if (!p) {
        if (base) {
            kstack_free(base + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        return NULL;
    }
    p->kernel_stack = base + KERNEL_STACK_SIZE;
    p->space_root = 0;
    p->page_dir = NULL;
    p->parent_pid = 0;
    proc_set_comm(p, name);
    setup_process_stack(p, entry);
    sched_make_ready(p);
    kirq_restore(flags);
    return p;
}

/* ---- the boot options ---- */

/** Set `*var` from the number `key=` gives, as Linux's kstrtoint reads a
 * sysctl's, when it lies in [`min`, `max`]; another is refused with a
 * warning, as a write of the file would be. */
static void opt_int(const char *key, int *var, int min, int max)
{
    char text[24];
    long long v = 0;

    if (!kcmdline_value(key, text, sizeof(text))) {
        return;
    }
    if (k_strtoll(text, k_strlen(text), &v) != 0 || v < min || v > max) {
        klog(KLOG_WARN, "watchdog", "boot option: refused: %s=%s", key, text);
        return;
    }
    *var = (int)v;
}

/* The /proc/sys/kernel files the boot line may set as `sysctl.kernel.NAME=`
 * (fs/proc/proc_sysctl.c, `process_sysctl_arg`), with their ranges, and the
 * three Linux also takes by their bare names (`sysctl_aliases`, :1580). */
static const struct {
    const char *name;
    int *var;
    int min;
    int max;
    int alias;
} sysctl_opts[] = {
    {"watchdog", &watchdog_user_enabled, 0, 1, 0},
    {"soft_watchdog", &watchdog_softlockup_user_enabled, 0, 1, 0},
    {"nmi_watchdog", &watchdog_hardlockup_user_enabled, 0, 1, 0},
    {"watchdog_thresh", &watchdog_thresh, 0, 60, 0},
    {"softlockup_panic", &softlockup_panic, 0, 1, 0},
    {"hardlockup_panic", &hardlockup_panic, 0, 1, 0},
    {"softlockup_all_cpu_backtrace", &sysctl_softlockup_all_cpu_backtrace, 0, 1, 1},
    {"hardlockup_all_cpu_backtrace", &sysctl_hardlockup_all_cpu_backtrace, 0, 1, 1},
    {"hung_task_all_cpu_backtrace", &sysctl_hung_task_all_cpu_backtrace, 0, 1, 0},
    {"hung_task_panic", &sysctl_hung_task_panic, 0, 1, 1},
    {"hung_task_check_count", &sysctl_hung_task_check_count, 0, 0x7FFFFFFF, 0},
    {"hung_task_timeout_secs", &sysctl_hung_task_timeout_secs, 0, 0x7FFFFFFF / TICK_HZ, 0},
    {"hung_task_check_interval_secs", &sysctl_hung_task_check_interval_secs, 0,
     0x7FFFFFFF / TICK_HZ, 0},
    {"hung_task_warnings", &sysctl_hung_task_warnings, -1, 0x7FFFFFFF, 0}};

/**
 * nmi_watchdog= takes a list of panic, nopanic, 0 and 1
 * (kernel/watchdog.c:79); every item's first letters decide, as Linux's
 * strncmp does.
 */
static void opt_nmi_watchdog(void)
{
    char text[32];
    char *s = text;

    if (!kcmdline_value("nmi_watchdog", text, sizeof(text))) {
        return;
    }
    while (*s) {
        if (s[0] == 'p' && s[1] == 'a' && s[2] == 'n' && s[3] == 'i' && s[4] == 'c') {
            hardlockup_panic = 1;
        } else if (s[0] == 'n' && s[1] == 'o' && s[2] == 'p' && s[3] == 'a' && s[4] == 'n') {
            hardlockup_panic = 0;
        } else if (s[0] == '0') {
            watchdog_hardlockup_user_enabled = 0;
        } else if (s[0] == '1') {
            watchdog_hardlockup_user_enabled = 1;
        }
        while (*s && *s != ',') {
            s++;
        }
        if (*s == ',') {
            s++;
        }
    }
}

/** The boot options of kernel/watchdog.c:79-349, then the sysctl ones,
 * which Linux applies after them (`do_sysctl_args`). */
static void read_options(void)
{
    char key[64];
    int i;

    opt_nmi_watchdog();
    opt_int("softlockup_panic", &softlockup_panic, 0, 0x7FFFFFFF);
    if (kcmdline_has("nowatchdog")) {
        watchdog_user_enabled = 0;
    }
    if (kcmdline_has("nosoftlockup")) {
        watchdog_softlockup_user_enabled = 0;
    }
    opt_int("watchdog_thresh", &watchdog_thresh, 0, 60);
    for (i = 0; i < (int)(sizeof(sysctl_opts) / sizeof(sysctl_opts[0])); i++) {
        if (sysctl_opts[i].alias) {
            opt_int(sysctl_opts[i].name, sysctl_opts[i].var, sysctl_opts[i].min,
                    sysctl_opts[i].max);
        }
        k_strncpy(key, "sysctl.kernel.", sizeof(key));
        k_strncpy(key + 14, sysctl_opts[i].name, sizeof(key) - 15);
        opt_int(key, sysctl_opts[i].var, sysctl_opts[i].min, sysctl_opts[i].max);
    }
}

/* The hooks of /sys/kernel/debug/test_lockup, below with the test. */
static const struct debugfs_text_ops test_lockup_ops;

void watchdog_init(void)
{
    int c;

    read_options();
    wd_perf = platform_pmu_probe();
    for (c = 0; c < NR_CPUS; c++) {
        if (cpu_table[c].online && watchdog_wanted()) {
            wd[c].touch_ts = get_timestamp();
            wd[c].hardlockup_touched = 1;
            wd[c].enabled = 1;
        }
        wd[c].report_ts = DELAY_REPORT;
    }
    /* The other processors take ticks already: none may read a record
     * before it is set up, as Linux's watchdog_enable sets the timestamps
     * before it starts the timer (kernel/watchdog.c). */
    k_barrier();
    wd_ready = 1;
    if (watchdog_hardlockup_user_enabled && watchdog_user_enabled) {
        if (wd_perf) {
            klog(KLOG_INFO, "nmi_watchdog", "enabled: permanently consumes one hw-PMU counter");
        } else if (smp_cpu_count() > 1) {
            klog(KLOG_INFO, "watchdog", "hard lockup: buddy check of the next processor");
        } else {
            klog(KLOG_INFO, "watchdog", "hard watchdog permanently disabled");
        }
    }
    hung_thread = kthread_start("khungtaskd", khungtaskd_main);
    if (!hung_thread) {
        klog(KLOG_ERR, "hung_task", "khungtaskd: refused: no record or stack for it");
    }
    if (debugfs_create_text("test_lockup", 0600u, &test_lockup_ops, NULL) != 0) {
        klog(KLOG_ERR, "watchdog", "test_lockup: refused: no debugfs node");
    }
}

/* ---- the debug shell's test, lib/test_lockup.c ---- */

static wait_queue_t test_wait;
static volatile uint32_t test_request;
static volatile int test_busy;
static uint64_t test_until;
static process_t *test_thread;

/** Whether the test thread's sleep has reached `test_until`. */
static int test_passed(void *arg)
{
    (void)arg;
    return tick_count() >= test_until;
}

/** Whether a test was asked for since the request `*arg` saw. */
static int test_asked(void *arg)
{
    return test_request != *(const uint32_t *)arg;
}

/** The test thread: for each request, one uninterruptible sleep until
 * `test_until`, which /proc reports in the state D. */
static void test_lockup_main(void)
{
    uint32_t seen = 0;

    arch_irq_enable();
    for (;;) {
        (void)wq_wait_event_idle(&test_wait, test_asked, &seen);
        seen = test_request;
        current_proc->wakeup_tick = test_until;
        (void)wq_wait_event_nointr(&test_wait, test_passed, NULL);
        current_proc->wakeup_tick = 0;
        klog(KLOG_INFO, "test_lockup", "hung: woke after its sleep");
        test_busy = 0;
    }
}

/** Whether `kind` is the four letters `name`. */
static int kind_is(const char *kind, const char *name)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (kind[i] != name[i]) {
            return 0;
        }
    }
    return kind[4] == '\0';
}

int watchdog_test(const char *kind, unsigned int seconds)
{
    uint32_t flags, i;
    int was_on;

    if (kind_is(kind, "soft") || kind_is(kind, "hard")) {
        flags = kirq_save();
        if (kind[0] == 's') {
            arch_irq_enable();
        }
        for (i = 0; i < seconds * 1000u; i++) {
            k_udelay(1000);
        }
        kirq_restore(flags);
        return 0;
    }
    if (!kind_is(kind, "hung") || test_busy) {
        return -1;
    }
    if (!test_thread) {
        test_thread = kthread_start("test_lockup", test_lockup_main);
        if (!test_thread) {
            return -1;
        }
    }
    test_until = tick_count() + (uint64_t)seconds * TICK_HZ;
    test_busy = 1;
    test_request++;
    wq_wake_all(&test_wait);
    was_on = sched_is_enabled();
    sched_enable();
    while (test_busy) {
        idle_wait_irq_on();
    }
    if (!was_on) {
        sched_disable();
    }
    return 0;
}

/** The writer's own sleep in the state D until `seconds` have passed, as
 * lib/test_lockup.c's `state=D` sleeps in the process that loads it. 0, or
 * -EBUSY while another test sleeps. */
static int32_t test_sleep_d(unsigned int seconds)
{
    if (test_busy) {
        return -EBUSY;
    }
    test_busy = 1;
    test_until = tick_count() + (uint64_t)seconds * TICK_HZ;
    current_proc->wakeup_tick = test_until;
    (void)wq_wait_event_nointr(&test_wait, test_passed, NULL);
    current_proc->wakeup_tick = 0;
    test_busy = 0;
    return 0;
}

/** The text of /sys/kernel/debug/test_lockup: the kinds a write takes. */
static int32_t test_lockup_show(void *data, char *buf, uint32_t cap)
{
    static const char text[] = "soft hard hung\n";
    uint32_t i;

    (void)data;
    for (i = 0; i + 1 < sizeof text && i < cap; i++) {
        buf[i] = text[i];
    }
    return (int32_t)(sizeof text - 1);
}

/**
 * A write of `KIND SECONDS` to /sys/kernel/debug/test_lockup, the debug
 * shell's `lockup` for a root shell: `soft` and `hard` spin in the write
 * with interrupts on or off, and `hung` sleeps in it in the state D, for 1
 * to 600 seconds, and the write returns when the lockup ends. `len`, or
 * -EINVAL for another text, -EFAULT or -EBUSY.
 */
static int32_t test_lockup_store(void *data, const char __user *buf, uint32_t len)
{
    char line[16];
    unsigned int seconds = 0;
    uint32_t i, n = len;

    (void)data;
    if (len < 6 || len >= sizeof line) {
        return -EINVAL;
    }
    if (copy_from_user(line, buf, len) != 0) {
        return -EFAULT;
    }
    if (line[n - 1] == '\n') {
        n--;
    }
    if (line[4] != ' ') {
        return -EINVAL;
    }
    line[4] = '\0';
    for (i = 5; i < n && line[i] >= '0' && line[i] <= '9' && seconds <= 600u; i++) {
        seconds = seconds * 10u + (unsigned int)(line[i] - '0');
    }
    if ((!kind_is(line, "soft") && !kind_is(line, "hard") && !kind_is(line, "hung")) || i != n ||
        seconds == 0 || seconds > 600u) {
        return -EINVAL;
    }
    if (line[1] == 'u') {
        return test_sleep_d(seconds) < 0 ? -EBUSY : (int32_t)len;
    }
    return watchdog_test(line, seconds) < 0 ? -EINVAL : (int32_t)len;
}

static const struct debugfs_text_ops test_lockup_ops = {test_lockup_show, test_lockup_store};
