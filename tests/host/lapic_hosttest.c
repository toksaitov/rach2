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
 * the registers as the SDM says, the request, in-service and trigger-mode
 * registers among them. While interrupts are on it takes what the request
 * register holds, as the processor would, and the end of a level vector
 * clears the line's remote IRR at the I/O APIC.
 *
 * The order matters as much as the values. Enabling the APIC with `LINT0`
 * masked stops the 8259's interrupts at the processor, so the row for a
 * masked entry requires the driver to have written the unmasked
 * external-interrupt value before the software enable.
 */

#include "../../rach2/sys/arch/x86/lapic.h"

#include <setjmp.h>

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

/* The processor's interrupts: the depth of `kirq_save`, 0 while they are
 * on; whether a vector waiting in the request register is taken while they
 * are on, as the processor takes it, its handler ending it; a level
 * interrupt's remote IRR at the I/O APIC, which only that end clears; and
 * a message that arrives as interrupts go off, its vector or 0. The 8259
 * and the I/O APIC's lines: whether each is masked, whether both were at
 * the last write of IA32_APIC_BASE, and the lines unmasked again. */
static int irq_off, taking, remote_irr;
static unsigned int late_vector;
static int pic_masked, lines_masked, masked_at_base;
static unsigned int lines_unmasked;

/** Take every vector in the request register, as the processor does while
 * its interrupts are on: each handler ends its vector, and the end of a
 * level one, its trigger-mode bit set, clears the line's remote IRR. */
static void take_requests(void)
{
    int w;

    if (!taking || irq_off != 0) {
        return;
    }
    for (w = 0; w < 8; w++) {
        if ((apic_page[(0x200 + 0x10 * w) / 4] & apic_page[(0x180 + 0x10 * w) / 4]) != 0) {
            remote_irr = 0;
        }
        apic_page[(0x200 + 0x10 * w) / 4] = 0;
    }
}

/* What the processor would fault on, and the x2APIC registers that are not
 * the page's: the 32-bit identity, the command register's last write and
 * how many, and the self-interrupt register's. */
static int faults;
static unsigned int x2_id;
static unsigned int icr_lo, icr_hi, self_vector;
static int icr_writes;

/* The timer's count, which the model moves only when a row asks: armed, it
 * reloads at the first x2APIC register access after the timer's entry is
 * masked, raising the vector only if the entry is unmasked by then, as the
 * count of a masked timer that comes to zero raises nothing. The vectors
 * this processor sent itself through the xAPIC command register, and the
 * timer's and the error's entries at the move to the disabled state, both
 * masked or not. */
static int reload_armed, reload_pending;
static unsigned int self_sent[8];
static int lvt_masked_at_base;

/** Reload the timer's count from its initial count, raising its vector
 * unless its entry is masked. */
static void timer_reload(void)
{
    unsigned int lvt = apic_page[0x320 / 4], v = lvt & 0xFFu;

    apic_page[0x390 / 4] = apic_page[0x380 / 4];
    if ((lvt & 0x10000u) == 0) {
        apic_page[(0x200 + 0x10 * (v / 32)) / 4] |= 1u << (v % 32);
    }
}

/** Read the register at byte offset `reg`. */
static unsigned int model_read(unsigned int reg)
{
    if (apic_base_lo & (1u << 10)) {
        faults++;
    }
    if (reg >= 0x200 && reg < 0x280) {
        take_requests();
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
    if (reg == 0x300 && (value & (1u << 18)) != 0) {
        self_sent[(value & 0xFFu) / 32] |= 1u << (value % 32);
    }
    if (writes < 64) {
        write_order[writes++] = reg;
    }
}

static const struct lapic_io model_io = {model_read, model_write};

/* The processor's features, a hypervisor that allows x2APIC mode
 * unremapped, a firmware's lock of it, the command line's rach2.nomsi and
 * nox2apic, the I/O APIC off by noapic or maxcpus=0, and the writes of
 * IA32_APIC_BASE. */
static int has_apic, has_msr, has_x2apic, hypervisor, locked;
static int nomsi, nox2apic, ioapic_off;
static int wrmsr_calls;
static unsigned int wrmsr_lo;

/* The panics the driver raised, each of which returns to `panic_return`. */
static int panics;
static jmp_buf panic_return;

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

/** The x2APIC identity `cpuid` gives, the same as the register's. */
unsigned int kcpu_x2apic_id(void)
{
    return x2_id;
}

/** Whether the boot line keeps the I/O APIC off: `ioapic_off`. */
int ioapic_disabled(void)
{
    return ioapic_off;
}

/** Whether remapping may yet start in the x2APIC format: yes unless
 * `ioapic_off`; ioapic_hosttest.c has the rows of the answer from the DMAR
 * table's flags and the boot line. */
int ioapic_remappable(void)
{
    return !ioapic_off;
}

/** Count a panic and return to the row that set `panic_return`. */
void k_panic(const char *str)
{
    (void)str;
    panics++;
    longjmp(panic_return, 1);
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
    if (reload_pending) {
        reload_pending = 0;
        timer_reload();
    }
    return 1;
}

/** Read IA32_APIC_BASE, or an x2APIC register: the identity whole, and the
 * others from the page's array. */
void kcpu_rdmsr(unsigned int msr, unsigned int *lo, unsigned int *hi)
{
    unsigned int l = apic_base_lo, h = apic_base_hi;

    if (x2_msr(msr)) {
        if (msr >= 0x820u && msr <= 0x827u) {
            take_requests();
        }
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
 * the spurious register's enable clear, every local-vector entry masked,
 * and the in-service, trigger-mode and request registers clear, which
 * drops whatever was waiting. */
static void model_disable(void)
{
    int i;

    for (i = 0x100 / 4; i < 0x280 / 4; i++) {
        apic_page[i] = 0;
    }
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
        if (msr == 0x832u && (lo & 0x10000u) != 0 && reload_armed) {
            reload_armed = 0;
            reload_pending = 1;
        }
        return;
    }
    wrmsr_calls++;
    wrmsr_lo = lo;
    masked_at_base = pic_masked && lines_masked;
    if ((apic_base_lo & (1u << 10)) != 0 && (lo & (1u << 11)) != 0 && (lo & (1u << 10)) == 0) {
        faults++; /* x2APIC to xAPIC without the disabled state between */
    }
    if ((lo & (1u << 11)) == 0) {
        lvt_masked_at_base = (apic_page[0x320 / 4] & apic_page[0x370 / 4] & 0x10000u) != 0;
        model_disable();
    }
    apic_base_lo = lo;
    apic_base_hi = hi;
}

/** Turn interrupts off, one level deeper; a message set to arrive now
 * waits in the request register. */
uint32_t kirq_save(void)
{
    if (late_vector != 0) {
        apic_page[(0x200 + 0x10 * (late_vector / 32)) / 4] |= 1u << (late_vector % 32);
        late_vector = 0;
    }
    irq_off++;
    return 0;
}

/** Undo one `kirq_save`. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
    irq_off--;
}

/** Mask the routed I/O APIC lines: line 1 was unmasked. */
unsigned short ioapic_mask_routed(void)
{
    lines_masked = 1;
    return 0x0002;
}

/** Unmask the lines `which` names. */
void ioapic_unmask_lines(unsigned short which)
{
    lines_masked = 0;
    lines_unmasked = which;
}

/** Mask the 8259 pair: the masks it had. */
unsigned short pic_mask_all(void)
{
    pic_masked = 1;
    return 0xFFFB;
}

/** Restore the 8259's masks. */
void pic_restore_mask(unsigned short mask)
{
    (void)mask;
    pic_masked = 0;
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
    ioapic_off = 0;
    panics = 0;
    irq_off = taking = remote_irr = 0;
    late_vector = 0;
    pic_masked = lines_masked = masked_at_base = 0;
    lines_unmasked = 0;
    wrmsr_calls = 0;
    faults = 0;
    x2_id = 3;
    icr_lo = icr_hi = self_vector = 0;
    icr_writes = 0;
    reload_armed = reload_pending = 0;
    for (i = 0; i < 8; i++) {
        self_sent[i] = 0;
    }
    lvt_masked_at_base = 0;
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
    x2_id = 0x13;
    check(lapic_init() == 0 && lapic_x2apic(), "under a hypervisor x2APIC mode is used");
    check((apic_base_lo & 0xC00u) == 0xC00u, "with the base register's bits 10 and 11 set");
    check(lapic_id() == 0x13 && lapic_this_id() == 0x13, "and the identity read from its MSR");
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

/** x2APIC mode not chosen: bare metal before remapping, `nox2apic`, the
 * I/O APIC off, a boot identity no message reaches, and a processor without
 * the mode. */
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

    reset_model(0x8700);
    has_x2apic = 1;
    hypervisor = 1;
    ioapic_off = 1;
    check(lapic_init() == 0 && !lapic_x2apic() && wrmsr_calls == 0,
          "noapic or maxcpus=0 keeps xAPIC mode under a hypervisor");

    reset_model(0x8700);
    has_x2apic = 1;
    hypervisor = 1;
    x2_id = 0xFF;
    check(lapic_init() == 0 && !lapic_x2apic() && wrmsr_calls == 0,
          "an identity of 255 keeps xAPIC mode under a hypervisor");
    check(lapic_x2apic_possible() && lapic_x2apic_enable() == 1 && lapic_x2apic() && faults == 0,
          "where remapping may still move it");
}

/** A firmware's x2APIC mode on bare metal, kept until remapping's format
 * is known and then kept or left. */
static void rows_x2apic_firmware_settled(void)
{
    reset_model(0x8700);
    has_x2apic = 1;
    apic_base_lo |= 1u << 10;
    check(lapic_init() == 0 && lapic_x2apic() && wrmsr_calls == 0,
          "a firmware's x2APIC mode is kept until remapping starts");
    check(apic_page[0xF0 / 4] == (0x100u | 0xFFu) && faults == 0, "and enabled through its MSRs");
    lapic_x2apic_settle(0);
    check(!lapic_x2apic() && (apic_base_lo & 0xC00u) == 0x800u && wrmsr_calls == 2,
          "remapping in the xAPIC format, or none, leaves it through the disabled state");
    check(apic_page[0xF0 / 4] == (0x100u | 0xFFu) && apic_page[0x350 / 4] == 0x8700u && faults == 0,
          "with the registers the disabled state reset written again");
    lapic_x2apic_settle(1);
    check(!lapic_x2apic() && wrmsr_calls == 2, "and a second settle does nothing");

    reset_model(0x8700);
    has_x2apic = 1;
    apic_base_lo |= 1u << 10;
    (void)lapic_init();
    lapic_x2apic_settle(1);
    check(lapic_x2apic() && wrmsr_calls == 0 && writes == 0 && faults == 0,
          "remapping in the x2APIC format keeps it");
}

/** The late leave of a firmware's x2APIC mode, with the devices'
 * interrupts live: a level interrupt waiting is taken and ended first, the
 * lines masked across the leave, and a late message sent again. */
static void rows_x2apic_firmware_late(void)
{
    reset_model(0x8700);
    has_x2apic = 1;
    apic_base_lo |= 1u << 10;
    (void)lapic_init();
    apic_page[0x220 / 4] = 1u << 9; /* vector 0x49 from a level line, waiting */
    apic_page[0x1A0 / 4] = 1u << 9;
    remote_irr = 1;
    taking = 1;
    late_vector = 0x41; /* a message that comes as interrupts go off */
    lapic_x2apic_settle(0);
    check(!lapic_x2apic() && remote_irr == 0,
          "a level interrupt waiting at the late leave is taken and ended before it");
    check(masked_at_base && lines_unmasked == 0x0002 && !lines_masked && !pic_masked &&
              irq_off == 0,
          "with the 8259 and the I/O APIC's lines masked across it and unmasked after");
    check(apic_page[0x300 / 4] == ((1u << 18) | 0x41u),
          "and a message that came in the last moment is sent again to this processor");
}

/** Whether this processor sent itself `v` through the xAPIC command
 * register. */
static int sent_self(unsigned int v)
{
    return (self_sent[v / 32] & (1u << (v % 32))) != 0;
}

/** A firmware's x2APIC mode kept, the local timer running periodic on
 * vector 0x20 from 1000 counts with 500 left, and the error entry on
 * vector 0xFE. */
static void timer_begin(void)
{
    reset_model(0x8700);
    has_x2apic = 1;
    apic_base_lo |= 1u << 10;
    (void)lapic_init();
    lapic_timer_set(0x20, 1, 0, 1000);
    apic_page[0x370 / 4] = 0xFEu;
    apic_page[0x390 / 4] = 500;
}

/**
 * The late leave with the local timer running: the timer's and the error's
 * entries masked across the move and written back as they were, and a tick
 * the masked timer dropped raised once. The model reloads the count at the
 * first access after the timer's mask, which raises nothing; a leave that
 * read the count only after that access would miss the reload.
 */
static void rows_x2apic_firmware_timer(void)
{
    timer_begin();
    lapic_x2apic_settle(0);
    check(!lapic_x2apic() && lvt_masked_at_base,
          "the late leave masks the timer's and the error's entries before the disabled state");
    check(apic_page[0x320 / 4] == ((1u << 17) | 0x20u) && apic_page[0x370 / 4] == 0xFEu,
          "and writes them back as they were after it");
    check(!sent_self(0x20), "a timer whose count did not reload raises nothing");

    timer_begin();
    reload_armed = 1;
    lapic_x2apic_settle(0);
    check(!reload_armed && !reload_pending && apic_page[0x390 / 4] == 1000,
          "the count reloads while the timer is masked");
    check(sent_self(0x20) && faults == 0,
          "and the tick the masked timer dropped is raised once after the move");

    timer_begin();
    apic_page[0x320 / 4] |= 0x10000u;
    reload_armed = 1;
    lapic_x2apic_settle(0);
    check(!sent_self(0x20) && apic_page[0x320 / 4] == ((1u << 17) | 0x10000u | 0x20u),
          "a timer masked before the leave has no tick raised, and stays masked");

    timer_begin();
    apic_page[0x320 / 4] = 0x20u; /* one-shot, 500 counts left */
    lapic_x2apic_settle(0);
    check(apic_page[0x380 / 4] == 500 && !sent_self(0x20),
          "a one-shot timer starts again from the count it had left");
}

/** A firmware's x2APIC mode with an identity past 254, where no remapping
 * can come, and under a hypervisor that allows the mode. */
static void rows_x2apic_firmware_wide(void)
{
    reset_model(0x8700);
    has_x2apic = 1;
    apic_base_lo |= 1u << 10;
    x2_id = 0x1FF;
    check(lapic_init() == 0 && lapic_x2apic() && lapic_id() == 0x1FF,
          "an identity past 254 is kept until remapping starts");
    lapic_x2apic_settle(1);
    check(lapic_x2apic() && panics == 0, "and kept when remapping can reach it");

    reset_model(0x8700);
    has_x2apic = 1;
    apic_base_lo |= 1u << 10;
    x2_id = 0x1FF;
    (void)lapic_init();
    if (setjmp(panic_return) == 0) {
        lapic_x2apic_settle(0);
    }
    check(panics == 1 && wrmsr_calls == 0,
          "without remapping in the x2APIC format it is a panic, as Linux's x2apic_disable");

    reset_model(0x8700);
    has_x2apic = 1;
    apic_base_lo |= 1u << 10;
    ioapic_off = 1;
    check(lapic_init() == 0 && !lapic_x2apic() && (apic_base_lo & 0xC00u) == 0x800u,
          "where no remapping can come it is left at once");
    check(wrmsr_calls == 2 && apic_page[0xF0 / 4] == (0x100u | 0xFFu) &&
              apic_page[0x350 / 4] == (7u << 8) && faults == 0,
          "and the reset APIC enabled with LINT0 an external interrupt again");

    reset_model(0x8700);
    has_x2apic = 1;
    apic_base_lo |= 1u << 10;
    ioapic_off = 1;
    x2_id = 0x1FF;
    check(lapic_init() == -1 && !lapic_ready() && wrmsr_calls == 0 && faults == 0,
          "and refused with an identity past 254, nothing written");

    reset_model(0x8700);
    has_x2apic = 1;
    hypervisor = 1;
    apic_base_lo |= 1u << 10;
    x2_id = 0x1FF;
    check(lapic_init() == 0 && lapic_x2apic() && lapic_id() == 0x1FF,
          "under a hypervisor an identity past 254 is kept");
    if (setjmp(panic_return) == 0) {
        lapic_x2apic_settle(0);
    }
    check(lapic_x2apic() && panics == 0 && wrmsr_calls == 0,
          "and stays without remapping, as Linux's try_to_enable_x2apic keeps it");

    reset_model(0x8700);
    has_x2apic = 1;
    hypervisor = 1;
    apic_base_lo |= 1u << 10;
    ioapic_off = 1;
    x2_id = 0x1FF;
    check(lapic_init() == 0 && lapic_x2apic() && wrmsr_calls == 0 && faults == 0,
          "and where no remapping can come it is kept, not refused");
}

/** A firmware's x2APIC mode kept where it is locked or a hypervisor allows
 * it, and `nox2apic`, which takes effect only where the mode can be left. */
static void rows_x2apic_firmware(void)
{
    rows_x2apic_firmware_settled();
    rows_x2apic_firmware_late();
    rows_x2apic_firmware_timer();
    rows_x2apic_firmware_wide();

    reset_model(0x8700);
    has_x2apic = 1;
    locked = 1;
    ioapic_off = 1;
    apic_base_lo |= 1u << 10;
    check(lapic_init() == 0 && lapic_x2apic(), "a locked x2APIC mode is kept");
    check(writes == 0 && wrmsr_calls == 0 && faults == 0, "and the page is not touched");

    reset_model(0x8700);
    has_x2apic = 1;
    hypervisor = 1;
    apic_base_lo |= 1u << 10;
    (void)lapic_init();
    lapic_x2apic_settle(0);
    check(lapic_x2apic() && wrmsr_calls == 0, "under a hypervisor it is kept without remapping");

    reset_model(0x8700);
    has_x2apic = 1;
    nox2apic = 1;
    apic_base_lo |= 1u << 10;
    check(lapic_init() == 0 && !lapic_x2apic() && wrmsr_calls == 2 && lapic_nox2apic(),
          "nox2apic leaves it at once and takes effect");

    reset_model(0x8700);
    has_x2apic = 1;
    nox2apic = 1;
    locked = 1;
    apic_base_lo |= 1u << 10;
    check(lapic_init() == 0 && lapic_x2apic() && !lapic_nox2apic() && lapic_x2apic_possible(),
          "nox2apic does not take effect on a locked mode");

    reset_model(0x8700);
    has_x2apic = 1;
    nox2apic = 1;
    x2_id = 0x1FF;
    apic_base_lo |= 1u << 10;
    check(lapic_init() == 0 && lapic_x2apic() && !lapic_nox2apic() && lapic_x2apic_possible(),
          "nor with an identity past 254, which waits for remapping");

    reset_model(0x8700);
    nox2apic = 1;
    (void)lapic_init();
    check(lapic_nox2apic(), "and does in xAPIC mode");
}

/** The move interrupt remapping makes, and the processors started before
 * and after it. */
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

    reset_model(0x8700);
    has_x2apic = 1;
    (void)lapic_init();
    apic_page[0x220 / 4] = 1u << 9; /* vector 0x49 waiting, level-triggered */
    apic_page[0x1A0 / 4] = 1u << 9;
    (void)lapic_x2apic_enable();
    check(apic_page[0x220 / 4] == 1u << 9 && apic_page[0x1A0 / 4] == 1u << 9,
          "the move keeps the request and trigger-mode registers");

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
 * itself one write of 0x83F. Without the processor's bit, with `nox2apic`,
 * with the I/O APIC off, or for an identity of 255, the APIC stays in xAPIC
 * mode. A firmware's x2APIC mode on bare metal is kept until remapping's
 * format is known, then kept or left through the disabled state, its
 * registers written again, or a panic with an identity past 254; where no
 * remapping can come it is left at once, or refused with such an identity.
 * A locked one is kept, and so is one under a hypervisor; `nox2apic` takes
 * effect only on a mode that can be left, and a late leave takes a waiting
 * level interrupt first, with the lines masked, and sends a late message
 * again, with the timer's and the error's entries masked across it and a
 * tick the masked timer dropped raised once. Interrupt remapping's move
 * from xAPIC mode keeps every register, the request and trigger-mode
 * registers among them. No row may touch the page in x2APIC mode or an MSR
 * outside it.
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
