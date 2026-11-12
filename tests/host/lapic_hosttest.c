/*
 * lapic_hosttest.c: the local APIC driver against a model of the APIC page
 * and of the model-specific register that says where it is.
 *
 * host-deps: rach2/sys/arch/x86/lapic.c
 *
 * No machine can be made to answer `cpuid` differently, to come up in
 * x2APIC mode, or to hand the kernel a firmware that masked `LINT0`; each is
 * a state this driver must recognise, and each is one variable here. The
 * rows check what the driver wrote to the model, not what it printed: the
 * task priority, the two local-vector entries, the spurious register, and
 * the one write an end-of-interrupt is.
 *
 * In x2APIC mode the same registers are MSRs from 0x800, one for each 16
 * bytes of the page, and the model keeps them in the same array. It counts
 * what the processor would fault on: a page access in x2APIC mode, an MSR
 * access outside it, and a move of IA32_APIC_BASE from x2APIC to xAPIC
 * mode that does not pass through the disabled state, which also resets
 * the registers as the SDM says.
 *
 * The order matters as much as the values. Enabling the APIC with `LINT0`
 * masked stops the 8259's interrupts at the processor, so the row for a
 * masked entry requires the driver to have written the unmasked
 * external-interrupt value before the software enable.
 */

#include "../../rach2/sys/arch/x86/lapic.h"

extern int printf(const char *fmt, ...);
extern void exit(int status);

/* ---- the model ---- */

#define APIC_WORDS 1024

/* The registers, through the interface the driver takes, so that no address
   is mapped: the model is an array and the driver never dereferences a
   truncated pointer. Every write is recorded in order, since the order of
   two of them is what keeps the 8259's interrupts arriving. */
static unsigned int apic_page[APIC_WORDS];
static unsigned int write_order[64]; /* the register offset of each write */
static int writes;

/* IA32_APIC_BASE, declared before the page's accessors that read it. */
static unsigned int apic_base_lo, apic_base_hi;

/* What the processor would fault on, and the x2APIC registers that are not
 * the page's: the 32-bit identity, the command register's last write and
 * how many, and the self-interrupt register's. */
static int faults;
static unsigned int x2_id;
static unsigned int icr_lo, icr_hi, self_vector;
static int icr_writes;

/** Read the register at byte offset `reg`. */
static unsigned int model_read(unsigned int reg)
{
    if (apic_base_lo & (1u << 10)) {
        faults++;
    }
    return apic_page[reg / 4];
}

/**
 * Write the register at byte offset `reg`, and record the write.
 *
 * The one rule of this model that is not a plain store: a processor whose
 * APIC is software disabled holds the mask bit of every local-vector entry
 * set and ignores a write that would reset one, so a driver that wrote
 * `LINT0` before the software enable would find it masked afterwards and the
 * 8259's interrupts would stop at the processor. Without this rule here the
 * order rows below would pass for a driver that does exactly that.
 */
static void model_write(unsigned int reg, unsigned int value)
{
    if (apic_base_lo & (1u << 10)) {
        faults++; /* the page is not decoded in x2APIC mode */
    }
    if ((reg == 0x350 || reg == 0x360) && (apic_page[0xF0 / 4] & 0x100u) == 0) {
        value |= 0x10000u; /* the mask bit stays set */
    }
    apic_page[reg / 4] = value;
    if (writes < 64) {
        write_order[writes++] = reg;
    }
}

static const struct lapic_io model_io = {model_read, model_write};

/* The processor's features, a hypervisor that allows x2APIC mode
 * unremapped, a firmware's lock of it, the command line's rach2.nomsi and
 * nox2apic, and the writes of IA32_APIC_BASE. */
static int has_apic, has_msr, has_x2apic, hypervisor, locked;
static int nomsi, nox2apic;
static int wrmsr_calls;
static unsigned int wrmsr_lo;

/** Whether `cpuid` reports an APIC: `has_apic`. */
int kcpu_apic_present(void)
{
    return has_apic;
}

/** Whether `cpuid` reports the model-specific registers: `has_msr`. */
int kcpu_msr_present(void)
{
    return has_msr;
}

/** Whether `cpuid` reports x2APIC mode: `has_x2apic`. */
int kcpu_x2apic_present(void)
{
    return has_x2apic;
}

/** Whether a hypervisor allows x2APIC mode unremapped. */
int kcpu_x2apic_unremapped(void)
{
    return has_x2apic && hypervisor;
}

/** Whether the firmware locked x2APIC mode. */
int kcpu_x2apic_locked(void)
{
    return locked;
}

/** A `cpuid` that writes nothing; the driver asks the calls above. */
void kcpu_id(unsigned int leaf, unsigned int *a, unsigned int *b, unsigned int *c, unsigned int *d)
{
    (void)leaf;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
}

/** Whether `msr` is an x2APIC register, and count a fault when the APIC is
 * not in x2APIC mode. */
static int x2_msr(unsigned int msr)
{
    if (msr < 0x800u || msr > 0x8FFu) {
        return 0;
    }
    if ((apic_base_lo & (1u << 10)) == 0) {
        faults++;
    }
    return 1;
}

/** Read IA32_APIC_BASE, or an x2APIC register: the identity whole, and the
 * others from the page's array. */
void kcpu_rdmsr(unsigned int msr, unsigned int *lo, unsigned int *hi)
{
    unsigned int l = apic_base_lo, h = apic_base_hi;

    if (x2_msr(msr)) {
        l = msr == 0x802u ? x2_id : apic_page[(msr - 0x800u) * 4u];
        h = 0;
    }
    if (lo) {
        *lo = l;
    }
    if (hi) {
        *hi = h;
    }
}

/** The registers as the processor resets them when its APIC is disabled:
 * the spurious register's enable clear and every local-vector entry
 * masked. */
static void model_disable(void)
{
    apic_page[0xF0 / 4] = 0xFFu;
    apic_page[0x80 / 4] = 0;
    apic_page[0x320 / 4] = 0x10000u;
    apic_page[0x340 / 4] = 0x10000u;
    apic_page[0x350 / 4] = 0x10000u;
    apic_page[0x360 / 4] = 0x10000u;
    apic_page[0x370 / 4] = 0x10000u;
    apic_page[0x380 / 4] = 0;
    apic_page[0x3E0 / 4] = 0;
}

/**
 * Write IA32_APIC_BASE, counting the write, or an x2APIC register: the
 * command register's two halves at once, the self-interrupt register, and
 * the others into the page's array. A move of the base register from
 * x2APIC mode straight to xAPIC mode faults; one to the disabled state
 * resets the registers.
 */
void kcpu_wrmsr(unsigned int msr, unsigned int lo, unsigned int hi)
{
    if (x2_msr(msr)) {
        if (msr == 0x830u) {
            icr_lo = lo;
            icr_hi = hi;
            icr_writes++;
        } else if (msr == 0x83Fu) {
            self_vector = lo;
        } else {
            apic_page[(msr - 0x800u) * 4u] = lo;
        }
        return;
    }
    wrmsr_calls++;
    wrmsr_lo = lo;
    if ((apic_base_lo & (1u << 10)) != 0 && (lo & (1u << 11)) != 0 && (lo & (1u << 10)) == 0) {
        faults++; /* x2APIC to xAPIC without the disabled state between */
    }
    if ((lo & (1u << 11)) == 0) {
        model_disable();
    }
    apic_base_lo = lo;
    apic_base_hi = hi;
}

/** No interrupt arrives in the middle of a row's command. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Restore nothing, as `kirq_save` saved nothing. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/** Whether `a` and `b` are the same string. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Whether the command line has `flag`: rach2.nomsi when `nomsi` is set,
 * nox2apic when `nox2apic` is. */
int kcmdline_has(const char *flag)
{
    return (nomsi && same(flag, "rach2.nomsi")) || (nox2apic && same(flag, "nox2apic"));
}

/** Map the APIC's page at its usual base, and nothing else. The driver does
 * not call it with the model installed; a base it rejects is set in the
 * register instead. */
unsigned int map_mmio(unsigned int phys, unsigned int size)
{
    (void)size;
    return phys == 0xFEE00000u ? phys : 0;
}

/** Discard the log: the rows read the model. */
void klog(int level, const char *subsystem, const char *fmt, ...)
{
    (void)level;
    (void)subsystem;
    (void)fmt;
}

/* ---- the rows ---- */

static int fails;

/** Count and print a failed row named `what`. */
static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("lapic_hosttest: FAIL: %s\n", what);
    }
}

/** A fresh model: an APIC page as a firmware leaves it, with `lint0` in
 * LINT0, enabled at 0xFEE00000 in the register, and a processor that has
 * the APIC and the model-specific registers. */
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
    has_x2apic = 0;
    hypervisor = 0;
    locked = 0;
    nomsi = 0;
    nox2apic = 0;
    wrmsr_calls = 0;
    faults = 0;
    x2_id = 3;
    icr_lo = icr_hi = self_vector = 0;
    icr_writes = 0;
}

/**
 * Check `lapic_pending`, which tells the USB stack whether a vector is waiting
 * at the processor before it counts an edge as lost.
 *
 * Vector 48 is bit 16 of the word at 0x210, and the registers are sixteen
 * bytes apart. The rows start from a fresh model, and the last one has no
 * APIC, to show that a driver that rejected it answers without reading its
 * page.
 */
static void rows_pending(void)
{
    reset_model(0x8700);
    (void)lapic_init();
    apic_page[0x210 / 4] = 0;
    apic_page[0x110 / 4] = 0;
    check(!lapic_pending(48), "a vector nothing has raised is not waiting");
    apic_page[0x210 / 4] = 1u << 16;
    check(lapic_pending(48), "a vector in the request register is waiting");
    check(!lapic_pending(49), "and its neighbour is not");
    apic_page[0x210 / 4] = 0;
    apic_page[0x110 / 4] = 1u << 16;
    check(lapic_pending(48), "a vector being handled counts as waiting too");
    apic_page[0x110 / 4] = 0;
    apic_page[0x200 / 4] = 0xFFFFFFFFu; /* vectors 0 to 31, a different word */
    check(!lapic_pending(48), "a vector in another word is not this one");
    apic_page[0x200 / 4] = 0;
    reset_model(0x8700);
    has_apic = 0;
    (void)lapic_init();
    apic_page[0x210 / 4] = 1u << 16;
    check(!lapic_pending(48), "and a refused APIC answers no rather than reading its page");
}

/** x2APIC mode under a hypervisor that allows it, from `lapic_init`. */
static void rows_x2apic_hypervisor(void)
{
    reset_model(0x8700);
    has_x2apic = 1;
    hypervisor = 1;
    x2_id = 0x103;
    check(lapic_init() == 0 && lapic_x2apic(), "under a hypervisor x2APIC mode is used");
    check((apic_base_lo & 0xC00u) == 0xC00u, "with the base register's bits 10 and 11 set");
    check(lapic_id() == 0x103 && lapic_this_id() == 0x103, "and the identity read whole");
    check(writes == 0, "and the page is not written");
    check(apic_page[0xF0 / 4] == (0x100u | 0xFFu) && apic_page[0x80 / 4] == 0,
          "the spurious register and the task priority are written through their MSRs");
    apic_page[0xB0 / 4] = 0x1234;
    lapic_eoi();
    check(apic_page[0xB0 / 4] == 0, "the end-of-interrupt is one write of zero to 0x80B");
    lapic_send_ipi(5, 0xF0);
    check(icr_writes == 1 && icr_hi == 5 && icr_lo == 0xF0,
          "an interprocessor interrupt is one write of the command, destination high");
    lapic_send_init(0x204);
    check(icr_writes == 2 && icr_hi == 0x204 && icr_lo == 0xC500u,
          "INIT names an identity past 255 the same way");
    lapic_self_ipi(0x31);
    check(self_vector == 0x31 && icr_writes == 2, "a message to itself writes 0x83F alone");
    lapic_timer_set(0x20, 1, 0, 1000);
    check(apic_page[0x380 / 4] == 1000 && apic_page[0x320 / 4] == 0x20020u &&
              apic_page[0x3E0 / 4] == 3,
          "the timer is set through its MSRs");
    check(lapic_x2apic_enable() == 0, "remapping finds the APIC in x2APIC mode already");
    check(faults == 0, "and nothing touched the page or faulted");
}

/** x2APIC mode not chosen: bare metal before remapping, `nox2apic`, and a
 * processor without the mode. */
static void rows_x2apic_not_chosen(void)
{
    reset_model(0x8700);
    has_x2apic = 1;
    check(lapic_init() == 0 && !lapic_x2apic(), "on bare metal xAPIC mode is kept at first");
    check((apic_base_lo & (1u << 10)) == 0 && wrmsr_calls == 0, "and the base is not written");
    check(lapic_x2apic_possible(), "but remapping may move it");

    reset_model(0x8700);
    has_x2apic = 1;
    hypervisor = 1;
    nox2apic = 1;
    check(lapic_init() == 0 && !lapic_x2apic() && !lapic_x2apic_possible(),
          "nox2apic keeps xAPIC mode under a hypervisor");

    reset_model(0x8700);
    hypervisor = 1;
    check(lapic_init() == 0 && !lapic_x2apic() && !lapic_x2apic_possible(),
          "a processor without x2APIC keeps xAPIC mode");
}

/** A firmware's x2APIC mode: left on bare metal, kept when locked, and
 * refused with an identity xAPIC mode cannot address. */
static void rows_x2apic_firmware(void)
{
    reset_model(0x8700);
    has_x2apic = 1;
    apic_base_lo |= 1u << 10;
    check(lapic_init() == 0 && !lapic_x2apic(),
          "a firmware's x2APIC mode on bare metal is returned to xAPIC mode");
    check((apic_base_lo & 0xC00u) == 0x800u && wrmsr_calls == 2,
          "through the disabled state, in two writes");
    check(apic_page[0xF0 / 4] == (0x100u | 0xFFu) && apic_page[0x350 / 4] == (7u << 8),
          "and the reset APIC is enabled with LINT0 an external interrupt again");
    check(faults == 0, "with no fault");

    reset_model(0x8700);
    has_x2apic = 1;
    locked = 1;
    apic_base_lo |= 1u << 10;
    check(lapic_init() == 0 && lapic_x2apic(), "a locked x2APIC mode is kept");
    check(writes == 0 && wrmsr_calls == 0 && faults == 0, "and the page is not touched");

    reset_model(0x8700);
    has_x2apic = 1;
    apic_base_lo |= 1u << 10;
    x2_id = 0x1FF;
    check(lapic_init() == -1 && !lapic_ready(),
          "a firmware's x2APIC mode with an identity past 254 is refused");
    check(wrmsr_calls == 0 && faults == 0, "and nothing is written");
}

/** The moves interrupt remapping makes, and the processors started after
 * them. */
static void rows_x2apic_moves(void)
{
    reset_model(0x8700);
    has_x2apic = 1;
    (void)lapic_init();
    lapic_timer_set(0x20, 1, 0, 777);
    lapic_set_lint(1, 0x400u);
    writes = 0;
    check(lapic_x2apic_enable() == 1 && lapic_x2apic(), "remapping moves the APIC to x2APIC mode");
    check((apic_base_lo & 0xC00u) == 0xC00u && lapic_id() == 3,
          "by setting bit 10, and reads the identity again");
    check(apic_page[0x380 / 4] == 777 && apic_page[0x360 / 4] == 0x400u &&
              apic_page[0xF0 / 4] == (0x100u | 0xFFu),
          "and every register is as it was");
    lapic_eoi();
    check(writes == 0 && faults == 0, "after which the page is never touched");
    lapic_x2apic_disable();
    check(!lapic_x2apic() && (apic_base_lo & 0xC00u) == 0x800u,
          "a failed remapping moves it back to xAPIC mode");
    check(apic_page[0x380 / 4] == 777 && apic_page[0x360 / 4] == 0x400u &&
              apic_page[0xF0 / 4] == (0x100u | 0xFFu) && apic_page[0x350 / 4] == 0x8700u &&
              apic_page[0x320 / 4] == 0x20020u,
          "with the registers the disabled state reset written again");
    check(faults == 0, "and no fault on the way");

    reset_model(0x8700);
    has_x2apic = 1;
    hypervisor = 1;
    (void)lapic_init();
    lapic_x2apic_disable();
    check(lapic_x2apic(), "under a hypervisor x2APIC mode is never left");

    reset_model(0x8700);
    has_x2apic = 1;
    (void)lapic_init();
    apic_base_lo |= 1u << 10; /* an application processor the firmware left in x2APIC mode */
    lapic_init_ap();
    check((apic_base_lo & 0xC00u) == 0x800u && faults == 0,
          "a processor started in xAPIC mode leaves the firmware's x2APIC mode");
    (void)lapic_x2apic_enable();
    apic_base_lo &= ~(1u << 10); /* the next one comes up in xAPIC mode */
    lapic_init_ap();
    check((apic_base_lo & 0xC00u) == 0xC00u && faults == 0,
          "and one started after the move enters x2APIC mode");
}

/**
 * Check x2APIC mode, as Linux 6.12.115 chooses it.
 *
 * Under a hypervisor that allows it, `lapic_init` moves the APIC there and
 * reaches every register through its MSR: the identity whole, the
 * end-of-interrupt one write of zero to 0x80B, an interprocessor interrupt
 * one write of 0x830 with the destination in its high half, a message to
 * itself one write of 0x83F. Without the processor's bit, or with
 * `nox2apic`, the APIC stays in xAPIC mode. A firmware's x2APIC mode on
 * bare metal is left through the disabled state, its registers set up
 * afresh through the page; a locked one is kept; one with an identity past
 * 254 is refused. Interrupt remapping's move from xAPIC mode keeps every
 * register, and the move back writes again those the disabled state reset.
 * No row may touch the page in x2APIC mode or an MSR outside it.
 */
static void rows_x2apic(void)
{
    rows_x2apic_hypervisor();
    rows_x2apic_not_chosen();
    rows_x2apic_firmware();
    rows_x2apic_moves();
}

/**
 * Run the rows: 0 when every check passes, 1 otherwise.
 *
 * A machine as QEMU leaves it has LINT0 already an unmasked external
 * interrupt, so the driver leaves it alone. A firmware that suppressed the
 * broadcast end-of-interrupt has that bit cleared and its other bits kept.
 * A firmware that masked LINT0, or left it a fixed interrupt, has it written
 * as the unmasked external-interrupt entry, or the 8259's interrupts stop
 * at the processor. An enable bit off in the register is written. The
 * states the driver rejects (no APIC, no model-specific registers, a base
 * above 4 GiB or none, rach2.nomsi) each leave the model untouched.
 *
 * The order: the software enable first, and the local-vector entries after
 * it, because the processor ignores an unmask before it. The first write of
 * each register is the one compared, since a second write of the spurious
 * register later would hide an enable that came too late. The repair the
 * boot makes when the timer stops after the APIC is enabled writes LINT0
 * again and touches nothing else. The spurious vector's handler counts and
 * returns.
 */
int main(void)
{
    reset_model(0x8700);
    check(lapic_init() == 0, "an APIC in virtual-wire mode is enabled");
    check(lapic_ready(), "and reports itself ready");
    check(lapic_id() == 3, "the identity comes from bits 24 and up");
    check(apic_page[0x80 / 4] == 0, "the task priority is cleared");
    {
        int touched = 0, i;

        for (i = 0; i < writes; i++) {
            if (write_order[i] == 0x350) {
                touched = 1;
            }
        }
        check(!touched, "LINT0 is not written when the firmware left it right");
    }
    check(apic_page[0x350 / 4] == 0x8700, "and it still has the firmware's value");
    check(apic_page[0x360 / 4] == 0x8400, "LINT1 is left alone");
    check(apic_page[0xF0 / 4] == (0x100u | 0xFFu),
          "the spurious register is enabled with vector 255");
    check(wrmsr_calls == 0, "an APIC already enabled in the register is not written again");

    apic_page[0xB0 / 4] = 0x1234;
    lapic_eoi();
    check(apic_page[0xB0 / 4] == 0, "the end-of-interrupt is one write of zero to 0xB0");

    reset_model(0x8700);
    apic_page[0xF0 / 4] = 0x100Fu | 0x200u;
    check(lapic_init() == 0 && apic_page[0xF0 / 4] == (0x300u | 0xFFu),
          "the spurious register loses the end-of-interrupt suppression and keeps the rest");

    reset_model(0x10700);
    check(lapic_init() == 0, "an APIC with LINT0 masked is enabled");
    check(apic_page[0x350 / 4] == (7u << 8), "with LINT0 set to an unmasked external interrupt");
    check(apic_page[0xF0 / 4] == (0x100u | 0xFFu), "and the spurious register enabled");

    reset_model(0x00020);
    check(lapic_init() == 0, "an APIC whose LINT0 is not an external interrupt is enabled");
    check(apic_page[0x350 / 4] == (7u << 8), "with LINT0 corrected");

    reset_model(0x8700);
    apic_base_lo = 0xFEE00000u;
    check(lapic_init() == 0, "an APIC disabled in the register is enabled");
    check(wrmsr_calls == 1 && (wrmsr_lo & (1u << 11)) != 0, "by one write that sets bit 11");

    reset_model(0x8700);
    has_apic = 0;
    check(lapic_init() == -1 && !lapic_ready(), "a processor without an APIC is refused");
    check(writes == 0, "and nothing is written to it at all");

    reset_model(0x8700);
    has_msr = 0;
    check(lapic_init() == -1, "a processor without the model-specific registers is refused");
    check(writes == 0 && wrmsr_calls == 0, "and neither its page nor its register is written");

    rows_x2apic();

    reset_model(0x8700);
    apic_base_hi = 1;
    check(lapic_init() == -1, "a base above 4 GiB is refused");
    check(writes == 0, "and nothing is written");

    reset_model(0x8700);
    apic_base_lo = 1u << 11; /* enabled, and no address at all */
    check(lapic_init() == -1, "a register with no base address in it is refused");
    check(writes == 0, "and nothing is written");

    reset_model(0x8700);
    nomsi = 1;
    check(lapic_init() == -1, "rach2.nomsi refuses");
    check(writes == 0, "and nothing is written");
    apic_page[0xB0 / 4] = 0x1234;
    lapic_eoi();
    check(apic_page[0xB0 / 4] == 0x1234,
          "and an end-of-interrupt on a refused APIC writes nothing");

    rows_pending();

    reset_model(0x10700);
    (void)lapic_init();
    {
        int tpr_at = -1, lint0_at = -1, svr_at = -1, i;

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
        check(tpr_at >= 0 && lint0_at >= 0 && svr_at >= 0 && svr_at < tpr_at && svr_at < lint0_at,
              "the software enable is written before the task priority and LINT0");
    }

    reset_model(0x8700);
    (void)lapic_init();
    apic_page[0x350 / 4] = 0x10700; /* masked behind the driver's back */
    writes = 0;
    lapic_repair_lint0();
    check(apic_page[0x350 / 4] == (7u << 8), "the repair writes LINT0 as an external interrupt");
    check(writes == 1 && write_order[0] == 0x350, "and writes that and nothing else");

    reset_model(0x8700);
    has_apic = 0;
    (void)lapic_init();
    apic_page[0x350 / 4] = 0x10700;
    lapic_repair_lint0();
    check(apic_page[0x350 / 4] == 0x10700, "an APIC that was refused is not repaired");

    reset_model(0x8700);
    (void)lapic_init();
    {
        unsigned int before = lapic_spurious_count();

        lapic_spurious();
        lapic_spurious();
        check(lapic_spurious_count() == before + 2, "the spurious handler counts");
    }

    if (fails == 0) {
        printf("lapic_hosttest: OK\n");
        return 0;
    }
    printf("lapic_hosttest: %d failure(s)\n", fails);
    return 1;
}
