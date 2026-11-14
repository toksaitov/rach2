/*
 * kvmclock_hosttest.c: KVM's paravirtual clock as its guest keeps it,
 * rach2/sys/arch/x86/kvmclock.c, against a model of KVM: the features and
 * MSRs it offers, the clock pages it fills and the wall clock it states.
 *
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/kirq_none.c
 * host-cflags: -DRACH2_HOST_CPU
 *
 * The source is included whole, so that a row reaches each processor's
 * page. The rows check which MSRs the boot chooses for each of KVM's two
 * clock features, and that `no-kvmclock`, `rach2.notsc`, a page KVM left
 * unwritten and a rate past 32 bits keep no page (`decide_rows`); that
 * each processor registers its own page, 32 bytes after the one before,
 * and a stop takes the running processor's back (`cpu_rows`); the
 * conversion of the TSC through a page, with a shift either way, a
 * difference past 32 bits and a count before the stamp, against the
 * host's own 128-bit product, and the counts in a tick a page's rate
 * gives (`scale_rows`); that a reading made while KVM writes the page, its
 * version odd or moved, is made again (`retry_rows`); that with the stable
 * bit a reading is the page's own, and without it, or without the feature
 * that gives the bit its meaning, no reading on one processor is earlier
 * than one given on another, Linux's last_value, while the page the vDSO
 * reads is refused (`stable_rows`); that the tick's sampling reports a new
 * rate or bit once (`sample_rows`); that a page's stopped flag is cleared
 * and the soft-lockup detector touched, by the check and by a reading
 * (`paused_rows`); and that real time is KVM's wall clock plus its clock
 * (`wall_rows`).
 *
 * Returns 0 when every row passes, 1 otherwise.
 */
#include "arch/x86/kvmclock.c"

extern int dprintf(int fd, const char *format, ...);
extern void *memset(void *s, int c, unsigned long n);
extern int strcmp(const char *a, const char *b);

unsigned int host_cpu;
static int failures;

#define CHECK(test, message)                                        \
    do {                                                            \
        if (!(test)) {                                              \
            failures++;                                             \
            dprintf(2, "kvmclock-hosttest: failed: %s\n", message); \
        }                                                           \
    } while (0)

/* The model of KVM and of the machine: the features `cpuid` reports, the
 * command line's one flag, the MSR writes so far with the last one's
 * register and value, the TSC, the touches of the soft-lockup detector,
 * and what KVM writes into a page when it is registered: nothing while
 * `fill_version` is 0, or else a page of that version at `fill_mul` and
 * `fill_shift` with `fill_flags`. */
static uint32_t features;
static const char *cmdline_flag;
static unsigned int writes, last_msr;
static uint64_t msr_value;
static uint64_t tsc_now;
static int touches;
static uint32_t fill_version, fill_mul;
static int fill_shift;
static uint8_t fill_flags;

/* The wall clock KVM writes when given its record: its version, 0 for no
 * write, and its time. */
static uint32_t wall_version, wall_sec, wall_nsec;

/* KVM's write of a page in the middle of a reading: at the `write_at`-th
 * fence from now, 0 for none, the page `write_page` takes the version
 * `write_version` and the system time `write_system`. */
static int write_at;
static struct kvmclock_page *write_page;
static uint32_t write_version;
static uint64_t write_system;

/** The modelled processor has MSRs. */
int kcpu_msr_present(void)
{
    return 1;
}

/** KVM's features as the row has them. */
uint32_t kcpu_kvm_features(void)
{
    return features;
}

/** Whether the command line has `flag`: only the row's. */
int kcmdline_has(const char *flag)
{
    return cmdline_flag != 0 && strcmp(flag, cmdline_flag) == 0;
}

/** Record the write. A clock page turned on is filled as the row says, the
 * running processor's; a wall clock record given is written. */
void kcpu_wrmsr(uint32_t msr, uint32_t lo, uint32_t hi)
{
    struct kvmclock_page *p = kvmclock_of((int)host_cpu);

    writes++;
    last_msr = msr;
    msr_value = ((uint64_t)hi << 32) | lo;
    if ((msr == MSR_KVM_SYSTEM_TIME || msr == MSR_KVM_SYSTEM_TIME_NEW) && (lo & 1u) &&
        fill_version != 0) {
        p->version = fill_version;
        p->tsc_timestamp = 0;
        p->system_time = 0;
        p->tsc_to_system_mul = fill_mul;
        p->tsc_shift = (signed char)fill_shift;
        p->flags = fill_flags;
    }
    if ((msr == MSR_KVM_WALL_CLOCK || msr == MSR_KVM_WALL_CLOCK_NEW) && wall_version != 0) {
        wall_record.version = wall_version;
        wall_record.sec = wall_sec;
        wall_record.nsec = wall_nsec;
    }
}

/** The fence of a reading: KVM's write of the page, when the row has one
 * due here. */
void arch_dma_fence(void)
{
    if (write_at != 0 && --write_at == 0) {
        write_page->version = write_version;
        write_page->system_time = write_system;
    }
}

/** The TSC as the row has it. */
uint64_t tsc_ordered(void)
{
    return tsc_now;
}

/** Count the touch. */
void touch_softlockup_watchdog(void)
{
    touches++;
}

/** tick.c's 64-bit division, which the host does itself. */
uint64_t tick_div64(uint64_t n, uint64_t d, uint64_t *rem)
{
    if (rem) {
        *rem = n % d;
    }
    return n / d;
}

/** A boot not yet decided, on processor 0, with no page written, KVM
 * offering `f` and filling each page registered at 2 GHz with `flags`. */
static void boot(uint32_t f, uint8_t flags)
{
    asked = kept = stable_valid = stable_now = 0;
    msr_system_time = msr_wall_clock = counts_now = boot_counts = 0;
    last_value = 0;
    memset(kvmclock_room, 0, sizeof(kvmclock_room));
    memset(&wall_record, 0, sizeof(wall_record));
    features = f;
    cmdline_flag = 0;
    writes = last_msr = 0;
    host_cpu = 0;
    tsc_now = 0;
    touches = 0;
    fill_version = 2;
    fill_mul = 0x80000000u; /* half a nanosecond a count */
    fill_shift = 0;
    fill_flags = flags;
    wall_version = 0;
    write_at = 0;
}

/** The value a registration of processor `cpu`'s page writes. */
static uint64_t page_value(int cpu)
{
    return (uint64_t)virt_to_phys(kvmclock_of(cpu)) | 1u;
}

#define BOTH   (KVM_FEATURE_CLOCKSOURCE | KVM_FEATURE_CLOCKSOURCE2)
#define STABLE KVM_FEATURE_CLOCKSOURCE_STABLE_BIT

/** The boot's decision. */
static void decide_rows(void)
{
    boot(0, 0);
    CHECK(kvmclock_counts_per_tick() == 0 && !kvmclock_kept() && writes == 0,
          "without KVM's clock features no page is given and none kept");

    boot(BOTH | STABLE, PVCLOCK_TSC_STABLE_BIT);
    CHECK(kvmclock_counts_per_tick() == 2u * TICK_NS && kvmclock_kept(),
          "the second feature keeps the pages at the rate the boot page states");
    CHECK(writes == 1 && last_msr == 0x4B564D01u && msr_value == page_value(0),
          "the boot page given with MSR_KVM_SYSTEM_TIME_NEW, its address with bit 0 set");
    CHECK(kvmclock_stable() && kvmclock_counts_per_tick() == 2u * TICK_NS && writes == 1,
          "with its stable bit, and the answer kept without another write");

    boot(KVM_FEATURE_CLOCKSOURCE, 0);
    CHECK(kvmclock_counts_per_tick() == 2u * TICK_NS && writes == 1 && last_msr == 0x12u,
          "the first feature alone gives the page with the older MSR");
    CHECK(kvmclock_kept() && !kvmclock_stable(), "and keeps it without the stable bit");

    boot(BOTH | STABLE, PVCLOCK_TSC_STABLE_BIT);
    cmdline_flag = "no-kvmclock";
    CHECK(kvmclock_counts_per_tick() == 0 && !kvmclock_kept() && writes == 0,
          "no-kvmclock gives KVM no page");
    boot(BOTH | STABLE, PVCLOCK_TSC_STABLE_BIT);
    cmdline_flag = "rach2.notsc";
    CHECK(kvmclock_counts_per_tick() == 0 && writes == 0, "nor does rach2.notsc");

    boot(BOTH, 0);
    fill_version = 0;
    CHECK(kvmclock_counts_per_tick() == 0 && !kvmclock_kept(),
          "a page KVM left unwritten is not kept");
    CHECK(writes == 2 && last_msr == 0x4B564D01u && msr_value == 0, "and is taken back");

    boot(BOTH, 0);
    fill_mul = 1;
    CHECK(kvmclock_counts_per_tick() == 0 && !kvmclock_kept() && msr_value == 0,
          "nor is one whose rate does not fit 32 bits");
    boot(BOTH, 0);
    CHECK(kvmclock_vsyscall(), "the vDSO may read KVM's clock");
    cmdline_flag = "no-kvmclock-vsyscall";
    CHECK(!kvmclock_vsyscall() && kvmclock_kept(),
          "but not under no-kvmclock-vsyscall, which keeps the pages");
}

/** Each processor's registration and its stop. */
static void cpu_rows(void)
{
    boot(BOTH, 0);
    kvmclock_cpu_setup(1);
    CHECK(writes == 0, "a processor registers nothing before the boot kept the pages");
    kvmclock_cpu_stop();
    CHECK(writes == 0, "and takes nothing back");
    (void)kvmclock_counts_per_tick();
    writes = 0;
    kvmclock_cpu_setup(0);
    CHECK(writes == 0, "the boot processor's page is given once, by the boot");
    host_cpu = 1;
    kvmclock_cpu_setup(1);
    CHECK(writes == 1 && last_msr == 0x4B564D01u && msr_value == page_value(1) &&
              page_value(1) - page_value(0) == 32u,
          "the second processor gives its own page, 32 bytes on");
    host_cpu = NR_CPUS - 1;
    kvmclock_cpu_setup(NR_CPUS - 1);
    CHECK(writes == 2 && msr_value == page_value(NR_CPUS - 1) &&
              page_value(NR_CPUS - 1) - page_value(0) < PAGE_SIZE,
          "the last processor's page lies in the same page of memory");
    kvmclock_cpu_setup(-1);
    kvmclock_cpu_setup(NR_CPUS);
    CHECK(writes == 2, "no processor outside the table registers");
    CHECK(((uint64_t)kvmclock_page_frame() | 1u) == page_value(0),
          "the page the vDSO maps begins with the boot processor's");
    kvmclock_cpu_stop();
    CHECK(writes == 3 && last_msr == 0x4B564D01u && msr_value == 0,
          "a stop takes the running processor's page back");

    boot(KVM_FEATURE_CLOCKSOURCE, 0);
    (void)kvmclock_counts_per_tick();
    kvmclock_cpu_stop();
    CHECK(writes == 2 && last_msr == 0x12u && msr_value == 0,
          "by the older MSR where that gave it");
}

/** The host's own conversion of `delta` counts at `mul` and `shift`. */
static uint64_t scale_want(uint64_t delta, uint32_t mul, int shift)
{
    if (shift < 0) {
        delta >>= -shift;
    } else {
        delta <<= shift;
    }
    return (uint64_t)(((unsigned __int128)delta * mul) >> 32);
}

/** One page's reading at the TSC `tsc`: the page of processor 0 with the
 * stamp `stamp`, the system time `system`, `mul` and `shift`. */
static uint64_t page_ns(uint64_t tsc, uint64_t stamp, uint64_t system, uint32_t mul, int shift)
{
    struct kvmclock_page *p = kvmclock_of(0);
    uint64_t at, ns = 0;

    last_value = 0; /* each row's reading stands alone */
    p->version += 2;
    p->tsc_timestamp = stamp;
    p->system_time = system;
    p->tsc_to_system_mul = mul;
    p->tsc_shift = (signed char)shift;
    tsc_now = tsc;
    CHECK(kvmclock_now(&at, &ns) && at == tsc, "a reading gives the TSC it was made at");
    return ns;
}

/** The conversion and the rate. */
static void scale_rows(void)
{
    static const struct {
        uint64_t delta;
        uint32_t mul;
        int shift;
    } rows[] = {
        {2000, 0x80000000u, 0},
        {4000, 0x80000000u, -1},
        {1000, 0x80000000u, 1},
        {((uint64_t)1 << 40) + 12345u, 0x80000000u, 0},
        {((uint64_t)1 << 33) + 5u, 0xFFFFFFFFu, 0},
        {0xFFFFFFFFFFull, 0x55555555u, 2},
        {0x123456789ABCull, 0xDEADBEEFu, -7},
        {1, 0xFFFFFFFFu, 0},
        {0xFFFFFFFFu, 0xFFFFFFFFu, 0},
        {((uint64_t)1 << 32), 1, 0},
    };
    struct kvmclock_sample s;
    unsigned int i;
    int ok = 1;

    boot(BOTH | STABLE, PVCLOCK_TSC_STABLE_BIT);
    (void)kvmclock_counts_per_tick();
    CHECK(page_ns(3000, 1000, 5000000000ull, 0x80000000u, 0) == 5000001000ull,
          "2000 counts of 2 GHz past the stamp are 1000 ns past the system time");
    CHECK(page_ns(5000, 1000, 77, 0x80000000u, -1) == 77u + 1000u,
          "a shift of -1 halves the counts first");
    CHECK(page_ns(2000, 1000, 77, 0x80000000u, 1) == 77u + 1000u, "a shift of 1 doubles them");
    CHECK(page_ns(999, 1000, 4242, 0x80000000u, 0) == 4242u,
          "a count before the stamp reads the system time");
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (page_ns(500u + rows[i].delta, 500, 9, rows[i].mul, rows[i].shift) !=
            9u + scale_want(rows[i].delta, rows[i].mul, rows[i].shift)) {
            ok = 0;
        }
    }
    CHECK(ok, "the conversion is the 128-bit product's, a difference past 32 bits too");

    CHECK(kvmclock_counts(0x80000000u, 0) == 2u * TICK_NS, "2 GHz is two counts a nanosecond");
    CHECK(kvmclock_counts(0x80000000u, -1) == 4u * TICK_NS, "with a shift of -1, four");
    CHECK(kvmclock_counts(0x80000000u, 1) == TICK_NS, "with a shift of 1, one");
    CHECK(kvmclock_counts(0x55555555u, 0) == (uint32_t)(((uint64_t)TICK_NS << 32) / 0x55555555u),
          "3 GHz is the quotient of a tick's nanoseconds by 2^32 and the multiplier");
    CHECK(kvmclock_counts(0, 0) == 0 && kvmclock_counts(1, 0) == 0 &&
              kvmclock_counts(0x80000000u, -32) == 0 && kvmclock_counts(0x80000000u, 32) == 0 &&
              kvmclock_counts(0x1000u, -31) == 0,
          "no multiplier, a rate past 32 bits and a shift out of range are no rate");
    (void)page_ns(1000, 0, 0, 1, 0);
    CHECK(kvmclock_read(&s) && s.counts == 0 && !s.stable,
          "a page whose rate does not fit reads as no rate and no stable bit");
    CHECK(kvmclock_sample(&s) == KVMCLOCK_CHANGED && s.counts == 2u * TICK_NS && !s.stable,
          "and the tick keeps the last rate, the bit taken as withdrawn");
}

/** A reading while KVM writes the page. */
static void retry_rows(void)
{
    struct kvmclock_page *p = kvmclock_of(0);
    uint64_t tsc, ns = 0;

    boot(BOTH | STABLE, PVCLOCK_TSC_STABLE_BIT);
    (void)kvmclock_counts_per_tick();
    p->version = 5; /* KVM is writing */
    p->system_time = 111;
    tsc_now = 0;
    write_page = p;
    write_version = 6;
    write_system = 222;
    write_at = 2;
    CHECK(kvmclock_now(&tsc, &ns) && ns == 222, "a page with an odd version is read again");
    p->version = 8;
    p->system_time = 333;
    write_version = 10;
    write_system = 444;
    write_at = 1;
    CHECK(kvmclock_now(&tsc, &ns) && ns == 444,
          "as is one whose version moved between the reading's two looks");
    p->version = 11;
    write_version = 12;
    write_system = 555;
    write_at = 2;
    CHECK(kvmclock_boot_read(&tsc, &ns) && ns == 555,
          "the vDSO's reading waits out a write the same way");
    p->version = 13;
    write_version = 14;
    write_at = 2;
    CHECK(!kvmclock_boot_withdrawn(), "and so does the look at the stable bit");
}

/** The stable bit and Linux's last_value. */
static void stable_rows(void)
{
    struct kvmclock_page *p0 = kvmclock_of(0), *p1 = kvmclock_of(1);
    struct kvmclock_sample s;
    uint64_t tsc, ns = 0;

    boot(BOTH | STABLE, PVCLOCK_TSC_STABLE_BIT);
    (void)kvmclock_counts_per_tick();
    host_cpu = 1;
    CHECK(kvmclock_now(&tsc, &ns) && ns == 0 && p1->version == 0,
          "a processor whose page KVM has not written reads the boot processor's");
    kvmclock_cpu_setup(1);
    p0->system_time = 1000;
    p1->system_time = 900; /* this processor's page is 100 ns behind */
    host_cpu = 0;
    CHECK(kvmclock_read(&s) && s.ns == 1000 && s.stable, "the boot processor reads its page");
    host_cpu = 1;
    CHECK(kvmclock_read(&s) && s.ns == 900 && s.stable,
          "with the stable bit a reading is the page's own, as Linux returns it unheld");
    CHECK(kvmclock_boot_read(&tsc, &ns) && ns == 1000 && !kvmclock_boot_withdrawn(),
          "and the vDSO's page is the boot processor's, on any processor");

    p0->flags = 0;
    p1->flags = 0;
    host_cpu = 0;
    CHECK(kvmclock_read(&s) && s.ns == 1000 && !s.stable, "without the bit a reading says so");
    host_cpu = 1;
    CHECK(kvmclock_now(&tsc, &ns) && ns == 1000,
          "and one on a processor whose page is behind is held to the latest given");
    tsc_now = 400; /* 200 ns on */
    CHECK(kvmclock_now(&tsc, &ns) && ns == 1100, "until its own page passes that");
    host_cpu = 0;
    CHECK(kvmclock_now(&tsc, &ns) && ns == 1200, "the processor ahead reads on");
    host_cpu = 1;
    CHECK(kvmclock_now(&tsc, &ns) && ns == 1200, "and the one behind is held to it again");
    CHECK(!kvmclock_boot_read(&tsc, &ns) && kvmclock_boot_withdrawn(),
          "the vDSO's page without the bit is refused");

    boot(BOTH, PVCLOCK_TSC_STABLE_BIT);
    (void)kvmclock_counts_per_tick();
    kvmclock_of(0)->system_time = 1000;
    CHECK(kvmclock_kept() && !kvmclock_stable() && kvmclock_read(&s) && !s.stable,
          "a page's bit means nothing without the feature that gives it, Linux's valid_flags");
    CHECK(!kvmclock_boot_read(&tsc, &ns) && kvmclock_boot_withdrawn(),
          "so the vDSO's page is refused");
    kvmclock_of(0)->system_time = 500;
    CHECK(kvmclock_now(&tsc, &ns) && ns == 1000, "and a reading behind the last is held");
}

/** The tick's sampling. */
static void sample_rows(void)
{
    struct kvmclock_page *p = kvmclock_of(0);
    struct kvmclock_sample s;

    boot(0, 0);
    CHECK(kvmclock_sample(&s) == KVMCLOCK_NONE && !kvmclock_read(&s), "no page kept is no sample");
    boot(BOTH | STABLE, PVCLOCK_TSC_STABLE_BIT);
    (void)kvmclock_counts_per_tick();
    CHECK(kvmclock_sample(&s) == KVMCLOCK_SAME && s.counts == 2u * TICK_NS && s.stable,
          "a page as the boot read it is the same");
    p->version += 2;
    p->tsc_to_system_mul = 0x40000000u;
    CHECK(kvmclock_sample(&s) == KVMCLOCK_CHANGED && s.counts == 4u * TICK_NS,
          "a new rate is reported");
    CHECK(kvmclock_sample(&s) == KVMCLOCK_SAME && s.counts == 4u * TICK_NS, "once");
    p->version += 2;
    p->flags = 0;
    CHECK(kvmclock_sample(&s) == KVMCLOCK_CHANGED && !s.stable && !kvmclock_stable(),
          "a withdrawn stable bit is reported");
    CHECK(kvmclock_sample(&s) == KVMCLOCK_SAME && kvmclock_kept(), "once, and the pages stay kept");
    p->version += 2;
    p->flags = PVCLOCK_TSC_STABLE_BIT;
    CHECK(kvmclock_sample(&s) == KVMCLOCK_CHANGED && s.stable && kvmclock_stable(),
          "as is the bit set again");
}

/** The stopped flag. */
static void paused_rows(void)
{
    struct kvmclock_page *p0 = kvmclock_of(0), *p1 = kvmclock_of(1);
    uint64_t tsc, ns;

    boot(0, 0);
    CHECK(!arch_guest_paused() && touches == 0, "no page kept is no stop of the guest");
    boot(BOTH | STABLE, PVCLOCK_TSC_STABLE_BIT);
    (void)kvmclock_counts_per_tick();
    host_cpu = 1;
    kvmclock_cpu_setup(1);
    CHECK(!arch_guest_paused() && touches == 0, "a page without the flag is none");
    p1->flags |= PVCLOCK_GUEST_STOPPED;
    host_cpu = 0;
    CHECK(!arch_guest_paused(), "another processor's flag is not this one's");
    host_cpu = 1;
    CHECK(arch_guest_paused() && touches == 1 && p1->flags == PVCLOCK_TSC_STABLE_BIT,
          "the flag is reported, cleared, and the soft-lockup detector touched");
    CHECK(!arch_guest_paused() && touches == 1, "once");
    host_cpu = 0;
    p0->flags |= PVCLOCK_GUEST_STOPPED;
    CHECK(kvmclock_now(&tsc, &ns) && touches == 2 && p0->flags == PVCLOCK_TSC_STABLE_BIT,
          "a reading of the clock that meets the flag clears it and touches the detector");
    CHECK(!arch_guest_paused() && touches == 2, "and the check after it finds none");
}

/** Real time. */
static void wall_rows(void)
{
    int64_t sec = -1;
    uint32_t nsec = 7;

    boot(0, 0);
    wall_version = 2;
    CHECK(!kvmclock_wallclock(&sec, &nsec) && writes == 0 && sec == -1 && nsec == 7,
          "no page kept is no wall clock, and KVM is not asked");

    boot(BOTH, 0);
    (void)kvmclock_counts_per_tick();
    wall_version = 2;
    wall_sec = 1790000000u;
    wall_nsec = 999999999u;
    kvmclock_of(0)->system_time = 1500000002ull; /* the guest has run 1.5 s */
    CHECK(kvmclock_wallclock(&sec, &nsec) && sec == 1790000002 && nsec == 500000001u,
          "real time is KVM's wall clock at the guest's start plus KVM's clock");
    CHECK(last_msr == 0x4B564D00u && msr_value == (uint64_t)virt_to_phys(&wall_record),
          "asked with MSR_KVM_WALL_CLOCK_NEW, the record's address written");

    boot(KVM_FEATURE_CLOCKSOURCE, 0);
    wall_version = 4;
    wall_sec = 5;
    wall_nsec = 0;
    CHECK(kvmclock_wallclock(&sec, &nsec) && sec == 5 && nsec == 0 && last_msr == 0x11u,
          "or with the older MSR where the first feature alone is offered");

    boot(BOTH, 0);
    sec = -1;
    CHECK(!kvmclock_wallclock(&sec, &nsec) && sec == -1, "a record KVM left unwritten is no time");
}

/** Run the rows. */
int main(void)
{
    decide_rows();
    cpu_rows();
    scale_rows();
    retry_rows();
    stable_rows();
    sample_rows();
    paused_rows();
    wall_rows();
    dprintf(1, "kvmclock-hosttest: %s\n", failures ? "fail" : "pass");
    return failures != 0;
}
