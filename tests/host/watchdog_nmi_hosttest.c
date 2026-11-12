/**
 * watchdog_nmi_hosttest.c: the NMI handler's part of the lockup detectors,
 * watchdog.c's `watchdog_nmi`, with the counter, KVM's hook and the other
 * sources of an NMI replaced by models that count their calls.
 *
 * host-cflags: -DRACH2_HOST_X86_64
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/lib/kstd.c
 *
 * Each row takes NMIs at given return addresses and checks which ones
 * watchdog_nmi claims: that KVM's hook runs before the detector's
 * `platform_pmu_overflow` and is passed the sources taken before it, a
 * backtrace request, that each source the hook and the counter report is
 * counted, and that an NMI after one of two or more handled
 * sources, at the same address and finding nothing, is swallowed, as
 * Linux's default_do_nmi swallows it (arch/x86/kernel/nmi.c:404-405,
 * 472-475), while one after a single source is not.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "../../rach2/sys/watchdog.c"

extern int printf(const char *fmt, ...);

static int failures;
static int guest_ret;    /* what KVM's hook returns */
static int overflow_ret; /* what the counter's overflow check returns */
static int calls;        /* calls of the hook and the check, in order */
static int guest_at;     /* the order of the last hook call, 0 for none */
static int overflow_at;  /* the order of the last check, 0 for none */
static int handled_at;   /* the order of the last platform_pmu_handled */
static int guest_others; /* the sources the hook was told of, -1 for none */

/** Record one failed contract. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("watchdog-nmi-hosttest: failed: %s\n", what);
        failures++;
    }
}

/* ---- the models of the handler's sources ---- */

/** KVM's hook: the number of guest overflows the row says it claims,
 * noting the sources the handler took before it. */
static int guest_hook(int others)
{
    guest_at = ++calls;
    guest_others = others;
    return guest_ret;
}

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

/** No other processor stops this one. */
int smp_stop_nmi(void)
{
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
    return 0;
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

int kcmdline_has(const char *flag)
{
    (void)flag;
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
    (void)level;
    (void)subsystem;
    (void)format;
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

int platform_frame_idle(const isr_params_t *p)
{
    (void)p;
    return 0;
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

/** Take one NMI at return address `pc` with the hook and the counter
 * reporting `guest` and `overflow` sources; watchdog_nmi's return. */
static int nmi_at(uintptr_t pc, int guest, int overflow)
{
    isr_params_t frame;

    k_memset(&frame, 0, sizeof(frame));
    FRAME_PC(&frame) = pc;
    guest_ret = guest;
    overflow_ret = overflow;
    calls = guest_at = overflow_at = handled_at = 0;
    guest_others = -1;
    return watchdog_nmi(&frame);
}

/** Start each row on a processor that has taken no NMI. */
static void reset(int perf)
{
    k_memset(wd, 0, sizeof(wd));
    wd_perf = perf;
    watchdog_guest_nmi = guest_hook;
}

/** KVM's hook runs on every NMI, before the counter's check. */
static void order_row(void)
{
    reset(1);
    check(nmi_at(0x1000, 1, 0) == 1 && guest_at == 1 && overflow_at == 2,
          "KVM's hook runs before the detector's overflow check");
    check(nmi_at(0x2000, 0, 1) == 1 && guest_at == 1 && overflow_at == 2 && handled_at == 3,
          "and on an NMI of the detector's own, whose overflow is then handled");
    check(nmi_at(0x3000, 0, 0) == 0 && guest_at == 1,
          "an NMI no source claims is not the handler's");
    watchdog_guest_nmi = NULL;
    check(nmi_at(0x4000, 0, 1) == 1 && !guest_at && overflow_at == 1,
          "without KVM the detector's check runs alone");
}

/** A hook that reports two guest overflows swallows the back-to-back NMI
 * the second latched, which finds nothing. */
static void swallow_row(void)
{
    reset(1);
    check(nmi_at(0x5000, 2, 0) == 1, "an NMI of two guest overflows is claimed");
    check(nmi_at(0x5000, 0, 0) == 1, "and the empty NMI after it at the same address is swallowed");
    check(nmi_at(0x5000, 0, 0) == 1, "as is another, until a new address");
    check(nmi_at(0x6000, 0, 0) == 0, "but not one at another address");
    reset(0);
    check(nmi_at(0x5000, 2, 0) == 1 && nmi_at(0x5000, 0, 0) == 1,
          "the hook's count swallows without the detector's counter too");
}

/** One source alone does not swallow the next empty NMI. */
static void single_row(void)
{
    reset(1);
    check(nmi_at(0x7000, 1, 0) == 1, "an NMI of one guest overflow is claimed");
    check(nmi_at(0x7000, 0, 0) == 0, "and an empty NMI after it is not swallowed");
    reset(1);
    check(nmi_at(0x8000, 1, 1) == 1 && nmi_at(0x8000, 0, 0) == 1,
          "a guest overflow and the detector's in one NMI count as two sources");
}

/** A backtrace request, which the handler takes before KVM's hook, is
 * passed to the hook as a source the NMI handled, so that the hook spends
 * no claim on an NMI another source took (kvm/pmu.c's `kvm_pmu_nmi`). */
static void backtrace_row(void)
{
    reset(1);
    check(nmi_at(0x9000, 0, 0) == 0 && guest_others == 0,
          "the hook is told of no other source on an NMI of none");
    backtrace_mask[arch_cpu_id()] = 1;
    check(nmi_at(0xA000, 0, 0) == 1 && guest_others == 1 && !backtrace_mask[arch_cpu_id()],
          "and of the backtrace request the handler took before it");
    check(nmi_at(0xA000, 0, 0) == 0,
          "which alone, the hook claiming nothing, swallows no empty NMI after it");
}

int main(void)
{
    order_row();
    swallow_row();
    single_row();
    backtrace_row();
    if (failures) {
        printf("watchdog-nmi-hosttest: result: fail: %d checks\n", failures);
        return 1;
    }
    printf("watchdog-nmi-hosttest: result: pass\n");
    return 0;
}
