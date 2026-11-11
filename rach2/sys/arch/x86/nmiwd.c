/*
 * nmiwd.c: the performance counter that raises the hard-lockup detector's
 * NMI on the PC, and the registers and switch frames its reports read.
 */
#include "../../platform.h"

#include "../../klog.h"
#include "../../proc/process.h"
#include "../../timer.h"
#include "../../udelay.h"
#include "../../watchdog.h"
#include "kcpu.h"
#include "lapic.h"
#include "pit.h"

/* Intel's architectural performance monitor (SDM volume 3, chapter 21):
 * general counter 0 and its event select, the full-width alias of the
 * counter, the capabilities that say whether that alias exists, and the
 * global control, status and overflow-clear registers of version 2. */
#define IA32_PMC0                 0x0C1u
#define IA32_PERFEVTSEL0          0x186u
#define IA32_PERF_CAPABILITIES    0x345u
#define IA32_A_PMC0               0x4C1u
#define IA32_PERF_GLOBAL_STATUS   0x38Eu
#define IA32_PERF_GLOBAL_CTRL     0x38Fu
#define IA32_PERF_GLOBAL_OVF_CTRL 0x390u
#define PERF_CAP_FW_WRITE         (1u << 13)

/* AMD's core counter 0 and its control (BKDG for family 15h, and Linux's
 * MSR_F15H_PERF_CTL and MSR_F15H_PERF_CTR). */
#define AMD_PERF_CTL0 0xC0010200u
#define AMD_PERF_CTR0 0xC0010201u

/* The event select's fields, the same on both: counting in user and kernel
 * mode, an interrupt on overflow, and the enable. The events are Intel's
 * UnHalted Core Cycles, 0x3C, and AMD's CPU Clocks not Halted, 0x76, which
 * Linux's PERF_COUNT_HW_CPU_CYCLES selects on each. */
#define EVTSEL_USR        (1u << 16)
#define EVTSEL_OS         (1u << 17)
#define EVTSEL_INT        (1u << 20)
#define EVTSEL_EN         (1u << 22)
#define INTEL_CORE_CYCLES 0x3Cu
#define AMD_CYCLES        0x76u
#define EVTSEL_COUNT      (EVTSEL_USR | EVTSEL_OS | EVTSEL_EN)
#define EVTSEL_WATCH(ev)  ((ev) | EVTSEL_COUNT | EVTSEL_INT)

enum pmu_kind { PMU_NONE, PMU_INTEL, PMU_AMD };

/* What `platform_pmu_probe` found: the kind, the architectural version
 * (Intel's), the counter's width in bits, the register a write of the count
 * goes to, the longest period one load counts, and the processor's clock. */
static enum pmu_kind kind;
static uint32_t version;
static uint32_t width;
static uint32_t count_msr;
static uint64_t max_period;
static uint64_t cpu_hz;

/* Each processor's period and what is left of it: a period longer than
 * `max_period` is counted in loads of at most that, as Linux's
 * x86_perf_event_set_period counts `period_left`. */
static struct {
    uint64_t period;
    uint64_t left;
    uint64_t chunk;
    int on;
} pmc[NR_CPUS];

/** Write the 64-bit `v` to model-specific register `msr`. */
static void wrmsr64(uint32_t msr, uint64_t v)
{
    kcpu_wrmsr(msr, (uint32_t)v, (uint32_t)(v >> 32));
}

/** Read model-specific register `msr`, 64 bits. */
static uint64_t rdmsr64(uint32_t msr)
{
    uint32_t lo = 0, hi = 0;

    kcpu_rdmsr(msr, &lo, &hi);
    return ((uint64_t)hi << 32) | lo;
}

/** Counter 0's event-select register. */
static uint32_t evtsel_msr(void)
{
    return kind == PMU_AMD ? AMD_PERF_CTL0 : IA32_PERFEVTSEL0;
}

/** The unhalted core-cycles event, for the event-select register. */
static uint32_t event(void)
{
    return kind == PMU_AMD ? AMD_CYCLES : INTEL_CORE_CYCLES;
}

/** The mask of the counter's `width` bits. */
static uint64_t width_mask(void)
{
    return width >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << width) - 1u);
}

/** Load the counter so that it overflows after `n` more cycles. Without
 * Intel's full-width alias a write of IA32_PMC0 is sign-extended from bit
 * 31, which `max_period` allows for. */
static void load_count(uint64_t n)
{
    wrmsr64(count_msr, (0 - n) & width_mask());
}

/** Whether `vendor`, the twelve bytes of `cpuid` leaf 0, names `name`. */
static int vendor_is(const uint32_t vendor[3], const char *name)
{
    const char *v = (const char *)vendor;
    int i;

    for (i = 0; i < 12; i++) {
        if (v[i] != name[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * Whether counter 0, counting cycles without an interrupt, moves over a
 * short wait: a hypervisor may list a PMU whose registers take writes and
 * count nothing. The counter is stopped again either way.
 */
static int counts(void)
{
    uint64_t a, b;

    wrmsr64(evtsel_msr(), 0);
    load_count(max_period);
    if (kind == PMU_INTEL && version >= 2) {
        wrmsr64(IA32_PERF_GLOBAL_CTRL, rdmsr64(IA32_PERF_GLOBAL_CTRL) | 1u);
    }
    wrmsr64(evtsel_msr(), event() | EVTSEL_COUNT);
    a = rdmsr64(kind == PMU_AMD ? AMD_PERF_CTR0 : IA32_PMC0) & width_mask();
    k_udelay(100);
    b = rdmsr64(kind == PMU_AMD ? AMD_PERF_CTR0 : IA32_PMC0) & width_mask();
    wrmsr64(evtsel_msr(), 0);
    return a != b;
}

/**
 * The processor's clock: the TSC's measured rate (pit.h), or, where it was
 * not measured, as under a hypervisor that does not say the TSC runs at one
 * rate, the TSC's counts over a 10 ms wait.
 */
static uint64_t measure_hz(void)
{
    uint64_t t0;

    if (tsc_counts_per_tick() != 0) {
        return (uint64_t)tsc_counts_per_tick() * TICK_HZ;
    }
    t0 = arch_cycles();
    k_udelay(10000);
    return (arch_cycles() - t0) * 100u;
}

/**
 * Linux takes general counter 0's cycles event for its watchdog
 * (kernel/watchdog_perf.c:86, `wd_hw_attr`) and the period as cycles at
 * `cpu_khz` (arch/x86/kernel/apic/hw_nmi.c:27); x86's PMU drivers refuse a
 * processor without the architectural monitor on Intel
 * (arch/x86/events/intel/core.c, `intel_pmu_init`) and use the core
 * counters on AMD (arch/x86/events/amd/core.c, `amd_core_pmu_init`).
 */
int platform_pmu_probe(void)
{
    uint32_t a, b, c, d, vendor[3];
    static int probed, found;

    if (probed) {
        return found;
    }
    probed = 1;
    cpu_hz = measure_hz();
    if (!kcpu_msr_present() || !lapic_ready()) {
        klog(KLOG_INFO, "nmi_watchdog", "no counter: no local apic or no msrs");
        return 0;
    }
    kcpu_id(0, &a, &vendor[0], &vendor[2], &vendor[1]);
    if (vendor_is(vendor, "GenuineIntel") && a >= 0xAu) {
        kcpu_id(0xAu, &a, &b, &c, &d);
        version = a & 0xFFu;
        width = (a >> 16) & 0xFFu;
        /* EBX bit 0 set says the core-cycles event is absent, and the
         * event mask is as long as EAX bits 31-24 say. */
        if (version >= 1 && ((a >> 8) & 0xFFu) >= 1 && width >= 32 && ((a >> 24) & 0xFFu) >= 1 &&
            (b & 1u) == 0) {
            kind = PMU_INTEL;
            count_msr = IA32_PMC0;
            max_period = 0x7FFFFFFFu;
            kcpu_id(1, NULL, NULL, &c, NULL);
            if ((c & (1u << 15)) != 0 && version >= 2 &&
                (rdmsr64(IA32_PERF_CAPABILITIES) & PERF_CAP_FW_WRITE) != 0) {
                count_msr = IA32_A_PMC0;
                max_period = ((uint64_t)1 << (width - 1)) - 1u;
            }
        }
    } else if (vendor_is(vendor, "AuthenticAMD") || vendor_is(vendor, "HygonGenuine")) {
        kcpu_id(0x80000000u, &a, NULL, NULL, NULL);
        if (a >= 0x80000001u) {
            kcpu_id(0x80000001u, NULL, NULL, &c, NULL);
            if ((c & (1u << 23)) != 0) {
                kind = PMU_AMD;
                width = 48;
                count_msr = AMD_PERF_CTR0;
                max_period = ((uint64_t)1 << 47) - 1u;
            }
        }
    }
    if (kind == PMU_NONE) {
        klog(KLOG_INFO, "nmi_watchdog", "no counter: the processor lists no cycles counter");
        return 0;
    }
    if (!counts()) {
        kind = PMU_NONE;
        klog(KLOG_INFO, "nmi_watchdog", "no counter: counter 0 does not count");
        return 0;
    }
    if (cpu_hz == 0) {
        kind = PMU_NONE;
        klog(KLOG_INFO, "nmi_watchdog", "no counter: the clock could not be measured");
        return 0;
    }
    found = 1;
    klog(KLOG_INFO, "nmi_watchdog", "counter: %s version %u, %u bits, %u MHz",
         kind == PMU_AMD ? "amd core" : "intel arch", version, width,
         (unsigned int)tick_div(tick_div(cpu_hz, 1000u, (uint32_t *)0), 1000u, (uint32_t *)0));
    return 1;
}

uint64_t platform_cpu_hz(void)
{
    return cpu_hz;
}

void platform_pmu_enable(uint64_t cycles)
{
    int me = (int)arch_cpu_id();

    if (kind == PMU_NONE || cycles == 0) {
        return;
    }
    wrmsr64(evtsel_msr(), 0);
    pmc[me].period = cycles;
    pmc[me].left = cycles;
    pmc[me].chunk = cycles < max_period ? cycles : max_period;
    load_count(pmc[me].chunk);
    if (kind == PMU_INTEL && version >= 2) {
        wrmsr64(IA32_PERF_GLOBAL_OVF_CTRL, 1u);
        wrmsr64(IA32_PERF_GLOBAL_CTRL, rdmsr64(IA32_PERF_GLOBAL_CTRL) | 1u);
    }
    lapic_set_lvtpc(LAPIC_LVT_NMI);
    pmc[me].on = 1;
    wrmsr64(evtsel_msr(), EVTSEL_WATCH(event()));
}

void platform_pmu_disable(void)
{
    int me = (int)arch_cpu_id();

    if (kind == PMU_NONE || !pmc[me].on) {
        return;
    }
    pmc[me].on = 0;
    wrmsr64(evtsel_msr(), 0);
    if (kind == PMU_INTEL && version >= 2) {
        wrmsr64(IA32_PERF_GLOBAL_CTRL, rdmsr64(IA32_PERF_GLOBAL_CTRL) & ~(uint64_t)1u);
        wrmsr64(IA32_PERF_GLOBAL_OVF_CTRL, 1u);
    }
    lapic_set_lvtpc(LAPIC_LVT_MASKED | LAPIC_LVT_NMI);
}

/**
 * Intel's version 2 says which counter overflowed in its global status,
 * cleared through the overflow-clear register
 * (arch/x86/events/intel/core.c, `intel_pmu_handle_irq`); version 1 and
 * AMD say nothing, and a counter loaded with the negative of its period has
 * overflowed once its top bit is clear (arch/x86/events/core.c,
 * `x86_pmu_handle_irq`). The APIC masks the counter's entry as it delivers
 * the NMI, so the entry is written again, as `intel_pmu_handle_irq` does.
 */
int platform_pmu_overflow(int *due)
{
    int me = (int)arch_cpu_id();
    uint64_t count;

    *due = 0;
    if (kind == PMU_NONE || !pmc[me].on) {
        return 0;
    }
    if (kind == PMU_INTEL && version >= 2) {
        if ((rdmsr64(IA32_PERF_GLOBAL_STATUS) & 1u) == 0) {
            return 0;
        }
        wrmsr64(IA32_PERF_GLOBAL_OVF_CTRL, 1u);
    } else {
        count = rdmsr64(kind == PMU_AMD ? AMD_PERF_CTR0 : IA32_PMC0) & width_mask();
        if ((count & ((uint64_t)1 << (width - 1))) != 0) {
            return 0;
        }
    }
    if (pmc[me].left <= pmc[me].chunk) {
        *due = 1;
        pmc[me].left = pmc[me].period;
    } else {
        pmc[me].left -= pmc[me].chunk;
    }
    pmc[me].chunk = pmc[me].left < max_period ? pmc[me].left : max_period;
    load_count(pmc[me].chunk);
    lapic_set_lvtpc(LAPIC_LVT_NMI);
    return 1;
}

/** Write `v` as `digits` hexadecimal digits into `out`, which takes them and
 * a terminating NUL. */
static void hex(char *out, unsigned long v, int digits)
{
    int i;

    for (i = digits - 1; i >= 0; i--) {
        out[i] = "0123456789abcdef"[v & 0xFu];
        v >>= 4;
    }
    out[digits] = '\0';
}

#if defined(__x86_64__)
#define W 16
#else
#define W 8
#endif

/**
 * Linux's x86-64 `__show_regs` (arch/x86/kernel/process_64.c:68) and i386's
 * (process_32.c) without the symbol after RIP, which this kernel cannot
 * give. A frame from kernel mode on the i686 has no stack pointer of its
 * own: the processor pushed none, and the stack resumes just above the
 * frame, as Linux's `kernel_stack_pointer` takes it.
 */
void platform_show_regs(const char *subsystem, int level, const isr_params_t *p)
{
    char a[W + 1], b[W + 1], c[W + 1], d[W + 1];
    process_t *cur = current_proc;
    uintptr_t top = cur ? cur->kernel_stack : 0;

#if defined(__x86_64__)
    hex(a, p->rip, 16);
    klog(level, subsystem, "RIP: %04x:0x%s", (unsigned int)p->cs, a);
    hex(a, p->rsp, 16);
    hex(b, p->rflags, 8);
    klog(level, subsystem, "RSP: %04x:%s EFLAGS: %s", (unsigned int)p->ss, a, b);
    hex(a, p->rax, 16);
    hex(b, p->rbx, 16);
    hex(c, p->rcx, 16);
    klog(level, subsystem, "RAX: %s RBX: %s RCX: %s", a, b, c);
    hex(a, p->rdx, 16);
    hex(b, p->rsi, 16);
    hex(c, p->rdi, 16);
    klog(level, subsystem, "RDX: %s RSI: %s RDI: %s", a, b, c);
    hex(a, p->rbp, 16);
    hex(b, p->r8, 16);
    hex(c, p->r9, 16);
    klog(level, subsystem, "RBP: %s R08: %s R09: %s", a, b, c);
    hex(a, p->r10, 16);
    hex(b, p->r11, 16);
    hex(c, p->r12, 16);
    klog(level, subsystem, "R10: %s R11: %s R12: %s", a, b, c);
    hex(a, p->r13, 16);
    hex(b, p->r14, 16);
    hex(c, p->r15, 16);
    klog(level, subsystem, "R13: %s R14: %s R15: %s", a, b, c);
    (void)d;
#else
    uintptr_t sp = FRAME_USER(p) ? p->user_esp : (uintptr_t)&p->user_esp;

    hex(a, p->eip, 8);
    klog(level, subsystem, "EIP: %04x:0x%s", p->cs & 0xFFFFu, a);
    hex(a, p->eax, 8);
    hex(b, p->ebx, 8);
    hex(c, p->ecx, 8);
    hex(d, p->edx, 8);
    klog(level, subsystem, "EAX: %s EBX: %s ECX: %s EDX: %s", a, b, c, d);
    hex(a, p->esi, 8);
    hex(b, p->edi, 8);
    hex(c, p->ebp, 8);
    hex(d, sp, 8);
    klog(level, subsystem, "ESI: %s EDI: %s EBP: %s ESP: %s", a, b, c, d);
    hex(a, p->eflags, 8);
    klog(level, subsystem, "DS: %04x ES: %04x FS: %04x GS: %04x EFLAGS: %s", p->ds & 0xFFFFu,
         p->es & 0xFFFFu, p->fs & 0xFFFFu, p->gs & 0xFFFFu, a);
#endif
    if (FRAME_USER(p)) {
        return;
    }
    watchdog_show_trace(subsystem, level, (uintptr_t)FRAME_FP(p), top);
}

/* The `cli` after the halt in `arch_idle_masked` (cpu.c). */
extern char arch_idle_resume[];

int platform_frame_idle(const isr_params_t *p)
{
    return FRAME_PC(p) == (unsigned long)arch_idle_resume;
}

void platform_show_stack(const char *subsystem, int level)
{
    process_t *cur = current_proc;
    uintptr_t fp;

#if defined(__x86_64__)
    __asm__ __volatile__("mov %%rbp, %0" : "=r"(fp));
#else
    __asm__ __volatile__("mov %%ebp, %0" : "=r"(fp));
#endif
    watchdog_show_trace(subsystem, level, fp, cur ? cur->kernel_stack : 0);
}

/** The switch pushes the callee-saved registers above its return address,
 * the frame pointer last before it: EBP three words up on the i686
 * (arch/i686/switch.s), RBP five on x86-64 (arch/x86_64/switch.s). A saved
 * stack pointer outside the thread's stack gives 0 for both. */
void platform_blocked_frame(const struct process *t, uintptr_t *pc, uintptr_t *fp)
{
#if defined(__x86_64__)
    uintptr_t at = (uintptr_t)t->regs.rsp;
    const int slot = 5;
#else
    uintptr_t at = (uintptr_t)t->regs.esp;
    const int slot = 3;
#endif
    uintptr_t top = t->kernel_stack;

    *pc = 0;
    *fp = 0;
    if (top < KERNEL_STACK_SIZE || at < top - KERNEL_STACK_SIZE ||
        at + (slot + 2) * sizeof(uintptr_t) > top || at % sizeof(uintptr_t) != 0) {
        return;
    }
    *fp = ((const uintptr_t *)at)[slot];
    *pc = ((const uintptr_t *)at)[slot + 1];
}
