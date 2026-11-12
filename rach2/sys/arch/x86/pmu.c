/*
 * pmu.c: the performance counter that raises the hard-lockup detector's NMI
 * on the PC, Intel's fixed counter 1 or general counter 0, or AMD's core
 * counter 2 on family 15h and core counter 0 on the others; and each
 * processor's freeze of its counters in SMM.
 */
#include "../../platform.h"

#include "../../klog.h"
#include "../../proc/percpu.h"
#include "../../timer.h"
#include "../../udelay.h"
#include "kcpu.h"
#include "lapic.h"
#include "pit.h"

/* Intel's architectural performance monitor (SDM volume 3, chapter 21):
 * general counter 0 and its event select, the full-width alias of the
 * counter, the capabilities that say whether that alias exists, fixed
 * counter 1 and the fixed counters' control, and the global control,
 * status and overflow-clear registers of version 2. */
#define IA32_PMC0                 0x0C1u
#define IA32_PERFEVTSEL0          0x186u
#define IA32_PERF_CAPABILITIES    0x345u
#define IA32_A_PMC0               0x4C1u
#define IA32_FIXED_CTR1           0x30Au
#define IA32_FIXED_CTR_CTRL       0x38Du
#define IA32_PERF_GLOBAL_STATUS   0x38Eu
#define IA32_PERF_GLOBAL_CTRL     0x38Fu
#define IA32_PERF_GLOBAL_OVF_CTRL 0x390u
#define PERF_CAP_FW_WRITE         (1u << 13)

/* IA32_DEBUGCTL's FREEZE_WHILE_SMM, which stops the counters while the
 * processor is in SMM (Linux's DEBUGCTLMSR_FREEZE_IN_SMM,
 * arch/x86/include/asm/msr-index.h:418-419); the SMM_FREEZE bit of
 * IA32_PERF_CAPABILITIES, which says the processor has it; `cpuid` leaf
 * 1's PDCM, which says IA32_PERF_CAPABILITIES is there; and the length of
 * leaf 0xA's event mask below which Linux's perf takes no Intel monitor
 * (arch/x86/events/intel/core.c:6536-6538, ARCH_PERFMON_EVENTS_COUNT). */
#define IA32_DEBUGCTL       0x1D9u
#define DEBUGCTL_FREEZE_SMM (1u << 14)
#define PERF_CAP_SMM_FREEZE (1u << 12)
#define CPUID1_PDCM         (1u << 15)
#define ARCH_PERFMON_EVENTS 7u

/* Fixed counter 1's field of IA32_FIXED_CTR_CTRL: counting in the kernel
 * and in user mode, and the interrupt on overflow; and its bit in the
 * global registers. */
#define FIXED1_COUNT  (0x3u << 4)
#define FIXED1_WATCH  (0xBu << 4)
#define FIXED1_GLOBAL ((uint64_t)1 << 33)

/* AMD's core counter 0 and its control (BKDG for family 15h, and Linux's
 * MSR_F15H_PERF_CTL and MSR_F15H_PERF_CTR); counter i's are 2i above. */
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

/* What `platform_pmu_probe` found: whether it ran and found a counter, the
 * kind, whether the counter is Intel's fixed counter 1 and not a general
 * counter, the general counter's index otherwise (2 on AMD's family 15h,
 * 0 elsewhere), the architectural version (Intel's), the number of general
 * counters, the counter's width in bits, the register a write of the count
 * goes to, the longest period one load counts, the processor's clock, and
 * the window in which an NMI of no known source is taken for the counter's
 * late one (`platform_pmu_late`), 100 ms in TSC counts. */
static int probed, found;
static enum pmu_kind kind;
static int fixed;
static uint32_t counter_index;
static uint32_t version;
static uint32_t counters;
static uint32_t width;
static uint32_t count_msr;
static uint64_t max_period;
static uint64_t cpu_hz;
static uint64_t late_window;

/* Each processor's period and what is left of it: a period longer than
 * `max_period` is counted in loads of at most that, as Linux's
 * x86_perf_event_set_period counts `period_left`; and the TSC at which its
 * window for a late NMI closes (`platform_pmu_late`). */
static struct {
    uint64_t period;
    uint64_t left;
    uint64_t chunk;
    int on;
    uint64_t late_until;
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

/** The general counter's event-select register. */
static uint32_t evtsel_msr(void)
{
    return kind == PMU_AMD ? AMD_PERF_CTL0 + 2u * counter_index : IA32_PERFEVTSEL0;
}

/** The unhalted core-cycles event, for the event-select register. */
static uint32_t event(void)
{
    return kind == PMU_AMD ? AMD_CYCLES : INTEL_CORE_CYCLES;
}

/** The counter's register read for its count. */
static uint32_t read_msr(void)
{
    if (fixed) {
        return IA32_FIXED_CTR1;
    }
    return kind == PMU_AMD ? AMD_PERF_CTR0 + 2u * counter_index : IA32_PMC0;
}

/** The counter's bit in Intel's global control, status and overflow
 * clear. */
static uint64_t global_bit(void)
{
    return fixed ? FIXED1_GLOBAL : 1u;
}

/** Start the counter counting unhalted cycles in both modes, with its
 * interrupt on overflow when `irq`. The fixed counters' control holds the
 * detector's field alone, as the general counter's select holds its event. */
static void start(int irq)
{
    if (fixed) {
        wrmsr64(IA32_FIXED_CTR_CTRL, irq ? FIXED1_WATCH : FIXED1_COUNT);
    } else {
        wrmsr64(evtsel_msr(), irq ? EVTSEL_WATCH(event()) : event() | EVTSEL_COUNT);
    }
}

/** Stop the counter, and with it every fixed counter on Intel's. */
static void stop(void)
{
    wrmsr64(fixed ? IA32_FIXED_CTR_CTRL : evtsel_msr(), 0);
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

/** Open this processor's window for a late NMI of the counter. */
static void open_window(int me)
{
    pmc[me].late_until = arch_cycles() + late_window;
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
 * Stop every general counter but the detector's, and on Intel's version 2
 * enable the detector's counter alone in the global control. The kernel
 * owns the counters, as Linux's PMU driver does from its reset
 * (arch/x86/events/intel/core.c, `intel_pmu_reset`, and AMD's six core
 * counters of arch/x86/events/amd/core.c, `amd_core_pmu_init`): a counter
 * the firmware left counting with its interrupt on would raise an NMI
 * through the same entry that is not the watchdog's, and the NMI handler
 * stops the machine on one it cannot place. The other fixed counters stop
 * with the detector's `start` and `stop`, which write their whole control.
 */
static void own_counters(void)
{
    uint32_t i;

    for (i = 0; i < counters; i++) {
        if (fixed || i != counter_index) {
            wrmsr64(kind == PMU_AMD ? AMD_PERF_CTL0 + 2u * i : IA32_PERFEVTSEL0 + i, 0);
        }
    }
    if (kind == PMU_INTEL && version >= 2) {
        wrmsr64(IA32_PERF_GLOBAL_CTRL, global_bit());
    }
}

/**
 * Whether the counter, counting cycles without an interrupt, moves over a
 * short wait: a hypervisor may list a PMU whose registers take writes and
 * count nothing. The counter is stopped again either way.
 */
static int counts(void)
{
    uint64_t a, b;

    stop();
    load_count(max_period);
    own_counters();
    start(0);
    a = rdmsr64(read_msr()) & width_mask();
    k_udelay(100);
    b = rdmsr64(read_msr()) & width_mask();
    stop();
    return a != b;
}

/**
 * Whether Intel's fixed counter 1, which counts unhalted core cycles, is
 * there, by `cpuid` leaf 0xA's ECX and EDX, `c` and `d`, as Linux's
 * `intel_pmu_init` counts the fixed counters (arch/x86/events/intel/
 * core.c:6536 and 6561-6567): version 5 lists them in a mask, and versions
 * 2 to 4 give their number, at least three outside a hypervisor.
 */
static int has_fixed1(uint32_t c, uint32_t d)
{
    uint32_t ecx1 = 0;

    if (version >= 5) {
        return (c & 2u) != 0;
    }
    if (version < 2) {
        return 0;
    }
    kcpu_id(1, NULL, NULL, &ecx1, NULL);
    return (d & 0x1Fu) >= 2 || (ecx1 & (1u << 31)) == 0;
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
 * Find Intel's architectural monitor by `cpuid` leaf 0xA, and its counter
 * for the detector: fixed counter 1 where `has_fixed1` finds it, general
 * counter 0 otherwise. Leaves `kind` PMU_NONE without a cycles counter.
 */
static void probe_intel(void)
{
    uint32_t a, b, c, d;

    kcpu_id(0xAu, &a, &b, &c, &d);
    version = a & 0xFFu;
    width = (a >> 16) & 0xFFu;
    /* EBX bit 0 set says the core-cycles event is absent, and the event
     * mask is as long as EAX bits 31-24 say. */
    if (version < 1 || ((a >> 8) & 0xFFu) < 1 || width < 32 || ((a >> 24) & 0xFFu) < 1 ||
        (b & 1u) != 0) {
        return;
    }
    kind = PMU_INTEL;
    counters = (a >> 8) & 0xFFu;
    fixed = has_fixed1(c, d);
    count_msr = fixed ? IA32_FIXED_CTR1 : IA32_PMC0;
    max_period = 0x7FFFFFFFu;
    kcpu_id(1, NULL, NULL, &c, NULL);
    /* With full-width writes Linux loads every counter, the fixed ones too,
     * with up to half the width (arch/x86/events/intel/core.c,
     * `intel_pmu_init`). */
    if ((c & (1u << 15)) != 0 && version >= 2 &&
        (rdmsr64(IA32_PERF_CAPABILITIES) & PERF_CAP_FW_WRITE) != 0) {
        count_msr = fixed ? IA32_FIXED_CTR1 : IA32_A_PMC0;
        max_period = ((uint64_t)1 << (width - 1)) - 1u;
    }
}

void platform_pmu_cpu_setup(void)
{
    uint32_t a, c, vendor[3];
    uint32_t lo = 0, hi = 0;

    if (!kcpu_msr_present()) {
        return;
    }
    kcpu_id(0, &a, &vendor[0], &vendor[2], &vendor[1]);
    if (!vendor_is(vendor, "GenuineIntel") || a < 0xAu) {
        return;
    }
    kcpu_id(0xAu, &a, NULL, NULL, NULL);
    if ((a & 0xFFu) < 2u || ((a >> 24) & 0xFFu) < ARCH_PERFMON_EVENTS) {
        return;
    }
    kcpu_id(1, NULL, NULL, &c, NULL);
    if ((c & CPUID1_PDCM) == 0 || (rdmsr64(IA32_PERF_CAPABILITIES) & PERF_CAP_SMM_FREEZE) == 0) {
        return;
    }
    kcpu_rdmsr(IA32_DEBUGCTL, &lo, &hi);
    kcpu_wrmsr(IA32_DEBUGCTL, lo | DEBUGCTL_FREEZE_SMM, hi);
}

/**
 * Linux takes the cycles event for its watchdog (kernel/watchdog_perf.c:86,
 * `wd_hw_attr`) and the period as cycles at `cpu_khz`
 * (arch/x86/kernel/apic/hw_nmi.c:27); x86's PMU drivers refuse a processor
 * without the architectural monitor on Intel (arch/x86/events/intel/core.c,
 * `intel_pmu_init`) and use the core counters on AMD
 * (arch/x86/events/amd/core.c, `amd_core_pmu_init`). Intel's cycles event,
 * 0x3C, may run on fixed counter 1 or any general counter
 * (arch/x86/events/intel/core.c:506 and 6353-6357), and the scheduler
 * takes the fixed counter first (arch/x86/events/core.c:880-892), which
 * leaves every general counter to the rest; without it, and on AMD,
 * the event takes counter 0. On AMD's family 15h it takes core counter 2:
 * there the cycles event, 0x76, may use counters 0 to 2, and events 0x08F,
 * 0x187 and 0x188 counter 0 alone (arch/x86/events/amd/core.c:1153-1154
 * and 1210-1223), which Linux's scheduler gives a guest's event by moving
 * the watchdog (arch/x86/events/core.c:1085-1086). Every event that may use
 * counter 2 may also use counter 0 or 1, so a guest's events find the
 * counters free that Linux's would.
 */
int platform_pmu_probe(void)
{
    uint32_t a, c, vendor[3];

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
        probe_intel();
    } else if (vendor_is(vendor, "AuthenticAMD") || vendor_is(vendor, "HygonGenuine")) {
        kcpu_id(0x80000000u, &a, NULL, NULL, NULL);
        if (a >= 0x80000001u) {
            kcpu_id(0x80000001u, NULL, NULL, &c, NULL);
            if ((c & (1u << 23)) != 0) {
                kcpu_id(1, &a, NULL, NULL, NULL);
                kind = PMU_AMD;
                counters = 6;
                /* Family 15h: base family 0xF, extended family 6. */
                counter_index = (a & 0x0FF00F00u) == 0x00600F00u ? 2u : 0u;
                width = 48;
                count_msr = AMD_PERF_CTR0 + 2u * counter_index;
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
        klog(KLOG_INFO, "nmi_watchdog", "no counter: %s %u does not count",
             fixed ? "fixed counter" : "counter", fixed ? 1u : counter_index);
        return 0;
    }
    if (cpu_hz == 0) {
        kind = PMU_NONE;
        klog(KLOG_INFO, "nmi_watchdog", "no counter: the clock could not be measured");
        return 0;
    }
    found = 1;
    late_window = tick_div(cpu_hz, 10u, (uint32_t *)0);
    klog(KLOG_INFO, "nmi_watchdog", "counter: %s version %u, %s %u, %u bits, %u MHz",
         kind == PMU_AMD ? "amd core" : "intel arch", version, fixed ? "fixed counter" : "counter",
         fixed ? 1u : counter_index, width,
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
    open_window(me);
    stop();
    pmc[me].period = cycles;
    pmc[me].left = cycles;
    pmc[me].chunk = cycles < max_period ? cycles : max_period;
    load_count(pmc[me].chunk);
    if (kind == PMU_INTEL && version >= 2) {
        wrmsr64(IA32_PERF_GLOBAL_OVF_CTRL, global_bit());
    }
    own_counters();
    /* On before the unmask, so that an NMI after it unmasks it again. */
    pmc[me].on = 1;
    lapic_set_lvtpc(LAPIC_LVT_NMI);
    start(1);
}

void platform_pmu_disable(void)
{
    int me = (int)arch_cpu_id();

    if (kind == PMU_NONE || !pmc[me].on) {
        return;
    }
    open_window(me);
    pmc[me].on = 0;
    stop();
    if (kind == PMU_INTEL && version >= 2) {
        wrmsr64(IA32_PERF_GLOBAL_CTRL, 0);
        wrmsr64(IA32_PERF_GLOBAL_OVF_CTRL, global_bit());
    }
    lapic_set_lvtpc(LAPIC_LVT_MASKED | LAPIC_LVT_NMI);
}

/**
 * Intel's version 2 says which counter overflowed in its global status,
 * cleared through the overflow-clear register
 * (arch/x86/events/intel/core.c:3183-3190, `intel_pmu_handle_irq`); version
 * 1 and AMD say nothing, and a counter loaded with the negative of its
 * period has overflowed once its top bit is clear
 * (arch/x86/events/core.c:1690-1699, `x86_pmu_handle_irq`). Intel's APIC
 * masks the counter's entry as it delivers the NMI, and AMD's does not. The
 * entry is written again on every NMI while the counter runs, whether or
 * not it overflowed, as Linux's handlers write it on every NMI while an
 * event is active (arch/x86/events/core.c:1688 and 1745-1746,
 * arch/x86/events/intel/core.c:3176-3177 and 3226-3227): a late NMI that
 * finds the status the arming cleared was still delivered through the
 * entry and masked it. The write comes after the counter is loaded again,
 * as Linux's late acknowledgement for Haswell and later
 * (arch/x86/events/intel/core.c:3168-3175); a stopped counter's entry
 * stays masked.
 */
int platform_pmu_overflow(int *due)
{
    int me = (int)arch_cpu_id();
    int hit;
    uint64_t count;

    *due = 0;
    if (kind == PMU_NONE || !pmc[me].on) {
        return 0;
    }
    if (kind == PMU_INTEL && version >= 2) {
        hit = (rdmsr64(IA32_PERF_GLOBAL_STATUS) & global_bit()) != 0;
        if (hit) {
            wrmsr64(IA32_PERF_GLOBAL_OVF_CTRL, global_bit());
        }
    } else {
        count = rdmsr64(read_msr()) & width_mask();
        hit = (count & ((uint64_t)1 << (width - 1))) == 0;
    }
    if (hit) {
        if (pmc[me].left <= pmc[me].chunk) {
            *due = 1;
            pmc[me].left = pmc[me].period;
        } else {
            pmc[me].left -= pmc[me].chunk;
        }
        pmc[me].chunk = pmc[me].left < max_period ? pmc[me].left : max_period;
        load_count(pmc[me].chunk);
    }
    lapic_set_lvtpc(LAPIC_LVT_NMI);
    return hit;
}

/**
 * On AMD, open the window for a late NMI once the handler has run the
 * overflow's callback, which may take seconds to report a lockup, as
 * `amd_pmu_handle_irq` opens it after `x86_pmu_handle_irq` and the
 * callbacks it runs (arch/x86/events/amd/core.c:874-882, 911-918;
 * arch/x86/events/core.c:1713).
 */
void platform_pmu_handled(void)
{
    if (kind == PMU_AMD) {
        open_window((int)arch_cpu_id());
    }
}

/**
 * An NMI the counter raised may come after the code that would have taken
 * it: one in flight as the counter is loaded again or stopped finds no
 * overflow, and on AMD the overflow is seen late, which Linux allows for by
 * claiming every NMI of no known source for 100 ms after a handled overflow
 * (arch/x86/events/amd/core.c:856-889, 1420, `perf_nmi_window`), which
 * `platform_pmu_handled` opens. The same window here follows each arming
 * and stop of the counter, on Intel too, whose global status the arming
 * clears.
 */
int platform_pmu_late(void)
{
    if (kind == PMU_NONE) {
        return 0;
    }
    return arch_cycles() < pmc[arch_cpu_id()].late_until;
}
