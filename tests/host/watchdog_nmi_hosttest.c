/**
 * watchdog_nmi_hosttest.c: the NMI handler's part of the lockup detectors,
 * watchdog.c's `watchdog_nmi`, with the counter, a stop of the processor and
 * the frame's idle check replaced by models that count their calls.
 *
 * host-cflags: -DRACH2_HOST_X86_64
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/lib/kstd.c
 *
 * Each row takes NMIs at given return addresses, with a backtrace request
 * (`nmi_cpu_backtrace`), the counter's overflow (`platform_pmu_overflow`)
 * and another processor's stop pending as it says, and checks which ones
 * watchdog_nmi claims: that the backtrace is answered before the counter is
 * checked, that each source is counted, and that an NMI after one of two or
 * more handled sources, at the same address and finding nothing, is
 * swallowed, as Linux's default_do_nmi swallows it
 * (arch/x86/kernel/nmi.c:404-405, 472-475), while one after a single source
 * is not. One row more runs the soft-lockup detector's timer past its
 * threshold with the host having stopped the guest and without
 * (`arch_guest_paused`).
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "../../rach2/sys/watchdog.c"

extern int printf(const char *fmt, ...);

static int failures;
static int overflow_ret;    /* what the counter's overflow check returns */
static int stop_ret;        /* what the check for another processor's stop returns */
static int calls;           /* calls of the models below, in order */
static int backtrace_at;    /* the order of the backtrace's frame check, 0 for none */
static int overflow_at;     /* the order of the last overflow check, 0 for none */
static int handled_at;      /* the order of the last platform_pmu_handled */
static uint64_t cycles_now; /* the counter of 1 GHz, the detectors' clock */
static int guest_paused;    /* what the host's stop of the guest reads as */
static int paused_asked;    /* how often the timer asked for it */
static int crit_logs;       /* records logged as KLOG_CRIT, a lockup's report */

/** Record one failed contract. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("watchdog-nmi-hosttest: failed: %s\n", what);
        failures++;
    }
}

/* ---- the models of the handler's sources ---- */

int platform_pmu_overflow(int *due)
{
    overflow_at = ++calls;
    *due = 0;
    return overflow_ret;
}

void platform_pmu_handled(void)
{
    handled_at = ++calls;
}

/** No counter NMI is in flight, so a late window claims nothing. */
int platform_pmu_late(void)
{
    return 0;
}

/** Another processor's stop, as the row says. */
int smp_stop_nmi(void)
{
    return stop_ret;
}

/** The backtrace's look at the frame, which marks when it ran; the frame
 * is never an idle wait. */
int platform_frame_idle(const isr_params_t *p)
{
    (void)p;
    backtrace_at = ++calls;
    return 0;
}

/* ---- the rest of the kernel, which no row reaches ---- */

struct percpu cpu_table[NR_CPUS];
process_t *proc_list;

process_t *alloc_process(void)
{
    return NULL;
}

unsigned int arch_atomic_cmpxchg(volatile unsigned int *p, unsigned int old, unsigned int value)
{
    unsigned int was = *p;

    if (was == old) {
        *p = value;
    }
    return was;
}

unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value)
{
    unsigned int was = *p;

    *p = value;
    return was;
}

uint64_t arch_cycles(void)
{
    return cycles_now;
}

void arch_irq_enable(void)
{}

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    (void)dst;
    (void)src;
    (void)n;
    return -1;
}

int32_t debugfs_create_text(const char *path, uint32_t mode, const struct debugfs_text_ops *ops,
                            void *obj)
{
    (void)path;
    (void)mode;
    (void)ops;
    (void)obj;
    return 0;
}

process_t *get_process(uint32_t pid)
{
    (void)pid;
    return NULL;
}

void idle_wait_irq_on(void)
{}

void k_panic(const char *str)
{
    printf("watchdog-nmi-hosttest: failed: panic: %s\n", str);
    for (;;) {
    }
}

int k_panicking(void)
{
    return 0;
}

void k_udelay(uint32_t usec)
{
    (void)usec;
}

int kcmdline_setup(const char *str)
{
    (void)str;
    return 0;
}

int kcmdline_value(const char *key, char *out, unsigned int outlen)
{
    (void)key;
    (void)out;
    (void)outlen;
    return 0;
}

uint32_t kirq_save(void)
{
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)subsystem;
    (void)format;
    if (level == KLOG_CRIT) {
        crit_logs++;
    }
}

/** Whether the host stopped the guest, as the row says, once. */
int arch_guest_paused(void)
{
    int was = guest_paused;

    paused_asked++;
    guest_paused = 0;
    return was;
}

void klog_flush_stuck(void)
{}

int klog_kept(void)
{
    return 0;
}

uintptr_t kstack_alloc(void)
{
    return 0;
}

void kstack_free(uintptr_t top)
{
    (void)top;
}

void platform_blocked_frame(const struct process *t, uintptr_t *pc, uintptr_t *fp)
{
    (void)t;
    *pc = *fp = 0;
}

uint64_t platform_cpu_hz(void)
{
    return 1000000000u;
}

void platform_cpu_nmi(int cpu)
{
    (void)cpu;
}

void platform_pmu_disable(void)
{}

void platform_pmu_enable(uint64_t cycles)
{
    (void)cycles;
}

int platform_pmu_probe(void)
{
    return 1;
}

void platform_show_regs(const char *subsystem, int level, const isr_params_t *p)
{
    (void)subsystem;
    (void)level;
    (void)p;
}

void platform_show_stack(const char *subsystem, int level)
{
    (void)subsystem;
    (void)level;
}

void proc_set_comm(process_t *p, const char *path)
{
    (void)p;
    (void)path;
}

void sched_disable(void)
{}

void sched_enable(void)
{}

int sched_is_enabled(void)
{
    return 0;
}

void sched_make_ready(process_t *p)
{
    (void)p;
}

void setup_process_stack(process_t *proc, void (*entry_point)(void))
{
    (void)proc;
    (void)entry_point;
}

int smp_cpu_count(void)
{
    return 1;
}

/** The unused scheduler clock stays at boot time in the NMI model. */
int64_t rtc_monotonic_ns(void)
{
    return 0;
}

/** Nothing: no row changes a watchdog setting on another processor. */
void smp_kick(int cpu)
{
    (void)cpu;
}

uint64_t tick_count(void)
{
    return 0;
}

uint64_t tick_div(uint64_t n, uint32_t d, uint32_t *rem)
{
    if (rem) {
        *rem = (uint32_t)(n % d);
    }
    return n / d;
}

int wq_wait_event_idle(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    return 0;
}

int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    return 0;
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/* ---- the rows ---- */

/** Take one NMI at return address `pc` with a backtrace request pending
 * when `backtrace`, the counter reporting an overflow when `overflow` and
 * another processor's stop when `stop`; watchdog_nmi's return. */
static int nmi_at(uintptr_t pc, int backtrace, int overflow, int stop)
{
    isr_params_t frame;

    k_memset(&frame, 0, sizeof(frame));
    FRAME_PC(&frame) = pc;
    backtrace_mask[arch_cpu_id()] = backtrace ? 1u : 0u;
    overflow_ret = overflow;
    stop_ret = stop;
    calls = backtrace_at = overflow_at = handled_at = 0;
    return watchdog_nmi(&frame);
}

/** Start each row on a processor that has taken no NMI. */
static void reset(int perf)
{
    k_memset(wd, 0, sizeof(wd));
    k_memset((void *)backtrace_mask, 0, sizeof(backtrace_mask));
    wd_perf = perf;
}

/** The backtrace is answered before the counter's check, and each source
 * claims its NMI. */
static void order_row(void)
{
    reset(1);
    check(nmi_at(0x1000, 1, 0, 0) == 1 && backtrace_at == 1 && overflow_at == 2,
          "the backtrace is answered before the detector's overflow check");
    check(!backtrace_mask[arch_cpu_id()], "and its request is cleared");
    check(nmi_at(0x2000, 0, 1, 0) == 1 && !backtrace_at && overflow_at == 1 && handled_at == 2,
          "an NMI of the detector's own is claimed and its overflow handled");
    check(nmi_at(0x3000, 0, 0, 0) == 0 && !backtrace_at && overflow_at == 1,
          "an NMI no source claims is not the handler's");
    reset(0);
    check(nmi_at(0x4000, 1, 1, 0) == 1 && backtrace_at == 1 && !overflow_at,
          "without the detector's counter the overflow is not checked");
}

/** A backtrace request and the counter's overflow in one NMI swallow the
 * back-to-back NMI the second latched, which finds nothing. */
static void swallow_row(void)
{
    reset(1);
    check(nmi_at(0x5000, 1, 1, 0) == 1, "an NMI of a backtrace and an overflow is claimed");
    check(nmi_at(0x5000, 0, 0, 0) == 1,
          "and the empty NMI after it at the same address is swallowed");
    check(nmi_at(0x5000, 0, 0, 0) == 1, "as is another, until a new address");
    check(nmi_at(0x6000, 0, 0, 0) == 0, "but not one at another address");
    check(nmi_at(0x6000, 0, 0, 0) == 0, "nor one at that address after it");
    reset(0);
    check(nmi_at(0x5000, 1, 0, 1) == 1 && nmi_at(0x5000, 0, 0, 0) == 1,
          "a backtrace and a stop swallow without the detector's counter too");
}

/** One source alone does not swallow the next empty NMI. */
static void single_row(void)
{
    reset(1);
    check(nmi_at(0x7000, 1, 0, 0) == 1, "an NMI of a backtrace alone is claimed");
    check(nmi_at(0x7000, 0, 0, 0) == 0, "and an empty NMI after it is not swallowed");
    reset(1);
    check(nmi_at(0x8000, 0, 1, 0) == 1, "an NMI of an overflow alone is claimed");
    check(nmi_at(0x8000, 0, 0, 0) == 0, "and an empty NMI after it is not swallowed");
}

/**
 * The soft-lockup timer of a processor that last scheduled 99 s ago: within
 * the threshold of its last report it asks nothing; past it, a guest the
 * host stopped meanwhile is no lockup, nothing is reported and the report's
 * time stays, as Linux's watchdog_timer_fn returns on
 * kvm_check_and_clear_guest_paused; the timer after it, with the guest not
 * stopped since, reports.
 */
static void paused_row(void)
{
    struct wd_cpu *w = &wd[arch_cpu_id()];
    isr_params_t frame;

    reset(0);
    k_memset(&frame, 0, sizeof(frame));
    wd_ready = 1;
    w->gen = wd_gen;
    w->enabled = 1;
    w->touch_ts = 1;
    w->report_ts = 95;
    cycles_now = (uint64_t)100 * 1000000000u;
    paused_asked = crit_logs = 0;
    guest_paused = 1;
    watchdog_timer_fn(w, &frame);
    check(paused_asked == 0 && crit_logs == 0 && guest_paused,
          "a timer within the threshold of the last report asks for no stop of the guest");
    w->report_ts = 1;
    watchdog_timer_fn(w, &frame);
    check(paused_asked == 1 && crit_logs == 0 && w->report_ts == 1,
          "a guest the host stopped is not reported as a soft lockup");
    watchdog_timer_fn(w, &frame);
    check(paused_asked == 2 && crit_logs == 1 && w->report_ts == 100,
          "and the timer after it, with no stop since, reports the lockup");
    wd_ready = 0;
    cycles_now = 0;
}

int main(void)
{
    order_row();
    swallow_row();
    single_row();
    paused_row();
    if (failures) {
        printf("watchdog-nmi-hosttest: result: fail: %d checks\n", failures);
        return 1;
    }
    printf("watchdog-nmi-hosttest: result: pass\n");
    return 0;
}
