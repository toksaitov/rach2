/**
 * pmu_hosttest.c: the hard-lockup detector's performance counter
 * (arch/x86/pmu.c) against a model of the processor's counter registers.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The model is the Lenovo's Core i3-4030U, a Haswell whose Linux reports
 * "Haswell events, full-width counters" and an architectural monitor of
 * version 3, 48 bits wide, with three fixed counters, and eight other
 * processors the driver must handle: one without the full-width writes, a
 * hypervisor's version 3 without fixed counters, a version 1 monitor
 * without the global registers, AMD's core counters, AMD's family 15h, a
 * hypervisor's counter that takes writes and never moves, and processors
 * without the cycles event or the model-specific registers. The
 * detector takes fixed counter 1 where there is one, core counter 2 on
 * family 15h, and general counter 0 otherwise. Each register
 * exists only where the SDM or AMD's manual says it does, and an access to
 * any other, a write of a read-only register, or a write of bits past a
 * counter's width is a #GP, which fails the row. A legacy write of
 * IA32_PMC0 is sign-extended from bit 31, as the SDM says, so a load wider
 * than 31 bits through it counts the wrong period here as it would on the
 * machine.
 *
 * Each row probes, enables the counter for 10 seconds at the processor's
 * clock, runs the clock for 25 seconds and takes each NMI the counter
 * raises as watchdog_nmi takes it, through KVM's hook and then
 * platform_pmu_overflow, and checks that the due NMIs come at 10 and 20
 * seconds, that the APIC's performance-counter entry is unmasked again
 * after each and after a late one that finds no overflow, that a guest
 * counter's overflows are claimed, counted and acknowledged while the entry
 * is still masked, that AMD's window for a late NMI opens after the
 * overflow's callback, and that a disabled counter stops and keeps its
 * entry masked.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "../../rach2/sys/arch/x86/pmu.c"

extern int printf(const char *fmt, ...);
extern int vsnprintf(char *buf, unsigned long n, const char *fmt, va_list ap);
extern char *strstr(const char *hay, const char *needle);

/* ---- the model ---- */

/* A processor: what `cpuid` says, the capabilities register, whether the
 * counter moves, and the clock in hertz, which the TSC and the unhalted
 * cycles both count at. */
struct chip {
    const char *name;
    const char *vendor; /* the twelve bytes of leaf 0 */
    uint32_t max_leaf, leaf_a_eax, leaf_a_ebx, leaf1_ecx;
    uint32_t max_ext, ext1_ecx;
    uint64_t perf_cap;
    int has_msr;
    int dead;     /* the counter takes writes and does not count */
    int measured; /* the TSC's rate was measured against the PIT */
    uint64_t hz;
    uint32_t leaf_a_edx; /* the fixed counters' number and width */
    uint32_t leaf1_eax;  /* the family, model and stepping */
};

static const struct chip *chip;
static uint64_t tsc;
static uint64_t evtsel, counter, global_ctrl, global_status; /* the detector's */
static uint64_t fixed_ctrl, fixed1; /* the fixed counters' control, counter 1 */
static uint64_t other_evtsel[8];    /* the other general counters' selects */
static uint32_t lvtpc = LAPIC_LVT_MASKED;
static int nmi_pending;
static int guest_ovf;     /* guest counters' overflows not yet acknowledged */
static int guest_handled; /* the sources KVM's hook counted */
static int guest_masked;  /* the entry was masked when the hook acknowledged */
static int msr_writes, last_msr_written;
static uint64_t last_value_written;
static char fault[160];
static char logged[256];
static int failures;

/** Record the first #GP of a row. */
static void gp(const char *what, uint32_t msr)
{
    if (fault[0] == '\0') {
        char *p = fault;
        const char *w;
        int i;

        for (w = what; *w != '\0' && p < fault + 120; w++) {
            *p++ = *w;
        }
        *p++ = ' ';
        for (i = 7; i >= 0; i--) {
            *p++ = "0123456789abcdef"[(msr >> (4 * i)) & 0xFu];
        }
        *p = '\0';
    }
}

/** The architectural monitor's version, Intel's, from leaf 0xA. */
static uint32_t chip_version(void)
{
    return chip->leaf_a_eax & 0xFFu;
}

/** The counter's width in bits. */
static uint32_t chip_width(void)
{
    return chip->leaf_a_eax != 0 ? (chip->leaf_a_eax >> 16) & 0xFFu : 48u;
}

/** Whether the processor is AMD's. */
static int chip_amd(void)
{
    return chip->vendor[0] == 'A';
}

/** The mask of the counter's bits. */
static uint64_t chip_mask(void)
{
    return ((uint64_t)1 << chip_width()) - 1u;
}

/** The general counters: leaf 0xA's, or AMD's six core counters. */
static uint32_t chip_counters(void)
{
    return chip_amd() ? 6u : (chip->leaf_a_eax >> 8) & 0xFFu;
}

/** The general counter the detector takes when it takes one: AMD's core
 * counter 2 on family 15h, counter 0 otherwise. */
static uint32_t chip_gp(void)
{
    return chip_amd() && (chip->leaf1_eax & 0x0FF00F00u) == 0x00600F00u ? 2u : 0u;
}

/** The detector's general counter's register on AMD's chips: its count
 * when `count`, else its event select. */
static uint32_t amd_msr(int count)
{
    return AMD_PERF_CTL0 + 2u * chip_gp() + (count ? 1u : 0u);
}

/** The index of the event select of a general counter other than the
 * detector's that `msr` is, or 0 when it is none: counter i is i + 1, so
 * that AMD's counter 0 beside family 15h's detector has an index. */
static uint32_t other_index(uint32_t msr)
{
    if (chip_amd() && msr >= AMD_PERF_CTL0 && msr < AMD_PERF_CTL0 + 2u * chip_counters() &&
        (msr - AMD_PERF_CTL0) % 2u == 0 && msr != amd_msr(0)) {
        return (msr - AMD_PERF_CTL0) / 2u + 1u;
    }
    if (!chip_amd() && msr > IA32_PERFEVTSEL0 && msr < IA32_PERFEVTSEL0 + chip_counters()) {
        return msr - IA32_PERFEVTSEL0 + 1u;
    }
    return 0;
}

/** Whether `msr` is the count of an AMD core counter other than the
 * detector's, which the model reads as 0 and lets be written. */
static int other_count(uint32_t msr)
{
    return chip_amd() && msr > AMD_PERF_CTL0 && msr < AMD_PERF_CTL0 + 2u * chip_counters() &&
           (msr - AMD_PERF_CTL0) % 2u == 1 && msr != amd_msr(1);
}

/** Whether the chip has Intel's fixed counter 1, by leaf 0xA's EDX. */
static int chip_fixed(void)
{
    return !chip_amd() && chip_version() >= 2 && (chip->leaf_a_edx & 0x1Fu) >= 2;
}

/** Whether general counter 0 counts unhalted cycles now. */
static int counting(void)
{
    uint32_t want = chip_amd() ? 0x76u : 0x3Cu;

    if (chip->dead || (evtsel & EVTSEL_EN) == 0 || (evtsel & 0xFFFFu) != want) {
        return 0;
    }
    return chip_amd() || chip_version() < 2 || (global_ctrl & 1u) != 0;
}

/** Whether fixed counter 1 counts unhalted cycles now. */
static int counting_fixed(void)
{
    return !chip->dead && (fixed_ctrl & 0x30u) != 0 && (global_ctrl & FIXED1_GLOBAL) != 0;
}

/**
 * A counter overflowed, fixed counter 1 when `fx`: the status bit of
 * version 2, and with the interrupt bit set an NMI through an unmasked NMI
 * entry, which the APIC masks as it delivers it.
 */
static void overflow(int fx)
{
    if (!chip_amd() && chip_version() >= 2) {
        global_status |= fx ? FIXED1_GLOBAL : 1u;
    }
    if ((fx ? (fixed_ctrl & 0x80u) : (evtsel & EVTSEL_INT)) != 0 &&
        (lvtpc & LAPIC_LVT_MASKED) == 0 && (lvtpc & 0x700u) == LAPIC_LVT_NMI) {
        nmi_pending++;
        lvtpc |= LAPIC_LVT_MASKED;
    }
}

/** Advance the count `*c`, fixed counter 1's when `fx`, by `cycles` while
 * `on`. */
static void advance(uint64_t *c, int on, uint64_t cycles, int fx)
{
    uint64_t room;

    while (on && cycles > 0) {
        room = chip_mask() + 1u - *c;
        if (cycles < room) {
            *c += cycles;
            return;
        }
        cycles -= room;
        *c = 0;
        overflow(fx);
    }
}

/** Run the clock `cycles` cycles. */
static void run(uint64_t cycles)
{
    tsc += cycles;
    advance(&counter, counting(), cycles, 0);
    advance(&fixed1, counting_fixed(), cycles, 1);
}

/** The cycles until the next overflow of a counting counter, or `left`
 * when that is sooner or none counts. */
static uint64_t next_overflow(uint64_t left)
{
    uint64_t room = left;

    if (counting() && chip_mask() + 1u - counter < room) {
        room = chip_mask() + 1u - counter;
    }
    if (counting_fixed() && chip_mask() + 1u - fixed1 < room) {
        room = chip_mask() + 1u - fixed1;
    }
    return room;
}

/** Whether `msr` exists on the chip. */
static int msr_exists(uint32_t msr)
{
    int fw = !chip_amd() && (chip->leaf1_ecx & (1u << 15)) != 0 &&
             (chip->perf_cap & PERF_CAP_FW_WRITE) != 0;

    if (!chip->has_msr) {
        return 0;
    }
    if (chip_amd()) {
        return msr >= AMD_PERF_CTL0 && msr < AMD_PERF_CTL0 + 2u * chip_counters() &&
               (chip->ext1_ecx & (1u << 23)) != 0;
    }
    if (chip->max_leaf < 0xAu || chip_version() == 0) {
        return 0;
    }
    if (other_index(msr) != 0) {
        return 1;
    }
    switch (msr) {
    case IA32_PMC0:
    case IA32_PERFEVTSEL0:
        return 1;
    case IA32_FIXED_CTR1:
    case IA32_FIXED_CTR_CTRL:
        return chip_fixed();
    case IA32_A_PMC0:
        return fw;
    case IA32_PERF_CAPABILITIES:
        return (chip->leaf1_ecx & (1u << 15)) != 0;
    case IA32_PERF_GLOBAL_STATUS:
    case IA32_PERF_GLOBAL_CTRL:
    case IA32_PERF_GLOBAL_OVF_CTRL:
        return chip_version() >= 2;
    default:
        return 0;
    }
}

void kcpu_rdmsr(uint32_t msr, uint32_t *lo, uint32_t *hi)
{
    uint64_t v = 0;

    if (!msr_exists(msr)) {
        gp("rdmsr of absent", msr);
    } else if (other_index(msr) != 0) {
        v = other_evtsel[other_index(msr) - 1u];
    } else if (other_count(msr)) {
        v = 0;
    } else if (msr == IA32_PMC0 || msr == IA32_A_PMC0 || (chip_amd() && msr == amd_msr(1))) {
        v = counter;
    } else if (msr == IA32_PERFEVTSEL0 || (chip_amd() && msr == amd_msr(0))) {
        v = evtsel;
    } else if (msr == IA32_PERF_CAPABILITIES) {
        v = chip->perf_cap;
    } else if (msr == IA32_PERF_GLOBAL_STATUS) {
        v = global_status;
    } else if (msr == IA32_PERF_GLOBAL_CTRL) {
        v = global_ctrl;
    } else if (msr == IA32_FIXED_CTR1) {
        v = fixed1;
    } else if (msr == IA32_FIXED_CTR_CTRL) {
        v = fixed_ctrl;
    }
    if (lo) {
        *lo = (uint32_t)v;
    }
    if (hi) {
        *hi = (uint32_t)(v >> 32);
    }
}

void kcpu_wrmsr(uint32_t msr, uint32_t lo, uint32_t hi)
{
    uint64_t v = ((uint64_t)hi << 32) | lo;

    msr_writes++;
    last_msr_written = (int)msr;
    last_value_written = v;
    if (!msr_exists(msr)) {
        gp("wrmsr of absent", msr);
        return;
    }
    if (other_index(msr) != 0) {
        other_evtsel[other_index(msr) - 1u] = v;
        return;
    }
    if (other_count(msr)) {
        return;
    }
    if (chip_amd()) {
        /* The detector's counter is counter 0's in the switch. */
        msr = msr == amd_msr(1) ? AMD_PERF_CTR0 : AMD_PERF_CTL0;
    }
    switch (msr) {
    case IA32_PMC0:
        /* Bits 31:0 are written and the rest copy bit 31 (SDM 21.2.6). */
        counter = ((lo & 0x80000000u) != 0 ? ~(uint64_t)0xFFFFFFFFu | lo : lo) & chip_mask();
        break;
    case IA32_A_PMC0:
    case AMD_PERF_CTR0:
        if ((v & ~chip_mask()) != 0) {
            gp("wrmsr past the width of", msr);
        }
        counter = v & chip_mask();
        break;
    case IA32_FIXED_CTR1:
        if ((v & ~chip_mask()) != 0) {
            gp("wrmsr past the width of", msr);
        }
        fixed1 = v & chip_mask();
        break;
    case IA32_FIXED_CTR_CTRL:
        /* A field of four bits for each fixed counter. */
        if ((v >> (4u * (chip->leaf_a_edx & 0x1Fu))) != 0) {
            gp("wrmsr of reserved bits of", msr);
        }
        fixed_ctrl = v;
        break;
    case IA32_PERFEVTSEL0:
    case AMD_PERF_CTL0:
        if (hi != 0 || (lo & (1u << 21 | 1u << 19)) != 0) {
            gp("wrmsr of reserved bits of", msr);
        }
        evtsel = v;
        break;
    case IA32_PERF_GLOBAL_CTRL:
        global_ctrl = v;
        break;
    case IA32_PERF_GLOBAL_OVF_CTRL:
        global_status &= ~v;
        break;
    default:
        gp("wrmsr of read-only", msr);
        break;
    }
}

int kcpu_msr_present(void)
{
    return chip->has_msr;
}

void kcpu_id(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    uint32_t r[4] = {0, 0, 0, 0};
    const char *v = chip->vendor;

    if (leaf == 0) {
        r[0] = chip->max_leaf;
        r[1] = (uint32_t)v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
        r[3] = (uint32_t)v[4] | (uint32_t)v[5] << 8 | (uint32_t)v[6] << 16 | (uint32_t)v[7] << 24;
        r[2] = (uint32_t)v[8] | (uint32_t)v[9] << 8 | (uint32_t)v[10] << 16 | (uint32_t)v[11] << 24;
    } else if (leaf == 1) {
        r[0] = chip->leaf1_eax;
        r[2] = chip->leaf1_ecx;
    } else if (leaf == 0xAu && leaf <= chip->max_leaf) {
        r[0] = chip->leaf_a_eax;
        r[1] = chip->leaf_a_ebx;
    } else if (leaf == 0x80000000u) {
        r[0] = chip->max_ext;
    } else if (leaf == 0x80000001u && leaf <= chip->max_ext) {
        r[2] = chip->ext1_ecx;
    }
    if (a) {
        *a = r[0];
    }
    if (b) {
        *b = r[1];
    }
    if (c) {
        *c = r[2];
    }
    if (d) {
        *d = r[3];
    }
}

int lapic_ready(void)
{
    return 1;
}

void lapic_set_lvtpc(uint32_t entry)
{
    lvtpc = entry;
}

void k_udelay(uint32_t usec)
{
    run(chip->hz / 1000000u * usec);
}

uint32_t tsc_counts_per_tick(void)
{
    return chip->measured ? (uint32_t)(chip->hz / TICK_HZ) : 0;
}

uint64_t arch_cycles(void)
{
    return tsc;
}

uint64_t tick_div(uint64_t n, uint32_t d, uint32_t *rem)
{
    if (rem) {
        *rem = (uint32_t)(n % d);
    }
    return n / d;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    va_list ap;

    (void)level;
    (void)subsystem;
    va_start(ap, format);
    vsnprintf(logged, sizeof logged, format, ap);
    va_end(ap);
}

/* ---- the rows ---- */

/** Check `ok`, naming the row and the check. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("pmu_hosttest: %s: FAIL: %s\n", chip->name, what);
        failures++;
    }
}

/** Forget the driver's probe and put the model in its power-on state. */
static void reset(const struct chip *c)
{
    chip = c;
    probed = 0;
    found = 0;
    kind = PMU_NONE;
    fixed = 0;
    counter_index = 0;
    version = counters = width = count_msr = 0;
    max_period = cpu_hz = 0;
    k_memset(pmc, 0, sizeof pmc);
    tsc = 1000000;
    evtsel = counter = global_ctrl = global_status = 0;
    fixed1 = 0;
    k_memset(other_evtsel, 0, sizeof other_evtsel);
    /* The firmware left counter 1 counting cycles with its interrupt on,
     * and fixed counter 0 counting instructions. */
    other_evtsel[1] = 0x53003Cu;
    global_ctrl = 3u;
    fixed_ctrl = 0;
    if (chip_fixed()) {
        fixed_ctrl = 0x3u;
        global_ctrl |= (uint64_t)1 << 32;
    }
    lvtpc = LAPIC_LVT_MASKED | LAPIC_LVT_NMI;
    nmi_pending = 0;
    guest_ovf = guest_handled = guest_masked = 0;
    msr_writes = 0;
    fault[0] = logged[0] = '\0';
}

/** KVM's hook, `watchdog_guest_nmi` (kvm/pmu.c's `kvm_pmu_nmi`): acknowledge
 * the guest counters' overflows and return how many there were, noting
 * whether the counter's entry was still masked. */
static int guest_nmi(void)
{
    int n = guest_ovf;

    if (n) {
        guest_masked = (lvtpc & LAPIC_LVT_MASKED) != 0;
        guest_ovf = 0;
        guest_handled += n;
    }
    return n;
}

/** Take each pending NMI as watchdog_nmi does, KVM's hook before
 * platform_pmu_overflow; see `drive`. */
static int take_nmis(long ms, long *due_ms, int n, int max, int *others, int *masked)
{
    int due, guest;

    while (nmi_pending > 0) {
        nmi_pending--;
        guest = guest_nmi();
        if (platform_pmu_overflow(&due) == 0) {
            *others += !guest;
            continue;
        }
        platform_pmu_handled();
        if ((lvtpc & LAPIC_LVT_MASKED) != 0) {
            (*masked)++;
        }
        if (due && n < max) {
            due_ms[n] = ms;
        }
        n += due;
        *others += !due;
    }
    return n;
}

/**
 * Run the clock `seconds` in steps of a millisecond, stopping at each
 * overflow to take its NMI at once, and store the millisecond of each due
 * one in `due_ms`, at most `max`. Returns how many were due; `*others`
 * counts the rest and `*masked` the NMIs after which the entry stayed
 * masked.
 */
static int drive(int seconds, long *due_ms, int max, int *others, int *masked)
{
    long ms;
    int n = 0;
    uint64_t left, room;

    *others = 0;
    *masked = 0;
    n = take_nmis(0, due_ms, n, max, others, masked);
    for (ms = 1; ms <= seconds * 1000L; ms++) {
        for (left = chip->hz / 1000u; left > 0; left -= room) {
            room = next_overflow(left);
            run(room);
            n = take_nmis(ms, due_ms, n, max, others, masked);
        }
    }
    return n;
}

/** Two guest counters' overflows in one NMI, which the APIC delivered
 * through the running counter's entry and masked it for. */
static void guest_row(void)
{
    long due_ms[4];
    int others, masked;

    guest_ovf = 2;
    lvtpc |= LAPIC_LVT_MASKED;
    nmi_pending = 1;
    drive(0, due_ms, 4, &others, &masked);
    check(others == 0 && guest_handled == 2 && guest_ovf == 0,
          "a guest counter's NMI is claimed, each overflow counted");
    check(guest_masked && lvtpc == LAPIC_LVT_NMI,
          "the overflows are acknowledged before the entry is unmasked");
}

/**
 * A probe that finds a counter, and the counter at work and stopped.
 * `want_ctl` is the value the enable writes last, to fixed counter 1's
 * control when `want_fixed` and to the detector's general counter's event
 * select otherwise.
 */
static void counter_row(const struct chip *c, const char *line, int want_fixed, uint32_t want_ctl,
                        int want_loads)
{
    long due_ms[4];
    int n, others, masked, writes;
    uint64_t period = c->hz * 10u;
    uint64_t want_global = want_fixed ? FIXED1_GLOBAL : 1u;
    uint64_t *ctl = want_fixed ? &fixed_ctrl : &evtsel;

    reset(c);
    check(platform_pmu_probe() == 1, "the probe finds the counter");
    check(strstr(logged, line) != NULL, line);
    check(fixed == want_fixed,
          want_fixed ? "the counter is fixed counter 1" : "the counter is general");
    check(counter_index == chip_gp(), "the general counter is the chip's");
    check(platform_cpu_hz() == c->hz, "the clock is the processor's");
    check(evtsel == 0 && fixed_ctrl == 0, "the probe leaves the counters stopped");
    check(other_evtsel[1] == 0, "the probe stops the counter the firmware left");

    platform_pmu_enable(period);
    check(*ctl == want_ctl, "the control counts cycles with an interrupt");
    check(want_fixed ? evtsel == 0 : fixed_ctrl == 0, "the other counter stays stopped");
    check(last_msr_written == (int)(want_fixed ? IA32_FIXED_CTR_CTRL : evtsel_msr()) &&
              last_value_written == want_ctl,
          "the enable is the last write");
    check(lvtpc == LAPIC_LVT_NMI, "the counter's entry is an unmasked NMI");
    check(chip_amd() || chip_version() < 2 || global_ctrl == want_global,
          "the global control enables the detector's counter alone");

    n = drive(25, due_ms, 4, &others, &masked);
    check(n == 2, "two NMIs are due in 25 seconds");
    check(n >= 1 && due_ms[0] >= 9999 && due_ms[0] <= 10001, "the first is due at 10 s");
    check(n >= 2 && due_ms[1] >= 19999 && due_ms[1] <= 20002, "the second is due at 20 s");
    /* The last 5 seconds take whole loads of 2^31 - 1 too. */
    check(others == 2 * (want_loads - 1) + (want_loads > 1 ? (int)(c->hz * 5u / 0x7FFFFFFFu) : 0),
          "each period takes its loads");
    check(masked == 0, "the entry is unmasked after each NMI");
    check(global_status == 0, "the overflow status is cleared");

    writes = msr_writes;
    nmi_pending = 1;
    drive(0, due_ms, 4, &others, &masked);
    check(others == 1 && (chip_amd() || chip_version() < 2 || msr_writes == writes),
          "an NMI of another source is not the counter's");

    guest_row();

    /* A late NMI, delivered after the arming cleared the status it set:
     * the APIC masked the entry, and nothing shows an overflow. */
    lvtpc |= LAPIC_LVT_MASKED;
    nmi_pending = 1;
    drive(0, due_ms, 4, &others, &masked);
    check(others == 1 && lvtpc == LAPIC_LVT_NMI, "a late NMI unmasks the entry");

    if (chip_amd()) {
        /* An overflow whose hard-lockup report takes a second. */
        counter = chip_mask();
        run(1);
        nmi_pending = 0;
        check(platform_pmu_overflow(&n) == 1, "the overflow is the counter's");
        tsc += c->hz;
        check(!platform_pmu_late(), "the window is closed during the callback");
        platform_pmu_handled();
        check(platform_pmu_late(), "the window opens after the callback");
        tsc += c->hz / 10u;
        check(!platform_pmu_late(), "the window closes 100 ms after the callback");
    }

    platform_pmu_disable();
    check(*ctl == 0, "the disable stops the counter");
    check((lvtpc & LAPIC_LVT_MASKED) != 0, "the disable masks the entry");
    check(chip_amd() || chip_version() < 2 || (global_ctrl & want_global) == 0,
          "the disable clears the global enable");
    n = drive(25, due_ms, 4, &others, &masked);
    check(n == 0 && others == 0, "a stopped counter raises nothing");
    nmi_pending = 1;
    drive(0, due_ms, 4, &others, &masked);
    check(others == 1 && (lvtpc & LAPIC_LVT_MASKED) != 0,
          "an NMI leaves a stopped counter's entry masked");
    check(fault[0] == '\0', fault[0] != '\0' ? fault : "no #GP");
}

/** A probe that finds no counter, and an enable that writes nothing. */
static void none_row(const struct chip *c, const char *line)
{
    int writes;

    reset(c);
    check(platform_pmu_probe() == 0, "the probe finds no counter");
    check(strstr(logged, line) != NULL, line);
    check(evtsel == 0 && (fixed_ctrl & 0x30u) == 0, "the counters are left stopped");
    writes = msr_writes;
    platform_pmu_enable(c->hz * 10u);
    platform_pmu_disable();
    check(msr_writes == writes && lvtpc == (LAPIC_LVT_MASKED | LAPIC_LVT_NMI),
          "the enable and disable touch nothing");
    check(fault[0] == '\0', fault[0] != '\0' ? fault : "no #GP");
}

/* Leaf 0xA's EAX: version 3, four counters, 48 bits, seven events; its
 * EDX: three fixed counters, 48 bits. */
#define HSW_LEAF_A   0x07300403u
#define HSW_LEAF_A_D 0x0603u
#define PDCM         (1u << 15)
#define HYPERVISOR   (1u << 31)

static const struct chip haswell = {
    "i3-4030U", "GenuineIntel", 0xDu,         HSW_LEAF_A, 0, PDCM, 0x80000008u, 0, 0x32C4u, 1, 0,
    1,          1895611000u,    HSW_LEAF_A_D, 0};
static const struct chip no_fw = {"version 3 without full-width writes",
                                  "GenuineIntel",
                                  0xDu,
                                  HSW_LEAF_A,
                                  0,
                                  PDCM,
                                  0x80000008u,
                                  0,
                                  0x00C3u,
                                  1,
                                  0,
                                  0,
                                  2400000000u,
                                  HSW_LEAF_A_D,
                                  0};
static const struct chip no_fixed = {"a hypervisor's version 3 without fixed counters",
                                     "GenuineIntel",
                                     0xDu,
                                     HSW_LEAF_A,
                                     0,
                                     PDCM | HYPERVISOR,
                                     0x80000008u,
                                     0,
                                     0x32C4u,
                                     1,
                                     0,
                                     1,
                                     1895611000u,
                                     0,
                                     0};
static const struct chip v1 = {"version 1",
                               "GenuineIntel",
                               0xAu,
                               0x07280201u,
                               0,
                               0,
                               0x80000008u,
                               0,
                               0,
                               1,
                               0,
                               1,
                               1600000000u,
                               0,
                               0};
static const struct chip ryzen = {"AMD core counters",
                                  "AuthenticAMD",
                                  0x10u,
                                  0,
                                  0,
                                  0,
                                  0x8000001Fu,
                                  1u << 23,
                                  0,
                                  1,
                                  0,
                                  1,
                                  3800000000u,
                                  0,
                                  0};
static const struct chip piledriver = {"AMD family 15h",
                                       "AuthenticAMD",
                                       0xDu,
                                       0,
                                       0,
                                       0,
                                       0x8000001Eu,
                                       1u << 23,
                                       0,
                                       1,
                                       0,
                                       1,
                                       3500000000u,
                                       0,
                                       0x00600F20u};
static const struct chip dead = {"a hypervisor's counter that does not count",
                                 "GenuineIntel",
                                 0xDu,
                                 HSW_LEAF_A,
                                 0,
                                 PDCM,
                                 0x80000008u,
                                 0,
                                 0x32C4u,
                                 1,
                                 1,
                                 1,
                                 1895611000u,
                                 HSW_LEAF_A_D,
                                 0};
static const struct chip no_cycles = {
    "no cycles event", "GenuineIntel", 0xDu, HSW_LEAF_A, 1u, PDCM, 0x80000008u, 0, 0x32C4u, 1, 0, 1,
    1895611000u,       HSW_LEAF_A_D,   0};
static const struct chip no_msr = {
    "no msrs", "GenuineIntel", 0xDu,         HSW_LEAF_A, 0, PDCM, 0x80000008u, 0, 0x32C4u, 0, 0,
    1,         1895611000u,    HSW_LEAF_A_D, 0};

int main(void)
{
    /* Fixed counter 1's field with OS, USR and PMI; or 0x3C or 0x76 with
     * USR, OS, INT and EN. */
    counter_row(&haswell, "counter: intel arch version 3, fixed counter 1, 48 bits, 1895 MHz", 1,
                0xB0u, 1);
    /* 24e9 cycles in loads of 2^31 - 1: twelve. */
    counter_row(&no_fw, "counter: intel arch version 3, fixed counter 1, 48 bits, 2400 MHz", 1,
                0xB0u, 12);
    counter_row(&no_fixed, "counter: intel arch version 3, counter 0, 48 bits, 1895 MHz", 0,
                0x53003Cu, 1);
    /* 16e9 cycles in loads of 2^31 - 1: eight. */
    counter_row(&v1, "counter: intel arch version 1, counter 0, 40 bits, 1600 MHz", 0, 0x53003Cu,
                8);
    counter_row(&ryzen, "counter: amd core version 0, counter 0, 48 bits, 3800 MHz", 0, 0x530076u,
                1);
    counter_row(&piledriver, "counter: amd core version 0, counter 2, 48 bits, 3500 MHz", 0,
                0x530076u, 1);
    none_row(&dead, "no counter: fixed counter 1 does not count");
    none_row(&no_cycles, "no counter: the processor lists no cycles counter");
    none_row(&no_msr, "no counter: no local apic or no msrs");
    if (failures == 0) {
        printf("pmu_hosttest: ok\n");
    }
    return failures != 0;
}
