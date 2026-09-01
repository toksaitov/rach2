/*
 * lapic_hosttest.c: the local APIC driver against a model of the APIC page
 * and of the model-specific register that says where it is.
 *
 * host-deps: rach2/sys/lapic.c
 *
 * No machine can be made to answer `cpuid` differently, to come up in
 * x2APIC mode, or to hand the kernel a firmware that masked `LINT0`; each is
 * a state this driver must recognise, and each is one variable here. The
 * rows check what the driver wrote to the model, not what it printed: the
 * task priority, the two local-vector entries, the spurious register, and
 * the one write an end-of-interrupt is.
 *
 * The order matters as much as the values. Enabling the APIC with `LINT0`
 * masked stops the 8259's interrupts at the processor, so the row for a
 * masked entry requires the driver to have written the unmasked
 * external-interrupt value before the software enable.
 */

#include "../../rach2/sys/lapic.h"

extern int printf(const char *fmt, ...);
extern void exit(int status);

/* ---- the model ---- */

#define APIC_WORDS 1024

/* The registers, through the interface the driver takes, so that no address
   games are needed: the model is an array and the driver never dereferences
   a truncated pointer. Every write is recorded in order, since the order of
   two of them is what keeps the 8259's interrupts arriving. */
static unsigned int apic_page[APIC_WORDS];
static unsigned int write_order[64];
static int writes;

static unsigned int model_read(unsigned int reg)
{
    return apic_page[reg / 4];
}

static void model_write(unsigned int reg, unsigned int value)
{
    apic_page[reg / 4] = value;
    if (writes < 64) {
        write_order[writes++] = reg;
    }
}

static const struct lapic_io model_io = {model_read, model_write};

static unsigned int apic_base_lo, apic_base_hi;
static int has_apic, has_msr;
static int nomsi;
static int wrmsr_calls;
static unsigned int wrmsr_lo;
static int svr_written_after_lint0;
static int lint0_write_seen;

int kcpu_apic_present(void)
{
    return has_apic;
}

int kcpu_msr_present(void)
{
    return has_msr;
}

void kcpu_id(unsigned int leaf, unsigned int *a, unsigned int *b, unsigned int *c, unsigned int *d)
{
    (void)leaf;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
}

void kcpu_rdmsr(unsigned int msr, unsigned int *lo, unsigned int *hi)
{
    (void)msr;
    if (lo) {
        *lo = apic_base_lo;
    }
    if (hi) {
        *hi = apic_base_hi;
    }
}

void kcpu_wrmsr(unsigned int msr, unsigned int lo, unsigned int hi)
{
    (void)msr;
    (void)hi;
    wrmsr_calls++;
    wrmsr_lo = lo;
    apic_base_lo = lo;
}

int kcmdline_has(const char *flag)
{
    const char *p = "rach2.nomsi";
    int i;

    for (i = 0; flag[i] && p[i]; i++) {
        if (flag[i] != p[i]) {
            return 0;
        }
    }
    return nomsi && flag[i] == p[i];
}

/* The APIC's page: the driver's writes land here, and the order of the two
   that matter is recorded. */
unsigned int map_mmio(unsigned int phys, unsigned int size)
{
    (void)size;
    /* Never reached with the model installed; a base the driver refuses is
       modelled by the register instead. */
    return phys == 0xFEE00000u ? phys : 0;
}

void klog(int level, const char *subsystem, const char *fmt, ...)
{
    (void)level;
    (void)subsystem;
    (void)fmt;
}

/* ---- the rows ---- */

static int fails;

static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("lapic_hosttest: FAIL: %s\n", what);
    }
}

static void reset_model(unsigned int lint0)
{
    int i;

    for (i = 0; i < APIC_WORDS; i++) {
        apic_page[i] = 0;
    }
    apic_page[0x350 / 4] = lint0;
    apic_page[0x360 / 4] = 0x8400;     /* the firmware's non-maskable entry */
    apic_page[0x20 / 4] = 3u << 24;    /* an identity that is not zero */
    apic_page[0x30 / 4] = 0x00050014u; /* version 0x14, six entries */
    apic_page[0x80 / 4] = 0x20;        /* a task priority that must be cleared */
    writes = 0;
    lapic_set_io(&model_io);
    apic_base_lo = 0xFEE00000u | (1u << 11) | (1u << 8);
    apic_base_hi = 0;
    has_apic = 1;
    has_msr = 1;
    nomsi = 0;
    wrmsr_calls = 0;
    svr_written_after_lint0 = 0;
    lint0_write_seen = 0;
}

int main(void)
{
    /* A machine as QEMU leaves it: LINT0 already an unmasked external
       interrupt, so the driver leaves it alone. */
    reset_model(0x8700);
    check(lapic_init() == 0, "an APIC in virtual-wire mode is enabled");
    check(lapic_ready(), "and reports itself ready");
    check(lapic_id() == 3, "the identity comes from bits 24 and up");
    check(apic_page[0x80 / 4] == 0, "the task priority is cleared");
    check(apic_page[0x350 / 4] == 0x8700, "LINT0 is left as the firmware set it");
    check(apic_page[0x360 / 4] == 0x8400, "LINT1 is left alone");
    check(apic_page[0xF0 / 4] == (0x100u | 0xFFu),
          "the spurious register is enabled with vector 255");
    check(wrmsr_calls == 0, "an APIC already enabled in the register is not written again");

    /* An end-of-interrupt is one write of zero to 0xB0. */
    apic_page[0xB0 / 4] = 0x1234;
    lapic_eoi();
    check(apic_page[0xB0 / 4] == 0, "the end-of-interrupt is one write of zero to 0xB0");

    /* A firmware that masked LINT0: the driver must write the unmasked
       external-interrupt entry, or the 8259's interrupts stop here. */
    reset_model(0x10700);
    check(lapic_init() == 0, "an APIC with LINT0 masked is enabled");
    check(apic_page[0x350 / 4] == (7u << 8), "with LINT0 set to an unmasked external interrupt");
    check(apic_page[0xF0 / 4] == (0x100u | 0xFFu), "and the spurious register enabled");

    /* A firmware that left LINT0 as a fixed interrupt: the same. */
    reset_model(0x00020);
    check(lapic_init() == 0, "an APIC whose LINT0 is not an external interrupt is enabled");
    check(apic_page[0x350 / 4] == (7u << 8), "with LINT0 corrected");

    /* The register's enable bit off: the driver writes it. */
    reset_model(0x8700);
    apic_base_lo = 0xFEE00000u;
    check(lapic_init() == 0, "an APIC disabled in the register is enabled");
    check(wrmsr_calls == 1 && (wrmsr_lo & (1u << 11)) != 0, "by one write that sets bit 11");

    /* The states the driver refuses, each leaving the model untouched. */
    reset_model(0x8700);
    has_apic = 0;
    check(lapic_init() == -1 && !lapic_ready(), "a processor without an APIC is refused");
    check(apic_page[0x80 / 4] == 0x20 && apic_page[0xF0 / 4] == 0, "and nothing is written");

    reset_model(0x8700);
    has_msr = 0;
    check(lapic_init() == -1, "a processor without the model-specific registers is refused");
    check(apic_page[0xF0 / 4] == 0, "and nothing is written");

    reset_model(0x8700);
    apic_base_lo |= 1u << 10;
    check(lapic_init() == -1, "x2APIC mode is refused");
    check(apic_page[0xF0 / 4] == 0, "and nothing is written");

    reset_model(0x8700);
    nomsi = 1;
    check(lapic_init() == -1, "rach2.nomsi refuses");
    check(apic_page[0xF0 / 4] == 0, "and nothing is written");

    /* The order: the task priority and LINT0 before the software enable. */
    reset_model(0x10700);
    (void)lapic_init();
    {
        int tpr_at = -1, lint0_at = -1, svr_at = -1, i;

        /* The first write of each: a second write of the spurious register
           later would hide an enable that came too early. */
        for (i = 0; i < writes; i++) {
            if (write_order[i] == 0x80 && tpr_at < 0) {
                tpr_at = i;
            }
            if (write_order[i] == 0x350 && lint0_at < 0) {
                lint0_at = i;
            }
            if (write_order[i] == 0xF0 && svr_at < 0) {
                svr_at = i;
            }
        }
        check(tpr_at >= 0 && lint0_at >= 0 && svr_at >= 0 && tpr_at < svr_at && lint0_at < svr_at,
              "the task priority and LINT0 are written before the software enable");
    }

    /* The repair the boot asks for when the timer stops after the APIC is
       enabled: `LINT0` written again, and nothing else touched. */
    reset_model(0x8700);
    (void)lapic_init();
    apic_page[0x350 / 4] = 0x10700; /* masked behind the driver's back */
    apic_page[0xF0 / 4] = 0;
    lapic_repair_lint0();
    check(apic_page[0x350 / 4] == (7u << 8), "the repair writes LINT0 as an external interrupt");
    check(apic_page[0xF0 / 4] == 0, "and writes nothing else");

    reset_model(0x8700);
    has_apic = 0;
    (void)lapic_init();
    apic_page[0x350 / 4] = 0x10700;
    lapic_repair_lint0();
    check(apic_page[0x350 / 4] == 0x10700, "an APIC that was refused is not repaired");

    /* The spurious vector's handler counts and returns. */
    reset_model(0x8700);
    (void)lapic_init();
    {
        unsigned int before = lapic_spurious_count();

        lapic_spurious();
        lapic_spurious();
        check(lapic_spurious_count() == before + 2, "the spurious handler counts");
    }
    (void)svr_written_after_lint0;
    (void)lint0_write_seen;

    if (fails == 0) {
        printf("lapic_hosttest: OK\n");
        return 0;
    }
    printf("lapic_hosttest: %d failure(s)\n", fails);
    return 1;
}
